// Commander X16 Emulator — whole-program cycle histogram. See perf_hist.h.

#include "perf_hist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool     perf_hist_enabled = false;
uint16_t perf_hist_caller_start = 0, perf_hist_caller_end = 0;

static uint32_t *hist_main = NULL;
static uint32_t *hist_irq  = NULL;
static uint32_t *hist_cnt  = NULL; // instructions executed at the key

static uint32_t frames;
static uint64_t total, irq_total, loop_total;

static perf_hist_task_t tasks[PH_MAX_TASKS];
static uint32_t         task_frame[PH_MAX_TASKS]; // this frame's cycles
static int              ntasks;
static int              cur_task = -1;            // -1: none open
static uint32_t         cur_call;                 // cycles of the open call

static uint32_t
key_of(uint16_t pc, uint8_t ram_bank, uint8_t rom_bank)
{
	if (pc < 0xA000)
		return pc;
	if (pc < 0xC000)
		return PH_RAM_BASE + (uint32_t)ram_bank * PH_RAM_SIZE + (pc - 0xA000u);
	return PH_ROM_BASE + (uint32_t)(rom_bank & (PH_ROM_BANKS - 1)) * PH_ROM_SIZE + (pc - 0xC000u);
}

bool
perf_hist_arm(bool on)
{
	if (on && !hist_main) {
		hist_main = calloc(PH_KEYS, sizeof *hist_main);
		hist_irq  = calloc(PH_KEYS, sizeof *hist_irq);
		hist_cnt  = calloc(PH_KEYS, sizeof *hist_cnt);
		if (!hist_main || !hist_irq || !hist_cnt) {
			free(hist_main);
			free(hist_irq);
			free(hist_cnt);
			hist_main = hist_irq = hist_cnt = NULL;
			perf_hist_enabled = false;
			return false;
		}
		perf_hist_reset();
	}
	perf_hist_enabled = on;
	return true;
}

static void
close_call(void)
{
	if (cur_task >= 0) {
		perf_hist_task_t *t = &tasks[cur_task];
		t->calls++;
		if (cur_call > t->call_max)
			t->call_max = cur_call;
	}
	cur_task = -1;
	cur_call = 0;
}

void
perf_hist_reset(void)
{
	if (hist_main) {
		memset(hist_main, 0, PH_KEYS * sizeof *hist_main);
		memset(hist_irq, 0, PH_KEYS * sizeof *hist_irq);
		memset(hist_cnt, 0, PH_KEYS * sizeof *hist_cnt);
	}
	frames = 0;
	total = irq_total = loop_total = 0;
	memset(tasks, 0, sizeof tasks);
	memset(task_frame, 0, sizeof task_frame);
	ntasks = 0;
	cur_task = -1;
	cur_call = 0;
}

void
perf_hist_set_caller(uint16_t start, uint16_t end)
{
	perf_hist_caller_start = start;
	perf_hist_caller_end   = end;
	close_call();
}

bool
perf_hist_caller(uint16_t *start, uint16_t *end)
{
	if (start) *start = perf_hist_caller_start;
	if (end)   *end   = perf_hist_caller_end;
	return perf_hist_caller_end > perf_hist_caller_start;
}

static int
task_for(uint16_t target, uint8_t ram_bank)
{
	for (int i = 0; i < ntasks; i++)
		if (tasks[i].target == target && (target < 0xA000 || tasks[i].ram_bank == ram_bank))
			return i;
	if (ntasks >= PH_MAX_TASKS)
		return -1;
	tasks[ntasks].target   = target;
	tasks[ntasks].ram_bank = ram_bank;
	return ntasks++;
}

void
perf_hist_step_(uint32_t clocks, uint16_t pc, uint8_t ram_bank, uint8_t rom_bank,
                uint8_t opcode, uint16_t next_pc, bool irq)
{
	const uint32_t k = key_of(pc, ram_bank, rom_bank);
	total += clocks;
	hist_cnt[k]++;
	if (irq) {
		hist_irq[k] += clocks;
		irq_total += clocks;
		return;
	}
	hist_main[k] += clocks;

	if (perf_hist_caller_end <= perf_hist_caller_start)
		return;

	if (perf_hist_in_caller(pc)) {
		// Back in the loop: the open call, if any, has returned.
		if (cur_task >= 0)
			close_call();
		loop_total += clocks;
		if (opcode == 0x20) { // JSR abs: the next instruction opens a task
			const int t = task_for(next_pc, ram_bank);
			if (t >= 0) {
				cur_task = t;
				cur_call = 0;
			}
		}
		return;
	}
	if (cur_task >= 0) {
		tasks[cur_task].cycles += clocks;
		task_frame[cur_task]   += clocks;
		cur_call               += clocks;
	} else {
		loop_total += clocks; // outside every task and the loop: count as loop
	}
}

void
perf_hist_close_frame_(void)
{
	frames++;
	for (int i = 0; i < ntasks; i++) {
		if (task_frame[i] > tasks[i].frame_max)
			tasks[i].frame_max = task_frame[i];
		task_frame[i] = 0;
	}
}

uint32_t perf_hist_frames(void)       { return frames; }
uint64_t perf_hist_total_cycles(void) { return total; }
uint64_t perf_hist_irq_cycles(void)   { return irq_total; }
uint64_t perf_hist_loop_cycles(void)  { return loop_total; }
int      perf_hist_task_count(void)   { return ntasks; }

bool
perf_hist_task_get(int i, perf_hist_task_t *out)
{
	if (i < 0 || i >= ntasks)
		return false;
	if (out)
		*out = tasks[i];
	return true;
}

bool
perf_hist_key_counts(uint32_t key, uint32_t *main_cycles, uint32_t *irq_cycles,
                     uint32_t *instructions)
{
	if (!hist_main || key >= PH_KEYS)
		return false;
	if (main_cycles)
		*main_cycles = hist_main[key];
	if (irq_cycles)
		*irq_cycles = hist_irq[key];
	if (instructions)
		*instructions = hist_cnt[key];
	return true;
}

long
perf_hist_dump(const char *path)
{
	if (!hist_main || !path)
		return -1;
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	const uint32_t hdr[4] = {0x54534850u /* "PHST" */, 2u, PH_KEYS, frames};
	fwrite(hdr, sizeof hdr, 1, f);
	long n = 0;
	for (uint32_t k = 0; k < PH_KEYS; k++) {
		if (!hist_main[k] && !hist_irq[k])
			continue;
		const uint32_t rec[4] = {k, hist_main[k], hist_irq[k], hist_cnt[k]};
		fwrite(rec, sizeof rec, 1, f);
		n++;
	}
	fclose(f);
	return n;
}
