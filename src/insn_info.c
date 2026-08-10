// Commander X16 Emulator
// Instruction reference data for the debugger. See insn_info.h.

#include "insn_info.h"

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define NZ   (INSN_FLAG_N | INSN_FLAG_Z)
#define NZC  (NZ | INSN_FLAG_C)
#define NZCV (NZC | INSN_FLAG_V)

// Keyed by mnemonic, covering the 65C02 set plus the 65C816 additions. Several
// mnemonics cover many opcodes/addressing modes; the description is the same for
// all of them, and the addressing mode is already visible in the operand text.
static const insn_info_t table[] = {
	// ---- loads / stores ----
	{ "lda", "Load Accumulator",        "A = operand",                                  INSN_KIND_LOAD,  NZ, 0, false },
	{ "ldx", "Load X",                  "X = operand",                                  INSN_KIND_LOAD,  NZ, 0, false },
	{ "ldy", "Load Y",                  "Y = operand",                                  INSN_KIND_LOAD,  NZ, 0, false },
	{ "sta", "Store Accumulator",       "operand = A",                                  INSN_KIND_STORE, 0,  0, false },
	{ "stx", "Store X",                 "operand = X",                                  INSN_KIND_STORE, 0,  0, false },
	{ "sty", "Store Y",                 "operand = Y",                                  INSN_KIND_STORE, 0,  0, false },
	{ "stz", "Store Zero",              "operand = 0",                                  INSN_KIND_STORE, 0,  0, false },

	// ---- transfers ----
	{ "tax", "Transfer A to X",         "X = A",                                        INSN_KIND_TRANSFER, NZ, 0, false },
	{ "tay", "Transfer A to Y",         "Y = A",                                        INSN_KIND_TRANSFER, NZ, 0, false },
	{ "txa", "Transfer X to A",         "A = X",                                        INSN_KIND_TRANSFER, NZ, 0, false },
	{ "tya", "Transfer Y to A",         "A = Y",                                        INSN_KIND_TRANSFER, NZ, 0, false },
	{ "tsx", "Transfer SP to X",        "X = SP",                                       INSN_KIND_TRANSFER, NZ, 0, false },
	{ "txs", "Transfer X to SP",        "SP = X",                                       INSN_KIND_TRANSFER, 0,  0, false },
	{ "tcd", "Transfer C to DP",        "DP = C (65C816)",                              INSN_KIND_TRANSFER, NZ, 0, false },
	{ "tdc", "Transfer DP to C",        "C = DP (65C816)",                              INSN_KIND_TRANSFER, NZ, 0, false },
	{ "tcs", "Transfer C to SP",        "SP = C (65C816)",                              INSN_KIND_TRANSFER, 0,  0, false },
	{ "tsc", "Transfer SP to C",        "C = SP (65C816)",                              INSN_KIND_TRANSFER, NZ, 0, false },
	{ "txy", "Transfer X to Y",         "Y = X (65C816)",                               INSN_KIND_TRANSFER, NZ, 0, false },
	{ "tyx", "Transfer Y to X",         "X = Y (65C816)",                               INSN_KIND_TRANSFER, NZ, 0, false },
	{ "xba", "Exchange B and A",        "swaps the two halves of C (65C816)",           INSN_KIND_TRANSFER, NZ, 0, false },
	{ "xce", "Exchange Carry and E",    "swaps C with the emulation flag - the only "
	                                    "way to switch CPU mode (65C816)",              INSN_KIND_FLAG, INSN_FLAG_C, 0, false },

	// ---- arithmetic / logic ----
	{ "adc", "Add with Carry",          "A = A + operand + C",                          INSN_KIND_ALU, NZCV, 0, false },
	{ "sbc", "Subtract with Carry",     "A = A - operand - (1-C)",                      INSN_KIND_ALU, NZCV, 0, false },
	{ "and", "Logical AND",             "A = A & operand",                              INSN_KIND_ALU, NZ, 0, false },
	{ "ora", "Logical OR",              "A = A | operand",                              INSN_KIND_ALU, NZ, 0, false },
	{ "eor", "Exclusive OR",            "A = A ^ operand",                              INSN_KIND_ALU, NZ, 0, false },

	// ---- compares ----
	{ "cmp", "Compare Accumulator",     "sets flags from A - operand (A unchanged)",    INSN_KIND_COMPARE, NZC, 0, false },
	{ "cpx", "Compare X",               "sets flags from X - operand (X unchanged)",    INSN_KIND_COMPARE, NZC, 0, false },
	{ "cpy", "Compare Y",               "sets flags from Y - operand (Y unchanged)",    INSN_KIND_COMPARE, NZC, 0, false },
	{ "bit", "Bit Test",                "Z from A & operand; N and V from operand bits 7 and 6",
	                                                                                    INSN_KIND_BIT, (NZ | INSN_FLAG_V), 0, false },

	// ---- increment / decrement ----
	{ "inc", "Increment",               "operand = operand + 1 (A when no operand)",    INSN_KIND_INCDEC, NZ, 0, false },
	{ "dec", "Decrement",               "operand = operand - 1 (A when no operand)",    INSN_KIND_INCDEC, NZ, 0, false },
	{ "inx", "Increment X",             "X = X + 1",                                    INSN_KIND_INCDEC, NZ, 0, false },
	{ "iny", "Increment Y",             "Y = Y + 1",                                    INSN_KIND_INCDEC, NZ, 0, false },
	{ "dex", "Decrement X",             "X = X - 1",                                    INSN_KIND_INCDEC, NZ, 0, false },
	{ "dey", "Decrement Y",             "Y = Y - 1",                                    INSN_KIND_INCDEC, NZ, 0, false },

	// ---- shifts / rotates ----
	{ "asl", "Arithmetic Shift Left",   "operand <<= 1; C = old bit 7",                 INSN_KIND_SHIFT, NZC, 0, false },
	{ "lsr", "Logical Shift Right",     "operand >>= 1; C = old bit 0",                 INSN_KIND_SHIFT, NZC, 0, false },
	{ "rol", "Rotate Left",             "operand <<= 1 through C",                      INSN_KIND_SHIFT, NZC, 0, false },
	{ "ror", "Rotate Right",            "operand >>= 1 through C",                      INSN_KIND_SHIFT, NZC, 0, false },
	{ "trb", "Test and Reset Bits",     "Z from A & operand; clears the bits of A in operand",
	                                                                                    INSN_KIND_BIT, INSN_FLAG_Z, 0, false },
	{ "tsb", "Test and Set Bits",       "Z from A & operand; sets the bits of A in operand",
	                                                                                    INSN_KIND_BIT, INSN_FLAG_Z, 0, false },
	{ "rmb", "Reset Memory Bit",        "clears one bit of the operand",                INSN_KIND_BIT, 0, 0, false },
	{ "smb", "Set Memory Bit",          "sets one bit of the operand",                  INSN_KIND_BIT, 0, 0, false },

	// ---- branches ----
	{ "bcc", "Branch if Carry Clear",   "taken when C = 0",  INSN_KIND_BRANCH, 0, INSN_FLAG_C, false },
	{ "bcs", "Branch if Carry Set",     "taken when C = 1",  INSN_KIND_BRANCH, 0, INSN_FLAG_C, true  },
	{ "beq", "Branch if Equal",         "taken when Z = 1",  INSN_KIND_BRANCH, 0, INSN_FLAG_Z, true  },
	{ "bne", "Branch if Not Equal",     "taken when Z = 0",  INSN_KIND_BRANCH, 0, INSN_FLAG_Z, false },
	{ "bmi", "Branch if Minus",         "taken when N = 1",  INSN_KIND_BRANCH, 0, INSN_FLAG_N, true  },
	{ "bpl", "Branch if Plus",          "taken when N = 0",  INSN_KIND_BRANCH, 0, INSN_FLAG_N, false },
	{ "bvc", "Branch if Overflow Clear","taken when V = 0",  INSN_KIND_BRANCH, 0, INSN_FLAG_V, false },
	{ "bvs", "Branch if Overflow Set",  "taken when V = 1",  INSN_KIND_BRANCH, 0, INSN_FLAG_V, true  },
	{ "bra", "Branch Always",           "always taken",      INSN_KIND_JUMP,   0, 0, false },
	{ "brl", "Branch Long",             "always taken, 16-bit displacement (65C816)",
	                                                         INSN_KIND_JUMP,   0, 0, false },
	{ "bbr", "Branch on Bit Reset",     "taken when the tested bit of the operand is 0",
	                                                         INSN_KIND_BRANCH, 0, 0, false },
	{ "bbs", "Branch on Bit Set",       "taken when the tested bit of the operand is 1",
	                                                         INSN_KIND_BRANCH, 0, 0, false },

	// ---- jumps / calls / returns ----
	{ "jmp", "Jump",                    "PC = operand",                                 INSN_KIND_JUMP, 0, 0, false },
	{ "jml", "Jump Long",               "PC and program bank = operand (65C816)",       INSN_KIND_JUMP, 0, 0, false },
	{ "jsr", "Jump to Subroutine",      "pushes the return address, then PC = operand", INSN_KIND_JUMP, 0, 0, false },
	{ "jsl", "Jump to Subroutine Long", "pushes bank and return address (65C816)",      INSN_KIND_JUMP, 0, 0, false },
	{ "rts", "Return from Subroutine",  "pulls the return address into PC",             INSN_KIND_RETURN, 0, 0, false },
	{ "rtl", "Return from Subroutine Long", "pulls address and bank (65C816)",          INSN_KIND_RETURN, 0, 0, false },
	{ "rti", "Return from Interrupt",   "pulls the status register, then the return address",
	                                                                                    INSN_KIND_RETURN, 0xFF, 0, false },

	// ---- stack ----
	{ "pha", "Push A",                  "pushes the accumulator",                       INSN_KIND_STACK, 0, 0, false },
	{ "phx", "Push X",                  "pushes X",                                     INSN_KIND_STACK, 0, 0, false },
	{ "phy", "Push Y",                  "pushes Y",                                     INSN_KIND_STACK, 0, 0, false },
	{ "php", "Push Status",             "pushes the status register",                   INSN_KIND_STACK, 0, 0, false },
	{ "phb", "Push Data Bank",          "pushes DB (65C816)",                           INSN_KIND_STACK, 0, 0, false },
	{ "phd", "Push Direct Page",        "pushes DP (65C816)",                           INSN_KIND_STACK, 0, 0, false },
	{ "phk", "Push Program Bank",       "pushes K (65C816)",                            INSN_KIND_STACK, 0, 0, false },
	{ "pla", "Pull A",                  "pulls into the accumulator",                   INSN_KIND_STACK, NZ, 0, false },
	{ "plx", "Pull X",                  "pulls into X",                                 INSN_KIND_STACK, NZ, 0, false },
	{ "ply", "Pull Y",                  "pulls into Y",                                 INSN_KIND_STACK, NZ, 0, false },
	{ "plp", "Pull Status",             "pulls the status register",                    INSN_KIND_STACK, 0xFF, 0, false },
	{ "plb", "Pull Data Bank",          "pulls into DB (65C816)",                       INSN_KIND_STACK, NZ, 0, false },
	{ "pld", "Pull Direct Page",        "pulls into DP (65C816)",                       INSN_KIND_STACK, NZ, 0, false },
	{ "pea", "Push Effective Address",  "pushes the 16-bit operand (65C816)",           INSN_KIND_STACK, 0, 0, false },
	{ "pei", "Push Effective Indirect", "pushes the word at the direct-page operand (65C816)",
	                                                                                    INSN_KIND_STACK, 0, 0, false },
	{ "per", "Push Effective Relative", "pushes a PC-relative address (65C816)",        INSN_KIND_STACK, 0, 0, false },

	// ---- flags ----
	{ "clc", "Clear Carry",             "C = 0",                        INSN_KIND_FLAG, INSN_FLAG_C, 0, false },
	{ "sec", "Set Carry",               "C = 1",                        INSN_KIND_FLAG, INSN_FLAG_C, 0, false },
	{ "cld", "Clear Decimal",           "D = 0 (binary arithmetic)",    INSN_KIND_FLAG, INSN_FLAG_D, 0, false },
	{ "sed", "Set Decimal",             "D = 1 (BCD arithmetic)",       INSN_KIND_FLAG, INSN_FLAG_D, 0, false },
	{ "cli", "Clear Interrupt Disable", "I = 0 (IRQs allowed)",         INSN_KIND_FLAG, INSN_FLAG_I, 0, false },
	{ "sei", "Set Interrupt Disable",   "I = 1 (IRQs masked)",          INSN_KIND_FLAG, INSN_FLAG_I, 0, false },
	{ "clv", "Clear Overflow",          "V = 0",                        INSN_KIND_FLAG, INSN_FLAG_V, 0, false },
	{ "rep", "Reset Status Bits",       "clears the status bits set in the operand (65C816)",
	                                                                    INSN_KIND_FLAG, 0xFF, 0, false },
	{ "sep", "Set Status Bits",         "sets the status bits set in the operand (65C816)",
	                                                                    INSN_KIND_FLAG, 0xFF, 0, false },

	// ---- misc ----
	{ "brk", "Break",                   "software interrupt through the IRQ/BRK vector", INSN_KIND_INTERRUPT, INSN_FLAG_I | INSN_FLAG_D, 0, false },
	{ "cop", "Coprocessor",             "software interrupt through the COP vector (65C816)",
	                                                                     INSN_KIND_INTERRUPT, INSN_FLAG_I | INSN_FLAG_D, 0, false },
	{ "nop", "No Operation",            "does nothing",                  INSN_KIND_NOP, 0, 0, false },
	{ "wai", "Wait for Interrupt",      "halts the CPU until an interrupt arrives",      INSN_KIND_OTHER, 0, 0, false },
	{ "stp", "Stop",                    "stops the CPU until reset",                     INSN_KIND_OTHER, 0, 0, false },
	{ "wdm", "Reserved",                "reserved 65C816 opcode; acts as a 2-byte NOP",  INSN_KIND_NOP, 0, 0, false },
	{ "mvn", "Block Move Next",         "copies a block upward (65C816)",                INSN_KIND_OTHER, 0, 0, false },
	{ "mvp", "Block Move Previous",     "copies a block downward (65C816)",              INSN_KIND_OTHER, 0, 0, false },
};

#define TABLE_COUNT ((int)(sizeof(table) / sizeof(table[0])))

const insn_info_t *
insn_info_lookup(const char *mnemonic)
{
	if (!mnemonic || !mnemonic[0]) {
		return NULL;
	}
	for (int i = 0; i < TABLE_COUNT; i++) {
		if (strcasecmp(table[i].mnemonic, mnemonic) == 0) {
			return &table[i];
		}
	}
	// The bit-addressed 65C02 forms are emitted with the bit number appended
	// (rmb0..rmb7, smb0..smb7, bbr0..bbr7, bbs0..bbs7); match on the stem.
	size_t len = strlen(mnemonic);
	if (len == 4 && mnemonic[3] >= '0' && mnemonic[3] <= '7') {
		char stem[4] = { mnemonic[0], mnemonic[1], mnemonic[2], '\0' };
		for (int i = 0; i < TABLE_COUNT; i++) {
			if (strcasecmp(table[i].mnemonic, stem) == 0) {
				return &table[i];
			}
		}
	}
	return NULL;
}

void
insn_info_flag_text(uint8_t flags_set, char *out, unsigned int max)
{
	static const char *names[8] = { "N", "V", "M", "X", "D", "I", "Z", "C" };
	unsigned int       n        = 0;

	if (!out || max == 0) {
		return;
	}
	out[0] = '\0';
	for (int bit = 7; bit >= 0; bit--) {
		if (!(flags_set & (1u << bit))) {
			continue;
		}
		const char *nm = names[7 - bit];
		unsigned int need = (unsigned int)strlen(nm) + (n ? 1u : 0u);
		if (n + need + 1 >= max) {
			break;
		}
		if (n) {
			out[n++] = ' ';
		}
		strcpy(out + n, nm);
		n += (unsigned int)strlen(nm);
	}
	out[n] = '\0';
}
