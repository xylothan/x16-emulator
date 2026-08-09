// Commander X16 Emulator — disassembly-accuracy core ("code map").
// See code_map.h for the rationale. This file is deliberately dependency-light
// (no SDL / glue.h) so it stays warning-clean under /W3 /WX and is easy to unit
// test in isolation: it leans on just disasm(), dbg_info, and the two core
// symbols declared directly below.
#include "code_map.h"
#include "disasm.h"
#include "dbg_info.h"
#include "cpu/registers.h"   // FLAG_* bits + struct regs layout

#include <string.h>
#include <stdlib.h>

// --- Core symbols, declared directly to avoid pulling in memory.h/glue.h ---
// The global CPU register file (defined in fake6502.c) and the non-intrusive
// memory read used by disasm(). Signatures match memory.h / glue.h exactly.
extern struct regs regs;
extern bool        is_gen2;
uint8_t real_read6502(uint16_t address, uint8_t bank, bool debugOn, int16_t x16Bank);

#define CM_X16_CURRENT_BANK ((int16_t)-1)
#define CM_READ(a, b, xb) real_read6502((uint16_t)(a), (uint8_t)(b), true, (int16_t)(xb))

// Longest instruction we ever have to back up over when self-syncing (the
// 65C816 tops out at 4 bytes).
#define CM_MAX_INSN_LEN 4

// ---------------------------------------------------------------------------
//  Coverage + flag storage, keyed by the bank context backing the address.
// ---------------------------------------------------------------------------
//  Each context is a 64K bit coverage map (1 bit per address = 8 KB) plus a
//  per-address effective-status byte. Contexts are allocated lazily and capped;
//  the least-recently-used one is recycled once the cap is reached.
//
//  The key is deliberately NOT the full live bank state. Only part of the
//  address space is banked, so keying every address by (rambank, rombank) would
//  file the same unbanked low-memory code under a fresh context for every bank
//  combination the program happens to be in. With 32 ROM banks and 256 RAM
//  banks that exhausts the context cache during ROM boot alone (measured: the
//  cap is hit before BASIC is ready), and worse, coverage recorded at $0801
//  under one ROM bank could no longer be found once the program switched banks
//  -- so the disassembler lost the very ground truth it exists to collect.
//
//  Instead each address is keyed by what actually backs it, mirroring the
//  decode in real_read6502():
//    - gen2 with a non-zero program bank: the whole 64K is flat bank RAM and
//      the window registers select nothing, so only the program bank keys it;
//    - otherwise the program bank selects nothing (read6502() forces it to 0 on
//      gen1, even with -c816), and the key is the RAM bank for $A000-$BFFF, the
//      ROM bank for $C000-$FFFF, and nothing at all below $A000.

#define CM_ADDR_SPACE   0x10000
#define CM_COVER_BYTES  (CM_ADDR_SPACE / 8)   // 8 KB: one bit per address
#define CM_MAX_CONTEXTS 32

typedef struct {
	bool     used;
	uint8_t  pbank;
	uint8_t  rambank;
	uint8_t  rombank;
	uint64_t lru;
	uint8_t  cover[CM_COVER_BYTES];
	uint8_t  status[CM_ADDR_SPACE];       // valid only where the cover bit is set
} cm_context_t;

static cm_context_t *g_ctx[CM_MAX_CONTEXTS];
static uint64_t       g_lru_clock;
static cm_context_t  *g_last_ctx;   // fast-path cache for the hot record loop

// Reduce the live bank state to the part that actually selects the memory
// behind `addr`. See the note above.
static void
cm_key_for(uint16_t addr, uint8_t pbank, uint8_t rambank, uint8_t rombank,
           uint8_t *key_pb, uint8_t *key_ram, uint8_t *key_rom)
{
	if (is_gen2 && pbank != 0) {
		*key_pb  = pbank;
		*key_ram = 0;
		*key_rom = 0;
		return;
	}
	*key_pb  = 0;
	*key_ram = (addr >= 0xA000 && addr < 0xC000) ? rambank : 0;
	*key_rom = (addr >= 0xC000) ? rombank : 0;
}

static cm_context_t *
cm_ctx_find(uint8_t pbank, uint8_t rambank, uint8_t rombank)
{
	for (int i = 0; i < CM_MAX_CONTEXTS; i++) {
		cm_context_t *c = g_ctx[i];
		if (c && c->used && c->pbank == pbank && c->rambank == rambank && c->rombank == rombank)
			return c;
	}
	return NULL;
}

static cm_context_t *
cm_ctx_get_or_create(uint8_t pbank, uint8_t rambank, uint8_t rombank)
{
	cm_context_t *c = cm_ctx_find(pbank, rambank, rombank);
	if (c)
		return c;

	// Prefer a free slot; otherwise evict the least-recently-used context.
	int slot = -1;
	for (int i = 0; i < CM_MAX_CONTEXTS; i++) {
		if (!g_ctx[i] || !g_ctx[i]->used) { slot = i; break; }
		if (slot < 0 || g_ctx[i]->lru < g_ctx[slot]->lru) slot = i;
	}
	if (slot < 0)
		slot = 0;

	if (!g_ctx[slot]) {
		g_ctx[slot] = (cm_context_t *)calloc(1, sizeof(cm_context_t));
		if (!g_ctx[slot])
			return NULL;
	} else {
		// Recycling: only the coverage bits must be cleared — status bytes are
		// never read unless their cover bit is set. The evicted context may be
		// the one g_last_ctx points at, so drop that cache too rather than let
		// it name a context that now holds different banks.
		memset(g_ctx[slot]->cover, 0, sizeof(g_ctx[slot]->cover));
		if (g_last_ctx == g_ctx[slot])
			g_last_ctx = NULL;
	}
	g_ctx[slot]->used    = true;
	g_ctx[slot]->pbank   = pbank;
	g_ctx[slot]->rambank = rambank;
	g_ctx[slot]->rombank = rombank;
	g_ctx[slot]->lru     = ++g_lru_clock;
	return g_ctx[slot];
}

static bool
cm_bit(const cm_context_t *c, uint16_t addr)
{
	return c && (c->cover[addr >> 3] & (uint8_t)(1u << (addr & 7))) != 0;
}

// ---------------------------------------------------------------------------
//  Recording
// ---------------------------------------------------------------------------

void
code_map_record(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank, uint8_t status)
{
	uint8_t k_pb, k_ram, k_rom;
	cm_key_for(pc, pbank, rambank, rombank, &k_pb, &k_ram, &k_rom);

	// Fast path: this is called for every executed instruction, so avoid the
	// context scan -- and the LRU bookkeeping -- when the bank context has not
	// changed since last time, which is overwhelmingly the common case.
	cm_context_t *c = g_last_ctx;
	if (!c || !c->used || c->pbank != k_pb || c->rambank != k_ram || c->rombank != k_rom) {
		c = cm_ctx_get_or_create(k_pb, k_ram, k_rom);
		if (!c)
			return;
		c->lru     = ++g_lru_clock;
		g_last_ctx = c;
	}
	c->cover[pc >> 3] |= (uint8_t)(1u << (pc & 7));
	c->status[pc]      = status;
}

// Locate the context holding coverage for `addr` under the given live banks.
static cm_context_t *
cm_ctx_for_addr(uint16_t addr, uint8_t pbank, uint8_t rambank, uint8_t rombank)
{
	uint8_t k_pb, k_ram, k_rom;
	cm_key_for(addr, pbank, rambank, rombank, &k_pb, &k_ram, &k_rom);
	return cm_ctx_find(k_pb, k_ram, k_rom);
}

bool
code_map_is_recorded_start(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank)
{
	return cm_bit(cm_ctx_for_addr(pc, pbank, rambank, rombank), pc);
}

uint8_t
code_map_recorded_status(uint16_t pc, uint8_t pbank, uint8_t rambank, uint8_t rombank, uint8_t fallback)
{
	cm_context_t *c = cm_ctx_for_addr(pc, pbank, rambank, rombank);
	if (cm_bit(c, pc))
		return c->status[pc];
	return fallback;
}

void
code_map_reset(void)
{
	for (int i = 0; i < CM_MAX_CONTEXTS; i++) {
		if (g_ctx[i]) {
			g_ctx[i]->used = false;
			memset(g_ctx[i]->cover, 0, sizeof(g_ctx[i]->cover));
		}
	}
	g_lru_clock = 0;
	g_last_ctx  = NULL;
}

// ---------------------------------------------------------------------------
//  Decoding helpers
// ---------------------------------------------------------------------------

// Which banked window a given address reads from, for the disasm/real_read6502
// x16Bank argument. Low memory ($0000-$9FFF) is not banked (the CPU program
// bank handles gen2), so it follows the current bank.
static int16_t
cm_x16bank_for(uint16_t addr, uint8_t rambank, uint8_t rombank)
{
	if (addr >= 0xC000)
		return (int16_t)rombank;
	if (addr >= 0xA000)
		return (int16_t)rambank;
	return CM_X16_CURRENT_BANK;
}

static int
cm_decode(uint16_t addr, uint8_t bank, int16_t x16bank, uint8_t status,
          char *line, unsigned line_sz, int32_t *eff)
{
	int32_t e  = -1;
	int     sz = disasm(addr, bank, NULL, line, line_sz, x16bank, status, &e);
	if (sz < 1)
		sz = 1;
	if (eff)
		*eff = e;
	return sz;
}

// Predict the implied status of the NEXT instruction from the current opcode.
// Mirrors the intent of debugger.c's DEBUGRenderCode but with correct opcodes
// and real fall-through control. Only used to fill gaps where live coverage is
// missing; recorded status always takes precedence over this estimate.
static uint8_t
cm_propagate(uint16_t addr, uint8_t bank, int16_t x16bank, uint8_t status, uint8_t *e_inout)
{
	if (!regs.is65c816)
		return status;

	uint8_t opcode = (uint8_t)CM_READ(addr, bank, x16bank);
	uint8_t e      = *e_inout;

	switch (opcode) {
		case 0x18: // CLC
			status &= (uint8_t)~FLAG_CARRY;
			break;
		case 0x38: // SEC
			status |= FLAG_CARRY;
			break;
		case 0xC2: // REP #imm — clears the given status bits
			status &= (uint8_t)~CM_READ((addr + 1) & 0xFFFF, bank, x16bank);
			break;
		case 0xE2: // SEP #imm — sets the given status bits
			status |= (uint8_t)CM_READ((addr + 1) & 0xFFFF, bank, x16bank);
			break;
		case 0xFB: { // XCE — exchange carry and emulation flags
			uint8_t carry = (uint8_t)(status & FLAG_CARRY);
			status = (uint8_t)((status & (uint8_t)~FLAG_CARRY) | (e ? FLAG_CARRY : 0));
			e = carry ? 1 : 0;
			break;
		}
		default:
			break;
	}

	// In emulation mode the 65C816 forces 8-bit memory/index widths.
	if (e)
		status |= (uint8_t)(FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH);

	*e_inout = e;
	return status;
}

// Fill one line at `addr`, decoded with an explicit status. Returns the size the
// decode produced, clamped so that it never swallows an address live execution
// proved is an instruction start: a recorded anchor is harder evidence than our
// width estimate, so truncate onto it rather than stepping over the very ground
// truth this file exists to collect.
static int
cm_fill(uint16_t addr, uint8_t bank, uint8_t rambank, uint8_t rombank,
        uint8_t st, bool recorded, code_map_line_t *ln)
{
	int16_t xb = cm_x16bank_for(addr, rambank, rombank);

	int32_t eff;
	int     sz = cm_decode(addr, bank, xb, st, ln->text, (unsigned)sizeof(ln->text), &eff);

	for (int i = 1; i < sz; i++) {
		uint16_t p = (uint16_t)((addr + i) & 0xFFFF);
		if (cm_bit(cm_ctx_for_addr(p, bank, rambank, rombank), p)) {
			sz = i;
			break;
		}
	}

	ln->addr     = addr;
	ln->size     = (uint8_t)sz;
	ln->status   = st;
	ln->eff_addr = eff;
	ln->recorded = recorded;
	memset(ln->bytes, 0, sizeof(ln->bytes));
	// Each byte is read through the window that backs its own address, so an
	// instruction straddling $BFFF/$C000 shows the ROM bank's bytes for the part
	// that lives there rather than the RAM bank's.
	for (int b = 0; b < sz && b < 4; b++) {
		uint16_t p = (uint16_t)((addr + b) & 0xFFFF);
		ln->bytes[b] = (uint8_t)CM_READ(p, bank, cm_x16bank_for(p, rambank, rombank));
	}
	return sz;
}

// Emit one line at `addr` and advance the running status estimate. Recorded
// status wins; otherwise *run_status is used. Returns the instruction size.
static int
cm_emit(uint16_t addr, uint8_t bank, uint8_t rambank, uint8_t rombank,
        uint8_t *run_status, uint8_t *run_e, code_map_line_t *ln)
{
	cm_context_t *c  = cm_ctx_for_addr(addr, bank, rambank, rombank);
	bool     recorded = cm_bit(c, addr);
	uint8_t  st       = recorded ? c->status[addr] : *run_status;

	int sz = cm_fill(addr, bank, rambank, rombank, st, recorded, ln);

	*run_status = cm_propagate(addr, bank, cm_x16bank_for(addr, rambank, rombank), st, run_e);
	return sz;
}

// ---------------------------------------------------------------------------
//  Anchored alignment
// ---------------------------------------------------------------------------

// The decision the backward walk made about one instruction, so a caller can
// render exactly that instruction instead of re-deriving it from a different
// status estimate and disagreeing about where it ends.
typedef struct {
	uint16_t addr;
	uint8_t  status;
	uint8_t  size;
	bool     recorded;
} cm_anchor_t;

static bool
cm_prev_anchor(uint16_t addr, uint8_t bank, uint8_t rambank, uint8_t rombank, cm_anchor_t *out)
{
	cm_context_t *ac            = cm_ctx_for_addr(addr, bank, rambank, rombank);
	uint8_t       anchor_status = cm_bit(ac, addr) ? ac->status[addr] : regs.status;

	cm_anchor_t best_recorded = { 0, 0, 0, false };
	cm_anchor_t best_span     = { 0, 0, 0, false };
	cm_anchor_t best_sync     = { 0, 0, 0, false };
	bool        has_recorded = false, has_span = false, has_sync = false;

	for (int backoff = 1; backoff <= CM_MAX_INSN_LEN; backoff++) {
		uint16_t p  = (uint16_t)((addr - backoff) & 0xFFFF);
		int16_t  xb = cm_x16bank_for(p, rambank, rombank);
		// A candidate may sit in a different window than the anchor (a window
		// boundary can fall mid-instruction), so resolve its context itself.
		cm_context_t *c        = cm_ctx_for_addr(p, bank, rambank, rombank);
		bool          recorded = cm_bit(c, p);
		uint8_t       st       = recorded ? c->status[p] : anchor_status;

		char    scratch[48];
		int32_t eff;
		int     sz = cm_decode(p, bank, xb, st, scratch, (unsigned)sizeof(scratch), &eff);

		// A candidate is only valid if it decodes to land exactly on `addr`.
		if (sz != backoff)
			continue;

		cm_anchor_t cand = { p, st, (uint8_t)sz, recorded };
		if (!has_recorded && recorded) {
			best_recorded = cand;
			has_recorded  = true;
		}
		// The .dbg address space is a flat 24-bit one, so the program bank has
		// to be folded in -- otherwise bank-K code never matches its own spans,
		// and a bank-0 span at the same offset matches when it should not.
		if (!has_span && dbg_info_is_span_start(((dbg_addr_t)bank << 16) | p)) {
			best_span = cand;
			has_span  = true;
		}
		if (!has_sync) {
			best_sync = cand;
			has_sync  = true;
		}
	}

	if (has_recorded) { *out = best_recorded; return true; } // (1) live coverage
	if (has_span)     { *out = best_span;     return true; } // (2) cc65 .dbg spans
	if (has_sync)     { *out = best_sync;     return true; } // (3) self-sync

	// Nothing aligned cleanly. Back up a single byte so callers still make
	// progress, and say the size that implies.
	out->addr     = (uint16_t)((addr - 1) & 0xFFFF);
	out->status   = anchor_status;
	out->size     = 1;
	out->recorded = false;
	return true;
}

uint16_t
code_map_prev_instruction(uint16_t addr, uint8_t bank, uint8_t rambank, uint8_t rombank)
{
	cm_anchor_t an;
	if (!cm_prev_anchor(addr, bank, rambank, rombank, &an))
		return (uint16_t)((addr - 1) & 0xFFFF);
	return an.addr;
}

int
code_map_disasm_window(uint16_t center, uint8_t bank, uint8_t rambank, uint8_t rombank,
                       int lines_before, int lines_after,
                       code_map_line_t *out, int max_out, int *out_center_index)
{
	if (out_center_index)
		*out_center_index = -1;
	if (!out || max_out <= 0)
		return 0;
	if (lines_before < 0)
		lines_before = 0;
	if (lines_after < 1)
		lines_after = 1; // always include the center line itself

	// Phase A — walk backward, keeping the decision made about each instruction
	// rather than just its address. Re-deriving these on a forward pass would
	// use a different (propagated) status estimate, and where the two disagree
	// the walk drifts: it steps over `center`, loses the current-line marker and
	// renders every following line at the wrong offset.
	cm_anchor_t before[256];
	int         nbefore   = lines_before > 255 ? 255 : lines_before;
	int         collected = 0;
	uint16_t    a         = center;
	for (int i = 0; i < nbefore; i++) {
		cm_anchor_t an;
		if (!cm_prev_anchor(a, bank, rambank, rombank, &an))
			break;
		if (an.addr == a)
			break;
		before[collected++] = an;
		a                   = an.addr;
	}

	int n = 0;

	// Phase B — render the preceding instructions lowest-first, exactly as the
	// backward walk resolved them. Each one's size reaches the next start by
	// construction, so the lines cannot overlap.
	for (int i = collected - 1; i >= 0 && n < max_out; i--) {
		cm_fill(before[i].addr, bank, rambank, rombank, before[i].status,
		        before[i].recorded, &out[n]);
		n++;
	}

	// Phase C — forward from `center` itself, so the center line is always
	// present and always starts where the caller asked.
	if (n < max_out && out_center_index)
		*out_center_index = n;

	uint16_t addr       = center;
	uint8_t  run_status = code_map_recorded_status(center, bank, rambank, rombank, regs.status);
	uint8_t  run_e      = regs.e;
	for (int i = 0; i < lines_after && n < max_out; i++) {
		int sz = cm_emit(addr, bank, rambank, rombank, &run_status, &run_e, &out[n]);
		n++;
		addr = (uint16_t)((addr + sz) & 0xFFFF);
	}
	return n;
}

int
code_map_disasm_forward(uint16_t start, uint8_t bank, uint8_t rambank, uint8_t rombank,
                        int count, code_map_line_t *out, int max_out, uint16_t *next_addr)
{
	if (!out || max_out <= 0) {
		if (next_addr)
			*next_addr = start;
		return 0;
	}
	if (count > max_out)
		count = max_out;
	if (count < 0)
		count = 0;

	uint16_t addr       = start;
	uint8_t  run_status = code_map_recorded_status(start, bank, rambank, rombank, regs.status);
	uint8_t  run_e      = regs.e;
	int      n          = 0;

	while (n < count) {
		int sz = cm_emit(addr, bank, rambank, rombank, &run_status, &run_e, &out[n]);
		n++;
		addr = (uint16_t)((addr + sz) & 0xFFFF);
	}
	if (next_addr)
		*next_addr = addr;
	return n;
}
