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

// A bank selector, stored on a breakpoint or watchpoint.
//
// Banks only mean anything at $A000-$FFFF: the RAM window follows the RAM bank,
// the ROM window the ROM bank, and everything below $A000 selects memory by
// address alone. So there are exactly two things a selector can say --
// "whichever bank is mapped" or "this specific bank" -- and for an address
// outside a banked window it says nothing at all.
//
// A selector recorded for an unbanked address is normalised to DEBUG_BANK_ANY
// on the way in, so it can never become a hidden field that distinguishes two
// otherwise identical entries. (A UI should grey the field out for such an
// address, for the same reason.)
#define DEBUG_BANK_ANY (-1)

// A breakpoint address. `bank` is the 65C816 program bank; `x16Bank` is the
// bank selector described above. Breakpoints are identified by all three.
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

// Does a recorded bank selector apply to `addr` right now? See DEBUG_BANK_ANY.
// Public so that every matcher in the emulator uses one rule -- the step
// breakpoint in debugger.c included -- rather than each re-deriving it and
// disagreeing about what -1 means.
bool debug_bank_selector_matches(int selector, int addr, uint8_t pbank);

// Which RAM/ROM window `pc` sits in given program bank `pbank`, or
// DEBUG_BANK_ANY where a bank means nothing there. Exported so callers that
// produce selectors use the same rule as the matcher above: a second copy will
// eventually disagree with it, and the one that existed did -- on gen1 with a
// non-zero program bank, where read6502 forces the bank to zero and the window
// registers still apply.
int debug_current_x16_bank(int pc, uint8_t pbank);

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

// ---- Memory write watchpoints ----------------------------------------------
// "Stop when something writes to this address" -- the counterpart to a
// breakpoint for data rather than code, and the tool for finding what is
// corrupting a variable when you have no idea which code is responsible.
//
// Checked by a linear scan on every CPU store, so the scan is skipped entirely
// while none are set: the cost is only paid during an active debugging session.
// The table is fixed-size because a scan per store has to stay small, and 64 is
// roomy enough to watch a whole structure a field at a time.
#define MAX_WATCHPOINTS 64

struct watchpoint {
	uint16_t addr;
	uint16_t len;       // 1 = a single byte, >1 = a range starting at addr
	int      x16Bank;   // bank selector, as for a breakpoint (see DEBUG_BANK_ANY)
	bool     active;
	bool     has_value; // when set, only fire if the written value compares true
	uint8_t  value;
	int      op;        // BPCMP_* comparison code
};

// Watch `len` bytes from `addr` in bank `x16Bank` (see DEBUG_BANK_ANY).
// Identified by (addr, x16Bank), like a breakpoint, so the same address can be
// watched in more than one bank. Returns the new index, or -1 if one already
// covers that pair or the table is full.
int  debug_wp_add(uint16_t addr, uint16_t len, int x16Bank);
bool debug_wp_remove(uint16_t addr, int x16Bank);
void debug_wp_clear_all(void);
int  debug_wp_count(void);
// Read-only access to watchpoint `index`, for listing them in a UI. NULL when
// the index is out of range. The pointer is into the live table, so it is only
// valid until the next add/remove.
const struct watchpoint *debug_wp_at(int index);

// Only fire when the value being written compares true against `value`, so
// "stop when this becomes zero" does not stop on every other write to it.
// Returns false if no watchpoint starts at (addr, x16Bank).
bool debug_wp_set_value(uint16_t addr, int x16Bank, int op, uint8_t value);
// Remove a value filter set by debug_wp_set_value(), so the watchpoint fires on
// any write again.
bool debug_wp_clear_value(uint16_t addr, int x16Bank);

// Temporarily stop a watchpoint firing without forgetting how it was set up.
bool debug_wp_set_active(uint16_t addr, int x16Bank, bool active);

// Does any watchpoint cover `addr`? Read-only; for a UI marking watched bytes.
bool debug_wp_covers(uint16_t addr);

// The CPU is writing `value` to `addr`: should execution stop? Honours the
// value filter. Called from the store path, so callers should test
// debug_wp_count() first and skip this entirely when nothing is being watched.
bool debug_wp_check_write(uint16_t addr, uint8_t value);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _DEBUG_CORE_H_
