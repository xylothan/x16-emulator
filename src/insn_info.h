// Commander X16 Emulator
// Instruction reference data for the debugger.
//
// disasm.c can render an instruction, but it carries no description of what an
// instruction *does*: no summary, no flag effects, no "will this branch be
// taken". The debug UI wants all of that for its hover tooltips, so it lives
// here, keyed by mnemonic rather than by opcode (the mnemonic is what the user
// hovers in a source listing, and it is what disasm.c already produces).

#ifndef _INSN_INFO_H_
#define _INSN_INFO_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Status bits an instruction can affect, for insn_info_t.flags_set.
#define INSN_FLAG_N 0x80
#define INSN_FLAG_V 0x40
#define INSN_FLAG_M 0x20
#define INSN_FLAG_X 0x10
#define INSN_FLAG_D 0x08
#define INSN_FLAG_I 0x04
#define INSN_FLAG_Z 0x02
#define INSN_FLAG_C 0x01

// What kind of instruction this is, so the debugger can decide what extra
// information is worth predicting.
typedef enum {
	INSN_KIND_OTHER = 0,
	INSN_KIND_LOAD,        // reads memory into a register
	INSN_KIND_STORE,       // writes a register to memory
	INSN_KIND_TRANSFER,    // register to register
	INSN_KIND_ALU,         // arithmetic / logic
	INSN_KIND_SHIFT,
	INSN_KIND_COMPARE,
	INSN_KIND_BRANCH,      // conditional relative branch
	INSN_KIND_JUMP,        // unconditional jump/call
	INSN_KIND_RETURN,
	INSN_KIND_STACK,
	INSN_KIND_FLAG,        // sets/clears a status bit
	INSN_KIND_INCDEC,
	INSN_KIND_BIT,
	INSN_KIND_INTERRUPT,
	INSN_KIND_NOP,
} insn_kind_t;

typedef struct {
	const char *mnemonic;   // lower case, as disasm.c emits
	const char *title;      // e.g. "Load Accumulator"
	const char *summary;    // one line of what it does
	insn_kind_t kind;
	uint8_t     flags_set;  // INSN_FLAG_* bits this instruction can change
	// For branches: the status bit tested and the value it must have for the
	// branch to be taken. Zero for everything else.
	uint8_t     branch_flag;
	bool        branch_when_set;
} insn_info_t;

// Look up by mnemonic (case-insensitive). NULL if unknown.
const insn_info_t *insn_info_lookup(const char *mnemonic);

// Render "N V - - - - Z C"-style text listing the flags an instruction touches.
// Writes at most `max` bytes including the terminator. Empty when none.
void insn_info_flag_text(uint8_t flags_set, char *out, unsigned int max);

#ifdef __cplusplus
}
#endif

#endif
