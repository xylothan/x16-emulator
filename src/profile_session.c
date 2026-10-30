// Commander X16 Emulator — capture mode, wired to the emulator. See
// profile_session.h.

#include "profile_session.h"

#include "dbg_info.h"
#include "host_prof.h"
#include "perf_budget.h"
#include "perf_hist.h"
#include "profile_cct.h"
#include "rom_syms.h"
#include "waterfall.h"
#include "io_trace.h"
#include "video.h"
#include "glue.h"
#include "memory.h"
#include "cpu/irq_ctx.h"
#include "vera_bandwidth.h"
#include "profile_run.h"
#include "dbg_load.h"
#include "git_rev.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static prof_report_t report;
static bool          have_report;
static uint32_t      report_frames;
static uint32_t      report_generation;
static uint32_t      recording_generation;
// dbg_info_changes() when the report was last named.
static unsigned      report_dbg_changes;

// A saved run being shown in place of the last capture (profile_run.h). While
// one is, names, source lines and the clock come from it, not from the
// emulator as it is now.
static bool            run_loaded;
static prof_run_meta_t run_meta;
static prof_run_key_t *run_keys;
static int             run_nkeys;
static char            run_path[1024];

static void
unload_run(void)
{
	for (int i = 0; run_keys && i < run_nkeys; i++)
		free(run_keys[i].source);
	free(run_keys);
	run_keys   = NULL;
	run_nkeys  = 0;
	run_loaded = false;
}

// The report row a key belongs to, from the run when one is shown.
static int
row_of(uint32_t key)
{
	if (!have_report)
		return -1;
	if (run_loaded) {
		const prof_run_key_t *k = prof_run_find_key(run_keys, run_nkeys, key);
		return k ? k->row : -1;
	}
	return prof_report_row_of_key(&report, key);
}

// Whether perf_hist was armed by someone else (a DAP client) before the
// capture, so ending the capture leaves it as it was found.
static bool hist_was_enabled;

static prof_budget_summary_t budget_summary;

// Each vsync's CPU work over the capture, oldest first, so the summary can be
// judged again when the target frame rate changes. Owned here; from a saved
// run when one is shown.
static uint32_t *vsync_work;
static uint32_t  vsync_n;
static uint32_t  vsync_cycles; // one vsync's cycles when it was recorded
static float     judged_fps = -1.0f;

// Judge the kept work against the current target: a game frame is `span`
// vsyncs and its work their sum -- the rolling sum perf_budget keeps -- and
// it is over when that passes perf_budget's own threshold, so this and the
// Performance panel agree about the same frames.
static void
judge(void)
{
	const float fps = perf_budget_get_target_fps();
	judged_fps      = fps;
	if (!budget_summary.have || !vsync_work || !vsync_n)
		return;
	const uint32_t      khz = (uint32_t)(profile_session_mhz() * 1000.0 + 0.5);
	const perf_period_t p   = perf_budget_period(khz, vsync_cycles ? vsync_cycles : perf_budget_cycles_per_frame(), fps);
	const uint32_t      n   = (uint32_t)p.frames;

	budget_summary.span       = p.frames;
	budget_summary.budget     = p.cycles;
	budget_summary.target_fps = fps;
	budget_summary.rejudged   = true;
	budget_summary.frame_max  = 0;
	budget_summary.overruns   = 0;
	uint64_t sum = 0;
	for (uint32_t i = 0; i < vsync_n; i++) {
		sum += vsync_work[i];
		if (i >= n)
			sum -= vsync_work[i - n];
		// A window is a whole game frame once it holds `n` vsyncs; a capture
		// shorter than one game frame is judged on what it has.
		if (i + 1 < n && i + 1 < vsync_n)
			continue;
		const uint32_t w = sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sum;
		if (w > budget_summary.frame_max)
			budget_summary.frame_max = w;
		if (w > p.threshold)
			budget_summary.overruns++;
	}
}

static void
forget_vsync_work(void)
{
	free(vsync_work);
	vsync_work   = NULL;
	vsync_n      = 0;
	vsync_cycles = 0;
	judged_fps   = -1.0f;
}

static bool cct_ready;      // pool allocated (once; reused by every capture)
static bool wf_ready;       // likewise, the waterfall's frame buffers
static bool wf_recorded;    // the last capture ran the waterfall

// Every frame's summary is kept. Its detail -- each write and call -- is
// recorded up to WF_FRAME_CAP events a frame, and kept for every frame within
// WF_BUDGET in all, thinned evenly past that (see waterfall.h).
#define WF_FRAME_CAP 32768
#ifdef __EMSCRIPTEN__
#define WF_BUDGET ((size_t)32 << 20)
#else
#define WF_BUDGET ((size_t)128 << 20)
#endif
// The extremes keep their detail only while they fit: 4 * WF_PROTECT frames
// of WF_FRAME_CAP events (about 12.6 MB) must stay well under WF_BUDGET.
_Static_assert((size_t)4 * WF_PROTECT * WF_FRAME_CAP * sizeof(wf_event_t) < WF_BUDGET / 2,
               "the waterfall budget must hold every protected frame");
// Frames a trace export carries: enough to compare, few enough to load.
#define WF_TRACE_MAX 64

// The waterfall ranks frames by CPU work, which the budget recorder measures.
// When the capture did not ask for it, the waterfall arms it and disarms it.
static bool wf_armed_budget;

static uint8_t ym_selected; // the YM register $9F40 last selected

// The I/O page, sorted into lanes. VERA's data ports are left to the VRAM
// tap, which knows where in VRAM each byte went.
static void
io_tap(uint16_t a, uint8_t v)
{
	if (a < 0x9F10) {
		wf_note(WF_LANE_VIA1, a & 0xF, v, 0);
	} else if (a < 0x9F20) {
		wf_note(WF_LANE_VIA2, a & 0xF, v, 0);
	} else if (a < 0x9F40) {
		const uint8_t r = a & 0x1F;
		if (r == 0x03 || r == 0x04)
			return;
		wf_note(r == 0x1D ? WF_LANE_PCM : r >= 0x1E ? WF_LANE_SPI : WF_LANE_VERA_REG, r, v, 0);
	} else if (a < 0x9F60) {
		if ((a & 1) == 0) // register select: becomes the target of the next data write
			ym_selected = v;
		else
			wf_note(WF_LANE_YM, ym_selected, v, 0);
	} else {
		wf_note(WF_LANE_IO_OTHER, a, v, 0);
	}
}

static void
vram_tap(uint32_t address, uint8_t value, video_vram_region_t region, bool fx4)
{
	static const uint8_t lane[] = {WF_LANE_PSG,    WF_LANE_PALETTE,  WF_LANE_SPRITES, WF_LANE_L0_MAP,
	                               WF_LANE_L0_TILES, WF_LANE_L1_MAP, WF_LANE_L1_TILES, WF_LANE_VRAM_OTHER};
	const uint8_t flags = (uint8_t)(((address >> 16) & 1 ? WF_F_VRAM_HI : 0) | (fx4 ? WF_F_FX4 : 0));
	wf_note(region <= VIDEO_VRAM_OTHER ? lane[region] : WF_LANE_VRAM_OTHER, (uint16_t)address, value,
	        flags);
}

static void
cct_switch(int node)
{
	wf_cpu((uint16_t)node, perf_hist_total_cycles());
}

// The CPU's instruction count (fake6502.c), for telling a held line's gaps.
static uint32_t
instr_now(void)
{
	extern uint32_t instructions;
	return instructions;
}

// An interrupt line is up. Inside a handler that is the request being
// served, not one being kept waiting, so only main-line code reports.
static void
irq_line_hook(bool held)
{
	if (cpu_irq_depth() == 0)
		wf_irq_line(held, perf_hist_total_cycles(), instr_now());
}
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
	unload_run();
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
	if (mask & CAP_REC_WATERFALL) {
		// Writes are stamped from the histogram's step.
		if (!(mask & CAP_REC_HIST))
			ok = false;
		if (!wf_ready)
			wf_ready = wf_init(WF_FRAME_CAP, WF_BUDGET);
		ok = wf_ready && ok;
		if (ok) {
			wf_reset();
			ym_selected          = 0;
			io_write_tap         = io_tap;
			video_vram_write_tap = vram_tap;
			cpu_irq_line_hook    = irq_line_hook;
			if (mask & CAP_REC_TREE)
				cct_switch_hook = cct_switch;
		}
	}
	wf_recorded = (mask & CAP_REC_WATERFALL) && ok;
	wf_armed_budget = wf_recorded && !(mask & CAP_REC_BUDGET);
	if ((mask & CAP_REC_BUDGET) || wf_armed_budget)
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
	if (mask & CAP_REC_WATERFALL) {
		io_write_tap         = NULL;
		video_vram_write_tap = NULL;
		cpu_irq_line_hook    = NULL;
		cct_switch_hook      = NULL;
	}
	if (mask & CAP_REC_TREE) {
		cpu_flow_hook  = NULL;
		perf_hist_tree = false;
	}
	if (mask & CAP_REC_HIST)
		perf_hist_arm(hist_was_enabled);
	if ((mask & CAP_REC_BUDGET) || ((mask & CAP_REC_WATERFALL) && wf_armed_budget))
		perf_budget_arm(PERF_OWNER_CAPTURE, false);
	if (mask & CAP_REC_WATERFALL)
		wf_armed_budget = false;
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

// The labels a report is named from: the program's debug info and the ROM's
// symbols. Names point into those; free() the array only. NULL (and *count 0)
// when there are none.
static prof_sym_t *
symbol_table(int *count)
{
	const int   n    = dbg_info_symbol_count();
	const int   nrom = rom_syms_count();
	prof_sym_t *syms = n + nrom > 0 ? calloc((size_t)(n + nrom), sizeof *syms) : NULL;
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
	for (int i = 0; syms && i < nrom; i++) {
		const rom_sym_t *r = rom_syms_at(i);
		syms[ns++] = (prof_sym_t){r->name, r->addr, r->bank, true};
	}
	*count = ns;
	return syms;
}

static void
finished(cap_end_t why)
{
	(void)why;
	if (have_report)
		prof_report_free(&report);
	have_report = false;
	report_generation++;
	recording_generation++;
	report_dbg_changes = dbg_info_changes();

	// Work against waiting, from the budget recorder's ring, newest first:
	// the capture's frames are the most recent ones.
	memset(&budget_summary, 0, sizeof budget_summary);
	forget_vsync_work();
	if (cap_recorders() & CAP_REC_BUDGET) {
		int want = (int)cap_frames();
		if (want > perf_budget_frame_count())
			want = perf_budget_frame_count();
		vsync_work   = want > 0 ? malloc(sizeof *vsync_work * (size_t)want) : NULL;
		vsync_cycles = perf_budget_cycles_per_frame();
		for (int i = 0; i < want; i++) {
			perf_frame_t fr;
			if (!perf_budget_frame_at(i, &fr))
				break;
			const uint32_t work = perf_frame_work(&fr);
			budget_summary.frames++;
			budget_summary.total += fr.total;
			budget_summary.idle += fr.idle;
			if (work > budget_summary.work_max)
				budget_summary.work_max = work;
			// The ring is newest first; kept oldest first.
			if (vsync_work)
				vsync_work[want - 1 - i] = work;
		}
		if (vsync_work && budget_summary.frames == (uint32_t)want) {
			vsync_n = (uint32_t)want;
		} else {
			free(vsync_work);
			vsync_work = NULL;
		}
		budget_summary.have = budget_summary.frames > 0;
		judge();
	}
	if (!(cap_recorders() & CAP_REC_HIST))
		return;

	int         ns;
	prof_sym_t *syms = symbol_table(&ns);
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

const prof_budget_summary_t *
profile_session_budget_summary(void)
{
	if (judged_fps != perf_budget_get_target_fps())
		judge();
	return &budget_summary;
}

float
profile_session_recorded_target_fps(void)
{
	return run_loaded ? run_meta.b_target_fps : 0.0f;
}

int
profile_session_export_folded(const char *path)
{
	const prof_report_t *r = profile_session_report();
	if (!r || r->node_count <= 2 || !path)
		return -1;
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	int lines = 0;
	int chain[512];
	for (int i = 0; i < r->node_count; i++) {
		if (!r->nodes[i].self)
			continue;
		int depth = 0;
		for (int n = i; n >= 0 && depth < 512; n = r->nodes[n].parent)
			chain[depth++] = n;
		for (int d = depth - 1; d >= 0; d--) {
			char name[PROF_NAME_MAX + 16];
			profile_session_node_name(r, chain[d], name, sizeof name);
			// ';' separates frames in this format; no symbol has one, but a
			// stray one would split a frame in two.
			for (char *c = name; *c; c++)
				if (*c == ';')
					*c = ',';
			fprintf(f, "%s%s", name, d ? ";" : "");
		}
		fprintf(f, " %llu\n", (unsigned long long)r->nodes[i].self);
		lines++;
	}
	fclose(f);
	return lines;
}

static void
json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(f, "\\u%04x", (unsigned char)*s);
		else
			fputc(*s, f);
	}
	fputc('"', f);
}


// ─── Waterfall export ───────────────────────────────────────────────────────

// A key's routine name, from the report's rows; or its address.
static void
key_name(uint32_t key, char *buf, int size)
{
	const int r = row_of(key);
	if (r >= 0) {
		snprintf(buf, (size_t)size, "%s", report.rows[r].name);
		return;
	}
	uint8_t  region, bank;
	uint16_t pc;
	perf_hist_key_decode(key, &region, &bank, &pc);
	if (region == PH_REGION_RAM)
		snprintf(buf, (size_t)size, "RAM %u:$%04X", (unsigned)bank, (unsigned)pc);
	else if (region == PH_REGION_ROM)
		snprintf(buf, (size_t)size, "ROM %u:$%04X", (unsigned)bank, (unsigned)pc);
	else
		snprintf(buf, (size_t)size, "$%04X", (unsigned)pc);
}

static void
key_where(uint32_t key, char *buf, int size)
{
	uint8_t  region, bank;
	uint16_t pc;
	perf_hist_key_decode(key, &region, &bank, &pc);
	if (region == PH_REGION_RAM)
		snprintf(buf, (size_t)size, "RAM %u:$%04X", (unsigned)bank, (unsigned)pc);
	else if (region == PH_REGION_ROM)
		snprintf(buf, (size_t)size, "ROM %u:$%04X", (unsigned)bank, (unsigned)pc);
	else
		snprintf(buf, (size_t)size, "$%04X", (unsigned)pc);
}

static int
cmp_writes_row(const void *a, const void *b)
{
	const prof_writes_t *x = a, *y = b;
	if (x->row != y->row)
		return x->row < y->row ? -1 : 1;
	if (x->row < 0 && x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->lane - y->lane;
}

static int
cmp_writes_heavy(const void *a, const void *b)
{
	const prof_writes_t *x = a, *y = b;
	if (x->writes != y->writes)
		return x->writes < y->writes ? 1 : -1;
	return cmp_writes_row(a, b);
}

const prof_writes_t *
profile_session_writes_by_routine(int *count)
{
	static prof_writes_t *out;
	*count = 0;
	free(out);
	out = NULL;
	if (!profile_session_has_waterfall())
		return NULL;
	int             na;
	const wf_agg_t *a = wf_aggregates(&na);
	out = malloc(sizeof *out * (size_t)(na ? na : 1));
	if (!out)
		return NULL;
	for (int i = 0; i < na; i++)
		out[i] = (prof_writes_t){row_of(a[i].key),
		                         a[i].lane, a[i].key, a[i].writes};
	// Merge entries for the same (row, lane): many instructions, one routine.
	qsort(out, (size_t)na, sizeof *out, cmp_writes_row);
	int n = 0;
	for (int i = 0; i < na; i++) {
		if (n && cmp_writes_row(&out[n - 1], &out[i]) == 0)
			out[n - 1].writes += out[i].writes;
		else
			out[n++] = out[i];
	}
	qsort(out, (size_t)n, sizeof *out, cmp_writes_heavy);
	*count = n;
	return out;
}

// Source lines, looked up once per key: an export can hold 100k writes from a
// few hundred instructions.
#define SRC_CACHE 8192
typedef struct {
	uint32_t key;
	bool     used;
	char    *text; // "file:line", or "" when the debug info has none
} src_entry_t;

static const char *
key_source(src_entry_t *cache, uint32_t key)
{
	if (run_loaded) {
		const prof_run_key_t *k = prof_run_find_key(run_keys, run_nkeys, key);
		return k && k->source ? k->source : "";
	}
	uint32_t h = (key * 0x9E3779B1u) & (SRC_CACHE - 1);
	for (int probe = 0; probe < SRC_CACHE; probe++, h = (h + 1) & (SRC_CACHE - 1)) {
		if (cache[h].used && cache[h].key == key)
			return cache[h].text ? cache[h].text : "";
		if (!cache[h].used)
			break;
	}
	uint8_t  region, bank;
	uint16_t pc;
	perf_hist_key_decode(key, &region, &bank, &pc);
	const char *file = NULL;
	int         line = 0;
	char        tmp[300] = "";
	if (region != PH_REGION_ROM &&
	    dbg_info_addr_to_source_banked_ex(pc, region == PH_REGION_RAM ? bank : -1, &file, &line) !=
	        DBG_BANK_NO_MATCH &&
	    file) {
		const char *base = strrchr(file, '/');
		const char *b2   = strrchr(file, '\\');
		if (b2 && (!base || b2 > base))
			base = b2;
		snprintf(tmp, sizeof tmp, "%s:%d", base ? base + 1 : file, line);
	}
	if (!cache[h].used) {
		cache[h].used = true;
		cache[h].key  = key;
		cache[h].text = malloc(strlen(tmp) + 1);
		if (cache[h].text)
			strcpy(cache[h].text, tmp);
		return cache[h].text ? cache[h].text : "";
	}
	return ""; // cache full: answer without remembering
}

typedef struct {
	FILE             *f;
	const wf_frame_t *frame;
	double            us_per_cycle;
	src_entry_t      *src;
	long              events;
} trace_ctx_t;

static int
trace_parent(void *ctx, int node)
{
	(void)ctx;
	return (have_report && node >= 0 && node < report.node_count) ? report.nodes[node].parent : -1;
}

static void
trace_emit(void *vctx, wf_out_kind_t kind, int thread, uint32_t cycle, int node, const wf_event_t *ev)
{
	trace_ctx_t *c  = vctx;
	const double ts = (double)(c->frame->start + cycle) * c->us_per_cycle;
	char         name[PROF_NAME_MAX + 32];
	if (kind == WF_OUT_MARK) {
		fprintf(c->f, ",\n{\"ph\":\"%c\",\"pid\":1,\"tid\":3,\"ts\":%.3f,\"name\":\"IRQ held off\"}",
		        ev->target == WF_MARK_HELD_ON ? 'B' : 'E', ts);
		c->events++;
		return;
	}
	if (kind != WF_OUT_EVENT) {
		profile_session_node_name(&report, node, name, sizeof name);
		fprintf(c->f, ",\n{\"ph\":\"%c\",\"pid\":1,\"tid\":%d,\"ts\":%.3f,\"name\":",
		        kind == WF_OUT_BEGIN ? 'B' : 'E', thread, ts);
		json_str(c->f, name);
		fputc('}', c->f);
		c->events++;
		return;
	}
	const int      lane = wf_event_lane(ev);
	const uint32_t key  = wf_event_key(ev);
	char           what[48], routine[PROF_NAME_MAX + 16], where[32];
	if (ev->flags & WF_F_VRAM_HI || (lane >= WF_LANE_PSG && lane <= WF_LANE_VRAM_OTHER))
		snprintf(what, sizeof what, "%s $%X%04X%s", wf_lane_name(lane),
		         (ev->flags & WF_F_VRAM_HI) ? 1u : 0u, (unsigned)ev->target,
		         (ev->flags & WF_F_FX4) ? " x4" : "");
	else
		snprintf(what, sizeof what, "%s $%02X", wf_lane_name(lane), (unsigned)ev->target);
	key_name(key, routine, sizeof routine);
	key_where(key, where, sizeof where);
	fprintf(c->f, ",\n{\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":%d,\"ts\":%.3f,\"name\":", 10 + lane,
	        ts);
	json_str(c->f, what);
	fprintf(c->f, ",\"args\":{\"value\":\"$%02X\",\"routine\":", (unsigned)ev->value);
	json_str(c->f, routine);
	fprintf(c->f, ",\"pc\":");
	json_str(c->f, where);
	const char *src = key_source(c->src, key);
	if (src[0]) {
		fprintf(c->f, ",\"source\":");
		json_str(c->f, src);
	}
	fprintf(c->f, "%s}}", (ev->flags & WF_F_IRQ) ? ",\"irq\":true" : "");
	c->events++;
}

// The frames a trace export carries, at most `max`, in frame order: the
// worst and best by each measure first, then others spread evenly. Only
// frames whose detail was kept.
static int
trace_frames(int *out, int max)
{
	const int n = wf_frame_count();
	if (max <= 0 || n == 0)
		return 0;
	char *in = calloc((size_t)n, 1);
	if (!in)
		return 0;
	int got = 0;
	for (int m = WF_BY_WORK; m <= WF_BY_WRITES; m++)
		for (int p = WF_PICK_WORST; p <= WF_PICK_BEST; p++) {
			int ext[WF_PROTECT];
			const int k = wf_pick(p, m, ext, WF_PROTECT);
			for (int e = 0; e < k && got < max; e++)
				if (!in[ext[e]] && wf_frame(ext[e])->events) {
					in[ext[e]]  = 1;
					out[got++] = ext[e];
				}
		}
	const int room = max - got, have = wf_detail_count();
	for (int s = 0; room > 0 && s < room; s++) {
		// The s-th of `room` evenly spaced points, to the nearest kept frame.
		const int at = wf_nearest_detail((int)((double)s * n / room));
		if (at >= 0 && !in[at] && got < max && got < have) {
			in[at]     = 1;
			out[got++] = at;
		}
	}
	got = 0;
	for (int i = 0; i < n; i++)
		if (in[i])
			out[got++] = i;
	free(in);
	return got;
}

double
profile_session_mhz(void)
{
	if (run_loaded && run_meta.mhz > 0)
		return run_meta.mhz;
	return MHZ ? (double)MHZ : 8.0;
}

void
profile_session_key_name(uint32_t key, char *buf, int size)
{
	key_name(key, buf, size);
}

void
profile_session_key_where(uint32_t key, char *buf, int size)
{
	key_where(key, buf, size);
}

bool
profile_session_key_source(uint32_t key, char *buf, int size)
{
	if (run_loaded) {
		const prof_run_key_t *k = prof_run_find_key(run_keys, run_nkeys, key);
		snprintf(buf, (size_t)size, "%s", k && k->source ? k->source : "");
		return buf[0] != 0;
	}
	uint8_t  region, bank;
	uint16_t pc;
	perf_hist_key_decode(key, &region, &bank, &pc);
	const char *file = NULL;
	int         line = 0;
	buf[0]           = 0;
	if (region == PH_REGION_ROM ||
	    dbg_info_addr_to_source_banked_ex(pc, region == PH_REGION_RAM ? bank : -1, &file, &line) ==
	        DBG_BANK_NO_MATCH ||
	    !file)
		return false;
	const char *base = strrchr(file, '/');
	const char *b2   = strrchr(file, '\\');
	if (b2 && (!base || b2 > base))
		base = b2;
	snprintf(buf, (size_t)size, "%s:%d", base ? base + 1 : file, line);
	return true;
}

int
profile_session_export_trace(const char *path)
{
	if (!profile_session_has_waterfall() || !path)
		return -1;
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	src_entry_t *cache = calloc(SRC_CACHE, sizeof *cache);

	// The frames: the extremes, then others spread evenly, in order.
	int       which[WF_TRACE_MAX];
	const int n = trace_frames(which, WF_TRACE_MAX);
	const wf_frame_t *frames[WF_TRACE_MAX];
	for (int i = 0; i < n; i++)
		frames[i] = wf_frame(which[i]);

	fprintf(f, "{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n");
	fprintf(f, "{\"ph\":\"M\",\"pid\":1,\"name\":\"process_name\",\"args\":{\"name\":\"X16\"}}");
	static const char *const fixed[] = {"CPU", "IRQ", "Frames", "IRQ held off"};
	for (int t = 0; t < 4; t++)
		fprintf(f, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_name\",\"args\":{\"name\":\"%s\"}}"
		           ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_sort_index\",\"args\":{\"sort_index\":%d}}",
		        t, fixed[t], t, t);
	for (int l = 1; l < WF_LANES; l++)
		fprintf(f, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_name\",\"args\":{\"name\":\"%s\"}}"
		           ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_sort_index\",\"args\":{\"sort_index\":%d}}",
		        10 + l, wf_lane_name(l), 10 + l, 10 + l);

	trace_ctx_t c = {f, NULL, 1.0 / profile_session_mhz(), cache, 0};
	for (int i = 0; i < n; i++) {
		c.frame = frames[i];
		fprintf(f, ",\n{\"ph\":\"X\",\"pid\":1,\"tid\":2,\"ts\":%.3f,\"dur\":%.3f,\"name\":\"frame %u\","
		           "\"args\":{\"cycles\":%u,\"work\":%llu,\"writes\":%u,\"events\":%d,\"truncated\":%s}}",
		        (double)frames[i]->start * c.us_per_cycle, (double)frames[i]->length * c.us_per_cycle,
		        frames[i]->number, frames[i]->length, (unsigned long long)frames[i]->work,
		        frames[i]->writes, frames[i]->count, frames[i]->truncated ? "true" : "false");
		if (have_report && report.node_count > 2)
			wf_walk_frame(frames[i], trace_parent, CCT_NODE_IRQ, trace_emit, &c);
		else
			for (int e = 0; e < frames[i]->count; e++)
				if (wf_event_lane(&frames[i]->events[e]) != WF_LANE_CPU)
					trace_emit(&c, wf_event_is_mark(&frames[i]->events[e]) ? WF_OUT_MARK : WF_OUT_EVENT, 0,
					           frames[i]->events[e].cycle, -1, &frames[i]->events[e]);
	}
	fprintf(f, "\n]}\n");
	fclose(f);
	if (cache) {
		for (int i = 0; i < SRC_CACHE; i++)
			free(cache[i].text);
		free(cache);
	}
	return (int)c.events;
}

int
profile_session_export_json(const char *path)
{
	const prof_report_t *r = profile_session_report();
	if (!r || !path)
		return -1;
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	static const char *const regions[] = {"low", "ram", "rom"};
	fprintf(f, "{\n  \"frames\": %u,\n  \"mainCycles\": %llu,\n  \"irqCycles\": %llu,\n"
	           "  \"instructions\": %llu,\n",
	        report_frames, (unsigned long long)r->main_cycles, (unsigned long long)r->irq_cycles,
	        (unsigned long long)r->instructions);
	const prof_budget_summary_t *bs = profile_session_budget_summary();
	if (bs->have)
		fprintf(f, "  \"budget\": {\"frames\": %u, \"totalCycles\": %llu, \"idleCycles\": %llu, "
		           "\"workMax\": %u, \"overruns\": %u, \"budgetCycles\": %u, \"targetFps\": %.4g, "
		           "\"vsyncsPerFrame\": %d, \"frameMax\": %u},\n",
		        bs->frames, (unsigned long long)bs->total, (unsigned long long)bs->idle, bs->work_max,
		        bs->overruns, bs->budget, (double)bs->target_fps, bs->span, bs->frame_max);
	fprintf(f, "  \"rows\": [\n");
	for (int i = 0; i < r->count; i++) {
		const prof_row_t *row = &r->rows[i];
		fprintf(f, "    {\"name\": ");
		json_str(f, row->name);
		fprintf(f, ", \"region\": \"%s\", \"bank\": %u, \"addr\": %u, \"named\": %s, "
		           "\"mainCycles\": %llu, \"irqCycles\": %llu, \"instructions\": %llu, "
		           "\"inclCycles\": %llu, \"calls\": %u, \"callMax\": %u, \"frameMax\": %u}%s\n",
		        regions[row->region < 3 ? row->region : 0], (unsigned)row->bank,
		        (unsigned)row->addr, row->named ? "true" : "false",
		        (unsigned long long)row->main_cycles, (unsigned long long)row->irq_cycles,
		        (unsigned long long)row->instructions, (unsigned long long)row->incl_cycles,
		        row->calls, row->call_max, row->frame_max, i + 1 < r->count ? "," : "");
	}
	fprintf(f, "  ],\n  \"tree\": [\n");
	for (int i = 0; i < r->node_count; i++) {
		const prof_node_t *n = &r->nodes[i];
		char name[PROF_NAME_MAX + 16];
		profile_session_node_name(r, i, name, sizeof name);
		fprintf(f, "    {\"name\": ");
		json_str(f, name);
		fprintf(f, ", \"parent\": %d, \"row\": %d, \"self\": %llu, \"incl\": %llu, "
		           "\"calls\": %u, \"callMax\": %u, \"frameMax\": %u}%s\n",
		        n->parent, n->row, (unsigned long long)n->self, (unsigned long long)n->incl,
		        n->calls, n->call_max, n->frame_max, i + 1 < r->node_count ? "," : "");
	}
	fprintf(f, "  ],\n  \"treeConsistent\": %s\n}\n", tree_consistent ? "true" : "false");
	fclose(f);
	return r->count + r->node_count;
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
	// The waterfall's frame closes first: ending the capture disarms it.
	if (cap_running() && (cap_recorders() & CAP_REC_WATERFALL) && wf_ready) {
		uint64_t     work = 0; // 0: not measured
		perf_frame_t fr;
		if (((cap_recorders() & CAP_REC_BUDGET) || wf_armed_budget) && perf_budget_last_frame(&fr))
			work = perf_frame_work(&fr);
		wf_frame_set_video_mode(video_get_dc_value(0));
		wf_irq_poll(instr_now());
		wf_frame_end(perf_hist_total_cycles(), work,
		             (uint16_t)((cap_recorders() & CAP_REC_TREE) ? cct_current() : 0));
	}
	cap_frame_end();
	// A program that LOADs its own .dbg (auto-load) or a DAP client's launch
	// changes the names without the debugger ever being drawn.
	profile_session_sync_names();
}

bool
profile_session_has_waterfall(void)
{
	return wf_recorded && wf_ready && wf_frames_seen() > 0;
}

// ─── Saved runs ─────────────────────────────────────────────────────────────

static int
cmp_u32(const void *a, const void *b)
{
	const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

int
profile_session_save_run(const char *path, char *msg, size_t msg_size)
{
	if (!have_report) {
		snprintf(msg, msg_size, "There is no recording to save.");
		return -1;
	}
	if (cap_running()) {
		snprintf(msg, msg_size, "A recording is running; save once it has stopped.");
		return -1;
	}
	// What it was recorded with.
	prof_run_meta_t m;
	if (run_loaded) {
		m = run_meta; // saving a run that was loaded keeps its own story
	} else {
		memset(&m, 0, sizeof m);
		snprintf(m.revision, sizeof m.revision, "%s", GIT_REV);
		snprintf(m.program, sizeof m.program, "%s", dbg_load_prg_path());
		size_t at = 0;
		for (int i = 0; i < dbg_info_loaded_count() && at < sizeof m.dbg; i++)
			at += (size_t)snprintf(m.dbg + at, sizeof m.dbg - at, "%s%s", i ? "; " : "", dbg_info_loaded_at(i));
		snprintf(m.end_reason, sizeof m.end_reason, "%s", cap_end_name(cap_end_reason()));
		m.mhz             = profile_session_mhz();
		m.elapsed_s       = cap_elapsed_s();
		m.frames          = report_frames;
		m.recorders       = cap_recorders();
		m.tree_consistent = tree_consistent;
		m.b_have          = budget_summary.have;
		m.b_frames        = budget_summary.frames;
		m.b_total         = budget_summary.total;
		m.b_idle          = budget_summary.idle;
		m.b_work_max      = budget_summary.work_max;
		m.b_overruns      = budget_summary.overruns;
		m.b_budget        = budget_summary.budget;
		m.b_target_fps    = budget_summary.target_fps;
		m.b_vsync_cycles  = vsync_cycles;
	}
	const time_t now = time(NULL);
	strftime(m.captured, sizeof m.captured, "%Y-%m-%d %H:%M:%S", localtime(&now));

	// Every key the panel can ask about: call targets, writing instructions.
	const bool wf   = profile_session_has_waterfall();
	size_t     cap  = (size_t)(report.nodes ? report.node_count : 0) + 1024, n = 0;
	uint32_t  *list = malloc(sizeof *list * cap);
	if (!list) {
		snprintf(msg, msg_size, "Out of memory.");
		return -1;
	}
#define PUSH(k)                                                                  	do {                                                                         		if (n == cap) {                                                          			uint32_t *g = realloc(list, sizeof *list * cap * 2);                 			if (!g)                                                              				break;                                                           			list = g;                                                            			cap *= 2;                                                            		}                                                                        		list[n++] = (k);                                                         	} while (0)
	for (int i = 0; report.nodes && i < report.node_count; i++)
		if (report.nodes[i].key < PH_KEYS)
			PUSH(report.nodes[i].key);
	if (wf) {
		for (int i = 0; i < wf_frame_count(); i++) {
			const wf_frame_t *fr = wf_frame(i);
			for (int e = 0; fr->events && e < fr->count; e++)
				if (wf_event_lane(&fr->events[e]) != WF_LANE_CPU && !wf_event_is_mark(&fr->events[e]))
					PUSH(wf_event_key(&fr->events[e]));
		}
		int             na;
		const wf_agg_t *a = wf_aggregates(&na);
		for (int i = 0; a && i < na; i++)
			PUSH(a[i].key);
	}
#undef PUSH
	qsort(list, n, sizeof *list, cmp_u32);
	size_t u = 0;
	for (size_t i = 0; i < n; i++)
		if (!u || list[u - 1] != list[i])
			list[u++] = list[i];

	prof_run_key_t *keys = calloc(u ? u : 1, sizeof *keys);
	char           *text = malloc(u ? u * 300 : 1);
	int             rc   = -1;
	if (keys && text) {
		for (size_t i = 0; i < u; i++) {
			keys[i].key    = list[i];
			keys[i].row    = row_of(list[i]);
			keys[i].source = text + i * 300;
			profile_session_key_source(list[i], keys[i].source, 300);
		}
		rc = prof_run_write(path, &m, &report, keys, (int)u, wf, vsync_work, vsync_n, msg, msg_size);
	} else {
		snprintf(msg, msg_size, "Out of memory.");
	}
	free(text);
	free(keys);
	free(list);
	return rc;
}

int
profile_session_load_run(const char *path, char *msg, size_t msg_size)
{
	if (cap_running()) {
		snprintf(msg, msg_size, "A recording is running; open a saved run once it has stopped.");
		return -1;
	}
	prof_run_t run;
	if (prof_run_read(path, &run, msg, msg_size) != 0)
		return -1;
	if (run.wf && !wf_ready)
		wf_ready = wf_init(WF_FRAME_CAP, WF_BUDGET);
	if (run.wf && !wf_ready) {
		prof_run_free(&run);
		snprintf(msg, msg_size, "Out of memory for the run's frames; nothing was loaded.");
		return -1;
	}

	// Everything is read: now it replaces what was shown.
	if (have_report)
		prof_report_free(&report);
	unload_run();
	report          = run.report;
	have_report     = true;
	report_frames   = run.meta.frames;
	tree_consistent = run.meta.tree_consistent;
	memset(&budget_summary, 0, sizeof budget_summary);
	budget_summary.have     = run.meta.b_have;
	budget_summary.frames   = run.meta.b_frames;
	budget_summary.total    = run.meta.b_total;
	budget_summary.idle     = run.meta.b_idle;
	budget_summary.work_max = run.meta.b_work_max;
	budget_summary.overruns = run.meta.b_overruns;
	budget_summary.budget   = run.meta.b_budget;
	budget_summary.target_fps = run.meta.b_target_fps;
	budget_summary.frame_max  = 0;
	budget_summary.span       = 0; // not known until judged again
	forget_vsync_work();
	vsync_work   = run.work; // handed on: judged against the current target
	vsync_n      = run.work ? run.nwork : 0;
	vsync_cycles = run.meta.b_vsync_cycles;
	if (run.wf) {
		wf_install(run.wf);
		wf_recorded = true;
	} else {
		if (wf_ready)
			wf_reset();
		wf_recorded = false;
	}
	run_keys   = run.keys;
	run_nkeys  = run.nkeys;
	run_meta   = run.meta;
	run_loaded = true;
	snprintf(run_path, sizeof run_path, "%s", path);
	report_generation++;
	recording_generation++;
	report_dbg_changes = dbg_info_changes();
	snprintf(msg, msg_size, "Showing %s, saved %s: %u frames.", path, run_meta.captured, run_meta.frames);
	return 0;
}

bool
profile_session_run_loaded(void)
{
	return run_loaded;
}

const prof_run_meta_t *
profile_session_run_meta(void)
{
	return run_loaded ? &run_meta : NULL;
}

const char *
profile_session_run_path(void)
{
	return run_loaded ? run_path : "";
}

const prof_report_t *
profile_session_report(void)
{
	return have_report ? &report : NULL;
}

uint32_t
profile_session_report_generation(void)
{
	return report_generation;
}

uint32_t
profile_session_recording_generation(void)
{
	return recording_generation;
}

void
profile_session_sync_names(void)
{
	// Only a report built from this run's own counts can be regrouped; a
	// saved run's stays as saved, and says so (run_names_stale).
	if (!have_report || run_loaded || !report.hist || dbg_info_changes() == report_dbg_changes)
		return;
	report_dbg_changes = dbg_info_changes();
	int         ns;
	prof_sym_t *syms = symbol_table(&ns);
	if (prof_report_rename(&report, syms, ns, covered))
		report_generation++;
	free(syms);
}

bool
profile_session_run_names_stale(void)
{
	return run_loaded && dbg_info_changes() != report_dbg_changes;
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
	const prof_budget_summary_t *bs = profile_session_budget_summary();
	if (bs->have)
		fprintf(f, "work %.1f%%, waiting %.1f%% (cycle budget); worst frame %u of %u cycles, "
		           "%u over budget\n",
		        bs->total ? 100.0 * (double)(bs->total - bs->idle) / (double)bs->total : 0.0,
		        bs->total ? 100.0 * (double)bs->idle / (double)bs->total : 0.0,
		        bs->rejudged ? bs->frame_max : bs->work_max, bs->budget, bs->overruns);
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

	if (profile_session_has_waterfall()) {
		int                  na;
		const prof_writes_t *a = profile_session_writes_by_routine(&na);
		fprintf(f, "\ndevice writes by routine (%u frames; detail kept for %d%s)\n",
		        wf_frames_seen(), wf_detail_count(),
		        wf_dropped() ? ", some writes dropped" : "");
		fprintf(f, "%10s %10s  %-10s %s\n", "writes", "per frame", "lane", "routine");
		for (int i = 0; i < na && i < top / 2; i++) {
			char name[PROF_NAME_MAX + 16];
			if (a[i].row >= 0)
				snprintf(name, sizeof name, "%s", r->rows[a[i].row].name);
			else
				key_name(a[i].key, name, sizeof name);
			fprintf(f, "%10u %10.1f  %-10s %s\n", a[i].writes,
			        (double)a[i].writes / (double)frames, wf_lane_name(a[i].lane), name);
		}
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
	if (!strcmp(name, "waterfall")) return CAP_REC_WATERFALL;
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
		case CAP_REC_WATERFALL: return "waterfall";
	}
	return "?";
}
