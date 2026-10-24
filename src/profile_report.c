// Commander X16 Emulator — capture report. See profile_report.h.

#include "profile_report.h"
#include "perf_hist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Symbols split by where they can apply: below $A000 (any bank), the banked
// window (by bank, -1 last), and nothing above $C000 -- program debug info
// does not describe the ROM, so ROM time is grouped by page.
typedef struct {
	const char *name;
	uint16_t    addr;
	int16_t     bank;
	int         row;   // row index once charged, else -1
} sym_t;

// Kept with the report after it is built, so a key seen later (a call
// target from the tree) is charged by exactly the same rule.
typedef struct {
	sym_t          *low, *ram;
	int             nlow, nram;
	int             unk_lo, unk_hi;
	int            *page_row;  // key >> 8 -> row, for time no label claims
	prof_covered_fn covered;
} lookup_t;

static int
cmp_low(const void *a, const void *b)
{
	const sym_t *x = a, *y = b;
	return (x->addr > y->addr) - (x->addr < y->addr);
}

// Banked symbols by (bank, addr), so one bank's labels are a contiguous run.
static int
cmp_ram(const void *a, const void *b)
{
	const sym_t *x = a, *y = b;
	if (x->bank != y->bank)
		return (x->bank > y->bank) - (x->bank < y->bank);
	return (x->addr > y->addr) - (x->addr < y->addr);
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
		const int i   = cov ? floor_in(L->low, 0, L->nlow, pc) : -1;
		return i >= 0 ? &L->low[i] : NULL;
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
	free(L->page_row);
	free(L);
}

bool
prof_report_build(prof_report_t *out, const prof_sym_t *syms, int nsyms, prof_covered_fn covered)
{
	memset(out, 0, sizeof *out);
	if (!perf_hist_key_counts(0, NULL, NULL, NULL))
		return false;

	// Split and sort the symbol table.
	lookup_t *L = calloc(1, sizeof *L);
	int       cap = 256;
	prof_row_t *rows = malloc(sizeof *rows * (size_t)cap);
	if (L) {
		L->low      = calloc((size_t)(nsyms > 0 ? nsyms : 1), sizeof *L->low);
		L->ram      = calloc((size_t)(nsyms > 0 ? nsyms : 1), sizeof *L->ram);
		L->page_row = malloc(sizeof(int) * ((PH_KEYS >> 8) + 1));
		L->covered  = covered;
	}
	if (!L || !L->low || !L->ram || !L->page_row || !rows) {
		lookup_free(L);
		free(rows);
		return false;
	}
	for (int i = 0; i < nsyms; i++) {
		const prof_sym_t *s = &syms[i];
		if (!s->name)
			continue;
		if (s->addr < 0xA000)
			L->low[L->nlow++] = (sym_t){s->name, s->addr, -1, -1};
		else if (s->addr < 0xC000)
			L->ram[L->nram++] = (sym_t){s->name, s->addr, s->bank, -1};
	}
	qsort(L->low, (size_t)L->nlow, sizeof *L->low, cmp_low);
	qsort(L->ram, (size_t)L->nram, sizeof *L->ram, cmp_ram);
	for (uint32_t p = 0; p <= (PH_KEYS >> 8); p++)
		L->page_row[p] = -1;

	// Where the unknown-bank labels live, found once.
	bank_run(L->ram, L->nram, -1, &L->unk_lo, &L->unk_hi);

	int n = 0;
	for (uint32_t k = 0; k < PH_KEYS; k++) {
		uint32_t m, q, c;
		perf_hist_key_counts(k, &m, &q, &c);
		if (!m && !q && !c)
			continue;
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
	// Nearest ancestor (or self) charged to each row: -1 none. Used to skip a
	// node already counted inside an outer call of the same routine.
	int *outer = malloc(sizeof *outer * (size_t)count);
	if (!nodes || !outer) {
		free(nodes);
		free(outer);
		return false;
	}
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
		outer[i] = nested;
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
	free(outer);
	r->nodes      = nodes;
	r->node_count = count;
	return true;
}

void
prof_report_free(prof_report_t *r)
{
	if (!r)
		return;
	free(r->rows);
	free(r->nodes);
	lookup_free(r->lookup);
	memset(r, 0, sizeof *r);
}
