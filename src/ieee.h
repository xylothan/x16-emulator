// Commander X16 Emulator
// Copyright (c) 2022 Michael Steil
// All rights reserved. License: 2-clause BSD

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define IEEE_HISTORY_MAX 64

typedef enum {
	IEEE_OP_OPEN        = 0,
	IEEE_OP_OPEN_FAILED = 1,
	IEEE_OP_CLOSE       = 2,
	IEEE_OP_DIR         = 3,
	IEEE_OP_COMMAND     = 4,
	IEEE_OP_STATUS      = 5,
} ieee_op_kind_t;

typedef struct {
	uint32_t seq;           /* monotonic; 0 means "empty slot" */
	uint8_t  kind;          /* ieee_op_kind_t */
	int8_t   channel;       /* -1 when not channel-specific */
	bool     read;
	bool     write;
	char     name[80];      /* filename, command text, or status text */
	char     status[48];    /* resulting DOS status, "" if none yet */
	uint32_t bytes_read;
	uint32_t bytes_written;
} ieee_history_entry_t;

#ifdef __cplusplus
extern "C" {
#endif

void ieee_init();
int SECOND(uint8_t a);
int TKSA(uint8_t a);
int ACPTR(uint8_t *a);
int CIOUT(uint8_t a);
int UNTLK(void);
int UNLSN(void);
int LISTEN(uint8_t a);
int TALK(uint8_t a);
int MACPTR(uint16_t addr, uint16_t *count, uint8_t stream_mode);
int MCIOUT(uint16_t addr, uint16_t *count, uint8_t stream_mode);
int XMACPTR(uint8_t stream_mode);
int XMCIOUT(uint8_t stream_mode);

// ─── Debugger accessor ───────────────────────────────────────────────────────
// Side-effect-free snapshot of IEEE/hostfs state. Safe to call every frame
// from the debugger while the emulation is running.
//
// All strings are NUL-terminated copies; no pointers into emulator state.
// Per-channel bytes_read/bytes_written are cumulative since the channel was
// last opened.

typedef struct {
	bool     is_open;          // channel has an active file handle
	bool     read;             // channel is open for reading
	bool     write;            // channel is open for writing
	char     name[80];         // filename as seen by the DOS layer (NUL-terminated)
	uint32_t bytes_read;       // bytes successfully read from this channel
	uint32_t bytes_written;    // bytes successfully written to this channel
} ieee_channel_debug_t;

typedef struct {
	bool     using_hostfs;              // hostfs is active (vs CMDR-DOS block device)
	int      ieee_unit;                 // emulated device number (usually 8)
	char     hostfscwd[512];            // current host working directory (NUL-terminated)
	int      channel;                   // currently addressed channel (secondary address)
	bool     listening;                 // device is in LISTEN state
	bool     talking;                   // device is in TALK state
	bool     opening;                   // currently accumulating a filename (OPEN in progress)
	char     cmd[256];                  // last DOS command received on channel 15
	int      cmdlen;                    // valid bytes in cmd[]
	char     error_str[256];            // current error string (channel 15 status)
	ieee_channel_debug_t channels[16]; // per-channel state
	int                  history_count;                    // valid entries, oldest first
	ieee_history_entry_t history[IEEE_HISTORY_MAX];
} ieee_debug_state_t;

void ieee_debug_get_state(ieee_debug_state_t *out);

#ifdef __cplusplus
}
#endif
