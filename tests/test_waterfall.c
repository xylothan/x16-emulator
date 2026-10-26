// Checks for the frame waterfall recorder (src/waterfall.c): stamping, every
// frame's summary, picking frames, thinning detail to a budget, truncation,
// aggregates, and the export walk's begin/end nesting.

#include "waterfall.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

// ── A tiny call tree for the walk: 0 [main] -> 2 A -> 3 B ; 1 [IRQ] -> 4 H ──
static int
parent_of(void *ctx, int node)
{
	(void)ctx;
	static const int parents[] = {-1, -1, 0, 2, 1};
	return node >= 0 && node < 5 ? parents[node] : -1;
}

typedef struct {
	char log[2048];
	int  depth[2];
	bool balanced;
	int  events;
	int  marks;
} walk_t;

static void
emit(void *ctx, wf_out_kind_t kind, int thread, uint32_t cycle, int node, const wf_event_t *ev)
{
	walk_t *w = ctx;
	char    b[48];
	(void)ev;
	switch (kind) {
		case WF_OUT_BEGIN:
			w->depth[thread]++;
			snprintf(b, sizeof b, "B%d:%d@%u ", thread, node, cycle);
			break;
		case WF_OUT_END:
			if (--w->depth[thread] < 0)
				w->balanced = false;
			snprintf(b, sizeof b, "E%d:%d@%u ", thread, node, cycle);
			break;
		case WF_OUT_MARK:
			w->marks++;
			return;
		default:
			w->events++;
			snprintf(b, sizeof b, "i%d@%u ", thread, cycle);
			break;
	}
	strncat(w->log, b, sizeof w->log - strlen(w->log) - 1);
}

static walk_t
walk(const wf_frame_t *f)
{
	walk_t w;
	memset(&w, 0, sizeof w);
	w.balanced = true;
	wf_walk_frame(f, parent_of, 1, emit, &w);
	w.balanced = w.balanced && w.depth[0] == 0 && w.depth[1] == 0;
	return w;
}

typedef struct { int node[8]; uint32_t cyc[8]; int n; int writes; } range_t;

static void
range_node(void *ctx, int node, uint32_t cycles)
{
	range_t *r = ctx;
	if (r->n < 8) {
		r->node[r->n] = node;
		r->cyc[r->n]  = cycles;
		r->n++;
	}
}

static void
range_write(void *ctx, const wf_event_t *ev)
{
	(void)ev;
	((range_t *)ctx)->writes++;
}

// The marks in a frame, as "ON@cycle OFF@cycle ...".
static void
marks_of(const wf_frame_t *f, char *out, size_t n)
{
	out[0] = 0;
	for (int i = 0; f && i < f->count; i++)
		if (wf_event_is_mark(&f->events[i])) {
			char b[24];
			snprintf(b, sizeof b, "%s@%u ", f->events[i].target == WF_MARK_HELD_ON ? "ON" : "OFF",
			         f->events[i].cycle);
			strncat(out, b, n - strlen(out) - 1);
		}
}

int
main(void)
{
	check(wf_init(64, 1 << 20), "init");

	// ── Writes are stamped with the instruction that made them ──────────────
	{
		wf_reset();
		wf_note(WF_LANE_PALETTE, 0xFA02, 0x0F, 0);
		check_eq((uint32_t)wf_pending, 1, "a write waits for its instruction");
		wf_flush(0x0810, false, 100);
		check_eq((uint32_t)wf_pending, 0, "and is stamped by the flush");
		wf_flush(0x0999, false, 200); // nothing queued: no event
		wf_frame_end(1000, 0, 0);
		const wf_frame_t *f = wf_frame(0);
		check(f && f->count == 1, "one event");
		check_eq(wf_event_key(&f->events[0]), 0x0810, "keyed by the writing instruction");
		check_eq((uint32_t)wf_event_lane(&f->events[0]), WF_LANE_PALETTE, "in its lane");
		check_eq(f->events[0].cycle, 100, "at the instruction's start");
		check_eq(f->length, 1000, "frame length");

		wf_note(WF_LANE_YM, 0x20, 0x55, 0);
		wf_flush(0x0900, true, 1100);
		wf_frame_end(2000, 0, 0);
		f = wf_frame(1);
		check(f && f->count == 1 && f->events[0].cycle == 100, "times are frame-relative");
		check(f && (f->events[0].flags & WF_F_IRQ), "an IRQ write is flagged");
	}

	// ── A frame carries the output mode it was recorded in ──────────────────
	{
		wf_reset();
		check_eq(wf_frame_count(), 0, "empty");
		wf_frame_end(100, 0, 0);
		check_eq(wf_frame(0)->video_mode, 1, "VGA by default");
		wf_frame_set_video_mode(2);
		wf_frame_end(200, 0, 0);
		check_eq(wf_frame(1)->video_mode, 2, "NTSC once set");
		wf_frame_end(300, 0, 0);
		check_eq(wf_frame(2)->video_mode, 2, "and it sticks for later frames");
		wf_frame_set_video_mode(0x83);
		wf_frame_end(400, 0, 0);
		check_eq(wf_frame(3)->video_mode, 3, "only the mode bits are kept");
	}

	// ── Every frame is kept, oldest first, with its writes counted ──────────
	{
		wf_reset();
		for (int i = 0; i < 5; i++) {
			for (int k = 0; k <= i; k++) {
				wf_note(WF_LANE_VIA1, 0, 0, 0);
				wf_flush(0x1000, false, (uint64_t)i * 1000 + (uint64_t)k);
			}
			wf_frame_end((uint64_t)(i + 1) * 1000, 0, 0);
		}
		check_eq((uint32_t)wf_frame_count(), 5, "all five kept");
		check_eq(wf_frames_seen(), 5, "five frames seen");
		check_eq(wf_frame(0)->number, 0, "oldest first");
		check_eq(wf_frame(4)->writes, 5, "frame 4 made five writes");
		check_eq((uint32_t)wf_detail_count(), 5, "with detail, inside the budget");
		check(wf_frame(4)->events != NULL, "events kept");
		check_eq((uint32_t)wf_detail_bytes(), (uint32_t)(sizeof(wf_event_t) * 15), "at their own size");
	}

	// ── Picking: worst, best, median, 90th percentile, average, all ─────────
	{
		wf_reset();
		const uint64_t work[]   = {10, 90, 20, 70, 30, 40};
		const int      writes[] = {3, 1, 4, 1, 5, 9};
		for (int i = 0; i < 6; i++) {
			for (int k = 0; k < writes[i]; k++) {
				wf_note(WF_LANE_PSG, 0, 0, 0);
				wf_flush(0x1000, false, (uint64_t)i * 1000 + (uint64_t)k);
			}
			wf_frame_end((uint64_t)(i + 1) * 1000, work[i], 0);
		}
		int out[16], n;
		n = wf_pick(WF_PICK_ALL, WF_BY_WORK, out, 16);
		check(n == 6 && out[0] == 0 && out[5] == 5, "all, in order");
		n = wf_pick(WF_PICK_WORST, WF_BY_WORK, out, 16);
		check(n == 6 && out[0] == 1 && out[1] == 3 && out[2] == 5 && out[5] == 0, "worst work first: 90, 70, 40 ...");
		n = wf_pick(WF_PICK_BEST, WF_BY_WORK, out, 16);
		check(n == 6 && out[0] == 0 && out[1] == 2 && out[5] == 1, "best work first: 10, 20 ...");
		n = wf_pick(WF_PICK_MEDIAN, WF_BY_WORK, out, 16);
		check(n == 1 && out[0] == 4, "median work: 30 (lower middle of 10 20 30 40 70 90)");
		n = wf_pick(WF_PICK_P90, WF_BY_WORK, out, 16);
		check(n == 1 && out[0] == 1, "90th percentile work: 90");
		n = wf_pick(WF_PICK_AVERAGE, WF_BY_WORK, out, 16);
		check(n == 1 && out[0] == 5, "nearest the mean (43.3): 40");
		n = wf_pick(WF_PICK_WORST, WF_BY_WRITES, out, 16);
		check(n == 6 && out[0] == 5 && out[1] == 4 && out[2] == 2, "worst writes: 9, 5, 4 ...");
		check(out[4] == 1 && out[5] == 3, "ties go to the earlier frame");
		n = wf_pick(WF_PICK_BEST, WF_BY_WRITES, out, 2);
		check(n == 2 && out[0] == 1 && out[1] == 3, "best writes, capped at max");
		n = wf_pick(WF_PICK_MEDIAN, WF_BY_WRITES, out, 16);
		check(n == 1 && out[0] == 0, "median writes: 3");
	}

	// ── Over budget: thin evenly, never the extremes ────────────────────────
	{
		// Four events (48 bytes) a frame; room for 48 frames' detail of 200.
		check(wf_init(64, sizeof(wf_event_t) * 4 * 48), "init small");
		for (int i = 0; i < 200; i++) {
			for (int k = 0; k < 4; k++) {
				wf_note(WF_LANE_PSG, 0, 0, 0);
				wf_flush(0x1000, false, (uint64_t)i * 1000 + (uint64_t)k);
			}
			wf_frame_end((uint64_t)(i + 1) * 1000, (uint64_t)((i * 37) % 200), 0);
		}
		check_eq((uint32_t)wf_frame_count(), 200, "every summary kept");
		check(wf_detail_bytes() <= sizeof(wf_event_t) * 4 * 48, "detail within the budget");
		check(wf_detail_count() > 32 && wf_detail_count() <= 48, "and the budget is used");

		bool prot[200] = {false}, all_kept = true;
		for (int m = 0; m < 2; m++)
			for (int p = WF_PICK_WORST; p <= WF_PICK_BEST; p++) {
				int out[WF_PROTECT];
				const int n = wf_pick(p, m, out, WF_PROTECT);
				for (int k = 0; k < n; k++) {
					prot[out[k]] = true;
					all_kept     = all_kept && wf_frame(out[k])->events != NULL;
				}
			}
		check(all_kept, "the worst and best by both measures keep their detail");

		// The others kept are evenly spaced: all multiples of one stride.
		int stride = 1 << 30, first_half = 0, second_half = 0;
		for (int i = 0; i < 200; i++)
			if (wf_frame(i)->events && !prot[i]) {
				int s = i ? (i & -i) : 1 << 30;
				stride = s < stride ? s : stride;
				first_half += i < 100;
				second_half += i >= 100;
			}
		bool even = stride >= 2;
		for (int i = 0; i < 200; i++)
			if (wf_frame(i)->events && !prot[i] && i % stride)
				even = false;
		check(even, "the rest share one stride");
		check(first_half && second_half, "spread over the whole capture");

		int gone = -1;
		for (int i = 0; i < 200 && gone < 0; i++)
			if (!wf_frame(i)->events)
				gone = i;
		const int near = wf_nearest_detail(gone);
		bool      none_closer = near >= 0 && wf_frame(near)->events;
		const int d           = near > gone ? near - gone : gone - near;
		for (int i = 0; i < 200 && none_closer; i++)
			if (wf_frame(i)->events && (i > gone ? i - gone : gone - i) < d)
				none_closer = false;
		check(gone >= 0 && none_closer, "a frame without detail finds its nearest with");
		check_eq((uint32_t)wf_nearest_detail(near), (uint32_t)near, "a kept frame is its own nearest");

		check(wf_init(64, 1 << 20), "init again");
	}

	// ── A full frame is flagged, not silently cut ───────────────────────────
	{
		wf_reset();
		for (int i = 0; i < 100; i++) {
			wf_note(WF_LANE_L0_MAP, (uint16_t)i, 0, 0);
			wf_flush(0x2000, false, (uint64_t)i);
		}
		wf_frame_end(5000, 0, 0);
		check(wf_frame(0)->truncated, "overflow flagged");
		check_eq(wf_frame(0)->writes, 100, "but every write is counted");
		check_eq((uint32_t)wf_frame(0)->count, 64, "at the buffer's size");
		int n;
		const wf_agg_t *a = wf_aggregates(&n);
		check(n == 1 && a[0].writes == 100, "aggregates still count every write");
	}

	// ── Aggregates by (key, lane), heaviest first ───────────────────────────
	{
		wf_reset();
		for (int i = 0; i < 5; i++) { wf_note(WF_LANE_PALETTE, 0, 0, 0); wf_flush(0x0810, false, 1); }
		for (int i = 0; i < 9; i++) { wf_note(WF_LANE_SPRITES, 0, 0, 0); wf_flush(0x0810, false, 2); }
		for (int i = 0; i < 2; i++) { wf_note(WF_LANE_PALETTE, 0, 0, 0); wf_flush(0x0900, false, 3); }
		int n;
		const wf_agg_t *a = wf_aggregates(&n);
		check_eq((uint32_t)n, 3, "three (key, lane) pairs");
		check(a[0].key == 0x0810 && a[0].lane == WF_LANE_SPRITES && a[0].writes == 9, "heaviest first");
		check(a[1].key == 0x0810 && a[1].lane == WF_LANE_PALETTE && a[1].writes == 5, "same key, other lane");
		check(a[2].key == 0x0900 && a[2].writes == 2, "then the lightest");
	}

	// ── An interrupt held off by the I flag, and how the stretch ends ───────
	{
		char m[256];
		wf_reset();
		// Held after instructions 10..12 (ending at 100, 110, 120), taken after 13.
		wf_irq_line(true, 100, 10);
		wf_irq_line(true, 110, 11);
		wf_irq_line(true, 120, 12);
		wf_irq_line(false, 130, 13);
		// Held after 20 and 21, then the line is withdrawn: 22 never reports.
		wf_irq_line(true, 200, 20);
		wf_irq_line(true, 210, 21);
		wf_irq_line(true, 300, 30); // held again, later
		wf_irq_poll(30);            // still held at the frame's end
		wf_frame_end(400, 0, 0);
		marks_of(wf_frame(0), m, sizeof m);
		check(strcmp(m, "ON@100 OFF@130 ON@200 OFF@210 ON@300 OFF@400 ") == 0,
		      "held from the first held instruction to when it was taken or withdrawn");
		if (strcmp(m, "ON@100 OFF@130 ON@200 OFF@210 ON@300 OFF@400 ") != 0)
			printf("      marks: %s\n", m);
		check_eq(wf_frame(0)->writes, 0, "marks are not writes");
		int na;
		wf_aggregates(&na);
		check_eq((uint32_t)na, 0, "nor in the aggregates");

		wf_irq_line(true, 410, 31); // the same stretch, into the next frame
		wf_irq_poll(40);            // and over before it ends
		wf_frame_end(800, 0, 0);
		marks_of(wf_frame(1), m, sizeof m);
		check(strcmp(m, "ON@0 OFF@10 ") == 0, "a stretch over a frame's end reopens at the next's start");
		if (strcmp(m, "ON@0 OFF@10 ") != 0)
			printf("      marks: %s\n", m);

		range_t r;
		memset(&r, 0, sizeof r);
		wf_frame_range(wf_frame(0), 0, 400, range_node, range_write, &r);
		check_eq((uint32_t)r.writes, 0, "a range does not count marks as writes");
		const walk_t w = walk(wf_frame(0));
		check(w.balanced && w.events == 0 && w.marks == 6, "the walk hands marks out as marks");
	}

	// ── Saved and loaded: the same frames, picks and aggregates ─────────────
	{
		// Room for half the frames' detail, so some summaries have none.
		check(wf_init(64, sizeof(wf_event_t) * 3 * 40), "init for the round trip");
		for (int i = 0; i < 80; i++) {
			for (int k = 0; k < 1 + i % 3; k++) {
				wf_note(k ? WF_LANE_PSG : WF_LANE_VIA1, (uint16_t)(i * 7 + k), (uint8_t)k, 0);
				wf_flush(0x1000u + (uint32_t)k, (i % 5) == 0, (uint64_t)i * 1000 + (uint64_t)k);
			}
			if (i % 9 == 0) {
				wf_irq_line(true, (uint64_t)i * 1000 + 50, (uint32_t)i * 100);
				wf_irq_line(false, (uint64_t)i * 1000 + 90, (uint32_t)i * 100 + 1);
			}
			wf_frame_set_video_mode((uint8_t)(i % 4 == 3 ? 2 : 1));
			wf_frame_end((uint64_t)(i + 1) * 1000, (uint64_t)((i * 37) % 80), (uint16_t)(i % 4));
		}
		const int  n0 = wf_frame_count(), d0 = wf_detail_count();
		int        picks0[6][2][80], np0[6][2];
		for (int pk = 0; pk < 6; pk++)
			for (int m = 0; m < 2; m++)
				np0[pk][m] = wf_pick(pk, m, picks0[pk][m], 80);
		int             na0;
		const wf_agg_t *a0 = wf_aggregates(&na0);
		wf_agg_t        agg0[64];
		for (int i = 0; i < na0 && i < 64; i++)
			agg0[i] = a0[i];
		// A copy of every frame, events included.
		static wf_frame_t f0[80];
		static wf_event_t e0[80][16];
		for (int i = 0; i < n0; i++) {
			f0[i] = *wf_frame(i);
			if (f0[i].events)
				memcpy(e0[i], f0[i].events, sizeof(wf_event_t) * (size_t)f0[i].count);
		}

		FILE *f = fopen("wf_roundtrip.bin", "wb");
		check(f && wf_write(f), "written");
		const long len = f ? ftell(f) : 0;
		if (f)
			fclose(f);
		static uint8_t buf[1 << 16];
		f = fopen("wf_roundtrip.bin", "rb");
		const size_t got = f ? fread(buf, 1, sizeof buf, f) : 0;
		if (f)
			fclose(f);
		remove("wf_roundtrip.bin");
		check(len > 0 && got == (size_t)len, "and read back whole");

		wf_reset();
		check_eq((uint32_t)wf_frame_count(), 0, "cleared before loading");
		check(wf_read(buf, got - 1) == NULL, "a file cut short is refused");
		uint8_t bad = buf[0];
		buf[0]      = 99;
		check(wf_read(buf, got) == NULL, "a version it does not know is refused");
		buf[0] = bad;
		check(wf_install(wf_read(buf, got)), "a whole one loads");

		check(wf_frame_count() == n0 && wf_detail_count() == d0, "the same frames, the same detail kept");
		bool same_frames = true;
		for (int i = 0; i < n0; i++) {
			const wf_frame_t *g = wf_frame(i);
			same_frames = same_frames && g->number == f0[i].number && g->start == f0[i].start &&
			              g->work == f0[i].work && g->writes == f0[i].writes && g->length == f0[i].length &&
			              g->start_node == f0[i].start_node && g->video_mode == f0[i].video_mode &&
			              g->truncated == f0[i].truncated && g->count == f0[i].count &&
			              (g->events != NULL) == (f0[i].events != NULL) &&
			              (!g->events || memcmp(g->events, e0[i], sizeof(wf_event_t) * (size_t)g->count) == 0);
		}
		check(same_frames, "every frame's summary and events are as saved");
		bool same_picks = true;
		for (int pk = 0; pk < 6; pk++)
			for (int m = 0; m < 2; m++) {
				int       p1[80];
				const int np = wf_pick(pk, m, p1, 80);
				same_picks   = same_picks && np == np0[pk][m] &&
				             memcmp(p1, picks0[pk][m], sizeof(int) * (size_t)np) == 0;
			}
		check(same_picks, "every pick, by both measures, is as before");
		int             na1;
		const wf_agg_t *a1 = wf_aggregates(&na1);
		bool            same_agg = na1 == na0;
		for (int i = 0; same_agg && i < na1; i++)
			same_agg = a1[i].key == agg0[i].key && a1[i].lane == agg0[i].lane && a1[i].writes == agg0[i].writes;
		check(same_agg, "and the aggregates");
		check(wf_frames_seen() == (uint32_t)n0, "frames seen restored");

		check(wf_init(64, 1 << 20), "init again");
	}

	// ── The walk: nested spans, two threads, balanced ───────────────────────
	{
		wf_reset();
		wf_frame_end(0, 1, 0);       // start the next frame in [main]
		wf_cpu(2, 10);               // A
		wf_cpu(3, 20);               // A/B
		wf_note(WF_LANE_VIA1, 0, 0, 0);
		wf_flush(0x3000, false, 25); // a write in B
		wf_cpu(4, 30);               // IRQ handler H: B stays open on thread 0
		wf_note(WF_LANE_VIA1, 0, 0, 0);
		wf_flush(0x3100, true, 35);
		wf_cpu(3, 40);               // back in B: H ends
		wf_cpu(0, 50);               // B and A return together
		wf_frame_end(100, 1, 0);
		const wf_frame_t *f = wf_frame(wf_frame_count() - 1);
		const walk_t      w = walk(f);
		check(w.balanced, "every begin has its end, per thread");
		check_eq((uint32_t)w.events, 2, "both writes emitted");
		check(strstr(w.log, "B0:0@0 B0:2@10 B0:3@20 i0@25 B1:1@30 B1:4@30 i1@35 E1:4@40 E1:1@40 "
		                    "E0:3@50 E0:2@50 E0:0@100") != NULL,
		      "spans nest, interrupts on their own thread");
		if (!w.balanced || !strstr(w.log, "E0:3@50"))
			printf("      walk: %s\n", w.log);
	}

	// ── A frame that starts inside a call opens its path first ──────────────
	{
		wf_reset();
		wf_frame_end(0, 1, 3);       // next frame starts in [main]/A/B
		wf_cpu(2, 10);               // B returns to A
		wf_frame_end(50, 1, 2);
		const walk_t w = walk(wf_frame(wf_frame_count() - 1));
		check(w.balanced, "balanced");
		check(strncmp(w.log, "B0:0@0 B0:2@0 B0:3@0 E0:3@10", 28) == 0, "opens main/A/B at 0");
	}

	// ── A range: cycles per innermost node, and the writes in it ────────────
	{
		wf_reset();
		wf_frame_end(0, 1, 0);         // frame starts in [main] (node 0)
		wf_cpu(2, 100);                // A from 100
		wf_note(WF_LANE_PALETTE, 0, 0, 0);
		wf_flush(0x0810, false, 150);  // a write at 150
		wf_cpu(3, 200);                // B from 200
		wf_cpu(2, 300);                // back in A at 300
		wf_note(WF_LANE_VIA1, 0, 0, 0);
		wf_flush(0x0900, false, 350);
		wf_frame_end(400, 1, 0);
		const wf_frame_t *f = wf_frame(wf_frame_count() - 1);

		range_t r;
		memset(&r, 0, sizeof r);
		wf_frame_range(f, 0, 400, range_node, range_write, &r);
		check_eq((uint32_t)r.n, 4, "four stretches over the whole frame");
		check(r.node[0] == 0 && r.cyc[0] == 100, "[main] 0-100");
		check(r.node[1] == 2 && r.cyc[1] == 100, "A 100-200");
		check(r.node[2] == 3 && r.cyc[2] == 100, "B 200-300");
		check(r.node[3] == 2 && r.cyc[3] == 100, "A again 300-400, to the frame's end");
		check_eq((uint32_t)r.writes, 2, "both writes");

		memset(&r, 0, sizeof r);
		wf_frame_range(f, 150, 250, range_node, range_write, &r);
		check(r.n == 2 && r.node[0] == 2 && r.cyc[0] == 50 && r.node[1] == 3 && r.cyc[1] == 50,
		      "a range cuts the stretches it overlaps");
		check_eq((uint32_t)r.writes, 1, "and holds only its own writes (150 is in, 350 is not)");

		memset(&r, 0, sizeof r);
		wf_frame_range(f, 250, 250, range_node, range_write, &r);
		check(r.n == 0 && r.writes == 0, "an empty range holds nothing");
	}

	wf_free();
	return x16_test_summary("waterfall");
}
