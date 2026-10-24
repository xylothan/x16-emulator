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

	return x16_test_summary("host_prof");
}
