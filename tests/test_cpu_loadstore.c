// CPU conformance: load and store instructions, and the addressing modes they
// reach memory through.
//
// Oracle: WDC W65C02S datasheet. Cycle note 1, p20, covers both timing rules
// asserted here: "Page boundary, add 1 cycle if page boundary is crossed when
// forming address. Add 1 cycle for STA abs,X" -- indexed reads pay for a
// crossing, indexed stores pay a fixed cost either way.
//
// Cycle counts are asserted alongside the results because timing is part of the
// specification, and because it is what a debugger hook can disturb without
// changing any visible register.
//
// Programs are written as raw bytes with the mnemonic in a comment. That reads
// and greps better than a builder DSL would at this size.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#define PROG 0x0200

// Assemble at PROG, run `steps` instructions from a clean machine.
static void
run(const uint8_t *prog, uint16_t len, int steps)
{
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, prog, len);
	cpu_steps(steps);
}

#define RUN(p, n) run((p), (uint16_t)sizeof(p), (n))

// ---- Loads set N and Z from the value loaded -------------------------------

static void
test_load_flags(void)
{
	const uint8_t zero[] = { 0xA9, 0x00 };       // LDA #$00
	RUN(zero, 1);
	check(regs.status & FLAG_ZERO, "LDA #$00 sets Z");
	check(!(regs.status & FLAG_SIGN), "LDA #$00 clears N");

	const uint8_t neg[] = { 0xA9, 0x80 };        // LDA #$80
	RUN(neg, 1);
	check(regs.status & FLAG_SIGN, "LDA #$80 sets N");
	check(!(regs.status & FLAG_ZERO), "LDA #$80 clears Z");

	const uint8_t pos[] = { 0xA2, 0x7F };        // LDX #$7F
	RUN(pos, 1);
	check(!(regs.status & (FLAG_SIGN | FLAG_ZERO)), "LDX #$7F clears N and Z");

	const uint8_t ldy[] = { 0xA0, 0xFF };        // LDY #$FF
	RUN(ldy, 1);
	check(regs.status & FLAG_SIGN, "LDY #$FF sets N");
}

// ---- Stores write, and touch no flag ---------------------------------------

static void
test_stores(void)
{
	// LDA #$5A ; STA $0400
	const uint8_t sta_abs[] = { 0xA9, 0x5A, 0x8D, 0x00, 0x04 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, sta_abs, sizeof sta_abs);
	cpu_steps(1);
	check_eq(cpu_step(), 4, "STA absolute costs 4 cycles");
	check_eq(cpu_mem[0x0400], 0x5A, "STA absolute writes the accumulator");

	// LDX #$01 ; LDA #$00 ; STA $0500,X
	// The load comes last so Z is set going into the store, which is what makes
	// "the store changed no flag" a meaningful claim. Both target bytes are
	// seeded non-zero first: memory resets to zero, so storing a zero into a
	// zeroed byte would compare equal however the address was computed, and
	// would assert nothing at all.
	const uint8_t flags[] = { 0xA2, 0x01, 0xA9, 0x00, 0x9D, 0x00, 0x05 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, flags, sizeof flags);
	cpu_mem[0x0500] = 0xFF;
	cpu_mem[0x0501] = 0xFF;
	cpu_steps(2);
	uint8_t before = regs.status;
	check_eq(cpu_step(), 5, "STA absolute,X always costs 5 cycles");
	check_eq(cpu_mem[0x0501], 0x00, "STA absolute,X writes at base plus index");
	check_eq(cpu_mem[0x0500], 0xFF, "and not at the unindexed address");
	check_eq(regs.status, before, "a store changes no flag");

	// LDX #$77 ; STX $30
	const uint8_t stx[] = { 0xA2, 0x77, 0x86, 0x30 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, stx, sizeof stx);
	cpu_steps(1);
	check_eq(cpu_step(), 3, "STX zero page costs 3 cycles");
	check_eq(cpu_mem[0x0030], 0x77, "STX zero page writes X");

	// LDY #$66 ; STY $0402
	const uint8_t sty[] = { 0xA0, 0x66, 0x8C, 0x02, 0x04 };
	RUN(sty, 2);
	check_eq(cpu_mem[0x0402], 0x66, "STY absolute writes Y");
}

// ---- Addressing modes reach the right address ------------------------------

static void
test_addressing_modes(void)
{
	// LDA $30
	const uint8_t zp[] = { 0xA5, 0x30 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, zp, sizeof zp);
	cpu_mem[0x0030] = 0x11;
	cpu_steps(1);
	check_eq(regs.a, 0x11, "LDA zero page");
	check_eq(cpu_last_cycles(), 3, "LDA zero page costs 3 cycles");

	// LDX #$05 ; LDA $30,X
	const uint8_t zpx[] = { 0xA2, 0x05, 0xB5, 0x30 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, zpx, sizeof zpx);
	cpu_mem[0x0035] = 0x22;
	cpu_steps(1);
	check_eq(cpu_step(), 4, "LDA zero page,X costs 4 cycles");
	check_eq(regs.a, 0x22, "LDA zero page,X");

	// LDY #$03 ; LDX $40,Y  -- the only zero page,Y load
	const uint8_t zpy[] = { 0xA0, 0x03, 0xB6, 0x40 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, zpy, sizeof zpy);
	cpu_mem[0x0043] = 0x33;
	cpu_steps(2);
	check_eq(regs.x, 0x33, "LDX zero page,Y");

	// LDA $0410
	const uint8_t abs[] = { 0xAD, 0x10, 0x04 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, abs, sizeof abs);
	cpu_mem[0x0410] = 0x44;
	cpu_steps(1);
	check_eq(regs.a, 0x44, "LDA absolute");
	check_eq(cpu_last_cycles(), 4, "LDA absolute costs 4 cycles");
}

// ---- Zero page indexing wraps inside page zero -----------------------------
// $FF + 2 addresses $01, not $0101. Getting this wrong reads the wrong page
// entirely, so it is worth pinning on its own.

static void
test_zero_page_wrap(void)
{
	// LDX #$02 ; LDA $FF,X
	const uint8_t wrap[] = { 0xA2, 0x02, 0xB5, 0xFF };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, wrap, sizeof wrap);
	cpu_mem[0x0001] = 0xAB;
	cpu_mem[0x0101] = 0xCD; // where it would land without the wrap
	cpu_steps(2);
	check_eq(regs.a, 0xAB, "zero page,X wraps within page zero");

	// LDX #$02 ; LDA ($FE,X) -- the pointer itself wraps too
	const uint8_t iwrap[] = { 0xA2, 0x02, 0xA1, 0xFE };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, iwrap, sizeof iwrap);
	cpu_mem[0x0000] = 0x20; // pointer low, at $FE+2 wrapped
	cpu_mem[0x0001] = 0x04; // pointer high
	cpu_mem[0x0420] = 0xEF;
	cpu_steps(1);
	check_eq(cpu_step(), 6, "LDA (zero page,X) costs 6 cycles");
	check_eq(regs.a, 0xEF, "(zero page,X) wraps when forming the pointer");
}

// ---- Indexed reads cost an extra cycle when they cross a page --------------

static void
test_page_cross_timing(void)
{
	// LDX #$01 ; LDA $0410,X -- stays inside page $04
	const uint8_t same[] = { 0xA2, 0x01, 0xBD, 0x10, 0x04 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, same, sizeof same);
	cpu_steps(1);
	check_eq(cpu_step(), 4, "LDA absolute,X costs 4 within a page");

	// LDX #$01 ; LDA $04FF,X -- crosses into page $05
	const uint8_t cross[] = { 0xA2, 0x01, 0xBD, 0xFF, 0x04 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, cross, sizeof cross);
	cpu_steps(1);
	check_eq(cpu_step(), 5, "LDA absolute,X costs 5 across a page");

	// LDY #$01 ; LDA $04FF,Y
	const uint8_t crossy[] = { 0xA0, 0x01, 0xB9, 0xFF, 0x04 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, crossy, sizeof crossy);
	cpu_steps(1);
	check_eq(cpu_step(), 5, "LDA absolute,Y costs 5 across a page");

	// LDY #$01 ; LDA ($20),Y with the pointer at $04FF
	const uint8_t indy[] = { 0xA0, 0x01, 0xB1, 0x20 };
	cpu_reset_to(CPU_65C02, PROG);
	cpu_load(PROG, indy, sizeof indy);
	cpu_mem[0x0020] = 0xFF;
	cpu_mem[0x0021] = 0x04;
	cpu_steps(1);
	check_eq(cpu_step(), 6, "LDA (zero page),Y costs 6 across a page");
}

int
main(void)
{
	test_load_flags();
	test_stores();
	test_addressing_modes();
	test_zero_page_wrap();
	test_page_cross_timing();
	return x16_test_summary("cpu_loadstore");
}
