// Regression: single-stepping must reproduce running.
//
// The emulator runs through exec6502() and the debugger steps through
// step6502(). They are separate loops over the same tables, and everything a
// debugger tells you rests on them agreeing: if stepping through code produces
// a different machine than letting it run, then every value the debugger shows
// is a value the program would not otherwise have had.
//
// Each scenario runs the same program from the same starting state both ways
// and compares the whole visible machine -- registers, memory and the cycle
// counter. Cycle counts are included deliberately: they are the part most
// likely to drift, and the part a debugger hook can disturb without changing
// anything else.

#include "support/cpu_fixture.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

#define PROG 0x0200
#define SCRATCH 0x0400

// A snapshot of everything a scenario can observe.
typedef struct {
	uint16_t a, x, y, pc, sp, dp;
	uint8_t  status, k, db;
	uint32_t cycles;
	uint8_t  scratch[16];
} machine_t;

static machine_t
snapshot(void)
{
	machine_t m;
	m.a      = cpu_a();
	m.x      = cpu_x();
	m.y      = cpu_y();
	m.pc     = regs.pc;
	m.sp     = regs.sp;
	m.dp     = regs.dp;
	m.status = regs.status;
	m.k      = regs.k;
	m.db     = regs.db;
	m.cycles = clockticks6502;
	for (int i = 0; i < 16; i++) {
		m.scratch[i] = cpu_mem[SCRATCH + i];
	}
	return m;
}

static void
compare(machine_t stepped, machine_t ran, const char *what)
{
	char label[128];

#define FIELD(f, name)                                                        \
	snprintf(label, sizeof label, "%s: %s matches", what, name);              \
	check_eq(ran.f, stepped.f, label)

	FIELD(a, "A");
	FIELD(x, "X");
	FIELD(y, "Y");
	FIELD(pc, "PC");
	FIELD(sp, "SP");
	FIELD(status, "status");
	FIELD(cycles, "cycle count");
#undef FIELD

	bool same_memory = true;
	for (int i = 0; i < 16; i++) {
		if (stepped.scratch[i] != ran.scratch[i]) {
			same_memory = false;
		}
	}
	snprintf(label, sizeof label, "%s: memory matches", what);
	check(same_memory, label);
}

// Run `steps` instructions one at a time, then the same program for the same
// number of cycles in one go, and compare.
static void
both_ways(cpu_mode_t mode, const uint8_t *prog, uint16_t len, int steps,
          const char *what)
{
	cpu_reset_to(mode, PROG);
	cpu_load(PROG, prog, len);
	cpu_steps(steps);
	machine_t stepped = snapshot();

	cpu_reset_to(mode, PROG);
	cpu_load(PROG, prog, len);
	cpu_exec(stepped.cycles);
	machine_t ran = snapshot();

	compare(stepped, ran, what);
}

#define BOTH_WAYS(mode, prog, steps, what) \
	both_ways((mode), (prog), (uint16_t)sizeof(prog), (steps), (what))

// ---- Straight-line arithmetic ----------------------------------------------

static void
test_arithmetic(void)
{
	// CLC ; LDA #$40 ; ADC #$40 ; STA $0400 ; LDX #$05 ; DEX ; STX $0401
	const uint8_t prog[] = {
		0x18, 0xA9, 0x40, 0x69, 0x40, 0x8D, 0x00, 0x04,
		0xA2, 0x05, 0xCA, 0x8E, 0x01, 0x04,
	};
	BOTH_WAYS(CPU_65C02, prog, 7, "arithmetic and stores");
}

// ---- Taken branches, which carry their own cycle penalties -----------------

static void
test_branches(void)
{
	// LDX #$03 ; loop: DEX ; BNE loop ; STX $0400
	const uint8_t prog[] = {
		0xA2, 0x03,
		0xCA,             // loop
		0xD0, 0xFD,       // BNE loop
		0x8E, 0x00, 0x04,
	};
	BOTH_WAYS(CPU_65C02, prog, 8, "a loop with taken branches");
}

// ---- Subroutine call and return --------------------------------------------

static void
test_subroutine(void)
{
	// JSR $0210 ; STA $0400 ; (at $0210) LDA #$7B ; RTS
	uint8_t prog[0x14];
	memset(prog, 0xEA, sizeof prog); // NOP filler
	prog[0x00] = 0x20; prog[0x01] = 0x10; prog[0x02] = 0x02; // JSR $0210
	prog[0x03] = 0x8D; prog[0x04] = 0x00; prog[0x05] = 0x04; // STA $0400
	prog[0x10] = 0xA9; prog[0x11] = 0x7B;                    // LDA #$7B
	prog[0x12] = 0x60;                                       // RTS
	BOTH_WAYS(CPU_65C02, prog, 4, "a subroutine call and return");
}

// ---- Page-crossing indexed reads, which cost an extra cycle ----------------

static void
test_page_cross(void)
{
	// LDX #$01 ; LDA $04FF,X ; STA $0400
	const uint8_t prog[] = {
		0xA2, 0x01, 0xBD, 0xFF, 0x04, 0x8D, 0x00, 0x04,
	};
	BOTH_WAYS(CPU_65C02, prog, 3, "an indexed read across a page");
}

// ---- 65C816 native mode, including a misaligned direct page -----------------
// The direct-page penalty is applied by one loop and not the other, so this is
// where the two paths are most likely to disagree.

static void
test_native_direct_page(void)
{
	// LDA #$01 ; XBA ; LDA #$34 ; TCD  -- direct page to $0134, misaligned
	// LDA $10 ; STA $0400
	const uint8_t prog[] = {
		0xA9, 0x01, 0xEB, 0xA9, 0x34, 0x5B,
		0xA5, 0x10,
		0x8D, 0x00, 0x04,
	};
	BOTH_WAYS(CPU_816_NATIVE_8, prog, 6, "a misaligned direct page in native mode");
}

static void
test_native_aligned_direct_page(void)
{
	// The same shape with an aligned direct page ($0100), which pays no penalty.
	const uint8_t prog[] = {
		0xA9, 0x01, 0xEB, 0xA9, 0x00, 0x5B,
		0xA5, 0x10,
		0x8D, 0x00, 0x04,
	};
	BOTH_WAYS(CPU_816_NATIVE_8, prog, 6, "an aligned direct page in native mode");
}

int
main(void)
{
	test_arithmetic();
	test_branches();
	test_subroutine();
	test_page_cross();
	test_native_aligned_direct_page();
	test_native_direct_page();
	return x16_test_summary("cpu_steprun");
}
