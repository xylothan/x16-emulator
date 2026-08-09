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

static uint8_t g_mem[0x10000];
static uint8_t g_ram_bank = 0;
static uint8_t g_rom_bank = 0;

uint8_t
real_read6502(uint16_t address, uint8_t bank, bool debugOn, int16_t x16Bank)
{
	(void)bank;
	(void)debugOn;
	(void)x16Bank;
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

static void
reset_all(void)
{
	code_map_reset();
	memset(g_mem, 0xEA, sizeof(g_mem)); // NOP everywhere by default
	memset(&regs, 0, sizeof(regs));
	regs.status = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH; // 8-bit A/X/Y
}

static void
poke(uint16_t addr, const uint8_t *bytes, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		g_mem[(addr + i) & 0xFFFF] = bytes[i];
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
		check(n == 3 && strstr(lines[1].text, "8d") == NULL && lines[1].text[0] != '\0',
		      "renders instruction text");

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

		check(n > 0, "still produces a window when the walks disagree");
		check(center_index >= 0, "does not lose the center line to width drift");
		check(center_index >= 0 && center_index < n && lines[center_index].addr == 0x8004,
		      "re-aligns onto the center address after drifting");
		check(n == 4, "returns exactly the requested line count despite the drift");
		check(center_index == 2, "puts exactly the requested number of lines before center");

		// The real damage from drift is contradictory output: a line claiming
		// bytes that the next line also claims. Nothing downstream can render
		// that sensibly, so the window must never emit it.
		bool overlap = false;
		for (int i = 1; i < n; i++) {
			if (lines[i].addr < lines[i - 1].addr + lines[i - 1].size) {
				overlap = true;
			}
		}
		check(!overlap, "never emits overlapping instructions");

		regs.is65c816 = false;
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
		check(code_map_disasm_window(0x8000, 0, 0, 0, -5, 1, lines, 4, &center_index) >= 1,
		      "clamps a negative lines_before");
	}

	// ── Wrapping at the top of the address space ────────────────────────────
	// Decoding off the end of memory must wrap rather than read past the end of
	// the address space.
	{
		reset_all();
		code_map_line_t lines[4];
		uint16_t        next = 0;
		int             n = code_map_disasm_forward(0xFFFE, 0, 0, 0, 3, lines, 4, &next);
		check(n == 3, "keeps disassembling across the $FFFF boundary");
		check(n == 3 && lines[2].addr < 0x0100, "wraps to the bottom of memory");
	}

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
