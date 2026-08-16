// test_sdcard_fat.c — functional tests for the SD card FAT index
//
// Synthesises small FAT32 and FAT16 images in memory, writes them to disk,
// builds the index, and exercises every major code path.
//
// The five file-access functions are stubbed onto stdio (no SDL, no zlib).
// The stubs mirror the REAL contracts in files.h / files.c exactly:
//   x16seek  returns the resulting stream offset (like SDL_RWseek), or -1
//   x16tell  returns the current offset, or -1 on error
//   x16read  returns the number of ITEMS read (not bytes)
//   x16open  returns a heap handle, or NULL on failure
//   x16close frees the handle (no-op on NULL)
// A stub that does not mirror the real contract manufactures false confidence.

#include "sdcard_fat.h"
#include "support/harness.h"
#include "files.h"   // pulls in XSEEK_* and checks stub signatures

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ── stdio stubs for src/files.h ───────────────────────────────────────────────

struct x16file {
	FILE *fp;
};

struct x16file *
x16open(const char *path, const char *attribs)
{
	FILE *fp = fopen(path, attribs);
	if (fp == NULL) {
		return NULL;
	}
	struct x16file *f = (struct x16file *)malloc(sizeof(struct x16file));
	if (f == NULL) {
		fclose(fp);
		return NULL;
	}
	f->fp = fp;
	return f;
}

void
x16close(struct x16file *f)
{
	if (f == NULL) {
		return;
	}
	fclose(f->fp);
	free(f);
}

int64_t
x16size(struct x16file *f)
{
	long here = ftell(f->fp);
	fseek(f->fp, 0, SEEK_END);
	long end  = ftell(f->fp);
	fseek(f->fp, here, SEEK_SET);
	return (int64_t)end;
}

// Mirrors the real contract exactly: SDL_RWseek -- and so the real x16seek --
// returns the resulting stream position, not a success/failure status. A stub
// that returned fseek()'s 0-on-success would let a parser which tested the
// result against zero pass here and fail against every real image.
int
x16seek(struct x16file *f, int64_t pos, int origin)
{
	int whence = (origin == XSEEK_END) ? SEEK_END
	           : (origin == XSEEK_CUR) ? SEEK_CUR
	                                   : SEEK_SET;
	if (fseek(f->fp, (long)pos, whence) != 0) {
		return -1;
	}
	long p = ftell(f->fp);
	return (p < 0) ? -1 : (int)p;
}

int64_t
x16tell(struct x16file *f)
{
	long p = ftell(f->fp);
	return (p < 0) ? -1 : (int64_t)p;
}

uint64_t
x16read(struct x16file *f, uint8_t *data, uint64_t data_size, uint64_t data_count)
{
	return (uint64_t)fread(data, (size_t)data_size, (size_t)data_count, f->fp);
}

// ── Tiny image builder ────────────────────────────────────────────────────────

#define SECTOR_SIZE 512u

typedef struct {
	uint8_t *data;
	uint32_t sector_count;
} disk_t;

static disk_t
disk_alloc(uint32_t sectors)
{
	disk_t d;
	d.sector_count = sectors;
	d.data         = (uint8_t *)calloc((size_t)sectors, SECTOR_SIZE);
	return d;
}

static void
disk_free(disk_t *d)
{
	free(d->data);
	d->data         = NULL;
	d->sector_count = 0;
}

static uint8_t *
disk_sector(disk_t *d, uint32_t lba)
{
	return d->data + (size_t)lba * SECTOR_SIZE;
}

static void wle16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wle32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8)  & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void
fat32_set(disk_t *d, uint32_t fat_lba, uint32_t cluster, uint32_t val)
{
	uint32_t byte_off = cluster * 4u;
	uint32_t s = fat_lba + byte_off / SECTOR_SIZE;
	uint32_t i = byte_off % SECTOR_SIZE;
	wle32(disk_sector(d, s) + i, val & 0x0FFFFFFFu);
}

static void
fat16_set(disk_t *d, uint32_t fat_lba, uint32_t cluster, uint16_t val)
{
	uint32_t byte_off = cluster * 2u;
	uint32_t s = fat_lba + byte_off / SECTOR_SIZE;
	uint32_t i = byte_off % SECTOR_SIZE;
	wle16(disk_sector(d, s) + i, val);
}

static void
write_dirent_83(uint8_t *de, const char *name83, uint8_t attr,
                uint32_t first_cluster, uint32_t size)
{
	memset(de, 0, 32);
	memcpy(de, name83, 11);
	de[11] = attr;
	wle16(de + 20, (uint16_t)(first_cluster >> 16));
	wle16(de + 26, (uint16_t)(first_cluster & 0xFFFFu));
	wle32(de + 28, size);
}

static uint8_t
lfn_checksum(const uint8_t *name83)
{
	uint8_t sum = 0;
	for (int i = 0; i < 11; i++) {
		sum = (uint8_t)(((sum & 1u) ? 0x80u : 0x00u) + (sum >> 1) + name83[i]);
	}
	return sum;
}

static void
write_lfn_entry(uint8_t *de, uint8_t seq, bool is_last,
                uint8_t checksum, const uint16_t ucs2[13])
{
	static const int off[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
	memset(de, 0, 32);
	de[0]  = seq | (is_last ? 0x40u : 0x00u);
	de[11] = 0x0F;
	de[13] = checksum;
	for (int i = 0; i < 13; i++) {
		wle16(de + off[i], ucs2[i]);
	}
}

static const char *
write_temp_disk(const disk_t *d, const char *leaf)
{
	static char path[512];
	snprintf(path, sizeof(path), "test_sdcard_fat_%s", leaf);
	FILE *f = fopen(path, "wb");
	if (!f) {
		return NULL;
	}
	fwrite(d->data, SECTOR_SIZE, (size_t)d->sector_count, f);
	fclose(f);
	return path;
}

// ── FAT32 image ───────────────────────────────────────────────────────────────
//
// Geometry chosen so cluster_count = 65664 >= 65525 (FAT32) exactly:
//   65664 clusters × 4 B = 262656 B = 513 sectors per FAT
//
// Absolute LBAs:
//   P_LBA      = 2048  (partition start / VBR)
//   FAT1_LBA   = 2054  (P_LBA + RSVD_SEC)
//   FAT2_LBA   = 2567  (FAT1 + FAT32_SZ)
//   F32_DATA   = 3080  (FAT2 + FAT32_SZ)
//   Cluster n  = F32_DATA + (n-2)
//
// Files:
//   /              dir,  cluster 2  ← FAT32 root (BUG 3 fix registers this)
//   /FILE1.PRG     100 B cluster 4
//   /SUBDIR/        dir, cluster 3
//   /A LONG NAME.PRG  50 B clusters 5→6 (VFAT LFN)
//   /SUBDIR/DEEP.PRG 200 B cluster 7

#define P_LBA        2048u
#define RSVD_SEC     6u
#define FAT32_SZ     513u
#define NUM_FATS     2u
#define F32_SPC      1u
#define F32_TOT_SEC  66696u
#define FAT1_LBA     (P_LBA + RSVD_SEC)
#define FAT2_LBA     (FAT1_LBA + FAT32_SZ)
#define F32_DATA_LBA (FAT2_LBA + FAT32_SZ)
#define F32_DISK_SECTORS (F32_DATA_LBA + 6u + 1u)

static disk_t
build_fat32_image(void)
{
	disk_t d = disk_alloc(F32_DISK_SECTORS);

	// MBR
	uint8_t *mbr = disk_sector(&d, 0);
	mbr[0] = 0xEB; mbr[1] = 0x58; mbr[2] = 0x90;
	uint8_t *pe = mbr + 0x1BE;
	pe[4] = 0x0C;
	wle32(pe + 8,  P_LBA);
	wle32(pe + 12, F32_TOT_SEC);
	mbr[510] = 0x55; mbr[511] = 0xAA;

	// VBR
	uint8_t *vbr = disk_sector(&d, P_LBA);
	vbr[0] = 0xEB; vbr[1] = 0x58; vbr[2] = 0x90;
	memcpy(vbr + 3, "MSDOS5.0", 8);
	wle16(vbr + 11, 512);
	vbr[13] = F32_SPC;
	wle16(vbr + 14, RSVD_SEC);
	vbr[16] = NUM_FATS;
	wle16(vbr + 17, 0);
	wle16(vbr + 19, 0);
	vbr[21] = 0xF8;
	wle16(vbr + 22, 0);
	wle16(vbr + 24, 63);
	wle16(vbr + 26, 255);
	wle32(vbr + 28, 0);
	wle32(vbr + 32, F32_TOT_SEC);
	wle32(vbr + 36, FAT32_SZ);
	wle16(vbr + 40, 0);
	wle16(vbr + 42, 0);
	wle32(vbr + 44, 2);   // root cluster = 2
	wle16(vbr + 48, 1);
	wle16(vbr + 50, 6);
	vbr[510] = 0x55; vbr[511] = 0xAA;

	// FAT entries (both copies)
	uint32_t fat_lbas[2] = { FAT1_LBA, FAT2_LBA };
	for (int fi = 0; fi < 2; fi++) {
		uint32_t fl = fat_lbas[fi];
		fat32_set(&d, fl, 0, 0x0FFFFFF8u);
		fat32_set(&d, fl, 1, 0x0FFFFFFFu);
		fat32_set(&d, fl, 2, 0x0FFFFFFFu); // root dir: EOC
		fat32_set(&d, fl, 3, 0x0FFFFFFFu); // SUBDIR: EOC
		fat32_set(&d, fl, 4, 0x0FFFFFFFu); // FILE1.PRG: EOC
		fat32_set(&d, fl, 5, 6u);           // LONGNAME: → 6
		fat32_set(&d, fl, 6, 0x0FFFFFFFu); // LONGNAME: EOC
		fat32_set(&d, fl, 7, 0x0FFFFFFFu); // DEEP.PRG: EOC
	}

	// Root directory (cluster 2 = F32_DATA_LBA)
	uint8_t *root = disk_sector(&d, F32_DATA_LBA + 0);

	write_dirent_83(root + 0*32, "FILE1   PRG", 0x20, 4, 100);
	write_dirent_83(root + 1*32, "SUBDIR     ", 0x10, 3, 0);

	// VFAT LFN for "A LONG NAME.PRG" (15 chars, 2 LFN slots)
	uint8_t short83[11];
	memcpy(short83, "ALONGN~1PRG", 11);
	uint8_t ck = lfn_checksum(short83);

	const char *lfn_name = "A LONG NAME.PRG";
	uint16_t frag1[13], frag2[13];
	for (int i = 0; i < 13; i++) {
		frag1[i] = (uint16_t)(unsigned char)lfn_name[i];
	}
	frag2[0] = (uint16_t)(unsigned char)lfn_name[13];
	frag2[1] = (uint16_t)(unsigned char)lfn_name[14];
	frag2[2] = 0x0000u;
	for (int i = 3; i < 13; i++) { frag2[i] = 0xFFFFu; }

	write_lfn_entry(root + 2*32, 2, true,  ck, frag2);
	write_lfn_entry(root + 3*32, 1, false, ck, frag1);
	write_dirent_83(root + 4*32, (const char *)short83, 0x20, 5, 50);

	// SUBDIR (cluster 3 = F32_DATA_LBA+1)
	uint8_t *sub = disk_sector(&d, F32_DATA_LBA + 1);
	write_dirent_83(sub + 0*32, ".          ", 0x10, 3, 0);
	write_dirent_83(sub + 1*32, "..         ", 0x10, 2, 0);
	write_dirent_83(sub + 2*32, "DEEP    PRG", 0x20, 7, 200);

	return d;
}

// ── FAT16 image ───────────────────────────────────────────────────────────────
//
// Superfloppy; cluster_count = 4162 (4085 <= 4162 < 65525 → FAT16).
//
//   FAT1_16  = LBA 4,  size 16 sectors
//   FAT2_16  = LBA 20, size 16 sectors
//   Root dir = LBA 36 (2 sectors for 32 entries)
//   Data     = LBA 38  (cluster 2 = ONE.TXT)

#define F16_RSVD     4u
#define F16_FATSZ    16u
#define F16_NFATS    2u
#define F16_ROOTENT  32u
#define F16_ROOTSEC  2u    // ceil(32*32/512) = 2
#define F16_SPC      1u
#define F16_TOT      4200u
#define F16_FAT1_LBA F16_RSVD
#define F16_FAT2_LBA (F16_FAT1_LBA + F16_FATSZ)
#define F16_ROOT_LBA (F16_FAT2_LBA + F16_FATSZ)
#define F16_DATA_LBA (F16_ROOT_LBA + F16_ROOTSEC)
#define F16_DISK_SECTORS (F16_DATA_LBA + 2u)

static disk_t
build_fat16_image(void)
{
	disk_t d = disk_alloc(F16_DISK_SECTORS);

	uint8_t *vbr = disk_sector(&d, 0);
	vbr[0] = 0xEB; vbr[1] = 0x3C; vbr[2] = 0x90;
	memcpy(vbr + 3, "MSDOS5.0", 8);
	wle16(vbr + 11, 512);
	vbr[13] = F16_SPC;
	wle16(vbr + 14, F16_RSVD);
	vbr[16] = F16_NFATS;
	wle16(vbr + 17, F16_ROOTENT);
	wle16(vbr + 19, (uint16_t)F16_TOT);
	vbr[21] = 0xF8;
	wle16(vbr + 22, (uint16_t)F16_FATSZ);
	wle16(vbr + 24, 63);
	wle16(vbr + 26, 255);
	wle32(vbr + 28, 0);
	wle32(vbr + 32, 0);
	vbr[510] = 0x55; vbr[511] = 0xAA;

	uint32_t fat_lbas[2] = { F16_FAT1_LBA, F16_FAT2_LBA };
	for (int fi = 0; fi < 2; fi++) {
		uint32_t fl = fat_lbas[fi];
		fat16_set(&d, fl, 0, 0xFFF8u);
		fat16_set(&d, fl, 1, 0xFFFFu);
		fat16_set(&d, fl, 2, 0xFFFFu);
	}

	uint8_t *rdir = disk_sector(&d, F16_ROOT_LBA);
	write_dirent_83(rdir + 0*32, "ONE     TXT", 0x20, 2, 42);

	return d;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static const sdcard_fat_file_t *
find_file(const char *path)
{
	int n = sdcard_fat_file_count();
	for (int i = 0; i < n; i++) {
		const sdcard_fat_file_t *f = sdcard_fat_file_at(i);
		if (f && strcmp(f->path, path) == 0) {
			return f;
		}
	}
	return NULL;
}

// Open a disk image, call sdcard_fat_build, run tests, then free+close.
// Returns the opened handle so callers can close after their tests.
static struct x16file *
open_and_build(const char *path, bool expect_ok, const char *label)
{
	struct x16file *img = x16open(path, "rb");
	check(img != NULL, label);
	if (!img) {
		return NULL;
	}
	bool ok = sdcard_fat_build(img);
	if (expect_ok) {
		check(ok, "sdcard_fat_build succeeds");
	} else {
		check(!ok, "sdcard_fat_build returns false (expected)");
	}
	return img;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void
test_fat32(void)
{
	printf("\n--- FAT32 image tests ---\n");

	disk_t      d    = build_fat32_image();
	const char *path = write_temp_disk(&d, "fat32.img");
	disk_free(&d);

	check(path != NULL, "FAT32 image written to disk");
	if (!path) { return; }

	struct x16file *img = open_and_build(path, true, "FAT32 image opened");
	if (!img) { remove(path); return; }

	check(sdcard_fat_ready(),                           "sdcard_fat_ready() is true");
	check(strcmp(sdcard_fat_type_name(), "FAT32") == 0, "type name is FAT32");

	// "/" root entry + FILE1.PRG + SUBDIR + LONGNAME + DEEP.PRG = 5 entries
	check(sdcard_fat_file_count() >= 5, "file count >= 5 (root + 4 files)");
	printf("  file count: %d\n", sdcard_fat_file_count());

	check(find_file("/")               != NULL, "found / (FAT32 root dir)");
	check(find_file("/FILE1.PRG")      != NULL, "found /FILE1.PRG");
	check(find_file("/SUBDIR")         != NULL, "found /SUBDIR");
	check(find_file("/A LONG NAME.PRG") != NULL, "found /A LONG NAME.PRG via LFN");
	check(find_file("/SUBDIR/DEEP.PRG") != NULL, "found /SUBDIR/DEEP.PRG");

	const sdcard_fat_file_t *f1 = find_file("/FILE1.PRG");
	if (f1) { check_eq(f1->size, 100u, "/FILE1.PRG size == 100"); }

	// ── Region lookups ────────────────────────────────────────────────────────

	// MBR
	{
		sdcard_fat_region_t r = sdcard_fat_lookup(0, NULL, NULL);
		check(r == SDCARD_FAT_REGION_MBR, "LBA 0 is REGION_MBR");
	}

	// FAT1
	{
		sdcard_fat_region_t r = sdcard_fat_lookup(FAT1_LBA, NULL, NULL);
		check(r == SDCARD_FAT_REGION_FAT, "LBA FAT1_LBA is REGION_FAT");
	}

	// FILE1.PRG: cluster 4 = F32_DATA_LBA+2
	{
		const sdcard_fat_file_t *hf = NULL;
		uint64_t off = 999u;
		sdcard_fat_region_t r = sdcard_fat_lookup(F32_DATA_LBA + 2u, &hf, &off);
		check(r == SDCARD_FAT_REGION_FILE,                          "cluster 4 is REGION_FILE");
		check(hf != NULL && strcmp(hf->path, "/FILE1.PRG") == 0,   "  ...names /FILE1.PRG");
		check_eq((uint32_t)off, 0u, "  ...offset 0");
	}

	// LONGNAME cluster 6 = F32_DATA_LBA+4 (second cluster → offset 512)
	{
		const sdcard_fat_file_t *hf = NULL;
		uint64_t off = 0u;
		sdcard_fat_region_t r = sdcard_fat_lookup(F32_DATA_LBA + 4u, &hf, &off);
		check(r == SDCARD_FAT_REGION_FILE,                              "cluster 6 is REGION_FILE");
		check(hf != NULL && strcmp(hf->path, "/A LONG NAME.PRG") == 0, "  ...names /A LONG NAME.PRG");
		check_eq((uint32_t)off, 512u, "  ...offset 512 (second cluster)");
	}

	// BUG 3: FAT32 root directory cluster (cluster 2 = F32_DATA_LBA)
	// Must return REGION_DIR, NOT REGION_FREE.
	{
		const sdcard_fat_file_t *hf = NULL;
		uint64_t off = 999u;
		sdcard_fat_region_t r = sdcard_fat_lookup(F32_DATA_LBA + 0u, &hf, &off);
		check(r == SDCARD_FAT_REGION_DIR,
		      "FAT32 root dir LBA is REGION_DIR (not FREE)");
		check(hf != NULL && strcmp(hf->path, "/") == 0,
		      "  ...attributed to '/'");
		check_eq((uint32_t)off, 0u, "  ...at offset 0");
	}

	// BUG 3: write to FAT32 root dir must mark the index stale.
	{
		check(!sdcard_fat_is_stale(), "not stale before root-dir write");
		sdcard_fat_note_access(F32_DATA_LBA + 0u, true, 512);
		check(sdcard_fat_is_stale(), "stale after write to FAT32 root dir sector");
	}

	// Unallocated data LBA
	{
		sdcard_fat_region_t r = sdcard_fat_lookup(F32_DATA_LBA + 100u, NULL, NULL);
		check(r == SDCARD_FAT_REGION_FREE || r == SDCARD_FAT_REGION_UNKNOWN,
		      "unallocated LBA returns FREE or UNKNOWN (no crash)");
	}

	sdcard_fat_free();
	check(!sdcard_fat_ready(), "sdcard_fat_ready() is false after free");

	x16close(img);
	remove(path);
}

static void
test_fat16(void)
{
	printf("\n--- FAT16 image tests ---\n");

	disk_t      d    = build_fat16_image();
	const char *path = write_temp_disk(&d, "fat16.img");
	disk_free(&d);

	check(path != NULL, "FAT16 image written to disk");
	if (!path) { return; }

	struct x16file *img = open_and_build(path, true, "FAT16 image opened");
	if (!img) { remove(path); return; }

	check(sdcard_fat_ready(),                           "sdcard_fat_ready() is true");
	check(strcmp(sdcard_fat_type_name(), "FAT16") == 0, "type name is FAT16");

	check(find_file("/ONE.TXT") != NULL, "found /ONE.TXT");
	const sdcard_fat_file_t *ft = find_file("/ONE.TXT");
	if (ft) { check_eq(ft->size, 42u, "/ONE.TXT size == 42"); }

	// FAT region
	{
		sdcard_fat_region_t r = sdcard_fat_lookup(F16_FAT1_LBA, NULL, NULL);
		check(r == SDCARD_FAT_REGION_FAT, "LBA 4 is REGION_FAT");
	}

	// Fixed root directory
	{
		sdcard_fat_region_t r = sdcard_fat_lookup(F16_ROOT_LBA, NULL, NULL);
		check(r == SDCARD_FAT_REGION_ROOTDIR, "LBA 36 is REGION_ROOTDIR (FAT16 fixed)");
	}

	// ONE.TXT cluster 2 = F16_DATA_LBA
	{
		const sdcard_fat_file_t *hf = NULL;
		uint64_t off = 999u;
		sdcard_fat_region_t r = sdcard_fat_lookup(F16_DATA_LBA, &hf, &off);
		check(r == SDCARD_FAT_REGION_FILE,                      "cluster 2 is REGION_FILE");
		check(hf != NULL && strcmp(hf->path, "/ONE.TXT") == 0, "  ...names /ONE.TXT");
		check_eq((uint32_t)off, 0u, "  ...offset 0");
	}

	// Superfloppy: LBA 0 is reserved (VBR), not MBR
	{
		sdcard_fat_region_t r = sdcard_fat_lookup(0, NULL, NULL);
		check(r == SDCARD_FAT_REGION_RESERVED, "FAT16 superfloppy LBA 0 is RESERVED (VBR)");
	}

	sdcard_fat_free();
	x16close(img);
	remove(path);
}

static void
test_garbage_image(void)
{
	printf("\n--- Garbage image tests ---\n");

	disk_t d = disk_alloc(2u);
	const char *path = write_temp_disk(&d, "garbage.img");
	disk_free(&d);

	check(path != NULL, "garbage image written");
	if (!path) { return; }

	struct x16file *img = x16open(path, "rb");
	check(img != NULL, "garbage image opened");
	if (!img) { remove(path); return; }

	bool ok = sdcard_fat_build(img);
	check(!ok,                 "sdcard_fat_build returns false for zero image");
	check(!sdcard_fat_ready(), "sdcard_fat_ready() is false after failed build");

	sdcard_fat_region_t r = sdcard_fat_lookup(0, NULL, NULL);
	check(r == SDCARD_FAT_REGION_UNKNOWN, "lookup on unbuilt index returns UNKNOWN");

	x16close(img);
	remove(path);
}

static void
test_stale(void)
{
	printf("\n--- Stale tracking tests ---\n");

	disk_t      d    = build_fat32_image();
	const char *path = write_temp_disk(&d, "stale.img");
	disk_free(&d);

	if (!path) { return; }

	struct x16file *img = x16open(path, "rb");
	check(img != NULL, "stale test: image opened");
	if (!img) { remove(path); return; }

	bool ok = sdcard_fat_build(img);
	check(ok,                    "stale test: build succeeds");
	check(!sdcard_fat_is_stale(), "not stale immediately after build");

	// Write to a FAT sector must mark stale
	sdcard_fat_note_access(FAT1_LBA, true, 512);
	check(sdcard_fat_is_stale(), "stale after write to a FAT sector");

	// Rebuild; read of file data must NOT mark stale
	sdcard_fat_free();
	sdcard_fat_build(img);
	check(!sdcard_fat_is_stale(), "not stale after rebuild");
	sdcard_fat_note_access(F32_DATA_LBA + 2u, false, 512); // read FILE1.PRG
	check(!sdcard_fat_is_stale(), "read of file data does not mark stale");

	sdcard_fat_free();
	x16close(img);
	remove(path);
}

// ── main ──────────────────────────────────────────────────────────────────────

int
main(void)
{
	test_fat32();
	test_fat16();
	test_garbage_image();
	test_stale();

	return x16_test_summary("sdcard_fat");
}
