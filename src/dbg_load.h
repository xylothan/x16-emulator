// Commander X16 Emulator — debug info for programs the guest loads at runtime.
//
// Debug info given on the command line only ever describes the program that was
// named there. The moment the program under test loads something of its own --
// an overlay, a second stage, a level -- source lookups stop describing what is
// actually executing. This watches host-filesystem loads and merges the debug
// info that sits beside each loaded file.
//
// The facts about a load are collected per channel, as the transfer happens,
// rather than read out of globals when it finishes. Two channels can be open at
// once, and a load that crosses $C000 moves the RAM bank mid-transfer, so
// anything sampled at the end describes the wrong thing.

#ifndef _DBG_LOAD_H_
#define _DBG_LOAD_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBG_LOAD_PATH_MAX 1024
#define DBG_LOAD_CHANNELS 16

// Everything known about one completed load, copied out of the channel so it
// cannot be disturbed by whatever the guest does next.
typedef struct {
	char     path[DBG_LOAD_PATH_MAX]; // host path the data came from
	uint16_t addr;                    // where the first byte landed
	uint8_t  bank;                    // RAM bank mapped at that moment
	uint32_t size;                    // bytes transferred into memory
	bool     valid;
} dbg_load_event_t;

// Whether to act on loads at all.
//
// The policy is a tri-state because "on" normally means "whenever the debugger
// is on", and the debugger can be switched on by the running machine itself
// (emu_write register 0) long after the command line was parsed. A snapshot
// taken at startup would then be wrong for the rest of the session.
//
//   policy < 0  follow the debugger
//   policy == 0 off, whatever the debugger is doing
//   policy > 0  on, whatever the debugger is doing
void dbg_load_set_policy(int policy);

// The debugger was switched on or off. Ignored unless the policy is to follow.
void dbg_load_note_debugger(bool on);

// Force it on or off, equivalent to setting the policy to 1 or 0.
void dbg_load_set_enabled(bool on);
bool dbg_load_is_enabled(void);

// ---- Reported by the file layer --------------------------------------------
// The host path behind -prg. That load bypasses filename resolution entirely:
// the KERNAL asks for ":*" and the file layer hands back an already-open
// handle, so nothing ever resolves a name for it. Without recording the path up
// front, the most common debugging workflow there is would be the one case that
// silently got no symbols.
void dbg_load_set_prg_path(const char *path);

// A channel was opened onto that -prg file.
void dbg_load_begin_prg(int channel);

// A channel was opened to read `host_path`. Forgets anything the channel was
// previously doing.
void dbg_load_begin(int channel, const char *host_path);

// One byte of a block transfer is going to `addr` with `bank` mapped, `last`
// marking the final byte of the file. Reported just before the write rather
// than just after, because the transfer loop closes its channel the moment it
// sees the end of the file.
//
// Counting the byte and finishing the load are one call on purpose. They were
// three, and that close landed between them -- so the completion ran a few
// lines before the last byte was counted, and every load came out one byte
// short. Doing both here means the call site cannot order them wrongly.
//
// Only the first call for a channel records the destination: that is the one
// that says where the program starts, and which bank it started in. A load
// crossing $C000 moves the RAM bank on as it goes, so a later byte would name a
// different segment.
void dbg_load_note_byte(int channel, uint16_t addr, uint8_t bank, bool last);

// The channel was closed without finishing; drop what was collected.
void dbg_load_abandon(int channel);

// ---- Consumed by the debugger ----------------------------------------------
// Take the completed load, if there is one. Clears it, so it is reported once.
bool dbg_load_take(dbg_load_event_t *out);

// Take any completed load and merge the debug info beside it: load the .dbg
// first, then tell it which bank the program went into, because loading
// replaces the mappings in that address range and would discard an annotation
// made beforehand. Returns true if debug info was loaded.
bool dbg_load_poll(void);

// Whether a host path names a .dbg file. A wildcard LOAD can match the debug
// info sitting beside a program, and reading that as debug info for itself
// would attribute its segments to wherever its text happened to land.
bool dbg_load_path_is_dbg(const char *path);

// Forget everything. For machine reset and tests.
void dbg_load_reset(void);

#ifdef __cplusplus
}
#endif

#endif // _DBG_LOAD_H_
