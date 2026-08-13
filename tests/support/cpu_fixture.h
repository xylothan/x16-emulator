// Test fixture for the CPU core: a flat 64K memory and the handful of symbols
// fake6502.c expects the rest of the emulator to provide.
//
// The CPU is the cheapest part of the machine to test. fake6502.c defines its
// own registers, cycle counter and run state, includes no memory.h, and reaches
// outside itself for only six symbols -- all supplied here. So a CPU test links
// the real instruction tables against this flat memory and nothing else: no
// SDL, no ROM, no banking, no devices.
//
// One fixture covers all three instruction sets. There is no separate 65816
// core: fake6502.c holds both optable_c02[] and optable_c816[] and swaps a
// single pointer inside reset6502(), so which CPU a scenario runs on is a
// parameter here rather than a different harness.

#ifndef X16_TEST_CPU_FIXTURE_H
#define X16_TEST_CPU_FIXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/fake6502.h"
#include "cpu/registers.h"

// registers.h defines the type but not the object, and the only declaration of
// it lives in glue.h, which reaches SDL. code_map.c and debug_core.c each
// declare it locally for the same reason; this does the same.
extern struct regs regs;

// The four CPU configurations the emulator can be in. Three of them change how
// wide a register is, which changes what an instruction does with it.
typedef enum {
	CPU_65C02,
	CPU_816_EMU,       // 65816, emulation mode
	CPU_816_NATIVE_8,  // 65816, native mode, 8-bit A/X/Y
	CPU_816_NATIVE_16, // 65816, native mode, 16-bit A/X/Y
} cpu_mode_t;

const char *cpu_mode_name(cpu_mode_t mode);

// True where the mode treats A/X/Y as 16 bits.
bool cpu_mode_is_16bit(cpu_mode_t mode);

// The machine's memory, exposed directly so a test can seed operands and read
// results back without going through the CPU.
extern uint8_t cpu_mem[0x10000];

// Zero memory and counters, point the reset vector at `start`, and reset the
// CPU into `mode`. Every scenario begins here, so one test cannot inherit
// state from the one before it.
void cpu_reset_to(cpu_mode_t mode, uint16_t start);

// Copy a block into memory, returning the address just past it so successive
// pieces can be laid down without recomputing offsets.
uint16_t cpu_load(uint16_t addr, const uint8_t *bytes, uint16_t len);

// Execute exactly `count` instructions.
void cpu_steps(int count);

// Execute one instruction and return what it cost. Timing assertions want this
// rather than cpu_last_cycles(), which covers a whole run: reading a per-
// instruction cost off a run that also included its setup is an easy mistake to
// make and a confusing one to debug.
uint32_t cpu_step(void);

// Run for `ticks` cycles through exec6502(), the loop the emulator itself uses.
// step6502() is the loop the debugger drives, and the two are separate pieces
// of code that are supposed to agree; this exists so a test can compare them.
void cpu_exec(uint32_t ticks);

// Cycles consumed by the most recent cpu_steps() call. Cycle counts are worth
// asserting on: they are what a debugger hook can perturb without changing any
// visible register.
uint32_t cpu_last_cycles(void);

// How often the CPU has signalled a halt (STP) or pulled a vector, since the
// last reset. Both are otherwise invisible to a test.
extern uint32_t cpu_stop_count;
extern uint32_t cpu_vector_pulls;

// Read a register at the width the CPU is currently using.
//
// The register names are less symmetric than they look: regs.a is the LOW BYTE
// of the accumulator, while regs.x and regs.y are the whole 16-bit registers
// (regs.xl and regs.yl are their low bytes). Checking regs.a in a 16-bit
// scenario would quietly drop the high byte and report a false pass, in exactly
// the mode this fixture exists to cover, so prefer these throughout.
uint16_t cpu_a(void);
uint16_t cpu_x(void);
uint16_t cpu_y(void);

// Set these to watch every bus access the CPU makes, so a test can compare the
// order and addresses against a reference trace. NULL by default, which costs
// one branch per access and keeps the other tests unaffected.
extern void (*cpu_bus_read_hook)(uint16_t addr, uint8_t value);
extern void (*cpu_bus_write_hook)(uint16_t addr, uint8_t value);

// Whether the CPU is currently treating the accumulator, or the index
// registers, as 16 bits. Emulation mode forces both to 8 whatever the flags say.
bool memory_16bit_width(void);
bool index_16bit_width(void);

#endif // X16_TEST_CPU_FIXTURE_H
