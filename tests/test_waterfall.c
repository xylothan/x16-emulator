// Checks for the frame waterfall recorder (src/waterfall.c): stamping, the
// frame ring, the worst-K list, truncation, aggregates, and the export walk's
// begin/end nesting.

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

int
main(void)
{
	check(wf_init(3, 2, 64), "init");

	// ── Writes are stamped with the instruction that made them ──────────────
	{
		wf_reset();
		wf_note(WF_LANE_PALETTE, 0xFA02, 0x0F, 0);
		check_eq((uint32_t)wf_pending, 1, "a write waits for its instruction");
		wf_flush(0x0810, false, 100);
		check_eq((uint32_t)wf_pending, 0, "and is stamped by the flush");
		wf_flush(0x0999, false, 200); // nothing queued: no event
		wf_frame_end(1000, 0, 0);
		const wf_frame_t *f = wf_ring_at(0);
		check(f && f->count == 1, "one event");
		check_eq(wf_event_key(&f->events[0]), 0x0810, "keyed by the writing instruction");
		check_eq((uint32_t)wf_event_lane(&f->events[0]), WF_LANE_PALETTE, "in its lane");
		check_eq(f->events[0].cycle, 100, "at the instruction's start");
		check_eq(f->length, 1000, "frame length");

		wf_note(WF_LANE_YM, 0x20, 0x55, 0);
		wf_flush(0x0900, true, 1100);
		wf_frame_end(2000, 0, 0);
		f = wf_ring_at(1);
		check(f && f->count == 1 && f->events[0].cycle == 100, "times are frame-relative");
		check(f && (f->events[0].flags & WF_F_IRQ), "an IRQ write is flagged");
	}

	// ── The ring keeps the newest N, oldest first ───────────────────────────
	{
		wf_reset();
		for (int i = 0; i < 5; i++) {
			for (int k = 0; k <= i; k++) {
				wf_note(WF_LANE_VIA1, 0, 0, 0);
				wf_flush(0x1000, false, (uint64_t)i * 1000 + (uint64_t)k);
			}
			wf_frame_end((uint64_t)(i + 1) * 1000, 0, 0);
		}
		check_eq((uint32_t)wf_ring_count(), 3, "ring holds three");
		check_eq(wf_ring_at(0)->number, 2, "oldest kept is frame 2");
		check_eq(wf_ring_at(2)->number, 4, "newest is frame 4");
		check_eq(wf_frames_seen(), 5, "five frames seen");
	}

	// ── Worst K survive leaving the ring ────────────────────────────────────
	{
		wf_reset();
		const uint64_t scores[] = {10, 90, 20, 70, 30, 40};
		for (int i = 0; i < 6; i++)
			wf_frame_end((uint64_t)(i + 1) * 1000, scores[i], 0);
		check_eq((uint32_t)wf_worst_count(), 2, "two worst kept");
		check_eq((uint32_t)wf_worst_at(0)->score, 90, "the worst first");
		check_eq(wf_worst_at(0)->number, 1, "frame 1, long gone from the ring");
		check_eq((uint32_t)wf_worst_at(1)->score, 70, "then the next");
		check_eq(wf_ring_at(0)->number, 3, "while the ring moved on");
	}

	// ── A full frame is flagged, not silently cut ───────────────────────────
	{
		wf_reset();
		for (int i = 0; i < 100; i++) {
			wf_note(WF_LANE_L0_MAP, (uint16_t)i, 0, 0);
			wf_flush(0x2000, false, (uint64_t)i);
		}
		wf_frame_end(5000, 0, 0);
		check(wf_ring_at(0)->truncated, "overflow flagged");
		check_eq((uint32_t)wf_ring_at(0)->count, 64, "at the buffer's size");
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
		const wf_frame_t *f = wf_ring_at(wf_ring_count() - 1);
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
		const walk_t w = walk(wf_ring_at(wf_ring_count() - 1));
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
		const wf_frame_t *f = wf_ring_at(wf_ring_count() - 1);

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
