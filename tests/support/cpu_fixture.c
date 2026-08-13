#include "cpu_fixture.h"

#include <string.h>

// exec6502() accumulates into clockgoal6502, and neither reset6502() nor the
// header exposes it. Left alone it would carry over between scenarios, so a
// run following another run would stop at the wrong point.
extern uint32_t clockgoal6502;

uint8_t  cpu_mem[0x10000];
uint32_t cpu_stop_count;
uint32_t cpu_vector_pulls;

static uint32_t cycles_before_run;

// ---- What fake6502.c expects the rest of the emulator to provide ------------
// Flat memory: no banking, no I/O. The bank byte is the 65816 program bank,
// which only selects a 64K space the emulator itself maps; a test that cares
// about banking belongs with memory.c, not here.

uint8_t
read6502(uint16_t address, uint8_t bank)
{
	(void)bank;
	return cpu_mem[address];
}

void
write6502(uint16_t address, uint8_t bank, uint8_t value)
{
	(void)bank;
	cpu_mem[address] = value;
}

// STP halts the CPU until reset. Counted rather than ignored so a test can tell
// a halt from a stall.
void
stop6502(uint16_t address, uint8_t bank)
{
	(void)address;
	(void)bank;
	cpu_stop_count++;
}

// Asserted while the CPU fetches an interrupt vector.
void
vp6502(void)
{
	cpu_vector_pulls++;
}

uint8_t
memory_get_ram_bank(void)
{
	return 0;
}

uint8_t
memory_get_rom_bank(void)
{
	return 0;
}

// ---- Fixture ---------------------------------------------------------------

const char *
cpu_mode_name(cpu_mode_t mode)
{
	switch (mode) {
		case CPU_65C02:          return "65C02";
		case CPU_816_EMU:        return "65C816 emulation";
		case CPU_816_NATIVE_8:   return "65C816 native, 8-bit";
		case CPU_816_NATIVE_16:  return "65C816 native, 16-bit";
	}
	return "?";
}

bool
cpu_mode_is_16bit(cpu_mode_t mode)
{
	return mode == CPU_816_NATIVE_16;
}

void
cpu_reset_to(cpu_mode_t mode, uint16_t start)
{
	memset(cpu_mem, 0, sizeof cpu_mem);
	cpu_stop_count   = 0;
	cpu_vector_pulls = 0;

	// reset6502() only ever sets bits in the status register -- it ORs in the
	// constant and width bits and calls setinterrupt()/cleardecimal() -- so
	// N, V, Z and C would otherwise survive from the previous scenario and
	// make a test's result depend on what ran before it.
	regs.status = 0;

	// reset6502() reads the vector from $FFFC/$FFFD, so the entry point has to
	// be in place before the reset rather than assigned to regs.pc after it.
	cpu_mem[0xFFFC] = (uint8_t)(start & 0xFF);
	cpu_mem[0xFFFD] = (uint8_t)(start >> 8);

	reset6502(mode != CPU_65C02);

	// Emulation mode forces 8-bit registers whatever these bits say, so the
	// width flags are only meaningful once E is clear.
	switch (mode) {
		case CPU_65C02:
		case CPU_816_EMU:
			break;
		case CPU_816_NATIVE_8:
			regs.e      = 0;
			regs.status |= FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH;
			break;
		case CPU_816_NATIVE_16:
			regs.e      = 0;
			regs.status &= (uint8_t) ~(FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH);
			break;
	}

	clockticks6502    = 0;
	clockgoal6502     = 0;
	cycles_before_run = 0;
}

uint16_t
cpu_load(uint16_t addr, const uint8_t *bytes, uint16_t len)
{
	memcpy(&cpu_mem[addr], bytes, len);
	return (uint16_t)(addr + len);
}

void
cpu_steps(int count)
{
	cycles_before_run = clockticks6502;
	for (int i = 0; i < count; i++) {
		step6502();
	}
}

uint32_t
cpu_step(void)
{
	cycles_before_run = clockticks6502;
	step6502();
	return clockticks6502 - cycles_before_run;
}

void
cpu_exec(uint32_t ticks)
{
	cycles_before_run = clockticks6502;
	exec6502(ticks);
}

uint32_t
cpu_last_cycles(void)
{
	return clockticks6502 - cycles_before_run;
}

// The register names are not as symmetric as they look: regs.a is the low byte
// of the accumulator while regs.x and regs.y are the whole 16-bit registers,
// so reading regs.a in a 16-bit scenario would silently discard the high byte
// and report a false pass. These read the width the CPU is actually using.

uint16_t
cpu_a(void)
{
	return memory_16bit_width() ? regs.c : regs.a;
}

uint16_t
cpu_x(void)
{
	return index_16bit_width() ? regs.x : regs.xl;
}

uint16_t
cpu_y(void)
{
	return index_16bit_width() ? regs.y : regs.yl;
}

bool
memory_16bit_width(void)
{
	return !regs.e && !(regs.status & FLAG_MEMORY_WIDTH);
}

bool
index_16bit_width(void)
{
	return !regs.e && !(regs.status & FLAG_INDEX_WIDTH);
}
