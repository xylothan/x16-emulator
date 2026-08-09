#ifndef __APPLE__
#ifndef _MSC_VER
#define _XOPEN_SOURCE   600
#define _POSIX_C_SOURCE 1
#endif
#endif
#include "glue.h"
#include "video.h"
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

static void timing_update_ex(bool may_sleep);

uint32_t frames;
uint32_t sdlTicks_base;
uint32_t last_perf_update;
uint32_t clockticks6502_old;
int64_t cpu_ticks;
int64_t last_perf_cpu_ticks;
char window_title[255];

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
	return ticks / MHZ - sdlTicks * 1000LL;
}

static void
timing_update_ex(bool may_sleep)
{
	frames++;
	cpu_ticks += clockticks6502 - clockticks6502_old;
	clockticks6502_old = clockticks6502;
	uint32_t sdlTicks = SDL_GetTicks() - sdlTicks_base;
	int64_t diff_time = cpu_ticks / MHZ - sdlTicks * 1000LL;

	// If we've fallen far behind real time, don't sprint to catch up (that
	// burst is the "fast-forward" artifact seen after dragging/resizing the
	// window, resuming from suspend, or on a host that can't keep up). Re-base
	// the wall-clock reference to now so the accumulated deficit is discarded
	// and emulation simply resumes at real speed.
	if (!warp_mode && diff_time < -(int64_t)MAX_CATCHUP_US) {
		uint32_t now = SDL_GetTicks();
		uint32_t new_base = now - (uint32_t)(cpu_ticks / (MHZ * 1000));
		last_perf_update -= new_base - sdlTicks_base; // keep perf timer aligned
		sdlTicks_base = new_base;
		sdlTicks = now - sdlTicks_base;
		diff_time = cpu_ticks / MHZ - sdlTicks * 1000LL;
	}

	if (may_sleep && !warp_mode && diff_time > 0) {
		if (diff_time >= 1000000) {
			sleep(diff_time / 1000000);
			diff_time %= 1000000;
		}
		usleep(diff_time);
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

