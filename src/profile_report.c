// Commander X16 Emulator — capture report. See profile_report.h.

#include "profile_report.h"
#include "perf_hist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Symbols split by where they can apply: below $A000 (any bank), the banked
// window (by bank, -1 last), the ROM (by ROM bank), and the KERNAL's low-RAM
// code (a fallback where the program's debug info is silent).
typedef struct {
	const char *name;
	uint16_t    addr;
	int16_t     bank;
	int         row;   // row index once charged, else -1
} sym_t;

// Kept with the report after it is built, so a key seen later (a call
// target from the tree) is charged by exactly the same rule.
typedef struct {
	sym_t          *low, *ram, *rom, *kram;
	int             nlow, nram, nrom, nkram;
	int             unk_lo, unk_hi;
	int            *page_row;  // key >> 8 -> row, for time no label claims
	prof_covered_fn covered;
} lookup_t;

// Ties on address break by name, so which of several labels at one address
// names it does not depend on the order they arrived in.
static int
cmp_low(const void *a, const void *b)
{
	const sym_t *x = a, *y = b;
	if (x->addr != y->addr)
		return (x->addr > y->addr) - (x->addr < y->addr);
	return strcmp(y->name, x->name); // floor takes the last: the first by name
}

// Banked symbols by (bank, addr), so one bank's labels are a contiguous run.
static int
cmp_ram(const void *a, const void *b)
{
	const sym_t *x = a, *y = b;
	if (x->bank != y->bank)
		return (x->bank > y->bank) - (x->bank < y->bank);
	return cmp_low(a, b);
}

// The last symbol in [lo, hi) whose addr <= pc, or -1. [lo, hi) is sorted by
// addr.
static int
floor_in(const sym_t *s, int lo, int hi, uint16_t pc)
{
	int best = -1;
	while (lo < hi) {
		const int mid = lo + (hi - lo) / 2;
		if (s[mid].addr <= pc) {
			best = mid;
			lo   = mid + 1;
		} else {
			hi = mid;
		}
	}
	return best;
}

// [lo, hi) of the run of banked symbols with this bank.
static void
bank_run(const sym_t *s, int n, int16_t bank, int *lo, int *hi)
{
	int a = 0, b = n;
	while (a < b) { // first with bank >= wanted
		const int m = a + (b - a) / 2;
		if (s[m].bank < bank) a = m + 1; else b = m;
	}
	*lo = a;
	b   = n;
	while (a < b) { // first with bank > wanted
		const int m = a + (b - a) / 2;
		if (s[m].bank <= bank) a = m + 1; else b = m;
	}
	*hi = a;
}

// The label a key is charged to, or NULL for page-grouped time.
static sym_t *
label_of(const lookup_t *L, uint32_t k, bool *ambiguous)
{
	uint8_t  region, bank;
	uint16_t pc;
	perf_hist_key_decode(k, &region, &bank, &pc);
	*ambiguous = false;
	if (region == PH_REGION_LOW) {
		const int cov = L->covered ? L->covered(pc, -1) : 0;
		if (cov) {
			const int i = floor_in(L->low, 0, L->nlow, pc);
			return i >= 0 ? &L->low[i] : NULL;
		}
		if (pc >= PROF_KRAM_START && pc < PROF_KRAM_END) {
			const int i = floor_in(L->kram, 0, L->nkram, pc);
			return i >= 0 ? &L->kram[i] : NULL;
		}
		return NULL;
	}
	if (region == PH_REGION_ROM) {
		int lo, hi;
		bank_run(L->rom, L->nrom, (int16_t)bank, &lo, &hi);
		const int i = floor_in(L->rom, lo, hi, pc);
		return i >= 0 ? &L->rom[i] : NULL;
	}
	if (region == PH_REGION_RAM) {
		const int cov = L->covered ? L->covered(pc, bank) : 0;
		if (!cov)
			return NULL;
		int lo, hi;
		bank_run(L->ram, L->nram, (int16_t)bank, &lo, &hi);
		int i = floor_in(L->ram, lo, hi, pc);
		if (i < 0) {
			// No label known to be in this bank: one whose bank was never
			// established, and say so.
			i          = floor_in(L->ram, L->unk_lo, L->unk_hi, pc);
			*ambiguous = i >= 0;
		}
		if (i < 0)
			return NULL;
		*ambiguous = *ambiguous || cov == 2;
		return &L->ram[i];
	}
	return NULL;
}

static int
cmp_rows(const void *a, const void *b)
{
	const prof_row_t *x = a, *y = b;
	const uint64_t    tx = x->main_cycles + x->irq_cycles, ty = y->main_cycles + y->irq_cycles;
	if (tx != ty)
		return tx < ty ? 1 : -1;
	return x->region != y->region ? x->region - y->region
	     : x->bank != y->bank     ? x->bank - y->bank
	                              : x->addr - y->addr;
}

static const char *
region_name(uint8_t region)
{
	return region == PH_REGION_RAM ? "RAM" : region == PH_REGION_ROM ? "ROM" : "";
}

static void
lookup_free(lookup_t *L)
{
	if (!L)
		return;
	free(L->low);
	free(L->ram);
	free(L->rom);
	free(L->kram);
	free(L->page_row);
	free(L);
}

// The rows, from the histogram's non-zero keys in key order.
static bool
build_from(prof_report_t *out, const prof_key_count_t *hist, int nhist, const prof_sym_t *syms, int nsyms,
           prof_covered_fn covered)
{
	memset(out, 0, sizeof *out);

	// Split and sort the symbol table.
	lookup_t *L = calloc(1, sizeof *L);
	int       cap = 256;
	prof_row_t *rows = malloc(sizeof *rows * (size_t)cap);
	if (L) {
		const size_t ns = (size_t)(nsyms > 0 ? nsyms : 1);
		L->low      = calloc(ns, sizeof *L->low);
		L->ram      = calloc(ns, sizeof *L->ram);
		L->rom      = calloc(ns, sizeof *L->rom);
		L->kram     = calloc(ns, sizeof *L->kram);
		L->page_row = malloc(sizeof(int) * ((PH_KEYS >> 8) + 1));
		L->covered  = covered;
	}
	if (!L || !L->low || !L->ram || !L->rom || !L->kram || !L->page_row || !rows) {
		lookup_free(L);
		free(rows);
		return false;
	}
	for (int i = 0; i < nsyms; i++) {
		const prof_sym_t *s = &syms[i];
		if (!s->name)
			continue;
		if (s->addr >= 0xC000) {
			if (s->bank >= 0)
				L->rom[L->nrom++] = (sym_t){s->name, s->addr, s->bank, -1};
		} else if (s->rom) {
			if (s->addr >= PROF_KRAM_START && s->addr < PROF_KRAM_END)
				L->kram[L->nkram++] = (sym_t){s->name, s->addr, -1, -1};
		} else if (s->addr < 0xA000) {
			L->low[L->nlow++] = (sym_t){s->name, s->addr, -1, -1};
		} else {
			L->ram[L->nram++] = (sym_t){s->name, s->addr, s->bank, -1};
		}
	}
	qsort(L->low, (size_t)L->nlow, sizeof *L->low, cmp_low);
	qsort(L->ram, (size_t)L->nram, sizeof *L->ram, cmp_ram);
	qsort(L->rom, (size_t)L->nrom, sizeof *L->rom, cmp_ram);
	qsort(L->kram, (size_t)L->nkram, sizeof *L->kram, cmp_low);
	for (uint32_t p = 0; p <= (PH_KEYS >> 8); p++)
		L->page_row[p] = -1;

	// Where the unknown-bank labels live, found once.
	bank_run(L->ram, L->nram, -1, &L->unk_lo, &L->unk_hi);

	int n = 0;
	for (int h = 0; h < nhist; h++) {
		const uint32_t k = hist[h].key, m = hist[h].main, q = hist[h].irq, c = hist[h].count;
		out->main_cycles  += m;
		out->irq_cycles   += q;
		out->instructions += c;

		bool   ambiguous;
		sym_t *hit = label_of(L, k, &ambiguous);

		// Which row.
		int r = hit ? hit->row : L->page_row[k >> 8];
		if (r < 0) {
			if (n == cap) {
				cap *= 2;
				prof_row_t *grown = realloc(rows, sizeof *rows * (size_t)cap);
				if (!grown) {
					lookup_free(L);
					free(rows);
					memset(out, 0, sizeof *out);
					return false;
				}
				rows = grown;
			}
			uint8_t  region, bank;
			uint16_t pc;
			perf_hist_key_decode(k, &region, &bank, &pc);
			r = n++;
			prof_row_t *row = &rows[r];
			memset(row, 0, sizeof *row);
			row->region = region;
			row->bank   = bank;
			if (hit) {
				row->named     = true;
				row->ambiguous = ambiguous;
				row->addr      = hit->addr;
				snprintf(row->name, sizeof row->name, "%s", hit->name);
				hit->row = r;
			} else {
				row->addr = (uint16_t)(pc & 0xFF00u);
				if (region == PH_REGION_LOW)
					snprintf(row->name, sizeof row->name, "$%02Xxx", row->addr >> 8);
				else
					snprintf(row->name, sizeof row->name, "%s %u:$%02Xxx", region_name(region),
					         (unsigned)bank, row->addr >> 8);
				L->page_row[k >> 8] = r;
			}
		}
		rows[r].main_cycles  += m;
		rows[r].irq_cycles   += q;
		rows[r].instructions += c;
		// A label first reached through the unknown-bank fallback and later
		// matched cleanly stays flagged: some of its time is uncertain.
		if (hit && ambiguous)
			rows[r].ambiguous = true;
	}

	// Sort, then point the lookup at the sorted positions. Each row carries
	// its old index through the sort in `calls`, which stays zero until
	// prof_report_add_tree() fills it.
	int *where = malloc(sizeof *where * (size_t)(n > 0 ? n : 1));
	if (!where) {
		lookup_free(L);
		free(rows);
		memset(out, 0, sizeof *out);
		return false;
	}
	for (int i = 0; i < n; i++)
		rows[i].calls = (uint32_t)i;
	qsort(rows, (size_t)n, sizeof *rows, cmp_rows);
	for (int i = 0; i < n; i++) {
		where[rows[i].calls] = i;
		rows[i].calls        = 0;
	}
	for (int i = 0; i < L->nlow; i++)
		if (L->low[i].row >= 0)
			L->low[i].row = where[L->low[i].row];
	for (int i = 0; i < L->nram; i++)
		if (L->ram[i].row >= 0)
			L->ram[i].row = where[L->ram[i].row];
	for (int i = 0; i < L->nrom; i++)
		if (L->rom[i].row >= 0)
			L->rom[i].row = where[L->rom[i].row];
	for (int i = 0; i < L->nkram; i++)
		if (L->kram[i].row >= 0)
			L->kram[i].row = where[L->kram[i].row];
	for (uint32_t p = 0; p <= (PH_KEYS >> 8); p++)
		if (L->page_row[p] >= 0)
			L->page_row[p] = where[L->page_row[p]];
	free(where);

	for (int i = 0; i < n; i++) {
		out->named_rows     += rows[i].named;
		out->ambiguous_rows += rows[i].ambiguous;
	}
	out->rows   = rows;
	out->count  = n;
	out->lookup = L;
	return true;
}

bool
prof_report_build(prof_report_t *out, const prof_sym_t *syms, int nsyms, prof_covered_fn covered)
{
	memset(out, 0, sizeof *out);
	if (!perf_hist_key_counts(0, NULL, NULL, NULL))
		return false;
	// The keys that ran, copied out: the report can then be named again
	// (prof_report_rename) from what was recorded, whatever the histogram
	// goes on to count.
	int nhist = 0;
	for (uint32_t k = 0; k < PH_KEYS; k++) {
		uint32_t m, q, c;
		perf_hist_key_counts(k, &m, &q, &c);
		nhist += (m || q || c);
	}
	prof_key_count_t *hist = malloc(sizeof *hist * (size_t)(nhist > 0 ? nhist : 1));
	if (!hist)
		return false;
	int h = 0;
	for (uint32_t k = 0; k < PH_KEYS && h < nhist; k++) {
		uint32_t m, q, c;
		perf_hist_key_counts(k, &m, &q, &c);
		if (m || q || c)
			hist[h++] = (prof_key_count_t){k, m, q, c};
	}
	if (!build_from(out, hist, h, syms, nsyms, covered)) {
		free(hist);
		return false;
	}
	out->hist       = hist;
	out->hist_count = h;
	return true;
}

bool
prof_report_rename(prof_report_t *r, const prof_sym_t *syms, int nsyms, prof_covered_fn covered)
{
	if (!r || !r->hist)
		return false;
	prof_report_t t;
	if (!build_from(&t, r->hist, r->hist_count, syms, nsyms, covered))
		return false;
	if (r->nodes && !prof_report_add_tree(&t, r->nodes, r->node_count)) {
		prof_report_free(&t);
		return false;
	}
	// Built: only now does it replace the old one.
	t.hist       = r->hist;
	t.hist_count = r->hist_count;
	r->hist      = NULL;
	prof_report_free(r);
	*r = t;
	return true;
}

int
prof_report_row_of_key(const prof_report_t *r, uint32_t key)
{
	if (!r || !r->lookup || key >= PH_KEYS)
		return -1;
	const lookup_t *L = r->lookup;
	bool            ambiguous;
	const sym_t    *hit = label_of(L, key, &ambiguous);
	return hit ? hit->row : L->page_row[key >> 8];
}

bool
prof_report_add_tree(prof_report_t *r, const prof_node_t *in, int count)
{
	if (!r)
		return false;
	free(r->nodes);
	r->nodes      = NULL;
	r->node_count = 0;
	for (int i = 0; i < r->count; i++) {
		r->rows[i].incl_cycles = 0;
		r->rows[i].calls = r->rows[i].call_max = r->rows[i].frame_max = 0;
	}
	if (count <= 0)
		return true;
	prof_node_t *nodes = malloc(sizeof *nodes * (size_t)count);
	if (!nodes)
		return false;
	memcpy(nodes, in, sizeof *nodes * (size_t)count);
	for (int i = 0; i < count; i++) {
		prof_node_t *nd = &nodes[i];
		nd->row = prof_report_row_of_key(r, nd->key);
		// Does any ancestor already charge this row? Walk up; trees are
		// shallow (bounded by the 6502 stack), so this stays cheap.
		bool nested = false;
		for (int p = nd->parent; p >= 0 && nd->row >= 0; p = nodes[p].parent)
			if (nodes[p].row == nd->row) {
				nested = true;
				break;
			}
		if (nd->row < 0)
			continue;
		prof_row_t *row = &r->rows[nd->row];
		row->calls += nd->calls;
		if (nd->call_max > row->call_max)
			row->call_max = nd->call_max;
		if (nd->frame_max > row->frame_max)
			row->frame_max = nd->frame_max;
		if (!nested)
			row->incl_cycles += nd->incl;
	}
	r->nodes      = nodes;
	r->node_count = count;
	return true;
}

// ─── One routine's callers and callees ──────────────────────────────────────

// Is `node` under another node of the same row? Then its time is already
// inside that outer call.
static bool
nested_in_row(const prof_report_t *r, int node, int row)
{
	for (int p = r->nodes[node].parent; p >= 0; p = r->nodes[p].parent)
		if (r->nodes[p].row == row)
			return true;
	return false;
}

// Add to the edge list entry for neighbour `node` (merged by row, or by node
// when it has none).
static bool
edge_add(prof_edge_t **list, int *n, int *cap, const prof_report_t *r, int node, uint64_t cycles,
         uint32_t calls)
{
	const int row = r->nodes[node].row;
	for (int i = 0; i < *n; i++) {
		prof_edge_t *e = &(*list)[i];
		if (row >= 0 ? e->row == row : (e->row < 0 && e->node == node)) {
			e->cycles += cycles;
			e->calls += calls;
			return true;
		}
	}
	if (*n == *cap) {
		const int    ncap  = *cap ? *cap * 2 : 16;
		prof_edge_t *grown = realloc(*list, sizeof **list * (size_t)ncap);
		if (!grown)
			return false;
		*list = grown;
		*cap  = ncap;
	}
	(*list)[(*n)++] = (prof_edge_t){row, node, cycles, calls};
	return true;
}

// Fold one call of the routine: its own time, and its callees. A child that is
// the routine again (recursion) is folded in rather than listed.
static bool
fold_call(const prof_report_t *r, int node, int row, prof_routine_view_t *v, int *cap)
{
	v->own += r->nodes[node].self;
	for (int c = 0; c < r->node_count; c++) {
		if (r->nodes[c].parent != node)
			continue;
		if (r->nodes[c].row == row) {
			if (!fold_call(r, c, row, v, cap))
				return false;
		} else if (!edge_add(&v->callees, &v->ncallees, cap, r, c, r->nodes[c].incl, r->nodes[c].calls)) {
			return false;
		}
	}
	return true;
}

static int
cmp_edges(const void *a, const void *b)
{
	const prof_edge_t *x = a, *y = b;
	if (x->cycles != y->cycles)
		return x->cycles < y->cycles ? 1 : -1;
	return x->node - y->node;
}

bool
prof_report_routine_view(const prof_report_t *r, int row, prof_routine_view_t *out)
{
	memset(out, 0, sizeof *out);
	if (!r || !r->nodes || r->node_count <= 2 || row < 0 || row >= r->count)
		return false;
	int ccap = 0, kcap = 0;
	for (int i = 0; i < r->node_count; i++) {
		const prof_node_t *n = &r->nodes[i];
		if (n->row != row || nested_in_row(r, i, row))
			continue;
		// An outermost call of the routine: its caller, its time, its callees.
		out->total += n->incl;
		out->calls += n->calls;
		if (n->parent >= 0 &&
		    !edge_add(&out->callers, &out->ncallers, &ccap, r, n->parent, n->incl, n->calls))
			goto oom;
		if (!fold_call(r, i, row, out, &kcap))
			goto oom;
	}
	qsort(out->callers, (size_t)out->ncallers, sizeof *out->callers, cmp_edges);
	qsort(out->callees, (size_t)out->ncallees, sizeof *out->callees, cmp_edges);
	return true;
oom:
	prof_routine_view_free(out);
	return false;
}

// ─── A routine's neighbourhood ──────────────────────────────────────────────

typedef struct {
	prof_graph_t g;
	int          bcap, ecap;
	bool         oom;
} graph_build_t;

// Which box is this neighbour, if it has one: by row, or for the top of the
// tree by its node.
static int
graph_find(const graph_build_t *b, int row, int node)
{
	for (int i = 0; i < b->g.nboxes; i++) {
		const prof_gbox_t *x = &b->g.boxes[i];
		if (row >= 0 ? x->row == row : (x->row < 0 && x->node == node))
			return i;
	}
	return -1;
}

static int
graph_add_box(graph_build_t *b, int row, int node, int col, uint64_t weight)
{
	if (b->g.nboxes == b->bcap) {
		const int    want = b->bcap ? b->bcap * 2 : 16;
		prof_gbox_t *g    = realloc(b->g.boxes, sizeof *g * (size_t)want);
		if (!g) {
			b->oom = true;
			return -1;
		}
		b->g.boxes = g;
		b->bcap    = want;
	}
	b->g.boxes[b->g.nboxes] = (prof_gbox_t){row, node, col, weight};
	return b->g.nboxes++;
}

static void
graph_add_edge(graph_build_t *b, int from, int to, uint64_t cycles, uint32_t calls)
{
	if (from < 0 || to < 0 || from == to)
		return;
	for (int i = 0; i < b->g.nedges; i++)
		if (b->g.edges[i].from == from && b->g.edges[i].to == to)
			return; // already drawn, from the other side
	if (b->g.nedges == b->ecap) {
		const int     want = b->ecap ? b->ecap * 2 : 32;
		prof_gedge_t *g    = realloc(b->g.edges, sizeof *g * (size_t)want);
		if (!g) {
			b->oom = true;
			return;
		}
		b->g.edges = g;
		b->ecap    = want;
	}
	b->g.edges[b->g.nedges++] = (prof_gedge_t){from, to, cycles, calls};
}

// One step out from the boxes in column `from`: their callers (`callers`) or
// callees, into column `to`. Candidates from every box are ranked together, so
// a column keeps the heaviest wherever they came from.
typedef struct {
	int      box;  // the box they connect to
	int      row, node;
	uint64_t cycles;
	uint32_t calls;
} graph_cand_t;

static int
cmp_cand(const void *a, const void *b)
{
	const graph_cand_t *x = a, *y = b;
	if (x->cycles != y->cycles)
		return x->cycles < y->cycles ? 1 : -1;
	return x->row - y->row;
}

static void
graph_step(graph_build_t *b, const prof_report_t *r, int from, int to, bool callers, int per_col)
{
	graph_cand_t *c  = NULL;
	int           nc = 0, cc = 0;
	const int     nb = b->g.nboxes; // only the boxes there before this step
	for (int i = 0; i < nb && !b->oom; i++) {
		const prof_gbox_t *x = &b->g.boxes[i];
		if (x->col != from || x->row < 0)
			continue; // the top of the tree has no callers
		prof_routine_view_t v;
		if (!prof_report_routine_view(r, x->row, &v))
			continue;
		const prof_edge_t *e = callers ? v.callers : v.callees;
		const int          n = callers ? v.ncallers : v.ncallees;
		for (int k = 0; k < n; k++) {
			if (nc == cc) {
				cc               = cc ? cc * 2 : 32;
				graph_cand_t *gr = realloc(c, sizeof *c * (size_t)cc);
				if (!gr) {
					b->oom = true;
					break;
				}
				c = gr;
			}
			c[nc++] = (graph_cand_t){i, e[k].row, e[k].node, e[k].cycles, e[k].calls};
		}
		prof_routine_view_free(&v);
	}
	qsort(c, (size_t)nc, sizeof *c, cmp_cand);
	int placed = 0;
	// Distinct neighbours that did not fit, counted once each.
	int *left = calloc((size_t)(nc ? nc : 1), sizeof *left), nleft = 0;
	for (int k = 0; k < nc && !b->oom; k++) {
		int at = graph_find(b, c[k].row, c[k].node);
		if (at < 0 && placed < per_col) {
			at = graph_add_box(b, c[k].row, c[k].node, to, c[k].cycles);
			placed++;
		}
		if (at < 0) {
			const int id = c[k].row >= 0 ? c[k].row : -2 - c[k].node;
			bool      seen = false;
			for (int j = 0; j < nleft && !seen; j++)
				seen = left[j] == id;
			if (!seen && left)
				left[nleft++] = id;
			continue;
		}
		if (callers)
			graph_add_edge(b, at, c[k].box, c[k].cycles, c[k].calls);
		else
			graph_add_edge(b, c[k].box, at, c[k].cycles, c[k].calls);
	}
	b->g.hidden[to] += nleft;
	free(left);
	free(c);
}

bool
prof_report_graph(const prof_report_t *r, int row, int per_col, prof_graph_t *out)
{
	memset(out, 0, sizeof *out);
	if (!r || !r->nodes || r->node_count <= 2 || row < 0 || row >= r->count)
		return false;
	if (per_col < 1)
		per_col = 1;
	graph_build_t b;
	memset(&b, 0, sizeof b);
	graph_add_box(&b, row, -1, PROF_GRAPH_CENTRE, r->rows[row].incl_cycles);
	// Out one step, then two, on each side.
	graph_step(&b, r, PROF_GRAPH_CENTRE, PROF_GRAPH_CENTRE - 1, true, per_col);
	graph_step(&b, r, PROF_GRAPH_CENTRE, PROF_GRAPH_CENTRE + 1, false, per_col);
	graph_step(&b, r, PROF_GRAPH_CENTRE - 1, PROF_GRAPH_CENTRE - 2, true, per_col);
	graph_step(&b, r, PROF_GRAPH_CENTRE + 1, PROF_GRAPH_CENTRE + 2, false, per_col);
	if (b.oom) {
		prof_graph_free(&b.g);
		return false;
	}
	*out = b.g;
	return true;
}

void
prof_graph_free(prof_graph_t *g)
{
	if (!g)
		return;
	free(g->boxes);
	free(g->edges);
	memset(g, 0, sizeof *g);
}

void
prof_routine_view_free(prof_routine_view_t *v)
{
	if (!v)
		return;
	free(v->callers);
	free(v->callees);
	memset(v, 0, sizeof *v);
}

void
prof_report_free(prof_report_t *r)
{
	if (!r)
		return;
	free(r->rows);
	free(r->nodes);
	free(r->hist);
	lookup_free(r->lookup);
	memset(r, 0, sizeof *r);
}
