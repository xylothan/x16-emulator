// Checks for the anchored disassembler / code map (src/code_map.c).

#include "code_map.h"
#include "cpu/registers.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- Stand-ins for the emulator core --------------------------------------

struct regs regs;
bool        is_gen2;

// Mirrors the bank decode in real_read6502(), so a wrong window shows up as the wrong bytes:
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

// Set when a read below $A000 arrives with an explicit window bank (low memory is unbanked).
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

// `recorded` holds exactly when a row is an instruction row and its start is a recorded anchor.
static void
check_line_flags(const code_map_line_t *lines, int n, const char *what)
{
	for (int i = 0; i < n; i++) {
		bool expect = (lines[i].kind == CM_LINE_INSTRUCTION) && lines[i].start_recorded;
		if (lines[i].recorded != expect) {
			printf("      line %d at $%04X: recorded=%d kind=%u start_recorded=%d\n",
			       i, (unsigned)lines[i].addr, (int)lines[i].recorded,
			       (unsigned)lines[i].kind, (int)lines[i].start_recorded);
			check(false, what);
			return;
		}
		if (lines[i].kind == CM_LINE_DATA && lines[i].eff_addr != -1) {
			printf("      line %d at $%04X: data row with eff_addr $%04X\n",
			       i, (unsigned)lines[i].addr, (unsigned)lines[i].eff_addr);
			check(false, what);
			return;
		}
	}
	check(n >= 1, what);
}

// Emitted lines must tile the range: each starts where the previous one ended.
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

// Write into a specific banked window; addresses are absolute, split as in real_read6502().
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

// Write through the window the live bank registers select.
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
	// Coverage recorded in one bank context is not visible in another.
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

		// Gen2: a non-zero program bank is flat RAM, so $01:8000 and $00:8000 differ.
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

		// A non-zero Gen2 program bank is flat RAM; window registers do not fragment it.
		check(code_map_is_recorded_start(0x8000, 1, 7, 3),
		      "ignores the window banks inside a Gen2 program bank");
		code_map_record(0xC000, 1, 0, 0, 0x30);
		check(code_map_is_recorded_start(0xC000, 1, 0, 9),
		      "ignores the ROM bank inside a Gen2 program bank");

		// Gen1: the program bank is forced to zero, so the address shares coverage.
		reset_all();
		is_gen2 = false;
		code_map_record(0x8000, 0, 0, 0, 0x30);
		check(code_map_is_recorded_start(0x8000, 1, 0, 0),
		      "shares coverage across program banks when they select nothing");
	}

	// ── Coverage follows the memory, not the bank registers ─────────────────
	// Unbanked low memory is filed once and found again after a bank switch.
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
	// Reset discards stale coverage.
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
	// The least-recently-used context is recycled and comes back empty.
	{
		reset_all();
		for (int b = 0; b < 40; b++) {
			code_map_record((uint16_t)(0xA000 + b), 0, (uint8_t)b, 0, 0x20);
			// A marker at the same address in every context.
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
	// Recorded coverage makes the previous instruction exact.
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

		// With no coverage, self-sync finds the offset that decodes onto the anchor.
		code_map_reset();
		check(code_map_prev_instruction(0x8002, 0, 0, 0) == 0x8000,
		      "self-syncs backwards without any coverage");

		// Coverage outranks self-sync. Two candidates decode onto the anchor at $8003:
		//   $8001 LDA #$EA (A9 EA) -- 2 bytes, and what really executed
		//   $8002 NOP     (EA)     -- 1 byte, the operand byte misread as code
		// Self-sync picks $8002; only the recorded start gets it right.
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
		// Pin the mnemonic for the opcode, not a hex dump.
		check(n == 3 && strstr(lines[1].text, "sta") != NULL &&
		          strstr(lines[1].text, "$1234") != NULL,
		      "renders the mnemonic and operand for the opcode it decoded");

		// Coverage is reported to tell known good from guess.
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
		center_index = -99;
		n = code_map_disasm_window(0x8004, 0, 0, 0, 3, 3, lines, 2, &center_index);
		check(n == 2, "honours the output capacity");
		check(center_index == 1 && lines[center_index].addr == 0x8004,
		      "still locates the center in a window that cannot fit");
		check(n == 2 && lines[0].addr == 0x8003,
		      "spends a too-small window on the lines nearest the center");
	}

	// ── The fallback path must still tile the window ────────────────────────
	// $7FFF holds an LDA #imm whose operand byte IS the center address. Every
	// byte before it is a one-byte NOP, so no backoff decodes onto $8000, the
	// fallback fires, and the emitted LDA covers $7FFF-$8000 -- overlapping the
	// center line.
	{
		reset_all();
		g_mem[0x7FFF] = 0xA9; // LDA #$EA: its operand is $8000 itself

		code_map_line_t lines[16];
		int             center_index = -99;
		int             n = code_map_disasm_window(0x8000, 0, 0, 0, 1, 2, lines, 16, &center_index);

		check(n >= 2, "still produces a window when nothing aligns onto the center");
		check_tiles(lines, n, "the fallback path emits no overlapping lines");
		check(center_index >= 0 && center_index < n && lines[center_index].addr == 0x8000,
		      "keeps the center where the caller asked it despite the fallback");
	}

	// ── Truncating onto an anchor must not leave a hole ─────────────────────
	// The `.byte $2C` skip idiom: BIT abs swallows two bytes; entry inside differs.
	//   $8000: 2C EA A9   BIT $A9EA   -- only the $2C is meant; it eats the rest
	//   $8001: EA         NOP         -- where execution actually enters
	//   $8002: A9 EA      LDA #$EA    -- runs into the center
	// The walk lands on the BIT, cut at the recorded start; its two bytes stay in.
	{
		reset_all();
		const uint8_t code[] = { 0x2C, 0xEA, 0xA9, 0xEA, 0xEA };
		poke(0x8000, code, sizeof(code));
		code_map_record(0x8001, 0, 0, 0, regs.status); // execution jumped past the BIT

		code_map_line_t lines[16];
		int             center_index = -99;
		int             n = code_map_disasm_window(0x8003, 0, 0, 0, 1, 2, lines, 16, &center_index);

		check_tiles(lines, n, "a truncated line leaves no gap before the center");

		bool has_entry = false;
		for (int i = 0; i < n; i++) {
			if (lines[i].addr == 0x8001) {
				has_entry = true;
			}
		}
		check(has_entry, "gives the skipped-to entry point its own line");
		check(center_index >= 0 && center_index < n && lines[center_index].addr == 0x8003,
		      "still centers on the requested address after filling the hole");
	}

	// ── Recovering from a disagreement about instruction width ──────────────
	// The backward walk sizes by recorded status; propagation cannot model PLP.
	//   $8000: PLP            (28)        recorded with 8-bit A
	//   $8001: LDA #$A9EA     (A9 EA A9)  3 bytes, because PLP cleared M
	//   $8004: NOP            (EA)        the center, recorded with 16-bit A
	// With 8-bit A the LDA is 2 bytes: the walk hits $8003, $8005, jumping the center.
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

		// Only the 16-bit reading reaches $8004.
		check(n == 4 && lines[1].addr == 0x8001 && lines[1].size == 3,
		      "sizes the drifting instruction from the status that reaches center");

		// A window must never emit lines that claim the same bytes.
		check_tiles(lines, n, "never emits overlapping instructions");

		regs.is65c816 = false;
	}

	// ── The one-byte fallback must not overlap the line after it ────────────
	// Backing off one byte is a guess; it can decode wider into the requested line.
	//   $7FFF: A9 ..   LDA #imm -- 2 bytes, so its operand IS $8000
	//   $8000: the requested center
	// Everything else is NOP, so no backoff of 2..4 decodes onto $8000 and the walk falls back.
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
	// Overlapping starts: `.byte $2C` hides an entry point inside a BIT abs operand.
	//   $8002: 2C 8D 12   BIT $128D -- 3 bytes, lands exactly on $8005
	//   $8003: 8D ..      STA abs   -- the hidden entry point, recorded
	// The walk picks the BIT, cut at $8003; its operand bytes still belong to a line.
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
		check_line_flags(lines, n, "keeps recorded/kind/start_recorded consistent");

		// $8003 starts its own line; the truncated line is not a verified instruction.
		bool starts_at_8003 = false;
		for (int i = 0; i < n; i++) {
			if (lines[i].addr == 0x8003) {
				starts_at_8003 = true;
			}
		}
		check(starts_at_8003, "begins a line at the recorded start inside the BIT");
		check(n >= 1 && lines[0].size == 1 && !lines[0].recorded && lines[0].eff_addr == -1,
		      "a line cut short is not reported as a whole verified instruction");
		// size, bytes and text agree: a cut row is rendered as the data bytes it owns.
		check(n >= 1 && lines[0].bytes[0] == 0x2C &&
		          strcmp(lines[0].text, ".byte $2c") == 0,
		      "renders a cut-short line as the bytes it owns");
		// The row kind says what the row is; start_recorded says the address was executed.
		check(n >= 1 && lines[0].kind == CM_LINE_DATA,
		      "marks a cut-short line as data rather than an instruction");
		check(center_index >= 0 && center_index < n &&
		          lines[center_index].kind == CM_LINE_INSTRUCTION,
		      "marks a whole line as an instruction");

		// Capacity: phase B needs two lines where the walk resolved one.
		center_index = -99;
		int tight = code_map_disasm_window(0x8005, 0, 0, 0, 1, 2, lines, 2, &center_index);
		check(tight >= 1 && center_index >= 0 && center_index < tight &&
		          lines[center_index].addr == 0x8005,
		      "keeps the center line when phase B cannot fit as well");
		check_tiles(lines, tight, "tiles what fits when capacity runs out");

		// With one slot left, keep the row nearest the center ($8003) and start the window later.
		check(tight == 2 && center_index == 1 && lines[0].addr == 0x8003,
		      "keeps the part of a too-large group closest to the center");
		// The recorded start at $8003, cut short: data with start_recorded set.
		check(tight == 2 && lines[0].kind == CM_LINE_DATA && lines[0].start_recorded &&
		          !lines[0].recorded,
		      "a row cut by a boundary still reports its start as executed");
	}

	// ── Two overlapping starts that are BOTH ground truth ───────────────────
	// Both `.byte $2C` starts are recorded at one rank; the requested one is not clipped.
	//   $8002: 2C 8D 12   BIT $128D   recorded -- the skip
	//   $8003:    8D 12 EA  STA $EA12  recorded -- the hidden entry point
	{
		reset_all();
		const uint8_t skip_idiom[] = { 0x2C, 0x8D, 0x12 };
		poke(0x8002, skip_idiom, sizeof(skip_idiom));
		code_map_record(0x8002, 0, 0, 0, regs.status);
		code_map_record(0x8003, 0, 0, 0, regs.status);

		code_map_line_t lines[8];
		uint16_t        next = 0;

		int n = code_map_disasm_forward(0x8002, 0, 0, 0, 2, lines, 8, &next);
		check(n == 2 && lines[0].addr == 0x8002 && lines[0].size == 3 &&
		          lines[0].recorded,
		      "emits a recorded instruction whole over a recorded start inside it");
		check(n == 2 && strstr(lines[0].text, "bit") != NULL,
		      "keeps the mnemonic of a recorded instruction spanning another start");
		check(n == 2 && lines[1].addr == 0x8005,
		      "continues past a swallowed start on the path the caller asked for");
		check_tiles(lines, n, "tiles the path entered at the outer start");

		// Entering at the hidden address gives the other real path, whole.
		n = code_map_disasm_forward(0x8003, 0, 0, 0, 2, lines, 8, &next);
		check(n == 2 && lines[0].addr == 0x8003 && lines[0].size == 3 &&
		          lines[0].recorded && strstr(lines[0].text, "sta") != NULL,
		      "emits the hidden entry point whole when entered there");
		check_tiles(lines, n, "tiles the path entered at the hidden start");

		// A guessed width still gives way.
		code_map_reset();
		code_map_record(0x8003, 0, 0, 0, regs.status);
		n = code_map_disasm_forward(0x8002, 0, 0, 0, 2, lines, 8, &next);
		check(n == 2 && lines[0].size == 1 && !lines[0].recorded,
		      "an unrecorded decode still gives way to a recorded start inside it");

		// A row forced to data by a window boundary still reports start_recorded.
		reset_all();
		poke(0x8002, skip_idiom, sizeof(skip_idiom));
		code_map_record(0x8002, 0, 0, 0, regs.status);
		code_map_record(0x8003, 0, 0, 0, regs.status);

		int             center_index = -99;
		code_map_line_t win[8];
		int             wn = code_map_disasm_window(0x8005, 0, 0, 0, 1, 1, win, 8, &center_index);
		check(wn >= 2 && win[0].addr == 0x8002 && win[0].size == 3 &&
		          win[0].kind == CM_LINE_INSTRUCTION && win[0].start_recorded,
		      "the window shows the outer instruction whole when both are recorded");
		check_tiles(win, wn, "tiles when both overlapping starts are recorded");

		// Centering on each overlapping start gives that start's own path.
		center_index = -99;
		wn = code_map_disasm_window(0x8002, 0, 0, 0, 0, 2, win, 8, &center_index);
		check(wn >= 1 && center_index == 0 && win[0].addr == 0x8002 &&
		          win[0].size == 3 && win[0].kind == CM_LINE_INSTRUCTION,
		      "centering on the outer start shows the outer instruction whole");
		center_index = -99;
		wn = code_map_disasm_window(0x8003, 0, 0, 0, 0, 2, win, 8, &center_index);
		check(wn >= 1 && center_index == 0 && win[0].addr == 0x8003 &&
		          win[0].size == 3 && win[0].kind == CM_LINE_INSTRUCTION,
		      "centering on the inner start shows the inner instruction whole");
	}

	// ── KNOWN LIMITATION: a stale anchor that kept its opcode byte ──────────
	// Anchors check one opcode byte, so code repeating it keeps the old anchor:
	//   old: $8000  A9 11 22   LDA #$2211  recorded with 16-bit A (3 bytes)
	//   new: $8000  A9 EA      LDA #$EA    8-bit A, and $8002 freshly executed
	// Still $A9, so the stale anchor wins. See docs/code-map-width-propagation.md.
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;

		// The old program runs here with a 16-bit accumulator, so its LDA is
		// three bytes and the anchor records both that opcode and that status.
		const uint8_t old_code[] = { 0xA9, 0x11, 0x22, 0x33 };
		poke(0x8000, old_code, sizeof(old_code));
		code_map_record(0x8000, 0, 0, 0, FLAG_INDEX_WIDTH);

		// An overlay beginning with $A9 passes the check; the 16-bit anchor survives.
		const uint8_t new_code[] = { 0xA9, 0xEA, 0xEA, 0xEA };
		poke(0x8000, new_code, sizeof(new_code));
		code_map_record(0x8002, 0, 0, 0, FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH);
		regs.status = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;

		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 2, lines, 4, &next);

		check(n == 2 && lines[0].size == 3,
		      "KNOWN LIMITATION: a stale same-opcode anchor still swallows a fresh one");
		check(n == 2 && lines[1].addr == 0x8003,
		      "KNOWN LIMITATION: the line after it starts past the fresh anchor");

		regs.is65c816 = false;
	}

	// 65C02: a stale anchor with a surviving $2C covers the fresh start at $8001.
	{
		reset_all(); // regs.is65c816 stays false

		const uint8_t old_bit[] = { 0x2C, 0x8D, 0x12 }; // bit $128d
		poke(0x8000, old_bit, sizeof(old_bit));
		code_map_record(0x8000, 0, 0, 0, regs.status);

		// Overlay: same $2C opcode byte, but the new program really starts an
		// instruction at $8001.
		const uint8_t new_bit[] = { 0x2C, 0xA9, 0xEA };
		poke(0x8000, new_bit, sizeof(new_bit));
		code_map_record(0x8001, 0, 0, 0, regs.status);

		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 2, lines, 4, &next);

		check(n == 2 && lines[0].size == 3 && lines[1].addr == 0x8003,
		      "KNOWN LIMITATION: stale-anchor swallowing is not 65C816-specific");
	}

	// ── Recorded starts are respected while decoding forward ────────────────
	// A recorded start that the width estimate would swallow wins over the decode.
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
	// Anchors go when their memory changes; a stale one outranks the .dbg spans.
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
	// PLP's status is unknowable; the width guess holds until the next anchor.
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

	// ── The same bound applies to RTI ───────────────────────────────────────
	// RTI restores status too; the guess is reported and the next anchor is reached.
	//   $8000: 40         RTI       recorded with 8-bit A
	//   $8001: A9 EA A9   LDA #...  2 bytes on the stale estimate, 3 in truth
	//   $8004: EA         NOP       recorded with 16-bit A -- the anchor
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;

		const uint8_t code[] = { 0x40, 0xA9, 0xEA, 0xA9, 0xEA };
		poke(0x8000, code, sizeof(code));
		const uint8_t st_8bit  = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
		const uint8_t st_16bit = FLAG_INDEX_WIDTH;
		regs.status = st_8bit;
		code_map_record(0x8000, 0, 0, 0, st_8bit);
		code_map_record(0x8004, 0, 0, 0, st_16bit);

		code_map_line_t lines[8];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 4, lines, 8, &next);

		check(n >= 2 && lines[0].recorded && lines[0].size == 1,
		      "decodes RTI itself from its own anchor");

		// cm_propagate skips RTI, so the next line decodes on the pre-RTI width.
		check(n >= 2 && lines[1].addr == 0x8001 && lines[1].size == 2 &&
		          lines[1].status == st_8bit,
		      "leaves the width estimate alone across RTI instead of guessing");

		// The guess is labelled as one and the next anchor is reached, restoring the real width.
		check(n >= 2 && !lines[1].recorded,
		      "reports a line decoded across RTI as a guess, not as known");
		bool resynced = false;
		for (int i = 0; i < n; i++) {
			if (lines[i].addr == 0x8004 && lines[i].recorded && lines[i].status == st_16bit) {
				resynced = true;
			}
		}
		check(resynced, "reaches the anchor after RTI and recovers the real width");
		check_tiles(lines, n, "tiles the range either side of an RTI");

		regs.is65c816 = false;
	}
	// ── Reads go through the window that backs each address ─────────────────
	// Reads name each address's bank; a wrong window shows the wrong instruction.
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

		// Low memory is unbanked: ask for the current bank.
		g_low_mem_windowed = 0;
		n = code_map_disasm_forward(0x0801, 0, 5, 2, 4, lines, 4, &next);
		check(n == 4 && g_low_mem_windowed == 0,
		      "does not put a window bank on an unbanked low-memory read");

		// An instruction straddling $BFFF/$C000 shows bytes that follow the address.
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
		      "shows each byte of a window-straddling instruction from its own bank");
		// Not covered: mnemonic and effective address of a straddling instruction.

		// REP at the last byte of the RAM window takes its operand from ROM.
		//   $BFFF: C2 20   REP #$20   opcode in RAM bank 5, operand in ROM 2
		//   $C001: A9 ..   LDA #imm   3 bytes once REP has cleared M
		// ROM bank 5 holds $00 at $C000, so reading through the opcode's window sizes the LDA at 2.
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;
		regs.status   = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH; // 8-bit A to start
		g_ram_bank    = 0x11;
		g_rom_bank    = 0x17;

		const uint8_t rep_op[]    = { 0xC2 };       // REP, in the RAM window
		const uint8_t rep_arg[]   = { 0x20 };       // its operand: clear M
		const uint8_t rep_decoy[] = { 0x00 };       // same address, wrong bank
		const uint8_t lda_imm[]   = { 0xA9, 0xEA, 0xEA };
		poke_banked(0xBFFF, 5, rep_op, sizeof(rep_op));
		poke_banked(0xC000, 2, rep_arg, sizeof(rep_arg));
		poke_banked(0xC000, 5, rep_decoy, sizeof(rep_decoy));
		poke_banked(0xC001, 2, lda_imm, sizeof(lda_imm));

		n = code_map_disasm_forward(0xBFFF, 0, 5, 2, 2, lines, 4, &next);
		// A REP is two bytes whatever its operand says; this pins that it is sized at all.
		check(n == 2 && lines[0].addr == 0xBFFF && lines[0].size == 2,
		      "sizes a REP that straddles the window boundary");
		check(n == 2 && lines[1].addr == 0xC001 && lines[1].size == 3,
		      "reads a REP operand through the window backing the operand");

		// At $9FFF/$A000 the opcode follows the live bank, the operand the RAM window.
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;
		regs.status   = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
		g_ram_bank    = 0x11;
		const uint8_t sep_op[]  = { 0xC2 };
		const uint8_t sep_arg[] = { 0x20 };
		const uint8_t sep_bad[] = { 0x00 };
		poke_banked(0x9FFF, 0, sep_op, sizeof(sep_op));   // below $A000: flat
		poke_banked(0xA000, 4, sep_arg, sizeof(sep_arg)); // requested RAM bank
		poke_banked(0xA000, 0x11, sep_bad, sizeof(sep_bad)); // live bank decoy
		poke_banked(0xA001, 4, lda_imm, sizeof(lda_imm));

		n = code_map_disasm_forward(0x9FFF, 0, 4, 0, 2, lines, 4, &next);
		check(n == 2 && lines[1].addr == 0xA001 && lines[1].size == 3,
		      "reads a REP operand across the $9FFF/$A000 boundary correctly");

		// At the top of memory the operand wraps to $0000; `addr` would size the LDA at 2.
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;
		regs.status   = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
		poke_banked(0xFFFF, 3, sep_op, sizeof(sep_op)); // REP in ROM bank 3
		poke(0x0000, sep_arg, sizeof(sep_arg));         // operand wraps to $0000
		poke(0x0001, lda_imm, sizeof(lda_imm));

		n = code_map_disasm_forward(0xFFFF, 0, 0, 3, 2, lines, 4, &next);
		check(n == 2 && lines[1].addr == 0x0001 && lines[1].size == 3,
		      "wraps the REP operand address past $FFFF instead of reading past the end");

		regs.is65c816 = false;
	}

	// ── Anchor staleness is judged in the right bank ────────────────────────
	// An anchor is believed while its opcode is still in memory; that read must use the right bank.
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
	// Gen2: a non-zero CPU bank is flat RAM; windows select nothing, even above $C000.
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

	// ── KNOWN LIMITATION: the carry feeding XCE is not tracked through data ──
	// XCE needs a carry cm_propagate does not track, so a stale one mispredicts E:
	//   $8000: 4A       LSR A   really clears C; the estimate keeps it set
	//   $8001: FB       XCE     estimate: C set -> enter emulation, force 8-bit
	//                           reality:  C clear -> stay native, A stays 16-bit
	//   $8002: A9 xx xx LDA #   3 bytes in reality, 2 on the estimate
	// Pins the gap; if XCE's inputs are tracked, this check should fail.
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;
		// 16-bit A, and carry set going in.
		regs.status = FLAG_INDEX_WIDTH | FLAG_CARRY;

		const uint8_t code[] = { 0x4A, 0xFB, 0xA9, 0xEA, 0xEA, 0xA9, 0xEA, 0xEA };
		poke(0x8000, code, sizeof(code));

		code_map_line_t lines[8];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 3, lines, 8, &next);

		check(n == 3 && lines[2].addr == 0x8002 && lines[2].size == 2,
		      "KNOWN LIMITATION: a stale carry into XCE mispredicts the widths");
		check(n == 3 && !lines[2].recorded,
		      "the mispredicted line is still reported as a guess");

		// An anchor fixes its own line; E is not re-established, so the next is wrong.
		const uint8_t st_16 = FLAG_INDEX_WIDTH;
		code_map_record(0x8002, 0, 0, 0, st_16);
		n = code_map_disasm_forward(0x8000, 0, 0, 0, 4, lines, 8, &next);
		check(n == 4 && lines[2].addr == 0x8002 && lines[2].size == 3 &&
		          lines[2].recorded,
		      "an anchor recovers the width of its own line after a bad XCE");
		check(n == 4 && lines[3].addr == 0x8005 && lines[3].size == 2 &&
		          !lines[3].recorded,
		      "KNOWN LIMITATION: past that anchor a stale E mis-sizes again");

		regs.is65c816 = false;
	}

	// ── KNOWN LIMITATION: the fold-in of emulation-mode widths uses the estimated E ───
	// The fold-in (`if (e) status |= INDEX|MEMORY`) uses the estimated e:
	//   real:  A bit 7 set and C=0, so ASL A really sets C; XCE then sees
	//          C=1,E=1 and STAYS in emulation, where widths are forced 8-bit
	//   model: the carry is not tracked through ASL, so XCE sees C=0,E=1 and
	//          predicts NATIVE
	//   then:  REP #$20 clears the model's M; the machine still forces 8-bit
	//   so:    the LDA # is really 2 bytes and the estimate sizes it 3
	//
	// regs.a only sets the premise; the assertion pins what the estimate produces.
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 1; // the machine really IS in emulation mode
		regs.a        = 0x80; // so a real ASL A would set the carry
		regs.status   = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH; // as emulation forces

		const uint8_t code[] = {
			0x0A,             // $8000 ASL A -- really sets C; not modelled
			0xFB,             // $8001 XCE   -- really stays in emulation
			0xC2, 0x20,       // $8002 REP #$20
			0xA9, 0xEA, 0xEA, // $8004 LDA # -- really 2 bytes in emulation
		};
		poke(0x8000, code, sizeof(code));

		code_map_line_t lines[8];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 4, lines, 8, &next);

		check(n == 4 && lines[3].addr == 0x8004 && lines[3].size == 3,
		      "KNOWN LIMITATION: a stale carry mis-sizes even in emulation mode");
		check(n == 4 && !lines[3].recorded,
		      "that mis-sized line is still reported as a guess");

		regs.is65c816 = false;
		regs.e        = 0;
	}

	// ── KNOWN LIMITATION: a stale anchor can also decode too SHORT ───────────
	// Too short: an 8-bit-A anchor sizes LDA # at 2 bytes; the rows still tile.
	{
		reset_all();
		regs.is65c816 = true;
		regs.e        = 0;

		// Old code, 8-bit A: LDA #$11 is two bytes.
		const uint8_t old_code[] = { 0xA9, 0x11 };
		poke(0x8000, old_code, sizeof(old_code));
		code_map_record(0x8000, 0, 0, 0, FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH);

		// $A9 is kept but runs with 16-bit A, so the real LDA is 3 bytes.
		const uint8_t new_code[] = { 0xA9, 0xEA, 0xEA, 0xEA };
		poke(0x8000, new_code, sizeof(new_code));
		regs.status = FLAG_INDEX_WIDTH;

		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n    = code_map_disasm_forward(0x8000, 0, 0, 0, 2, lines, 4, &next);

		check(n == 2 && lines[0].size == 2,
		      "KNOWN LIMITATION: a stale anchor can size an instruction too short");
		check(n == 2 && lines[1].addr == 0x8002,
		      "KNOWN LIMITATION: the next row then starts inside a real operand");
		check_tiles(lines, n, "the too-short reading still tiles, so it looks correct");

		regs.is65c816 = false;
	}
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

		// Negative "lines before" is clamped; pin where the center lands.
		center_index = -99;
		int n = code_map_disasm_window(0x8000, 0, 0, 0, -5, 1, lines, 4, &center_index);
		check(n == 1 && center_index == 0 && lines[0].addr == 0x8000,
		      "treats a negative lines_before as none at all");

		// An absurd count is capped: 255 preceding lines (256-entry walk buffer).
		static code_map_line_t wide[300];
		center_index = -99;
		n = code_map_disasm_window(0x8000, 0, 0, 0, 100000, 1, wide, 300, &center_index);
		check(n == 256 && center_index == 255,
		      "caps an absurd lines_before at the backward-walk ceiling");
		check_tiles(wide, n, "tiles a window capped by the backward-walk ceiling");
	}

	// ── Wrapping at the top of the address space ────────────────────────────
	// Decoding off the end of memory wraps to $0000 and reads the bytes there.
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

	// An opcode at $FFFF must take its operand from $0000/$0001, even though
	// those bytes are reached through a different memory window.
	{
		reset_all();
		const uint8_t opcode = 0xAD; // LDA $1234
		const uint8_t operand[] = { 0x34, 0x12 };
		poke(0xFFFF, &opcode, 1);
		poke(0x0000, operand, sizeof(operand));

		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n = code_map_disasm_forward(0xFFFE, 0, 0, 0, 3, lines, 4, &next);

		check(n == 3 && lines[1].addr == 0xFFFF && lines[1].size == 3 &&
		          lines[1].bytes[0] == 0xAD && lines[1].bytes[1] == 0x34 &&
		          lines[1].bytes[2] == 0x12,
		      "reads operand bytes across the $FFFF wrap");
		check(n == 3 && lines[2].addr == 0x0002 && next == 0x0003,
		      "resumes after a wrapped operand");
		check_tiles(lines, n, "tiles an instruction with a wrapped operand");
	}

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
