// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD
#ifndef _SD_CARD_H_
#define _SD_CARD_H_
#include <inttypes.h>
#include <stdbool.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool sdcard_attached;
void sdcard_set_path(char const *path);
bool sdcard_path_is_set();
void sdcard_attach();
void sdcard_detach();

const char *sdcard_get_path(void);

// Rebuild the FAT index over the attached image, keeping per-file counters.
// Returns false when nothing is attached or the image is not indexable.
bool sdcard_reindex(void);

// Block access history. Consecutive blocks in the same direction that resolve
// to the same file or region are merged into one entry. Kept whether or not
// I/O capture is on, like the host-filesystem history.
#define SDCARD_HISTORY_SIZE 128
#define SDCARD_HISTORY_PATH 96

typedef struct {
	bool     is_write;
	uint32_t first_lba;
	uint32_t blocks;
	uint8_t  region;                     // sdcard_fat_region_t at access time
	uint64_t offset;                     // byte offset into the file at first_lba
	uint32_t command;                    // CMD17/18/24 that moved the data
	char     path[SDCARD_HISTORY_PATH];  // "" when the region is not a file
} sdcard_history_entry_t;

// i = 0 is the newest entry. Returns NULL past the end.
int                           sdcard_history_count(void);
const sdcard_history_entry_t *sdcard_history_at(int i);
void                          sdcard_history_clear(void);

void sdcard_select(bool select);
uint8_t sdcard_handle(uint8_t inbyte);

typedef struct {
	bool        attached;
	bool        selected;
	const char *image_path;           // "" when none
	int64_t     image_size;           // -1 when not attached
	uint8_t     last_cmd;             // command number (0-63)
	bool        last_cmd_is_acmd;
	uint32_t    last_lba;
	bool        is_idle;
	bool        is_initialized;
	bool        ongoing_multiblock_read;
	int         response_length;
	int         response_counter;
	// Session counters:
	uint64_t    blocks_read;
	uint64_t    blocks_written;
	uint64_t    bytes_read;
	uint64_t    bytes_written;
	uint32_t    commands;
} sdcard_debug_state_t;

void sdcard_debug_get_state(sdcard_debug_state_t *out);

#ifdef __cplusplus
}
#endif

#endif
