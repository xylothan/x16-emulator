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

// For the ROM block: the program covers only its own $0800-$0EFF.
static int
covered_prog(uint16_t pc, int ram_bank)
{
	(void)ram_bank;
	return pc >= 0x0800 && pc < 0x0F00;
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

	// ── The call tree: rows by call target; recursion counted once ──────────
	{
		const int init_row = (int)(row_named(&r, "init") - r.rows);
		check_eq((uint32_t)prof_report_row_of_key(&r, 0x0810), (uint32_t)init_row,
		         "a key maps to the row its cycles went to");
		check_eq((uint32_t)prof_report_row_of_key(&r, perf_hist_key(0xA000, 65, 0)),
		         (uint32_t)(row_named(&r, "draw65") - r.rows), "banked keys use their own bank");
		check(prof_report_row_of_key(&r, 0x3000) < 0, "a key that never ran has no row");

		const prof_node_t tree[] = {
			// parent row  key                          self incl calls max fmax
			{-1, 0, 0xFFFFFFF0u,                      0, 160, 0, 0, 0},   // [main]
			{-1, 0, 0xFFFFFFF1u,                      0, 0,   0, 0, 0},   // [IRQ]
			{0,  0, 0x0810,                          60, 100, 2, 70, 90}, // init
			{2,  0, 0x0900,                          20, 30,  4, 12, 25}, // init/update
			{3,  0, 0x0810,                          10, 10,  1, 10, 10}, // .../init again
			{0,  0, perf_hist_key(0xA000, 64, 0),    14, 14,  1, 14, 14}, // draw64
			{0,  0, 0x3000,                          16, 16,  1, 16, 16}, // never ran
		};
		check(prof_report_add_tree(&r, tree, 7), "tree attaches");
		check_eq((uint32_t)r.node_count, 7, "all nodes kept");
		const prof_row_t *init = row_named(&r, "init");
		check_eq((uint32_t)init->incl_cycles, 100, "init incl: the inner recursive call is inside");
		check_eq(init->calls, 3, "but its calls all count");
		check_eq(init->call_max, 70, "worst call over its nodes");
		check_eq(init->frame_max, 90, "worst frame over its nodes");
		check_eq((uint32_t)row_named(&r, "update")->incl_cycles, 30, "update incl");
		check_eq((uint32_t)row_named(&r, "draw64")->incl_cycles, 14, "banked target");
		check_eq((uint32_t)row_named(&r, "draw65")->incl_cycles, 0, "never called: no incl");
		check(r.nodes[6].row < 0 && r.nodes[0].row < 0, "roots and unknown targets have no row");
		check_eq((uint32_t)r.nodes[3].row, (uint32_t)(row_named(&r, "update") - r.rows),
		         "nodes carry their row");
	}

	// ── One routine's callers and callees, merged over every path ───────────
	{
		const uint32_t K_INIT = 0x0810, K_UPD = 0x0900, K_DRAW = perf_hist_key(0xA000, 64, 0);
		// [main] -> init (self 40)
		//             -> update (self 20) -> init (self 4) -> draw64 (6)   mutual recursion
		//             -> init (self 10) -> draw64 (5)                       direct recursion
		//             -> draw64 (15)
		//        -> update (self 50)
		// [IRQ]  -> update (self 7)
		const prof_node_t t[] = {
			// parent row key            self incl calls max fmax
			{-1, 0, 0xFFFFFFF0u,           0, 187, 0, 0, 0},  // 0 [main]
			{-1, 0, 0xFFFFFFF1u,           0,   7, 0, 0, 0},  // 1 [IRQ]
			{0,  0, K_INIT,               40, 100, 2, 0, 0},  // 2 init
			{2,  0, K_UPD,                20,  30, 3, 0, 0},  // 3 init/update
			{3,  0, K_INIT,                4,  10, 1, 0, 0},  // 4 init/update/init
			{4,  0, K_DRAW,                6,   6, 1, 0, 0},  // 5 .../init/draw64
			{2,  0, K_INIT,               10,  15, 1, 0, 0},  // 6 init/init
			{6,  0, K_DRAW,                5,   5, 1, 0, 0},  // 7 init/init/draw64
			{2,  0, K_DRAW,               15,  15, 4, 0, 0},  // 8 init/draw64
			{0,  0, K_UPD,                50,  50, 5, 0, 0},  // 9 update
			{1,  0, K_UPD,                 7,   7, 6, 0, 0},  // 10 [IRQ]/update
		};
		check(prof_report_add_tree(&r, t, 11), "tree for the routine view");
		const int init_row = (int)(row_named(&r, "init") - r.rows);
		const int upd_row  = (int)(row_named(&r, "update") - r.rows);
		const int draw_row = (int)(row_named(&r, "draw64") - r.rows);

		prof_routine_view_t v;
		check(prof_report_routine_view(&r, init_row, &v), "init's view builds");
		check_eq((uint32_t)v.total, 100, "init total: the one outermost call (the inner ones are inside it)");
		check_eq((uint32_t)v.own, 50, "own: 40 + 10 from the direct recursion, folded in");
		check(v.ncallers == 1 && v.callers[0].node == 0 && v.callers[0].cycles == 100,
		      "called only from [main], for all 100");
		check_eq((uint32_t)v.ncallees, 2, "two callees: update and draw64 (not init itself)");
		check(v.callees[0].row == upd_row && v.callees[0].cycles == 30, "update costs it 30 (with the init inside)");
		check(v.callees[1].row == draw_row && v.callees[1].cycles == 20 && v.callees[1].calls == 5,
		      "draw64 costs it 20 over 5 calls, merged across both paths");
		uint64_t sum = v.own;
		for (int i = 0; i < v.ncallees; i++)
			sum += v.callees[i].cycles;
		check_eq((uint32_t)sum, (uint32_t)v.total, "own + callees = total");
		check_eq((uint32_t)v.total, (uint32_t)r.rows[init_row].incl_cycles, "and total is the row's total");
		prof_routine_view_free(&v);

		check(prof_report_routine_view(&r, upd_row, &v), "update's view builds");
		check_eq((uint32_t)v.total, 87, "update total: 30 + 50 + 7 over its three outermost calls");
		check_eq((uint32_t)v.ncallers, 3, "three callers");
		check(v.callers[0].node == 0 && v.callers[0].cycles == 50, "[main] caused 50");
		check(v.callers[1].row == init_row && v.callers[1].cycles == 30, "init caused 30");
		check(v.callers[2].node == 1 && v.callers[2].cycles == 7, "[IRQ] caused 7");
		uint64_t csum = 0;
		for (int i = 0; i < v.ncallers; i++)
			csum += v.callers[i].cycles;
		check_eq((uint32_t)csum, (uint32_t)v.total, "callers add up to the total");
		check(v.ncallees == 1 && v.callees[0].row == init_row && v.callees[0].cycles == 10,
		      "update calls init back, for 10");
		check_eq((uint32_t)(v.own + v.callees[0].cycles), 87, "own 77 + callee 10");
		prof_routine_view_free(&v);

		check(!prof_report_routine_view(&r, -1, &v), "no view for a row that is not there");
	}

	prof_report_free(&r);
	check(r.rows == NULL && r.count == 0, "free empties it");

	// ── ROM labels: by ROM bank; KERNAL low-RAM code as a fallback ──────────
	{
		perf_hist_reset();
		run(0xC123, 0, 0, 10, 1, false);  // ROM 0
		run(0xC123, 0, 4, 20, 1, false);  // ROM 4: same address, other bank
		run(0xC800, 0, 7, 5, 1, false);   // ROM 7: no labels there
		run(0x0390, 0, 0, 30, 1, true);   // KERNAL's RAM IRQ code
		run(0x0810, 0, 0, 4, 1, false);   // the program's own init
		const prof_sym_t rsyms[] = {
			{"init", 0x0810, -1, false},
			{"k_thing", 0xC100, 0, true},  {"b_thing", 0xC100, 4, true},
			{"zzz", 0xC100, 4, true},          // second name, same place: by name
			{"__irq", 0x038B, -1, true},
			{"k_late", 0xC200, 0, true},
			{"CHROUT", 0xFFD2, -1, false},     // a program's equate: not a ROM label
		};
		prof_report_t q;
		check(prof_report_build(&q, rsyms, 7, covered_prog), "report with ROM labels builds");
		const prof_row_t *k = row_named(&q, "k_thing");
		check(k && k->region == PH_REGION_ROM && k->bank == 0 && k->main_cycles == 10,
		      "ROM 0 time named from bank 0's labels");
		const prof_row_t *b = row_named(&q, "b_thing");
		check(b && b->bank == 4 && b->main_cycles == 20, "ROM 4 from bank 4's, first name wins");
		check(row_named(&q, "zzz") == NULL, "the other name at that address is not used");
		check(row_named(&q, "ROM 7:$C8xx") != NULL, "a bank with no labels stays by page");
		const prof_row_t *i = row_named(&q, "__irq");
		check(i && i->irq_cycles == 30, "KERNAL RAM code named where the program is silent");
		check(row_named(&q, "init") != NULL, "the program's labels still apply");
		prof_report_free(&q);
	}
	return x16_test_summary("profile_report");
}
