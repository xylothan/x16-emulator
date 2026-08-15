// Commander X16 Emulator — SD card FAT index implementation
// Copyright (c) 2024 X16Emu ADD contributors
// All rights reserved. License: 2-clause BSD
//
// Parses FAT16/FAT32 structures inside an SD card image (512-byte sectors).
// Opens its own independent file handle — never touches sdcard.c's handle.

#include "sdcard_fat.h"
#include "compat.h"
#include "files.h"

#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// _strdup is the MSVC name; most POSIX systems have strdup.
#ifdef _MSC_VER
#define fat_strdup _strdup
#else
#define fat_strdup strdup
#endif

// ── Caps ─────────────────────────────────────────────────────────────────────
#define SDCARD_FAT_MAX_FILES   4096
#define SDCARD_FAT_MAX_EXTENTS 16384
#define SDCARD_FAT_MAX_DEPTH   16
#define SDCARD_FAT_MAX_CLUSTERS_PER_FILE 131072  // 64 MB / 512 bytes

// ── Internal types ────────────────────────────────────────────────────────────

typedef enum {
	FAT_TYPE_NONE = 0,
	FAT_TYPE_12,
	FAT_TYPE_16,
	FAT_TYPE_32,
} fat_type_t;

// An LBA extent belonging to a file/dir (all inclusive).
typedef struct {
	uint32_t first_lba;
	uint32_t last_lba;
	int      file_index;   // index into g_files
	uint64_t file_offset;  // byte offset in file at first_lba
} fat_extent_t;

// Internal mutable entry — sdcard_fat_file_t is the public const view.
typedef struct {
	char    *path;         // heap-allocated "/FOO/BAR.PRG"
	uint32_t size;
	uint32_t first_cluster;
	bool     is_dir;
	uint64_t bytes_read;
	uint64_t bytes_written;
} fat_file_entry_t;

// ── Module state ──────────────────────────────────────────────────────────────

static struct x16file *g_img        = NULL;  // borrowed from caller; never closed here
static bool            g_ready      = false;
static bool            g_stale      = false;
static bool            g_truncated  = false;
static fat_type_t      g_fat_type   = FAT_TYPE_NONE;
static uint32_t        g_bps        = 512;   // bytes per sector (always 512)
static uint32_t        g_spc        = 0;     // sectors per cluster
static uint32_t        g_fat_lba    = 0;     // first LBA of FAT region
static uint32_t        g_fat_count  = 0;     // number of FATs
static uint32_t        g_fat_size   = 0;     // sectors per FAT
static uint32_t        g_rootdir_lba= 0;     // FAT16 fixed root dir LBA
static uint32_t        g_rootdir_sectors = 0;// FAT16 root dir size in sectors
static uint32_t        g_first_data_lba = 0; // LBA of cluster 2
static uint32_t        g_root_cluster = 0;   // FAT32 root cluster
static uint32_t        g_total_sectors = 0;
static uint32_t        g_partition_lba = 0;  // absolute LBA of VBR

static fat_file_entry_t  g_files[SDCARD_FAT_MAX_FILES];
static int               g_file_count = 0;

static fat_extent_t      g_extents[SDCARD_FAT_MAX_EXTENTS];
static int               g_extent_count = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Read exactly one 512-byte sector at absolute LBA into buf.
// Saves and restores the stream position so the caller's (card's) file offset
// is unchanged after this call.  Returns true on success.
static bool read_sector(uint32_t abs_lba, uint8_t buf[512])
{
	if (g_img == NULL) {
		return false;
	}
	int64_t saved  = x16tell(g_img);
	int64_t offset = (int64_t)abs_lba * 512;
	// x16seek(), like the SDL_RWseek() it wraps, returns the resulting stream
	// position -- not a status. Only a negative result is an error; testing
	// against zero would reject every seek to a non-zero offset, which is to
	// say every sector but the first.
	if (x16seek(g_img, offset, XSEEK_SET) < 0) {
		if (saved >= 0) {
			x16seek(g_img, saved, XSEEK_SET);
		}
		return false;
	}
	bool ok = (x16read(g_img, buf, 1, 512) == 512);
	// Restore position regardless of read outcome.
	if (saved >= 0) {
		x16seek(g_img, saved, XSEEK_SET);
	}
	return ok;
}

// ── Path string helpers ───────────────────────────────────────────────────────

// Emit a UCS-2 code unit as UTF-8 into buf (must have space for 4 bytes + NUL).
// Returns number of bytes written.
static int ucs2_to_utf8(uint16_t cp, char *out)
{
	if (cp == 0xFFFF || cp == 0x0000) {
		return 0; // end-of-name sentinel
	}
	if (cp < 0x80) {
		out[0] = (char)(uint8_t)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(uint8_t)(0xC0 | (cp >> 6));
		out[1] = (char)(uint8_t)(0x80 | (cp & 0x3F));
		return 2;
	}
	out[0] = (char)(uint8_t)(0xE0 | (cp >> 12));
	out[1] = (char)(uint8_t)(0x80 | ((cp >> 6) & 0x3F));
	out[2] = (char)(uint8_t)(0x80 | (cp & 0x3F));
	return 3;
}

// Build a 13-character LFN fragment from a directory entry into out_utf8.
// out_utf8 must hold at least 40 bytes (13 chars × 3 UTF-8 bytes + NUL).
// Returns the number of UTF-8 bytes written (stops at first 0x0000/0xFFFF).
static int lfn_chars(const uint8_t *dirent, char *out_utf8)
{
	static const int offsets[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
	int n = 0;
	for (int i = 0; i < 13; i++) {
		uint16_t cp = (uint16_t)(dirent[offsets[i]] | ((uint16_t)dirent[offsets[i]+1] << 8));
		if (cp == 0x0000 || cp == 0xFFFF) {
			break;
		}
		int w = ucs2_to_utf8(cp, out_utf8 + n);
		n += w;
	}
	return n;
}

// Decode an 8.3 short name into name_buf (null-terminated, no trailing spaces,
// extension separated by '.'). name_buf must be >= 13 bytes.
static void decode_83(const uint8_t *dirent, char *name_buf)
{
	char base[9], ext[4];
	int  bi = 0, ei = 0;

	for (int i = 0; i < 8; i++) {
		uint8_t c = dirent[i];
		if (c == ' ') {
			break;
		}
		base[bi++] = (char)c;
	}
	base[bi] = '\0';

	for (int i = 0; i < 3; i++) {
		uint8_t c = dirent[8 + i];
		if (c == ' ') {
			break;
		}
		ext[ei++] = (char)c;
	}
	ext[ei] = '\0';

	if (ei > 0) {
		snprintf(name_buf, 13, "%s.%s", base, ext);
	} else {
		snprintf(name_buf, 13, "%s", base);
	}
}

// Append an extent, staying within the cap.  Returns false when cap hit.
static bool add_extent(uint32_t first_lba, uint32_t last_lba,
                       int file_index, uint64_t file_offset)
{
	if (g_extent_count >= SDCARD_FAT_MAX_EXTENTS) {
		g_truncated = true;
		return false;
	}
	fat_extent_t *e = &g_extents[g_extent_count++];
	e->first_lba   = first_lba;
	e->last_lba    = last_lba;
	e->file_index  = file_index;
	e->file_offset = file_offset;
	return true;
}

// ── FAT chain walking ─────────────────────────────────────────────────────────

// Read FAT entry for cluster N. Returns 0xFFFFFFFF on I/O error.
static uint32_t fat_next(uint32_t cluster)
{
	uint8_t buf[512];
	uint32_t byte_offset;
	uint32_t fat_lba;

	if (g_fat_type == FAT_TYPE_32) {
		byte_offset = cluster * 4;
		fat_lba = g_fat_lba + byte_offset / 512;
		uint32_t idx = byte_offset % 512;
		if (!read_sector(fat_lba, buf)) {
			return 0xFFFFFFFF;
		}
		return le32(buf + idx) & 0x0FFFFFFF;
	} else {
		// FAT16
		byte_offset = cluster * 2;
		fat_lba = g_fat_lba + byte_offset / 512;
		uint32_t idx = byte_offset % 512;
		if (!read_sector(fat_lba, buf)) {
			return 0xFFFFFFFF;
		}
		uint16_t val = le16(buf + idx);
		if (val >= 0xFFF8) {
			return 0x0FFFFFFF; // EOC in FAT32 encoding
		}
		return (uint32_t)val;
	}
}

static bool is_eoc(uint32_t entry)
{
	if (g_fat_type == FAT_TYPE_32) {
		return entry >= 0x0FFFFFF8;
	}
	return entry >= 0xFFF8 || entry == 0x0FFFFFFF;
}

static uint32_t cluster_to_lba(uint32_t cluster)
{
	return g_first_data_lba + (cluster - 2) * g_spc;
}

// Walk a cluster chain, adding extents to the index.
// file_index: index into g_files. Cap enforced internally.
static void walk_chain(uint32_t start_cluster, int file_index)
{
	if (start_cluster < 2) {
		return; // invalid
	}

	uint32_t cluster     = start_cluster;
	uint32_t count       = 0;
	uint64_t file_offset = 0;

	// For extent coalescing:
	uint32_t run_first_lba = 0;
	uint32_t run_last_lba  = 0;
	uint64_t run_file_off  = 0;
	bool     in_run        = false;

	while (!is_eoc(cluster) && cluster != 0) {
		if (count++ >= SDCARD_FAT_MAX_CLUSTERS_PER_FILE) {
			g_truncated = true;
			break;
		}

		uint32_t lba_start = cluster_to_lba(cluster);
		uint32_t lba_end   = lba_start + g_spc - 1;

		if (!in_run) {
			run_first_lba = lba_start;
			run_last_lba  = lba_end;
			run_file_off  = file_offset;
			in_run        = true;
		} else if (lba_start == run_last_lba + 1) {
			// Contiguous — extend current run.
			run_last_lba = lba_end;
		} else {
			// Gap — flush current run.
			if (!add_extent(run_first_lba, run_last_lba, file_index, run_file_off)) {
				return;
			}
			run_first_lba = lba_start;
			run_last_lba  = lba_end;
			run_file_off  = file_offset;
		}

		file_offset += (uint64_t)g_spc * g_bps;
		cluster = fat_next(cluster);
	}

	if (in_run) {
		add_extent(run_first_lba, run_last_lba, file_index, run_file_off);
	}
}

// ── Directory traversal ───────────────────────────────────────────────────────

// Forward declaration for recursion.
static void walk_dir(uint32_t start_cluster, bool is_fat32_root,
                     const char *parent_path, int depth);

// Add a new file/dir entry. Returns the index, or -1 on cap.
static int add_file(const char *path, uint32_t size,
                    uint32_t first_cluster, bool is_dir)
{
	if (g_file_count >= SDCARD_FAT_MAX_FILES) {
		g_truncated = true;
		return -1;
	}
	fat_file_entry_t *f = &g_files[g_file_count];
	f->path          = fat_strdup(path);
	f->size          = size;
	f->first_cluster = first_cluster;
	f->is_dir        = is_dir;
	f->bytes_read    = 0;
	f->bytes_written = 0;
	return g_file_count++;
}

// Process a single 32-byte directory entry that is NOT an LFN record.
// lfn_buf: accumulated LFN (may be empty), lfn_len: length.
static void process_dirent(const uint8_t *de, const char *parent_path,
                            const char *lfn_buf, int lfn_len, int depth)
{
	uint8_t attr = de[11];

	// Volume label — skip.
	if (attr & 0x08) {
		return;
	}

	// Build name.
	char name[512]; // large enough for a maximal LFN
	if (lfn_len > 0) {
		if (lfn_len >= (int)sizeof(name)) {
			lfn_len = (int)sizeof(name) - 1;
		}
		memcpy(name, lfn_buf, (size_t)lfn_len);
		name[lfn_len] = '\0';
	} else {
		char sn[13];
		decode_83(de, sn);
		if (sn[0] == '\0') {
			return;
		}
		snprintf(name, sizeof(name), "%s", sn);
	}

	// Skip "." and "..".
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		return;
	}

	// Build full path.
	char full_path[PATH_MAX];
	if (parent_path[1] == '\0') { // parent == "/"
		snprintf(full_path, sizeof(full_path), "/%s", name);
	} else {
		snprintf(full_path, sizeof(full_path), "%s/%s", parent_path, name);
	}

	uint32_t first_cluster = ((uint32_t)le16(de + 20) << 16) | le16(de + 26);
	uint32_t size          = le32(de + 28);
	bool     is_dir        = (attr & 0x10) != 0;

	int idx = add_file(full_path, size, first_cluster, is_dir);
	if (idx < 0) {
		return; // cap hit
	}

	walk_chain(first_cluster, idx);

	if (is_dir && depth + 1 < SDCARD_FAT_MAX_DEPTH) {
		walk_dir(first_cluster, false, full_path, depth + 1);
	}
}

// Walk a directory given its start cluster (or FAT16 root, indicated by
// is_fat16_root flag encoded in is_fat32_root=false + start_cluster==0).
static void walk_dir(uint32_t start_cluster, bool is_fat32_root,
                     const char *parent_path, int depth)
{
	uint8_t  sector[512];
	uint8_t  lfn_buf[512 * 3]; // generous LFN buffer
	int      lfn_len  = 0;
	char     lfn_frags[20][40]; // up to 20 LFN fragments (13 chars * 3 bytes each)
	int      lfn_frag_count = 0;
	int      lfn_expected_seq = 0;

	// Whether we're in the FAT16 fixed root dir.
	bool is_fat16_root = (!is_fat32_root && start_cluster == 0 &&
	                      g_fat_type == FAT_TYPE_16);

	uint32_t cur_cluster  = start_cluster;
	uint32_t sector_in_cluster = 0;
	uint32_t sectors_done = 0;
	bool     done         = false;
	uint32_t cluster_chain_count = 0;

	while (!done) {
		uint32_t lba;
		if (is_fat16_root) {
			if (sectors_done >= g_rootdir_sectors) {
				break;
			}
			lba = g_rootdir_lba + sectors_done;
		} else {
			if (cur_cluster < 2 || is_eoc(cur_cluster)) {
				break;
			}
			lba = cluster_to_lba(cur_cluster) + sector_in_cluster;
		}

		if (!read_sector(lba, sector)) {
			break;
		}

		// Process 16 entries per sector.
		for (int e = 0; e < 16 && !done; e++) {
			const uint8_t *de = sector + e * 32;
			uint8_t first_byte = de[0];
			uint8_t attr       = de[11];

			if (first_byte == 0x00) {
				// End of directory.
				done = true;
				break;
			}
			if (first_byte == 0xE5) {
				// Deleted — also reset LFN accumulator.
				lfn_frag_count   = 0;
				lfn_expected_seq = 0;
				continue;
			}

			if (attr == 0x0F) {
				// LFN entry.
				uint8_t seq = de[0];
				bool is_last = (seq & 0x40) != 0;
				seq &= 0x1F;
				if (seq == 0 || seq > 20) {
					// Corrupt — reset.
					lfn_frag_count   = 0;
					lfn_expected_seq = 0;
					continue;
				}
				if (is_last) {
					lfn_frag_count   = 0;
					lfn_expected_seq = (int)seq;
				}
				if ((int)seq != lfn_expected_seq) {
					// Out of order — reset.
					lfn_frag_count   = 0;
					lfn_expected_seq = 0;
					continue;
				}
				// Store fragment (seq is 1-based, stored in reverse order).
				int slot = (int)seq - 1;
				if (slot < 20) {
					int n = lfn_chars(de, lfn_frags[slot]);
					lfn_frags[slot][n] = '\0';
					if (lfn_frag_count < slot + 1) {
						lfn_frag_count = slot + 1;
					}
				}
				lfn_expected_seq--;
				continue;
			}

			// Regular entry — assemble LFN if available.
			if (lfn_frag_count > 0 && lfn_expected_seq == 0) {
				// Reassemble from fragment 0 (lowest) up.
				lfn_len = 0;
				for (int fi = 0; fi < lfn_frag_count && lfn_len < (int)sizeof(lfn_buf) - 40; fi++) {
					int n = (int)strlen(lfn_frags[fi]);
					memcpy(lfn_buf + lfn_len, lfn_frags[fi], (size_t)n);
					lfn_len += n;
				}
				lfn_buf[lfn_len] = '\0';
			} else {
				lfn_len = 0;
			}

			process_dirent(de, parent_path, (const char *)lfn_buf, lfn_len, depth);

			// Reset LFN accumulator for next entry.
			lfn_frag_count   = 0;
			lfn_expected_seq = 0;
			lfn_len          = 0;
		}

		// Advance to next sector.
		sectors_done++;
		if (is_fat16_root) {
			// Nothing to advance; sectors_done checked above.
		} else {
			sector_in_cluster++;
			if (sector_in_cluster >= g_spc) {
				sector_in_cluster = 0;
				if (++cluster_chain_count >= SDCARD_FAT_MAX_CLUSTERS_PER_FILE) {
					g_truncated = true;
					break;
				}
				cur_cluster = fat_next(cur_cluster);
			}
		}
	}
}

// ── Extent sorting ────────────────────────────────────────────────────────────

static int extent_cmp(const void *a, const void *b)
{
	const fat_extent_t *ea = (const fat_extent_t *)a;
	const fat_extent_t *eb = (const fat_extent_t *)b;
	if (ea->first_lba < eb->first_lba) return -1;
	if (ea->first_lba > eb->first_lba) return  1;
	return 0;
}

// ── Internal teardown ─────────────────────────────────────────────────────────

static void free_state(void)
{
	for (int i = 0; i < g_file_count; i++) {
		free(g_files[i].path);
		g_files[i].path = NULL;
	}
	g_file_count   = 0;
	g_extent_count = 0;
	g_ready        = false;
	g_stale        = false;
	g_truncated    = false;
	g_fat_type     = FAT_TYPE_NONE;

	// g_img is a borrowed handle — never close it here.
	g_img = NULL;
}

// ── BPB detection/parsing ─────────────────────────────────────────────────────

// Validate a 512-byte boot sector and fill geometry fields.
// abs_lba: absolute LBA of this boot sector.
// Returns FAT_TYPE_NONE if invalid.
static fat_type_t parse_bpb(const uint8_t *sector, uint32_t abs_lba)
{
	// Jump instruction check.
	bool valid_jmp = (sector[0] == 0xEB && sector[2] == 0x90) ||
	                 (sector[0] == 0xE9);
	// Some disk tools use 0x00 for superfloppy — tolerate if signature is ok.
	bool valid_sig = (sector[510] == 0x55 && sector[511] == 0xAA);

	if (!valid_jmp && !valid_sig) {
		return FAT_TYPE_NONE;
	}
	if (!valid_sig) {
		return FAT_TYPE_NONE;
	}

	uint16_t bps   = le16(sector + 11);
	uint8_t  spc   = sector[13];
	uint16_t rsvd  = le16(sector + 14);
	uint8_t  nfats = sector[16];
	uint16_t root_ent = le16(sector + 17);
	uint16_t tot16 = le16(sector + 19);
	uint16_t fat16_sz = le16(sector + 22);
	uint32_t tot32 = le32(sector + 32);
	uint32_t fat32_sz = le32(sector + 36);
	uint32_t root_cluster = le32(sector + 44);

	// Validate.
	if (bps != 512) {
		return FAT_TYPE_NONE;
	}
	if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128) {
		return FAT_TYPE_NONE;
	}
	if (nfats == 0 || nfats > 2) {
		return FAT_TYPE_NONE;
	}
	if (rsvd == 0) {
		return FAT_TYPE_NONE;
	}

	uint32_t fat_sz  = fat16_sz != 0 ? fat16_sz : fat32_sz;
	uint32_t tot_sec = tot16 != 0 ? (uint32_t)tot16 : tot32;

	if (fat_sz == 0 || tot_sec == 0) {
		return FAT_TYPE_NONE;
	}

	uint32_t root_dir_sectors = ((uint32_t)root_ent * 32 + 511) / 512;
	uint32_t data_start_off   = rsvd + (uint32_t)nfats * fat_sz + root_dir_sectors;

	if (data_start_off >= tot_sec) {
		return FAT_TYPE_NONE;
	}

	uint32_t data_sectors  = tot_sec - data_start_off;
	uint32_t cluster_count = data_sectors / spc;

	fat_type_t ft;
	if (cluster_count < 4085) {
		ft = FAT_TYPE_12; // not supported, we'll reject below
	} else if (cluster_count < 65525) {
		ft = FAT_TYPE_16;
	} else {
		ft = FAT_TYPE_32;
	}

	if (ft == FAT_TYPE_12) {
		return FAT_TYPE_NONE; // unsupported — reject cleanly
	}

	// Fill geometry globals.
	g_bps         = bps;
	g_spc         = spc;
	g_fat_lba     = abs_lba + rsvd;
	g_fat_count   = nfats;
	g_fat_size    = fat_sz;
	g_total_sectors = tot_sec;

	if (ft == FAT_TYPE_16) {
		g_rootdir_lba     = abs_lba + rsvd + (uint32_t)nfats * fat_sz;
		g_rootdir_sectors = root_dir_sectors;
		g_first_data_lba  = g_rootdir_lba + root_dir_sectors;
		g_root_cluster    = 0;
	} else {
		// FAT32
		if (root_cluster < 2) {
			return FAT_TYPE_NONE;
		}
		g_rootdir_lba     = 0;
		g_rootdir_sectors = 0;
		g_first_data_lba  = abs_lba + rsvd + (uint32_t)nfats * fat_sz;
		g_root_cluster    = root_cluster;
	}

	return ft;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool sdcard_fat_autoindex = true;

bool
sdcard_fat_build(struct x16file *img)
{
	free_state();

	if (img == NULL) {
		return false;
	}

	g_img = img; // borrow

	int64_t img_size = x16size(g_img);
	if (img_size < 512) {
		g_img = NULL;
		return false;
	}

	uint8_t sector0[512];
	if (!read_sector(0, sector0)) {
		g_img = NULL;
		return false;
	}

	// Check for MBR (0x55AA at offset 510, partition table at 0x1BE).
	bool has_mbr = (sector0[510] == 0x55 && sector0[511] == 0xAA);

	// FAT type constants.
	static const uint8_t fat_types[] = {
		0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0x1B, 0x1C
	};

	// Try MBR partitions first.
	uint32_t part_lba = 0;
	bool     found_part = false;

	if (has_mbr) {
		for (int p = 0; p < 4; p++) {
			const uint8_t *pe = sector0 + 0x1BE + p * 16;
			uint8_t  ptype = pe[4];
			uint32_t plba  = le32(pe + 8);
			uint32_t pcnt  = le32(pe + 12);
			if (pcnt == 0) {
				continue;
			}
			bool is_fat_part = false;
			for (int k = 0; k < (int)(sizeof(fat_types)/sizeof(fat_types[0])); k++) {
				if (ptype == fat_types[k]) {
					is_fat_part = true;
					break;
				}
			}
			if (is_fat_part) {
				part_lba   = plba;
				found_part = true;
				break;
			}
		}
	}

	// Try found partition or superfloppy (LBA 0).
	uint8_t vbr[512];
	fat_type_t ft = FAT_TYPE_NONE;

	if (found_part) {
		if (!read_sector(part_lba, vbr)) {
			g_img = NULL;
			return false;
		}
		ft = parse_bpb(vbr, part_lba);
	}

	if (ft == FAT_TYPE_NONE) {
		// Try superfloppy: VBR is at LBA 0.
		ft = parse_bpb(sector0, 0);
		if (ft != FAT_TYPE_NONE) {
			part_lba = 0;
		}
	}

	if (ft == FAT_TYPE_NONE) {
		g_img = NULL;
		return false;
	}

	g_fat_type      = ft;
	g_partition_lba = part_lba;

	// Walk the directory tree.
	if (ft == FAT_TYPE_32) {
		// The FAT32 root is an ordinary cluster chain, so it has to be entered
		// into the extent table like any other directory. Without it, root
		// sectors match nothing, fall through to "unallocated", and -- worse --
		// a write into the root does not mark the index stale, so the panel
		// would keep serving names for files that had since been renamed.
		int root_idx = add_file("/", 0, g_root_cluster, true);
		if (root_idx >= 0) {
			walk_chain(g_root_cluster, root_idx);
		}
		walk_dir(g_root_cluster, true, "/", 0);
	} else {
		// FAT16: pass start_cluster=0 as a sentinel for the fixed root dir.
		walk_dir(0, false, "/", 0);
	}

	// Sort extents by first_lba for binary search.
	qsort(g_extents, (size_t)g_extent_count, sizeof(fat_extent_t), extent_cmp);

	g_ready = true;
	return true;
}

void
sdcard_fat_free(void)
{
	free_state();
}

bool
sdcard_fat_ready(void)
{
	return g_ready;
}

bool
sdcard_fat_is_stale(void)
{
	return g_stale;
}

void
sdcard_fat_mark_stale(void)
{
	g_stale = true;
}

bool
sdcard_fat_truncated(void)
{
	return g_truncated;
}

const char *
sdcard_fat_type_name(void)
{
	if (!g_ready) {
		return "";
	}
	switch (g_fat_type) {
		case FAT_TYPE_16: return "FAT16";
		case FAT_TYPE_32: return "FAT32";
		default:          return "FAT??";
	}
}

uint32_t
sdcard_fat_bytes_per_cluster(void)
{
	if (!g_ready) {
		return 0;
	}
	return g_spc * g_bps;
}

int
sdcard_fat_file_count(void)
{
	return g_ready ? g_file_count : 0;
}

const sdcard_fat_file_t *
sdcard_fat_file_at(int i)
{
	if (!g_ready || i < 0 || i >= g_file_count) {
		return NULL;
	}
	// The public struct has the same layout as the internal one (same fields,
	// same types, same order).  Cast is safe.
	return (const sdcard_fat_file_t *)&g_files[i];
}

sdcard_fat_region_t
sdcard_fat_lookup(uint32_t lba,
                  const sdcard_fat_file_t **out_file,
                  uint64_t                 *out_offset)
{
	if (out_file)   *out_file   = NULL;
	if (out_offset) *out_offset = 0;

	if (!g_ready) {
		return SDCARD_FAT_REGION_UNKNOWN;
	}

	// Check against partition bounds (if there is a partition).
	uint32_t part_end = g_partition_lba + g_total_sectors;
	if (g_total_sectors > 0 && lba >= part_end) {
		return SDCARD_FAT_REGION_UNKNOWN;
	}

	// MBR (only valid when the partition doesn't start at LBA 0).
	if (g_partition_lba > 0 && lba == 0) {
		return SDCARD_FAT_REGION_MBR;
	}

	// Reserved region: from VBR to start of FAT.
	if (lba >= g_partition_lba && lba < g_fat_lba) {
		return SDCARD_FAT_REGION_RESERVED;
	}

	// FAT region.
	uint32_t fat_end = g_fat_lba + g_fat_count * g_fat_size;
	if (lba >= g_fat_lba && lba < fat_end) {
		return SDCARD_FAT_REGION_FAT;
	}

	// FAT16 root directory.
	if (g_fat_type == FAT_TYPE_16 && g_rootdir_sectors > 0) {
		if (lba >= g_rootdir_lba && lba < g_rootdir_lba + g_rootdir_sectors) {
			return SDCARD_FAT_REGION_ROOTDIR;
		}
	}

	// Binary search in extent table.
	int lo = 0, hi = g_extent_count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		const fat_extent_t *e = &g_extents[mid];
		if (lba < e->first_lba) {
			hi = mid - 1;
		} else if (lba > e->last_lba) {
			lo = mid + 1;
		} else {
			// Hit.
			const fat_file_entry_t *fe = &g_files[e->file_index];
			if (out_file) {
				*out_file = (const sdcard_fat_file_t *)fe;
			}
			if (out_offset) {
				*out_offset = e->file_offset + (uint64_t)(lba - e->first_lba) * 512;
			}
			return fe->is_dir ? SDCARD_FAT_REGION_DIR : SDCARD_FAT_REGION_FILE;
		}
	}

	// Data region but not in any file's cluster chain.
	if (lba >= g_first_data_lba) {
		return SDCARD_FAT_REGION_FREE;
	}

	return SDCARD_FAT_REGION_UNKNOWN;
}

void
sdcard_fat_note_access(uint32_t lba, bool is_write, uint32_t bytes)
{
	if (!g_ready) {
		return;
	}

	const sdcard_fat_file_t *f   = NULL;
	uint64_t                 off = 0;
	sdcard_fat_region_t      reg = sdcard_fat_lookup(lba, &f, &off);

	if (is_write) {
		// A write to FAT or directory metadata makes the index stale.
		if (reg == SDCARD_FAT_REGION_FAT     ||
		    reg == SDCARD_FAT_REGION_ROOTDIR ||
		    reg == SDCARD_FAT_REGION_DIR) {
			g_stale = true;
		}
	}

	if (f != NULL) {
		// The file_t pointer is actually a fat_file_entry_t* under the hood.
		fat_file_entry_t *fe = (fat_file_entry_t *)(uintptr_t)f;
		if (is_write) {
			fe->bytes_written += bytes;
		} else {
			fe->bytes_read += bytes;
		}
	}

	(void)off;
}
