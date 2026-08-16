// Commander X16 Emulator — SD card FAT index
// Copyright (c) 2024 X16Emu ADD contributors
// All rights reserved. License: 2-clause BSD
//
// Read-only FAT16/FAT32 index over an SD card image.  Borrows the caller's
// already-open file handle and never opens or closes it.

#ifndef SDCARD_FAT_H
#define SDCARD_FAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration so C++ panels can include this header without pulling in
// files.h (which includes SDL.h).
struct x16file;

// ── Region tags ─────────────────────────────────────────────────────────────
typedef enum {
	SDCARD_FAT_REGION_UNKNOWN = 0,
	SDCARD_FAT_REGION_MBR,
	SDCARD_FAT_REGION_RESERVED,
	SDCARD_FAT_REGION_FAT,
	SDCARD_FAT_REGION_ROOTDIR,
	SDCARD_FAT_REGION_FILE,
	SDCARD_FAT_REGION_DIR,
	SDCARD_FAT_REGION_FREE,
} sdcard_fat_region_t;

// ── File/directory entry ─────────────────────────────────────────────────────
typedef struct {
	const char *path;          // e.g. "/GAMES/FOO.PRG", owned by the index
	uint32_t    size;          // file size in bytes (0 for directories)
	uint32_t    first_cluster;
	bool        is_dir;
	uint64_t    bytes_read;    // session counter, updated by sdcard_fat_note_access
	uint64_t    bytes_written;
} sdcard_fat_file_t;

// ── Lifecycle ────────────────────────────────────────────────────────────────

// Build or rebuild the index using the already-open image handle `img`.
// The handle is borrowed: sdcard_fat never closes it, and save/restores the
// stream position around every internal read so the caller's file position is
// unchanged on return.
// Returns false if the image cannot be parsed (not a FAT image, I/O error, etc.).
// On false the previous index (if any) is freed.  Safe to call repeatedly.
bool sdcard_fat_build(struct x16file *img);

// Release all index memory.  Safe to call when not built.
void sdcard_fat_free(void);

// ── Global flag ──────────────────────────────────────────────────────────────

// When false, sdcard_attach() will not build the index.  Owned by the debugger
// UI; defaults to true so a build without the UI behaves as before.
extern bool sdcard_fat_autoindex;

// ── State queries ────────────────────────────────────────────────────────────

bool        sdcard_fat_ready(void);
bool        sdcard_fat_is_stale(void);
void        sdcard_fat_mark_stale(void);
bool        sdcard_fat_truncated(void);
const char *sdcard_fat_type_name(void);   // "FAT16" / "FAT32" / ""
uint32_t    sdcard_fat_bytes_per_cluster(void);

// ── Lookup ───────────────────────────────────────────────────────────────────

sdcard_fat_region_t sdcard_fat_lookup(uint32_t lba,
                                      const sdcard_fat_file_t **out_file,
                                      uint64_t                 *out_offset);

void sdcard_fat_note_access(uint32_t lba, bool is_write, uint32_t bytes);

// ── Enumeration ─────────────────────────────────────────────────────────────

int                      sdcard_fat_file_count(void);
const sdcard_fat_file_t *sdcard_fat_file_at(int i);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_FAT_H
