// CPU conformance: decimal mode (BCD) arithmetic.
//
// Oracle: WDC W65C02S datasheet. Its 6502-vs-65C02 comparison table, p30, gives
// both of the differences this file rests on -- where the NMOS part has
// "Invalid N, V and Z flags", the CMOS part has "Valid flags" and "One
// additional cycle".
//
// This gets its own scenarios because it is the corner most often got wrong,
// and because those two differences matter to anything running on this machine:
//
//   * N and Z reflect the BCD result. On the NMOS part they were taken from
//     the binary intermediate value and were effectively meaningless.
//   * ADC and SBC take one cycle longer than in binary mode. That extra cycle
//     is real and observable, and the X16 ships a 65C02.
//
// Only valid BCD operands are used. The result of feeding the decimal adjust
// a non-decimal nibble is genuinely undefined, so asserting anything about it
// would be inventing a specification rather than testing one.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#define PROG 0x0200

// SED ; CLC/SEC ; LDA #a ; ADC #b
static void
bcd_add(uint8_t a, uint8_t b, bool carry_in)
{
	const uint8_t prog[] = { 0xF8, (uint8_t)(carry_in ? 0x38 : 0x18), 0xA9, a, 0x69, b };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(4);
}

// SED ; SEC/CLC ; LDA #a ; SBC #b
static void
bcd_sub(uint8_t a, uint8_t b, bool carry_in)
{
	const uint8_t prog[] = { 0xF8, (uint8_t)(carry_in ? 0x38 : 0x18), 0xA9, a, 0xE9, b };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(4);
}

static bool c(void) { return (regs.status & FLAG_CARRY) != 0; }
static bool z(void) { return (regs.status & FLAG_ZERO) != 0; }
static bool n(void) { return (regs.status & FLAG_SIGN) != 0; }

// ---- The decimal flag itself -----------------------------------------------

static void
test_decimal_flag(void)
{
	const uint8_t sed[] = { 0xF8 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, sed, sizeof sed);
	cpu_steps(1);
	check(regs.status & FLAG_DECIMAL, "SED sets the decimal flag");

	const uint8_t cld[] = { 0xF8, 0xD8 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, cld, sizeof cld);
	cpu_steps(2);
	check(!(regs.status & FLAG_DECIMAL), "CLD clears it again");
}

// ---- Addition --------------------------------------------------------------

static void
test_bcd_add(void)
{
	bcd_add(0x09, 0x01, false);
	check_eq(regs.a, 0x10, "9 + 1 = 10 in BCD");
	check(!c(), "and does not carry");

	bcd_add(0x15, 0x27, false);
	check_eq(regs.a, 0x42, "15 + 27 = 42 in BCD");

	bcd_add(0x99, 0x01, false);
	check_eq(regs.a, 0x00, "99 + 1 wraps to 00 in BCD");
	check(c(), "and carries out of the hundreds");

	bcd_add(0x50, 0x50, false);
	check_eq(regs.a, 0x00, "50 + 50 = 00 with a carry");
	check(c(), "50 + 50 carries");

	bcd_add(0x12, 0x34, true);
	check_eq(regs.a, 0x47, "a carry in adds one to the BCD sum");

	bcd_add(0x79, 0x14, false);
	check_eq(regs.a, 0x93, "79 + 14 = 93, adjusting the low nibble only");
}

// ---- Subtraction -----------------------------------------------------------

static void
test_bcd_sub(void)
{
	bcd_sub(0x42, 0x15, true);
	check_eq(regs.a, 0x27, "42 - 15 = 27 in BCD");
	check(c(), "and does not borrow");

	bcd_sub(0x50, 0x25, true);
	check_eq(regs.a, 0x25, "50 - 25 = 25 in BCD");

	bcd_sub(0x00, 0x01, true);
	check_eq(regs.a, 0x99, "00 - 1 wraps to 99 in BCD");
	check(!c(), "and borrows");

	bcd_sub(0x10, 0x01, true);
	check_eq(regs.a, 0x09, "10 - 1 = 09, borrowing across the nibble");

	bcd_sub(0x42, 0x15, false);
	check_eq(regs.a, 0x26, "a clear carry subtracts one more");
}

// ---- 65C02: N and Z describe the decimal result ----------------------------
// On the NMOS part these came from the binary intermediate and were useless.

static void
test_flags_are_valid(void)
{
	bcd_add(0x99, 0x01, false);
	check(z(), "a BCD result of 00 sets Z");
	check(!n(), "and is not negative");

	bcd_add(0x40, 0x40, false);
	check_eq(regs.a, 0x80, "40 + 40 = 80 in BCD");
	check(n(), "a BCD result with bit 7 set reports negative");
	check(!z(), "and is not zero");

	bcd_sub(0x01, 0x01, true);
	check(z(), "a BCD subtraction reaching 00 sets Z");
}

// ---- 65C02: decimal costs one extra cycle ----------------------------------

static void
test_decimal_timing(void)
{
	// SED ; CLC ; LDA #$12 ; ADC #$34
	const uint8_t dec[] = { 0xF8, 0x18, 0xA9, 0x12, 0x69, 0x34 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, dec, sizeof dec);
	cpu_steps(3);
	check_eq(cpu_step(), 3, "ADC immediate costs 3 cycles in decimal mode");

	// The same instruction in binary mode, for contrast.
	const uint8_t bin[] = { 0xD8, 0x18, 0xA9, 0x12, 0x69, 0x34 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, bin, sizeof bin);
	cpu_steps(3);
	check_eq(cpu_step(), 2, "and 2 cycles in binary mode");

	// SED ; SEC ; LDA #$42 ; SBC #$15
	const uint8_t sbc[] = { 0xF8, 0x38, 0xA9, 0x42, 0xE9, 0x15 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, sbc, sizeof sbc);
	cpu_steps(3);
	check_eq(cpu_step(), 3, "SBC immediate also costs 3 cycles in decimal mode");
}

int
main(void)
{
	test_decimal_flag();
	test_bcd_add();
	test_bcd_sub();
	test_flags_are_valid();
	test_decimal_timing();
	return x16_test_summary("cpu_decimal");
}
