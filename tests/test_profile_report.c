// Checks for the capture report (src/profile_report.c).
//
// The real perf_hist is driven with synthetic instructions, and the symbol
// table and coverage test are supplied here, so each banking rule can be
// pinned exactly: a banked address is named from its own bank's labels, never
// from another bank's, and every cycle lands in some row.

#include "perf_hist.h"
#include "profile_report.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

// Below $0F00 and the first two pages of the banked window are covered; the
// second banked page only with its bank unknown.
static int
covered(uint16_t pc, int ram_bank)
{
	(void)ram_bank;
	if (pc < 0x0F00)
		return 1;
	if (pc >= 0xA000 && pc < 0xA100)
		return 1;
	if (pc >= 0xA100 && pc < 0xA200)
		return 2;
	return 0;
}

static void
run(uint16_t pc, uint8_t ram, uint8_t rom, uint32_t clocks, int times, bool irq)
{
	for (int i = 0; i < times; i++)
		perf_hist_step_(clocks, pc, ram, rom, 0xEA, (uint16_t)(pc + 1), irq);
}

static const prof_row_t *
row_named(const prof_report_t *r, const char *name)
{
	for (int i = 0; i < r->count; i++)
		if (strcmp(r->rows[i].name, name) == 0)
			return &r->rows[i];
	return NULL;
}

int
main(void)
{
	check(perf_hist_arm(true), "histogram arms");
	perf_hist_reset();

	run(0x0810, 0, 0, 4, 10, false);  // init: 40
	run(0x0905, 0, 0, 3, 1, false);   // update: 3 main
	run(0x0905, 0, 0, 2, 1, true);    // update: 2 irq
	run(0xA010, 64, 0, 5, 1, false);  // draw64
	run(0xA020, 65, 0, 7, 1, false);  // draw65 -- same address range, other bank
	run(0xA010, 66, 0, 6, 1, false);  // bank 66 has no labels
	run(0xA150, 64, 0, 9, 1, false);  // covered, but its bank is unknown
	run(0x2000, 0, 0, 8, 1, false);   // not covered at all
	run(0xC123, 0, 0, 10, 1, false);  // ROM

	const prof_sym_t syms[] = {
		{"update", 0x0900, -1}, {"init", 0x0810, -1},          // unsorted on purpose
		{"draw64", 0xA000, 64}, {"draw65", 0xA000, 65},
		{"mystery", 0xA100, -1},
	};

	prof_report_t r;
	check(prof_report_build(&r, syms, 5, covered), "report builds");

	const prof_row_t *x;
	x = row_named(&r, "init");
	check(x && x->main_cycles == 40 && x->instructions == 10, "init: 40 cycles, 10 instructions");
	x = row_named(&r, "update");
	check(x && x->main_cycles == 3 && x->irq_cycles == 2, "update: main and IRQ kept apart");

	x = row_named(&r, "draw64");
	check(x && x->bank == 64, "bank 64's $A010 is draw64");
	check(x && x->main_cycles == 14, "and so is its $A150, from the same bank");
	check(x && x->ambiguous, "which is flagged: that page's bank was not established");
	x = row_named(&r, "draw65");
	check(x && x->main_cycles == 7 && !x->ambiguous, "bank 65 gets its own label, unflagged");

	x = row_named(&r, "RAM 66:$A0xx");
	check(x && x->main_cycles == 6 && !x->named,
	      "a bank with no labels is grouped by page, not given another bank's name");
	x = row_named(&r, "$20xx");
	check(x && x->main_cycles == 8, "uncovered low memory is grouped by page");
	x = row_named(&r, "ROM 0:$C1xx");
	check(x && x->main_cycles == 10, "ROM is grouped by bank and page");

	check(r.count > 0 && strcmp(r.rows[0].name, "init") == 0, "rows are worst first");

	uint64_t m = 0, q = 0, c = 0;
	for (int i = 0; i < r.count; i++) {
		m += r.rows[i].main_cycles;
		q += r.rows[i].irq_cycles;
		c += r.rows[i].instructions;
	}
	check_eq((uint32_t)m, (uint32_t)perf_hist_total_cycles() - (uint32_t)perf_hist_irq_cycles(),
	         "rows add up to the main-line total");
	check_eq((uint32_t)q, (uint32_t)perf_hist_irq_cycles(), "and to the IRQ total");
	check_eq((uint32_t)m, (uint32_t)r.main_cycles, "the report's own totals agree");
	check_eq((uint32_t)c, 18, "every instruction is counted once");

	prof_report_free(&r);
	check(r.rows == NULL && r.count == 0, "free empties it");
	return x16_test_summary("profile_report");
}
