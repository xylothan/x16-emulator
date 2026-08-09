// Commander X16 Emulator — debugger core: breakpoints.
//
// The breakpoint table and the decision "should execution stop here?", kept
// apart from debugger.c so the logic can be exercised on its own. debugger.c
// is an SDL window; this is not, and the interesting behaviour here -- bank
// matching, conditions, ignore counts -- is exactly the kind that is worth
// testing directly rather than by driving a UI.
//
// Upstream tracked a single breakpoint in one global. That is enough to answer
// "stop at this address" and nothing else: no second breakpoint, no way to say
// which RAM bank was meant, no "only when A == 3", no "skip the first 100
// iterations". Those are the things a debugging session actually needs, and
// they are what this adds.

#ifndef _DEBUG_CORE_H_
#define _DEBUG_CORE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A breakpoint address. `bank` is the 65C816 program bank; `x16Bank` is the
// RAM/ROM window bank for addresses at $A000 and above, or -1 where the address
// is not banked. Breakpoints are keyed by (pc, bank).
struct breakpoint {
	int     pc;
	uint8_t bank;
	int     x16Bank;
};

extern struct breakpoint *breakPoints;
extern int                numBreakpoints;

// What a condition tests. Values are part of the debugger's external interface
// (the DAP server and the GUI both name them), so do not renumber.
enum {
	BPOPERAND_A = 0,
	BPOPERAND_X,
	BPOPERAND_Y,
	BPOPERAND_SP,
	BPOPERAND_P,
	BPOPERAND_BYTE, // the byte at operand_addr
	BPOPERAND_WORD, // the little-endian word at operand_addr
};

// How it is compared. Also external; do not renumber.
enum {
	BPCMP_EQ = 0,
	BPCMP_NE,
	BPCMP_LT,
	BPCMP_LE,
	BPCMP_GT,
	BPCMP_GE,
};

// ---- Table management ------------------------------------------------------
// Breakpoints are identified by all three of (pc, bank, x16Bank), so the same
// address in two different RAM banks is two different breakpoints. Add returns
// the new index, or -1 if one already exists at that triple or memory ran out.
// Remove returns whether one was there.
int  debug_bp_add(struct breakpoint bp);
bool debug_bp_remove(int pc, uint8_t bank, int x16Bank);
int  debug_bp_find(int pc, uint8_t bank, int x16Bank);
void debug_bp_toggle(int pc, uint8_t bank, int x16Bank);
void debug_bp_clear_all(void);

// ---- The hot-path question -------------------------------------------------
// Tell the core the CPU has arrived at (pc, bank), and get back whether to stop.
//
// Named as an event rather than a question because it IS one: it counts the hit
// and spends the ignore budget. Call it once per instruction, only while
// actually running, and only from whatever owns execution. Calling it while
// halted re-counts the same arrival -- the debugger polls with the PC parked on
// the breakpoint, so that would burn through an ignore count while the machine
// stands still. Use debug_bp_is_set() for the read-only question.
bool debug_bp_on_arrival(int pc, uint8_t bank);

// Whether any breakpoint covers (pc, bank), asking nothing and changing
// nothing. This is the one to use for "is there a breakpoint here?".
bool debug_bp_is_set(int pc, uint8_t bank);

// ---- Conditions and counts -------------------------------------------------
// A breakpoint stops when its condition (if any) holds AND it has been hit more
// times than its ignore count. Counting is per breakpoint and starts the first
// time that breakpoint is reached, so a count exists even for one that was
// never given a condition. Only arrivals that satisfy the condition are
// counted, so "ignore the first 100" means 100 that would otherwise have
// stopped, not 100 that merely passed through.
//
// A count deliberately survives removing and re-adding the same breakpoint,
// since that is how a UI implements an enable/disable toggle and a toggle
// should not silently reset it. The two calls that do discard it are
// debug_bp_forget(), for one breakpoint, and debug_bp_clear_all(), which is a
// delete of everything rather than a disable.
void     debug_bp_set_condition(int pc, uint8_t bank, int x16Bank, int operand,
                                uint16_t operand_addr, int op, uint32_t value);
void     debug_bp_clear_condition(int pc, uint8_t bank, int x16Bank);
void     debug_bp_forget(int pc, uint8_t bank, int x16Bank);
void     debug_bp_set_ignore(int pc, uint8_t bank, int x16Bank, uint32_t ignore);
uint32_t debug_bp_get_hits(int pc, uint8_t bank, int x16Bank);
void     debug_bp_reset_hits(int pc, uint8_t bank, int x16Bank);
bool     debug_bp_get_condition(int pc, uint8_t bank, int x16Bank, int *has_cond, int *operand,
                                uint16_t *operand_addr, int *op, uint32_t *value,
                                uint32_t *ignore);

// Release everything. Only needed at shutdown or in tests.
void debug_core_free(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _DEBUG_CORE_H_
