// Commander X16 Emulator — capture mode state machine. See profile_capture.h.

#include "profile_capture.h"

#include <string.h>

cap_state_t cap_state_now = CAP_IDLE;

static cap_hooks_t  hooks;
static cap_config_t config;
static cap_end_t    end_reason;
static uint32_t     frames;
static double       started_at, ended_after;

static double
now_s(void)
{
	return hooks.now_s ? hooks.now_s() : 0.0;
}

void
cap_init(const cap_hooks_t *h)
{
	memset(&hooks, 0, sizeof hooks);
	if (h)
		hooks = *h;
	memset(&config, 0, sizeof config);
	cap_state_now = CAP_IDLE;
	end_reason    = CAP_END_NONE;
	frames        = 0;
	started_at = ended_after = 0.0;
}

static void
begin_recording(void)
{
	frames     = 0;
	end_reason = CAP_END_NONE;
	if (hooks.arm && !hooks.arm(config.recorders)) {
		// Disarm whatever did arm; nothing was recorded, so no report.
		if (hooks.disarm)
			hooks.disarm(config.recorders);
		cap_state_now = CAP_DONE;
		end_reason    = CAP_END_FAILED;
		ended_after   = 0.0;
		return;
	}
	// After arming: arming the histogram allocates and clears tens of MB, and
	// that is not part of the run being measured.
	started_at    = now_s();
	cap_state_now = CAP_RUNNING;
}

static void
finish(cap_end_t why)
{
	ended_after   = now_s() - started_at;
	end_reason    = why;
	cap_state_now = CAP_DONE;
	if (hooks.finished)
		hooks.finished(why);
	if (hooks.disarm)
		hooks.disarm(config.recorders);
}

bool
cap_start(const cap_config_t *cfg)
{
	if (!cfg || cap_state_now == CAP_ARMED || cap_state_now == CAP_RUNNING)
		return false;
	config      = *cfg;
	frames      = 0;
	ended_after = 0.0;
	end_reason  = CAP_END_NONE;
	if (config.on_marker) {
		cap_state_now = CAP_ARMED;
		return true;
	}
	begin_recording();
	return cap_state_now == CAP_RUNNING;
}

void
cap_stop(void)
{
	if (cap_state_now == CAP_RUNNING)
		finish(CAP_END_REQUESTED);
	else if (cap_state_now == CAP_ARMED)
		cap_state_now = CAP_IDLE;
}

bool
cap_marker(uint8_t value)
{
	if (value != CAP_MARKER_START && value != CAP_MARKER_STOP)
		return false;
	if (value == CAP_MARKER_START && cap_state_now == CAP_ARMED)
		begin_recording();
	else if (value == CAP_MARKER_STOP && cap_state_now == CAP_RUNNING)
		finish(CAP_END_MARKER);
	// Consumed either way: a capture marker is never a perf_budget marker.
	return true;
}

void
cap_frame_end(void)
{
	if (cap_state_now != CAP_RUNNING)
		return;
	frames++;
	if (config.frames && frames >= config.frames)
		finish(CAP_END_FRAMES);
	else if (config.seconds > 0.0 && now_s() - started_at >= config.seconds)
		finish(CAP_END_SECONDS);
}

cap_state_t
cap_state(void)
{
	return cap_state_now;
}

cap_end_t
cap_end_reason(void)
{
	return end_reason;
}

const char *
cap_end_name(cap_end_t why)
{
	switch (why) {
		case CAP_END_NONE:      return "none";
		case CAP_END_REQUESTED: return "stopped";
		case CAP_END_FRAMES:    return "frame limit";
		case CAP_END_SECONDS:   return "time limit";
		case CAP_END_MARKER:    return "guest marker";
		case CAP_END_FAILED:    return "could not arm recorders";
	}
	return "?";
}

uint32_t
cap_frames(void)
{
	return frames;
}

double
cap_elapsed_s(void)
{
	return cap_state_now == CAP_RUNNING ? now_s() - started_at : ended_after;
}

uint32_t
cap_recorders(void)
{
	return config.recorders;
}

const cap_config_t *
cap_config(void)
{
	return &config;
}
