// Commander X16 Emulator — call stack unwinding.
//
// The 6502 and 65C816 keep no frame pointer, so a call stack can only be
// guessed from the hardware stack: any byte pair that points just past a JSR is
// taken for a return address. A stored value can look like one, and frames
// entered through JSL or the KERNAL's far call are not recognised.
//
// Memory is read through a callback, so this has no dependency on the machine
// and every debugger front end gets the same answer.

#ifndef _CALL_STACK_H_
#define _CALL_STACK_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALL_STACK_MAX 64

// Must read without side effects.
typedef uint8_t (*call_stack_read_t)(uint16_t address, uint8_t bank);

// Fills `frames` (CALL_STACK_MAX entries) innermost first and returns how many.
// frames[0] is `pc`; the rest are the addresses execution would return to.
// `native` is the 65C816's native mode, whose stack is not confined to page 1.
int call_stack_unwind(uint16_t pc, uint16_t sp, uint8_t program_bank, bool native,
                      call_stack_read_t read, uint16_t *frames);

// The instruction a frame is at: the PC for the current frame, the JSR for a
// caller. A return address is the instruction after the call, which can be the
// next source line or the next routine, so name and locate a caller by this.
static inline uint16_t call_stack_site(bool is_current, uint16_t address)
{
	return is_current ? address : (uint16_t)(address - 3);
}

#ifdef __cplusplus
}
#endif

#endif
