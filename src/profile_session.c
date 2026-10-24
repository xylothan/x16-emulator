// Commander X16 Emulator — capture mode, wired to the emulator. See
// profile_session.h.

#include "profile_session.h"

#include "dbg_info.h"
#include "host_prof.h"
#include "perf_budget.h"
#include "perf_hist.h"
#include "profile_cct.h"
#include "memory.h"
#include "cpu/irq_ctx.h"
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

static bool cct_ready;      // pool allocated (once; reused by every capture)
static bool tree_consistent = true;

// Pool size: one node per distinct call path. A large program has a few
// thousand; past this, new paths fold into [other] under their parent.
#define TREE_NODES 65536

static uint16_t
peek16(uint16_t a)
{
	return (uint16_t)(debug_read6502(a, 0, USE_CURRENT_X16_BANK) |
	                  (debug_read6502((uint16_t)(a + 1), 0, USE_CURRENT_X16_BANK) << 8));
}

// A JSR whose target is a JMP stub -- a jump table entry, or the KERNAL's
// jmpfr, through which every jsrfar from every ROM bank reaches its target --
// is keyed by where the JMP goes, so the tree names the routine that runs
// rather than the stub. Read now, which is exact: the JMP executes next, with
// the same banks and the same pointer. A few hops at most, in case stubs
// chain.
static uint16_t
through_jmp_stubs(uint16_t target)
{
	for (int hop = 0; hop < 4; hop++) {
		const uint8_t op = debug_read6502(target, 0, USE_CURRENT_X16_BANK);
		if (op == 0x4C)      // JMP abs
			target = peek16((uint16_t)(target + 1));
		else if (op == 0x6C) // JMP (abs); the 65C02 has no page-wrap bug
			target = peek16(peek16((uint16_t)(target + 1)));
		else
			break;
	}
	return target;
}

static void
flow_hook(int kind, uint32_t pc, uint16_t sp, uint16_t ret)
{
	(void)ret;
	switch (kind) {
		case CPU_FLOW_CALL:
			cct_call(perf_hist_key(through_jmp_stubs((uint16_t)pc), memory_get_ram_bank(),
			                       memory_get_rom_bank()),
			         sp);
			break;
		case CPU_FLOW_RETURN:
			cct_return(sp);
			break;
		case CPU_FLOW_IRQ_ENTER:
			cct_irq_enter(perf_hist_key((uint16_t)pc, memory_get_ram_bank(), memory_get_rom_bank()),
			              sp);
			break;
		case CPU_FLOW_IRQ_LEAVE:
			cct_irq_leave(sp);
			break;
	}
}

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
	if (mask & CAP_REC_TREE) {
		// The tree is fed from the histogram's step, so it cannot run alone.
		if (!(mask & CAP_REC_HIST))
			ok = false;
		if (!cct_ready)
			cct_ready = cct_init(TREE_NODES);
		ok = cct_ready && ok;
		if (ok) {
			cct_reset();
			perf_hist_tree = true;
			cpu_flow_hook  = flow_hook;
		}
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
	if (mask & CAP_REC_TREE) {
		cpu_flow_hook  = NULL;
		perf_hist_tree = false;
	}
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

	tree_consistent = true;
	if (have_report && (cap_recorders() & CAP_REC_TREE) && cct_ready) {
		cct_finish();
		const int    nn    = cct_node_count();
		prof_node_t *nodes = malloc(sizeof *nodes * (size_t)(nn > 0 ? nn : 1));
		for (int i = 0; nodes && i < nn; i++) {
			cct_node_t c;
			cct_node_get(i, &c);
			nodes[i] = (prof_node_t){c.parent, -1, c.key, c.self, c.incl, c.calls, c.call_max,
			                         c.frame_max};
		}
		if (nodes) {
			prof_report_add_tree(&report, nodes, nn);
			free(nodes);
			// Self time in the tree is the same cycles as the histogram, split
			// by call path instead of by address: the totals must agree.
			tree_consistent = report.nodes &&
			                  report.nodes[CCT_NODE_MAIN].incl == report.main_cycles &&
			                  report.nodes[CCT_NODE_IRQ].incl == report.irq_cycles;
		}
	}
}

void
profile_session_node_name(const prof_report_t *r, int i, char *buf, int size)
{
	if (!r || i < 0 || i >= r->node_count) {
		snprintf(buf, (size_t)size, "?");
		return;
	}
	const prof_node_t *n = &r->nodes[i];
	if (n->key == CCT_KEY_MAIN) {
		snprintf(buf, (size_t)size, "[main]");
	} else if (n->key == CCT_KEY_IRQ) {
		snprintf(buf, (size_t)size, "[IRQ]");
	} else if (n->key == CCT_KEY_OTHER) {
		snprintf(buf, (size_t)size, "[other]");
	} else {
		uint8_t  region, bank;
		uint16_t pc;
		perf_hist_key_decode(n->key, &region, &bank, &pc);
		const prof_row_t *row = n->row >= 0 ? &r->rows[n->row] : NULL;
		if (row && row->named && row->addr == pc)
			snprintf(buf, (size_t)size, "%s", row->name);
		else if (row && row->named)
			snprintf(buf, (size_t)size, "%s+%u", row->name, (unsigned)(pc - row->addr));
		else if (region == PH_REGION_RAM)
			snprintf(buf, (size_t)size, "RAM %u:$%04X", (unsigned)bank, (unsigned)pc);
		else if (region == PH_REGION_ROM)
			snprintf(buf, (size_t)size, "ROM %u:$%04X", (unsigned)bank, (unsigned)pc);
		else
			snprintf(buf, (size_t)size, "$%04X", (unsigned)pc);
	}
}

bool
profile_session_tree_consistent(void)
{
	return tree_consistent;
}

// The tree, top-down, heaviest first: every node of at least `min` cycles
// inclusive, until `lines` runs out.
static void
print_tree(FILE *f, const prof_report_t *r, const int *first, const int *next, int node,
           int depth, uint64_t min, uint64_t total, uint32_t frames, int *lines)
{
	if (*lines <= 0)
		return;
	const prof_node_t *n = &r->nodes[node];
	char name[PROF_NAME_MAX + 16];
	profile_session_node_name(r, node, name, sizeof name);
	fprintf(f, "%6.2f %6.2f %8u %10u %10llu  %*s%s\n",
	        total ? 100.0 * (double)n->incl / (double)total : 0.0,
	        total ? 100.0 * (double)n->self / (double)total : 0.0, n->calls, n->call_max,
	        (unsigned long long)(n->incl / frames), depth * 2, "", name);
	(*lines)--;
	// Children heaviest first, each printed once: repeatedly take the
	// heaviest one lighter than the last (ties broken by index).
	uint64_t last_incl = UINT64_MAX;
	int      last_idx  = -1;
	for (;;) {
		int best = -1;
		for (int c = first[node]; c >= 0; c = next[c]) {
			const uint64_t w = r->nodes[c].incl;
			if (w < min)
				continue;
			if (w > last_incl || (w == last_incl && c <= last_idx))
				continue;
			if (best < 0 || w > r->nodes[best].incl ||
			    (w == r->nodes[best].incl && c < best))
				best = c;
		}
		if (best < 0)
			break;
		last_incl = r->nodes[best].incl;
		last_idx  = best;
		print_tree(f, r, first, next, best, depth + 1, min, total, frames, lines);
	}
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
	const bool tree = r->node_count > 2;
	if (tree)
		fprintf(f, "%6s %6s %6s %12s %12s %10s %8s %7s  %s\n", "self%", "incl%", "irq%",
		        "main cycles", "irq cycles", "per frame", "calls", "cyc/ins", "routine");
	else
		fprintf(f, "%6s %6s %12s %12s %10s %7s  %s\n", "self%", "irq%", "main cycles",
		        "irq cycles", "per frame", "cyc/ins", "routine");
	for (int i = 0; i < r->count && i < top; i++) {
		const prof_row_t *row = &r->rows[i];
		const uint64_t    t   = row->main_cycles + row->irq_cycles;
		char              where[24] = "";
		if (row->named && row->region == PH_REGION_RAM)
			snprintf(where, sizeof where, " [RAM %u]", (unsigned)row->bank);
		if (tree)
			fprintf(f, "%6.2f %6.2f ", total ? 100.0 * (double)t / (double)total : 0.0,
			        total ? 100.0 * (double)row->incl_cycles / (double)total : 0.0);
		else
			fprintf(f, "%6.2f ", total ? 100.0 * (double)t / (double)total : 0.0);
		fprintf(f, "%6.2f %12llu %12llu %10llu ",
		        total ? 100.0 * (double)row->irq_cycles / (double)total : 0.0,
		        (unsigned long long)row->main_cycles, (unsigned long long)row->irq_cycles,
		        (unsigned long long)(t / frames));
		if (tree)
			fprintf(f, "%8u ", row->calls);
		fprintf(f, "%7.2f  %s%s%s%s\n",
		        row->instructions ? (double)t / (double)row->instructions : 0.0, row->name, where,
		        row->ambiguous ? " *" : "",
		        row->instructions && t < row->instructions * 3 / 2 ? "  (waiting: WAI)" : "");
	}

	if (r->node_count > 2) {
		int *first = malloc(sizeof *first * (size_t)r->node_count);
		int *next  = malloc(sizeof *next * (size_t)r->node_count);
		if (first && next) {
			for (int i = 0; i < r->node_count; i++)
				first[i] = next[i] = -1;
			for (int i = r->node_count - 1; i >= 0; i--) {
				const int p = r->nodes[i].parent;
				if (p >= 0) {
					next[i]  = first[p];
					first[p] = i;
				}
			}
			fprintf(f, "\ncall tree: %d nodes%s; self totals %s the histogram\n", r->node_count,
			        cct_overflowed() ? " (pool full: some paths in [other])" : "",
			        tree_consistent ? "match" : "DO NOT MATCH");
			fprintf(f, "%6s %6s %8s %10s %10s  %s\n", "incl%", "self%", "calls", "worst call",
			        "per frame", "path");
			int            lines = top * 2;
			const uint64_t min   = total / 200; // 0.5%
			print_tree(f, r, first, next, CCT_NODE_MAIN, 0, min, total, frames, &lines);
			print_tree(f, r, first, next, CCT_NODE_IRQ, 0, min, total, frames, &lines);
		}
		free(first);
		free(next);
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
	if (!strcmp(name, "tree"))   return CAP_REC_TREE;
	return 0;
}

const char *
profile_session_recorder_name(uint32_t bit)
{
	switch (bit) {
		case CAP_REC_HIST:   return "hist";
		case CAP_REC_BUDGET: return "budget";
		case CAP_REC_VRAM:   return "vram";
		case CAP_REC_TREE:   return "tree";
	}
	return "?";
}
