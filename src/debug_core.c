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
uint8_t real_read6502(uint16_t address, uint8_t bank, bool debugOn, int16_t x16Bank);
uint8_t memory_get_ram_bank(void);
uint8_t memory_get_rom_bank(void);

#define DC_READ(a) real_read6502((uint16_t)(a), 0, true, -1)

struct breakpoint *breakPoints    = NULL;
int                numBreakpoints = 0;
static int         bpCapacity     = 0;

// Which RAM/ROM window an address sits in, or -1 where it is not banked. A
// breakpoint records the window it was set for so one set in RAM bank 5 does
// not fire on the unrelated code that shares its address in bank 6.
static int
current_x16_bank(int pc, uint8_t bank)
{
	if (pc >= 0xA000 && bank == 0) {
		return pc < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	}
	return -1;
}

static bool
bp_matches(int pc, uint8_t bank, const struct breakpoint *bp)
{
	return pc == bp->pc && bank == bp->bank && current_x16_bank(pc, bank) == bp->x16Bank;
}

static void cond_forget_all(void);

// ---------------------------------------------------------------------------
//  Table management
// ---------------------------------------------------------------------------

int
debug_bp_find(int pc, uint8_t bank, int x16Bank)
{
	for (int i = 0; i < numBreakpoints; i++) {
		if (breakPoints[i].pc == pc && breakPoints[i].bank == bank
		    && breakPoints[i].x16Bank == x16Bank)
			return i;
	}
	return -1;
}

int
debug_bp_add(struct breakpoint bp)
{
	if (debug_bp_find(bp.pc, bp.bank, bp.x16Bank) >= 0)
		return -1;

	if (numBreakpoints >= bpCapacity) {
		int                newCap = bpCapacity == 0 ? 16 : bpCapacity * 2;
		struct breakpoint *newArr =
		    (struct breakpoint *)realloc(breakPoints, (size_t)newCap * sizeof(struct breakpoint));
		if (!newArr)
			return -1;
		breakPoints = newArr;
		bpCapacity  = newCap;
	}
	breakPoints[numBreakpoints] = bp;
	return numBreakpoints++;
}

bool
debug_bp_remove(int pc, uint8_t bank, int x16Bank)
{
	int idx = debug_bp_find(pc, bank, x16Bank);
	if (idx < 0)
		return false;
	for (int i = idx; i < numBreakpoints - 1; i++) {
		breakPoints[i] = breakPoints[i + 1];
	}
	numBreakpoints--;
	return true;
}

void
debug_bp_toggle(int pc, uint8_t bank, int x16Bank)
{
	if (!debug_bp_remove(pc, bank, x16Bank)) {
		struct breakpoint bp = { pc, bank, x16Bank };
		debug_bp_add(bp);
	}
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
	c->x16Bank = x16Bank;
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
}