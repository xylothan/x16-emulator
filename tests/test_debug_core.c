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
bool        is_gen2;

static uint8_t g_mem[0x10000];
static uint8_t g_ram_bank = 0;
static uint8_t g_rom_bank = 0;

// The four CPU configurations the emulator can be in, since three of them
// change how wide a register comparison is.
typedef enum { CPU_65C02, CPU_816_EMU, CPU_816_NATIVE_8, CPU_816_NATIVE_16 } cpu_mode_t;

static const char *
cpu_mode_name(cpu_mode_t m)
{
	switch (m) {
		case CPU_65C02:          return "65C02";
		case CPU_816_EMU:        return "65C816 emulation";
		case CPU_816_NATIVE_8:   return "65C816 native, 8-bit";
		case CPU_816_NATIVE_16:  return "65C816 native, 16-bit";
	}
	return "?";
}

static void
set_cpu_mode(cpu_mode_t m)
{
	switch (m) {
		case CPU_65C02:
			regs.is65c816 = false;
			regs.e        = 1;
			regs.status   = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
			break;
		case CPU_816_EMU:
			regs.is65c816 = true;
			regs.e        = 1;
			// Emulation mode forces 8-bit whatever these bits say.
			regs.status   = 0;
			break;
		case CPU_816_NATIVE_8:
			regs.is65c816 = true;
			regs.e        = 0;
			regs.status   = FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
			break;
		case CPU_816_NATIVE_16:
			regs.is65c816 = true;
			regs.e        = 0;
			regs.status   = 0;
			break;
	}
}

// True where the CPU is treating A/X/Y as 16-bit.
static bool
mode_is_16bit(cpu_mode_t m)
{
	return m == CPU_816_NATIVE_16;
}

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
	struct breakpoint bp = { pc, bank, x16Bank, 0, false };
	return bp;
}

// ---- Owner-defaulting shim -------------------------------------------------
//
// Ownership is exercised on its own further down. Everything above it is about
// bank matching, conditions and counts, which are the same whoever asked, so
// those cases name a single owner through these rather than restating it on
// every line. Deliberately thin: each is the ownership call the emulator itself
// makes, with DEBUG_OWNER_CLI standing in for "somebody".

static int
debug_bp_add(struct breakpoint bp)
{
	// The old entry point answered with the new entry's index, and several
	// cases below check that a second breakpoint got its own slot rather than
	// displacing the first. A created entry always lands at the end.
	return debug_bp_add_for(bp, DEBUG_OWNER_CLI) == DEBUG_ADD_CREATED ? numBreakpoints - 1 : -1;
}

static bool
debug_bp_remove(int pc, uint8_t bank, int x16Bank)
{
	return debug_bp_delete(pc, bank, x16Bank);
}

static void
debug_bp_toggle(int pc, uint8_t bank, int x16Bank)
{
	debug_bp_toggle_for(pc, bank, x16Bank, DEBUG_OWNER_CLI);
}

static int
debug_wp_add(uint16_t addr, uint16_t len, int x16Bank)
{
	return debug_wp_add_for(addr, len, x16Bank, DEBUG_OWNER_CLI) == DEBUG_ADD_CREATED
	           ? debug_wp_count() - 1
	           : -1;
}

static bool
debug_wp_remove(uint16_t addr, int x16Bank)
{
	return debug_wp_delete(addr, x16Bank);
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

	// ── Memory write watchpoints ────────────────────────────────────────────
	// The counterpart to a breakpoint, for data rather than code: stop when
	// something writes here. The tool for finding what is corrupting a variable
	// when you have no idea which code is responsible.
	{
		reset_all();
		check(debug_wp_count() == 0, "starts with no watchpoints");
		check(!debug_wp_check_write(0x1234, 0x00), "nothing fires before anything is watched");

		check(debug_wp_add(0x1234, 1, -1) == 0, "adds a watchpoint");
		check(debug_wp_count() == 1, "counts it");
		check(debug_wp_check_write(0x1234, 0x55), "fires on a write to the watched byte");
		check(!debug_wp_check_write(0x1235, 0x55), "does not fire on the next byte");
		check(!debug_wp_check_write(0x1233, 0x55), "does not fire on the previous byte");

		// A second watchpoint on the same address in the same bank is a
		// duplicate and is refused, rather than quietly reporting success while
		// discarding whatever the caller asked for.
		check(debug_wp_add(0x1234, 8, DEBUG_BANK_ANY) == -1 && debug_wp_count() == 1,
		      "refuses a duplicate address in the same bank");

		check(debug_wp_remove(0x1234, DEBUG_BANK_ANY), "removes a watchpoint");
		check(debug_wp_count() == 0, "the table is empty again");
		check(!debug_wp_check_write(0x1234, 0x55), "the removed one no longer fires");
		check(!debug_wp_remove(0x1234, DEBUG_BANK_ANY), "removing a missing one reports nothing done");
	}

	// ── Watching a range ────────────────────────────────────────────────────
	{
		reset_all();
		debug_wp_add(0x2000, 4, -1);
		check(debug_wp_check_write(0x2000, 0) && debug_wp_check_write(0x2003, 0),
		      "fires anywhere inside the range");
		check(!debug_wp_check_write(0x1FFF, 0) && !debug_wp_check_write(0x2004, 0),
		      "does not fire either side of it");

		// A zero length is a single byte, not an empty range that never fires.
		reset_all();
		debug_wp_add(0x3000, 0, -1);
		check(debug_wp_check_write(0x3000, 0), "a zero length watches one byte");

		// A range running off the top of memory catches nothing beyond it: the
		// coverage test is a half-open comparison in 32 bits, so the end cannot
		// wrap round to the bottom of memory.
		reset_all();
		debug_wp_add(0xFFFE, 8, DEBUG_BANK_ANY);
		check(debug_wp_check_write(0xFFFF, 0), "watches up to the last byte");
		check(!debug_wp_check_write(0x0000, 0), "does not wrap past the end of memory");
		check(!debug_wp_check_write(0x0005, 0), "really does not wrap");
	}

	// ── Value filters ───────────────────────────────────────────────────────
	// "Stop when this becomes zero" is far more useful than stopping on every
	// write to a variable that is written constantly.
	{
		reset_all();
		debug_wp_add(0x1234, 1, -1);
		check(debug_wp_set_value(0x1234, DEBUG_BANK_ANY, BPCMP_EQ, 0x00), "attaches a value filter");
		check(debug_wp_check_write(0x1234, 0x00), "fires when the written value matches");
		check(!debug_wp_check_write(0x1234, 0x01), "stays quiet when it does not");

		check(!debug_wp_set_value(0x9999, DEBUG_BANK_ANY, BPCMP_EQ, 0),
		      "declines a filter for an unwatched address");

		// Every comparison, so none is quietly inverted.
		struct { int op; uint8_t val; uint8_t written; bool want; const char *what; } wcases[] = {
			{ BPCMP_EQ, 0x10, 0x10, true,  "== when equal" },
			{ BPCMP_EQ, 0x10, 0x11, false, "== when not equal" },
			{ BPCMP_NE, 0x10, 0x11, true,  "!= when different" },
			{ BPCMP_NE, 0x10, 0x10, false, "!= when same" },
			{ BPCMP_LT, 0x10, 0x0F, true,  "< when below" },
			{ BPCMP_LT, 0x10, 0x10, false, "< when equal" },
			{ BPCMP_LE, 0x10, 0x10, true,  "<= when equal" },
			{ BPCMP_LE, 0x10, 0x11, false, "<= when above" },
			{ BPCMP_GT, 0x10, 0x11, true,  "> when above" },
			{ BPCMP_GT, 0x10, 0x10, false, "> when equal" },
			{ BPCMP_GE, 0x10, 0x10, true,  ">= when equal" },
			{ BPCMP_GE, 0x10, 0x0F, false, ">= when below" },
		};
		bool wp_ops_ok = true;
		for (unsigned i = 0; i < sizeof(wcases) / sizeof(wcases[0]); i++) {
			debug_wp_set_value(0x1234, DEBUG_BANK_ANY, wcases[i].op, wcases[i].val);
			if (debug_wp_check_write(0x1234, wcases[i].written) != wcases[i].want) {
				printf("      (watchpoint comparison wrong: %s)\n", wcases[i].what);
				wp_ops_ok = false;
			}
		}
		check(wp_ops_ok, "every watchpoint comparison behaves as named");
	}

	// ── Watchpoints follow the RAM bank ─────────────────────────────────────
	// $A100 holds a different variable in every bank. A watchpoint that fired
	// on all of them would bury the write it was set to find.
	{
		reset_all();
		debug_wp_add(0xA100, 1, 5);

		g_ram_bank = 5;
		check(debug_wp_check_write(0xA100, 0x99), "fires in the bank it was set for");
		g_ram_bank = 6;
		check(!debug_wp_check_write(0xA100, 0x99), "stays quiet in a different bank");

		// Below the banked window the bank is irrelevant and must be ignored.
		reset_all();
		debug_wp_add(0x0400, 1, -1);
		g_ram_bank = 7;
		check(debug_wp_check_write(0x0400, 0x01), "unbanked addresses ignore the RAM bank");

		// -1 on a banked address means "any bank", for watching an address
		// wherever it happens to live.
		reset_all();
		debug_wp_add(0xA100, 1, -1);
		g_ram_bank = 2;
		bool any_first = debug_wp_check_write(0xA100, 0x01);
		g_ram_bank = 9;
		check(any_first && debug_wp_check_write(0xA100, 0x01),
		      "a bankless banked watchpoint fires in every bank");
	}

	// ── Enable and disable ──────────────────────────────────────────────────
	{
		reset_all();
		debug_wp_add(0x1234, 1, -1);
		debug_wp_set_value(0x1234, DEBUG_BANK_ANY, BPCMP_EQ, 0x42);

		check(debug_wp_set_active(0x1234, DEBUG_BANK_ANY, false), "disables a watchpoint");
		check(!debug_wp_check_write(0x1234, 0x42), "a disabled watchpoint does not fire");
		check(debug_wp_count() == 1, "but it is still in the table");

		check(debug_wp_set_active(0x1234, DEBUG_BANK_ANY, true), "re-enables it");
		check(debug_wp_check_write(0x1234, 0x42), "and its value filter survived");
		check(!debug_wp_set_active(0x9999, DEBUG_BANK_ANY, true), "declines to enable one that is not there");
	}

	// ── Coverage query, and a full table ────────────────────────────────────
	{
		reset_all();
		debug_wp_add(0x2000, 4, -1);
		check(debug_wp_covers(0x2002), "reports a watched address as covered");
		check(!debug_wp_covers(0x2004), "reports an unwatched address as not covered");
		debug_wp_set_active(0x2000, DEBUG_BANK_ANY, false);
		check(!debug_wp_covers(0x2002), "a disabled watchpoint covers nothing");

		reset_all();
		int added = 0;
		for (int i = 0; i < MAX_WATCHPOINTS + 10; i++) {
			if (debug_wp_add((uint16_t)(0x4000 + i), 1, -1) >= 0) {
				added++;
			}
		}
		check(added == MAX_WATCHPOINTS, "fills the table and then refuses more");
		check(debug_wp_count() == MAX_WATCHPOINTS, "does not overflow the table");
		check(debug_wp_check_write(0x4000, 0) && debug_wp_check_write(0x403F, 0),
		      "every entry that fit still works");

		debug_wp_clear_all();
		check(debug_wp_count() == 0, "clears them all");
		check(!debug_wp_check_write(0x4000, 0), "and none fire afterwards");
	}

	// ── Removal keeps the survivors intact ──────────────────────────────────
	{
		reset_all();
		for (int i = 0; i < 5; i++) {
			debug_wp_add((uint16_t)(0x5000 + i * 0x10), 1, -1);
		}
		debug_wp_remove(0x5010, DEBUG_BANK_ANY);
		debug_wp_remove(0x5030, DEBUG_BANK_ANY);
		check(debug_wp_count() == 3, "removes from the middle");
		check(debug_wp_check_write(0x5000, 0) && debug_wp_check_write(0x5020, 0)
		          && debug_wp_check_write(0x5040, 0),
		      "the survivors still fire");
		check(!debug_wp_check_write(0x5010, 0) && !debug_wp_check_write(0x5030, 0),
		      "the removed ones do not");
	}

	// ── One bank-selector rule, for breakpoints and watchpoints alike ───────
	// A bank selector says one of two things: "whichever bank is mapped"
	// (DEBUG_BANK_ANY) or "this specific bank". For an address outside a banked
	// window it says nothing at all, so it is normalised away on the way in --
	// otherwise it would be a hidden field distinguishing two entries that are
	// really the same.
	{
		reset_all();

		// Breakpoints: ANY fires in every bank.
		debug_bp_add(bp_at(0xA000, 0, DEBUG_BANK_ANY));
		g_ram_bank = 0;
		bool any0 = debug_bp_is_set(0xA000, 0);
		g_ram_bank = 5;
		bool any5 = debug_bp_is_set(0xA000, 0);
		g_ram_bank = 200;
		check(any0 && any5 && debug_bp_is_set(0xA000, 0),
		      "a bankless breakpoint fires in every RAM bank");

		// The ROM window follows the ROM bank the same way.
		reset_all();
		debug_bp_add(bp_at(0xC000, 0, DEBUG_BANK_ANY));
		g_rom_bank = 0;
		bool rany0 = debug_bp_is_set(0xC000, 0);
		g_rom_bank = 31;
		check(rany0 && debug_bp_is_set(0xC000, 0),
		      "a bankless breakpoint fires in every ROM bank");

		// A specific bank still means only that bank.
		reset_all();
		debug_bp_add(bp_at(0xA000, 0, 5));
		g_ram_bank = 5;
		check(debug_bp_is_set(0xA000, 0), "a specific bank fires in that bank");
		g_ram_bank = 6;
		check(!debug_bp_is_set(0xA000, 0), "and not in another");

		// Below the banked window the selector is meaningless, so a stored bank
		// must not stop the breakpoint firing, and must not create a second
		// distinct breakpoint either.
		reset_all();
		debug_bp_add(bp_at(0x0801, 0, 7));
		g_ram_bank = 3;
		check(debug_bp_is_set(0x0801, 0),
		      "an unbanked address ignores a bank it was given");
		check(debug_bp_add(bp_at(0x0801, 0, 9)) == -1,
		      "the same unbanked address is one breakpoint whatever bank is named");
		check(debug_bp_remove(0x0801, 0, DEBUG_BANK_ANY),
		      "and it can be removed without naming that bank");

		// Watchpoints follow exactly the same rule.
		reset_all();
		debug_wp_add(0xA100, 1, DEBUG_BANK_ANY);
		g_ram_bank = 1;
		bool wany1 = debug_wp_check_write(0xA100, 0);
		g_ram_bank = 99;
		check(wany1 && debug_wp_check_write(0xA100, 0),
		      "a bankless watchpoint fires in every bank");

		reset_all();
		debug_wp_add(0x0400, 1, 7);
		g_ram_bank = 2;
		check(debug_wp_check_write(0x0400, 0),
		      "an unbanked watch address ignores a bank it was given");

		// And the ROM window is watched by ROM bank, not RAM bank.
		reset_all();
		debug_wp_add(0xC100, 1, 3);
		g_rom_bank = 3;
		g_ram_bank = 9;
		check(debug_wp_check_write(0xC100, 0), "the ROM window follows the ROM bank");
		g_rom_bank = 4;
		check(!debug_wp_check_write(0xC100, 0), "and not a different ROM bank");

		// The same address can be watched in two banks at once -- the whole
		// reason the selector is part of a watchpoint's identity. Keying on the
		// address alone silently kept only the first.
		reset_all();
		check(debug_wp_add(0xA100, 1, 5) == 0, "watches an address in bank 5");
		check(debug_wp_add(0xA100, 1, 6) == 1, "watches the same address in bank 6");
		check(debug_wp_count() == 2, "keeps both");
		g_ram_bank = 5;
		bool w5 = debug_wp_check_write(0xA100, 0);
		g_ram_bank = 6;
		check(w5 && debug_wp_check_write(0xA100, 0), "each fires in its own bank");
		g_ram_bank = 7;
		check(!debug_wp_check_write(0xA100, 0), "and neither fires in a third");

		// Removing one leaves the other, and names which one.
		check(debug_wp_remove(0xA100, 5) && debug_wp_count() == 1,
		      "removes only the bank it names");
		g_ram_bank = 6;
		check(debug_wp_check_write(0xA100, 0), "the other bank's watchpoint survives");

		// A value filter belongs to one of them, not to the address.
		reset_all();
		debug_wp_add(0xA100, 1, 5);
		debug_wp_add(0xA100, 1, 6);
		check(debug_wp_set_value(0xA100, 5, BPCMP_EQ, 0x11),
		      "attaches a filter to one bank's watchpoint");
		g_ram_bank = 5;
		check(!debug_wp_check_write(0xA100, 0x22), "that bank honours its filter");
		g_ram_bank = 6;
		check(debug_wp_check_write(0xA100, 0x22), "the other bank is unaffected");
	}

	// ── Every condition operand, in every CPU mode ──────────────────────────
	// A/X/Y are 8 or 16 bits depending on the CPU and the M/X status bits;
	// SP and a memory word are always 16; P and a memory byte always 8. The
	// comparison has to agree with whatever the CPU is actually doing, in all
	// four configurations the emulator can be in.
	{
		const cpu_mode_t modes[] = { CPU_65C02, CPU_816_EMU, CPU_816_NATIVE_8,
			                         CPU_816_NATIVE_16 };
		bool low_ok = true, wide_ok = true, narrow_ok = true;

		for (unsigned mi = 0; mi < sizeof(modes) / sizeof(modes[0]); mi++) {
			const cpu_mode_t m    = modes[mi];
			const bool       wide = mode_is_16bit(m);

			// -- A, X and Y, compared on their low byte. Always works.
			const struct { int operand; const char *name; } regs8[] = {
				{ BPOPERAND_A, "A" }, { BPOPERAND_X, "X" }, { BPOPERAND_Y, "Y" },
			};
			for (unsigned ri = 0; ri < 3; ri++) {
				reset_all();
				set_cpu_mode(m);
				debug_bp_add(bp_at(0x8000, 0, DEBUG_BANK_ANY));
				regs.c = regs.x = regs.y = 0x0042;
				debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, regs8[ri].operand, 0,
				                       BPCMP_EQ, 0x42);
				if (!debug_bp_on_arrival(0x8000, 0)) {
					printf("      (%s == $42 failed on %s)\n", regs8[ri].name, cpu_mode_name(m));
					low_ok = false;
				}
				debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, regs8[ri].operand, 0,
				                       BPCMP_NE, 0x42);
				if (debug_bp_on_arrival(0x8000, 0)) {
					printf("      (%s != $42 wrongly fired on %s)\n", regs8[ri].name,
					       cpu_mode_name(m));
					low_ok = false;
				}

				// -- The high byte. Only visible where the register is 16-bit,
				// and a 16-bit value must never match an 8-bit register.
				reset_all();
				set_cpu_mode(m);
				debug_bp_add(bp_at(0x8000, 0, DEBUG_BANK_ANY));
				regs.c = regs.x = regs.y = 0x1234;
				debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, regs8[ri].operand, 0,
				                       BPCMP_EQ, 0x1234);
				bool matched16 = debug_bp_on_arrival(0x8000, 0);
				if (matched16 != wide) {
					printf("      (%s == $1234 %s on %s)\n", regs8[ri].name,
					       matched16 ? "wrongly matched" : "did not match", cpu_mode_name(m));
					wide_ok = false;
				}

				// The low byte alone must satisfy an 8-bit register and must
				// NOT satisfy a 16-bit one.
				debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, regs8[ri].operand, 0,
				                       BPCMP_EQ, 0x34);
				bool matched8 = debug_bp_on_arrival(0x8000, 0);
				if (matched8 == wide) {
					printf("      (%s == $34 with A=$1234 %s on %s)\n", regs8[ri].name,
					       matched8 ? "wrongly matched" : "did not match", cpu_mode_name(m));
					narrow_ok = false;
				}
			}
		}
		check(low_ok, "A, X and Y compare their low byte in every CPU mode");
		check(wide_ok, "A, X and Y compare 16 bits only where the CPU is 16-bit");
		check(narrow_ok, "an 8-bit register is not matched by a 16-bit value, and vice versa");

		// -- SP, P, and memory operands are the same width in every mode.
		bool fixed_ok = true;
		for (unsigned mi = 0; mi < sizeof(modes) / sizeof(modes[0]); mi++) {
			const cpu_mode_t m = modes[mi];
			reset_all();
			set_cpu_mode(m);
			debug_bp_add(bp_at(0x8000, 0, DEBUG_BANK_ANY));

			regs.sp = 0x01F0;
			debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, BPOPERAND_SP, 0, BPCMP_EQ, 0x01F0);
			if (!debug_bp_on_arrival(0x8000, 0)) { printf("      (SP full on %s)\n", cpu_mode_name(m)); fixed_ok = false; }
			debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, BPOPERAND_SP, 0, BPCMP_EQ, 0x00F0);
			if (debug_bp_on_arrival(0x8000, 0)) { printf("      (SP truncated on %s)\n", cpu_mode_name(m)); fixed_ok = false; }

			// P is 8 bits wide on every CPU. Set it last: it is the mode.
			uint8_t saved = regs.status;
			debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, BPOPERAND_P, 0, BPCMP_EQ, saved);
			if (!debug_bp_on_arrival(0x8000, 0)) { printf("      (P on %s)\n", cpu_mode_name(m)); fixed_ok = false; }

			g_mem[0x1234] = 0x99;
			g_mem[0x1235] = 0xAB;
			debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, BPOPERAND_BYTE, 0x1234, BPCMP_EQ, 0x99);
			if (!debug_bp_on_arrival(0x8000, 0)) { printf("      (BYTE on %s)\n", cpu_mode_name(m)); fixed_ok = false; }
			debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, BPOPERAND_WORD, 0x1234, BPCMP_EQ, 0xAB99);
			if (!debug_bp_on_arrival(0x8000, 0)) { printf("      (WORD on %s)\n", cpu_mode_name(m)); fixed_ok = false; }
			debug_bp_set_condition(0x8000, 0, DEBUG_BANK_ANY, BPOPERAND_WORD, 0x1234, BPCMP_EQ, 0x99AB);
			if (debug_bp_on_arrival(0x8000, 0)) { printf("      (WORD byte order on %s)\n", cpu_mode_name(m)); fixed_ok = false; }
		}
		check(fixed_ok, "SP, P, and memory operands keep their width in every CPU mode");

		reset_all();
	}


	// ── PC matching across banks and machine types ──────────────────────────
	// A breakpoint's address is (pc, program bank, bank selector). Which of
	// those actually select memory depends on the machine, so the same
	// breakpoint has to behave correctly on a stock X16, on one with a 65C816,
	// and on a Gen2 where a non-zero program bank maps its own flat 64K.
	{
		// -- Unbanked low memory: neither bank means anything.
		reset_all();
		// Named with a specific bank, which is the only way this proves
		// anything: an ANY selector matches before the bank rule is consulted,
		// so it would pass even under a rule that wrongly called low memory
		// banked. A bank named here has to be normalised away on insert.
		debug_bp_add(bp_at(0x0801, 0, 7));
		bool unbanked_ok = true;
		for (int b = 0; b < 3; b++) {
			g_ram_bank = (uint8_t)(b * 7);
			g_rom_bank = (uint8_t)(b * 3);
			if (!debug_bp_is_set(0x0801, 0))
				unbanked_ok = false;
		}
		check(unbanked_ok, "an unbanked address matches whatever banks are mapped");

		// And the rule itself agrees, asked directly.
		check(debug_bank_selector_matches(7, 0x0801, 0),
		      "a bank named for low memory does not filter anything out");

		// -- RAM window, every bank a stock X16 can have.
		reset_all();
		debug_bp_add(bp_at(0xA000, 0, 5));
		bool ram_exact_ok = true;
		for (int b = 0; b < 256; b++) {
			g_ram_bank = (uint8_t)b;
			if (debug_bp_is_set(0xA000, 0) != (b == 5))
				ram_exact_ok = false;
		}
		check(ram_exact_ok, "a RAM-bank breakpoint matches exactly one of all 256 banks");

		reset_all();
		debug_bp_add(bp_at(0xA000, 0, DEBUG_BANK_ANY));
		bool ram_any_ok = true;
		for (int b = 0; b < 256; b++) {
			g_ram_bank = (uint8_t)b;
			if (!debug_bp_is_set(0xA000, 0))
				ram_any_ok = false;
		}
		check(ram_any_ok, "a bankless RAM breakpoint matches all 256 banks");

		// -- ROM window, every bank the machine has.
		reset_all();
		debug_bp_add(bp_at(0xE000, 0, 7));
		bool rom_exact_ok = true;
		for (int b = 0; b < 32; b++) {
			g_rom_bank = (uint8_t)b;
			if (debug_bp_is_set(0xE000, 0) != (b == 7))
				rom_exact_ok = false;
		}
		check(rom_exact_ok, "a ROM-bank breakpoint matches exactly one of all 32 banks");

		reset_all();
		debug_bp_add(bp_at(0xE000, 0, DEBUG_BANK_ANY));
		bool rom_any_ok = true;
		for (int b = 0; b < 32; b++) {
			g_rom_bank = (uint8_t)b;
			if (!debug_bp_is_set(0xE000, 0))
				rom_any_ok = false;
		}
		check(rom_any_ok, "a bankless ROM breakpoint matches all 32 banks");

		// -- The RAM bank must not be consulted for a ROM address, or vice
		// versa: the two windows have independent bank registers.
		reset_all();
		debug_bp_add(bp_at(0xA000, 0, 4));
		debug_bp_add(bp_at(0xE000, 0, 4));
		g_ram_bank = 4;
		g_rom_bank = 9;
		bool ram_hit = debug_bp_is_set(0xA000, 0), rom_miss = debug_bp_is_set(0xE000, 0);
		g_ram_bank = 9;
		g_rom_bank = 4;
		check(ram_hit && !rom_miss && !debug_bp_is_set(0xA000, 0) && debug_bp_is_set(0xE000, 0),
		      "the two windows use their own bank registers");

		// -- Gen1 with a 65C816: the program bank selects nothing, so the RAM
		// window still applies at $A000 even from program bank 1.
		reset_all();
		is_gen2 = false;
		debug_bp_add(bp_at(0xA000, 1, 5));
		g_ram_bank = 5;
		bool g1_hit = debug_bp_is_set(0xA000, 1);
		g_ram_bank = 6;
		check(g1_hit && !debug_bp_is_set(0xA000, 1),
		      "on a gen1 65C816 the RAM window still applies inside a program bank");

		// -- Gen2 (GS): a non-zero program bank is flat RAM, so the window
		// registers select nothing and must not filter the breakpoint out.
		reset_all();
		is_gen2 = true;
		// Added with a SPECIFIC bank, not ANY. An ANY selector short-circuits
		// before the bank rule is ever consulted, so it passes whatever that
		// rule does -- which is how the only direct check of this case used to
		// pass with the very bug it was written for.
		debug_bp_add(bp_at(0xA000, 1, 5));
		bool gs_ok = true;
		for (int b = 0; b < 8; b++) {
			g_ram_bank = (uint8_t)b;
			if (!debug_bp_is_set(0xA000, 1))
				gs_ok = false;
		}
		check(gs_ok, "in a Gen2 program bank the RAM bank does not apply");

		// The shared entry point, called directly. hitBreakpoint() in the
		// debugger reaches the rule through this and nothing else did.
		check(debug_bank_selector_matches(5, 0xA000, 1),
		      "a named bank does not filter inside a Gen2 program bank");
		g_ram_bank = 5;
		is_gen2 = false;
		check(debug_bank_selector_matches(5, 0xA000, 1),
		      "but on gen1 the window still applies inside a program bank");
		g_ram_bank = 6;
		check(!debug_bank_selector_matches(5, 0xA000, 1),
		      "and there it filters on the mapped bank");
		is_gen2 = true;

		// And a bank named for such an address is normalised away rather than
		// silently making a second, unreachable breakpoint.
		check(debug_bp_add(bp_at(0xA000, 1, 3)) == -1,
		      "naming a bank inside a Gen2 program bank does not make a new breakpoint");

		// -- Program bank 0 on a Gen2 is still the ordinary windowed map.
		reset_all();
		is_gen2 = true;
		debug_bp_add(bp_at(0xA000, 0, 5));
		g_ram_bank = 5;
		bool gs0_hit = debug_bp_is_set(0xA000, 0);
		g_ram_bank = 6;
		check(gs0_hit && !debug_bp_is_set(0xA000, 0),
		      "program bank 0 on a Gen2 is banked as usual");

		// -- The program bank is part of the address: the same pc in two
		// program banks is two different breakpoints.
		reset_all();
		is_gen2 = true;
		check(debug_bp_add(bp_at(0x8000, 0, DEBUG_BANK_ANY)) == 0
		          && debug_bp_add(bp_at(0x8000, 1, DEBUG_BANK_ANY)) == 1,
		      "the same address in two program banks is two breakpoints");
		check(debug_bp_is_set(0x8000, 0) && debug_bp_is_set(0x8000, 1)
		          && !debug_bp_is_set(0x8000, 2),
		      "each program bank matches only itself");

		reset_all();
	}


	// ── Watchpoints, the same matrix ────────────────────────────────────────
	{
		reset_all();
		debug_wp_add(0xA100, 1, 5);
		bool wp_ram_ok = true;
		for (int b = 0; b < 256; b++) {
			g_ram_bank = (uint8_t)b;
			if (debug_wp_check_write(0xA100, 0) != (b == 5))
				wp_ram_ok = false;
		}
		check(wp_ram_ok, "a RAM-bank watchpoint fires in exactly one of all 256 banks");

		reset_all();
		debug_wp_add(0xA100, 1, DEBUG_BANK_ANY);
		bool wp_any_ok = true;
		for (int b = 0; b < 256; b++) {
			g_ram_bank = (uint8_t)b;
			if (!debug_wp_check_write(0xA100, 0))
				wp_any_ok = false;
		}
		check(wp_any_ok, "a bankless watchpoint fires in all 256 banks");

		reset_all();
		debug_wp_add(0xE000, 1, 7);
		bool wp_rom_ok = true;
		for (int b = 0; b < 32; b++) {
			g_rom_bank = (uint8_t)b;
			if (debug_wp_check_write(0xE000, 0) != (b == 7))
				wp_rom_ok = false;
		}
		check(wp_rom_ok, "a ROM-window watchpoint fires in exactly one of all 32 banks");

		// A watchpoint address is only ever bank 0, so a Gen2 program bank
		// cannot change what it means -- but the lookup still has to normalise
		// consistently, or removal by a named bank would miss.
		reset_all();
		debug_wp_add(0x0400, 1, 7);       // bank named for an unbanked address
		check(debug_wp_remove(0x0400, DEBUG_BANK_ANY),
		      "an unbanked watchpoint can be removed without naming a bank");
		reset_all();
		debug_wp_add(0x0400, 1, DEBUG_BANK_ANY);
		check(debug_wp_remove(0x0400, 7),
		      "and can be removed while naming one, since it means nothing there");

		// A watchpoint's bank means one thing for the whole range, judged on
		// where the range starts. The selector's meaning changes at $C000 (RAM
		// bank below, ROM bank above), so a range spanning that line would
		// otherwise have its halves tested against unrelated registers.
		reset_all();
		debug_wp_add(0xBFFF, 2, 5);
		g_ram_bank = 5;
		g_rom_bank = 9;                   // deliberately not 5
		bool lo_hit = debug_wp_check_write(0xBFFF, 0);
		check(lo_hit && debug_wp_check_write(0xC000, 0),
		      "a range crossing $C000 keeps its bank meaning throughout");
		g_ram_bank = 6;
		check(!debug_wp_check_write(0xBFFF, 0) && !debug_wp_check_write(0xC000, 0),
		      "and the whole range stops matching when that bank is unmapped");

		reset_all();
	}

	// ── Ownership ───────────────────────────────────────────────────────────
	// The table used to record only that an address was wanted, never by whom,
	// so the first remove disarmed it for everyone. Five independent things can
	// want one address -- -bp, the debugger's F9, and the three kinds of DAP
	// breakpoint -- and the DAP server spent eight review rounds trying to
	// reconstruct from outside what only the table can know. Every case here is
	// a defect that was actually found during those rounds.
	{
		// -- Two owners, one address. The first to let go must not disarm the
		// other. (Round 4: teardown deleted -bp and F9 breakpoints outright.)
		reset_all();
		check(debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI) == DEBUG_ADD_CREATED,
		      "the first owner creates the entry");
		check(debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE) == DEBUG_ADD_EXISTED,
		      "the second owner joins the entry that is already there");
		check(numBreakpoints == 1, "two owners at one address are still one place to stop");

		check(debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_CLI),
		      "the first owner can let go");
		check(numBreakpoints == 1 && debug_bp_is_set(0x2000, 0),
		      "and the breakpoint stays armed for the owner that still wants it");
		check(debug_bp_on_arrival(0x2000, 0), "and still actually fires");

		// -- ...and when the last owner goes, so does the entry. (Round 5: the
		// orphan -- armed, with no table naming it, unreachable by any request.
		// On a headless -debugport run that is a machine that cannot resume.)
		check(debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE),
		      "the last owner can let go too");
		check(numBreakpoints == 0 && !debug_bp_is_set(0x2000, 0),
		      "and then nothing is left armed");

		// -- Letting go of something you never asked for changes nothing.
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI);
		check(!debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE),
		      "an owner that never asked cannot remove");
		check(numBreakpoints == 1, "and the entry is untouched");

		// -- Asking twice as the same owner is idempotent, and one clear
		// retires it. Two source paths with the same basename resolved to one
		// address and the two records vetoed each other's removal, leaving it
		// armed. (Round 8: the mutual veto.)
		reset_all();
		debug_bp_add_for(bp_at(0x3000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_add_for(bp_at(0x3000, 0, -1), DEBUG_OWNER_DAP_SOURCE) == DEBUG_ADD_EXISTED,
		      "one owner asking twice is idempotent");
		check(debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE) == 1,
		      "and one clear retires it exactly once");
		check(numBreakpoints == 0, "leaving nothing armed");

		// -- Clearing an owner that holds nothing is a no-op.
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI);
		check(debug_bp_clear_owner(DEBUG_OWNER_DAP_FUNCTION) == 0,
		      "clearing an owner that holds nothing disarms nothing");
		check(numBreakpoints == 1, "and leaves the table alone");

		// -- A -bp breakpoint survives a DAP session, with its condition and
		// hit count intact. (Rounds 5 and 6: the flag that was meant to protect
		// it either orphaned it or handed it to the wrong table.)
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI);
		debug_bp_set_condition(0x2000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 5);
		regs.c = 5;
		debug_bp_on_arrival(0x2000, 0);
		debug_bp_on_arrival(0x2000, 0);
		check(debug_bp_get_hits(0x2000, 0, -1) == 2, "the -bp breakpoint has been hit twice");

		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE) == 0,
		      "a DAP session ending disarms nothing another owner still wants");
		check(debug_bp_is_set(0x2000, 0), "the -bp breakpoint is still armed");

		int      has_cond = 0, operand = 0, op = 0;
		uint32_t value = 0, ignore = 0;
		uint16_t operand_addr = 0;
		check(debug_bp_get_condition(0x2000, 0, -1, &has_cond, &operand, &operand_addr, &op,
		                             &value, &ignore)
		          && has_cond && operand == BPOPERAND_A && op == BPCMP_EQ && value == 5,
		      "with its condition intact");
		check(debug_bp_get_hits(0x2000, 0, -1) == 2, "and its hit count intact");

		// -- A client re-sending its list keeps the count. VS Code re-sends a
		// whole file's breakpoints on every edit, so this path runs constantly;
		// a count that reset each time could not drive a hit condition.
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_on_arrival(0x2000, 0);
		debug_bp_on_arrival(0x2000, 0);
		debug_bp_on_arrival(0x2000, 0);
		debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
		check(numBreakpoints == 0, "the client's breakpoint goes when the client drops it");
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_get_hits(0x2000, 0, -1) == 3,
		      "but re-sending the same breakpoint keeps its hit count");

		// -- The stale-registry pair. The server cached a per-address verdict
		// and nothing invalidated it, so it went wrong in both directions.
		//
		// Direction 1: a client set and cleared an address, the user then set
		// their own there, and teardown deleted the user's. (Round 7.)
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_UI);   // the user's F9
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);             // session teardown
		check(numBreakpoints == 1 && debug_bp_is_set(0x2000, 0),
		      "a client session ending leaves the user's F9 breakpoint armed");
		check(debug_bp_has_owner(0x2000, 0, -1, DEBUG_OWNER_UI)
		          && !debug_bp_has_owner(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE),
		      "and owned by the user alone");

		// Direction 2: a cached "someone else owns this" survived the user
		// deleting their own, leaving a server-created entry armed and orphaned.
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE);
		debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_CLI);      // the user deletes theirs
		check(numBreakpoints == 0, "both owners letting go clears the address");
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
		check(numBreakpoints == 0,
		      "and a later client breakpoint there is not mistaken for the user's");

		// -- The human's delete is authoritative. Under a strict refcount F9 on
		// a client's breakpoint would only drop the UI's own reference and
		// leave it armed, so the user would press the key and watch nothing
		// happen. Whoever asked for it, the keyboard wins.
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_FUNCTION);
		check(debug_bp_delete(0x2000, 0, -1), "the user can delete a client's breakpoint");
		check(numBreakpoints == 0 && !debug_bp_is_set(0x2000, 0),
		      "and it is really gone, not merely down one owner");

		// F9 over an address nobody has claimed creates one owned by the UI;
		// pressing it again takes it away, exactly as it did before ownership.
		reset_all();
		debug_bp_toggle_for(0x2000, 0, -1, DEBUG_OWNER_UI);
		check(debug_bp_is_set(0x2000, 0) && debug_bp_has_owner(0x2000, 0, -1, DEBUG_OWNER_UI),
		      "F9 on a clear address arms one for the user");
		debug_bp_toggle_for(0x2000, 0, -1, DEBUG_OWNER_UI);
		check(!debug_bp_is_set(0x2000, 0), "and F9 again takes it away");

		// -- Ownership keys are normalised like every other key. A bank named
		// for an address that is not banked must not produce an entry that the
		// owner can never name again. (This was its own defect, on gen2 with a
		// non-zero program bank.)
		reset_all();
		debug_bp_add_for(bp_at(0x0801, 0, 7), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_has_owner(0x0801, 0, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE),
		      "an owner recorded with a meaningless bank is found without one");
		check(debug_bp_remove_for(0x0801, 0, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE),
		      "and can let go without naming it");
		check(numBreakpoints == 0, "leaving nothing behind");

		// -- The "restate what I still want" pattern the DAP source handler
		// uses. Source files are matched to addresses by basename, so two paths
		// can resolve to one entry and one owner's two claims are a single bit;
		// removing per entry would disarm an address the owner still wants.
		// Clearing the owner and re-asserting the survivors is safe instead.
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI);        // the user's
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE); // two paths,
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE); // one address
		debug_bp_add_for(bp_at(0x2100, 0, -1), DEBUG_OWNER_DAP_SOURCE); // a third, elsewhere
		debug_bp_set_condition(0x2000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 5);
		regs.c = 5;
		debug_bp_on_arrival(0x2000, 0);

		debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_is_set(0x2000, 0),
		      "clearing the owner leaves an address another owner still wants");
		check(!debug_bp_is_set(0x2100, 0), "and releases the one only it wanted");

		// Re-assert only the surviving claim.
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_is_set(0x2000, 0) && numBreakpoints == 1,
		      "re-asserting a claim restores it without duplicating the entry");
		check(debug_bp_get_hits(0x2000, 0, -1) == 1,
		      "and the hit count survives the round trip");
		check(debug_bp_get_condition(0x2000, 0, -1, &has_cond, NULL, NULL, NULL, NULL, NULL)
		          && has_cond,
		      "as does the condition, which belongs to the address");

		reset_all();
	}

	// ── Enable and disable ──────────────────────────────────────────────────
	// Front ends used to implement this by deleting the entry and remembering
	// it themselves, which loses the condition, the count, and every other
	// view's marker. A disabled breakpoint keeps its place.
	{
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_UI);
		debug_bp_on_arrival(0x2000, 0);
		debug_bp_on_arrival(0x2000, 0);

		check(debug_bp_set_enabled(0x2000, 0, -1, false), "a breakpoint can be disabled");
		check(numBreakpoints == 1, "and keeps its place in the table");
		check(!debug_bp_is_set(0x2000, 0), "but no longer counts as set");
		check(!debug_bp_on_arrival(0x2000, 0), "and does not stop the machine");
		check(debug_bp_get_hits(0x2000, 0, -1) == 2,
		      "arriving while disabled does not spend the count");
		check(debug_bp_has_owner(0x2000, 0, -1, DEBUG_OWNER_UI),
		      "and it still belongs to whoever asked for it");

		check(debug_bp_set_enabled(0x2000, 0, -1, true), "and can be enabled again");
		check(debug_bp_on_arrival(0x2000, 0), "whereupon it fires again");
		check(debug_bp_get_hits(0x2000, 0, -1) == 3, "carrying its count with it");

		// Asking for a breakpoint is asking for it to be armed, so a client
		// re-sending one that is disabled gets an armed breakpoint back.
		debug_bp_set_enabled(0x2000, 0, -1, false);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_is_enabled(0x2000, 0, -1), "re-adding a disabled breakpoint re-arms it");

		check(!debug_bp_set_enabled(0x9999, 0, -1, false),
		      "enabling something that is not there reports so");

		reset_all();
	}

	// ── Watchpoint ownership ────────────────────────────────────────────────
	// The same shape, and it had the same defect: -wp, the debugger and a DAP
	// client can all want one address.
	{
		reset_all();
		check(debug_wp_add_for(0xA100, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI) == DEBUG_ADD_CREATED,
		      "-wp creates the watchpoint");
		check(debug_wp_add_for(0xA100, 1, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE)
		          == DEBUG_ADD_EXISTED,
		      "a data breakpoint joins it");
		check(debug_wp_count() == 1, "two owners share one watch");

		debug_wp_set_value(0xA100, DEBUG_BANK_ANY, BPCMP_EQ, 0x42);
		check(debug_wp_clear_owner(DEBUG_OWNER_DAP_SOURCE) == 0,
		      "the client's session ending disarms nothing -wp still wants");
		check(debug_wp_count() == 1, "the -wp watchpoint survives");
		check(debug_wp_check_write(0xA100, 0x42) && !debug_wp_check_write(0xA100, 0x01),
		      "with its value filter intact");

		check(debug_wp_remove_for(0xA100, DEBUG_BANK_ANY, DEBUG_OWNER_CLI),
		      "and goes when its last owner does");
		check(debug_wp_count() == 0, "leaving nothing watched");

		// The console's watch_clear is the human speaking, so it takes
		// everything, exactly like the breakpoint delete above.
		reset_all();
		debug_wp_add_for(0xA100, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI);
		debug_wp_add_for(0xA100, 1, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_CONSOLE);
		check(debug_wp_delete(0xA100, DEBUG_BANK_ANY), "a delete takes the whole watch");
		check(debug_wp_count() == 0, "whoever asked for it");

		// A full table has to be distinguishable from a duplicate. The server
		// read one -1 as "already there" and would have armed nothing while
		// believing it had.
		reset_all();
		bool filled = true;
		for (int i = 0; i < MAX_WATCHPOINTS; i++) {
			if (debug_wp_add_for((uint16_t)(0x1000 + i), 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI)
			    != DEBUG_ADD_CREATED)
				filled = false;
		}
		check(filled, "the watchpoint table takes its full complement");
		check(debug_wp_add_for(0x2000, 1, DEBUG_BANK_ANY, DEBUG_OWNER_CLI) == DEBUG_ADD_FULL,
		      "one more reports a full table");
		check(debug_wp_add_for(0x1000, 1, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE)
		          == DEBUG_ADD_EXISTED,
		      "which is not the same answer as one that is already there");

		reset_all();
	}

	debug_core_free();

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
