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

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

static prof_report_t report;
static bool          have_report;
static uint32_t      report_frames;
static uint32_t      report_generation;

// Whether perf_hist was armed by someone else (a DAP client) before the
// capture, so ending the capture leaves it as it was found.
static bool hist_was_enabled;

static prof_budget_summary_t budget_summary;

static bool cct_ready;      // pool allocated (once; reused by every capture)
static bool wf_ready;       // likewise, the waterfall's frame buffers
static bool wf_recorded;    // the last capture ran the waterfall

// Detail kept: the last WF_RING frames and the WF_WORST worst, each up to
// WF_FRAME_CAP events. About 20 MB at most, allocated on first use.
#define WF_RING      30
#define WF_WORST     8
#define WF_FRAME_CAP 32768

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
	if (mask & CAP_REC_WATERFALL) {
		// Writes are stamped from the histogram's step.
		if (!(mask & CAP_REC_HIST))
			ok = false;
		if (!wf_ready)
			wf_ready = wf_init(WF_RING, WF_WORST, WF_FRAME_CAP);
		ok = wf_ready && ok;
		if (ok) {
			wf_reset();
			ym_selected          = 0;
			io_write_tap         = io_tap;
			video_vram_write_tap = vram_tap;
			if (mask & CAP_REC_TREE)
				cct_switch_hook = cct_switch;
		}
	}
	wf_recorded = (mask & CAP_REC_WATERFALL) && ok;
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
	if (mask & CAP_REC_WATERFALL) {
		io_write_tap         = NULL;
		video_vram_write_tap = NULL;
		cct_switch_hook      = NULL;
	}
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
	report_generation++;

	// Work against waiting, from the budget recorder's ring, newest first:
	// the capture's frames are the most recent ones.
	memset(&budget_summary, 0, sizeof budget_summary);
	if (cap_recorders() & CAP_REC_BUDGET) {
		const int want = (int)cap_frames();
		budget_summary.budget = perf_budget_budget_cycles();
		for (int i = 0; i < want && i < perf_budget_frame_count(); i++) {
			perf_frame_t fr;
			if (!perf_budget_frame_at(i, &fr))
				break;
			const uint32_t work = perf_frame_work(&fr);
			budget_summary.frames++;
			budget_summary.total += fr.total;
			budget_summary.idle += fr.idle;
			if (work > budget_summary.work_max)
				budget_summary.work_max = work;
			if (budget_summary.budget && fr.period_work > budget_summary.budget)
				budget_summary.overruns++;
		}
		budget_summary.have = budget_summary.frames > 0;
	}
	if (!(cap_recorders() & CAP_REC_HIST))
		return;

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
	return &budget_summary;
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
	const int r = have_report ? prof_report_row_of_key(&report, key) : -1;
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
		out[i] = (prof_writes_t){have_report ? prof_report_row_of_key(&report, a[i].key) : -1,
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

static int
cmp_frame_ptr(const void *a, const void *b)
{
	const wf_frame_t *x = *(const wf_frame_t *const *)a, *y = *(const wf_frame_t *const *)b;
	return (x->number > y->number) - (x->number < y->number);
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

	// The frames: the ring and the worst list, each frame once, in order.
	const wf_frame_t *frames[WF_RING + WF_WORST];
	int               n = 0;
	for (int i = 0; i < wf_ring_count(); i++)
		frames[n++] = wf_ring_at(i);
	for (int i = 0; i < wf_worst_count(); i++) {
		const wf_frame_t *w   = wf_worst_at(i);
		bool              dup = false;
		for (int k = 0; k < n && !dup; k++)
			dup = frames[k]->number == w->number;
		if (!dup)
			frames[n++] = w;
	}
	qsort((void *)frames, (size_t)n, sizeof frames[0], cmp_frame_ptr);

	fprintf(f, "{\"displayTimeUnit\":\"ns\",\"traceEvents\":[\n");
	fprintf(f, "{\"ph\":\"M\",\"pid\":1,\"name\":\"process_name\",\"args\":{\"name\":\"X16\"}}");
	static const char *const fixed[] = {"CPU", "IRQ", "Frames"};
	for (int t = 0; t < 3; t++)
		fprintf(f, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_name\",\"args\":{\"name\":\"%s\"}}"
		           ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_sort_index\",\"args\":{\"sort_index\":%d}}",
		        t, fixed[t], t, t);
	for (int l = 1; l < WF_LANES; l++)
		fprintf(f, ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_name\",\"args\":{\"name\":\"%s\"}}"
		           ",\n{\"ph\":\"M\",\"pid\":1,\"tid\":%d,\"name\":\"thread_sort_index\",\"args\":{\"sort_index\":%d}}",
		        10 + l, wf_lane_name(l), 10 + l, 10 + l);

	trace_ctx_t c = {f, NULL, 1.0 / (double)(MHZ ? MHZ : 8), cache, 0};
	for (int i = 0; i < n; i++) {
		c.frame = frames[i];
		bool worst = false;
		for (int k = 0; k < wf_worst_count() && !worst; k++)
			worst = wf_worst_at(k)->number == frames[i]->number;
		fprintf(f, ",\n{\"ph\":\"X\",\"pid\":1,\"tid\":2,\"ts\":%.3f,\"dur\":%.3f,\"name\":\"frame %u%s\","
		           "\"args\":{\"cycles\":%u,\"events\":%d,\"truncated\":%s}}",
		        (double)frames[i]->start * c.us_per_cycle, (double)frames[i]->length * c.us_per_cycle,
		        frames[i]->number, worst ? " (worst)" : "", frames[i]->length, frames[i]->count,
		        frames[i]->truncated ? "true" : "false");
		if (have_report && report.node_count > 2)
			wf_walk_frame(frames[i], trace_parent, CCT_NODE_IRQ, trace_emit, &c);
		else
			for (int e = 0; e < frames[i]->count; e++)
				if (wf_event_lane(&frames[i]->events[e]) != WF_LANE_CPU)
					trace_emit(&c, WF_OUT_EVENT, 0, frames[i]->events[e].cycle, -1,
					           &frames[i]->events[e]);
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
	if (budget_summary.have)
		fprintf(f, "  \"budget\": {\"frames\": %u, \"totalCycles\": %llu, \"idleCycles\": %llu, "
		           "\"workMax\": %u, \"overruns\": %u, \"budgetCycles\": %u},\n",
		        budget_summary.frames, (unsigned long long)budget_summary.total,
		        (unsigned long long)budget_summary.idle, budget_summary.work_max,
		        budget_summary.overruns, budget_summary.budget);
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
		uint64_t     score = 0; // 0: rank by event count
		perf_frame_t fr;
		if ((cap_recorders() & CAP_REC_BUDGET) && perf_budget_last_frame(&fr))
			score = perf_frame_work(&fr);
		wf_frame_end(perf_hist_total_cycles(), score,
		             (uint16_t)((cap_recorders() & CAP_REC_TREE) ? cct_current() : 0));
	}
	cap_frame_end();
}

bool
profile_session_has_waterfall(void)
{
	return wf_recorded && wf_ready && wf_frames_seen() > 0;
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
	if (budget_summary.have)
		fprintf(f, "work %.1f%%, waiting %.1f%% (cycle budget); worst frame %u of %u cycles, "
		           "%u over budget\n",
		        budget_summary.total
		            ? 100.0 * (double)(budget_summary.total - budget_summary.idle) /
		                  (double)budget_summary.total
		            : 0.0,
		        budget_summary.total
		            ? 100.0 * (double)budget_summary.idle / (double)budget_summary.total
		            : 0.0,
		        budget_summary.work_max, budget_summary.budget, budget_summary.overruns);
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
		fprintf(f, "\ndevice writes by routine (%u frames; %d frames of detail kept%s)\n",
		        wf_frames_seen(), wf_ring_count() + wf_worst_count(),
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
