// CPU conformance: ADC and SBC in binary mode, and the flags they produce.
//
// Oracle: WDC W65C02S datasheet.
//
// The overflow flag is the interesting part and the one most often got wrong.
// V is about signed arithmetic: it is set when two operands of the same sign
// produce a result of the other sign, which is the only case where the signed
// answer cannot fit in eight bits. Carry, by contrast, is the unsigned answer
// not fitting. The two are independent, so each combination is checked.
//
// Decimal mode is deliberately absent; it has its own scenarios.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#define PROG 0x0200

// CLC ; LDA #a ; ADC #b   (or SEC ; ... for carry in)
static void
add(uint8_t a, uint8_t b, bool carry_in)
{
	const uint8_t prog[] = { (uint8_t)(carry_in ? 0x38 : 0x18), 0xA9, a, 0x69, b };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(3);
}

// SEC ; LDA #a ; SBC #b   (carry set means "no borrow")
static void
sub(uint8_t a, uint8_t b, bool carry_in)
{
	const uint8_t prog[] = { (uint8_t)(carry_in ? 0x38 : 0x18), 0xA9, a, 0xE9, b };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(3);
}

static bool c(void) { return (regs.status & FLAG_CARRY) != 0; }
static bool v(void) { return (regs.status & FLAG_OVERFLOW) != 0; }
static bool z(void) { return (regs.status & FLAG_ZERO) != 0; }
static bool n(void) { return (regs.status & FLAG_SIGN) != 0; }

// ---- Addition --------------------------------------------------------------

static void
test_add_basic(void)
{
	add(0x01, 0x01, false);
	check_eq(regs.a, 0x02, "1 + 1 = 2");
	check(!c(), "1 + 1 does not carry");
	check(!v(), "1 + 1 does not overflow");

	add(0x01, 0x01, true);
	check_eq(regs.a, 0x03, "carry in is added");

	add(0xFF, 0x01, false);
	check_eq(regs.a, 0x00, "$FF + 1 wraps to zero");
	check(c(), "$FF + 1 carries");
	check(z(), "the wrapped result sets Z");

	add(0x7F, 0x01, false);
	check_eq(regs.a, 0x80, "$7F + 1 = $80");
	check(n(), "a result with bit 7 set is negative");
}

static void
test_add_overflow(void)
{
	// Same-signed operands crossing into the other sign: V set.
	add(0x50, 0x50, false); // 80 + 80 = 160, too big for a signed byte
	check_eq(regs.a, 0xA0, "$50 + $50 = $A0");
	check(v(), "positive + positive giving a negative sets V");
	check(!c(), "and it does not carry");

	add(0x90, 0x90, false); // -112 + -112 = -224, too small
	check_eq(regs.a, 0x20, "$90 + $90 = $20");
	check(v(), "negative + negative giving a positive sets V");
	check(c(), "and it does carry");

	// Opposite signs can never overflow: the result always fits.
	add(0x50, 0x90, false);
	check(!v(), "positive + negative never sets V");
	check(!c(), "$50 + $90 does not carry");

	add(0xD0, 0x50, false); // -48 + 80 = 32, fits
	check(!v(), "negative + positive never sets V");
	check(c(), "$D0 + $50 carries");
}

// ---- Subtraction -----------------------------------------------------------
// SBC subtracts the carry's complement, so carry set means "no borrow" and is
// the normal way to start a subtraction.

static void
test_sub_basic(void)
{
	sub(0x05, 0x03, true);
	check_eq(regs.a, 0x02, "5 - 3 = 2");
	check(c(), "a subtraction that does not borrow leaves C set");
	check(!v(), "5 - 3 does not overflow");

	sub(0x05, 0x05, true);
	check_eq(regs.a, 0x00, "5 - 5 = 0");
	check(z(), "an exactly zero result sets Z");
	check(c(), "and still does not borrow");

	sub(0x03, 0x05, true);
	check_eq(regs.a, 0xFE, "3 - 5 wraps to $FE");
	check(!c(), "a subtraction that borrows clears C");
	check(n(), "the wrapped result is negative");

	// Carry clear on entry costs an extra one: 5 - 3 - 1.
	sub(0x05, 0x03, false);
	check_eq(regs.a, 0x01, "a clear carry subtracts one more");
}

static void
test_sub_overflow(void)
{
	// Overflow in subtraction needs operands of opposite sign.
	sub(0x50, 0xB0, true); // 80 - (-80) = 160, too big
	check_eq(regs.a, 0xA0, "$50 - $B0 = $A0");
	check(v(), "positive - negative giving a negative sets V");

	sub(0xD0, 0x70, true); // -48 - 112 = -160, too small
	check_eq(regs.a, 0x60, "$D0 - $70 = $60");
	check(v(), "negative - positive giving a positive sets V");

	sub(0x50, 0x30, true);
	check(!v(), "same-signed operands do not overflow");
}

// ---- Timing ----------------------------------------------------------------

static void
test_timing(void)
{
	const uint8_t prog[] = { 0x18, 0xA9, 0x10, 0x69, 0x20 }; // CLC ; LDA # ; ADC #
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, sizeof prog);
	cpu_steps(2);
	check_eq(cpu_step(), 2, "ADC immediate costs 2 cycles");

	// CLC ; LDA #$10 ; ADC $30
	const uint8_t zp[] = { 0x18, 0xA9, 0x10, 0x65, 0x30 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, zp, sizeof zp);
	cpu_mem[0x0030] = 0x05;
	cpu_steps(2);
	check_eq(cpu_step(), 3, "ADC zero page costs 3 cycles");
	check_eq(regs.a, 0x15, "ADC zero page adds the addressed byte");
}

int
main(void)
{
	test_add_basic();
	test_add_overflow();
	test_sub_basic();
	test_sub_overflow();
	test_timing();
	return x16_test_summary("cpu_adc_sbc");
}
