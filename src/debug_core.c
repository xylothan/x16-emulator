// Commander X16 Emulator — debugger core: breakpoints. See debug_core.h.
//
// Dependency-light on purpose (no SDL, no glue.h) so the decision logic can be
// unit-tested without standing up a machine: it reaches for only the CPU
// register file, the two bank registers, and a non-intrusive memory read.

#include "debug_core.h"

#include <stdlib.h>
#include <string.h>

#include "cpu/registers.h"

// Declared directly rather than including memory.h/glue.h, which pull in SDL.
// Signatures match memory.h exactly.
extern struct regs regs;
extern bool        is_gen2;
uint8_t real_read6502(uint16_t address, uint8_t bank, bool debugOn, int16_t x16Bank);
uint8_t memory_get_ram_bank(void);
uint8_t memory_get_rom_bank(void);

#define DC_READ(a) real_read6502((uint16_t)(a), 0, true, -1)

struct breakpoint *breakPoints    = NULL;
int                numBreakpoints = 0;
static int         bpCapacity     = 0;

// Is `addr` inside a window whose contents depend on a bank register?
//
// Nothing below $A000 is banked. Above it, the answer depends on the machine:
// a non-zero program bank on a Gen2 maps its own flat 64K, so the window
// registers select nothing there, while on a gen1 the program bank selects
// nothing at all (read6502 forces it to zero even with -c816) and the window
// applies regardless.
static bool
addr_is_banked(int addr, uint8_t pbank)
{
	if (addr < 0xA000)
		return false;
	return !(is_gen2 && pbank != 0);
}

// Which RAM/ROM window an address sits in, or DEBUG_BANK_ANY where it is not
// banked at all.
int
debug_current_x16_bank(int pc, uint8_t bank)
{
	if (!addr_is_banked(pc, bank))
		return DEBUG_BANK_ANY;
	return pc < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
}

// Does a recorded bank selector apply to `addr` right now? See DEBUG_BANK_ANY.
static bool
bank_selector_matches(int selector, int addr, uint8_t pbank)
{
	if (!addr_is_banked(addr, pbank))
		return true;                     // banks select nothing here
	if (selector == DEBUG_BANK_ANY)
		return true;                     // whichever bank is mapped
	return selector == debug_current_x16_bank(addr, pbank);
}

bool
debug_bank_selector_matches(int selector, int addr, uint8_t pbank)
{
	return bank_selector_matches(selector, addr, pbank);
}

// What to record for an address: outside a banked window there is nothing to
// select, so store ANY rather than a number that would be silently ignored.
static int
normalise_bank(int selector, int addr, uint8_t pbank)
{
	return addr_is_banked(addr, pbank) ? selector : DEBUG_BANK_ANY;
}

int
debug_normalise_bank(int selector, int addr, uint8_t pbank)
{
	return normalise_bank(selector, addr, pbank);
}

static bool
bp_matches(int pc, uint8_t bank, const struct breakpoint *bp)
{
	return bp->enabled && pc == bp->pc && bank == bp->bank
	       && bank_selector_matches(bp->x16Bank, pc, bank);
}

static void cond_forget_all(void);

// ---------------------------------------------------------------------------
//  Ownership
// ---------------------------------------------------------------------------
//  A bitmask per entry rather than a list: there are seven owners, the set
//  operations wanted are exactly bit operations, and the hot path never has to
//  look at it at all.

static uint16_t
owner_bit(debug_owner_t owner)
{
	if (owner < 0 || owner >= DEBUG_OWNER_COUNT)
		return 0;
	return (uint16_t)(1u << (unsigned)owner);
}

// ---------------------------------------------------------------------------
//  Table management
// ---------------------------------------------------------------------------

int
debug_bp_find(int pc, uint8_t bank, int x16Bank)
{
	x16Bank = normalise_bank(x16Bank, pc, bank);
	for (int i = 0; i < numBreakpoints; i++) {
		if (breakPoints[i].pc == pc && breakPoints[i].bank == bank
		    && breakPoints[i].x16Bank == x16Bank)
			return i;
	}
	return -1;
}

int
debug_bp_count(void)
{
	return numBreakpoints;
}

const struct breakpoint *
debug_bp_at(int index)
{
	if (index < 0 || index >= numBreakpoints)
		return NULL;
	return &breakPoints[index];
}

// Drop entry `idx` from the table. The condition record is deliberately left
// alone; see the note on counts in debug_core.h.
static void
bp_erase(int idx)
{
	for (int i = idx; i < numBreakpoints - 1; i++) {
		breakPoints[i] = breakPoints[i + 1];
	}
	numBreakpoints--;
}

debug_add_result_t
debug_bp_add_for(struct breakpoint bp, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);

	bp.x16Bank = normalise_bank(bp.x16Bank, bp.pc, bp.bank);

	int idx = debug_bp_find(bp.pc, bp.bank, bp.x16Bank);
	if (idx >= 0) {
		breakPoints[idx].owners |= bit;
		// Asking for a breakpoint is asking for it to be armed, so a disabled
		// entry comes back. Otherwise a client that set one, disabled it and
		// set it again would be quietly ignored.
		breakPoints[idx].enabled = true;
		return DEBUG_ADD_EXISTED;
	}

	if (numBreakpoints >= bpCapacity) {
		int                newCap = bpCapacity == 0 ? 16 : bpCapacity * 2;
		struct breakpoint *newArr =
		    (struct breakpoint *)realloc(breakPoints, (size_t)newCap * sizeof(struct breakpoint));
		if (!newArr)
			return DEBUG_ADD_FULL;
		breakPoints = newArr;
		bpCapacity  = newCap;
	}

	bp.owners  = bit;
	bp.enabled = true;

	breakPoints[numBreakpoints++] = bp;
	return DEBUG_ADD_CREATED;
}

bool
debug_bp_remove_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);

	int idx = debug_bp_find(pc, bank, x16Bank);
	if (idx < 0 || !(breakPoints[idx].owners & bit))
		return false;

	breakPoints[idx].owners &= (uint16_t)~bit;
	if (breakPoints[idx].owners == 0)
		bp_erase(idx);
	return true;
}

int
debug_bp_clear_owner(debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);
	int            disarmed = 0;

	// Backwards, so erasing an entry cannot shuffle one we have not looked at
	// yet down into a slot we have already passed.
	for (int i = numBreakpoints - 1; i >= 0; i--) {
		if (!(breakPoints[i].owners & bit))
			continue;
		breakPoints[i].owners &= (uint16_t)~bit;
		if (breakPoints[i].owners == 0) {
			bp_erase(i);
			disarmed++;
		}
	}
	return disarmed;
}

bool
debug_bp_delete(int pc, uint8_t bank, int x16Bank)
{
	int idx = debug_bp_find(pc, bank, x16Bank);
	if (idx < 0)
		return false;
	bp_erase(idx);
	return true;
}

bool
debug_bp_has_owner(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	int idx = debug_bp_find(pc, bank, x16Bank);
	return idx >= 0 && (breakPoints[idx].owners & owner_bit(owner)) != 0;
}

void
debug_bp_toggle_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	if (!debug_bp_delete(pc, bank, x16Bank)) {
		struct breakpoint bp = { pc, bank, x16Bank, 0, false };
		debug_bp_add_for(bp, owner);
	}
}

bool
debug_bp_set_enabled(int pc, uint8_t bank, int x16Bank, bool enabled)
{
	int idx = debug_bp_find(pc, bank, x16Bank);
	if (idx < 0)
		return false;
	breakPoints[idx].enabled = enabled;
	return true;
}

bool
debug_bp_is_enabled(int pc, uint8_t bank, int x16Bank)
{
	int idx = debug_bp_find(pc, bank, x16Bank);
	return idx >= 0 && breakPoints[idx].enabled;
}

void
debug_bp_clear_all(void)
{
	// A full clear is a delete, not a disable, so the conditions and counts go
	// with it. Leaving them behind would let a front end that replaces its
	// whole breakpoint set (which is how the Debug Adapter Protocol works)
	// silently inherit an old condition or ignore count on a breakpoint the
	// user thinks they just created.
	numBreakpoints = 0;
	cond_forget_all();
}

bool
debug_bp_is_set(int pc, uint8_t bank)
{
	for (int i = 0; i < numBreakpoints; i++) {
		if (bp_matches(pc, bank, &breakPoints[i]))
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
//  Conditions and hit counts
// ---------------------------------------------------------------------------
//  Held in a table parallel to the breakpoints rather than inside struct
//  breakpoint, so that struct stays the plain address record the rest of the
//  emulator already passes around by value.

struct bp_cond {
	int      pc;
	uint8_t  bank;
	int      x16Bank;
	bool     used;
	bool     has_cond;
	int      operand;
	uint16_t operand_addr;
	int      op;
	uint32_t value;
	uint32_t ignore;
	uint32_t hits;
};

static struct bp_cond *bpConds     = NULL;
static int             bpCondCount = 0;
static int             bpCondCap   = 0;

static void
cond_forget_all(void)
{
	for (int i = 0; i < bpCondCount; i++) {
		bpConds[i].used = false;
	}
}

static struct bp_cond *
cond_find(int pc, uint8_t bank, int x16Bank){
	x16Bank = normalise_bank(x16Bank, pc, bank);
	for (int i = 0; i < bpCondCount; i++) {
		if (bpConds[i].used && bpConds[i].pc == pc && bpConds[i].bank == bank
		    && bpConds[i].x16Bank == x16Bank)
			return &bpConds[i];
	}
	return NULL;
}

static struct bp_cond *
cond_ensure(int pc, uint8_t bank, int x16Bank)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (c)
		return c;

	for (int i = 0; i < bpCondCount; i++) {
		if (!bpConds[i].used) {
			c = &bpConds[i];
			break;
		}
	}
	if (!c) {
		if (bpCondCount >= bpCondCap) {
			int             nc = bpCondCap ? bpCondCap * 2 : 16;
			struct bp_cond *na =
			    (struct bp_cond *)realloc(bpConds, (size_t)nc * sizeof(struct bp_cond));
			if (!na)
				return NULL;
			bpConds   = na;
			bpCondCap = nc;
		}
		c = &bpConds[bpCondCount++];
	}
	memset(c, 0, sizeof(*c));
	c->used    = true;
	c->pc      = pc;
	c->bank    = bank;
	c->x16Bank = normalise_bank(x16Bank, pc, bank);
	return c;
}

static uint32_t
cond_operand_value(const struct bp_cond *c)
{
	switch (c->operand) {
		case BPOPERAND_A:  return regs.c;
		case BPOPERAND_X:  return regs.x;
		case BPOPERAND_Y:  return regs.y;
		case BPOPERAND_SP: return regs.sp;
		case BPOPERAND_P:  return regs.status;
		case BPOPERAND_BYTE: return DC_READ(c->operand_addr);
		case BPOPERAND_WORD:
			return (uint32_t)DC_READ(c->operand_addr)
			       | ((uint32_t)DC_READ((uint16_t)(c->operand_addr + 1)) << 8);
	}
	return 0;
}

static bool
cond_satisfied(const struct bp_cond *c)
{
	if (!c->has_cond)
		return true;

	// Compare at the width the CPU is actually using for that register right
	// now. On a 65C02, and on a 65C816 in emulation mode, A/X/Y are 8-bit; in
	// native mode the M and X status bits decide, exactly as they do for the
	// instructions being debugged.
	//
	// Only the register is masked, never the value the user asked about. Mask
	// both and "A == $1234" quietly becomes "A == $34" and stops somewhere it
	// was never asked to; leaving the value alone means such a condition simply
	// cannot be true while A is 8 bits, which is the honest answer.
	bool     native = regs.is65c816 && !regs.e;
	uint32_t mask;
	switch (c->operand) {
		case BPOPERAND_A:
			mask = (native && !(regs.status & FLAG_MEMORY_WIDTH)) ? 0xFFFFu : 0xFFu;
			break;
		case BPOPERAND_X:
		case BPOPERAND_Y:
			mask = (native && !(regs.status & FLAG_INDEX_WIDTH)) ? 0xFFFFu : 0xFFu;
			break;
		case BPOPERAND_SP:
		case BPOPERAND_WORD:
			mask = 0xFFFFu;
			break;
		default:
			mask = 0xFFu;
			break;
	}

	uint32_t v   = cond_operand_value(c) & mask;
	uint32_t cmp = c->value;

	switch (c->op) {
		case BPCMP_EQ: return v == cmp;
		case BPCMP_NE: return v != cmp;
		case BPCMP_LT: return v < cmp;
		case BPCMP_LE: return v <= cmp;
		case BPCMP_GT: return v > cmp;
		case BPCMP_GE: return v >= cmp;
	}
	return true;
}

bool
debug_bp_on_arrival(int pc, uint8_t bank)
{
	for (int i = 0; i < numBreakpoints; i++) {
		if (!bp_matches(pc, bank, &breakPoints[i]))
			continue;

		struct bp_cond *c =
		    cond_ensure(breakPoints[i].pc, breakPoints[i].bank, breakPoints[i].x16Bank);
		if (!c)
			return true; // out of memory: stopping is the safe way to be wrong

		if (!cond_satisfied(c))
			continue; // condition false, so this breakpoint is not firing

		c->hits++;
		if (c->hits > c->ignore)
			return true;
		// still inside the ignore count: counted, but keep running
	}
	return false;
}

void
debug_bp_set_condition(int pc, uint8_t bank, int x16Bank, int operand, uint16_t operand_addr,
                       int op, uint32_t value)
{
	struct bp_cond *c = cond_ensure(pc, bank, x16Bank);
	if (!c)
		return;
	c->has_cond     = true;
	c->operand      = operand;
	c->operand_addr = operand_addr;
	c->op           = op;
	c->value        = value;
}

void
debug_bp_clear_condition(int pc, uint8_t bank, int x16Bank)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (c)
		c->has_cond = false; // drop the test, keep the count and ignore budget
}

void
debug_bp_forget(int pc, uint8_t bank, int x16Bank)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (c)
		c->used = false;
}

void
debug_bp_set_ignore(int pc, uint8_t bank, int x16Bank, uint32_t ignore)
{
	struct bp_cond *c = cond_ensure(pc, bank, x16Bank);
	if (c)
		c->ignore = ignore;
}

uint32_t
debug_bp_get_hits(int pc, uint8_t bank, int x16Bank)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	return c ? c->hits : 0;
}

void
debug_bp_reset_hits(int pc, uint8_t bank, int x16Bank)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (c)
		c->hits = 0;
}

bool
debug_bp_get_condition(int pc, uint8_t bank, int x16Bank, int *has_cond, int *operand, uint16_t *operand_addr,
                       int *op, uint32_t *value, uint32_t *ignore)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (!c)
		return false;
	if (has_cond)
		*has_cond = c->has_cond ? 1 : 0;
	if (operand)
		*operand = c->operand;
	if (operand_addr)
		*operand_addr = c->operand_addr;
	if (op)
		*op = c->op;
	if (value)
		*value = c->value;
	if (ignore)
		*ignore = c->ignore;
	return true;
}

void
debug_core_free(void)
{
	free(breakPoints);
	breakPoints    = NULL;
	numBreakpoints = 0;
	bpCapacity     = 0;

	free(bpConds);
	bpConds     = NULL;
	bpCondCount = 0;
	bpCondCap   = 0;

	debug_wp_clear_all();
}

// ---------------------------------------------------------------------------
//  Memory write watchpoints
// ---------------------------------------------------------------------------
//  A fixed table scanned on every CPU store. Everything here is shaped by that:
//  the scan is skipped entirely while the table is empty, each entry is a
//  handful of integer compares, and there is no allocation on the path.

static struct watchpoint watchPoints[MAX_WATCHPOINTS];
static int               numWatchpoints = 0;

// Watchpoints are identified by (addr, bank selector), exactly like
// breakpoints. Keying on the address alone would silently throw away the
// second of two watches on the same address in different banks -- the very
// case bank awareness exists for.
static int
wp_find(uint16_t addr, int x16Bank)
{
	x16Bank = normalise_bank(x16Bank, addr, 0);
	for (int i = 0; i < numWatchpoints; i++) {
		if (watchPoints[i].addr == addr && watchPoints[i].x16Bank == x16Bank)
			return i;
	}
	return -1;
}

int
debug_wp_count(void)
{
	return numWatchpoints;
}

const struct watchpoint *
debug_wp_at(int index)
{
	if (index < 0 || index >= numWatchpoints)
		return NULL;
	return &watchPoints[index];
}

static void
wp_erase(int idx)
{
	for (int i = idx; i < numWatchpoints - 1; i++) {
		watchPoints[i] = watchPoints[i + 1];
	}
	numWatchpoints--;
}

debug_add_result_t
debug_wp_add_for(uint16_t addr, uint16_t len, int x16Bank, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);

	int idx = wp_find(addr, x16Bank);
	if (idx >= 0) {
		// Length and value filter are left as the first owner set them: this
		// owner is asking to be told about the address, not to redefine
		// someone else's watch.
		watchPoints[idx].owners |= bit;
		return DEBUG_ADD_EXISTED;
	}
	if (numWatchpoints >= MAX_WATCHPOINTS)
		return DEBUG_ADD_FULL;

	if (len == 0)
		len = 1;

	struct watchpoint *w = &watchPoints[numWatchpoints];
	w->addr      = addr;
	w->len       = len;
	w->x16Bank   = normalise_bank(x16Bank, addr, 0);
	w->active    = true;
	w->has_value = false;
	w->value     = 0;
	w->op        = BPCMP_EQ;
	w->owners    = bit;
	numWatchpoints++;
	return DEBUG_ADD_CREATED;
}

bool
debug_wp_remove_for(uint16_t addr, int x16Bank, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);

	int idx = wp_find(addr, x16Bank);
	if (idx < 0 || !(watchPoints[idx].owners & bit))
		return false;

	watchPoints[idx].owners &= (uint16_t)~bit;
	if (watchPoints[idx].owners == 0)
		wp_erase(idx);
	return true;
}

int
debug_wp_clear_owner(debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);
	int            disarmed = 0;

	for (int i = numWatchpoints - 1; i >= 0; i--) {
		if (!(watchPoints[i].owners & bit))
			continue;
		watchPoints[i].owners &= (uint16_t)~bit;
		if (watchPoints[i].owners == 0) {
			wp_erase(i);
			disarmed++;
		}
	}
	return disarmed;
}

bool
debug_wp_delete(uint16_t addr, int x16Bank)
{
	int idx = wp_find(addr, x16Bank);
	if (idx < 0)
		return false;
	wp_erase(idx);
	return true;
}

bool
debug_wp_has_owner(uint16_t addr, int x16Bank, debug_owner_t owner)
{
	int idx = wp_find(addr, x16Bank);
	return idx >= 0 && (watchPoints[idx].owners & owner_bit(owner)) != 0;
}

void
debug_wp_clear_all(void)
{
	numWatchpoints = 0;
}

bool
debug_wp_set_value(uint16_t addr, int x16Bank, int op, uint8_t value)
{
	int idx = wp_find(addr, x16Bank);
	if (idx < 0)
		return false;
	watchPoints[idx].has_value = true;
	watchPoints[idx].op        = op;
	watchPoints[idx].value     = value;
	return true;
}

bool
debug_wp_set_active(uint16_t addr, int x16Bank, bool active)
{
	int idx = wp_find(addr, x16Bank);
	if (idx < 0)
		return false;
	watchPoints[idx].active = active;
	return true;
}

// Whether this watchpoint applies to the bank mapped right now. The same rule
// as breakpoints: $A100 holds a different variable in each bank, so a watch set
// for one must not fire on the others -- unless it was recorded as
// DEBUG_BANK_ANY, which asks for exactly that.
//
// Judged on the watchpoint's own start address rather than the address being
// written, so a range keeps one meaning throughout. The selector changes
// meaning at $C000 (RAM bank below, ROM bank above), and a range spanning that
// line would otherwise have its two halves tested against unrelated registers.
static bool
wp_bank_ok(const struct watchpoint *w)
{
	return bank_selector_matches(w->x16Bank, w->addr, 0);
}

static bool
wp_covers_addr(const struct watchpoint *w, uint16_t addr)
{
	return w->active && addr >= w->addr && (uint32_t)addr < (uint32_t)w->addr + w->len;
}

bool
debug_wp_covers(uint16_t addr)
{
	for (int i = 0; i < numWatchpoints; i++) {
		if (wp_covers_addr(&watchPoints[i], addr) && wp_bank_ok(&watchPoints[i]))
			return true;
	}
	return false;
}

bool
debug_wp_check_write(uint16_t addr, uint8_t value)
{
	for (int i = 0; i < numWatchpoints; i++) {
		const struct watchpoint *w = &watchPoints[i];
		if (!wp_covers_addr(w, addr) || !wp_bank_ok(w))
			continue;
		if (!w->has_value)
			return true;
		switch (w->op) {
			case BPCMP_EQ: if (value == w->value) return true; break;
			case BPCMP_NE: if (value != w->value) return true; break;
			case BPCMP_LT: if (value <  w->value) return true; break;
			case BPCMP_LE: if (value <= w->value) return true; break;
			case BPCMP_GT: if (value >  w->value) return true; break;
			case BPCMP_GE: if (value >= w->value) return true; break;
			default: return true;
		}
	}
	return false;
}