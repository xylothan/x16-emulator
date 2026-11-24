// Commander X16 Emulator
// The keyframe ring, wired to save states and the input log.

#include "keyframes_host.h"

#include <SDL.h>
#include <stdio.h>

#include "cpu/fake6502.h"
#include "debug_core.h"
#include "glue.h"
#include "input_log.h"
#include "input_log_host.h"
#include "joystick.h"
#include "keyframes.h"
#include "savestate.h"
#include "timing.h"

#define REWIND_INTERVAL_FRAMES 30u
#define REWIND_BUDGET_BYTES    ((size_t)64 * 1024 * 1024)

bool keyframes_host_active = false;

static ss_writer writer;
static bool      writer_ready = false;
static uint32_t  generation   = 0;
static bool      has_ring     = false;

static uint64_t cost_ticks, cost_max, cost_n;

static bool rewind_on    = false;
static bool rewound      = false; // replaying from a step back, to record on at its end
static char blocked[160] = "";

typedef enum { SEEK_FIND, SEEK_LAND } seek_phase_t;

static struct {
	seek_phase_t     phase;
	bool             reverse; // to the previous breakpoint or watch hit
	uint32_t         index;
	uint64_t         now;     // where the machine stood
	uint64_t         stop;    // end of the window from keyframe `index`
	uint64_t         candidate;
	bool             candidate_is_start;
	bool             start_waiting;
	keyframes_seek_t hit; // what the candidate is; RUN while a reverse pass has none
	bool             was_waiting;
	uint16_t         pc;
	uint8_t          k;
	int              instructions;
} seek;

static bool
capture_state(const void **data, size_t *len, uint64_t *hash, char *why, size_t why_len)
{
	if (!savestate_can_save(why, why_len)) {
		return false;
	}
	if (!writer_ready) {
		ss_writer_init(&writer, false, NULL);
		writer_ready = true;
	}
	ss_writer_rewind(&writer);
	savestate_write(&writer);
	if (writer.failed) {
		snprintf(why, why_len, "out of memory");
		return false;
	}
	*data = writer.data;
	*len  = writer.len;
	*hash = savestate_hash(writer.data, writer.len);
	return true;
}

static void
on_replay_finished(input_replay_result_t result)
{
	if (input_log_seeking || !rewound) {
		return;
	}
	rewound = false;
	if (result == INPUT_REPLAY_MISMATCH || result == INPUT_REPLAY_DIVERGED) {
		printf("Rewind: the replay did not match the recording; history restarts here.\n");
		fflush(stdout);
		return;
	}
	if ((result == INPUT_REPLAY_VERIFIED || result == INPUT_REPLAY_UNVERIFIED) && input_log_resume_recording()) {
		joystick_log_host_state(true);
		keyframes_resume();
	}
}

void
keyframes_host_init(void)
{
	static const keyframes_hooks_t hooks = {capture_state};
	keyframes_init(&hooks);
	input_log_set_finish_hook(on_replay_finished);
}

void
keyframes_host_sync(void)
{
	if (keyframes_armed && !input_log_is_recording()) {
		keyframes_stop();
	}
}

bool
keyframes_host_start(uint32_t interval_frames, size_t budget_bytes, char *err, size_t err_len)
{
	keyframes_host_sync();
	if (input_log_mode != INPUT_LOG_IDLE) {
		snprintf(err, err_len, "a recording or replay is already running");
		return false;
	}
	if (!savestate_can_save(err, err_len)) {
		return false;
	}
	if (!input_log_host_start()) {
		snprintf(err, err_len, "the recording could not start");
		return false;
	}
	has_ring   = false;
	rewound    = false;
	cost_ticks = cost_max = cost_n = 0;
	uint64_t cycle;
	uint32_t index;
	input_log_position(&cycle, &index);
	if (!keyframes_start(interval_frames, budget_bytes) || !keyframes_capture(cycle, index)) {
		const keyframe_gap_t *g = keyframes_gap(0);
		snprintf(err, err_len, "no first keyframe: %s", g ? g->reason : "bad interval or budget");
		keyframes_stop();
		input_log_end("no first keyframe");
		return false;
	}
	generation            = input_log_generation();
	has_ring              = true;
	keyframes_host_active = true;
	return true;
}

void
keyframes_host_stop(void)
{
	rewind_on  = false;
	blocked[0] = '\0';
	if (keyframes_armed || rewound) {
		rewound = false;
		input_log_end("stopped");
	}
	keyframes_host_sync();
	keyframes_host_active = false;
}

void
keyframes_host_boundary(void)
{
	if (input_log_seeking || input_log_mode == INPUT_LOG_REPLAYING) {
		return;
	}
	if (input_log_is_recording()) {
		if (!keyframes_armed || !keyframes_host_usable()) {
			return;
		}
		keyframes_status_t before, after;
		keyframes_get_status(&before);
		uint64_t cycle;
		uint32_t index;
		input_log_position(&cycle, &index);
		const uint64_t t0 = SDL_GetPerformanceCounter();
		keyframes_frame(cycle, index);
		const uint64_t dt = SDL_GetPerformanceCounter() - t0;
		keyframes_get_status(&after);
		if (after.captured != before.captured) {
			cost_ticks += dt;
			cost_n++;
			if (dt > cost_max) {
				cost_max = dt;
			}
		}
		return;
	}
	keyframes_stop();
	if (!rewind_on) {
		keyframes_host_active = false;
		return;
	}
	if (keyframes_host_start(REWIND_INTERVAL_FRAMES, REWIND_BUDGET_BYTES, blocked, sizeof blocked)) {
		blocked[0] = '\0';
	}
}

bool
keyframes_host_usable(void)
{
	return has_ring && generation == input_log_generation();
}

bool
keyframes_host_replay(uint32_t index, char *err, size_t err_len)
{
	keyframes_host_sync();
	if (input_log_mode != INPUT_LOG_IDLE) {
		snprintf(err, err_len, "stop the recording or replay first");
		return false;
	}
	if (!keyframes_host_usable()) {
		snprintf(err, err_len, "no keyframes for the current recording");
		return false;
	}
	const keyframe_t *k = keyframes_get(index);
	if (!k) {
		snprintf(err, err_len, "no keyframe %u", index);
		return false;
	}
	if (!savestate_apply_own(k->data, k->size, err, err_len)) {
		return false;
	}
	if (!input_log_replay_from(k->event_index, k->cycle, k->hash)) {
		snprintf(err, err_len, "the keyframe does not match its recording");
		return false;
	}
	return true;
}

void
keyframes_host_capture_cost(double *mean_us, double *max_us)
{
	const double us_per_tick = 1e6 / (double)SDL_GetPerformanceFrequency();
	*mean_us = cost_n ? (double)cost_ticks / (double)cost_n * us_per_tick : 0;
	*max_us  = (double)cost_max * us_per_tick;
}

// ─── Rewind ──────────────────────────────────────────────────────────────────

void
keyframes_host_set_enabled(bool on)
{
	if (on == rewind_on) {
		return;
	}
	rewind_on  = on;
	blocked[0] = '\0';
	if (on) {
		keyframes_host_active = true;
		return;
	}
	if (keyframes_host_usable() && input_log_mode != INPUT_LOG_IDLE) {
		rewound = false;
		input_log_end("rewind turned off");
	}
	keyframes_stop();
	keyframes_clear();
	has_ring              = false;
	keyframes_host_active = false;
}

bool
keyframes_host_enabled(void)
{
	return rewind_on;
}

const char *
keyframes_host_blocked(void)
{
	return rewind_on && blocked[0] ? blocked : NULL;
}

// The newest keyframe before where the machine stands.
static bool
keyframe_before_now(uint32_t *out, uint64_t *now_cycle)
{
	if (!keyframes_host_usable() ||
	    !(input_log_is_recording() || (input_log_mode == INPUT_LOG_REPLAYING && rewound))) {
		return false;
	}
	uint32_t events;
	input_log_position(now_cycle, &events);
	keyframes_status_t s;
	keyframes_get_status(&s);
	for (uint32_t i = s.count; i-- > 0;) {
		if (keyframes_get(i)->cycle < *now_cycle) {
			*out = i;
			return true;
		}
	}
	return false;
}

bool
keyframes_host_can_step_back(void)
{
	uint32_t index;
	uint64_t now_cycle;
	return keyframe_before_now(&index, &now_cycle);
}

static bool
seek_restore(uint32_t index)
{
	char              err[160];
	const keyframe_t *k = keyframes_get(index);
	if (!k || !savestate_apply_own(k->data, k->size, err, sizeof err) ||
	    !input_log_replay_from(k->event_index, k->cycle, k->hash)) {
		return false;
	}
	// A seek can resume mid-pass, past the loop's own call at this position.
	input_log_replay(INPUT_SITE_TOP);
	(void)debug_wp_probe_take();
	return input_log_mode == INPUT_LOG_REPLAYING;
}

static bool
seek_find_from(uint32_t index, uint64_t stop)
{
	if (!seek_restore(index)) {
		return false;
	}
	seek.phase              = SEEK_FIND;
	seek.index              = index;
	seek.stop               = stop;
	seek.candidate          = keyframes_get(index)->cycle;
	seek.candidate_is_start = true;
	seek.start_waiting      = waiting != 0;
	seek.was_waiting        = waiting != 0;
	seek.pc                 = regs.pc;
	seek.k                  = regs.k;
	seek.instructions       = instruction_counter;
	seek.hit                = KEYFRAMES_SEEK_RUN;
	// No poll tests the position a pass starts from.
	if (seek.reverse && !waiting && debug_bp_would_stop(regs.pc, regs.k)) {
		seek.hit = KEYFRAMES_SEEK_HIT_BREAKPOINT;
	}
	return true;
}

static void
seek_finish(void)
{
	input_log_seeking = false;
	timing_set_no_sleep(false);
	debug_wp_suspend(false);
	debug_wp_probe(false);
}

static keyframes_seek_t
seek_fail(void)
{
	seek_finish();
	rewound = false;
	input_log_end("rewind failed");
	printf("Rewind: the replay did not reach its target; history restarts here.\n");
	fflush(stdout);
	return KEYFRAMES_SEEK_FAILED;
}

static bool
seek_begin(bool reverse, char *err, size_t err_len)
{
	uint32_t index;
	uint64_t now_cycle;
	if (!keyframe_before_now(&index, &now_cycle)) {
		snprintf(err, err_len, "%s", rewind_on || keyframes_host_usable() ? "no earlier history" : "rewind history is off");
		return false;
	}
	if (input_log_is_recording()) {
		keyframes_stop();
		input_log_end(reverse ? "reversed" : "stepped back");
	}
	seek.reverse      = reverse;
	seek.now          = now_cycle;
	input_log_seeking = true;
	rewound           = true;
	// Not warp: it skips part of line rendering, which is in the machine's state.
	timing_set_no_sleep(true);
	if (reverse) {
		debug_wp_probe(true);
	} else {
		debug_wp_suspend(true);
	}
	if (!seek_find_from(index, now_cycle)) {
		seek_fail();
		snprintf(err, err_len, "the keyframe could not be restored");
		return false;
	}
	return true;
}

bool
keyframes_host_step_back(char *err, size_t err_len)
{
	return seek_begin(false, err, err_len);
}

bool
keyframes_host_reverse_continue(char *err, size_t err_len)
{
	return seek_begin(true, err, err_len);
}

static keyframes_seek_t
seek_land(uint64_t target, keyframes_seek_t landed)
{
	if (!seek_restore(seek.index)) {
		return seek_fail();
	}
	seek.phase     = SEEK_LAND;
	seek.candidate = target;
	seek.hit       = landed;
	if (keyframes_get(seek.index)->cycle == target) {
		seek_finish();
		return landed;
	}
	return KEYFRAMES_SEEK_RESTART;
}

// A watched access is placed where its instruction ends, as a forward stop is,
// so one in the last instruction of a window is placed at the window's end.
static void
reverse_note(uint64_t t)
{
	const bool watched = debug_wp_probe_take();
	const bool moved   = regs.pc != seek.pc || regs.k != seek.k || instruction_counter != seek.instructions ||
	                   (seek.was_waiting && !waiting);
	seek.pc           = regs.pc;
	seek.k            = regs.k;
	seek.instructions = instruction_counter;
	seek.was_waiting  = waiting != 0;
	if (t >= seek.now) {
		return;
	}
	if (watched) {
		seek.candidate = t;
		seek.hit       = KEYFRAMES_SEEK_HIT_WATCH;
	} else if (moved && t < seek.stop && !waiting && debug_bp_would_stop(regs.pc, regs.k)) {
		seek.candidate = t;
		seek.hit       = KEYFRAMES_SEEK_HIT_BREAKPOINT;
	}
}

static keyframes_seek_t
reverse_window_done(void)
{
	if (seek.hit != KEYFRAMES_SEEK_RUN) {
		return seek_land(seek.candidate, seek.hit);
	}
	if (seek.index > 0) {
		const uint64_t start = keyframes_get(seek.index)->cycle;
		return seek_find_from(seek.index - 1, start) ? KEYFRAMES_SEEK_RESTART : seek_fail();
	}
	return seek_land(keyframes_get(0)->cycle, KEYFRAMES_SEEK_HISTORY_START);
}

keyframes_seek_t
keyframes_host_seek_poll(void)
{
	uint64_t t;
	if (input_log_mode == INPUT_LOG_REPLAYING) {
		uint32_t events;
		input_log_position(&t, &events);
	} else {
		input_log_status_t st;
		input_log_get_status(&st);
		if (seek.phase != SEEK_FIND ||
		    (st.result != INPUT_REPLAY_VERIFIED && st.result != INPUT_REPLAY_UNVERIFIED)) {
			return seek_fail();
		}
		t = st.end_cycle;
	}

	if (seek.phase == SEEK_LAND) {
		if (t < seek.candidate) {
			return KEYFRAMES_SEEK_RUN;
		}
		if (t > seek.candidate) {
			return seek_fail();
		}
		seek_finish();
		return seek.hit;
	}

	if (seek.reverse) {
		if (t > seek.stop) {
			return seek_fail();
		}
		reverse_note(t);
		return t < seek.stop ? KEYFRAMES_SEEK_RUN : reverse_window_done();
	}

	if (t < seek.stop) {
		if (regs.pc != seek.pc || regs.k != seek.k || instruction_counter != seek.instructions) {
			seek.pc                 = regs.pc;
			seek.k                  = regs.k;
			seek.instructions       = instruction_counter;
			seek.candidate          = t;
			seek.candidate_is_start = false;
		}
		return KEYFRAMES_SEEK_RUN;
	}
	if (t != seek.stop) {
		return seek_fail();
	}
	// A keyframe parked in WAI is not a position a step could have stopped at.
	if (seek.candidate_is_start && seek.start_waiting && seek.index > 0) {
		return seek_find_from(seek.index - 1, seek.stop) ? KEYFRAMES_SEEK_RESTART : seek_fail();
	}
	return seek_land(seek.candidate, KEYFRAMES_SEEK_LANDED);
}
