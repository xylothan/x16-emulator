// Checks for guest performance budget accounting (src/perf_budget.c).
//
// The module's whole job is to decide which cycles were WORK and which were
// WAITING, so most of what is worth testing is the classification: that a spin
// loop is recognised, that something which merely looks like one is not, and
// that the three layers override each other in the documented order. The
// statistics on top of that are ordinary arithmetic, but they are the numbers a
// developer would act on, so the percentile and overrun maths is pinned too.
//
// perf_budget.c reaches for no emulator state, so this drives it directly: no
// CPU, no memory, no video, no SDL.

#include "perf_budget.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

// The machine this file assumes throughout: 8 MHz, VGA. Stated once here so a
// change of default in the module cannot quietly rewrite what the checks mean.
#define KHZ              8000
#define CYCLES_PER_FRAME 134400
#define SCANLINES        525

static void
configure(void)
{
	perf_budget_zone_clear();
	perf_budget_set_enabled(false);
	perf_budget_set_capacity(PERF_DEFAULT_CAPACITY);
	perf_budget_configure(KHZ, CYCLES_PER_FRAME, SCANLINES);
	perf_budget_set_target_fps(60.0f);
	perf_budget_set_idle_mode(PERF_IDLE_AUTO);
	perf_budget_set_enabled(true);
	perf_budget_reset();
}

// Run `count` instructions of straight-line work, each costing `clocks`.
static void
run_work(uint32_t clocks, int count)
{
	for (int i = 0; i < count; i++)
		perf_budget_step(clocks, (uint16_t)(0x2000 + i * 3), 0, false, 0);
}

// Run `passes` iterations of a three-instruction loop at `base` that stores
// nothing -- the shape of `lda $9F27 / and #1 / beq -`.
static void
run_spin(uint16_t base, int passes)
{
	for (int p = 0; p < passes; p++) {
		perf_budget_step(4, base, 0, false, 0);
		perf_budget_step(2, (uint16_t)(base + 3), 0, false, 0);
		perf_budget_step(3, (uint16_t)(base + 5), 0, false, 0);
	}
}

int
main(void)
{
	// ── Derived machine figures ─────────────────────────────────────────────
	{
		configure();
		check_eq(perf_budget_cycles_per_frame(), CYCLES_PER_FRAME,
		         "a VGA frame at 8 MHz is 134,400 cycles");
		check_eq(perf_budget_cycles_per_scanline(), 256,
		         "which is 256 cycles per scanline over 525 lines");
		check_eq(perf_budget_budget_cycles(), CYCLES_PER_FRAME,
		         "and at 60 fps the budget is one frame's worth");
		check_eq((uint32_t)perf_budget_frames_per_budget(), 1,
		         "spanning a single vsync");

		const float hz = perf_budget_vsync_hz();
		check(hz > 59.0f && hz < 60.5f, "vsync is derived at ~59.5 Hz");
	}

	// ── A lower target fps buys more frames, not longer ones ────────────────
	{
		configure();
		perf_budget_set_target_fps(30.0f);
		check_eq((uint32_t)perf_budget_frames_per_budget(), 2,
		         "30 fps lets the work span two vsyncs");
		check_eq(perf_budget_budget_cycles(), CYCLES_PER_FRAME * 2,
		         "so the budget is two frames' cycles");

		perf_budget_set_target_fps(20.0f);
		check_eq((uint32_t)perf_budget_frames_per_budget(), 3, "20 fps spans three");

		perf_budget_set_target_fps(60.0f);
		check_eq((uint32_t)perf_budget_frames_per_budget(), 1, "and 60 fps is back to one");
	}

	// ── Nothing is idle until something says so ─────────────────────────────
	{
		configure();
		run_work(4, 100);
		perf_budget_frame_end(0);

		perf_frame_t f;
		check(perf_budget_last_frame(&f), "a completed frame is retained");
		check_eq(f.total, 400, "every cycle stepped is counted");
		check_eq(f.idle, 0, "straight-line code is never idle");
		check_eq(perf_frame_work(&f), 400, "so all of it is work");
		check_eq(f.instructions, 100, "and the instructions are counted");
	}

	// ── WAI is idle, unambiguously ──────────────────────────────────────────
	{
		configure();
		run_work(4, 10);                             //  40 work
		for (int i = 0; i < 10; i++)                 // 100 idle
			perf_budget_step(10, 0x3000, 0, true, 0);
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.idle, 100, "WAI cycles are idle");
		check_eq(perf_frame_work(&f), 40, "and the rest is work");
	}

	// ── A store-free tight loop is recognised as waiting ────────────────────
	{
		configure();
		run_work(4, 10); // 40 work
		run_spin(0x4000, 20);
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		// Each pass is 9 cycles. The detector re-anchors on the loop head and
		// only credits confirmed passes, so a couple go to work; the point is
		// that the great majority is recognised.
		check(f.idle >= 9 * 16, "a store-free spin loop is mostly idle");
		check(perf_frame_work(&f) < 40 + 9 * 4,
		      "and only the run-up is charged as work");
	}

	// ── A loop that stores is computing, not waiting ────────────────────────
	{
		configure();
		for (int p = 0; p < 20; p++) {
			perf_budget_step(4, 0x4000, 0, false, 0);
			perf_budget_step(2, 0x4003, 0, false, 0);
			perf_budget_note_store(); // the difference from the case above
			perf_budget_step(3, 0x4005, 0, false, 0);
		}
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.idle, 0, "a loop with a side effect is never idle");
		check_eq(perf_frame_work(&f), 9 * 20, "all of it counts as work");
	}

	// ── A loop wider than the span is not a spin ────────────────────────────
	{
		configure();
		for (int p = 0; p < 20; p++) {
			perf_budget_step(4, 0x5000, 0, false, 0);
			perf_budget_step(2, 0x5000 + PERF_SPIN_SPAN + 8, 0, false, 0);
		}
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.idle, 0, "a loop spanning more than PERF_SPIN_SPAN is not a spin");
	}

	// ── Interrupt cycles are counted, and are never idle ────────────────────
	{
		configure();
		run_spin(0x4000, 20);
		for (int i = 0; i < 10; i++)
			perf_budget_step(5, (uint16_t)(0x9000 + i * 2), 0, false, 1);
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.irq, 50, "cycles at interrupt depth are attributed to the handler");
		check(f.idle > 0, "the spin before it is still recognised");
		check(f.idle <= f.total - 50, "but the handler's cycles are not idle");
	}

	// ── An idle zone overrides the heuristic ────────────────────────────────
	{
		configure();
		const int z = perf_budget_zone_add("wait", 0x6000, 0x6100, -1, true);
		check(z >= 0, "an idle zone can be defined");

		// A loop that stores, which the heuristic would call work.
		for (int p = 0; p < 10; p++) {
			perf_budget_step(4, 0x6000, 0, false, 0);
			perf_budget_note_store();
			perf_budget_step(3, 0x6002, 0, false, 0);
		}
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.idle, 70, "an idle zone declares its cycles waiting regardless");
		check_eq(perf_frame_work(&f), 0, "leaving no work at all");
	}

	// ── Zones attribute cycles, and respect their bank ──────────────────────
	{
		configure();
		const int z0 = perf_budget_zone_add("sprites", 0x2000, 0x2100, -1, false);
		const int z1 = perf_budget_zone_add("banked", 0xA000, 0xA100, 3, false);
		check(z0 == 0 && z1 == 1, "zones take the first free slots");

		run_work(4, 4);                                    // $2000,$2003,$2006,$2009
		perf_budget_step(7, 0xA000, 3, false, 0);          // matching bank
		perf_budget_step(9, 0xA000, 5, false, 0);          // wrong bank
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.zone[0], 16, "a zone collects the cycles inside it");
		check_eq(f.zone[1], 7, "a banked zone collects only its own bank");
		check_eq(f.total, 32, "while the frame total counts everything");
	}

	// ── Removing a zone stops attribution ───────────────────────────────────
	{
		configure();
		perf_budget_zone_add("gone", 0x2000, 0x2100, -1, false);
		check(perf_budget_zone_remove(0), "a zone can be removed");
		check(!perf_budget_zone_get(0, NULL), "and is then unknown");
		run_work(4, 4);
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.zone[0], 0, "cycles are no longer attributed to it");
	}

	// ── Markers: the guest says where its work is ───────────────────────────
	{
		configure();
		check_eq(perf_budget_marker_read(), PERF_MARKER_PROTOCOL,
		         "the marker register reads back a protocol version");

		perf_budget_marker_write(0xF0); // work begins
		run_work(4, 10);                // 40 work
		perf_budget_marker_write(0xF1); // work ends
		run_work(4, 10);                // 40 idle -- and it stores, and it is
		perf_budget_note_store();       // straight-line, so only the marker
		run_work(4, 10);                // could possibly call this waiting
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(perf_frame_work(&f), 40, "only the marked span is work");
		check_eq(f.idle, 80, "everything after the end marker is idle");
	}

	// ── Marker regions claim zone slots, and nest ───────────────────────────
	{
		configure();
		perf_budget_marker_write(0xF0);
		perf_budget_marker_write(0x01); // enter region 1
		run_work(4, 5);                 // 20 -> region 1
		perf_budget_marker_write(0x02); // enter region 2, nested
		run_work(4, 5);                 // 20 -> region 2 (innermost wins)
		perf_budget_marker_write(0x82); // leave region 2
		run_work(4, 5);                 // 20 -> region 1 again
		perf_budget_marker_write(0x81); // leave region 1
		run_work(4, 5);                 // 20 -> unattributed
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.zone[0], 40, "an outer region collects its own cycles");
		check_eq(f.zone[1], 20, "the innermost open region wins while nested");

		perf_zone_t z;
		check(perf_budget_zone_get(0, &z), "a marker region claims a zone slot");
		check(z.is_marker && z.marker_id == 1, "tagged with the region it stands for");
		check(strcmp(z.name, "region 1") == 0, "and named after it");
	}

	// ── Unbalanced markers cost one frame, not every frame ──────────────────
	{
		configure();
		perf_budget_marker_write(0xF0);
		perf_budget_marker_write(0x05); // entered and never left
		run_work(4, 10);
		perf_budget_frame_end(0);

		run_work(4, 10); // the next frame must not still be inside region 5
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.zone[0], 0, "a region left open is closed by the frame boundary");

		// Leaving a region that was never entered is ignored rather than
		// corrupting the stack.
		perf_budget_marker_write(0x93);
		run_work(4, 5);
		perf_budget_frame_end(0);
		perf_budget_last_frame(&f);
		check_eq(f.total, 20, "an unmatched leave is ignored");
	}

	// ── Marker mode does not outlive a reset ────────────────────────────────
	{
		// Marker mode suppresses the heuristic. If a reset did not clear it, a
		// program that wrote one marker would disable automatic detection for
		// the rest of the session, with no way back.
		configure();
		perf_budget_marker_write(0xF0);
		run_work(4, 5);
		perf_budget_frame_end(0);

		perf_budget_reset();
		run_spin(0x4000, 20);
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check(f.idle > 0, "a reset gives automatic detection back after markers");

		// And a guest still driving markers re-asserts the mode immediately.
		perf_budget_reset();
		perf_budget_marker_write(0xF1); // work has ended: all of this is idle
		run_work(4, 10);
		perf_budget_frame_end(0);
		perf_budget_last_frame(&f);
		check_eq(f.idle, 40, "while the next marker write takes control again");
	}

	// ── Idle modes ──────────────────────────────────────────────────────────
	{
		configure();
		perf_budget_set_idle_mode(PERF_IDLE_NONE);
		run_spin(0x4000, 20);
		for (int i = 0; i < 5; i++)
			perf_budget_step(10, 0x3000, 0, true, 0);
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.idle, 0, "PERF_IDLE_NONE classifies nothing as waiting");

		configure();
		perf_budget_set_idle_mode(PERF_IDLE_MARKERS);
		run_spin(0x4000, 20);
		perf_budget_frame_end(0);
		perf_budget_last_frame(&f);
		check_eq(f.idle, 0, "PERF_IDLE_MARKERS ignores the spin heuristic");
	}

	// ── Overruns, against the budget ────────────────────────────────────────
	{
		configure();
		// Three frames under budget, two over.
		const uint32_t under = CYCLES_PER_FRAME / 2;
		const uint32_t over  = CYCLES_PER_FRAME + 1000;
		const uint32_t counts[5] = {under, over, under, over, under};
		for (int i = 0; i < 5; i++) {
			perf_budget_step(counts[i], (uint16_t)(0x2000 + i), 0, false, 0);
			perf_budget_frame_end(1000);
		}

		perf_window_t w;
		check(perf_budget_window(0.0f, &w), "the whole ring is a valid window");
		check_eq((uint32_t)w.frames, 5, "holding every frame recorded");
		check_eq((uint32_t)w.overruns, 2, "two of which exceeded the budget");
		check_eq(w.worst_overrun, 1000, "by 1000 cycles at worst");
		check_eq(w.work.max, over, "the worst frame is the maximum");
		check_eq(w.work.min, under, "and the best the minimum");
		check(w.max_utilization > 100.0f, "so peak utilisation is over budget");
		check(w.mean_utilization < 100.0f, "while the mean is under it");
	}

	// ── The overrun drain, which the DAP event rides on ─────────────────────
	{
		configure();
		perf_overrun_report_t r;
		check(!perf_budget_drain_overruns(&r), "a clean run reports nothing");

		perf_budget_step(CYCLES_PER_FRAME + 500, 0x2000, 0, false, 0);
		perf_budget_frame_end(0);
		perf_budget_step(CYCLES_PER_FRAME + 2500, 0x2000, 0, false, 0);
		perf_budget_frame_end(0);

		check(perf_budget_drain_overruns(&r), "overruns are reported once seen");
		check_eq(r.overruns, 2, "coalescing every frame since the last drain");
		check_eq(r.worst_overrun, 2500, "and keeping the worst of them");
		check(!perf_budget_drain_overruns(&r), "draining clears the accumulator");
	}

	// ── Percentiles ─────────────────────────────────────────────────────────
	{
		configure();
		// 100 frames of 1000..100000 cycles, ascending.
		for (int i = 1; i <= 100; i++) {
			perf_budget_step((uint32_t)i * 1000, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		perf_window_t w;
		check(perf_budget_window(0.0f, &w), "a hundred frames make a window");
		check_eq((uint32_t)w.frames, 100, "all of them retained");
		check_eq(w.work.min, 1000, "minimum is the smallest sample");
		check_eq(w.work.max, 100000, "maximum is the largest");
		check_eq(w.work.mean, 50500, "the mean is the average of 1..100 thousand");
		check_eq(w.work.p50, 50000, "nearest-rank p50 over 100 samples");
		check_eq(w.work.p95, 95000, "p95");
		check_eq(w.work.p99, 99000, "p99");
	}

	// ── The ring wraps, keeping the newest ──────────────────────────────────
	{
		configure();
		check(perf_budget_set_capacity(8), "the ring can be resized");
		check_eq((uint32_t)perf_budget_capacity(), 8, "to the size asked for");

		for (int i = 0; i < 20; i++) {
			perf_budget_step((uint32_t)(i + 1) * 100, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		check_eq((uint32_t)perf_budget_frame_count(), 8, "which then holds eight frames");
		check_eq(perf_budget_total_frames(), 20, "having seen twenty");

		perf_frame_t f;
		perf_budget_last_frame(&f);
		check_eq(f.total, 2000, "the newest frame survives the wrap");
		check_eq(f.frame, 19, "with its original ordinal");

		perf_window_t w;
		perf_budget_window(0.0f, &w);
		check_eq(w.work.min, 1300, "and the window holds only the last eight");
		check_eq(w.work.max, 2000, "up to the newest");
	}

	// ── A window narrower than the ring ─────────────────────────────────────
	{
		configure();
		for (int i = 0; i < 600; i++) { // ten seconds at ~60 Hz
			perf_budget_step(1000, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		perf_window_t w;
		check(perf_budget_window(1.0f, &w), "one second is a valid window");
		check(w.frames >= 55 && w.frames <= 65,
		      "holding about one vsync period's worth of frames");

		check(perf_budget_window(5.0f, &w), "and so is five seconds");
		check(w.frames >= 290 && w.frames <= 305, "with about five times as many");
	}

	// ── A budget spanning two frames sums the work across them ──────────────
	{
		configure();
		perf_budget_set_target_fps(30.0f);

		// Each frame does 3/4 of a frame's work: fine at 30 fps (1.5 of a
		// 2-frame budget), and would be an overrun at 60.
		const uint32_t each = CYCLES_PER_FRAME * 3 / 4;
		for (int i = 0; i < 6; i++) {
			perf_budget_step(each, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		perf_window_t w;
		perf_budget_window(0.0f, &w);
		check_eq((uint32_t)w.overruns, 0, "3/4 of a frame each is within a 30 fps budget");
		check_eq(w.budget_work.max, each * 2, "which is measured over two frames");
		check(w.max_utilization > 74.0f && w.max_utilization < 76.0f,
		      "putting utilisation at about 75%");

		// Now overshoot it.
		configure();
		perf_budget_set_target_fps(30.0f);
		for (int i = 0; i < 6; i++) {
			perf_budget_step(CYCLES_PER_FRAME + 5000, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		perf_budget_window(0.0f, &w);
		check(w.overruns > 0, "but a full frame of work each is not");
	}

	// ── A tight poll nested inside a compact routine that stores ────────────
	{
		// The case a real ROM found: a whole routine small enough to fit inside
		// PERF_SPIN_SPAN, containing a storing loop AND a store-free one. A
		// detector that anchors on PC drift rather than on the backward jump
		// treats the entire routine as one storing "loop" and reports no idle
		// at all.
		configure();
		for (int outer = 0; outer < 20; outer++) {
			// work loop at $C004: STA / DEX / BNE, 80 passes
			for (int i = 0; i < 8; i++) {
				perf_budget_step(4, 0xC004, 0, false, 0);
				perf_budget_note_store();
				perf_budget_step(2, 0xC007, 0, false, 0);
				perf_budget_step(3, 0xC008, 0, false, 0);
			}
			// idle loop at $C00C: NOP / DEX / BNE, store-free
			for (int i = 0; i < 8; i++) {
				perf_budget_step(2, 0xC00C, 0, false, 0);
				perf_budget_step(2, 0xC00D, 0, false, 0);
				perf_budget_step(3, 0xC00E, 0, false, 0);
			}
			perf_budget_step(3, 0xC010, 0, false, 0); // JMP back to the top
		}
		perf_budget_frame_end(0);

		perf_frame_t f;
		perf_budget_last_frame(&f);
		const float idle_pct = 100.0f * (float)f.idle / (float)f.total;
		check(idle_pct > 20.0f && idle_pct < 70.0f,
		      "a store-free poll nested in a compact storing routine is still found");
		check(f.idle > 0, "and its cycles are credited as waiting");
	}

	// ── A target faster than the machine scans gets a fraction of a frame ────
	{
		configure();
		// ~120 fps against a ~59.5 Hz vsync is very close to half a frame.
		perf_budget_set_target_fps(120.0f);
		check_eq((uint32_t)perf_budget_frames_per_budget(), 1,
		         "a sub-frame target still measures over one frame");
		const uint32_t half = perf_budget_budget_cycles();
		check(half > CYCLES_PER_FRAME * 45 / 100 && half < CYCLES_PER_FRAME * 55 / 100,
		      "and gets about half a frame's cycles");

		// A routine using three quarters of a frame genuinely misses a
		// half-frame budget, and this is the case where an overrun means
		// something: work cannot exceed a whole frame, so without sub-frame
		// budgets there would be nothing to overrun.
		for (int i = 0; i < 5; i++) {
			perf_budget_step(CYCLES_PER_FRAME * 3 / 4, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		perf_window_t w;
		perf_budget_window(0.0f, &w);
		check_eq((uint32_t)w.overruns, 5, "three quarters of a frame overruns a half-frame budget");
		check(w.max_utilization > 140.0f, "at around 150% utilisation");
	}

	// ── "60" means every frame, not 0.8% faster than the hardware ───────────
	{
		configure();
		// The machine scans at 59.52 Hz. Asking for 60 must not shave the
		// budget to 133,333 and put every saturated program over it.
		perf_budget_set_target_fps(60.0f);
		check_eq(perf_budget_budget_cycles(), CYCLES_PER_FRAME,
		         "a 60 fps target snaps onto the 59.52 Hz frame");
		perf_budget_set_target_fps(59.0f);
		check_eq(perf_budget_budget_cycles(), CYCLES_PER_FRAME,
		         "and so does 59");
	}

	// ── A late frame boundary is not an overrun ─────────────────────────────
	{
		configure();
		// A frame boundary is detected after the instruction that crossed it,
		// so a saturated program overshoots the nominal period by a few cycles.
		// That is where the boundary landed, not a budget failure.
		for (int i = 0; i < 10; i++) {
			perf_budget_step(CYCLES_PER_FRAME + 3, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		perf_window_t w;
		perf_budget_window(0.0f, &w);
		check_eq((uint32_t)w.overruns, 0,
		         "overshooting the frame by one instruction is not an overrun");

		// Well past the slack still is.
		configure();
		for (int i = 0; i < 10; i++) {
			perf_budget_step(CYCLES_PER_FRAME + 4000, 0x2000, 0, false, 0);
			perf_budget_frame_end(0);
		}
		perf_budget_window(0.0f, &w);
		check_eq((uint32_t)w.overruns, 10, "but a real overshoot still is");
	}

	// ── Disabled costs nothing and records nothing ──────────────────────────
	{
		configure();
		perf_budget_set_enabled(false);
		check(!perf_budget_is_enabled(), "profiling can be switched off");
		run_work(4, 100);
		perf_budget_frame_end(0);

		perf_frame_t f;
		check(!perf_budget_last_frame(&f), "and then records nothing");

		perf_window_t w;
		check(!perf_budget_window(0.0f, &w), "with no window to report");
	}

	// ── Reset forgets history but keeps the configuration ───────────────────
	{
		configure();
		perf_budget_set_target_fps(30.0f);
		run_work(4, 10);
		perf_budget_frame_end(0);
		perf_budget_reset();

		check_eq((uint32_t)perf_budget_frame_count(), 0, "reset empties the ring");
		check_eq(perf_budget_total_frames(), 0, "and the frame ordinal");
		check_eq((uint32_t)perf_budget_frames_per_budget(), 2,
		         "while leaving the target fps alone");

		perf_budget_set_enabled(false);
	}

	return x16_test_summary("perf_budget");
}
