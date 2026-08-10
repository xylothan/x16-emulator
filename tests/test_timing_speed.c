// Checks for the emulation-speed control in src/timing.c.
//
// The speed is an absolute target clock in kHz, not a percentage of something.
// The machine's own clock comes from -mhz and can be anything from 1 to 40, so
// a percentage says nothing on its own: "50%" is a different speed on a machine
// started with -mhz 1 than on one started with -mhz 12. That makes the
// arithmetic here worth pinning -- particularly that stepping always passes
// through the machine's own clock, so normal speed is reachable however the
// emulator was started.
//
// timing.c reaches for a handful of emulator globals and SDL; the test supplies
// its own so it links nothing else.

#include "timing.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- Stand-ins for the emulator and SDL ------------------------------------

uint8_t MHZ        = 8;
bool    warp_mode  = false;
bool    log_speed  = false;
bool    mouse_grabbed = false;
uint32_t clockticks6502 = 0;

void video_update_title(const char *t) { (void)t; }

// timing.c only uses these to pace; the tests here never call timing_update().
// Settable, so a check can advance emulated time and wall-clock time
// independently. Never moved backwards: timing.c subtracts it as uint32_t, so a
// backwards step wraps to about 4.29e9 ms rather than reading as negative.
static uint32_t fake_ticks_ms = 0;
uint32_t SDL_GetTicks(void) { return fake_ticks_ms; }
void     SDL_PumpEvents(void) {}
void     video_repaint_only(void) {}

// ---- Harness ---------------------------------------------------------------

static int failures = 0;

static void
check(bool cond, const char *what)
{
	if (!cond) {
		failures++;
		printf("FAIL: %s\n", what);
	} else {
		printf("ok  : %s\n", what);
	}
}

// Step `n` times in a direction and return where we land.
static int
step_n(int direction, int n)
{
	for (int i = 0; i < n; i++) {
		timing_step_speed(direction);
	}
	return timing_get_speed_khz();
}

int
main(void)
{
	// ── The state the emulator actually ships in ────────────────────────────
	// Checked before any setter runs, because nothing outside this test calls
	// them: unset is the only path the shipping binary ever takes, and a check
	// placed after a reset exercises a different one and leaves the fallback
	// untested.
	{
		MHZ = 8;
		check(timing_get_speed_khz() == timing_native_khz(),
		      "unset means the machine's own clock");
		check(timing_get_speed_percent() == 100, "which reads as 100%");
	}

	// ── The machine's own clock is the default ──────────────────────────────
	{
		MHZ = 8;

		timing_reset_speed();
		check(timing_native_khz() == 8000, "reports the machine's clock in kHz");
		check(timing_get_speed_khz() == 8000, "starts at the machine's own clock");
		check(timing_get_speed_percent() == 100, "which is 100%");

		MHZ = 1;

		timing_reset_speed();
		check(timing_native_khz() == 1000, "follows -mhz 1");
		check(timing_get_speed_percent() == 100, "and that is still 100%");

		MHZ = 12;

		timing_reset_speed();
		check(timing_native_khz() == 12000, "follows -mhz 12");
		check(timing_get_speed_percent() == 100, "and that is 100% too");
	}

	// ── A percentage alone would be ambiguous ───────────────────────────────
	// The same absolute speed is a different percentage on different machines,
	// which is exactly why the target is held in kHz.
	{
		MHZ = 8;
		timing_set_speed_khz(1000);
		int pct_on_8mhz = timing_get_speed_percent();

		MHZ = 1;
		timing_set_speed_khz(1000);
		int pct_on_1mhz = timing_get_speed_percent();

		check(pct_on_8mhz == 13 && pct_on_1mhz == 100,
		      "1 MHz reads as ~13% of an 8 MHz machine but 100% of a 1 MHz one");
	}

	// ── Clamping ────────────────────────────────────────────────────────────
	{
		MHZ = 8;
		timing_set_speed_khz(0);
		check(timing_get_speed_khz() == TIMING_SPEED_MIN_KHZ, "clamps zero up to the minimum");
		timing_set_speed_khz(-5000);
		check(timing_get_speed_khz() == TIMING_SPEED_MIN_KHZ, "clamps negative up to the minimum");
		timing_set_speed_khz(1 << 30);
		check(timing_get_speed_khz() == TIMING_SPEED_MAX_KHZ, "clamps absurdly high down to the maximum");
		timing_set_speed_khz(4000);
		check(timing_get_speed_khz() == 4000, "keeps a value inside the range");
	}

	// ── Stepping through the presets ────────────────────────────────────────
	{
		MHZ = 8;

		timing_reset_speed();
		int slower = step_n(-1, 1);
		check(slower < 8000, "stepping down leaves the machine's clock");
		check(step_n(1, 1) == 8000, "and stepping back up returns to it exactly");

		// Stepping down repeatedly must bottom out at the minimum, not run off
		// the end of the ladder.

		timing_reset_speed();
		check(step_n(-1, 100) == TIMING_SPEED_MIN_KHZ, "stepping down bottoms out at the minimum");
		check(step_n(1, 200) == TIMING_SPEED_MAX_KHZ, "stepping up tops out at the maximum");

		// Monotonic in both directions, with no repeats or reversals.

		timing_reset_speed();
		bool monotonic = true;
		int  prev      = timing_get_speed_khz();
		for (int i = 0; i < 40; i++) {
			timing_step_speed(-1);
			const int now = timing_get_speed_khz();
			if (now > prev)
				monotonic = false;
			prev = now;
		}
		check(monotonic, "stepping down never speeds up");

		prev = timing_get_speed_khz();
		monotonic = true;
		for (int i = 0; i < 60; i++) {
			timing_step_speed(1);
			const int now = timing_get_speed_khz();
			if (now < prev)
				monotonic = false;
			prev = now;
		}
		check(monotonic, "stepping up never slows down");
	}

	// ── The machine's own clock is always a rung ────────────────────────────
	// However the emulator was started, stepping has to be able to land back on
	// normal speed -- otherwise "back to normal" would be unreachable on a
	// machine whose clock is not one of the presets.
	{
		const uint8_t odd_clocks[] = { 1, 2, 3, 5, 7, 8, 11, 13, 17, 23, 40 };
		bool reachable = true;
		for (unsigned i = 0; i < sizeof(odd_clocks) / sizeof(odd_clocks[0]); i++) {
			MHZ = odd_clocks[i];
	
		timing_reset_speed();
			const int native = timing_native_khz();

			// Walk to the bottom, then back up, and see if we pass through it.
			step_n(-1, 60);
			bool seen = false;
			for (int s = 0; s < 60; s++) {
				timing_step_speed(1);
				if (timing_get_speed_khz() == native) {
					seen = true;
					break;
				}
			}
			if (!seen) {
				printf("      (native %d kHz unreachable from below on -mhz %u)\n",
				       native, odd_clocks[i]);
				reachable = false;
			}
		}
		check(reachable, "the machine's own clock is always reachable by stepping");
	}

	// ── Reset ───────────────────────────────────────────────────────────────
	{
		MHZ = 8;
		timing_set_speed_khz(100);
		check(timing_get_speed_khz() == 100, "runs slow when asked");

		timing_reset_speed();
		check(timing_get_speed_khz() == 8000 && timing_get_speed_percent() == 100,
		      "reset returns to the machine's own clock");
	}

	// ── The scaling itself ──────────────────────────────────────────────────
	// Everything above pins the ladder bookkeeping. This pins the arithmetic
	// that actually changes how the machine runs, which nothing else did: the
	// first version of this feature left timing_lead_us() unscaled and the
	// whole suite still passed. That value alone paces the window-drag path, so
	// an unscaled one lets a slow target run at full speed while a window is
	// being dragged, and freezes a fast one.
	//
	// timing_lead_us() is how far ahead of real time the emulation is. With
	// SDL_GetTicks() stubbed at zero, it is the emulated time accrued since the
	// clock was re-based -- so the counter is zeroed before each re-base, or the
	// delta it measures would be taken against the previous block's value.
	{
		MHZ = 8;
		timing_reset_speed();
		clockticks6502 = 0;
		timing_init();
		clockticks6502 = 8000;            // one millisecond at 8 MHz
		check(timing_lead_us() == 1000,
		      "lead is emulated microseconds at the machine's own clock");

		// A target 320x slower stretches the same emulated work over 320x the
		// real time, so the lead grows by the same factor.
		timing_set_speed_khz(25);
		clockticks6502 = 0;
		timing_init();
		clockticks6502 = 8000;
		check(timing_lead_us() == 320000, "and 320x that at 25 kHz");

		// Faster than the machine's own clock shrinks it.
		timing_set_speed_khz(16000);
		clockticks6502 = 0;
		timing_init();
		clockticks6502 = 8000;
		check(timing_lead_us() == 500, "and half that at twice the clock");

		// The scaling follows the machine's clock, not a fixed constant.
		MHZ = 1;
		timing_reset_speed();
		clockticks6502 = 0;
		timing_init();
		clockticks6502 = 1000;            // one millisecond at 1 MHz
		check(timing_lead_us() == 1000,
		      "a different machine clock still reads as real microseconds");

		MHZ = 8;
		timing_reset_speed();
		clockticks6502 = 0;
		timing_init();
	}
	// ── The wall-clock side of the lead ─────────────────────────────────────
	// The checks above pin the scaling of emulated time, but hold wall-clock
	// time at zero, so they cannot see the term that subtracts it -- nor the
	// accumulated cpu_ticks that the pending delta is added to. Both survive
	// deletion without them.
	//
	// timing_update() is deliberately never called: its slice loop re-reads the
	// clock to decide when to stop, and a stub that does not advance during a
	// sleep would spin forever. timing_update_no_sleep() skips that block.
	{
		MHZ = 8;
		timing_reset_speed();

		// Three milliseconds of real time against one of emulated work: the
		// machine is two milliseconds behind.
		fake_ticks_ms  = 0;
		clockticks6502 = 0;
		timing_init();
		fake_ticks_ms  = 3;
		clockticks6502 = 8000;
		check(timing_lead_us() == -2000, "lead goes negative when real time runs ahead");

		// Only the emulated side is scaled. At 25 kHz the same 8000 ticks are
		// worth 320 ms, against one second of real time.
		timing_set_speed_khz(25);
		fake_ticks_ms  = 0;
		clockticks6502 = 0;
		timing_init();
		fake_ticks_ms  = 1000;
		clockticks6502 = 8000;
		check(timing_lead_us() == 320000 - 1000000,
		      "and the wall-clock side is not scaled with it");

		// Ticks already folded into the running total still count. The wall
		// clock stays at zero across the update, so the catch-up re-base -- which
		// would silently rewrite the base and hide this -- cannot trigger.
		timing_reset_speed();
		fake_ticks_ms  = 0;
		clockticks6502 = 0;
		timing_init();
		clockticks6502 = 8000;
		timing_update_no_sleep();      // folds 8000 into the accumulated total
		clockticks6502 = 16000;        // another 8000 pending
		check(timing_lead_us() == 2000, "accumulated ticks count as well as pending ones");

		fake_ticks_ms  = 0;
		clockticks6502 = 0;
		timing_reset_speed();
		timing_init();
	}

	if (failures) {
		printf("\nFAILED (%d failures)\n", failures);
		return 1;
	}
	printf("\nPASSED (0 failures)\n");
	return 0;
}
