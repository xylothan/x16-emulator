// Checks for the saved profile run (src/profile_run.c): everything written
// reads back the same, a damaged file loads nothing, and a section from a
// newer writer is skipped.

#include "profile_run.h"
#include "support/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long
file_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fclose(f);
	return n;
}

static unsigned char *
slurp(const char *path, long *n)
{
	*n       = file_size(path);
	FILE *f  = fopen(path, "rb");
	unsigned char *b = malloc((size_t)(*n > 0 ? *n : 1));
	if (f && b)
		*n = (long)fread(b, 1, (size_t)*n, f);
	if (f)
		fclose(f);
	return b;
}

static void
spit(const char *path, const unsigned char *b, long n)
{
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(b, 1, (size_t)n, f);
		fclose(f);
	}
}

int
main(void)
{
	char msg[512];

	// ── A small run: two routines, a three-node tree, keys, a waterfall ─────
	prof_row_t rows[2];
	memset(rows, 0, sizeof rows);
	snprintf(rows[0].name, sizeof rows[0].name, "MainLoop");
	rows[0].region = 1; rows[0].bank = 3; rows[0].addr = 0xA123; rows[0].named = true;
	rows[0].main_cycles = 900; rows[0].irq_cycles = 10; rows[0].instructions = 300;
	rows[0].incl_cycles = 1000; rows[0].calls = 5; rows[0].call_max = 400; rows[0].frame_max = 950;
	snprintf(rows[1].name, sizeof rows[1].name, "$08xx");
	rows[1].addr = 0x0800; rows[1].ambiguous = true; rows[1].main_cycles = 77;
	prof_node_t nodes[3] = {
		{-1, -1, 0xFFFFFFF0u, 0, 1000, 1, 1000, 1000},
		{0, 0, 0x00A123u, 900, 990, 5, 400, 950},
		{1, 1, 0x000810u, 77, 77, 9, 20, 30},
	};
	prof_report_t rep;
	memset(&rep, 0, sizeof rep);
	rep.rows = rows; rep.count = 2; rep.nodes = nodes; rep.node_count = 3;
	rep.main_cycles = 977; rep.irq_cycles = 10; rep.instructions = 400; rep.named_rows = 1; rep.ambiguous_rows = 1;

	char src_a[] = "main.s:12", src_b[] = "";
	prof_run_key_t keys[2] = {{0x000810u, 1, src_a}, {0x00A123u, 0, src_b}};

	prof_run_meta_t meta;
	memset(&meta, 0, sizeof meta);
	snprintf(meta.captured, sizeof meta.captured, "2026-10-25 16:21:41");
	snprintf(meta.revision, sizeof meta.revision, "abc1234");
	snprintf(meta.program, sizeof meta.program, "C:\\games\\demo.prg");
	snprintf(meta.dbg, sizeof meta.dbg, "C:\\games\\demo.dbg");
	snprintf(meta.end_reason, sizeof meta.end_reason, "frame count reached");
	meta.mhz = 8.0; meta.elapsed_s = 2.5; meta.frames = 3; meta.recorders = 0x1D; meta.tree_consistent = true;
	meta.b_have = true; meta.b_frames = 3; meta.b_total = 403200; meta.b_idle = 100000;
	meta.b_work_max = 120000; meta.b_overruns = 1; meta.b_budget = 134400;

	check(wf_init(64, 1 << 20), "waterfall ready");
	for (int i = 0; i < 3; i++) {
		wf_note(WF_LANE_PALETTE, (uint16_t)i, 7, 0);
		wf_flush(0x00A123u, false, (uint64_t)i * 100 + 5);
		wf_frame_end((uint64_t)(i + 1) * 100, 50u + (uint64_t)i, 1);
	}

	check(prof_run_write("run_test" PROF_RUN_EXT, &meta, &rep, keys, 2, true, msg, sizeof msg) == 0, "written");

	// ── Read back: every field as written ───────────────────────────────────
	wf_reset();
	prof_run_t run;
	check(prof_run_read("run_test" PROF_RUN_EXT, &run, msg, sizeof msg) == 0, "read");
	if (strncmp(msg, "Loaded", 6) != 0)
		printf("      %s\n", msg);
	check(strcmp(run.meta.captured, meta.captured) == 0 && strcmp(run.meta.program, meta.program) == 0 &&
	          strcmp(run.meta.dbg, meta.dbg) == 0 && strcmp(run.meta.end_reason, meta.end_reason) == 0 &&
	          strcmp(run.meta.revision, meta.revision) == 0,
	      "metadata text");
	check(run.meta.mhz == 8.0 && run.meta.elapsed_s == 2.5 && run.meta.frames == 3 && run.meta.recorders == 0x1D &&
	          run.meta.tree_consistent,
	      "metadata numbers");
	check(run.meta.b_have && run.meta.b_frames == 3 && run.meta.b_total == 403200 && run.meta.b_idle == 100000 &&
	          run.meta.b_work_max == 120000 && run.meta.b_overruns == 1 && run.meta.b_budget == 134400,
	      "the work-against-waiting summary");
	const prof_report_t *r = &run.report;
	check(r->count == 2 && r->main_cycles == 977 && r->irq_cycles == 10 && r->instructions == 400 &&
	          r->named_rows == 1 && r->ambiguous_rows == 1 && r->lookup == NULL,
	      "report totals");
	bool rows_same = r->count == 2;
	for (int i = 0; rows_same && i < 2; i++)
		rows_same = !strcmp(r->rows[i].name, rows[i].name) && r->rows[i].region == rows[i].region &&
		            r->rows[i].bank == rows[i].bank && r->rows[i].addr == rows[i].addr &&
		            r->rows[i].named == rows[i].named && r->rows[i].ambiguous == rows[i].ambiguous &&
		            r->rows[i].main_cycles == rows[i].main_cycles && r->rows[i].irq_cycles == rows[i].irq_cycles &&
		            r->rows[i].instructions == rows[i].instructions &&
		            r->rows[i].incl_cycles == rows[i].incl_cycles && r->rows[i].calls == rows[i].calls &&
		            r->rows[i].call_max == rows[i].call_max && r->rows[i].frame_max == rows[i].frame_max;
	check(rows_same, "every row field");
	bool nodes_same = r->node_count == 3;
	for (int i = 0; nodes_same && i < 3; i++)
		nodes_same = r->nodes[i].parent == nodes[i].parent && r->nodes[i].row == nodes[i].row &&
		             r->nodes[i].key == nodes[i].key && r->nodes[i].self == nodes[i].self &&
		             r->nodes[i].incl == nodes[i].incl && r->nodes[i].calls == nodes[i].calls &&
		             r->nodes[i].call_max == nodes[i].call_max && r->nodes[i].frame_max == nodes[i].frame_max;
	check(nodes_same, "every node field");
	const prof_run_key_t *ka = prof_run_find_key(run.keys, run.nkeys, 0x000810u);
	const prof_run_key_t *kb = prof_run_find_key(run.keys, run.nkeys, 0x00A123u);
	check(ka && ka->row == 1 && !strcmp(ka->source, "main.s:12") && kb && kb->row == 0 && !kb->source[0],
	      "keys: their rows and source lines, found by key");
	check(prof_run_find_key(run.keys, run.nkeys, 0x123456u) == NULL, "an unknown key is not found");
	check(run.wf != NULL && wf_install(run.wf), "the waterfall installs");
	run.wf = NULL;
	check(wf_frame_count() == 3 && wf_frame(2)->work == 52 && wf_frame(1)->writes == 1, "with its frames");
	prof_run_free(&run);

	// ── Damage: nothing loads, and says why ─────────────────────────────────
	long           n;
	unsigned char *whole = slurp("run_test" PROF_RUN_EXT, &n);
	spit("run_cut" PROF_RUN_EXT, whole, n - 20);
	check(prof_run_read("run_cut" PROF_RUN_EXT, &run, msg, sizeof msg) != 0 && run.report.rows == NULL &&
	          strstr(msg, "damaged"),
	      "a file cut short loads nothing");
	spit("run_junk" PROF_RUN_EXT, (const unsigned char *)"hello, not a run", 16);
	check(prof_run_read("run_junk" PROF_RUN_EXT, &run, msg, sizeof msg) != 0 && strstr(msg, "not a saved"),
	      "a file that is not a run is refused");
	check(prof_run_read("no_such" PROF_RUN_EXT, &run, msg, sizeof msg) != 0, "a missing file is refused");

	// ── A section this reader does not know is skipped ──────────────────────
	// Insert "ZZZZ", 3 bytes, before the END section (the last 12 bytes).
	unsigned char *more = malloc((size_t)n + 15);
	memcpy(more, whole, (size_t)(n - 12));
	const unsigned char extra[15] = {'Z', 'Z', 'Z', 'Z', 3, 0, 0, 0, 0, 0, 0, 0, 'a', 'b', 'c'};
	memcpy(more + n - 12, extra, 15);
	memcpy(more + n + 3, whole + n - 12, 12);
	spit("run_newer" PROF_RUN_EXT, more, n + 15);
	check(prof_run_read("run_newer" PROF_RUN_EXT, &run, msg, sizeof msg) == 0 && run.report.count == 2,
	      "a newer writer's extra section is skipped");
	prof_run_free(&run);
	free(more);
	free(whole);

	remove("run_test" PROF_RUN_EXT);
	remove("run_cut" PROF_RUN_EXT);
	remove("run_junk" PROF_RUN_EXT);
	remove("run_newer" PROF_RUN_EXT);
	wf_free();
	return x16_test_summary("profile_run");
}
