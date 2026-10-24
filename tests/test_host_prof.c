// Checks for host-side time accounting (src/host_prof.c).
//
// The module's promise is that sections are exclusive and add up to the
// frame's wall time, with nesting pausing the outer section. A fake clock
// advanced by hand makes every expected number exact.

#include "host_prof.h"
#include "support/harness.h"

#include <stdio.h>

static uint64_t fake_now;

static uint64_t
fake_clock(void)
{
	return fake_now;
}

// The fake clock runs at 1 MHz, so ticks are microseconds.
static void
advance(uint64_t us)
{
	fake_now += us;
}

int
main(void)
{
	// ── Time outside every section is emulation ─────────────────────────────
	{
		fake_now = 0;
		host_prof_init(fake_clock, 1000000);
		advance(1000);
		host_prof_frame_end(8000);

		uint32_t us[HP_SECTIONS], wall;
		host_prof_last_frame(us, &wall);
		check_eq(us[HP_EMU], 1000, "unclaimed time is emu");
		check_eq(wall, 1000, "wall is the whole frame");
	}

	// ── Nesting is exclusive and sums to wall time ──────────────────────────
	{
		fake_now = 0;
		host_prof_init(fake_clock, 1000000);
		advance(500);                    // emu
		host_prof_begin(HP_SLEEP);
		advance(200);                    // sleep
		host_prof_begin(HP_PRESENT);
		advance(30);                     // present
		host_prof_begin(HP_UI_BUILD);
		advance(40);                     // ui_build
		host_prof_begin(HP_UI_DRAW);
		advance(10);                     // ui_draw
		host_prof_end(HP_UI_DRAW);
		advance(5);                      // ui_build again
		host_prof_end(HP_UI_BUILD);
		advance(3);                      // present again
		host_prof_end(HP_PRESENT);
		advance(100);                    // sleep again
		host_prof_end(HP_SLEEP);
		advance(12);                     // emu again
		host_prof_frame_end(134400);

		uint32_t us[HP_SECTIONS], wall;
		host_prof_last_frame(us, &wall);
		check_eq(us[HP_EMU], 512, "emu outside sections");
		check_eq(us[HP_SLEEP], 300, "sleep excludes what ran inside it");
		check_eq(us[HP_PRESENT], 33, "present excludes the UI inside it");
		check_eq(us[HP_UI_BUILD], 45, "ui_build excludes ui_draw");
		check_eq(us[HP_UI_DRAW], 10, "ui_draw");
		check_eq(wall, 900, "sections sum to wall");

		host_prof_summary_t s;
		host_prof_summary(&s);
		check_eq(s.busy_p50_us, 600, "busy is wall minus sleep");
		check_eq((uint32_t)(s.emu_mhz * 1000 + 0.5), 149333,
		         "emulated MHz from cycles over wall time");
	}

	// ── A section left open across a frame boundary keeps charging ─────────
	{
		fake_now = 0;
		host_prof_init(fake_clock, 1000000);
		host_prof_begin(HP_SLEEP);
		advance(100);
		host_prof_frame_end(0);
		advance(50);
		host_prof_end(HP_SLEEP);
		host_prof_frame_end(0);

		uint32_t us[HP_SECTIONS], wall;
		host_prof_last_frame(us, &wall);
		check_eq(us[HP_SLEEP], 50, "second frame gets the rest of the sleep");
		check_eq(us[HP_EMU], 0, "and no emu");
	}

	// ── Unbalanced ends are ignored, not underflowed ────────────────────────
	{
		fake_now = 0;
		host_prof_init(fake_clock, 1000000);
		host_prof_end(HP_PRESENT);
		advance(10);
		host_prof_frame_end(0);
		uint32_t us[HP_SECTIONS], wall;
		host_prof_last_frame(us, &wall);
		check_eq(us[HP_EMU], 10, "a stray end leaves time with emu");
	}

	// ── Panels accumulate per name; reset clears totals ─────────────────────
	{
		fake_now = 0;
		host_prof_init(fake_clock, 1000000);
		static const char io[] = "I/O";
		host_prof_panel(io, 300);
		host_prof_panel(io, 100);
		host_prof_panel("Performance", 50);
		advance(1000);
		host_prof_frame_end(0);

		check_eq((uint32_t)host_prof_panel_count(), 2, "two panels");
		host_prof_panel_t p;
		check(host_prof_panel_get(0, &p), "first panel exists");
		check_eq(p.calls, 2, "I/O rendered twice");
		check_eq((uint32_t)(p.avg_us + 0.5), 200, "mean render");
		check_eq(p.max_us, 300, "worst render");
		check_eq((uint32_t)(p.ms_per_s + 0.5), 400, "share of wall time");

		host_prof_reset();
		check(host_prof_panel_get(0, &p), "panels survive a reset");
		check_eq(p.calls, 0, "but their totals do not");

		host_prof_summary_t s;
		host_prof_summary(&s);
		check_eq(s.frames, 0, "reset clears frames");
	}

	// ── Spikes: worst frames survive the ring, and are attributed ───────────
	{
		fake_now = 0;
		host_prof_init(fake_clock, 1000000);
		host_prof_set_spike_us(16800);

		// One slow frame dominated by present, then far more ordinary frames
		// than the ring holds.
		advance(3000);                   // emu
		host_prof_begin(HP_PRESENT);
		advance(20000);                  // a 20 ms present
		host_prof_end(HP_PRESENT);
		host_prof_frame_end(134400);
		for (int i = 0; i < HP_RING_FRAMES + 50; i++) {
			advance(3000);
			host_prof_begin(HP_SLEEP);
			advance(13000);              // sleep is not busy
			host_prof_end(HP_SLEEP);
			host_prof_frame_end(134400);
		}
		// A second, smaller spike dominated by emulation.
		advance(18000);
		host_prof_frame_end(134400);

		host_prof_frame_t w[HP_WORST_FRAMES];
		const int n = host_prof_worst(w, HP_WORST_FRAMES);
		check_eq((uint32_t)n, HP_WORST_FRAMES, "worst list fills");
		check_eq(w[0].frame, 0, "the first spike is still the worst, long after the ring wrapped");
		check_eq(w[0].busy_us, 23000, "with its busy time");
		check_eq(w[0].us[HP_PRESENT], 20000, "and its section breakdown");
		check_eq((uint32_t)host_prof_dominant(&w[0]), HP_PRESENT, "dominated by present");
		check_eq(w[1].busy_us, 18000, "second worst");
		check_eq((uint32_t)host_prof_dominant(&w[1]), HP_EMU, "dominated by emu");
		check(w[2].busy_us == 3000, "then ordinary frames");

		check_eq(host_prof_spikes(), 2, "two frames over the period");
		check_eq(host_prof_spikes_by(HP_PRESENT), 1, "one blamed on present");
		check_eq(host_prof_spikes_by(HP_EMU), 1, "one on emu");
		check_eq(host_prof_spikes_by(HP_SLEEP), 0, "sleep is never blamed");

		host_prof_frame_t r;
		check(host_prof_recent(0, &r), "recent frame 0 exists");
		check_eq(r.busy_us, 18000, "recent 0 is the newest");
		check(host_prof_recent(1, &r) && r.busy_us == 3000 && r.us[HP_SLEEP] == 13000,
		      "recent 1 is the frame before it");
		check(!host_prof_recent(HP_RING_FRAMES, &r), "nothing past the ring");

		host_prof_reset();
		check_eq((uint32_t)host_prof_worst(w, HP_WORST_FRAMES), 0, "reset clears the worst list");
		check_eq(host_prof_spikes(), 0, "and the spike count");
		check_eq(host_prof_spike_us(), 16800, "but keeps the threshold");
	}

	return x16_test_summary("host_prof");
}
