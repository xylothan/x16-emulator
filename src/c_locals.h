// Commander X16 Emulator — where a C function's variables are.
//
// A value is reported only when its address is known to be right. Everything
// else carries a reason. cc65's debug info has no types, so a variable is an
// address and a run of bytes. Function-level variables only: nested-block and
// `static` locals have no record, and an inner declaration can shadow a listed
// name with no sign of it.
//
// Takes everything it needs as arguments, so it links alone.

#ifndef _C_LOCALS_H_
#define _C_LOCALS_H_

#include "c_frames.h"
#include "dbg_info.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	C_LOCAL_OK = 0,
	C_LOCAL_UNALLOCATED,      // below the current C stack pointer
	C_LOCAL_NOT_STORED,       // last fastcall parameter, still in A/X
	C_LOCAL_UNKNOWN_FRAME,    // no trustworthy base for this frame
	C_LOCAL_REFUSED_VARIADIC, // fixed parameter of a variadic function
	C_LOCAL_REFUSED_REGISTER, // sc=reg
	C_LOCAL_REFUSED_LOCATION, // not wholly in plain RAM
} c_local_status_t;

typedef enum {
	C_FRAME_KNOWN = 0,
	C_FRAME_NOT_SEEN,         // this call was not seen entering
	C_FRAME_NATIVE,           // 65C816 native mode
	C_FRAME_DIRECT_PAGE,      // D != 0
	C_FRAME_BANKED,           // records are not keyed by bank
	C_FRAME_NO_STACK_POINTER, // no usable c_sp or sp in the symbols
	C_FRAME_PROLOGUE,         // the first instruction cannot be classified
} c_frame_state_t;

const char *c_local_status_name(c_local_status_t status);
const char *c_frame_state_name(c_frame_state_t state);
const char *c_frame_state_reason(c_frame_state_t state);

// cc65's prologue helpers: an address, -1 when absent, -2 when the loaded
// symbols disagree.
typedef struct {
	int pusha, pushax, pusheax, enter;
} c_locals_helpers_t;

void c_locals_pick_helpers(int count, c_frames_symbol_at_fn at, c_locals_helpers_t *out);

// Bytes the function's first instruction pushes, or -1 when it is a JSR that
// cannot be classified. `named` says whether any symbol is at an address.
int c_locals_push_bytes(uint16_t entry, uint8_t (*read)(uint16_t),
                        const c_locals_helpers_t *helpers, bool (*named)(uint16_t), bool *variadic);

// Whether the entry JSR has yet to return, for a frame at `address`.
bool c_locals_entry_pending(bool is_current, uint16_t address, uint16_t entry, int push_bytes);

// Whether a record found at the caller's slot describes this activation.
// `lo`/`hi` are the stack bytes at that slot now.
bool c_locals_record_matches(const c_frame_record_t *r, uint16_t slot, uint16_t caller_address,
                             uint16_t entry, uint8_t lo, uint8_t hi);

// C stack bytes cc65's startup pushes before it jumps to main.
#define C_LOCALS_MAIN_START_PUSH 4

// Whether the code at `start` is exactly cc65's callmain ending in a JMP to
// `entry`: load and push argc, load and push argv, LDY #4.
bool c_locals_is_main_start(uint16_t start, uint16_t entry, uint8_t (*read)(uint16_t),
                            const c_locals_helpers_t *helpers);

typedef struct {
	c_frame_state_t state;
	int             base;          // C stack pointer at entry, less what the prologue pushes
	uint16_t        c_sp;          // now
	bool            variadic;
	bool            entry_pending;
} c_frame_view_t;

typedef struct {
	c_local_status_t status;
	bool             is_param;
	bool             has_address;
	uint16_t         address;
	int              size;        // 0 when unknown
} c_local_t;

void c_locals_var(const c_frame_view_t *frame, const dbg_cvar_t *vars, int count, int index,
                  c_local_t *out);

// The first identifier in `expr` that is exactly one of `vars`, or NULL. `$hex`
// and numbers are not identifiers.
const char *c_locals_expr_name(const char *expr, const dbg_cvar_t *vars, int count);

#ifdef __cplusplus
}
#endif

#endif
