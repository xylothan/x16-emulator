// Checks for the debugger breakpoint core (src/debug_core.c).
//
// Upstream tracked one breakpoint in one global, so there was very little to
// get wrong. A table with bank matching, conditions and ignore counts has real
// decision logic in it, and that logic decides whether a debugging session
// stops where you asked -- which is exactly the sort of thing that should not
// be verified by clicking around a UI.
//
// debug_core.c reaches for only the register file, the two bank registers and a
// debug memory read, so this supplies its own and links no emulator core.

#include "debug_core.h"
#include "cpu/registers.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- Stand-ins for the emulator core --------------------------------------

struct regs regs;

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

// ---- Harness ---------------------------------------------------------------

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
	debug_core_free();
	memset(g_mem, 0, sizeof(g_mem));
	memset(&regs, 0, sizeof(regs));
	g_ram_bank = 0;
	g_rom_bank = 0;
}

static struct breakpoint
bp_at(int pc, uint8_t bank, int x16Bank)
{
	struct breakpoint bp = { pc, bank, x16Bank };
	return bp;
}

int
main(void)
{
	// ── The table ───────────────────────────────────────────────────────────
	// Upstream had exactly one breakpoint, so setting a second silently threw
	// the first away. Holding several at once is the whole point of the table.
	{
		reset_all();
		check(numBreakpoints == 0, "starts with no breakpoints");
		check(!debug_bp_is_set(0x8000, 0), "reports nothing set before anything is added");

		check(debug_bp_add(bp_at(0x8000, 0, -1)) == 0, "adds a breakpoint");
		check(debug_bp_add(bp_at(0x9000, 0, -1)) == 1, "adds a second without losing the first");
		check(numBreakpoints == 2, "keeps both");
		check(debug_bp_is_set(0x8000, 0) && debug_bp_is_set(0x9000, 0),
		      "both breakpoints are live at once");

		check(debug_bp_add(bp_at(0x8000, 0, -1)) == -1, "refuses a duplicate address");
		check(numBreakpoints == 2, "a refused duplicate does not grow the table");

		check(debug_bp_remove(0x8000, 0, -1), "removes a breakpoint");
		check(!debug_bp_is_set(0x8000, 0), "the removed one stops matching");
		check(debug_bp_is_set(0x9000, 0), "removing one leaves the other alone");
		check(!debug_bp_remove(0x8000, 0, -1), "removing a missing breakpoint reports nothing done");

		// Removal compacts the array; the survivors must still be intact.
		reset_all();
		for (int i = 0; i < 5; i++) {
			debug_bp_add(bp_at(0x8000 + i * 0x10, 0, -1));
		}
		debug_bp_remove(0x8010, 0, -1);
		debug_bp_remove(0x8030, 0, -1);
		check(numBreakpoints == 3, "removes from the middle");
		check(debug_bp_is_set(0x8000, 0) && debug_bp_is_set(0x8020, 0) && debug_bp_is_set(0x8040, 0),
		      "the survivors are still the right ones");
		check(!debug_bp_is_set(0x8010, 0) && !debug_bp_is_set(0x8030, 0),
		      "the removed ones are gone");
	}

	// ── Growing past the initial capacity ───────────────────────────────────
	// The table starts small and doubles; entries have to survive the
	// reallocations that implies.
	{
		reset_all();
		bool all_added = true;
		for (int i = 0; i < 200; i++) {
			if (debug_bp_add(bp_at(0x1000 + i, 0, -1)) != i) {
				all_added = false;
			}
		}
		check(all_added, "adds 200 breakpoints, each at the expected index");
		check(numBreakpoints == 200, "grows past the initial capacity");
		check(debug_bp_is_set(0x1000, 0) && debug_bp_is_set(0x1063, 0) && debug_bp_is_set(0x10C7, 0),
		      "entries survive the reallocations");
	}

	// ── Toggling ────────────────────────────────────────────────────────────
	{
		reset_all();
		debug_bp_toggle(0x8000, 0, -1);
		check(debug_bp_is_set(0x8000, 0), "toggle adds when absent");
		debug_bp_toggle(0x8000, 0, -1);
		check(!debug_bp_is_set(0x8000, 0), "toggle removes when present");

		// Toggling a banked address must act on the bank being displayed, not
		// on whatever entry happens to share the address. Keying the table on
		// (pc, bank) alone made F9 in bank 6 delete the bank-5 breakpoint and
		// arm nothing -- so the user lost a breakpoint and gained none.
		reset_all();
		g_ram_bank = 5;
		debug_bp_toggle(0xA100, 0, 5);
		check(debug_bp_is_set(0xA100, 0), "arms a breakpoint in the displayed bank");

		g_ram_bank = 6;
		debug_bp_toggle(0xA100, 0, 6);
		check(debug_bp_is_set(0xA100, 0), "toggling the same address in another bank arms it");
		g_ram_bank = 5;
		check(debug_bp_is_set(0xA100, 0), "and leaves the first bank's breakpoint alone");
		check(numBreakpoints == 2, "both banks' breakpoints coexist");

		// And toggling again removes only the one for that bank.
		g_ram_bank = 6;
		debug_bp_toggle(0xA100, 0, 6);
		check(!debug_bp_is_set(0xA100, 0), "toggling off removes the right one");
		g_ram_bank = 5;
		check(debug_bp_is_set(0xA100, 0), "the other bank's breakpoint survives");
	}

	// ── Same address in two banks ───────────────────────────────────────────
	// Banked RAM means $A000 holds a different program in each bank, so wanting
	// a breakpoint at the same address in two of them is ordinary, not exotic.
	{
		reset_all();
		check(debug_bp_add(bp_at(0xA000, 0, 5)) == 0, "adds a breakpoint in bank 5");
		check(debug_bp_add(bp_at(0xA000, 0, 6)) == 1,
		      "adds one at the same address in bank 6");
		check(numBreakpoints == 2, "keeps both");

		// Each keeps its own hit count, rather than sharing one.
		g_ram_bank = 5;
		check(debug_bp_on_arrival(0xA000, 0), "stops in bank 5");
		check(debug_bp_on_arrival(0xA000, 0), "stops in bank 5 again");
		g_ram_bank = 6;
		check(debug_bp_on_arrival(0xA000, 0), "stops in bank 6");
		check(debug_bp_get_hits(0xA000, 0, 5) == 2 && debug_bp_get_hits(0xA000, 0, 6) == 1,
		      "each bank's breakpoint counts its own hits");

		// And its own condition.
		debug_bp_set_condition(0xA000, 0, 5, BPOPERAND_A, 0, BPCMP_EQ, 0x11);
		regs.c = 0x22;
		g_ram_bank = 5;
		check(!debug_bp_on_arrival(0xA000, 0), "bank 5's condition applies in bank 5");
		g_ram_bank = 6;
		check(debug_bp_on_arrival(0xA000, 0), "and does not leak into bank 6");

		check(debug_bp_add(bp_at(0xA000, 0, 5)) == -1,
		      "still refuses a true duplicate (same address AND bank)");
	}

	// ── Bank matching ───────────────────────────────────────────────────────
	// Banked RAM means the same address holds unrelated code depending on which
	// bank is mapped. A breakpoint set while looking at bank 5 must not fire on
	// bank 6's code, which upstream's single unbanked breakpoint could not
	// express at all.
	{
		reset_all();
		debug_bp_add(bp_at(0xA000, 0, 5));

		g_ram_bank = 5;
		check(debug_bp_is_set(0xA000, 0), "fires in the bank it was set for");
		g_ram_bank = 6;
		check(!debug_bp_is_set(0xA000, 0), "does not fire in a different RAM bank");

		// Below $A000 nothing is banked, so the bank must be ignored entirely
		// rather than accidentally filtering the breakpoint out.
		reset_all();
		debug_bp_add(bp_at(0x0801, 0, -1));
		g_ram_bank = 3;
		check(debug_bp_is_set(0x0801, 0), "unbanked addresses ignore the RAM bank");

		// The ROM window follows the ROM bank, not the RAM bank.
		reset_all();
		debug_bp_add(bp_at(0xC000, 0, 2));
		g_rom_bank = 2;
		g_ram_bank = 9;
		check(debug_bp_is_set(0xC000, 0), "the ROM window follows the ROM bank");
		g_rom_bank = 3;
		check(!debug_bp_is_set(0xC000, 0), "does not fire in a different ROM bank");
	}

	// ── Stopping, and counting ──────────────────────────────────────────────
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));

		check(!debug_bp_on_arrival(0x7FFF, 0), "does not stop where no breakpoint is");
		check(debug_bp_get_hits(0x8000, 0, -1) == 0, "a breakpoint starts with no hits");

		check(debug_bp_on_arrival(0x8000, 0), "stops on a plain breakpoint");
		check(debug_bp_get_hits(0x8000, 0, -1) == 1, "counts the hit");
		check(debug_bp_on_arrival(0x8000, 0), "stops again");
		check(debug_bp_get_hits(0x8000, 0, -1) == 2, "keeps counting");

		debug_bp_reset_hits(0x8000, 0, -1);
		check(debug_bp_get_hits(0x8000, 0, -1) == 0, "the count can be reset");

		// A count is kept for an address even without a condition, and survives
		// the remove/re-add a UI performs when a breakpoint is disabled and
		// re-enabled -- silently resetting it there would be surprising.
		debug_bp_on_arrival(0x8000, 0);
		debug_bp_remove(0x8000, 0, -1);
		debug_bp_add(bp_at(0x8000, 0, -1));
		check(debug_bp_get_hits(0x8000, 0, -1) == 1, "the count survives a disable/enable cycle");

		// An explicit delete does discard it.
		debug_bp_remove(0x8000, 0, -1);
		debug_bp_forget(0x8000, 0, -1);
		debug_bp_add(bp_at(0x8000, 0, -1));
		check(debug_bp_get_hits(0x8000, 0, -1) == 0, "an explicit delete discards the count");
	}

	// ── Ignore counts ───────────────────────────────────────────────────────
	// "Stop the 100th time round this loop" is the reason to have these: the
	// hit is counted every time, but execution only stops once the budget is
	// spent.
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));
		debug_bp_set_ignore(0x8000, 0, -1, 3);

		check(!debug_bp_on_arrival(0x8000, 0), "does not stop on the 1st of 3 ignored hits");
		check(!debug_bp_on_arrival(0x8000, 0), "does not stop on the 2nd");
		check(!debug_bp_on_arrival(0x8000, 0), "does not stop on the 3rd");
		check(debug_bp_on_arrival(0x8000, 0), "stops on the 4th, once the budget is spent");
		check(debug_bp_get_hits(0x8000, 0, -1) == 4, "ignored arrivals are still counted");
		check(debug_bp_on_arrival(0x8000, 0), "keeps stopping thereafter");
	}

	// ── Conditions ──────────────────────────────────────────────────────────
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 0x42);

		regs.c = 0x41;
		check(!debug_bp_on_arrival(0x8000, 0), "does not stop while the condition is false");
		check(debug_bp_get_hits(0x8000, 0, -1) == 0,
		      "an unsatisfied condition does not spend a hit");

		regs.c = 0x42;
		check(debug_bp_on_arrival(0x8000, 0), "stops once the condition holds");

		// Every comparison, so none of them is quietly inverted.
		regs.c = 0x10;
		struct { int op; uint32_t val; bool want; const char *what; } cases[] = {
			{ BPCMP_EQ, 0x10, true,  "== when equal" },
			{ BPCMP_EQ, 0x11, false, "== when not equal" },
			{ BPCMP_NE, 0x11, true,  "!= when different" },
			{ BPCMP_NE, 0x10, false, "!= when same" },
			{ BPCMP_LT, 0x11, true,  "< when below" },
			{ BPCMP_LT, 0x10, false, "< when equal" },
			{ BPCMP_LE, 0x10, true,  "<= when equal" },
			{ BPCMP_LE, 0x0F, false, "<= when above" },
			{ BPCMP_GT, 0x0F, true,  "> when above" },
			{ BPCMP_GT, 0x10, false, "> when equal" },
			{ BPCMP_GE, 0x10, true,  ">= when equal" },
			{ BPCMP_GE, 0x11, false, ">= when below" },
		};
		bool all_ok = true;
		for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, cases[i].op, cases[i].val);
			if (debug_bp_on_arrival(0x8000, 0) != cases[i].want) {
				printf("      (comparison wrong: %s)\n", cases[i].what);
				all_ok = false;
			}
		}
		check(all_ok, "every comparison operator behaves as named");

		// Clearing the condition leaves an unconditional breakpoint behind
		// rather than deleting it.
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 0xFF);
		regs.c = 0x00;
		check(!debug_bp_on_arrival(0x8000, 0), "condition still applies before clearing");
		debug_bp_clear_condition(0x8000, 0, -1);
		check(debug_bp_on_arrival(0x8000, 0), "clearing the condition makes it fire again");
	}

	// ── Condition operands ──────────────────────────────────────────────────
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));

		regs.x  = 0x33;
		regs.y  = 0x44;
		regs.sp = 0x01F0;
		regs.status = 0x24;

		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_X, 0, BPCMP_EQ, 0x33);
		check(debug_bp_on_arrival(0x8000, 0), "tests X");
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_Y, 0, BPCMP_EQ, 0x44);
		check(debug_bp_on_arrival(0x8000, 0), "tests Y");
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_P, 0, BPCMP_EQ, 0x24);
		check(debug_bp_on_arrival(0x8000, 0), "tests the status register");

		// SP is compared 16-bit wide, so a stack pointer above $FF is not
		// truncated into matching the wrong value.
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_SP, 0, BPCMP_EQ, 0x01F0);
		check(debug_bp_on_arrival(0x8000, 0), "tests the full 16-bit stack pointer");
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_SP, 0, BPCMP_EQ, 0x00F0);
		check(!debug_bp_on_arrival(0x8000, 0), "does not truncate the stack pointer to 8 bits");

		// Memory operands: "stop when this variable becomes N" is the most
		// useful conditional breakpoint there is.
		g_mem[0x1234] = 0x99;
		g_mem[0x1235] = 0xAB;
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_BYTE, 0x1234, BPCMP_EQ, 0x99);
		check(debug_bp_on_arrival(0x8000, 0), "tests a byte in memory");
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_WORD, 0x1234, BPCMP_EQ, 0xAB99);
		check(debug_bp_on_arrival(0x8000, 0), "tests a little-endian word in memory");
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_WORD, 0x1234, BPCMP_EQ, 0x99AB);
		check(!debug_bp_on_arrival(0x8000, 0), "reads the word in the right byte order");
	}

	// ── Conditions and ignore counts together ───────────────────────────────
	// The ignore count counts *satisfying* arrivals, so "the 3rd time we pass
	// here with A == 7" means what it says, rather than counting arrivals that
	// failed the test.
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 0x07);
		debug_bp_set_ignore(0x8000, 0, -1, 2);

		regs.c = 0x00;
		for (int i = 0; i < 10; i++) {
			debug_bp_on_arrival(0x8000, 0);
		}
		check(debug_bp_get_hits(0x8000, 0, -1) == 0,
		      "arrivals that fail the condition do not spend the ignore budget");

		regs.c = 0x07;
		check(!debug_bp_on_arrival(0x8000, 0), "1st satisfying arrival is ignored");
		check(!debug_bp_on_arrival(0x8000, 0), "2nd satisfying arrival is ignored");
		check(debug_bp_on_arrival(0x8000, 0), "3rd satisfying arrival stops");
	}

	// ── Reading a condition back ────────────────────────────────────────────
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));

		int      has_cond = 9, operand = 9, op = 9;
		uint16_t operand_addr = 9;
		uint32_t value = 9, ignore = 9;
		check(!debug_bp_get_condition(0x8000, 0, -1, &has_cond, &operand, &operand_addr, &op, &value, &ignore),
		      "reports nothing for a breakpoint never given a condition");

		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_BYTE, 0x1234, BPCMP_GE, 0x55);
		debug_bp_set_ignore(0x8000, 0, -1, 7);
		check(debug_bp_get_condition(0x8000, 0, -1, &has_cond, &operand, &operand_addr, &op, &value, &ignore),
		      "reads a condition back");
		check(has_cond == 1 && operand == BPOPERAND_BYTE && operand_addr == 0x1234
		          && op == BPCMP_GE && value == 0x55 && ignore == 7,
		      "reads back exactly what was set");

		// Every out-param is optional.
		check(debug_bp_get_condition(0x8000, 0, -1, NULL, NULL, NULL, NULL, NULL, NULL),
		      "tolerates NULL out-parameters");
	}

	// ── Register widths follow the CPU ──────────────────────────────────────
	// On a 65C816 in native mode A/X/Y can be 16-bit. Comparing only the low
	// byte would make "A == $1234" silently mean "A == $34" and stop in the
	// wrong place, so the comparison uses the width the CPU is using.
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 0x1234);

		// 65C02: A is 8 bits, so a 16-bit value can never match.
		regs.is65c816 = false;
		regs.c        = 0x1234;
		check(!debug_bp_on_arrival(0x8000, 0), "an 8-bit A never matches a 16-bit value");

		// 65C816 native with M clear: A is 16 bits and matches in full.
		regs.is65c816 = true;
		regs.e        = 0;
		regs.status   = FLAG_INDEX_WIDTH;             // M clear => 16-bit accumulator
		check(debug_bp_on_arrival(0x8000, 0), "a 16-bit A matches the full value");

		// The low byte alone must not be enough.
		regs.c = 0x9934;
		check(!debug_bp_on_arrival(0x8000, 0),
		      "a 16-bit A does not match on the low byte alone");

		// M set => back to 8-bit, where the low byte is all there is.
		regs.status = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 0x34);
		check(debug_bp_on_arrival(0x8000, 0), "an 8-bit A compares its low byte");

		// Emulation mode forces 8-bit regardless of the width bits.
		regs.e      = 1;
		regs.status = FLAG_INDEX_WIDTH;
		regs.c      = 0x9934;
		check(debug_bp_on_arrival(0x8000, 0), "emulation mode forces the 8-bit comparison");

		// X and Y follow the index-width bit, not the memory-width bit.
		regs.e      = 0;
		regs.status = FLAG_MEMORY_WIDTH;              // X clear => 16-bit index
		regs.x      = 0x0555;
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_X, 0, BPCMP_EQ, 0x0555);
		check(debug_bp_on_arrival(0x8000, 0), "a 16-bit X matches the full value");

		regs.y = 0x0666;
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_Y, 0, BPCMP_EQ, 0x0666);
		check(debug_bp_on_arrival(0x8000, 0), "a 16-bit Y matches the full value");

		regs.status = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;   // both 8-bit again
		check(!debug_bp_on_arrival(0x8000, 0),
		      "an 8-bit Y does not match a 16-bit value");

		regs.is65c816 = false;
		regs.e        = 0;
	}

	// ── Clearing everything really clears everything ────────────────────────
	// A full clear is a delete, not a disable. A front end that replaces its
	// whole breakpoint set -- which is how the Debug Adapter Protocol works --
	// must not silently inherit an old condition or ignore count on what the
	// user thinks is a brand new breakpoint.
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 0xAA);
		debug_bp_set_ignore(0x8000, 0, -1, 5);
		regs.c = 0xAA;
		debug_bp_on_arrival(0x8000, 0);

		debug_bp_clear_all();
		debug_bp_add(bp_at(0x8000, 0, -1));

		check(debug_bp_get_hits(0x8000, 0, -1) == 0, "a full clear discards hit counts");
		int has_cond = 9;
		check(!debug_bp_get_condition(0x8000, 0, -1, &has_cond, NULL, NULL, NULL, NULL, NULL),
		      "a full clear discards conditions");
		regs.c = 0x00;
		check(debug_bp_on_arrival(0x8000, 0),
		      "the recreated breakpoint is unconditional, not the old one");
	}

	// ── Clearing everything ─────────────────────────────────────────────────
	{
		reset_all();
		debug_bp_add(bp_at(0x8000, 0, -1));
		debug_bp_add(bp_at(0x9000, 0, -1));
		debug_bp_clear_all();
		check(numBreakpoints == 0, "clears the whole table");
		check(!debug_bp_is_set(0x8000, 0) && !debug_bp_is_set(0x9000, 0),
		      "nothing matches after clearing");
		check(!debug_bp_on_arrival(0x8000, 0), "and nothing stops");
	}

	debug_core_free();

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
