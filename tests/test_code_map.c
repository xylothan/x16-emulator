// Checks for the anchored disassembler / code map (src/code_map.c).
//
// code_map exists because a linear 65C02/65C816 disassembler drifts: start one
// byte off, or assume the wrong operand width, and every following line is
// garbage. These checks pin the three things that keep it aligned -- recorded
// instruction starts, the bank context they are keyed by, and the backward
// anchor search -- plus the recovery path for when the forward walk and the
// backward walk disagree.
//
// code_map.c leans on only disasm(), dbg_info and two core symbols, so the test
// supplies its own memory and register file and links no emulator core.

#include "code_map.h"
#include "cpu/registers.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- Stand-ins for the emulator core --------------------------------------
// The real ones live in fake6502.c and memory.c and would drag in SDL, video
// and the whole machine; code_map and disasm only ever reach for these.

struct regs regs;
bool        is_gen2;

// The memory model has to be real enough to tell banks apart, or the bank
// arguments code_map computes are unobservable and cm_x16bank_for() is checked
// by nothing: a stub returning g_mem[address] gives the same answer for every
// bank, so any window selection -- or none -- passes. This mirrors the decode
// in real_read6502() closely enough to make a wrong window show up as the
// wrong bytes:
//   - gen2 with a non-zero CPU bank: the whole 64K is that bank's flat RAM,
//     and the window registers select nothing;
//   - $C000-$FFFF: banked ROM, x16Bank if given, else the live ROM register;
//   - $A000-$BFFF: banked RAM, x16Bank if given, else the live RAM register;
//   - below that: flat, unbanked, and x16Bank is not consulted at all.
#define TEST_RAM_BANKS  256
#define TEST_ROM_BANKS  32
#define TEST_GEN2_BANKS 4

static uint8_t g_mem[0x10000];                            // flat low memory
static uint8_t g_bram[TEST_RAM_BANKS][0x2000];            // $A000-$BFFF
static uint8_t g_brom[TEST_ROM_BANKS][0x4000];            // $C000-$FFFF
static uint8_t g_gen2[TEST_GEN2_BANKS][0x10000];          // gen2 flat CPU banks
static uint8_t g_ram_bank = 0;
static uint8_t g_rom_bank = 0;

// Set whenever a read below $A000 arrives with an explicit window bank. Low
// memory is not banked, so code_map is supposed to pass "current bank" there;
// anything else is it inventing a window that does not exist.
static int g_low_mem_windowed = 0;

uint8_t
real_read6502(uint16_t address, uint8_t bank, bool debugOn, int16_t x16Bank)
{
	(void)debugOn;

	if (is_gen2 && bank != 0) {
		if (bank < TEST_GEN2_BANKS) {
			return g_gen2[bank][address];
		}
		return (uint8_t)((address >> 8) & 0xFF); // open bus, as in memory.c
	}
	if (address >= 0xC000) {
		int rb = x16Bank >= 0 ? (uint8_t)x16Bank : g_rom_bank;
		if (rb >= TEST_ROM_BANKS) {
			return (uint8_t)((address >> 8) & 0xFF); // open bus, as in memory.c
		}
		return g_brom[rb][address - 0xC000];
	}
	if (address >= 0xA000) {
		int rb = x16Bank >= 0 ? (uint8_t)x16Bank : g_ram_bank;
		return g_bram[rb % TEST_RAM_BANKS][address - 0xA000];
	}
	if (x16Bank >= 0) {
		g_low_mem_windowed++;
	}
	return g_mem[address];
}

uint8_t
memory_get_ram_bank(void)
{
	return g_ram_bank;
}

uint8_t
memory_get_rom_bank(void)
{
	return g_rom_bank;
}

// ---- Test harness ----------------------------------------------------------

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

// The property that makes a disassembly window renderable at all: the emitted
// lines must TILE the range they cover. Every line starts exactly where the
// previous one ended -- an overlap claims the same byte for two instructions, a
// gap describes no instruction at all for the bytes in between, and nothing
// downstream can draw either. Asserted directly, rather than against golden
// text, because this is the actual invariant; golden strings pass for the wrong
// reasons and fail for cosmetic ones.
static void
check_tiles(const code_map_line_t *lines, int n, const char *what)
{
	for (int i = 0; i < n; i++) {
		if (lines[i].size < 1) {
			printf("      line %d at $%04X has size %u\n",
			       i, (unsigned)lines[i].addr, (unsigned)lines[i].size);
			check(false, what);
			return;
		}
		if (i == 0) {
			continue;
		}
		uint16_t end = (uint16_t)((lines[i - 1].addr + lines[i - 1].size) & 0xFFFF);
		if (lines[i].addr != end) {
			printf("      %s: line %d $%04X+%u ends at $%04X, line %d starts at $%04X\n",
			       (uint16_t)(lines[i].addr - end) < 0x8000 ? "gap" : "overlap",
			       i - 1, (unsigned)lines[i - 1].addr, (unsigned)lines[i - 1].size,
			       (unsigned)end, i, (unsigned)lines[i].addr);
			check(false, what);
			return;
		}
	}
	check(n >= 1, what);
}

static void
reset_all(void)
{
	code_map_reset();
	memset(g_mem, 0xEA, sizeof(g_mem)); // NOP everywhere by default
	memset(g_bram, 0xEA, sizeof(g_bram));
	memset(g_brom, 0xEA, sizeof(g_brom));
	memset(g_gen2, 0xEA, sizeof(g_gen2));
	g_ram_bank         = 0;
	g_rom_bank         = 0;
	g_low_mem_windowed = 0;
	memset(&regs, 0, sizeof(regs));
	regs.status = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH; // 8-bit A/X/Y
}

// Write into a specific banked window, so a test can put different code at the
// same address in different banks. Addresses are absolute; which array they
// land in follows the same split real_read6502() uses.
static void
poke_banked(uint16_t addr, uint8_t window_bank, const uint8_t *bytes, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint16_t a = (uint16_t)((addr + i) & 0xFFFF);
		if (a >= 0xC000) {
			g_brom[window_bank % TEST_ROM_BANKS][a - 0xC000] = bytes[i];
		} else if (a >= 0xA000) {
			g_bram[window_bank % TEST_RAM_BANKS][a - 0xA000] = bytes[i];
		} else {
			g_mem[a] = bytes[i];
		}
	}
}

// Write through whichever window the live bank registers currently select, so
// a plain poke and a plain read agree without the test naming a bank.
static void
poke(uint16_t addr, const uint8_t *bytes, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint16_t a = (uint16_t)((addr + i) & 0xFFFF);
		poke_banked(a, a >= 0xC000 ? g_rom_bank : g_ram_bank, &bytes[i], 1);
	}
}

static void
poke_gen2(uint8_t cpu_bank, uint16_t addr, const uint8_t *bytes, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		g_gen2[cpu_bank % TEST_GEN2_BANKS][(addr + i) & 0xFFFF] = bytes[i];
	}
}

int
main(void)
{
	// ── Coverage recording ──────────────────────────────────────────────────
	{
		reset_all();
		check(!code_map_is_recorded_start(0x8000, 0, 0, 0),
		      "reports nothing recorded before anything runs");

		code_map_record(0x8000, 0, 0, 0, 0x34);
		check(code_map_is_recorded_start(0x8000, 0, 0, 0),
		      "remembers a recorded instruction start");
		check(code_map_recorded_status(0x8000, 0, 0, 0, 0xFF) == 0x34,
		      "returns the status captured at that instruction");
		check(code_map_recorded_status(0x8001, 0, 0, 0, 0xFF) == 0xFF,
		      "falls back for an address that never executed");
		check(!code_map_is_recorded_start(0x8001, 0, 0, 0),
		      "does not claim neighbouring addresses");
	}

	// ── Bank context is part of the key ─────────────────────────────────────
	// Coverage recorded in one bank context must not be visible in another:
	// the same address holds completely different code in each, so leaking a
	// recorded start across banks is exactly how the disassembler goes wrong.
	{
		reset_all();
		code_map_record(0xA000, 0, 5, 0, 0x20);

		check(code_map_is_recorded_start(0xA000, 0, 5, 0),
		      "finds the record in its own RAM bank");
		check(!code_map_is_recorded_start(0xA000, 0, 6, 0),
		      "keeps RAM banks apart");

		code_map_record(0xC000, 0, 0, 1, 0x20);
		check(code_map_is_recorded_start(0xC000, 0, 0, 1),
		      "finds the record in its own ROM bank");
		check(!code_map_is_recorded_start(0xC000, 0, 0, 2),
		      "keeps ROM banks apart");

		// The 65C816 program bank belongs in the key on a Gen2 machine, where a
		// non-zero program bank maps its whole 64K as flat RAM: $01:8000 and
		// $00:8000 are then different memory, and sharing coverage between them
		// would hand the disassembler another bank's boundaries and widths.
		reset_all();
		is_gen2 = true;
		code_map_record(0x8000, 0, 0, 0, 0x30);
		check(code_map_is_recorded_start(0x8000, 0, 0, 0),
		      "finds the record in program bank 0");
		check(!code_map_is_recorded_start(0x8000, 1, 0, 0),
		      "keeps Gen2 program banks apart");

		code_map_record(0x8000, 1, 0, 0, 0x00);
		check(code_map_recorded_status(0x8000, 0, 0, 0, 0xFF) == 0x30 &&
		          code_map_recorded_status(0x8000, 1, 0, 0, 0xFF) == 0x00,
		      "keeps a separate status per Gen2 program bank");

		// A non-zero Gen2 program bank maps flat RAM, so the window registers
		// select nothing there and must not fragment the key.
		check(code_map_is_recorded_start(0x8000, 1, 7, 3),
		      "ignores the window banks inside a Gen2 program bank");
		code_map_record(0xC000, 1, 0, 0, 0x30);
		check(code_map_is_recorded_start(0xC000, 1, 0, 9),
		      "ignores the ROM bank inside a Gen2 program bank");

		// On gen1 the program bank selects nothing at all -- read6502() forces
		// it to zero even with -c816 -- so the same address is the same memory
		// and must share coverage rather than being filed twice.
		reset_all();
		is_gen2 = false;
		code_map_record(0x8000, 0, 0, 0, 0x30);
		check(code_map_is_recorded_start(0x8000, 1, 0, 0),
		      "shares coverage across program banks when they select nothing");
	}

	// ── Coverage follows the memory, not the bank registers ─────────────────
	// Only part of the address space is banked. Filing unbanked low memory
	// under the live (RAM, ROM) bank pair would record the same code once per
	// bank combination -- exhausting the context cache during ROM boot -- and
	// then fail to find it again after the program switched banks, throwing
	// away the ground truth the whole file exists to collect.
	{
		reset_all();
		code_map_record(0x0801, 0, 0, 0, 0x30);

		check(code_map_is_recorded_start(0x0801, 0, 7, 3),
		      "finds low-memory coverage under different banks");
		check(code_map_recorded_status(0x0801, 0, 7, 3, 0xFF) == 0x30,
		      "keeps the recorded status for low memory across banks");

		// A whole sweep of bank settings must not create a context each.
		for (int b = 0; b < 64; b++) {
			code_map_record(0x0900, 0, (uint8_t)b, (uint8_t)(b & 31), 0x30);
		}
		check(code_map_is_recorded_start(0x0801, 0, 0, 0),
		      "does not evict low-memory coverage while banks churn");

		// Banked windows still have to be told apart, though.
		reset_all();
		code_map_record(0xA000, 0, 5, 0, 0x30);
		check(code_map_is_recorded_start(0xA000, 0, 5, 9),
		      "ignores the ROM bank for a RAM-window address");
		check(!code_map_is_recorded_start(0xA000, 0, 6, 0),
		      "still separates RAM banks in the RAM window");

		code_map_record(0xC000, 0, 0, 2, 0x30);
		check(code_map_is_recorded_start(0xC000, 0, 9, 2),
		      "ignores the RAM bank for a ROM-window address");
		check(!code_map_is_recorded_start(0xC000, 0, 0, 3),
		      "still separates ROM banks in the ROM window");
	}

	// ── Reset ───────────────────────────────────────────────────────────────
	// Anchors describe the code that *was* there. After a reset it may not be,
	// so stale coverage has to go or the disassembly aligns to a dead program.
	{
		reset_all();
		code_map_record(0x8000, 0, 0, 0, 0x24);
		code_map_record(0x9000, 0, 3, 0, 0x24);
		code_map_reset();
		check(!code_map_is_recorded_start(0x8000, 0, 0, 0) &&
		          !code_map_is_recorded_start(0x9000, 0, 3, 0),
		      "forgets all coverage on reset");
	}

	// ── Context recycling ───────────────────────────────────────────────────
	// Only a bounded number of bank contexts are kept. Once that is exceeded the
	// least-recently-used one is recycled, and it must come back *empty* rather
	// than carrying the previous tenant's coverage.
	{
		reset_all();
		for (int b = 0; b < 40; b++) {
			code_map_record((uint16_t)(0xA000 + b), 0, (uint8_t)b, 0, 0x20);
			// A marker at the same address in every context, so the check below
			// does not depend on guessing which slot gets recycled.
			code_map_record(0xA0FF, 0, (uint8_t)b, 0, 0x20);
		}
		check(code_map_is_recorded_start(0xA027, 0, 39, 0),
		      "keeps the most recent context after recycling");

		// Whichever early context was evicted must not report stale coverage.
		int stale = 0;
		for (int b = 0; b < 40; b++) {
			if (code_map_is_recorded_start((uint16_t)(0xA000 + b), 0, (uint8_t)b, 0)) {
				continue;
			}
			stale++;
		}
		check(stale > 0, "recycles contexts once the cap is reached");

		// Re-recording into a recycled context must see a clean slate: the
		// marker its previous tenant left behind must be gone.
		code_map_record(0xB000, 0, 0, 0, 0x20);
		check(code_map_is_recorded_start(0xB000, 0, 0, 0),
		      "records into a recycled context");
		check(!code_map_is_recorded_start(0xA0FF, 0, 0, 0),
		      "a recycled context does not resurrect old coverage");
	}

	// ── Backward anchoring ──────────────────────────────────────────────────
	// Finding the instruction *before* an address is the hard direction: you
	// cannot decode backwards. Recorded coverage is the ground truth that makes
	// it exact.
	{
		reset_all();
		// $8000: LDA #$00   (A9 00)   2 bytes
		// $8002: NOP        (EA)      1 byte
		const uint8_t code[] = { 0xA9, 0x00, 0xEA };
		poke(0x8000, code, sizeof(code));

		code_map_record(0x8000, 0, 0, 0, regs.status);
		code_map_record(0x8002, 0, 0, 0, regs.status);

		check(code_map_prev_instruction(0x8002, 0, 0, 0) == 0x8000,
		      "walks back onto a recorded instruction start");

		// With no coverage at all it still has to produce something usable, by
		// finding the offset that decodes to land exactly on the anchor.
		code_map_reset();
		check(code_map_prev_instruction(0x8002, 0, 0, 0) == 0x8000,
		      "self-syncs backwards without any coverage");

		// Coverage must actually OUTRANK self-sync, not merely agree with it.
		// Here two candidates both decode onto the anchor at $8003:
		//   $8001 LDA #$EA (A9 EA) -- 2 bytes, and what really executed
		//   $8002 NOP     (EA)     -- 1 byte, the operand byte misread as code
		// Self-sync tries the smallest backoff first and so picks $8002; only
		// the recorded start can get this right.
		reset_all();
		const uint8_t amb[] = { 0xA9, 0xEA, 0xEA };
		poke(0x8001, amb, sizeof(amb));
		check(code_map_prev_instruction(0x8003, 0, 0, 0) == 0x8002,
		      "self-sync alone picks the nearer decoy");
		code_map_record(0x8001, 0, 0, 0, regs.status);
		check(code_map_prev_instruction(0x8003, 0, 0, 0) == 0x8001,
		      "recorded coverage outranks the self-sync guess");
	}

	// ── Forward disassembly ─────────────────────────────────────────────────
	{
		reset_all();
		// $8000: LDA #$12   (A9 12)
		// $8002: STA $1234  (8D 34 12)
		// $8005: NOP        (EA)
		const uint8_t code[] = { 0xA9, 0x12, 0x8D, 0x34, 0x12, 0xEA };
		poke(0x8000, code, sizeof(code));

		code_map_line_t lines[8];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 3, lines, 8, &next);

		check(n == 3, "disassembles the requested number of instructions");
		check(n == 3 && lines[0].addr == 0x8000 && lines[0].size == 2,
		      "sizes an immediate load");
		check(n == 3 && lines[1].addr == 0x8002 && lines[1].size == 3,
		      "sizes an absolute store");
		check(n == 3 && lines[2].addr == 0x8005 && lines[2].size == 1,
		      "sizes an implied instruction");
		check(next == 0x8006, "reports the address just past the last instruction");
		check(n == 3 && lines[0].bytes[0] == 0xA9 && lines[0].bytes[1] == 0x12,
		      "captures the raw instruction bytes");
		// The text must be a disassembly, not a hex dump: pin the mnemonic that
		// belongs to the opcode. (Asserting only that "8d" is absent cannot
		// fail -- the mnemonics are alphabetic and the operand here is $1234.)
		check(n == 3 && strstr(lines[1].text, "sta") != NULL &&
		          strstr(lines[1].text, "$1234") != NULL,
		      "renders the mnemonic and operand for the opcode it decoded");

		// Coverage is reported so a UI can distinguish "known good" from "guess".
		check(n == 3 && !lines[0].recorded, "marks unexecuted lines as unrecorded");
		code_map_record(0x8000, 0, 0, 0, regs.status);
		n = code_map_disasm_forward(0x8000, 0, 0, 0, 1, lines, 8, &next);
		check(n == 1 && lines[0].recorded, "marks executed lines as recorded");
	}

	// ── Windowed disassembly ────────────────────────────────────────────────
	{
		reset_all();
		const uint8_t code[] = { 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA };
		poke(0x8000, code, sizeof(code));
		for (uint16_t a = 0x8000; a < 0x8008; a++) {
			code_map_record(a, 0, 0, 0, regs.status);
		}

		code_map_line_t lines[16];
		int             center_index = -99;
		int             n = code_map_disasm_window(0x8004, 0, 0, 0, 3, 3, lines, 16, &center_index);

		check(n == 6, "returns the requested window size");
		check(center_index >= 0 && center_index < n, "reports where the center landed");
		check(center_index >= 0 && center_index < n && lines[center_index].addr == 0x8004,
		      "centers the window on the requested address");
		check(n >= 1 && lines[0].addr == 0x8001, "starts the window before the center");

		// A window that cannot fit must still be safe and still locate center.
		n = code_map_disasm_window(0x8004, 0, 0, 0, 3, 3, lines, 2, &center_index);
		check(n <= 2, "honours the output capacity");
	}

	// ── Recovering from a disagreement about instruction width ──────────────
	// The backward walk sizes candidates using the status recorded *at* each
	// one; the forward pass carries a propagated estimate. Propagation cannot
	// model PLP -- it restores a status pulled from the stack -- so here the two
	// disagree, and a naive forward walk steps straight over the center address
	// and renders every following line at the wrong offset.
	//
	//   $8000: PLP            (28)        recorded with 8-bit A
	//   $8001: LDA #$A9EA     (A9 EA A9)  3 bytes, because PLP cleared M
	//   $8004: NOP            (EA)        the center, recorded with 16-bit A
	//
	// Read with 8-bit A the LDA is only 2 bytes, so the walk lands on $8003,
	// then $8005 -- jumping the center entirely.
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;

		const uint8_t code[] = { 0x28, 0xA9, 0xEA, 0xA9, 0xEA };
		poke(0x8000, code, sizeof(code));

		const uint8_t st_8bit  = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
		const uint8_t st_16bit = FLAG_INDEX_WIDTH; // PLP cleared M

		regs.status = st_8bit;
		code_map_record(0x8000, 0, 0, 0, st_8bit);
		code_map_record(0x8004, 0, 0, 0, st_16bit);

		code_map_line_t lines[16];
		int             center_index = -99;
		int             n = code_map_disasm_window(0x8004, 0, 0, 0, 2, 2, lines, 16, &center_index);

		check(n > 0 && center_index >= 0, "does not lose the center line to width drift");
		check(center_index >= 0 && center_index < n && lines[center_index].addr == 0x8004,
		      "re-aligns onto the center address after drifting");
		check(n == 4, "returns exactly the requested line count despite the drift");
		check(center_index == 2, "puts exactly the requested number of lines before center");

		// The width the backward walk settled on is the whole point: read with
		// the 8-bit status the LDA is 2 bytes and the walk lands on $8003, then
		// $8005, jumping the center. Only the 16-bit reading reaches $8004.
		check(n == 4 && lines[1].addr == 0x8001 && lines[1].size == 3,
		      "sizes the drifting instruction from the status that reaches center");

		// The real damage from drift is contradictory output: a line claiming
		// bytes that the next line also claims. Nothing downstream can render
		// that sensibly, so the window must never emit it.
		check_tiles(lines, n, "never emits overlapping instructions");

		regs.is65c816 = false;
	}

	// ── The one-byte fallback must not overlap the line after it ────────────
	// When no backoff decodes cleanly onto the anchor, the backward walk gives
	// up and backs off a single byte so the caller still makes progress. That
	// is a guess about where the previous instruction STARTS -- it is not a
	// decode -- so the byte sitting there can still decode wider than the one
	// byte the walk allowed for, and the rendered line then runs straight into
	// the line the caller actually asked for.
	//
	//   $7FFF: A9 ..   LDA #imm -- 2 bytes, so its operand IS $8000
	//   $8000: the requested center
	//
	// Everything else is NOP, so no backoff of 2..4 decodes onto $8000 either;
	// the walk has nothing to align to and falls back.
	{
		reset_all();
		g_mem[0x7FFF] = 0xA9; // LDA #imm: 2 bytes, operand at $8000

		code_map_line_t lines[8];
		int             center_index = -99;
		int             n = code_map_disasm_window(0x8000, 0, 0, 0, 1, 2, lines, 8, &center_index);

		check(n >= 2 && lines[0].addr == 0x7FFF,
		      "backs off a single byte when nothing decodes onto the anchor");
		check(center_index >= 0 && center_index < n && lines[center_index].addr == 0x8000,
		      "keeps the center where the caller asked despite the fallback");
		check_tiles(lines, n, "the one-byte fallback tiles the range instead of overlapping");
	}

	// ── Truncating onto an interior anchor must not leave a hole ────────────
	// Overlapping-but-legal instruction starts are ordinary on this CPU. The
	// `.byte $2C` skip idiom hides a real entry point inside a BIT abs operand,
	// so the BIT and the byte after it are BOTH genuine instruction starts, and
	// live execution can record either.
	//
	//   $8002: 2C 8D 12   BIT $128D -- 3 bytes, lands exactly on $8005
	//   $8003: 8D ..      STA abs   -- the hidden entry point, recorded
	//
	// The inner start is not itself a candidate (STA abs from $8003 runs to
	// $8006, past the anchor), so the backward walk picks the BIT. The fill then
	// truncates the BIT onto the recorded start at $8003 -- correctly, an anchor
	// outranks a decode -- and the two operand bytes end up described by no line
	// at all unless the truncation is followed up.
	{
		reset_all();
		const uint8_t skip_idiom[] = { 0x2C, 0x8D, 0x12 };
		poke(0x8002, skip_idiom, sizeof(skip_idiom));
		code_map_record(0x8003, 0, 0, 0, regs.status);

		code_map_line_t lines[8];
		int             center_index = -99;
		int             n = code_map_disasm_window(0x8005, 0, 0, 0, 1, 2, lines, 8, &center_index);

		check(n >= 2 && lines[0].addr == 0x8002,
		      "walks back onto the wider of two overlapping starts");
		check(center_index >= 0 && center_index < n && lines[center_index].addr == 0x8005,
		      "still centers on the requested address after truncating");
		check_tiles(lines, n, "truncating onto an interior anchor leaves no gap");
	}

	// ── Recorded starts are respected while decoding forward ────────────────
	// A wrong width estimate can size an instruction so that it swallows an
	// address live execution proved was an instruction start. The recorded
	// anchor is the stronger evidence, so the decode has to give way to it.
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;

		// $8000: LDA #$xx -- 2 bytes with 8-bit A, 3 bytes with 16-bit A.
		// $8002 is recorded, so a 3-byte read would step straight over it.
		const uint8_t code[] = { 0xA9, 0xEA, 0xEA, 0xEA };
		poke(0x8000, code, sizeof(code));
		regs.status = FLAG_INDEX_WIDTH; // 16-bit A: LDA # decodes as 3 bytes
		code_map_record(0x8002, 0, 0, 0, FLAG_INDEX_WIDTH);

		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 2, lines, 4, &next);
		check(n == 2 && lines[0].size == 2,
		      "truncates an instruction that would swallow a recorded start");
		check(n == 2 && lines[1].addr == 0x8002,
		      "lands on the recorded start instead of stepping over it");

		regs.is65c816 = false;
	}

	// ── Anchors do not outlive the code they describe ───────────────────────
	// Nothing removes an anchor when memory changes, and on this machine memory
	// under code changes constantly: a second program is loaded over the first,
	// an overlay is swapped in, a decompressor unpacks over its own loader. A
	// stale anchor is worse than none, because live-execution evidence outranks
	// the .dbg spans that would have been right -- it can confidently place a
	// boundary in the middle of a real instruction.
	{
		reset_all();
		// Program 1 runs: $8000 is genuinely an instruction start.
		const uint8_t prog1[] = { 0xA9, 0x00, 0xEA };
		poke(0x8000, prog1, sizeof(prog1));
		code_map_record(0x8000, 0, 0, 0, regs.status);
		code_map_record(0x8002, 0, 0, 0, regs.status);
		check(code_map_is_recorded_start(0x8002, 0, 0, 0),
		      "trusts an anchor while its code is still there");

		// Program 2 is loaded over it. $8002 is now the middle of a 3-byte
		// instruction, not a start.
		const uint8_t prog2[] = { 0xEA, 0x8D, 0x34, 0x12 };
		poke(0x8000, prog2, sizeof(prog2));
		check(!code_map_is_recorded_start(0x8002, 0, 0, 0),
		      "drops an anchor once the code under it changed");
		check(code_map_recorded_status(0x8002, 0, 0, 0, 0xC3) == 0xC3,
		      "does not report a stale anchor's status");

		// $8000 still holds an instruction start, but a different instruction,
		// so its anchor must go too rather than claim the old width.
		check(!code_map_is_recorded_start(0x8000, 0, 0, 0),
		      "drops an anchor whose opcode was replaced");

		// A stale anchor must not truncate a correct instruction either: the
		// STA at $8001 is 3 bytes and spans the dead anchor at $8002.
		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8001, 0, 0, 0, 1, lines, 4, &next);
		check(n == 1 && lines[0].size == 3,
		      "a stale anchor does not cut a real instruction short");

		// Re-running the new code re-establishes trust.
		code_map_record(0x8001, 0, 0, 0, regs.status);
		check(code_map_is_recorded_start(0x8001, 0, 0, 0),
		      "trusts the new code once it has actually run");
	}

	// ── A width guess cannot run on past the next anchor ────────────────────
	// PLP restores a status byte from the stack, so what it does to the M/X
	// widths is unknowable until it runs. The estimate after it is therefore
	// wrong, and the point of anchoring is that it stays wrong only until the
	// next piece of live evidence -- which re-establishes both the instruction
	// boundary and the width. This pins that self-correction.
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;

		//   $8000: PLP        (28)        recorded with 8-bit A
		//   $8001: LDA #$A9EA (A9 EA A9)  really 3 bytes: PLP cleared M
		//   $8004: NOP        (EA)        recorded with 16-bit A -- the anchor
		const uint8_t code[] = { 0x28, 0xA9, 0xEA, 0xA9, 0xEA };
		poke(0x8000, code, sizeof(code));
		const uint8_t st_8bit  = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
		const uint8_t st_16bit = FLAG_INDEX_WIDTH;
		regs.status = st_8bit;
		code_map_record(0x8000, 0, 0, 0, st_8bit);
		code_map_record(0x8004, 0, 0, 0, st_16bit);

		code_map_line_t lines[8];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 4, lines, 8, &next);

		// The guessed line is reported as a guess, so a UI can say so.
		check(n >= 2 && lines[0].recorded, "reports the anchored line as recorded");
		check(n >= 2 && !lines[1].recorded,
		      "reports a line decoded from a guessed width as unrecorded");

		// And the guess does not run on: the anchor at $8004 is reached rather
		// than stepped over, so decoding is right again from there.
		check(n == 4 && lines[3].addr == 0x8004,
		      "re-syncs onto the next anchor after a bad width guess");
		check(n == 4 && lines[3].recorded && lines[3].status == st_16bit,
		      "recovers the real width from the anchor it landed on");

		regs.is65c816 = false;
	}
	// ── Reads go through the window that backs each address ─────────────────
	// Every read code_map makes has to name the bank that actually backs the
	// address: the ROM bank above $C000, the RAM bank in $A000-$BFFF, and the
	// live registers below that. Get it wrong and the disassembler decodes some
	// other bank's bytes, which is indistinguishable from garbage.
	//
	// The live bank registers are deliberately pointed somewhere else here, with
	// different code in them, so selecting the wrong window shows up as the
	// wrong instruction rather than passing by coincidence.
	{
		reset_all();
		g_ram_bank = 0x11; // live registers disagree with the requested banks
		g_rom_bank = 0x17;

		// What the caller asks for.
		const uint8_t ram5[]  = { 0xA9, 0x12 };       // lda #$12
		const uint8_t rom2[]  = { 0x8D, 0x34, 0x12 }; // sta $1234
		// Decoys of a different length sitting in the live banks.
		const uint8_t decoy[] = { 0xEA, 0xEA, 0xEA }; // nop nop nop
		poke_banked(0xA000, 5, ram5, sizeof(ram5));
		poke_banked(0xC000, 2, rom2, sizeof(rom2));
		poke_banked(0xA000, 0x11, decoy, sizeof(decoy));
		poke_banked(0xC000, 0x17, decoy, sizeof(decoy));

		code_map_line_t lines[4];
		uint16_t        next = 0;

		int n = code_map_disasm_forward(0xA000, 0, 5, 2, 1, lines, 4, &next);
		check(n == 1 && lines[0].size == 2 && lines[0].bytes[0] == 0xA9 &&
		          lines[0].bytes[1] == 0x12,
		      "reads a $A000 address through the requested RAM bank");

		n = code_map_disasm_forward(0xC000, 0, 5, 2, 1, lines, 4, &next);
		check(n == 1 && lines[0].size == 3 && lines[0].bytes[0] == 0x8D &&
		          lines[0].bytes[2] == 0x12,
		      "reads a $C000 address through the requested ROM bank");

		// Low memory is not banked at all, so code_map must ask for "current
		// bank" there rather than invent a window for it.
		g_low_mem_windowed = 0;
		n = code_map_disasm_forward(0x0801, 0, 5, 2, 4, lines, 4, &next);
		check(n == 4 && g_low_mem_windowed == 0,
		      "does not put a window bank on an unbanked low-memory read");

		// An instruction may straddle the $BFFF/$C000 boundary, and then each
		// half lives behind a different window. The bytes shown have to follow
		// the address, not the opcode.
		reset_all();
		g_ram_bank = 0x11;
		g_rom_bank = 0x17;
		const uint8_t head[] = { 0x8D };       // sta $xxxx, opcode in RAM bank 5
		const uint8_t tail[] = { 0x34, 0x12 }; // its operand, in ROM bank 2
		poke_banked(0xBFFF, 5, head, sizeof(head));
		poke_banked(0xC000, 2, tail, sizeof(tail));
		const uint8_t straddle_decoy[] = { 0x00, 0x00 };
		poke_banked(0xC000, 0x17, straddle_decoy, sizeof(straddle_decoy));

		n = code_map_disasm_forward(0xBFFF, 0, 5, 2, 1, lines, 4, &next);
		check(n == 1 && lines[0].size == 3 && lines[0].bytes[0] == 0x8D &&
		          lines[0].bytes[1] == 0x34 && lines[0].bytes[2] == 0x12,
		      "reads each byte of a window-straddling instruction from its own bank");
	}

	// ── Anchor staleness is judged in the right bank ────────────────────────
	// An anchor is only believed while the opcode it recorded is still in
	// memory. That comparison is a read, so it too has to name the right
	// window: check it against the wrong bank and a live anchor looks stale
	// (or, worse, a stale one looks live) purely because another bank happens
	// to hold a different byte at the same address.
	{
		reset_all();
		const uint8_t lda[] = { 0xA9, 0x12 };
		const uint8_t nop[] = { 0xEA, 0xEA };
		poke_banked(0xA000, 5, lda, sizeof(lda));
		poke_banked(0xA000, 6, nop, sizeof(nop));

		g_ram_bank = 6; // live register points at the other bank throughout
		code_map_record(0xA000, 0, 5, 0, regs.status);
		check(code_map_is_recorded_start(0xA000, 0, 5, 0),
		      "validates an anchor against the bank it was recorded in");

		// Overwrite bank 5 only. The anchor must die even though the live bank
		// register still points at untouched memory.
		poke_banked(0xA000, 5, nop, sizeof(nop));
		check(!code_map_is_recorded_start(0xA000, 0, 5, 0),
		      "drops the anchor when its own bank's code changes");
	}

	// ── Gen2 program banks are flat memory ──────────────────────────────────
	// On a Gen2 machine a non-zero CPU bank maps its whole 64K as flat RAM, so
	// the window registers select nothing there -- including above $C000, where
	// they otherwise would.
	{
		reset_all();
		is_gen2 = true;
		g_rom_bank = 0x17;
		const uint8_t code[] = { 0xA9, 0x12 };
		const uint8_t decoy[] = { 0x8D, 0x34, 0x12 };
		poke_gen2(1, 0xC000, code, sizeof(code));
		poke_banked(0xC000, 0x17, decoy, sizeof(decoy));
		poke_banked(0xC000, 3, decoy, sizeof(decoy));

		code_map_line_t lines[4];
		uint16_t        next = 0;
		int n = code_map_disasm_forward(0xC000, 1, 0, 3, 1, lines, 4, &next);
		check(n == 1 && lines[0].size == 2 && lines[0].bytes[0] == 0xA9 &&
		          lines[0].bytes[1] == 0x12,
		      "reads a Gen2 program bank as flat RAM, ignoring the ROM window");

		is_gen2 = false;
	}

	// ── Degenerate input ────────────────────────────────────────────────────
	{
		reset_all();
		code_map_line_t lines[4];
		int             center_index = -99;

		check(code_map_disasm_window(0x8000, 0, 0, 0, 2, 2, NULL, 4, &center_index) == 0,
		      "declines a NULL output buffer");
		check(center_index == -1, "clears the center index when it declines");
		check(code_map_disasm_window(0x8000, 0, 0, 0, 2, 2, lines, 0, NULL) == 0,
		      "declines a zero-capacity buffer");

		uint16_t next = 0x1234;
		check(code_map_disasm_forward(0x8000, 0, 0, 0, 4, NULL, 4, &next) == 0,
		      "forward disassembly declines a NULL buffer");
		check(next == 0x8000, "leaves next_addr at the start when it declines");
		check(code_map_disasm_forward(0x8000, 0, 0, 0, -1, lines, 4, &next) == 0,
		      "treats a negative count as nothing to do");

		// Negative "lines before" must be clamped, not used as an array size.
		// Asserting only that a line comes back proves nothing -- phase C
		// always emits the center -- so pin where the center actually lands.
		center_index = -99;
		int n = code_map_disasm_window(0x8000, 0, 0, 0, -5, 1, lines, 4, &center_index);
		check(n == 1 && center_index == 0 && lines[0].addr == 0x8000,
		      "treats a negative lines_before as none at all");

		// An absurd one must be capped too: the backward walk collects into a
		// fixed 256-entry buffer, so an uncapped count writes off the end of
		// it. 255 preceding lines is the ceiling, and the cap is only worth
		// asserting if the count it produces is actually pinned -- a bare
		// "did not overrun" passes for any cap at all.
		static code_map_line_t wide[300];
		center_index = -99;
		n = code_map_disasm_window(0x8000, 0, 0, 0, 100000, 1, wide, 300, &center_index);
		check(n == 256 && center_index == 255,
		      "caps an absurd lines_before at the backward-walk ceiling");
		check_tiles(wide, n, "tiles a window capped by the backward-walk ceiling");
	}

	// ── Wrapping at the top of the address space ────────────────────────────
	// Decoding off the end of memory must wrap to $0000 and read the bytes that
	// are really there. Asserting only that the address stayed below $0100
	// proves nothing: code_map_line_t::addr is a uint16_t, so no arithmetic in
	// the file can produce anything else.
	{
		reset_all();
		// $FFFE: A9 12     LDA #$12   -- 2 bytes, ends exactly on the wrap
		// $0000: 8D 34 12  STA $1234  -- must be decoded from the bottom of RAM
		const uint8_t tail[] = { 0xA9, 0x12 };
		const uint8_t head[] = { 0x8D, 0x34, 0x12 };
		poke(0xFFFE, tail, sizeof(tail));
		poke(0x0000, head, sizeof(head));

		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n = code_map_disasm_forward(0xFFFE, 0, 0, 0, 2, lines, 4, &next);

		check(n == 2 && lines[0].addr == 0xFFFE && lines[0].size == 2,
		      "decodes the instruction at the very top of memory");
		check(n == 2 && lines[1].addr == 0x0000 && lines[1].size == 3 &&
		          lines[1].bytes[0] == 0x8D && lines[1].bytes[2] == 0x12,
		      "wraps to $0000 and decodes the bytes actually there");
		check(next == 0x0003, "reports the wrapped next address");
		check_tiles(lines, n, "tiles across the $FFFF wrap");
	}

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
