// Test fixture for the CPU core: a flat 64K memory and the handful of symbols
// fake6502.c expects the rest of the emulator to provide.
// fake6502.c needs six outside symbols, all supplied here: no SDL, ROM or devices.
//
// One fixture for all three instruction sets; reset6502() swaps one pointer.

#ifndef X16_TEST_CPU_FIXTURE_H
#define X16_TEST_CPU_FIXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/fake6502.h"
#include "cpu/registers.h"

// registers.h has the type, glue.h the object (and SDL); declared locally.
extern struct regs regs;

// The four CPU configurations the emulator can be in. Three of them change how
// wide a register is, which changes what an instruction does with it.
typedef enum {
	CPU_65C02,
	CPU_816_EMU,       // 65C816, emulation mode
	CPU_816_NATIVE_8,  // 65C816, native mode, 8-bit A/X/Y
	CPU_816_NATIVE_16, // 65C816, native mode, 16-bit A/X/Y
} cpu_mode_t;

const char *cpu_mode_name(cpu_mode_t mode);

// True where the mode treats A/X/Y as 16 bits.
bool cpu_mode_is_16bit(cpu_mode_t mode);

// The machine's memory, exposed directly so a test can seed operands and read
// results back without going through the CPU.
//
// 16 MB, indexed by the 65C816's full 24-bit address: (bank << 16) | offset.
// Bank 0 is the first 64K, so a 65C02 test indexes it exactly as before.
#define CPU_MEM_SIZE 0x1000000
extern uint8_t cpu_mem[CPU_MEM_SIZE];

// Seed a byte anywhere in the 24-bit space, recording it so the next reset
// clears it again. Writing cpu_mem[] directly is fine within bank 0, which is
// always cleared; outside it, use this or the byte survives into the next test.
void cpu_seed(uint32_t addr, uint8_t value);

// Zero memory and counters, point the reset vector at `start`, and reset the CPU into `mode`.
void cpu_reset_to(cpu_mode_t mode, uint16_t start);

// Copy a block into memory, returning the address just past it so successive
// pieces can be laid down without recomputing offsets.
uint16_t cpu_load(uint16_t addr, const uint8_t *bytes, uint16_t len);

// Execute exactly `count` instructions.
void cpu_steps(int count);

// Execute one instruction and return what it cost; cpu_last_cycles() covers a whole run.
uint32_t cpu_step(void);

// Run for `ticks` cycles through exec6502(), the emulator's loop.
void cpu_exec(uint32_t ticks);

// Cycles consumed by the most recent cpu_steps() call.
uint32_t cpu_last_cycles(void);

// How often the CPU has signalled a halt (STP) or pulled a vector, since the
// last reset. Both are otherwise invisible to a test.
extern uint32_t cpu_stop_count;
extern uint32_t cpu_vector_pulls;

// Read a register at the width the CPU is currently using.
// regs.a is A's low byte; regs.x/y are 16-bit, regs.xl/yl their low bytes.
uint16_t cpu_a(void);
uint16_t cpu_x(void);
uint16_t cpu_y(void);

// Set these to watch every bus access, for comparison with a reference trace. NULL by default.
// The address is the full 24-bit one; on a 65C02 the bank is zero.
extern void (*cpu_bus_read_hook)(uint32_t addr, uint8_t value);
extern void (*cpu_bus_write_hook)(uint32_t addr, uint8_t value);

// Whether the CPU is currently treating the accumulator, or the index
// registers, as 16 bits. Emulation mode forces both to 8 whatever the flags say.
bool memory_16bit_width(void);
bool index_16bit_width(void);

#endif // X16_TEST_CPU_FIXTURE_H
