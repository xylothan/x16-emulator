// Checks for the debugger breakpoint core (src/debug_core.c).
//
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

// Logpoint hooks: record what fired and what was released.
#define LOG_TOKENS 8
static int  g_log_fired[LOG_TOKENS];
static int  g_log_freed[LOG_TOKENS];
static int  g_log_owner;
static int  g_log_pc;
static char g_log_token[LOG_TOKENS];

static void
log_fire(void *log, debug_owner_t owner, int pc, uint8_t bank, int x16Bank)
{
	(void)bank;
	(void)x16Bank;
	g_log_fired[(char *)log - g_log_token]++;
	g_log_owner = owner;
	g_log_pc    = pc;
}

static void
log_free(void *log)
{
	g_log_freed[(char *)log - g_log_token]++;
}

// Expression condition hooks.
#define COND_TOKENS 6
static char                g_cond_token[COND_TOKENS];
static int                 g_cond_freed[COND_TOKENS];
static int                 g_cond_tests;
static bool                g_cond_answer;
static bool                g_cond_had_access;
static debug_cond_access_t g_cond_access;

static bool
cond_test(void *cond, const debug_cond_access_t *access)
{
	(void)cond;
	g_cond_tests++;
	g_cond_had_access = access != NULL;
	if (access)
		g_cond_access = *access;
	return g_cond_answer;
}

static bool
cond_same(const void *a, const void *b)
{
	return a == b;
}

static void
cond_free(void *cond)
{
	g_cond_freed[(char *)cond - g_cond_token]++;
}

// The four CPU configurations; three change register comparison width.
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
// Names a single owner through DEBUG_OWNER_CLI so cases need not restate it.

static int
debug_bp_add(struct breakpoint bp)
{
	// A created entry always lands at the end.
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
	// Several breakpoints held at once.
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

		// Toggling a banked address acts on the displayed bank.
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
	// The same address in two banks is ordinary.
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
	// A breakpoint set in bank 5 does not fire in bank 6.
	{
		reset_all();
		debug_bp_add(bp_at(0xA000, 0, 5));

		g_ram_bank = 5;
		check(debug_bp_is_set(0xA000, 0), "fires in the bank it was set for");
		g_ram_bank = 6;
		check(!debug_bp_is_set(0xA000, 0), "does not fire in a different RAM bank");

		// Below $A000 the bank is ignored.
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

		// A count is kept without a condition and survives remove/re-add.
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
	// Hits are counted every time; execution stops once the ignore budget is spent.
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

		// Memory operands: stop when a variable becomes N.
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
	// The ignore count counts satisfying arrivals.
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
	// On a 65C816 in native mode A/X/Y can be 16-bit; the comparison uses the CPU's width.
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
	// A full clear is a delete: no condition or ignore count carries over.
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
	// Stop when something writes here.
	{
		reset_all();
		check(debug_wp_count() == 0, "starts with no watchpoints");
		check(!debug_wp_check_write(0x1234, 0x00), "nothing fires before anything is watched");

		check(debug_wp_add(0x1234, 1, -1) == 0, "adds a watchpoint");
		check(debug_wp_count() == 1, "counts it");
		check(debug_wp_check_write(0x1234, 0x55), "fires on a write to the watched byte");
		check(!debug_wp_check_write(0x1235, 0x55), "does not fire on the next byte");
		check(!debug_wp_check_write(0x1233, 0x55), "does not fire on the previous byte");

		// A duplicate watchpoint on the same address and bank is refused.
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

		// A range running off the top of memory catches nothing beyond it; the end cannot wrap.
		reset_all();
		debug_wp_add(0xFFFE, 8, DEBUG_BANK_ANY);
		check(debug_wp_check_write(0xFFFF, 0), "watches up to the last byte");
		check(!debug_wp_check_write(0x0000, 0), "does not wrap past the end of memory");
		check(!debug_wp_check_write(0x0005, 0), "really does not wrap");
	}

	// ── Value filters ───────────────────────────────────────────────────────
	// Stop on a value rather than on every write.
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
	// $A100 holds a different variable per bank.
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

	// ── A VRAM watch is a separate memory at the same numbers ───────────────
	{
		reset_all();
		const debug_wp_spec_t vram = { DEBUG_WP_SPACE_VRAM, 0x0400, 4, DEBUG_BANK_ANY, false, 0, 0 };

		check(debug_wp_add_ex(&vram, DEBUG_OWNER_MCP) == DEBUG_ADD_CREATED, "adds a VRAM watch");
		check(debug_wp_armed_vram == 1 && debug_wp_armed_cpu_write == 0,
		      "it arms the VRAM path and leaves the CPU path alone");
		check(!debug_wp_check_write(0x0400, 0), "a CPU store to the same number does not fire it");
		check(!debug_wp_covers(0x0400), "nor does it mark the CPU byte as watched");
		check(debug_wp_covers_vram(0x0403) && !debug_wp_covers_vram(0x0404),
		      "it covers its own range in VRAM");

		check(debug_wp_add(0x0400, 1, DEBUG_BANK_ANY) >= 0, "the same number can be watched in the CPU map");
		check(debug_wp_armed_vram == 1 && debug_wp_armed_cpu_write == 1, "each path counts its own");
		check(!debug_wp_check_vram(0x0404, 0, 0), "a VRAM store past the range does not fire");
		check(debug_wp_check_vram(0x0402, 0x7E, 1), "a VRAM store inside it fires");

		uint8_t  space = 0, value = 0;
		uint32_t addr = 0;
		int      port = -1;
		uint16_t cpu_addr = 0;
		check(debug_wp_last_access(&space, &addr, &value, &port) && space == DEBUG_WP_SPACE_VRAM &&
		          addr == 0x0402 && value == 0x7E && port == 1,
		      "the hit records where, what and through which port");
		check(!debug_wp_last_hit(&cpu_addr, &value), "a VRAM hit is not reported as a CPU one");
		const int at = debug_wp_find(DEBUG_WP_SPACE_VRAM, 0x0400, DEBUG_BANK_ANY);
		check(at >= 0 && debug_wp_at(at)->hits == 1, "it counts the hit");

		check(debug_wp_add_ex(&vram, DEBUG_OWNER_DAP_SOURCE) == DEBUG_ADD_EXISTED,
		      "an identical request shares the watch");
		debug_wp_spec_t other = vram;
		other.len = 8;
		check(debug_wp_add_ex(&other, DEBUG_OWNER_UI) == DEBUG_ADD_CONFLICT,
		      "a different range is refused");
		check(debug_wp_at(at)->len == 4, "and the watch is unchanged");

		debug_wp_set_active_at(at, false);
		check(debug_wp_armed_vram == 0 && !debug_wp_check_vram(0x0402, 0, 0),
		      "disabling it disarms the VRAM path");
		debug_wp_set_active_at(at, true);
		check(debug_wp_armed_vram == 1, "enabling it arms the path again");

		check(debug_wp_remove_ex(DEBUG_WP_SPACE_VRAM, 0x0400, DEBUG_BANK_ANY, DEBUG_OWNER_MCP) &&
		          debug_wp_armed_vram == 1,
		      "the other owner keeps it armed");
		check(debug_wp_remove_ex(DEBUG_WP_SPACE_VRAM, 0x0400, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE) &&
		          debug_wp_armed_vram == 0,
		      "the last owner takes it off");
		check(debug_wp_exists(0x0400, DEBUG_BANK_ANY), "the CPU watch is still there");
		debug_wp_clear_all();
		check(debug_wp_armed_cpu_write == 0, "clearing the table resets the counts");
	}

	// ── A watch's ignore count ──────────────────────────────────────────────
	{
		reset_all();
		debug_wp_spec_t spec = { DEBUG_WP_SPACE_CPU, 0x0700, 2, DEBUG_BANK_ANY, false, 0, 0,
		                         DEBUG_WP_ACCESS_BOTH, false, 2 };
		check(debug_wp_add_ex(&spec, DEBUG_OWNER_DAP_SOURCE) == DEBUG_ADD_CREATED, "ignore: adds");
		const int at = debug_wp_find(DEBUG_WP_SPACE_CPU, 0x0700, DEBUG_BANK_ANY);
		check(!debug_wp_fires_on_ex(DEBUG_WP_SPACE_CPU, DEBUG_WP_ACCESS_WRITE, false, 0x0700,
		                            DEBUG_BANK_ANY, 0x0700, 0),
		      "ignore: not named before its count is reached");
		check(!debug_wp_check_write(0x0700, 1), "ignore: hit 1 is counted, not fired");
		check(!debug_wp_check_read(0x0701, 1, false), "ignore: hit 2 is counted, not fired");
		check(debug_wp_at(at)->hits == 2, "ignore: both were counted");
		check(debug_wp_check_write(0x0701, 1), "ignore: hit 3 fires");
		check(debug_wp_fires_on_ex(DEBUG_WP_SPACE_CPU, DEBUG_WP_ACCESS_WRITE, false, 0x0700,
		                           DEBUG_BANK_ANY, 0x0701, 1),
		      "ignore: named once past its count");
		check(debug_wp_check_read(0x0700, 1, false), "ignore: and every hit after it");

		debug_wp_spec_t other = spec;
		other.ignore = 3;
		check(debug_wp_add_ex(&other, DEBUG_OWNER_MCP) == DEBUG_ADD_CONFLICT,
		      "ignore: a different count is refused");
		check(debug_wp_add_ex(&spec, DEBUG_OWNER_MCP) == DEBUG_ADD_EXISTED,
		      "ignore: the same count shares it");
		check(debug_wp_add_for(0x0700, 2, DEBUG_BANK_ANY, DEBUG_OWNER_UI) == DEBUG_ADD_CONFLICT,
		      "ignore: an owner that would miss hits is refused");

		reset_all();
		const debug_wp_spec_t filtered = { DEBUG_WP_SPACE_CPU, 0x0710, 1, DEBUG_BANK_ANY, true,
		                                   BPCMP_EQ, 7, DEBUG_WP_ACCESS_WRITE, false, 1 };
		debug_wp_add_ex(&filtered, DEBUG_OWNER_DAP_SOURCE);
		check(!debug_wp_check_write(0x0710, 1) && debug_wp_at(0)->hits == 0,
		      "ignore: a filtered value is not counted");
		check(!debug_wp_check_write(0x0710, 7), "ignore: first match is counted");
		check(debug_wp_check_write(0x0710, 7), "ignore: second match fires");

		reset_all();
		const debug_wp_spec_t vr = { DEBUG_WP_SPACE_VRAM, 0x0400, 1, DEBUG_BANK_ANY, false, 0, 0,
		                             DEBUG_WP_ACCESS_WRITE, false, 1 };
		debug_wp_add_ex(&vr, DEBUG_OWNER_MCP);
		check(!debug_wp_check_vram(0x0400, 0, 0) && debug_wp_check_vram(0x0400, 0, 0),
		      "ignore: applies to VRAM");
		debug_wp_clear_all();
	}

	// ── Read watches, and what counts as a read ─────────────────────────────
	{
		reset_all();
		debug_wp_spec_t rd = { DEBUG_WP_SPACE_CPU, 0x0600, 2, DEBUG_BANK_ANY, false, 0, 0,
		                       DEBUG_WP_ACCESS_READ, true };

		check(debug_wp_add_ex(&rd, DEBUG_OWNER_MCP) == DEBUG_ADD_CREATED, "adds a read watch");
		check(debug_wp_armed_cpu_read == 1 && debug_wp_armed_cpu_write == 0,
		      "it arms the read path and leaves the write path alone");
		check(debug_wp_check_read(0x0601, 0x12, false), "a data read fires it");
		check(!debug_wp_check_read(0x0601, 0x12, true), "an instruction fetch does not");
		check(!debug_wp_check_write(0x0601, 0x12), "a write does not");
		check(!debug_wp_check_read(0x0602, 0x12, false), "a read past the range does not");

		uint8_t access = 0;
		bool    fetch  = true;
		check(debug_wp_last_access_kind(&access, &fetch) && access == DEBUG_WP_ACCESS_READ && !fetch,
		      "the hit records that it was a data read");
		check(debug_wp_fires_on_ex(DEBUG_WP_SPACE_CPU, DEBUG_WP_ACCESS_READ, false, 0x0600,
		                           DEBUG_BANK_ANY, 0x0601, 0x12),
		      "the stop can be matched to the watch");
		check(!debug_wp_fires_on_ex(DEBUG_WP_SPACE_CPU, DEBUG_WP_ACCESS_WRITE, false, 0x0600,
		                            DEBUG_BANK_ANY, 0x0601, 0x12),
		      "but not as a write");
		check(debug_wp_add_for(0x0600, 2, DEBUG_BANK_ANY, DEBUG_OWNER_CLI) == DEBUG_ADD_CONFLICT,
		      "a write owner cannot share a read-only watch");
		debug_wp_spec_t both = rd;
		both.access = DEBUG_WP_ACCESS_BOTH;
		check(debug_wp_add_ex(&both, DEBUG_OWNER_UI) == DEBUG_ADD_CONFLICT,
		      "nor can a different access");
		debug_wp_spec_t with_fetch = rd;
		with_fetch.exclude_fetch = false;
		check(debug_wp_add_ex(&with_fetch, DEBUG_OWNER_UI) == DEBUG_ADD_CONFLICT,
		      "nor a different fetch rule");

		debug_wp_clear_all();
		with_fetch.addr = 0x0700;
		with_fetch.access = DEBUG_WP_ACCESS_BOTH;
		debug_wp_add_ex(&with_fetch, DEBUG_OWNER_MCP);
		check(debug_wp_armed_cpu_read == 1 && debug_wp_armed_cpu_write == 1, "read-write arms both paths");
		check(debug_wp_check_read(0x0700, 0, true), "with fetches included, a fetch fires");
		check(debug_wp_check_write(0x0700, 0), "and so does a write");

		debug_wp_spec_t vram = { DEBUG_WP_SPACE_VRAM, 0x0800, 1, DEBUG_BANK_ANY, false, 0, 0,
		                         DEBUG_WP_ACCESS_READ, true };
		debug_wp_add_ex(&vram, DEBUG_OWNER_MCP);
		const int at = debug_wp_find(DEBUG_WP_SPACE_VRAM, 0x0800, DEBUG_BANK_ANY);
		check(at >= 0 && debug_wp_at(at)->access == DEBUG_WP_ACCESS_WRITE, "a VRAM watch is write only");
		debug_wp_clear_all();
		check(debug_wp_armed_cpu_read == 0, "clearing resets the read count");
	}

	// ── One bank-selector rule, for breakpoints and watchpoints alike ───────
	// A bank selector means ANY or one bank; it is normalised away outside a banked window.
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

		// Below the banked window a stored bank neither blocks firing nor adds an entry.
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

		// The same address in two banks: the bank is part of a watchpoint's identity.
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
	// A/X/Y are 8 or 16 bits by CPU mode; SP and a memory word are 16, P and a memory byte 8.
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
	// A breakpoint's address is (pc, program bank, bank selector).
	{
		// -- Unbanked low memory: neither bank means anything.
		reset_all();
		// Named with a specific bank: an ANY selector matches before the bank rule is consulted.
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
		// Added with a specific bank, not ANY, which short-circuits before the bank rule.
		debug_bp_add(bp_at(0xA000, 1, 5));
		bool gs_ok = true;
		for (int b = 0; b < 8; b++) {
			g_ram_bank = (uint8_t)b;
			if (!debug_bp_is_set(0xA000, 1))
				gs_ok = false;
		}
		check(gs_ok, "in a Gen2 program bank the RAM bank does not apply");

		// The shared entry point, called directly.
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

		// Watchpoint addresses are bank 0; lookup normalises so named-bank removal hits.
		reset_all();
		debug_wp_add(0x0400, 1, 7);       // bank named for an unbanked address
		check(debug_wp_remove(0x0400, DEBUG_BANK_ANY),
		      "an unbanked watchpoint can be removed without naming a bank");
		reset_all();
		debug_wp_add(0x0400, 1, DEBUG_BANK_ANY);
		check(debug_wp_remove(0x0400, 7),
		      "and can be removed while naming one, since it means nothing there");

		// A watchpoint's bank is judged at the range start; the selector moves at $C000.
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
	// Five things can want one address: -bp, F9 and three kinds of DAP breakpoint.
	{
		// -- Two owners, one address. The first to let go must not disarm the other.
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

		// -- ...and when the last owner goes, so does the entry.
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

		// -- Asking twice as the same owner is idempotent; one clear retires it.
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

		// -- A -bp breakpoint survives a DAP session with its condition and hit count.
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

		// -- A client re-sending its list keeps the count.
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

		// -- Stale registry, direction 1: teardown must not delete the user's own entry.
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

		// Direction 2: a cached foreign owner must not survive the user's delete.
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

		// -- The human's delete is authoritative: F9 on a client's breakpoint removes it.
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

		// -- Ownership keys are normalised: a bank on an unbanked address makes no entry.
		reset_all();
		debug_bp_add_for(bp_at(0x0801, 0, 7), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_has_owner(0x0801, 0, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE),
		      "an owner recorded with a meaningless bank is found without one");
		check(debug_bp_remove_for(0x0801, 0, DEBUG_BANK_ANY, DEBUG_OWNER_DAP_SOURCE),
		      "and can let go without naming it");
		check(numBreakpoints == 0, "leaving nothing behind");

		// -- Restate pattern: clear the owner and re-assert the survivors.
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
	// A disabled breakpoint keeps its place, condition and count.
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

		// Re-sending a disabled breakpoint arms it.
		debug_bp_set_enabled(0x2000, 0, -1, false);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_is_enabled(0x2000, 0, -1), "re-adding a disabled breakpoint re-arms it");

		check(!debug_bp_set_enabled(0x9999, 0, -1, false),
		      "enabling something that is not there reports so");

		reset_all();
	}

	// ── Enable state per owner ──────────────────────────────────────────────
	{
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_MCP);
		check(debug_bp_set_enabled_for(0x2000, 0, -1, DEBUG_OWNER_MCP, false), "an owner turns off its claim");
		check(!debug_bp_is_enabled(0x2000, 0, -1), "the only claim off: the address is off");
		check(!debug_bp_set_enabled_for(0x2000, 0, -1, DEBUG_OWNER_UI, true), "a non-holder changes nothing");
		check(!debug_bp_is_enabled(0x2000, 0, -1), "and it stays off");

		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_is_set(0x2000, 0), "a second owner's claim arms the address");
		debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE);
		check(!debug_bp_is_enabled(0x2000, 0, -1), "removing it restores the first owner's off");

		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
		check(!debug_bp_is_enabled(0x2000, 0, -1), "so does clearing that owner");

		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		debug_bp_clear_owner(DEBUG_OWNER_MCP);
		check(debug_bp_is_enabled(0x2000, 0, -1), "an off claim leaving does not disturb a live one");
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_MCP);
		check(debug_bp_set_enabled_for(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE, false) &&
		      debug_bp_is_enabled(0x2000, 0, -1), "a returning owner's claim starts on");

		debug_bp_set_enabled(0x2000, 0, -1, false);
		check(!debug_bp_is_enabled(0x2000, 0, -1), "the human's switch turns off every holder");
		debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE);
		check(!debug_bp_is_enabled(0x2000, 0, -1), "and the survivor stays off");
		debug_bp_set_enabled(0x2000, 0, -1, true);
		check(debug_bp_is_enabled(0x2000, 0, -1), "and turns every holder on");
		reset_all();
	}

	// An off claim neither stops nor logs beside a live one.
	{
		reset_all();
		debug_bp_log_fire = log_fire;
		memset(g_log_fired, 0, sizeof g_log_fired);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_MCP);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_set_log(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE, &g_log_token[0]), "a logger is set");
		debug_bp_set_enabled_for(0x2000, 0, -1, DEBUG_OWNER_MCP, false);
		check(!debug_bp_on_arrival(0x2000, 0), "an off plain claim does not stop beside a live logger");
		check(g_log_fired[0] == 1, "and the logger fires");

		debug_bp_set_enabled_for(0x2000, 0, -1, DEBUG_OWNER_MCP, true);
		debug_bp_set_enabled_for(0x2000, 0, -1, DEBUG_OWNER_DAP_SOURCE, false);
		check(debug_bp_on_arrival(0x2000, 0), "a live plain claim stops");
		check(g_log_fired[0] == 1, "and an off logger stays quiet");
		debug_bp_log_fire = NULL;
		reset_all();
	}

	// ── Dump breakpoints and other owners ───────────────────────────────────
	{
		const uint32_t addr = 0x10, len = 2;
		const int      rb   = -1;
		debug_owner_t  who  = DEBUG_OWNER_UI;
		uint32_t       writes = 0;
		bool           full = true, stopped;

		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_MCP);
		FILE *f = tmpfile();
		check(f && debug_bp_set_dump_file(0x2000, 0, -1, DEBUG_OWNER_MCP, f, 0, 1, &addr, &len, &rb),
		      "a dump can be set");
		stopped = debug_bp_on_arrival(0x2000, 0);
		check(!stopped, "a dump alone runs on");
		check(debug_bp_dump_writes(0x2000, 0, -1) == 1, "and records the hit");
		check(debug_bp_dump_info(0x2000, 0, -1, &who, &writes, &full) && who == DEBUG_OWNER_MCP &&
		          writes == 1 && !full,
		      "and reports its owner and count");

		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI);
		stopped = debug_bp_on_arrival(0x2000, 0);
		check(stopped, "another owner's plain breakpoint still stops");
		check(debug_bp_dump_writes(0x2000, 0, -1) == 2, "and the dump still records that hit");

		FILE *g = tmpfile();
		check(g && !debug_bp_set_dump_file(0x2000, 0, -1, DEBUG_OWNER_CLI, g, 0, 1, &addr, &len, &rb),
		      "a second owner's dump is refused");
		if (g)
			fclose(g);
		debug_bp_clear_dump_for(0x2000, 0, -1, DEBUG_OWNER_CLI);
		check(debug_bp_dump_info(0x2000, 0, -1, NULL, NULL, NULL), "only its owner clears a dump");

		debug_bp_remove_for(0x2000, 0, -1, DEBUG_OWNER_MCP);
		check(!debug_bp_dump_info(0x2000, 0, -1, NULL, NULL, NULL), "a dump goes with its owner");
		check(debug_bp_on_arrival(0x2000, 0), "and the other owner's breakpoint stops as plain");

		// A later entry's dump records the arrival an earlier entry stops on.
		reset_all();
		g_ram_bank = 5;
		debug_bp_add_for(bp_at(0xA000, 0, DEBUG_BANK_ANY), DEBUG_OWNER_CLI);
		debug_bp_add_for(bp_at(0xA000, 0, 5), DEBUG_OWNER_MCP);
		f = tmpfile();
		debug_bp_set_dump_file(0xA000, 0, 5, DEBUG_OWNER_MCP, f, 0, 1, &addr, &len, &rb);
		stopped = debug_bp_on_arrival(0xA000, 0);
		check(stopped, "an overlapping plain entry stops");
		check(debug_bp_dump_writes(0xA000, 0, 5) == 1, "and the dump in the later entry records");
		g_ram_bank = 0;

		// A record is 4 bytes of index plus the regions: two of 6 fit in 15.
		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_MCP);
		f = tmpfile();
		debug_bp_set_dump_file(0x2000, 0, -1, DEBUG_OWNER_MCP, f, 15, 1, &addr, &len, &rb);
		for (int i = 0; i < 3; i++)
			debug_bp_on_arrival(0x2000, 0);
		debug_bp_dump_info(0x2000, 0, -1, NULL, &writes, &full);
		check(writes == 2 && full, "a dump stops at its size limit and says so");

		// A delete, and a whole-owner clear, close it too.
		debug_bp_delete(0x2000, 0, -1);
		check(!debug_bp_dump_info(0x2000, 0, -1, NULL, NULL, NULL), "deleting the entry closes it");
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_UI);
		check(debug_bp_on_arrival(0x2000, 0), "and a new breakpoint there is plain");

		reset_all();
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_DAP_INSTRUCTION);
		debug_bp_add_for(bp_at(0x2000, 0, -1), DEBUG_OWNER_CLI);
		f = tmpfile();
		debug_bp_set_dump_file(0x2000, 0, -1, DEBUG_OWNER_DAP_INSTRUCTION, f, 0, 1, &addr, &len, &rb);
		debug_bp_clear_owner(DEBUG_OWNER_DAP_INSTRUCTION);
		check(!debug_bp_dump_info(0x2000, 0, -1, NULL, NULL, NULL), "clearing an owner closes its dump");

		reset_all();
	}

	// ── Watchpoint ownership ────────────────────────────────────────────────
	// -wp, the debugger and a DAP client can all want one address.
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

		// A full table is distinguishable from a duplicate.
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

	// ── Address references: which field a bank byte lands in ────────────────
	// A reference `pc | bank << 16` selects the window bank, not the program bank, off a Gen2.
	{
		reset_all();
		is_gen2 = false;
		struct breakpoint bp;

		check(debug_bp_from_ref(0xA555, DEBUG_BANK_ANY, 0, &bp)
		          && bp.pc == 0xA555 && bp.bank == 0 && bp.x16Bank == DEBUG_BANK_ANY,
		      "a bare address names no bank and matches whichever is mapped");

		// Off a Gen2 the program bank is zero, so a top byte names the window bank.
		check(debug_bp_from_ref(0x33A555, DEBUG_BANK_ANY, 0, &bp)
		          && bp.pc == 0xA555 && bp.bank == 0 && bp.x16Bank == 0x33,
		      "a packed bank byte is the window bank on a gen1, not the program bank");

		// "bb:aaaa", the form -bp takes, has to arrive at the same entry.
		struct breakpoint colon;
		check(debug_bp_from_ref(0xA555, 0x33, 0, &colon)
		          && colon.pc == bp.pc && colon.bank == bp.bank
		          && colon.x16Bank == bp.x16Bank,
		      "the bb:aaaa form and the packed form describe one breakpoint");

		// It fires: added through the real table, matched through the real rule.
		check(debug_bp_add(bp) == 0, "the resolved breakpoint goes into the table");
		g_ram_bank = 0x33;
		bool armed = debug_bp_is_set(0xA555, 0);
		g_ram_bank = 0x34;
		check(armed && !debug_bp_is_set(0xA555, 0),
		      "and fires in the bank it named, and only there");

		// Window bank in the program-bank field: gen1 is in bank 0, so nothing matches.
		reset_all();
		is_gen2 = false;
		debug_bp_add(bp_at(0xA555, 0x33, DEBUG_BANK_ANY));
		bool dead = true;
		for (int b = 0; b < 0x40; b++) {
			g_ram_bank = (uint8_t)b;
			if (debug_bp_is_set(0xA555, 0))
				dead = false;
		}
		check(dead, "a window bank left in the program-bank field can never match");

		reset_all();
		is_gen2 = false;

		// A bank selects nothing below $A000, so naming one is refused rather than widened to ANY.
		check(!debug_bp_from_ref(0x330555, DEBUG_BANK_ANY, 0, &bp),
		      "a bank packed onto an unbanked address is refused");
		check(!debug_bp_from_ref(0x0555, 0x33, 0, &bp),
		      "and so is one named alongside it");

		check(!debug_bp_from_ref(0x33A555, 0x34, 0, &bp),
		      "a bank given twice, two different ways, is refused");

		check(!debug_bp_from_ref(-1, DEBUG_BANK_ANY, 0, &bp)
		          && !debug_bp_from_ref(0x1000000, DEBUG_BANK_ANY, 0, &bp),
		      "a reference outside 24 bits is refused");

		// The offset is a displacement within the bank and must not carry.
		check(debug_bp_from_ref(0x33A555, DEBUG_BANK_ANY, 0x10, &bp)
		          && bp.pc == 0xA565 && bp.x16Bank == 0x33,
		      "an offset moves the address and leaves the bank alone");
		check(debug_bp_from_ref(0xFFFF, DEBUG_BANK_ANY, 1, &bp) && bp.pc == 0x0000,
		      "and wraps inside 16 bits rather than into the bank byte");

		// -- Gen2: there a top byte really is the program bank, which is what
		// the disassembly and the stack frames hand out.
		is_gen2 = true;
		check(debug_bp_from_ref(0x01A555, DEBUG_BANK_ANY, 0, &bp)
		          && bp.pc == 0xA555 && bp.bank == 1 && bp.x16Bank == DEBUG_BANK_ANY,
		      "on a Gen2 a packed bank byte is the program bank");
		check(debug_bp_from_ref(0xA555, 5, 0, &bp)
		          && bp.pc == 0xA555 && bp.bank == 0 && bp.x16Bank == 5,
		      "and program bank 0 there is still the windowed map");

		reset_all();
		is_gen2 = true;
	}

	// ── The PC map the per-instruction fast path trusts ─────────────────────
	// A clear bit skips the breakpoint test; every table change is checked against it.
	{
		reset_all();
		check(!debug_bp_maybe_at(0x8000), "map: empty table, empty map");

		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_CLI);
		check(debug_bp_maybe_at(0x8000), "map: an add sets its PC");
		check(!debug_bp_maybe_at(0x8001), "map: and only its PC");

		// Two entries at one PC under different banks: removing one must not
		// clear the bit the other still needs.
		debug_bp_add_for(bp_at(0xA000, 0, 3), DEBUG_OWNER_CLI);
		debug_bp_add_for(bp_at(0xA000, 0, 4), DEBUG_OWNER_CLI);
		debug_bp_remove_for(0xA000, 0, 3, DEBUG_OWNER_CLI);
		check(debug_bp_maybe_at(0xA000), "map: a PC shared by two banks survives removing one");
		debug_bp_remove_for(0xA000, 0, 4, DEBUG_OWNER_CLI);
		check(!debug_bp_maybe_at(0xA000), "map: and clears with the last");

		debug_bp_add_for(bp_at(0x9000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_maybe_at(0x9000), "map: owner-scoped add");
		check(debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE) == 1, "map: owner clear removes it");
		check(!debug_bp_maybe_at(0x9000), "map: and its bit");
		check(debug_bp_maybe_at(0x8000), "map: other owners' bits are untouched");

		// Growing the table past its first allocation keeps every bit.
		for (int i = 0; i < 40; i++)
			debug_bp_add_for(bp_at(0x1000 + i * 3, 0, -1), DEBUG_OWNER_CLI);
		bool all = true;
		for (int i = 0; i < 40; i++)
			all = all && debug_bp_maybe_at((uint16_t)(0x1000 + i * 3));
		check(all, "map: every entry is mapped after the table grows");

		debug_bp_clear_all();
		check(!debug_bp_maybe_at(0x8000) && !debug_bp_maybe_at(0x1000),
		      "map: clear-all empties it");
		reset_all();
	}

	// --- Logpoints -----------------------------------------------------------
	{
		debug_bp_log_fire = log_fire;
		debug_bp_log_free = log_free;
		memset(g_log_fired, 0, sizeof g_log_fired);
		memset(g_log_freed, 0, sizeof g_log_freed);
		void *const t0 = &g_log_token[0], *const t1 = &g_log_token[1], *const t2 = &g_log_token[2];

		check(!debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_MCP, t0),
		      "log: refused where the owner holds no breakpoint");
		check(g_log_freed[0] == 0, "log: a refused message is left with the caller");

		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_MCP);
		check(debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_MCP, t0), "log: set");
		check(debug_bp_log_for(0x8000, 0, -1, DEBUG_OWNER_MCP) == t0, "log: reads back");
		check(!debug_bp_log_for(0x8000, 0, -1, DEBUG_OWNER_UI), "log: is per owner");
		check(debug_bp_quiet_owners(0x8000, 0, -1) == (1u << DEBUG_OWNER_MCP), "log: owner is quiet");

		check(!debug_bp_on_arrival(0x8000, 0), "log: does not stop its own owner");
		check(g_log_fired[0] == 1 && g_log_owner == DEBUG_OWNER_MCP && g_log_pc == 0x8000,
		      "log: fires with its owner and address");
		check(debug_bp_get_hits(0x8000, 0, -1) == 1, "log: counts the hit");

		// Another owner's plain breakpoint at the same address still stops.
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_DAP_SOURCE);
		check(debug_bp_on_arrival(0x8000, 0), "log: another owner's plain breakpoint stops");
		check(g_log_fired[0] == 2, "log: and the logpoint still fires");

		// Both owners logging: nobody asked for a stop.
		check(debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_DAP_SOURCE, t1), "log: second owner");
		check(!debug_bp_on_arrival(0x8000, 0), "log: two logpoints do not stop");
		check(g_log_fired[0] == 3 && g_log_fired[1] == 1, "log: each owner's fires");

		// A third, plain owner stops it again and a release leaves the rest.
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_CLI);
		check(debug_bp_on_arrival(0x8000, 0), "log: a third owner's plain breakpoint stops");
		debug_bp_remove_for(0x8000, 0, -1, DEBUG_OWNER_CLI);
		check(!debug_bp_on_arrival(0x8000, 0), "log: quiet again once it lets go");

		// Replacing frees the old message; removing a claim frees that owner's.
		check(debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_MCP, t2), "log: replace");
		check(g_log_freed[0] == 1, "log: the replaced message is released");
		debug_bp_remove_for(0x8000, 0, -1, DEBUG_OWNER_MCP);
		check(g_log_freed[2] == 1, "log: released with its owner's claim");
		check(g_log_freed[1] == 0, "log: the other owner's stays");
		check(!debug_bp_on_arrival(0x8000, 0), "log: the survivor still logs");
		check(g_log_fired[1] == 4 && g_log_fired[2] == 0, "log: and only the survivor");

		debug_bp_clear_owner(DEBUG_OWNER_DAP_SOURCE);
		check(g_log_freed[1] == 1, "log: released by clearing the owner");
		check(!debug_bp_is_set(0x8000, 0), "log: nothing left armed");
		reset_all();

		// The condition and ignore budget gate it like any breakpoint.
		memset(g_log_fired, 0, sizeof g_log_fired);
		memset(g_log_freed, 0, sizeof g_log_freed);
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_MCP);
		debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_MCP, t0);
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 7);
		debug_bp_set_ignore(0x8000, 0, -1, 1);
		regs.c = 1;
		check(!debug_bp_on_arrival(0x8000, 0) && g_log_fired[0] == 0, "log: gated by its condition");
		regs.c = 7;
		check(!debug_bp_on_arrival(0x8000, 0) && g_log_fired[0] == 0, "log: gated by its ignore count");
		check(!debug_bp_on_arrival(0x8000, 0) && g_log_fired[0] == 1, "log: fires past both");
		debug_bp_set_enabled(0x8000, 0, -1, false);
		check(!debug_bp_on_arrival(0x8000, 0) && g_log_fired[0] == 1, "log: a disabled one is silent");
		debug_bp_set_enabled(0x8000, 0, -1, true);

		debug_bp_forget(0x8000, 0, -1);
		check(g_log_freed[0] == 1, "log: released by forget");
		check(!debug_bp_log_for(0x8000, 0, -1, DEBUG_OWNER_MCP), "log: and gone");
		reset_all();

		// Delete and clear-all release everyone's.
		memset(g_log_freed, 0, sizeof g_log_freed);
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_MCP);
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_UI);
		debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_MCP, t0);
		debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_UI, t1);
		check(debug_bp_delete(0x8000, 0, -1), "log: delete");
		check(g_log_freed[0] == 1 && g_log_freed[1] == 1, "log: delete releases every owner's");
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_MCP);
		debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_MCP, t2);
		debug_bp_clear_all();
		check(g_log_freed[2] == 1, "log: clear-all releases it");

		// One slot of the table, then the pointer is gone: no call through NULL.
		debug_bp_log_fire = NULL;
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_MCP);
		debug_bp_set_log(0x8000, 0, -1, DEBUG_OWNER_MCP, t0);
		check(!debug_bp_on_arrival(0x8000, 0), "log: quiet with no hook installed");
		debug_bp_log_fire = log_fire;
		memset(g_log_freed, 0, sizeof g_log_freed);
		debug_core_free();
		check(g_log_freed[0] == 1, "log: released at shutdown");
		debug_bp_log_fire = NULL;
		debug_bp_log_free = NULL;
		reset_all();
	}

	// ---- Expression conditions --------------------------------------------
	{
		debug_cond_test = cond_test;
		debug_cond_same = cond_same;
		debug_cond_free = cond_free;
		char *const c0 = &g_cond_token[0], *const c1 = &g_cond_token[1], *const c2 = &g_cond_token[2];
		memset(g_cond_freed, 0, sizeof g_cond_freed);

		// Breakpoints.
		debug_bp_add_for(bp_at(0x8000, 0, -1), DEBUG_OWNER_MCP);
		debug_bp_set_expr(0x8000, 0, -1, c0);
		check(debug_bp_expr(0x8000, 0, -1) == c0, "expr: stored on the breakpoint");
		g_cond_answer = false;
		check(!debug_bp_on_arrival(0x8000, 0), "expr: false does not stop");
		check(!g_cond_had_access, "expr: an exec test has no access");
		check(debug_bp_get_hits(0x8000, 0, -1) == 0, "expr: false is not a hit");
		g_cond_answer = true;
		check(debug_bp_on_arrival(0x8000, 0), "expr: true stops");
		check(debug_bp_get_hits(0x8000, 0, -1) == 1, "expr: true is a hit");
		debug_bp_set_ignore(0x8000, 0, -1, 2);
		check(!debug_bp_on_arrival(0x8000, 0), "expr: the ignore budget still applies");
		debug_bp_set_ignore(0x8000, 0, -1, 0);

		debug_bp_set_expr(0x8000, 0, -1, c1);
		check(g_cond_freed[0] == 1 && debug_bp_expr(0x8000, 0, -1) == c1, "expr: replaced and released");
		debug_bp_set_condition(0x8000, 0, -1, BPOPERAND_A, 0, BPCMP_EQ, 7);
		check(g_cond_freed[1] == 1 && !debug_bp_expr(0x8000, 0, -1), "expr: a simple test replaces it");
		int has = 0, operand = 0, op = 0;
		uint16_t oaddr = 0;
		uint32_t value = 0, ignore = 0;
		debug_bp_set_expr(0x8000, 0, -1, c2);
		debug_bp_get_condition(0x8000, 0, -1, &has, &operand, &oaddr, &op, &value, &ignore);
		check(!has, "expr: replaces a simple test");
		debug_bp_clear_condition(0x8000, 0, -1);
		check(g_cond_freed[2] == 1 && !debug_bp_expr(0x8000, 0, -1), "expr: clear releases it");
		debug_bp_set_expr(0x8000, 0, -1, c0);
		debug_bp_forget(0x8000, 0, -1);
		check(g_cond_freed[0] == 2, "expr: forget releases it");
		reset_all();

		// Watches.
		memset(g_cond_freed, 0, sizeof g_cond_freed);
		debug_wp_spec_t spec = { DEBUG_WP_SPACE_CPU, 0x0700, 4, DEBUG_BANK_ANY, false, 0, 0,
		                         DEBUG_WP_ACCESS_BOTH, true, 0, c0 };
		check(debug_wp_add_ex(&spec, DEBUG_OWNER_MCP) == DEBUG_ADD_CREATED, "expr: watch created");
		check(debug_wp_at(0)->expr == c0, "expr: the watch took it");
		check(debug_wp_add_ex(&spec, DEBUG_OWNER_UI) == DEBUG_ADD_EXISTED, "expr: the same one shares");
		debug_wp_spec_t other = spec;
		other.expr = c1;
		check(debug_wp_add_ex(&other, DEBUG_OWNER_CLI) == DEBUG_ADD_CONFLICT, "expr: another conflicts");
		other.expr = NULL;
		check(debug_wp_add_ex(&other, DEBUG_OWNER_CLI) == DEBUG_ADD_CONFLICT, "expr: none conflicts");
		check(g_cond_freed[0] == 0 && g_cond_freed[1] == 0, "expr: a refused one is the caller's");

		g_cond_answer = false;
		check(!debug_wp_check_write(0x0701, 0x12), "expr: false does not fire");
		check(debug_wp_at(0)->hits == 0, "expr: false is not a hit");
		g_cond_answer = true;
		check(debug_wp_check_write(0x0702, 0x34), "expr: true fires");
		check(g_cond_had_access && g_cond_access.space == DEBUG_WP_SPACE_CPU &&
		      g_cond_access.access == DEBUG_WP_ACCESS_WRITE && !g_cond_access.fetch &&
		      g_cond_access.addr == 0x0702 && g_cond_access.value == 0x34, "expr: a write's access");
		check(debug_wp_check_read(0x0703, 0x56, false), "expr: fires on a read");
		check(g_cond_access.access == DEBUG_WP_ACCESS_READ && g_cond_access.addr == 0x0703 &&
		      g_cond_access.value == 0x56, "expr: a read's access");

		// Which watch a stop names is decided at the access.
		debug_wp_clear_all();
		check(g_cond_freed[0] == 1, "expr: clear-all releases it");
		debug_wp_spec_t first = { DEBUG_WP_SPACE_CPU, 0x0710, 2, DEBUG_BANK_ANY, false, 0, 0,
		                          DEBUG_WP_ACCESS_WRITE, true, 0, c0 };
		debug_wp_spec_t second = { DEBUG_WP_SPACE_CPU, 0x0711, 1, DEBUG_BANK_ANY, false, 0, 0,
		                           DEBUG_WP_ACCESS_WRITE, true, 0, c1 };
		debug_wp_add_ex(&first, DEBUG_OWNER_MCP);
		debug_wp_add_ex(&second, DEBUG_OWNER_MCP);
		g_cond_answer = true;
		check(debug_wp_check_write(0x0710, 1), "expr: the first write fires");
		debug_wp_latch_stop();
		check(debug_wp_check_write(0x0711, 2), "expr: the same instruction's second write fires");
		const int tests = g_cond_tests;
		g_cond_answer = false;
		check(debug_wp_fires_on_ex(DEBUG_WP_SPACE_CPU, DEBUG_WP_ACCESS_WRITE, false, 0x0710,
		                           DEBUG_BANK_ANY, 0x0710, 1), "expr: the stop names the first");
		check(!debug_wp_fires_on_ex(DEBUG_WP_SPACE_CPU, DEBUG_WP_ACCESS_WRITE, false, 0x0711,
		                            DEBUG_BANK_ANY, 0x0711, 2), "expr: and not the second");
		check(debug_wp_fires_on(0x0710, DEBUG_BANK_ANY, 0x0710, 1), "expr: fires_on agrees");
		check(g_cond_tests == tests, "expr: not judged again after the stop");

		check(debug_wp_set_value_at(0, BPCMP_EQ, 1) && g_cond_freed[0] == 2 && !debug_wp_at(0)->expr,
		      "expr: a value filter replaces it");
		check(debug_wp_set_expr_at(0, c2) && !debug_wp_at(0)->has_value && debug_wp_at(0)->expr == c2,
		      "expr: replaces a value filter");
		check(!debug_wp_set_expr_at(5, c0), "expr: no such watch");
		check(debug_wp_clear_value_at(0) && g_cond_freed[2] == 1 && !debug_wp_at(0)->expr,
		      "expr: clearing the filter releases it");
		debug_wp_delete_at(0);
		check(debug_wp_at(0)->expr == c1, "expr: a later entry keeps its own");
		debug_wp_clear_owner(DEBUG_OWNER_MCP);
		check(g_cond_freed[1] == 1, "expr: released with its last owner");

		debug_wp_spec_t last = spec;
		last.expr = c2;
		debug_wp_add_ex(&last, DEBUG_OWNER_MCP);
		debug_core_free();
		check(g_cond_freed[2] == 2, "expr: released at shutdown");

		debug_cond_test = NULL;
		debug_cond_same = NULL;
		debug_cond_free = NULL;
		reset_all();
	}

	// ---- Stop filters -----------------------------------------------------
	{
		check(debug_stop_filters() == 0, "stop: nothing armed by default");
		check(debug_stop_on_entry(DEBUG_STOP_IRQ, false, false, false) == DEBUG_ENTRY_NO_STOP,
		      "stop: an entry with nothing armed runs on");

		debug_stop_filter_set(DEBUG_OWNER_DAP_SOURCE, DEBUG_STOP_IRQ);
		debug_stop_filter_set(DEBUG_OWNER_MCP, DEBUG_STOP_BRK);
		check(debug_stop_filters() == (DEBUG_STOP_IRQ | DEBUG_STOP_BRK), "stop: owners are ORed");
		check(debug_stop_filter_get(DEBUG_OWNER_MCP) == DEBUG_STOP_BRK, "stop: each owner keeps its own");
		check(debug_stop_on_entry(DEBUG_STOP_BRK, false, false, false) == DEBUG_ENTRY_EXCEPTION,
		      "stop: an armed kind is an exception");
		check(debug_stop_on_entry(DEBUG_STOP_NMI, false, false, false) == DEBUG_ENTRY_NO_STOP,
		      "stop: an unarmed kind runs on");
		check(debug_stop_on_entry(DEBUG_STOP_IRQ, true, false, false) == DEBUG_ENTRY_NO_STOP,
		      "stop: an existing halt keeps its reason");
		check(debug_stop_on_entry(DEBUG_STOP_IRQ, false, true, false) == DEBUG_ENTRY_EXCEPTION,
		      "stop: a filter outranks the saved setting");

		debug_stop_filter_set(DEBUG_OWNER_DAP_SOURCE, 0);
		check(debug_stop_filters() == DEBUG_STOP_BRK, "stop: clearing one leaves the other");
		debug_stop_filter_set(DEBUG_OWNER_MCP, 0);
		check(debug_stop_filters() == 0, "stop: all released");

		check(debug_stop_on_entry(DEBUG_STOP_IRQ, false, true, false) == DEBUG_ENTRY_INTERRUPT,
		      "stop: empty filters leave the saved setting in force");
		check(debug_stop_on_entry(1u << 0x4, false, true, false) == DEBUG_ENTRY_INTERRUPT,
		      "stop: the saved setting covers COP");
		check(debug_stop_on_entry(DEBUG_STOP_IRQ, false, false, true) == DEBUG_ENTRY_INTERRUPT,
		      "stop: a followed step still stops");
		check(debug_stop_on_entry(0, false, true, true) == DEBUG_ENTRY_NO_STOP,
		      "stop: no entry, no stop");

		debug_stop_filter_set(DEBUG_OWNER_COUNT, DEBUG_STOP_IRQ);
		check(debug_stop_filters() == 0, "stop: an invalid owner is ignored");
		debug_stop_filter_set(DEBUG_OWNER_MCP, 0xFFFF);
		check(debug_stop_filter_get(DEBUG_OWNER_MCP) == DEBUG_STOP_ALL, "stop: unknown bits are dropped");
		debug_stop_filter_set(DEBUG_OWNER_MCP, 0);
	}

	debug_core_free();

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
