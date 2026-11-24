// Commander X16 Emulator
// The keyframe ring, wired to save states and the input log.

#include "keyframes_host.h"

#include <SDL.h>
#include <stdio.h>

#include "input_log.h"
#include "input_log_host.h"
#include "keyframes.h"
#include "savestate.h"

static ss_writer writer;
static bool      writer_ready = false;
static uint32_t  generation   = 0;
static bool      has_ring     = false;

static uint64_t cost_ticks, cost_max, cost_n;

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

void
keyframes_host_init(void)
{
	static const keyframes_hooks_t hooks = {capture_state};
	keyframes_init(&hooks);
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
	generation = input_log_generation();
	has_ring   = true;
	return true;
}

void
keyframes_host_boundary(void)
{
	if (!input_log_is_recording()) {
		keyframes_stop();
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
	if (!savestate_apply(k->data, k->size, false, err, err_len)) {
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
