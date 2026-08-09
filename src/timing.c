#ifndef __APPLE__
#ifndef _MSC_VER
#define _XOPEN_SOURCE   600
#define _POSIX_C_SOURCE 1
#endif
#endif
#include "glue.h"
#include "video.h"
#include "timing.h"
#include "cpu/fake6502.h"
#include <SDL.h>
#include <stdio.h>
#include "compat.h"

// Largest real-time deficit (in microseconds) we let the emulator try to make
// up by running unthrottled. Anything bigger is treated as a stall — the host
// was blocked in a modal window move/resize loop, was suspended, or simply
// can't keep up — and is dropped instead of "fast-forwarded" through once the
// stall ends. 250 ms comfortably exceeds normal per-frame scheduling jitter.
#define MAX_CATCHUP_US (250 * 1000)

// Above this much surplus, the throttle sleeps in slices instead of one block,
// so the host stays responsive while the machine is being run slowly. The
// threshold is above a frame's worth of pacing at the machine's own clock, so
// normal-speed behaviour is untouched.
#define THROTTLE_SLICE_THRESHOLD_US (25 * 1000)
#define THROTTLE_SLICE_US           (12 * 1000)

static void timing_update_ex(bool may_sleep);

uint32_t frames;
uint32_t sdlTicks_base;
uint32_t last_perf_update;
uint32_t clockticks6502_old;
int64_t cpu_ticks;
int64_t last_perf_cpu_ticks;
char window_title[255];

// The target clock, or 0 for "not set", meaning run at the machine's own.
static int emu_speed_khz = 0;

int
timing_native_khz(void)
{
	return (int)MHZ * 1000;
}

int
timing_get_speed_khz(void)
{
	return emu_speed_khz > 0 ? emu_speed_khz : timing_native_khz();
}

int
timing_get_speed_percent(void)
{
	const int native = timing_native_khz();
	return native > 0
	           ? (int)(((int64_t)timing_get_speed_khz() * 100 + native / 2) / native)
	           : 100;
}

// Emulated microseconds represented by `ticks`, scaled to the target clock. The
// throttle compares this against wall-clock microseconds, so a lower target
// makes each emulated cycle take proportionally longer in real time. At the
// machine's own clock this reduces exactly to the original ticks / MHZ.
static int64_t
scaled_us(int64_t ticks)
{
	const int khz = timing_get_speed_khz();
	return khz > 0 ? ticks * 1000 / (int64_t)khz : ticks / (MHZ ? MHZ : 1);
}

static int64_t
scaled_emulated_us(void)
{
	return scaled_us(cpu_ticks);
}

void
timing_set_speed_khz(int khz)
{
	if (khz < TIMING_SPEED_MIN_KHZ)
		khz = TIMING_SPEED_MIN_KHZ;
	if (khz > TIMING_SPEED_MAX_KHZ)
		khz = TIMING_SPEED_MAX_KHZ;
	if (khz == emu_speed_khz)
		return;
	emu_speed_khz = khz;
	// Re-base, so changing the scale is not read as a huge surplus or deficit
	// and answered with a sprint or a stall.
	timing_init();
}

// Preset clocks, in kHz. Anchored on speeds real hardware is run at (1 / 8 / 12
// MHz and neighbours) with steps between, so stepping moves through meaningful
// rates rather than arbitrary percentages.
static const int speed_ladder_khz[] = {
	   25,    50,   100,   250,   500,   750,
	 1000,  1500,  2000,  3000,  4000,  6000,
	 8000, 10000, 12000, 14000, 16000, 20000, 24000,
	// Past anything the hardware runs at, but still useful for getting through
	// a slow section without the all-or-nothing of warp mode.
	32000, 40000, 48000, 64000, 80000, 100000,
};
#define SPEED_LADDER_COUNT ((int)(sizeof(speed_ladder_khz) / sizeof(speed_ladder_khz[0])))

// Build the ladder for this machine, splicing its own clock in at the right
// place if it is not already a preset, so normal speed is always a rung.
static int
build_ladder(int *out, int max)
{
	const int native   = timing_native_khz();
	int       n        = 0;
	bool      inserted = false;

	for (int i = 0; i < SPEED_LADDER_COUNT && n < max; i++) {
		const int v = speed_ladder_khz[i];
		if (!inserted && v >= native) {
			out[n++] = native;
			inserted = true;
			if (v == native)
				continue;               // it *is* a preset, do not add it twice
			if (n >= max)
				break;
		}
		out[n++] = v;
	}
	if (!inserted && n < max)
		out[n++] = native;              // native sits above every preset
	return n;
}

void
timing_step_speed(int direction)
{
	int       ladder[SPEED_LADDER_COUNT + 1];
	const int n = build_ladder(ladder, (int)(sizeof(ladder) / sizeof(ladder[0])));
	if (n <= 0)
		return;

	// Find the nearest rung, then move from there.
	const int cur   = timing_get_speed_khz();
	int       best  = 0;
	int       bestd = 1 << 30;
	for (int i = 0; i < n; i++) {
		const int d = ladder[i] > cur ? ladder[i] - cur : cur - ladder[i];
		if (d < bestd) {
			bestd = d;
			best  = i;
		}
	}

	int idx = best;
	if (ladder[best] == cur) {
		idx = best + (direction > 0 ? 1 : (direction < 0 ? -1 : 0));
	} else if (direction > 0 && ladder[best] < cur) {
		idx = best + 1;                 // snap up past the rung we are above
	} else if (direction < 0 && ladder[best] > cur) {
		idx = best - 1;                 // snap down past the rung we are below
	}
	if (idx < 0)
		idx = 0;
	if (idx >= n)
		idx = n - 1;
	timing_set_speed_khz(ladder[idx]);
}

void
timing_reset_speed(void)
{
	timing_set_speed_khz(timing_native_khz());
}

void
timing_init() {
	frames = 0;
	sdlTicks_base = SDL_GetTicks();
	last_perf_update = 0;
	last_perf_cpu_ticks = 0;
	clockticks6502_old = clockticks6502;
	cpu_ticks = 0;
}

void
timing_update()
{
	timing_update_ex(true);
}

// As timing_update(), but never sleeps. The window-drag path runs inside the
// OS modal message loop, which cannot deliver mouse input while we are in the
// window procedure; sleeping there shows up directly as a jerky drag. That
// caller paces itself with timing_lead_us() instead.
void
timing_update_no_sleep()
{
	timing_update_ex(false);
}

// Microseconds of emulated time we are ahead of the wall clock; negative means
// behind. Lets a caller pace itself without blocking.
int64_t
timing_lead_us()
{
	int64_t ticks = cpu_ticks + (int64_t)(clockticks6502 - clockticks6502_old);
	uint32_t sdlTicks = SDL_GetTicks() - sdlTicks_base;
	// Scaled the same way the throttle is, or the two would disagree by exactly
	// native/target: the drag path is paced by this alone, so an unscaled lead
	// would let a slow target run at full speed while dragging, and freeze the
	// machine outright for a fast one.
	return scaled_us(ticks) - sdlTicks * 1000LL;
}

static void
timing_update_ex(bool may_sleep)
{
	frames++;
	cpu_ticks += clockticks6502 - clockticks6502_old;
	clockticks6502_old = clockticks6502;
	uint32_t sdlTicks = SDL_GetTicks() - sdlTicks_base;
	int64_t diff_time = scaled_emulated_us() - sdlTicks * 1000LL;

	// If we've fallen far behind real time, don't sprint to catch up (that
	// burst is the "fast-forward" artifact seen after dragging/resizing the
	// window, resuming from suspend, or on a host that can't keep up). Re-base
	// the wall-clock reference to now so the accumulated deficit is discarded
	// and emulation simply resumes at real speed.
	if (!warp_mode && diff_time < -(int64_t)MAX_CATCHUP_US) {
		uint32_t now = SDL_GetTicks();
		uint32_t new_base = now - (uint32_t)(scaled_emulated_us() / 1000);
		last_perf_update -= new_base - sdlTicks_base; // keep perf timer aligned
		sdlTicks_base = new_base;
		sdlTicks = now - sdlTicks_base;
		diff_time = scaled_emulated_us() - sdlTicks * 1000LL;
	}

	if (may_sleep && !warp_mode && diff_time > 0) {
		// A slower target multiplies the per-frame surplus by native/target.
		// At the machine's own clock that surplus is at most one frame, but at
		// the slowest setting it is seconds, and sleeping it in a single call
		// leaves the window unrepainted and unable to answer the OS -- which
		// marks it "not responding" -- for the whole of it. So break a long
		// wait into slices and let the host breathe between them.
		//
		// SDL_PumpEvents() dispatches the OS message queue without dequeuing
		// anything, and video_present_no_input() repaints without touching the
		// event queue, so input still arrives intact at the next real poll.
		// The threshold is above one frame's worth, so pacing at the machine's
		// own clock takes the single-sleep path exactly as before.
		if (diff_time > THROTTLE_SLICE_THRESHOLD_US) {
			while (diff_time > 0) {
				const int64_t chunk = diff_time > THROTTLE_SLICE_US
				                          ? THROTTLE_SLICE_US
				                          : diff_time;
				usleep(chunk);
				diff_time -= chunk;
				if (diff_time > 0) {
					SDL_PumpEvents();
					video_present_no_input();
				}
			}
		} else {
			if (diff_time >= 1000000) {
				sleep(diff_time / 1000000);
				diff_time %= 1000000;
			}
			usleep(diff_time);
		}
	}

	if (sdlTicks - last_perf_update > 5000) {
		uint32_t perf = (cpu_ticks - last_perf_cpu_ticks) / (MHZ * 50000);

		if (perf < 100 || warp_mode) {
			sprintf(window_title, WINDOW_TITLE " (%d%%)%s", perf, mouse_grabbed ? MOUSE_GRAB_MSG : "");
		} else {
			sprintf(window_title, WINDOW_TITLE "%s", mouse_grabbed ? MOUSE_GRAB_MSG : "");
		}

		video_update_title(window_title);

		last_perf_cpu_ticks = cpu_ticks;
		last_perf_update = sdlTicks;
	}

	if (log_speed) {
		float frames_behind = -((float)diff_time * 6e-5);
		int load = (int)((1 + frames_behind) * 100);
		printf("Load: %d%%\n", load > 100 ? 100 : load);

		if ((int)frames_behind > 0) {
			printf("Rendering is behind %d frames.\n", -(int)frames_behind);
		} else {
		}
	}
}

