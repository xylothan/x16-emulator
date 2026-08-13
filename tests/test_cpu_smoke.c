// Checks that the CPU fixture itself works, before any conformance scenario
// relies on it.
//
// These are deliberately the least interesting assertions in the CPU track:
// the reset vector is honoured, an instruction executes, memory reads and
// writes reach the flat array, cycles accumulate, and a reset really does
// clear the previous scenario. If any of that is wrong then every later CPU
// result is meaningless, so it is worth stating plainly and separately.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#define PROG 0x0200

static void
test_reset_vector(void)
{
	cpu_reset_to(CPU_65C02, PROG);
	check_eq(regs.pc, PROG, "reset takes PC from the vector at $FFFC");
	check_eq(clockticks6502, 0, "cycle counter starts at zero");
}

static void
test_executes_an_instruction(void)
{
	// LDA #$42
	const uint8_t prog[] = { 0xA9, 0x42 };

	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(1);

	check_eq(regs.a, 0x42, "LDA #$42 loads the accumulator");
	check_eq(regs.pc, PROG + 2, "PC advances past a two-byte instruction");
	check_eq(cpu_last_cycles(), 2, "LDA immediate costs two cycles");
}

static void
test_memory_round_trip(void)
{
	// LDA #$7E ; STA $0400
	const uint8_t prog[] = { 0xA9, 0x7E, 0x8D, 0x00, 0x04 };

	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(2);

	check_eq(cpu_mem[0x0400], 0x7E, "a store reaches the fixture's memory");

	// And the other direction: a seeded byte is visible to a load.
	cpu_reset_to(CPU_65C02, PROG);
	cpu_mem[0x0400] = 0x39;
	const uint8_t load[] = { 0xAD, 0x00, 0x04 }; // LDA $0400
	cpu_load(PROG, load, sizeof load);
	cpu_steps(1);

	check_eq(regs.a, 0x39, "a load sees a byte seeded by the test");
}

static void
test_reset_isolates_scenarios(void)
{
	const uint8_t prog[] = { 0xA9, 0x11, 0x8D, 0x00, 0x04 };

	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(2);
	check_eq(cpu_mem[0x0400], 0x11, "first scenario wrote to memory");

	cpu_reset_to(CPU_65C02, PROG);
	check_eq(cpu_mem[0x0400], 0x00, "reset clears memory from the last scenario");
	check_eq(cpu_mem[PROG], 0x00, "reset clears the previous program");
	check_eq(cpu_stop_count, 0, "reset clears the stop counter");
}

// Flags leak between scenarios unless the fixture clears them: reset6502() only
// ever ORs bits into the status register, so N, V, Z and C survive it. A test
// asserting "this instruction did not touch V" would otherwise pass or fail
// depending on which scenario ran before it.
static void
test_reset_clears_flags(void)
{
	// SEC ; LDA #$FF ; ADC #$FF -- leaves C, N and V set.
	const uint8_t dirty[] = { 0x38, 0xA9, 0xFF, 0x69, 0xFF };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, dirty, sizeof dirty);
	cpu_steps(3);
	check(regs.status & FLAG_CARRY, "the dirtying scenario left C set");
	check(regs.status & FLAG_SIGN, "and N set");

	cpu_reset_to(CPU_65C02, PROG);
	check(!(regs.status & FLAG_CARRY), "reset clears C");
	check(!(regs.status & FLAG_SIGN), "reset clears N");
	check(!(regs.status & FLAG_OVERFLOW), "reset clears V");
	check(!(regs.status & FLAG_ZERO), "reset clears Z");
	check(regs.status & FLAG_INTERRUPT, "reset leaves interrupts disabled");
	check(!(regs.status & FLAG_DECIMAL), "reset clears decimal mode");
}

// penaltyd is set by the 65816 direct-page addressing modes when the direct
// page is not page-aligned. It used not to be cleared per instruction, so one
// misaligned access added a cycle to every instruction thereafter -- which
// would show up as a scenario that passes alone and fails after another.
static void
test_direct_page_penalty_does_not_persist(void)
{
	// LDA #$01 ; XBA ; LDA #$34 ; TCD  -- direct page to $0134, misaligned.
	// Then LDA $10 (direct page, pays the penalty) and NOP (must not).
	const uint8_t prog[] = {
		0xA9, 0x01, 0xEB, 0xA9, 0x34, 0x5B, // set D = $0134
		0xA5, 0x10,                         // LDA $10
		0xEA,                               // NOP
	};
	cpu_reset_to(CPU_816_NATIVE_8, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(4);

	uint32_t dp_cost  = cpu_step(); // LDA direct page, misaligned
	uint32_t nop_cost = cpu_step(); // NOP

	check_eq(nop_cost, 2, "a NOP after a misaligned direct-page access still costs 2");
	check(dp_cost > nop_cost, "and the direct-page access itself paid the penalty");
}

static void
test_cycles_are_per_run(void)
{
	const uint8_t prog[] = { 0xA9, 0x01, 0xA9, 0x02 }; // LDA #$01 ; LDA #$02

	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);

	cpu_steps(1);
	check_eq(cpu_last_cycles(), 2, "cycles are counted for one step");
	cpu_steps(1);
	check_eq(cpu_last_cycles(), 2, "the count covers only the latest run");
	check_eq(clockticks6502, 4, "the total keeps accumulating");
}

static void
test_modes_select_register_width(void)
{
	// Naming and the width predicate are what the 65816 scenarios branch on,
	// so an error here would quietly mis-label every one of them.
	check(cpu_mode_is_16bit(CPU_816_NATIVE_16), "native 16-bit mode reports 16-bit");
	check(!cpu_mode_is_16bit(CPU_65C02), "65C02 does not report 16-bit");
	check(!cpu_mode_is_16bit(CPU_816_EMU), "emulation mode does not report 16-bit");

	cpu_reset_to(CPU_816_NATIVE_16, PROG);
	check_eq(regs.e, 0, "native mode clears the emulation bit");
	check_eq(regs.status & (FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH), 0,
	         "native 16-bit clears both width flags");

	cpu_reset_to(CPU_816_NATIVE_8, PROG);
	check_eq(regs.status & (FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH),
	         FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH,
	         "native 8-bit sets both width flags");

	cpu_reset_to(CPU_816_EMU, PROG);
	check_eq(regs.e, 1, "emulation mode leaves the emulation bit set");
}

int
main(void)
{
	test_reset_vector();
	test_executes_an_instruction();
	test_memory_round_trip();
	test_reset_isolates_scenarios();
	test_reset_clears_flags();
	test_direct_page_penalty_does_not_persist();
	test_cycles_are_per_run();
	test_modes_select_register_width();
	return x16_test_summary("cpu_smoke");
}
