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

// Rebuild the FAT index over the attached image, discarding the old one.
// Returns false when nothing is attached or the image is not indexable.
bool sdcard_reindex(void);

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
