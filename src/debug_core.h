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
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Interrupts and traps a client can ask to stop on. Each interrupt's bit is
// 1 << its vector, the mask cpu_irq_take_entered_kinds() reports. COP has no
// filter. STP is not an interrupt, so it takes the one bit no vector uses.
#define DEBUG_STOP_BRK (1u << 0x6)
#define DEBUG_STOP_NMI (1u << 0xA)
#define DEBUG_STOP_IRQ (1u << 0xE)
#define DEBUG_STOP_STP (1u << 0xF)
#define DEBUG_STOP_ALL (DEBUG_STOP_BRK | DEBUG_STOP_NMI | DEBUG_STOP_IRQ | DEBUG_STOP_STP)

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
	DEBUG_OWNER_MCP,            // an MCP client
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

// Build a breakpoint from an address reference, deciding what a packed bank byte
// meant. `ref` is up to 24 bits: the low 16 are the address, and a non-zero top
// byte is the 65C816 program bank on a Gen2 and the RAM/ROM window bank
// otherwise -- read6502 forces the program bank to zero off Gen2, so a top byte
// there cannot be one. `explicit_bank` carries a window bank the caller was told
// separately (the "bb:aaaa" form), or DEBUG_BANK_ANY; naming one both ways is
// refused rather than resolved. `offset` is a byte displacement, wrapped inside
// the 16-bit address so it cannot carry into the bank.
//
// Returns false when the request could never match -- a bank named for an
// address no window register reaches, or a reference out of range -- so a caller
// can say so instead of arming an entry that silently never fires. `out` is
// left alone on failure, and receives an unowned, disabled entry on success,
// ready for debug_bp_add_for().
bool debug_bp_from_ref(long ref, int explicit_bank, long offset, struct breakpoint *out);

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
	DEBUG_ADD_FULL,        // out of memory; nothing was armed
	DEBUG_ADD_CONFLICT     // held already with different settings; left alone
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

// One bit per 16-bit PC: set when ANY breakpoint entry has that PC, whatever its
// bank, owner, enabled state or condition. A clear bit means no entry can match
// here, so the per-instruction status check can be skipped; a set bit only
// means the full test has to run. Kept in step with every change to the table.
extern uint8_t debug_bp_pc_map[0x10000 / 8];
static inline bool
debug_bp_maybe_at(uint16_t pc)
{
	return (debug_bp_pc_map[pc >> 3] >> (pc & 7)) & 1;
}

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

// Expression conditions, on breakpoints and watches. Opaque here and judged
// through these hooks, since this file links without the expression engine in
// the memory.c test targets; see dbg_cond.h. `access` is NULL for a breakpoint.
// The core owns a condition once it is stored, and releases it with the entry.
// A breakpoint or watch has an expression or a simple test, never both.
typedef struct {
	uint8_t  space;     // DEBUG_WP_SPACE_*
	uint8_t  access;    // DEBUG_WP_ACCESS_READ or _WRITE
	bool     fetch;
	uint32_t addr;
	uint8_t  value;
} debug_cond_access_t;
extern bool (*debug_cond_test)(void *cond, const debug_cond_access_t *access);
extern bool (*debug_cond_same)(const void *a, const void *b);
extern void (*debug_cond_free)(void *cond);
void     debug_bp_set_expr(int pc, uint8_t bank, int x16Bank, void *cond);
void    *debug_bp_expr(int pc, uint8_t bank, int x16Bank);

// DUMP breakpoints (x16Dump over DAP, `dump` over MCP): a firing appends memory
// to a file. The machine runs on unless another owner holds the same entry, since
// that owner asked for a stop. An entry has at most one dump, which belongs to
// the owner that set it and closes when that owner's claim goes. Regions are
// 24-bit addresses with an x16 bank pin each (USE_CURRENT_X16_BANK = -1 for the
// live mapping), or DEBUG_DUMP_VRAM for a region in VERA's 17-bit video space,
// which is not in the CPU map. The reader is installed by the emulator proper;
// debug_core.c itself touches no memory.
#define DEBUG_DUMP_VRAM (-2)
// False, with nothing changed, where another owner's dump is on the entry.
bool     debug_bp_set_dump(int pc, uint8_t bank, int x16Bank, debug_owner_t owner,
                           const char *path, int nreg, const uint32_t *addr, const uint32_t *len,
                           const int *regBank);
// The same with a file already open, which it takes over on success. A record
// that would take the file past `max_bytes` (0: no limit) is not written.
bool     debug_bp_set_dump_file(int pc, uint8_t bank, int x16Bank, debug_owner_t owner, FILE *f,
                                uint64_t max_bytes, int nreg, const uint32_t *addr,
                                const uint32_t *len, const int *regBank);
void     debug_bp_clear_dump_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
// Whether the entry has a dump, and its owner, records written and whether it
// has reached its limit. Any out-parameter may be NULL.
bool     debug_bp_dump_info(int pc, uint8_t bank, int x16Bank, debug_owner_t *owner,
                            uint32_t *writes, bool *full);
uint32_t debug_bp_dump_writes(int pc, uint8_t bank, int x16Bank);
extern uint8_t (*debug_bp_dump_read)(uint32_t addr, int x16Bank);

// Stop filters, one set per owner, replaced whole. The machine stops on the union
// of them. The debugger window's saved "break on every interrupt" setting is a
// separate input to debug_stop_on_entry(), so no filter can change it.
void     debug_stop_filter_set(debug_owner_t owner, uint16_t mask);
uint16_t debug_stop_filter_get(debug_owner_t owner);
uint16_t debug_stop_filters(void);

typedef enum {
	DEBUG_ENTRY_NO_STOP,
	DEBUG_ENTRY_INTERRUPT,   // the saved setting, or a step following an interrupt
	DEBUG_ENTRY_EXCEPTION,   // a client's filter
} debug_entry_stop_t;

// What an interrupt entry means. `kinds` is the mask of entries, `halted` that
// something else has already stopped the machine in the same instruction,
// `break_on_interrupt` the saved setting, `follow_step` a step being followed.
debug_entry_stop_t debug_stop_on_entry(uint16_t kinds, bool halted, bool break_on_interrupt,
                                       bool follow_step);

// Logpoints: a firing calls debug_bp_log_fire instead of stopping for that
// owner. The machine still stops if any other owner on the entry wants a stop.
// Each owner has at most one message per entry, opaque here and released through
// debug_bp_log_free when that owner's claim goes. See logpoint.h.
extern void (*debug_bp_log_fire)(void *log, debug_owner_t owner, int pc, uint8_t bank,
                                 int x16Bank);
extern void (*debug_bp_log_free)(void *log);
// Takes `log` on success. False, with nothing changed, unless `owner` holds the entry.
bool     debug_bp_set_log(int pc, uint8_t bank, int x16Bank, debug_owner_t owner, void *log);
void     debug_bp_clear_log_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
void    *debug_bp_log_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner);
// The owners here that dump or log rather than stop.
uint16_t debug_bp_quiet_owners(int pc, uint8_t bank, int x16Bank);

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

// Which memory a watchpoint is on. The same number means different bytes in
// each: VRAM is reached only through VERA's data ports.
enum { DEBUG_WP_SPACE_CPU = 0, DEBUG_WP_SPACE_VRAM = 1 };
// What a watchpoint fires on, as bits. VRAM watches are write only.
enum { DEBUG_WP_ACCESS_WRITE = 1, DEBUG_WP_ACCESS_READ = 2, DEBUG_WP_ACCESS_BOTH = 3 };

struct watchpoint {
	uint32_t addr;      // 16-bit in CPU space, 17-bit in VRAM
	uint32_t len;       // 1 = a single byte, >1 = a range starting at addr
	uint8_t  space;     // DEBUG_WP_SPACE_*
	uint32_t hits;      // accesses that have fired it since it was created
	uint8_t  access;    // DEBUG_WP_ACCESS_*
	bool     exclude_fetch; // a read watch ignores bytes fetched as instructions
	int      x16Bank;   // bank selector, as for a breakpoint (see DEBUG_BANK_ANY)
	bool     active;
	bool     has_value; // when set, only fire if the written value compares true
	uint8_t  value;
	int      op;        // BPCMP_* comparison code
	uint16_t owners;    // bitmask of debug_owner_t; maintained by the core
	uint32_t ignore;    // accesses to count before it fires, as for a breakpoint
	void    *expr;      // an expression condition, or NULL; see debug_cond_test
	uint32_t fired;     // the check that last fired it
	bool     at_stop;   // it fired on the access the stop reports
};

// Watch `len` bytes from `addr` in bank `x16Bank` (see DEBUG_BANK_ANY).
// Identified by (addr, x16Bank), like a breakpoint, so the same address can be
// watched in more than one bank.
//
// Ownership works exactly as it does for breakpoints, and for the same reason:
// `-wp` on the command line, the debugger and a DAP client can all want one
// address, and the first remove must not disarm it for the others. A watch that
// already exists keeps its length and value filter; the new owner is asking to
// be told about the address, not to redefine someone else's watch. A watch with
// an ignore count is refused instead, since the new owner would miss those hits.
debug_add_result_t debug_wp_add_for(uint16_t addr, uint16_t len, int x16Bank, debug_owner_t owner);
bool debug_wp_remove_for(uint16_t addr, int x16Bank, debug_owner_t owner);
int  debug_wp_clear_owner(debug_owner_t owner);
bool debug_wp_delete(uint16_t addr, int x16Bank);
bool debug_wp_has_owner(uint16_t addr, int x16Bank, debug_owner_t owner);
void debug_wp_clear_all(void);
int  debug_wp_count(void);
// Read-only access to watchpoint `index`, for listing them in a UI. NULL when
// the index is out of range. The pointer is into the live table, so it is only
// valid until the next add/remove.
const struct watchpoint *debug_wp_at(int index);
// Exact-identity test, the counterpart to debug_wp_covers(). Use this to decide
// whether a toggle should add or remove, so that the decision and the removal
// agree on the key.
bool debug_wp_exists(uint16_t addr, int x16Bank);

// A watch in any space, with its filter set in one step. An entry that already
// exists is shared only if it matches this request exactly and is enabled;
// otherwise the answer is DEBUG_ADD_CONFLICT and the entry is unchanged, since
// adopting it would report a watch armed while it does something else.
typedef struct {
	uint8_t  space;
	uint32_t addr;
	uint32_t len;
	int      x16Bank;
	bool     has_value;
	int      op;
	uint8_t  value;
	uint8_t  access;        // DEBUG_WP_ACCESS_*; 0 means write
	bool     exclude_fetch; // only meaningful with read access
	uint32_t ignore;        // accesses to count before it fires
	void    *expr;          // taken only when the answer is DEBUG_ADD_CREATED
} debug_wp_spec_t;
debug_add_result_t debug_wp_add_ex(const debug_wp_spec_t *spec, debug_owner_t owner);

// The index of the watch with this identity, or -1.
int  debug_wp_find(uint8_t space, uint32_t addr, int x16Bank);
bool debug_wp_remove_ex(uint8_t space, uint32_t addr, int x16Bank, debug_owner_t owner);
bool debug_wp_has_owner_ex(uint8_t space, uint32_t addr, int x16Bank, debug_owner_t owner);

// Edits by table index, for a UI walking debug_wp_at(). A key would have to
// carry the space as well, and the row already knows which entry it is.
bool debug_wp_set_active_at(int index, bool active);
bool debug_wp_set_value_at(int index, int op, uint8_t value);
bool debug_wp_clear_value_at(int index);
// Replaces any value filter or earlier expression. Takes `cond` on success.
bool debug_wp_set_expr_at(int index, void *cond);
bool debug_wp_delete_at(int index);
// Put back a count, for a client re-sending a watch it already had.
bool debug_wp_set_hits_at(int index, uint32_t hits);

// How many enabled watches each store path has to consider. Read directly by
// the hot paths, so a watch on one memory costs the other nothing.
extern int debug_wp_armed_cpu_write;
extern int debug_wp_armed_cpu_read;
extern int debug_wp_armed_vram;
// Called after any change to the table. NULL unless something needs to know;
// debug_core cannot reach video.c itself, because its tests link without it.
extern void (*debug_wp_changed)(void);

// A guest store landed in VRAM through data port `port`. True if a watch fires,
// and recorded for debug_wp_last_access().
bool debug_wp_check_vram(uint32_t addr, uint8_t value, int port);
// The CPU read a byte; `fetch` says it was part of the instruction stream. True
// if a watch fires, and recorded for debug_wp_last_access().
bool debug_wp_check_read(uint16_t addr, uint8_t value, bool fetch);
// Does an enabled VRAM watch cover this address? For a UI marking watched bytes.
bool debug_wp_covers_vram(uint32_t addr);
// The access that most recently fired a watch, in either space. `port` is -1
// for a CPU store.
bool debug_wp_last_access(uint8_t *space, uint32_t *addr, uint8_t *value, int *port);
// Whether that access was a read or a write, and an instruction fetch.
bool debug_wp_last_access_kind(uint8_t *access, bool *fetch);
// debug_wp_fires_on() with the space and the kind of access spelled out.
bool debug_wp_fires_on_ex(uint8_t space, uint8_t access, bool fetch, uint32_t wp_addr, int x16Bank,
                          uint32_t addr, uint8_t value);

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

// Read-only view of the watchpoint table, for UIs and the DAP server that need
// to list what is being watched. Returns NULL if `index` is out of range.
// Valid until the next call that adds or removes a watchpoint.
const struct watchpoint *debug_wp_at(int index);

// The CPU is writing `value` to `addr`: should execution stop? Honours the
// value filter. Called from the store path, so callers should test
// debug_wp_count() first and skip this entirely when nothing is being watched.
bool debug_wp_check_write(uint16_t addr, uint8_t value);

// The CPU write that most recently made debug_wp_check_write() answer yes, so a
// front end can say which watch stopped the machine. False until one has.
bool debug_wp_last_hit(uint16_t *addr, uint8_t *value);
// Would the watchpoint at (wp_addr, x16Bank) fire on this write? Changes nothing.
bool debug_wp_fires_on(uint16_t wp_addr, int x16Bank, uint16_t addr, uint8_t value);
// Fixes which watches the stop names, for an expression cannot be asked again
// once the machine has moved on. Later accesses in the instruction do not count.
void debug_wp_latch_stop(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _DEBUG_CORE_H_
