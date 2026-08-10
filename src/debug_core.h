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

// Who asked for a breakpoint or watchpoint.
//
// Several independent things want breakpoints, they do not know about each
// other, and more than one of them can want the same address at the same time:
// a `-bp` on the command line, the user at the keyboard, and each of the three
// kinds of breakpoint a DAP client can set. Without a record of who asked, the
// first remove disarms the address for all of them -- and the entry that is
// left behind belongs to nobody, which on a headless `-debugport` run means a
// machine that halts with nothing able to resume it.
//
// The core only ever compares these, so the list can be extended freely.
typedef enum {
	DEBUG_OWNER_CLI = 0,        // -bp / -wp
	DEBUG_OWNER_UI,             // the SDL debugger's F9, and the ImGui panels
	DEBUG_OWNER_DAP_SOURCE,     // DAP setBreakpoints
	DEBUG_OWNER_DAP_FUNCTION,   // DAP setFunctionBreakpoints
	DEBUG_OWNER_DAP_INSTRUCTION,// DAP setInstructionBreakpoints
	DEBUG_OWNER_DAP_CONSOLE,    // bp_add / watch_add typed in the debug console
	DEBUG_OWNER_STEP,           // the debugger's own step-over/step-out target
	DEBUG_OWNER_COUNT
} debug_owner_t;

// A breakpoint address. `bank` is the 65C816 program bank; `x16Bank` is the
// bank selector described above. Breakpoints are identified by all three.
//
// `owners` and `enabled` are maintained by the core; a caller that builds one
// of these to hand to debug_bp_add_for() should set only the three address
// fields and leave the rest zeroed.
struct breakpoint {
	int      pc;
	uint8_t  bank;
	int      x16Bank;
	uint16_t owners;  // bitmask of debug_owner_t; never 0 for a live entry
	bool     enabled; // a disabled breakpoint keeps its place, and its count
};

// The live table. Read-only for everyone outside debug_core.c: everything that
// changes it goes through the functions below, so that ownership cannot be
// bypassed. Prefer debug_bp_count()/debug_bp_at().
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

// What the core will actually record for a selector at this address. Outside a
// banked window there is nothing to select, so it stores DEBUG_BANK_ANY. A
// caller that keeps its own copy of the key must normalise the same way, or its
// copy will not match the entry the core created.
int debug_normalise_bank(int selector, int addr, uint8_t pbank);

// Which RAM/ROM window `pc` sits in given program bank `pbank`, or
// DEBUG_BANK_ANY where a bank means nothing there. Exported so callers that
// produce selectors use the same rule as the matcher above: a second copy will
// eventually disagree with it, and the one that existed did -- on gen1 with a
// non-zero program bank, where read6502 forces the bank to zero and the window
// registers still apply.
int debug_current_x16_bank(int pc, uint8_t pbank);

// ---- Table management ------------------------------------------------------
// Breakpoints are identified by all three of (pc, bank, x16Bank), so the same
// address in two different RAM banks is two different breakpoints.
//
// Every entry records which owners asked for it. Two owners wanting one address
// share a single entry -- there is only ever one place the CPU can stop -- but
// the entry survives until the last of them lets go. This is the whole point of
// the API: an owner can clear everything it asked for without having to know
// what anyone else wanted.

// What debug_bp_add_for() did. "Already there" is deliberately distinct from
// "no room": a caller that conflates them silently mishandles a full table.
typedef enum {
	DEBUG_ADD_CREATED = 0, // a new entry, now owned by the caller
	DEBUG_ADD_EXISTED,     // an entry was already there; the caller now owns it too
	DEBUG_ADD_FULL         // out of memory; nothing was armed
} debug_add_result_t;

// Add `owner`'s reference. Adding one that this owner already holds is
// idempotent. A re-added breakpoint is re-enabled, since asking for a
// breakpoint is a request for it to be armed.
debug_add_result_t debug_bp_add_for(struct breakpoint bp, debug_owner_t owner);

// Drop `owner`'s reference. The entry itself only goes when the last owner
// does. Returns whether this owner held one.
//
// The condition and hit count survive, exactly as they do for any other
// removal: a front end that replaces its whole breakpoint list on every edit
// (which is how the Debug Adapter Protocol works) passes through here
// constantly, and a hit count that reset on each keystroke would be useless.
bool debug_bp_remove_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);

// Drop every reference `owner` holds -- what each DAP "here is the complete
// list" request wants, and what session teardown wants. Returns how many
// entries were disarmed entirely (as opposed to left for another owner).
int debug_bp_clear_owner(debug_owner_t owner);

// Delete the breakpoint outright, whoever asked for it.
//
// This is the human's delete -- F9, or `bp_remove` typed in the console -- and
// it is deliberately not a refcount operation. Someone at the keyboard asking
// for a breakpoint to go and watching it stay armed because a client also
// wanted it is worse than the confusion this ownership model exists to fix.
// Returns whether anything was there.
bool debug_bp_delete(int pc, uint8_t bank, int x16Bank);

// Does `owner` hold a reference here? For a UI marking its own breakpoints.
bool debug_bp_has_owner(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);

// The human's F9: delete whatever is at this address, or create one owned by
// `owner` if there is nothing there.
void debug_bp_toggle_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);

int  debug_bp_find(int pc, uint8_t bank, int x16Bank);
void debug_bp_clear_all(void);

// ---- Enable / disable ------------------------------------------------------
// A disabled breakpoint keeps its entry, its owners, its condition and its hit
// count, and simply does not stop the machine.
//
// Front ends used to implement this by removing the entry and remembering it
// themselves, which is the same mistake as reconstructing ownership from
// outside: the breakpoint vanishes from the table, so every other view of it
// loses its marker and its count.
bool debug_bp_set_enabled(int pc, uint8_t bank, int x16Bank, bool enabled);
bool debug_bp_is_enabled(int pc, uint8_t bank, int x16Bank);

// ---- Read-only view --------------------------------------------------------
// For UIs and the DAP server listing what is set. `debug_bp_at` returns NULL if
// `index` is out of range, and its result is valid only until the next call
// that adds or removes a breakpoint.
int                      debug_bp_count(void);
const struct breakpoint *debug_bp_at(int index);

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

// Whether any *enabled* breakpoint covers (pc, bank), asking nothing and
// changing nothing. This is the one to use for "would the machine stop here?".
// A disabled breakpoint is still in the table -- see debug_bp_at() for a view
// that includes it.
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
// should not silently reset it. That applies to owner-scoped removal too, so a
// client re-sending its breakpoint list does not reset the counts it is about
// to ask about. The two calls that discard a count are debug_bp_forget(), for
// one breakpoint, and debug_bp_clear_all(), which is a delete of everything
// rather than a disable.
//
// A condition belongs to the address, not to whoever asked for it. Two owners
// wanting different conditions at one address is a real conflict with no
// correct answer, and per-owner conditions would mean per-owner evaluation on
// arrival, on the hot path. The last writer wins; a front end that replaces a
// breakpoint list should therefore state the condition it wants every time,
// including clearing it, rather than assuming removal cleared it.
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
	uint16_t owners;    // bitmask of debug_owner_t; maintained by the core
};

// Watch `len` bytes from `addr` in bank `x16Bank` (see DEBUG_BANK_ANY).
// Identified by (addr, x16Bank), like a breakpoint, so the same address can be
// watched in more than one bank.
//
// Ownership works exactly as it does for breakpoints, and for the same reason:
// `-wp` on the command line, the debugger and a DAP client can all want one
// address, and the first remove must not disarm it for the others. A watch that
// already exists keeps its length and value filter; the new owner is asking to
// be told about the address, not to redefine someone else's watch.
debug_add_result_t debug_wp_add_for(uint16_t addr, uint16_t len, int x16Bank, debug_owner_t owner);
bool debug_wp_remove_for(uint16_t addr, int x16Bank, debug_owner_t owner);
int  debug_wp_clear_owner(debug_owner_t owner);
bool debug_wp_delete(uint16_t addr, int x16Bank);
bool debug_wp_has_owner(uint16_t addr, int x16Bank, debug_owner_t owner);
void debug_wp_clear_all(void);
int  debug_wp_count(void);

// Only fire when the value being written compares true against `value`, so
// "stop when this becomes zero" does not stop on every other write to it.
// Returns false if no watchpoint starts at (addr, x16Bank).
bool debug_wp_set_value(uint16_t addr, int x16Bank, int op, uint8_t value);

// Temporarily stop a watchpoint firing without forgetting how it was set up.
bool debug_wp_set_active(uint16_t addr, int x16Bank, bool active);

// Does any watchpoint cover `addr`? Read-only; for a UI marking watched bytes.
bool debug_wp_covers(uint16_t addr);

// Read-only view of the watchpoint table, for UIs and the DAP server that need
// to list what is being watched. Returns NULL if `index` is out of range.
// Valid until the next call that adds or removes a watchpoint.
const struct watchpoint *debug_wp_at(int index);

// The CPU is writing `value` to `addr`: should execution stop? Honours the
// value filter. Called from the store path, so callers should test
// debug_wp_count() first and skip this entirely when nothing is being watched.
bool debug_wp_check_write(uint16_t addr, uint8_t value);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _DEBUG_CORE_H_
