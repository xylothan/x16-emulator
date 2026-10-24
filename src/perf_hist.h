// Commander X16 Emulator — whole-program cycle histogram and task attribution.
//
// perf_budget.c answers "how much of the frame is work?" and, through zones,
// "how much of it is this address range?". Zones match the CPU's PC only, and
// on an X16 the same $A000-$BFFF addresses hold a different program in every
// RAM bank, so a zone cannot name one banked routine. Nor can 255 zones cover a
// program with hundreds of routines.
//
// This module answers both, with no cooperation from the guest:
//
//   1. A HISTOGRAM of cycles per EFFECTIVE address -- (RAM bank, PC) in the
//      banked window, (ROM bank, PC) above $C000, plain PC below $A000 --
//      split into main-line and interrupt cycles. A host maps it onto the
//      program's debug info and gets self time for every routine at once.
//
//   2. TASKS. The host names a CALLER range (a main loop). Every JSR executed
//      inside that range opens a task named by its target; the task owns every
//      main-line cycle until the PC is back in the caller range. That is the
//      inclusive cost of each top-level call, with calls, worst single call and
//      worst frame -- the numbers a mean hides.
//
// Like perf_budget.c it takes everything it knows about the machine as an
// argument and touches no emulator globals.
//
// COST WHEN OFF: one load and one predictable branch per instruction.

#ifndef PERF_HIST_H
#define PERF_HIST_H

#include <stdbool.h>
#include <stdint.h>

// Effective-address key layout. Exported so a dump reader can decode it.
#define PH_LOW_KEYS     0xA000u                  // $0000-$9FFF, unbanked
#define PH_RAM_BANKS    256u
#define PH_RAM_SIZE     0x2000u
#define PH_ROM_BANKS    32u
#define PH_ROM_SIZE     0x4000u
#define PH_RAM_BASE     PH_LOW_KEYS
#define PH_ROM_BASE     (PH_RAM_BASE + PH_RAM_BANKS * PH_RAM_SIZE)
#define PH_KEYS         (PH_ROM_BASE + PH_ROM_BANKS * PH_ROM_SIZE)

#define PH_MAX_TASKS    96

typedef struct {
	uint16_t target;     // the JSR's target address
	uint8_t  ram_bank;   // RAM bank when the JSR ran (a thunk switches later)
	uint32_t calls;
	uint64_t cycles;     // main-line cycles, IRQs excluded
	uint32_t call_max;   // worst single call
	uint32_t frame_max;  // worst vsync frame
} perf_hist_task_t;

extern bool perf_hist_enabled;

bool perf_hist_arm(bool on);
void perf_hist_reset(void);
void perf_hist_set_caller(uint16_t start, uint16_t end); // end exclusive; 0,0 = off
bool perf_hist_caller(uint16_t *start, uint16_t *end);

// `pc` is the instruction that just ran; `opcode` its first byte (read before
// it ran); `next_pc` the PC after it.
void perf_hist_step_(uint32_t clocks, uint16_t pc, uint8_t ram_bank, uint8_t rom_bank,
                     uint8_t opcode, uint16_t next_pc, bool irq);
void perf_hist_close_frame_(void);

static inline bool
perf_hist_in_caller(uint16_t pc)
{
	extern uint16_t perf_hist_caller_start, perf_hist_caller_end;
	return pc >= perf_hist_caller_start && pc < perf_hist_caller_end;
}

static inline void
perf_hist_close_frame(void)
{
	if (perf_hist_enabled)
		perf_hist_close_frame_();
}

// Queries.
uint32_t perf_hist_frames(void);
uint64_t perf_hist_total_cycles(void);
uint64_t perf_hist_irq_cycles(void);
uint64_t perf_hist_loop_cycles(void);   // main-line cycles in the caller range
int      perf_hist_task_count(void);
bool     perf_hist_task_get(int i, perf_hist_task_t *out);

// ─── Read access for analysis ───────────────────────────────────────────────
// Which memory a key describes, and the bank and CPU address within it.
#define PH_REGION_LOW 0   // $0000-$9FFF, not banked
#define PH_REGION_RAM 1   // $A000-$BFFF, by RAM bank
#define PH_REGION_ROM 2   // $C000-$FFFF, by ROM bank

static inline void
perf_hist_key_decode(uint32_t k, uint8_t *region, uint8_t *bank, uint16_t *pc)
{
	if (k < PH_RAM_BASE) {
		*region = PH_REGION_LOW;
		*bank   = 0;
		*pc     = (uint16_t)k;
	} else if (k < PH_ROM_BASE) {
		*region = PH_REGION_RAM;
		*bank   = (uint8_t)((k - PH_RAM_BASE) / PH_RAM_SIZE);
		*pc     = (uint16_t)(0xA000u + (k - PH_RAM_BASE) % PH_RAM_SIZE);
	} else {
		*region = PH_REGION_ROM;
		*bank   = (uint8_t)((k - PH_ROM_BASE) / PH_ROM_SIZE);
		*pc     = (uint16_t)(0xC000u + (k - PH_ROM_BASE) % PH_ROM_SIZE);
	}
}

// Counts at one key since the last reset. False when the histogram was never
// armed or `key` is out of range.
bool perf_hist_key_counts(uint32_t key, uint32_t *main_cycles, uint32_t *irq_cycles,
                          uint32_t *instructions);

// Sparse binary dump: "PHST", u32 version 2, u32 PH_KEYS, u32 frames, then
// (u32 key, u32 main cycles, u32 irq cycles, u32 instructions executed) for
// every key that ran. Returns entry count or -1.
long     perf_hist_dump(const char *path);

#endif
