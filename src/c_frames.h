// Commander X16 Emulator — a shadow of the live JSRs, for finding C frames.
//
// cc65 addresses a function's variables from the C stack pointer as it stood
// when the function was entered, and nothing on the machine keeps that value.
// So it is recorded at each JSR, against the stack slot of the return address.
// Fed by the CPU flow hook. Reads the machine only through what it is
// configured with, so it links alone. Never part of a save state.

#ifndef _C_FRAMES_H_
#define _C_FRAMES_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// JSR pushes two bytes, so page 1 cannot hold more live calls than this.
#define C_FRAMES_MAX 128

typedef struct {
	uint16_t slot;   // where the return address's low byte is, as call_stack reads it
	uint16_t ret;    // the address pushed: the JSR's last byte
	uint16_t c_sp;   // the C stack pointer at the call
	uint16_t target; // where the call went, through callax and JMP stubs
} c_frame_record_t;

typedef struct {
	uint16_t        c_sp_addr;           // zero page
	int             callax;              // cc65's indirect call helper, -1 if unknown
	const uint8_t  *low_ram;             // the CPU's view of $0000-$9EFF
	const uint8_t  *e;                   // the emulation flag
	const uint16_t *dp;                  // the direct page register
	uint8_t         (*read)(uint16_t a); // any address; must not touch the machine
	uint16_t        (*ax)(void);         // A in the low byte, X in the high byte
} c_frames_config_t;

// Replaces the configuration and drops every record.
void c_frames_configure(const c_frames_config_t *config);
void c_frames_clear(void);

// A cpu_flow_hook_fn.
void c_frames_flow(int kind, uint32_t pc, uint16_t sp, uint16_t ret);

// Records still live at stack pointer `sp`. Takes `sp` because a longjmp raises
// it with no event to report it.
int  c_frames_live(uint16_t sp);
bool c_frames_at_slot(uint16_t sp, uint16_t slot, c_frame_record_t *out);

// Finds the C stack pointer and callax among the symbols, by exact name. `c_sp`
// is the name since cc65 2.19 and `sp` before it; a stack pointer has to be in
// the zero page. False when there is none or the loaded files disagree.
// `*callax` is -1 when it is missing or ambiguous.
typedef bool (*c_frames_symbol_at_fn)(int index, const char **name, uint32_t *addr);
bool c_frames_pick_symbols(int count, c_frames_symbol_at_fn at, uint16_t *c_sp_addr, int *callax);

#ifdef __cplusplus
}
#endif

#endif
