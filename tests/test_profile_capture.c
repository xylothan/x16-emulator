// Checks for the capture-mode state machine (src/profile_capture.c).
//
// Recorders and the clock are injected, so every transition is driven by hand
// and the hooks record what the machine asked for, in order.

#include "profile_capture.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

static double   t_now;
static int      arms, disarms, finishes;
static uint32_t armed_mask;
static bool     arm_ok = true;
static cap_end_t last_end;
static char     order[32];
static int      norder;

static void note(char c) { if (norder < 31) { order[norder++] = c; order[norder] = 0; } }

static bool   h_arm(uint32_t m)       { arms++; armed_mask = m; note('a'); return arm_ok; }
static void   h_disarm(uint32_t m)    { (void)m; disarms++; note('d'); }
static void   h_finished(cap_end_t w) { finishes++; last_end = w; note('f'); }
static double h_now(void)             { return t_now; }

static void
reset(void)
{
	t_now = 0.0;
	arms = disarms = finishes = 0;
	armed_mask = 0;
	arm_ok     = true;
	last_end   = CAP_END_NONE;
	norder     = 0;
	order[0]   = 0;
	const cap_hooks_t h = {h_arm, h_disarm, h_finished, h_now};
	cap_init(&h);
}

int
main(void)
{
	// ── A frame-limited capture ─────────────────────────────────────────────
	{
		reset();
		cap_config_t c = {0};
		c.frames    = 3;
		c.recorders = CAP_REC_HIST | CAP_REC_BUDGET;
		check(cap_start(&c), "start");
		check_eq(cap_state(), CAP_RUNNING, "running at once");
		check(cap_running(), "cap_running agrees");
		check_eq(armed_mask, CAP_REC_HIST | CAP_REC_BUDGET, "armed what was asked");
		check(!cap_start(&c), "a second start is refused while running");

		cap_frame_end();
		cap_frame_end();
		check_eq(cap_state(), CAP_RUNNING, "still running after 2 of 3");
		cap_frame_end();
		check_eq(cap_state(), CAP_DONE, "done after 3");
		check_eq(cap_end_reason(), CAP_END_FRAMES, "ended by the frame limit");
		check_eq(cap_frames(), 3, "counted 3 frames");
		check(strcmp(order, "afd") == 0, "arm, then report, then disarm");
		cap_frame_end();
		check_eq(cap_frames(), 3, "frames after the end are not counted");
	}

	// ── Time limit, elapsed measured from after arming ──────────────────────
	{
		reset();
		cap_config_t c = {0};
		c.seconds   = 2.0;
		c.recorders = CAP_REC_HIST;
		t_now = 10.0;
		cap_start(&c);
		t_now = 11.0;
		cap_frame_end();
		check_eq(cap_state(), CAP_RUNNING, "1 s in, still running");
		check_eq((uint32_t)(cap_elapsed_s() * 1000 + 0.5), 1000, "elapsed while running");
		t_now = 12.5;
		cap_frame_end();
		check_eq(cap_end_reason(), CAP_END_SECONDS, "ended by the time limit");
		check_eq((uint32_t)(cap_elapsed_s() * 1000 + 0.5), 2500, "elapsed kept after the end");
	}

	// ── Requested stop, and restart after DONE ──────────────────────────────
	{
		reset();
		cap_config_t c = {0};
		c.recorders = CAP_REC_HIST;
		cap_start(&c);
		cap_frame_end();
		cap_stop();
		check_eq(cap_end_reason(), CAP_END_REQUESTED, "stop ends it as requested");
		check_eq(finishes, 1, "the report is built once");
		cap_stop();
		check_eq(finishes, 1, "stopping a finished capture does nothing");
		check(cap_start(&c), "a new capture can start after one is done");
		check_eq(cap_frames(), 0, "and starts from zero");
	}

	// ── Guest markers only work once armed ──────────────────────────────────
	{
		reset();
		check(cap_marker(CAP_MARKER_START), "$F8 is consumed even when idle");
		check_eq(cap_state(), CAP_IDLE, "but a stray start marker starts nothing");
		check_eq(arms, 0, "and arms nothing");
		check(!cap_marker(0xF0), "other marker values pass through to perf_budget");

		cap_config_t c = {0};
		c.on_marker = true;
		c.recorders = CAP_REC_HIST;
		cap_start(&c);
		check_eq(cap_state(), CAP_ARMED, "on_marker waits");
		check_eq(arms, 0, "without arming recorders yet");
		cap_frame_end();
		check_eq(cap_frames(), 0, "frames before the marker are not counted");
		cap_marker(CAP_MARKER_START);
		check_eq(cap_state(), CAP_RUNNING, "$F8 starts it");
		cap_frame_end();
		cap_marker(CAP_MARKER_STOP);
		check_eq(cap_end_reason(), CAP_END_MARKER, "$F9 ends it");
		check_eq(cap_frames(), 1, "one frame between the markers");
	}

	// ── Cancelling an armed capture ─────────────────────────────────────────
	{
		reset();
		cap_config_t c = {0};
		c.on_marker = true;
		cap_start(&c);
		cap_stop();
		check_eq(cap_state(), CAP_IDLE, "stop cancels an armed capture");
		check_eq(finishes, 0, "with no report");
	}

	// ── A recorder that cannot be armed ─────────────────────────────────────
	{
		reset();
		arm_ok = false;
		cap_config_t c = {0};
		c.recorders = CAP_REC_HIST;
		check(!cap_start(&c), "start reports the failure");
		check_eq(cap_state(), CAP_DONE, "ends at once");
		check_eq(cap_end_reason(), CAP_END_FAILED, "as failed");
		check_eq(finishes, 0, "no report from nothing");
		check_eq(disarms, 1, "and whatever did arm is released");
	}

	return x16_test_summary("profile_capture");
}
