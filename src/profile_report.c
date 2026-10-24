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

bool
prof_report_build(prof_report_t *out, const prof_sym_t *syms, int nsyms, prof_covered_fn covered)
{
	memset(out, 0, sizeof *out);
	if (!perf_hist_key_counts(0, NULL, NULL, NULL))
		return false;

	// Split and sort the symbol table.
	sym_t *low = calloc((size_t)(nsyms > 0 ? nsyms : 1), sizeof *low);
	sym_t *ram = calloc((size_t)(nsyms > 0 ? nsyms : 1), sizeof *ram);
	// One row per page, for time no label claims: page index = key >> 8.
	int   *page_row = malloc(sizeof(int) * ((PH_KEYS >> 8) + 1));
	int    nlow = 0, nram = 0, cap = 256;
	prof_row_t *rows = malloc(sizeof *rows * (size_t)cap);
	if (!low || !ram || !page_row || !rows) {
		free(low); free(ram); free(page_row); free(rows);
		return false;
	}
	for (int i = 0; i < nsyms; i++) {
		const prof_sym_t *s = &syms[i];
		if (!s->name)
			continue;
		if (s->addr < 0xA000)
			low[nlow++] = (sym_t){s->name, s->addr, -1, -1};
		else if (s->addr < 0xC000)
			ram[nram++] = (sym_t){s->name, s->addr, s->bank, -1};
	}
	qsort(low, (size_t)nlow, sizeof *low, cmp_low);
	qsort(ram, (size_t)nram, sizeof *ram, cmp_ram);
	for (uint32_t p = 0; p <= (PH_KEYS >> 8); p++)
		page_row[p] = -1;

	// Where the unknown-bank labels live, found once.
	int unk_lo, unk_hi;
	bank_run(ram, nram, -1, &unk_lo, &unk_hi);

	int n = 0;
	for (uint32_t k = 0; k < PH_KEYS; k++) {
		uint32_t m, q, c;
		perf_hist_key_counts(k, &m, &q, &c);
		if (!m && !q && !c)
			continue;
		out->main_cycles  += m;
		out->irq_cycles   += q;
		out->instructions += c;

		uint8_t  region, bank;
		uint16_t pc;
		perf_hist_key_decode(k, &region, &bank, &pc);

		// Which label, if any.
		sym_t *hit       = NULL;
		bool   ambiguous = false;
		if (region == PH_REGION_LOW) {
			const int cov = covered ? covered(pc, -1) : 0;
			const int i   = cov ? floor_in(low, 0, nlow, pc) : -1;
			if (i >= 0)
				hit = &low[i];
		} else if (region == PH_REGION_RAM) {
			const int cov = covered ? covered(pc, bank) : 0;
			if (cov) {
				int lo, hi;
				bank_run(ram, nram, (int16_t)bank, &lo, &hi);
				int i = floor_in(ram, lo, hi, pc);
				if (i < 0) {
					// No label known to be in this bank: one whose bank was
					// never established, and say so.
					i         = floor_in(ram, unk_lo, unk_hi, pc);
					ambiguous = i >= 0;
				}
				if (i >= 0) {
					hit       = &ram[i];
					ambiguous = ambiguous || cov == 2;
				}
			}
		}

		// Which row.
		int r = hit ? hit->row : page_row[k >> 8];
		if (r < 0) {
			if (n == cap) {
				cap *= 2;
				prof_row_t *grown = realloc(rows, sizeof *rows * (size_t)cap);
				if (!grown) {
					free(low); free(ram); free(page_row); free(rows);
					memset(out, 0, sizeof *out);
					return false;
				}
				rows = grown;
			}
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
				page_row[k >> 8] = r;
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

	free(low);
	free(ram);
	free(page_row);

	qsort(rows, (size_t)n, sizeof *rows, cmp_rows);
	for (int i = 0; i < n; i++) {
		out->named_rows     += rows[i].named;
		out->ambiguous_rows += rows[i].ambiguous;
	}
	out->rows  = rows;
	out->count = n;
	return true;
}

void
prof_report_free(prof_report_t *r)
{
	if (!r)
		return;
	free(r->rows);
	memset(r, 0, sizeof *r);
}
