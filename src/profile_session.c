// Commander X16 Emulator — capture mode, wired to the emulator. See
// profile_session.h.

#include "profile_session.h"

#include "dbg_info.h"
#include "host_prof.h"
#include "perf_budget.h"
#include "perf_hist.h"
#include "vera_bandwidth.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

static prof_report_t report;
static bool          have_report;
static uint32_t      report_frames;

// Whether perf_hist was armed by someone else (a DAP client) before the
// capture, so ending the capture leaves it as it was found.
static bool hist_was_enabled;

static double
now_s(void)
{
	return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

static bool
arm(uint32_t mask)
{
	bool ok = true;
	if (mask & CAP_REC_HIST) {
		hist_was_enabled = perf_hist_enabled;
		ok = perf_hist_arm(true) && ok;
		// The capture's histogram starts empty. This discards whatever a DAP
		// client had accumulated; perf_hist has one set of counters.
		perf_hist_reset();
	}
	if (mask & CAP_REC_BUDGET)
		ok = perf_budget_arm(PERF_OWNER_CAPTURE, true) && ok;
	if (mask & CAP_REC_VRAM)
		vera_bandwidth_arm(VERA_BW_OWNER_CAPTURE, true);
	// Last: arming the histogram allocates and clears ~32 MB, and that hitch
	// is not part of the run. Reset, the host profile covers the capture only.
	host_prof_reset();
	return ok;
}

static void
disarm(uint32_t mask)
{
	if (mask & CAP_REC_HIST)
		perf_hist_arm(hist_was_enabled);
	if (mask & CAP_REC_BUDGET)
		perf_budget_arm(PERF_OWNER_CAPTURE, false);
	if (mask & CAP_REC_VRAM)
		vera_bandwidth_arm(VERA_BW_OWNER_CAPTURE, false);
}

// Coverage for the report: does debug info describe this address (in this
// bank)? 0 no, 1 yes, 2 yes but the bank was not established.
static int
covered(uint16_t pc, int ram_bank)
{
	switch (dbg_info_addr_to_source_banked_ex(pc, ram_bank, NULL, NULL)) {
		case DBG_BANK_RESOLVED: return 1;
		case DBG_BANK_UNKNOWN:  return ram_bank < 0 ? 1 : 2;
		default:                return 0;
	}
}

static void
finished(cap_end_t why)
{
	(void)why;
	if (have_report)
		prof_report_free(&report);
	have_report = false;
	if (!(cap_recorders() & CAP_REC_HIST))
		return;

	const int   n    = dbg_info_symbol_count();
	prof_sym_t *syms = n > 0 ? calloc((size_t)n, sizeof *syms) : NULL;
	int         ns   = 0;
	// Name time by routine: when the debug info says which labels open a
	// .proc/.scope, only those count, so a local like `loop` or `next` rolls
	// up into the routine it is in. Without scope records, every label.
	bool routines_only = false;
	for (int i = 0; i < n && !routines_only; i++)
		routines_only = dbg_info_symbol_is_scope_at(i);
	for (int i = 0; syms && i < n; i++) {
		const char *name;
		dbg_addr_t  addr;
		if (!dbg_info_symbol_at(i, &name, &addr) || addr > 0xFFFF)
			continue;
		if (routines_only && !dbg_info_symbol_is_scope_at(i))
			continue;
		syms[ns].name = name;
		syms[ns].addr = (uint16_t)addr;
		syms[ns].bank = (int16_t)dbg_info_symbol_bank_at(i);
		ns++;
	}
	have_report   = prof_report_build(&report, syms, ns, covered);
	report_frames = cap_frames();
	free(syms);
}

static bool
marker_hook(uint8_t value)
{
	return cap_marker(value);
}

void
profile_session_init(void)
{
	const cap_hooks_t h = {arm, disarm, finished, now_s};
	cap_init(&h);
	perf_budget_marker_hook = marker_hook;
}

void
profile_session_frame_end(void)
{
	cap_frame_end();
}

const prof_report_t *
profile_session_report(void)
{
	return have_report ? &report : NULL;
}

uint32_t
profile_session_report_frames(void)
{
	return have_report ? report_frames : 0;
}

void
profile_session_print(FILE *f, int top)
{
	fprintf(f, "\n=== capture: %s, %u frames, %.2f s ===\n", cap_end_name(cap_end_reason()),
	        cap_frames(), cap_elapsed_s());
	const prof_report_t *r = profile_session_report();
	if (!r) {
		fprintf(f, "no report (the histogram was not recorded)\n");
		return;
	}
	const uint64_t total = r->main_cycles + r->irq_cycles;
	const uint32_t frames = report_frames ? report_frames : 1;
	fprintf(f, "guest cycles: %llu main + %llu IRQ = %llu (%llu per frame), %llu instructions\n",
	        (unsigned long long)r->main_cycles, (unsigned long long)r->irq_cycles,
	        (unsigned long long)total, (unsigned long long)(total / frames),
	        (unsigned long long)r->instructions);
	fprintf(f, "%d routines/pages, %d named by label, %d with an unestablished bank (*)\n", r->count,
	        r->named_rows, r->ambiguous_rows);
	// cyc/ins: no 6502 instruction takes fewer than 2 cycles, so a figure near
	// 1 means the CPU sat in WAI there, ticking one cycle per step -- waiting,
	// not working.
	fprintf(f, "%6s %6s %12s %12s %10s %7s  %s\n", "self%", "irq%", "main cycles", "irq cycles",
	        "per frame", "cyc/ins", "routine");
	for (int i = 0; i < r->count && i < top; i++) {
		const prof_row_t *row = &r->rows[i];
		const uint64_t    t   = row->main_cycles + row->irq_cycles;
		char              where[24] = "";
		if (row->named && row->region == PH_REGION_RAM)
			snprintf(where, sizeof where, " [RAM %u]", (unsigned)row->bank);
		fprintf(f, "%6.2f %6.2f %12llu %12llu %10llu %7.2f  %s%s%s%s\n",
		        total ? 100.0 * (double)t / (double)total : 0.0,
		        total ? 100.0 * (double)row->irq_cycles / (double)total : 0.0,
		        (unsigned long long)row->main_cycles, (unsigned long long)row->irq_cycles,
		        (unsigned long long)(t / frames),
		        row->instructions ? (double)t / (double)row->instructions : 0.0, row->name, where,
		        row->ambiguous ? " *" : "",
		        row->instructions && t < row->instructions * 3 / 2 ? "  (waiting: WAI)" : "");
	}
	fflush(f);
}

uint32_t
profile_session_recorder_bit(const char *name)
{
	if (!name)
		return 0;
	if (!strcmp(name, "hist"))   return CAP_REC_HIST;
	if (!strcmp(name, "budget")) return CAP_REC_BUDGET;
	if (!strcmp(name, "vram"))   return CAP_REC_VRAM;
	return 0;
}

const char *
profile_session_recorder_name(uint32_t bit)
{
	switch (bit) {
		case CAP_REC_HIST:   return "hist";
		case CAP_REC_BUDGET: return "budget";
		case CAP_REC_VRAM:   return "vram";
	}
	return "?";
}
