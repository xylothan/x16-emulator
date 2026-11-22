// Commander X16 Emulator — debugger core: breakpoints. See debug_core.h.
//
// Dependency-light on purpose (no SDL, no glue.h) so the decision logic can be
// unit-tested without standing up a machine: it reaches for only the CPU
// register file, the two bank registers, and a non-intrusive memory read.

#include <stdio.h>
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
uint8_t            debug_bp_pc_map[0x10000 / 8];

static void
bp_map_set(int pc)
{
	const uint16_t p = (uint16_t)pc;
	debug_bp_pc_map[p >> 3] |= (uint8_t)(1u << (p & 7));
}

// Rebuilt from the table rather than cleared bit by bit: two entries can share
// a PC under different banks, so removing one says nothing about the other.
static void
bp_map_rebuild(void)
{
	memset(debug_bp_pc_map, 0, sizeof debug_bp_pc_map);
	for (int i = 0; i < numBreakpoints; i++)
		bp_map_set(breakPoints[i].pc);
}

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

// See debug_core.h. The rule this encodes is the one a caller cannot be trusted
// to re-derive: which field a packed bank byte belongs in. Putting it in the
// program-bank field on a gen1 builds an entry bp_matches() rejects on every
// instruction, because the running program bank is always zero there -- an
// accepted breakpoint that never fires, which is worse than a refused one.
bool
debug_bp_from_ref(long ref, int explicit_bank, long offset, struct breakpoint *out)
{
	if (!out || ref < 0 || ref > 0xFFFFFF)
		return false;
	if (explicit_bank != DEBUG_BANK_ANY && (explicit_bank < 0 || explicit_bank > 0xFF))
		return false;

	int     addr    = (int)(ref & 0xFFFF);
	uint8_t pbank   = 0;
	int     x16bank = explicit_bank;

	uint8_t top = (uint8_t)((ref >> 16) & 0xFF);
	if (top) {
		// Two answers to the same question; refuse rather than silently
		// preferring one of them.
		if (explicit_bank != DEBUG_BANK_ANY)
			return false;
		if (is_gen2)
			pbank = top;
		else
			x16bank = (int)top;
	}

	addr = (int)(((long)addr + offset) & 0xFFFF);

	// A bank named where the window registers select nothing can never be
	// satisfied. Arming it under ANY instead would put the breakpoint in
	// whichever bank happened to be mapped -- not what was asked for.
	if (x16bank != DEBUG_BANK_ANY && normalise_bank(x16bank, addr, pbank) == DEBUG_BANK_ANY)
		return false;

	out->pc      = addr;
	out->bank    = pbank;
	out->x16Bank = x16bank;
	out->owners  = 0;
	out->enabled = false;
	return true;
}

static void cond_forget_all(void);
static void claim_release_for(const struct breakpoint *bp, uint16_t owners);

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
//  Stop filters
// ---------------------------------------------------------------------------

static uint16_t stop_filter[DEBUG_OWNER_COUNT];
static uint16_t stop_filter_union;

void
debug_stop_filter_set(debug_owner_t owner, uint16_t mask)
{
	if (!owner_bit(owner))
		return;
	stop_filter[owner] = mask & DEBUG_STOP_ALL;
	stop_filter_union = 0;
	for (int i = 0; i < DEBUG_OWNER_COUNT; i++)
		stop_filter_union |= stop_filter[i];
}

uint16_t
debug_stop_filter_get(debug_owner_t owner)
{
	return owner_bit(owner) ? stop_filter[owner] : 0;
}

uint16_t
debug_stop_filters(void)
{
	return stop_filter_union;
}

debug_entry_stop_t
debug_stop_on_entry(uint16_t kinds, bool halted, bool break_on_interrupt, bool follow_step)
{
	if (!kinds)
		return DEBUG_ENTRY_NO_STOP;
	if (!halted && (kinds & stop_filter_union))
		return DEBUG_ENTRY_EXCEPTION;
	return (break_on_interrupt || follow_step) ? DEBUG_ENTRY_INTERRUPT : DEBUG_ENTRY_NO_STOP;
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
	bp_map_rebuild();
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
	bp_map_set(bp.pc);
	return DEBUG_ADD_CREATED;
}

bool
debug_bp_remove_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);

	int idx = debug_bp_find(pc, bank, x16Bank);
	if (idx < 0 || !(breakPoints[idx].owners & bit))
		return false;

	claim_release_for(&breakPoints[idx], bit);
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
		claim_release_for(&breakPoints[i], bit);
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
	claim_release_for(&breakPoints[idx], 0xFFFFu);
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
	bp_map_rebuild();
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
	struct bp_dump *dump;   // x16Dump: write memory and continue (NULL = stop)
	void    *log[DEBUG_OWNER_COUNT];   // each owner's logpoint, opaque here
	uint16_t log_owners;               // the owners that have one
};

// A DUMP breakpoint does not stop for its own owner: each firing -- after its
// condition and ignore budget, exactly where a plain breakpoint would stop --
// appends a little-endian uint32 firing index and then each region's bytes to
// a file.  A stop-read-resume loop pays a debugger round trip per hit; this
// pays a memory copy, so a capture of thousands of hits runs at full speed.
#define BP_DUMP_MAX_REGIONS 16
struct bp_dump {
	FILE    *f;
	uint16_t owner;       // the owner's bit
	uint32_t writes;
	uint64_t bytes;
	uint64_t max_bytes;   // 0: no limit
	bool     full;
	int      nreg;
	struct { uint32_t addr, len; int x16Bank; } reg[BP_DUMP_MAX_REGIONS];
};

uint8_t (*debug_bp_dump_read)(uint32_t addr, int x16Bank) = NULL;

static void
dump_release(struct bp_cond *c)
{
	if (c->dump) {
		if (c->dump->f)
			fclose(c->dump->f);
		free(c->dump);
		c->dump = NULL;
	}
}

static void
dump_write(struct bp_dump *d)
{
	if (!d->f)
		return;
	uint64_t record = 4;
	for (int r = 0; r < d->nreg; r++)
		record += d->reg[r].len;
	if (d->max_bytes && d->bytes + record > d->max_bytes) {
		d->full = true;
		return;
	}
	d->bytes += record;
	uint8_t n[4] = {(uint8_t)d->writes, (uint8_t)(d->writes >> 8), (uint8_t)(d->writes >> 16),
	                (uint8_t)(d->writes >> 24)};
	fwrite(n, 1, 4, d->f);
	uint8_t buf[1024];
	for (int r = 0; r < d->nreg; r++) {
		for (uint32_t done = 0; done < d->reg[r].len;) {
			uint32_t k = d->reg[r].len - done;
			if (k > sizeof buf)
				k = sizeof buf;
			for (uint32_t i = 0; i < k; i++)
				buf[i] = debug_bp_dump_read ? debug_bp_dump_read(d->reg[r].addr + done + i, d->reg[r].x16Bank) : 0;
			fwrite(buf, 1, k, d->f);
			done += k;
		}
	}
	d->writes++;
}

// A logpoint prints instead of stopping for its own owner. The message and its
// formatting live outside this file, which links without the emulator.
void (*debug_bp_log_fire)(void *log, debug_owner_t owner, int pc, uint8_t bank,
                          int x16Bank) = NULL;
void (*debug_bp_log_free)(void *log)   = NULL;

static void
log_release(struct bp_cond *c, uint16_t owners)
{
	for (int i = 0; i < DEBUG_OWNER_COUNT; i++) {
		const uint16_t bit = (uint16_t)(1u << i);
		if ((owners & bit) && (c->log_owners & bit)) {
			if (debug_bp_log_free)
				debug_bp_log_free(c->log[i]);
			c->log[i] = NULL;
			c->log_owners &= (uint16_t)~bit;
		}
	}
}

static void
cond_release(struct bp_cond *c)
{
	dump_release(c);
	log_release(c, 0xFFFFu);
}

static struct bp_cond *bpConds     = NULL;
static int             bpCondCount = 0;
static int             bpCondCap   = 0;

static void
cond_forget_all(void)
{
	for (int i = 0; i < bpCondCount; i++) {
		cond_release(&bpConds[i]);
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

// A dump or logpoint goes when the owner that set it lets go, whoever else still
// holds the entry: left on the record, it would keep firing for nobody.
static void
claim_release_for(const struct breakpoint *bp, uint16_t owners)
{
	struct bp_cond *c = cond_find(bp->pc, bp->bank, bp->x16Bank);
	if (!c)
		return;
	if (c->dump && (c->dump->owner & owners))
		dump_release(c);
	log_release(c, owners);
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
			cond_release(c);    // a reused slot may still own a dump file or logpoint
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
	memset(c, 0, sizeof(*c));   // a fresh slot is realloc'd garbage: no release
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
	bool stop = false;

	// The whole table, because a later entry for this PC under another bank
	// selector may hold a dump that has to record this arrival.
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
		if (c->hits > c->ignore) {
			const uint16_t owners = breakPoints[i].owners;
			const uint16_t dumper = (c->dump ? c->dump->owner : 0) & owners;
			const uint16_t logger = c->log_owners & owners;
			if (dumper)
				dump_write(c->dump);
			if (logger && debug_bp_log_fire) {
				for (int o = 0; o < DEBUG_OWNER_COUNT; o++) {
					if (logger & (1u << o))
						debug_bp_log_fire(c->log[o], (debug_owner_t)o, breakPoints[i].pc,
						                  breakPoints[i].bank, breakPoints[i].x16Bank);
				}
			}
			// Any owner here that neither dumps nor logs asked for a stop.
			const uint16_t quiet = dumper | logger;
			if (!quiet || (owners & (uint16_t)~quiet))
				stop = true;
		}
		// still inside the ignore count: counted, but keep running
	}
	return stop;
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
	if (c) {
		cond_release(c);
		c->used = false;
	}
}

bool
debug_bp_set_dump(int pc, uint8_t bank, int x16Bank, debug_owner_t owner, const char *path,
                  int nreg, const uint32_t *addr, const uint32_t *len, const int *regBank)
{
	if (nreg < 0 || nreg > BP_DUMP_MAX_REGIONS || !owner_bit(owner))
		return false;
	// Before the open, which empties the file.
	struct bp_cond *held = cond_find(pc, bank, x16Bank);
	if (held && held->dump && held->dump->owner != owner_bit(owner))
		return false;
	if (held)
		dump_release(held);
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	if (!debug_bp_set_dump_file(pc, bank, x16Bank, owner, f, 0, nreg, addr, len, regBank)) {
		fclose(f);
		return false;
	}
	return true;
}

bool
debug_bp_set_dump_file(int pc, uint8_t bank, int x16Bank, debug_owner_t owner, FILE *f,
                       uint64_t max_bytes, int nreg, const uint32_t *addr, const uint32_t *len,
                       const int *regBank)
{
	const uint16_t bit = owner_bit(owner);
	if (!f || !bit || nreg < 0 || nreg > BP_DUMP_MAX_REGIONS)
		return false;
	struct bp_cond *c = cond_ensure(pc, bank, x16Bank);
	if (!c || (c->dump && c->dump->owner != bit))
		return false;
	struct bp_dump *d = (struct bp_dump *)calloc(1, sizeof(*d));
	if (!d)
		return false;
	dump_release(c);
	d->f         = f;
	d->owner     = bit;
	d->max_bytes = max_bytes;
	d->nreg      = nreg;
	for (int i = 0; i < nreg; i++) {
		d->reg[i].addr    = addr[i];
		d->reg[i].len     = len[i];
		d->reg[i].x16Bank = regBank[i];
	}
	c->dump = d;
	return true;
}

void
debug_bp_clear_dump_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (c && c->dump && c->dump->owner == owner_bit(owner))
		dump_release(c);
}

bool
debug_bp_dump_info(int pc, uint8_t bank, int x16Bank, debug_owner_t *owner, uint32_t *writes,
                   bool *full)
{
	const struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (!c || !c->dump)
		return false;
	if (owner) {
		for (int i = 0; i < DEBUG_OWNER_COUNT; i++) {
			if (c->dump->owner == (uint16_t)(1u << i))
				*owner = (debug_owner_t)i;
		}
	}
	if (writes)
		*writes = c->dump->writes;
	if (full)
		*full = c->dump->full;
	return true;
}

uint32_t
debug_bp_dump_writes(int pc, uint8_t bank, int x16Bank)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	return (c && c->dump) ? c->dump->writes : 0;
}

bool
debug_bp_set_log(int pc, uint8_t bank, int x16Bank, debug_owner_t owner, void *log)
{
	const uint16_t bit = owner_bit(owner);
	const int      idx = debug_bp_find(pc, bank, x16Bank);
	if (!bit || !log || idx < 0 || !(breakPoints[idx].owners & bit))
		return false;
	struct bp_cond *c = cond_ensure(pc, bank, x16Bank);
	if (!c)
		return false;
	log_release(c, bit);
	c->log[owner] = log;
	c->log_owners |= bit;
	return true;
}

void
debug_bp_clear_log_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (c)
		log_release(c, owner_bit(owner));
}

void *
debug_bp_log_for(int pc, uint8_t bank, int x16Bank, debug_owner_t owner)
{
	const struct bp_cond *c = cond_find(pc, bank, x16Bank);
	return c && (c->log_owners & owner_bit(owner)) ? c->log[owner] : NULL;
}

uint16_t
debug_bp_quiet_owners(int pc, uint8_t bank, int x16Bank)
{
	const struct bp_cond *c = cond_find(pc, bank, x16Bank);
	if (!c)
		return 0;
	return (uint16_t)((c->dump ? c->dump->owner : 0) | c->log_owners);
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
	bp_map_rebuild();

	for (int i = 0; i < bpCondCount; i++)
		cond_release(&bpConds[i]);
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

int debug_wp_armed_cpu_write = 0;
int debug_wp_armed_cpu_read  = 0;
int debug_wp_armed_vram      = 0;
void (*debug_wp_changed)(void) = NULL;

// Every function that changes the table ends here, so the counts the store
// paths read cannot fall out of step with it.
static void
wp_table_changed(void)
{
	int cpu_write = 0, cpu_read = 0, vram = 0;
	for (int i = 0; i < numWatchpoints; i++) {
		if (!watchPoints[i].active)
			continue;
		if (watchPoints[i].space == DEBUG_WP_SPACE_VRAM) {
			vram++;
		} else {
			if (watchPoints[i].access & DEBUG_WP_ACCESS_WRITE)
				cpu_write++;
			if (watchPoints[i].access & DEBUG_WP_ACCESS_READ)
				cpu_read++;
		}
	}
	debug_wp_armed_cpu_write = cpu_write;
	debug_wp_armed_cpu_read  = cpu_read;
	debug_wp_armed_vram      = vram;
	if (debug_wp_changed)
		debug_wp_changed();
}

// VRAM has no banks, so its entries all carry DEBUG_BANK_ANY.
static int
wp_key_bank(uint8_t space, uint32_t addr, int x16Bank)
{
	return space == DEBUG_WP_SPACE_VRAM ? DEBUG_BANK_ANY
	                                    : normalise_bank(x16Bank, (uint16_t)addr, 0);
}

// Watchpoints are identified by (space, addr, bank selector). Keying on the
// address alone would silently throw away the second of two watches on the same
// address in different banks -- the very case bank awareness exists for -- and
// would treat $0400 in the CPU map and $00400 in VRAM as one watch.
int
debug_wp_find(uint8_t space, uint32_t addr, int x16Bank)
{
	x16Bank = wp_key_bank(space, addr, x16Bank);
	for (int i = 0; i < numWatchpoints; i++) {
		if (watchPoints[i].space == space && watchPoints[i].addr == addr &&
		    watchPoints[i].x16Bank == x16Bank)
			return i;
	}
	return -1;
}

static int
wp_find(uint16_t addr, int x16Bank)
{
	return debug_wp_find(DEBUG_WP_SPACE_CPU, addr, x16Bank);
}

int
debug_wp_count(void)
{
	return numWatchpoints;
}

// Read-only access to one watchpoint, for UIs that list them. Returns NULL for
// an out-of-range index rather than trusting the caller's bound, since the
// table shrinks as watchpoints are removed and a UI's cached count can outlive
// it. The table itself stays private: handing out a mutable pointer would let a
// caller change addr or x16Bank behind wp_find()'s back, and the identity of a
// watchpoint is exactly those two fields.
const struct watchpoint *
debug_wp_at(int index)
{
	if (index < 0 || index >= numWatchpoints)
		return NULL;
	return &watchPoints[index];
}

// Is there a watchpoint with exactly this identity? The counterpart to
// debug_wp_covers(), which asks the different question "would a write to this
// address fire something" and treats DEBUG_BANK_ANY as a wildcard. A UI that
// offers a toggle needs this one: deciding with covers() and then removing with
// an exact key means the control can report a watchpoint it is unable to
// remove, and the button does nothing.
bool
debug_wp_exists(uint16_t addr, int x16Bank)
{
	return wp_find(addr, x16Bank) >= 0;
}

int
debug_wp_add(uint16_t addr, uint16_t len, int x16Bank)
{
	if (wp_find(addr, x16Bank) >= 0)
		return -1;
	if (numWatchpoints >= MAX_WATCHPOINTS)
		return -1;

	if (len == 0)
		len = 1;

	struct watchpoint *w = &watchPoints[numWatchpoints];
	w->addr      = addr;
	w->len       = len;
	w->space     = DEBUG_WP_SPACE_CPU;
	w->hits      = 0;
	w->access    = DEBUG_WP_ACCESS_WRITE;
	w->exclude_fetch = false;
	w->x16Bank   = normalise_bank(x16Bank, addr, 0);
	w->active    = true;
	w->has_value = false;
	w->value     = 0;
	w->op        = BPCMP_EQ;
	w->owners    = 0;
	const int created = numWatchpoints++;
	wp_table_changed();
	return created;
}

static void
wp_erase(int idx)
{
	for (int i = idx; i < numWatchpoints - 1; i++) {
		watchPoints[i] = watchPoints[i + 1];
	}
	numWatchpoints--;
	wp_table_changed();
}

debug_add_result_t
debug_wp_add_for(uint16_t addr, uint16_t len, int x16Bank, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);

	int idx = wp_find(addr, x16Bank);
	if (idx >= 0) {
		// A read-only watch would report this owner armed and never tell it
		// about a write.
		if (!(watchPoints[idx].access & DEBUG_WP_ACCESS_WRITE))
			return DEBUG_ADD_CONFLICT;
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
	w->space     = DEBUG_WP_SPACE_CPU;
	w->hits      = 0;
	w->access    = DEBUG_WP_ACCESS_WRITE;
	w->exclude_fetch = false;
	w->x16Bank   = normalise_bank(x16Bank, addr, 0);
	w->active    = true;
	w->has_value = false;
	w->value     = 0;
	w->op        = BPCMP_EQ;
	w->owners    = bit;
	numWatchpoints++;
	wp_table_changed();
	return DEBUG_ADD_CREATED;
}

debug_add_result_t
debug_wp_add_ex(const debug_wp_spec_t *spec, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);
	const uint32_t len = spec->len ? spec->len : 1;
	const uint8_t  access = (spec->space == DEBUG_WP_SPACE_VRAM || !spec->access)
	                            ? DEBUG_WP_ACCESS_WRITE : spec->access;
	const bool     exclude_fetch = (access & DEBUG_WP_ACCESS_READ) && spec->exclude_fetch;

	int idx = debug_wp_find(spec->space, spec->addr, spec->x16Bank);
	if (idx >= 0) {
		struct watchpoint *w = &watchPoints[idx];
		if (w->len != len || !w->active || w->access != access ||
		    w->exclude_fetch != exclude_fetch || w->has_value != spec->has_value ||
		    (spec->has_value && (w->op != spec->op || w->value != spec->value)))
			return DEBUG_ADD_CONFLICT;
		w->owners |= bit;
		return DEBUG_ADD_EXISTED;
	}
	if (numWatchpoints >= MAX_WATCHPOINTS)
		return DEBUG_ADD_FULL;

	struct watchpoint *w = &watchPoints[numWatchpoints];
	w->addr      = spec->addr;
	w->len       = len;
	w->space     = spec->space;
	w->hits      = 0;
	w->access    = access;
	w->exclude_fetch = exclude_fetch;
	w->x16Bank   = wp_key_bank(spec->space, spec->addr, spec->x16Bank);
	w->active    = true;
	w->has_value = spec->has_value;
	w->value     = spec->has_value ? spec->value : 0;
	w->op        = spec->has_value ? spec->op : BPCMP_EQ;
	w->owners    = bit;
	numWatchpoints++;
	wp_table_changed();
	return DEBUG_ADD_CREATED;
}

bool
debug_wp_remove_ex(uint8_t space, uint32_t addr, int x16Bank, debug_owner_t owner)
{
	const uint16_t bit = owner_bit(owner);

	int idx = debug_wp_find(space, addr, x16Bank);
	if (idx < 0 || !(watchPoints[idx].owners & bit))
		return false;

	watchPoints[idx].owners &= (uint16_t)~bit;
	if (watchPoints[idx].owners == 0)
		wp_erase(idx);
	return true;
}

bool
debug_wp_remove_for(uint16_t addr, int x16Bank, debug_owner_t owner)
{
	return debug_wp_remove_ex(DEBUG_WP_SPACE_CPU, addr, x16Bank, owner);
}

bool
debug_wp_has_owner_ex(uint8_t space, uint32_t addr, int x16Bank, debug_owner_t owner)
{
	int idx = debug_wp_find(space, addr, x16Bank);
	return idx >= 0 && (watchPoints[idx].owners & owner_bit(owner)) != 0;
}

bool
debug_wp_set_active_at(int index, bool active)
{
	if (index < 0 || index >= numWatchpoints)
		return false;
	watchPoints[index].active = active;
	wp_table_changed();
	return true;
}

bool
debug_wp_set_value_at(int index, int op, uint8_t value)
{
	if (index < 0 || index >= numWatchpoints)
		return false;
	watchPoints[index].has_value = true;
	watchPoints[index].op        = op;
	watchPoints[index].value     = value;
	return true;
}

bool
debug_wp_clear_value_at(int index)
{
	if (index < 0 || index >= numWatchpoints)
		return false;
	watchPoints[index].has_value = false;
	return true;
}

bool
debug_wp_delete_at(int index)
{
	if (index < 0 || index >= numWatchpoints)
		return false;
	wp_erase(index);
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
	return debug_wp_has_owner_ex(DEBUG_WP_SPACE_CPU, addr, x16Bank, owner);
}

void
debug_wp_clear_all(void)
{
	numWatchpoints = 0;
	wp_table_changed();
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
	wp_table_changed();
	return true;
}

// Drop the value filter, so the watchpoint fires on any write again. The
// counterpart to debug_wp_set_value(); without it a UI that offers a "only when
// the written value is..." toggle can turn the filter on but never off, since
// has_value is not otherwise reachable from outside.
bool
debug_wp_clear_value(uint16_t addr, int x16Bank)
{
	int idx = wp_find(addr, x16Bank);
	if (idx < 0)
		return false;
	watchPoints[idx].has_value = false;
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
	return w->active && w->space == DEBUG_WP_SPACE_CPU &&
	       addr >= w->addr && (uint32_t)addr < w->addr + w->len;
}

static bool
wp_covers_vram(const struct watchpoint *w, uint32_t addr)
{
	return w->active && w->space == DEBUG_WP_SPACE_VRAM &&
	       addr >= w->addr && addr < w->addr + w->len;
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

static bool
wp_value_passes(const struct watchpoint *w, uint8_t value)
{
	if (!w->has_value)
		return true;
	switch (w->op) {
		case BPCMP_EQ: return value == w->value;
		case BPCMP_NE: return value != w->value;
		case BPCMP_LT: return value <  w->value;
		case BPCMP_LE: return value <= w->value;
		case BPCMP_GT: return value >  w->value;
		case BPCMP_GE: return value >= w->value;
		default:       return true;
	}
}

static bool     wpLastHitValid = false;
static uint8_t  wpLastHitSpace = DEBUG_WP_SPACE_CPU;
static uint32_t wpLastHitAddr  = 0;
static uint8_t  wpLastHitValue = 0;
static int      wpLastHitPort  = -1;
static uint8_t  wpLastHitAccess = DEBUG_WP_ACCESS_WRITE;
static bool     wpLastHitFetch  = false;

static void
wp_record_hit(uint8_t space, uint32_t addr, uint8_t value, int port, uint8_t access, bool fetch)
{
	wpLastHitValid  = true;
	wpLastHitSpace  = space;
	wpLastHitAddr   = addr;
	wpLastHitValue  = value;
	wpLastHitPort   = port;
	wpLastHitAccess = access;
	wpLastHitFetch  = fetch;
}

// The whole table is walked rather than stopping at the first match, so that
// overlapping watches each count the write.
bool
debug_wp_check_write(uint16_t addr, uint8_t value)
{
	bool fired = false;
	for (int i = 0; i < numWatchpoints; i++) {
		struct watchpoint *w = &watchPoints[i];
		if ((w->access & DEBUG_WP_ACCESS_WRITE) && wp_covers_addr(w, addr) && wp_bank_ok(w) &&
		    wp_value_passes(w, value)) {
			w->hits++;
			fired = true;
		}
	}
	if (fired)
		wp_record_hit(DEBUG_WP_SPACE_CPU, addr, value, -1, DEBUG_WP_ACCESS_WRITE, false);
	return fired;
}

bool
debug_wp_check_read(uint16_t addr, uint8_t value, bool fetch)
{
	bool fired = false;
	for (int i = 0; i < numWatchpoints; i++) {
		struct watchpoint *w = &watchPoints[i];
		if ((w->access & DEBUG_WP_ACCESS_READ) && !(fetch && w->exclude_fetch) &&
		    wp_covers_addr(w, addr) && wp_bank_ok(w) && wp_value_passes(w, value)) {
			w->hits++;
			fired = true;
		}
	}
	if (fired)
		wp_record_hit(DEBUG_WP_SPACE_CPU, addr, value, -1, DEBUG_WP_ACCESS_READ, fetch);
	return fired;
}

bool
debug_wp_check_vram(uint32_t addr, uint8_t value, int port)
{
	bool fired = false;
	for (int i = 0; i < numWatchpoints; i++) {
		struct watchpoint *w = &watchPoints[i];
		if (wp_covers_vram(w, addr) && wp_value_passes(w, value)) {
			w->hits++;
			fired = true;
		}
	}
	if (fired)
		wp_record_hit(DEBUG_WP_SPACE_VRAM, addr, value, port, DEBUG_WP_ACCESS_WRITE, false);
	return fired;
}

bool
debug_wp_covers_vram(uint32_t addr)
{
	for (int i = 0; i < numWatchpoints; i++) {
		if (wp_covers_vram(&watchPoints[i], addr))
			return true;
	}
	return false;
}

bool
debug_wp_last_hit(uint16_t *addr, uint8_t *value)
{
	if (!wpLastHitValid || wpLastHitSpace != DEBUG_WP_SPACE_CPU)
		return false;
	*addr  = (uint16_t)wpLastHitAddr;
	*value = wpLastHitValue;
	return true;
}

bool
debug_wp_last_access(uint8_t *space, uint32_t *addr, uint8_t *value, int *port)
{
	if (!wpLastHitValid)
		return false;
	*space = wpLastHitSpace;
	*addr  = wpLastHitAddr;
	*value = wpLastHitValue;
	*port  = wpLastHitPort;
	return true;
}

bool
debug_wp_last_access_kind(uint8_t *access, bool *fetch)
{
	if (!wpLastHitValid)
		return false;
	*access = wpLastHitAccess;
	*fetch  = wpLastHitFetch;
	return true;
}

bool
debug_wp_fires_on_ex(uint8_t space, uint8_t access, bool fetch, uint32_t wp_addr, int x16Bank,
                     uint32_t addr, uint8_t value)
{
	const int idx = debug_wp_find(space, wp_addr, x16Bank);
	if (idx < 0)
		return false;
	const struct watchpoint *w = &watchPoints[idx];
	if (!(w->access & access) || (fetch && w->exclude_fetch))
		return false;
	if (space == DEBUG_WP_SPACE_VRAM)
		return wp_covers_vram(w, addr) && wp_value_passes(w, value);
	return addr <= 0xFFFF && wp_covers_addr(w, (uint16_t)addr) && wp_bank_ok(w) &&
	       wp_value_passes(w, value);
}

bool
debug_wp_fires_on(uint16_t wp_addr, int x16Bank, uint16_t addr, uint8_t value)
{
	const int idx = wp_find(wp_addr, x16Bank);
	if (idx < 0)
		return false;
	const struct watchpoint *w = &watchPoints[idx];
	return (w->access & DEBUG_WP_ACCESS_WRITE) && wp_covers_addr(w, addr) && wp_bank_ok(w) &&
	       wp_value_passes(w, value);
}