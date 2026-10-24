// Commander X16 Emulator — host-side time accounting.
//
// perf_budget.c and perf_hist.c count GUEST cycles. Those are immune to how
// slow the host is: the emulator is cycle-stepped, so a slow debugger frame
// delays the host but never adds a 6502 cycle. What a slow debugger does hurt
// is HOST time. The UI renders on the same thread that steps the CPU, so every
// millisecond it takes is a millisecond the emulation does not get, and once
// the frame's slack is gone the machine drops below real speed.
//
// This module says where the host's time goes, per emulated frame:
//
//   emu       stepping the CPU and devices. Not timed directly -- it is
//             whatever no section below claimed, which keeps the per-
//             instruction loop free of clock reads.
//   events    video_update() minus what it calls below: the SDL event pump,
//             input, hotkeys.
//   present   uploading and presenting the emulator's own framebuffer.
//   ui_build  the debugger's ImGui frame: panels and widgets.
//   ui_draw   turning that frame into GPU work and presenting it.
//   sleep     the throttle sleeping off surplus time.
//   dap       the debug adapter socket.
//   hostfs    KERNAL file calls the emulator intercepts and serves from the
//             host filesystem (-fsroot): real file I/O on this thread.
//   dbginfo   reading the debug info for a program the guest just loaded.
//
// Sections NEST and are EXCLUSIVE: a section begun inside another pauses the
// outer one, so the throttle's repaint (present + UI inside the sleep loop) is
// charged to present and UI, not to sleep. The sum of all sections is exactly
// the frame's wall time.
//
// Panels are timed separately, inside ui_build, so the readout can name the
// expensive one.
//
// The clock is supplied by the caller, so this file needs no SDL and is
// unit-tested on its own (tests/test_host_prof.c).
//
// THREADING: emulator thread only, like perf_budget.c.
#ifndef _HOST_PROF_H_
#define _HOST_PROF_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	HP_EMU = 0, // implicit: time outside every other section
	HP_EVENTS,
	HP_PRESENT,
	HP_UI_BUILD,
	HP_UI_DRAW,
	HP_SLEEP,
	HP_DAP,
	HP_HOSTFS,
	HP_DBGINFO,
	HP_SECTIONS
} hp_section_t;

#define HP_MAX_PANELS   32
#define HP_RING_FRAMES  1024   // recent frames, for percentiles and spike detail
#define HP_STACK_DEPTH  8

typedef uint64_t (*host_prof_clock_fn)(void);

// `freq` is clock ticks per second. Resets everything.
void host_prof_init(host_prof_clock_fn now, uint64_t freq);

uint64_t host_prof_now(void);

void host_prof_begin(hp_section_t s);
void host_prof_end(hp_section_t s);

// One render of a debugger panel took `ticks`.
void host_prof_panel(const char *name, uint64_t ticks);

// The debugger UI produced a frame (it is paced by wall clock, not by the
// emulated frame rate, so this counts separately).
void host_prof_ui_frame(void);

// An emulated frame finished; `cycles` is how many guest cycles it took.
void host_prof_frame_end(uint32_t cycles);

// Forget all totals and history (e.g. after a warm-up).
void host_prof_reset(void);

const char *host_prof_section_name(hp_section_t s);

typedef struct {
	uint32_t frames;           // emulated frames since reset
	uint32_t ui_frames;        // debugger UI frames since reset
	double   wall_s;           // host seconds since reset
	double   emu_mhz;          // guest cycles per host microsecond
	double   fps;              // emulated frames per host second
	double   ui_hz;            // debugger frames per host second
	// Share of wall time, in milliseconds per host second. Sums to 1000.
	// The right view under warp, where frames and UI frames run at
	// different rates.
	double   ms_per_s[HP_SECTIONS];
	// Mean per emulated frame, in microseconds. The right view at native
	// speed, where one frame is one vsync.
	double   us_per_frame[HP_SECTIONS];
	// Busy time per frame (wall minus sleep) over the recent ring: what
	// decides whether the machine keeps up.
	uint32_t busy_p50_us, busy_p95_us, busy_max_us;
	int      busy_samples;
} host_prof_summary_t;

void host_prof_summary(host_prof_summary_t *out);

// The last completed frame, per section, in microseconds.
void host_prof_last_frame(uint32_t out_us[HP_SECTIONS], uint32_t *wall_us);

typedef struct {
	const char *name;
	uint32_t    calls;     // renders since reset
	double      avg_us;    // mean per render since reset
	double      recent_us; // exponentially smoothed, for a live readout
	uint32_t    max_us;
	double      ms_per_s;  // share of wall time since reset
} host_prof_panel_t;

int  host_prof_panel_count(void);
bool host_prof_panel_get(int i, host_prof_panel_t *out);

// ─── Spikes ─────────────────────────────────────────────────────────────────
// A mean says nothing about the one frame that missed its vsync. So every
// frame is kept in a ring with its full section breakdown, and the worst
// HP_WORST_FRAMES since reset are kept apart from the ring, so a spike is not
// lost just because a thousand ordinary frames came after it.
#define HP_WORST_FRAMES 8

typedef struct {
	uint32_t frame;              // ordinal since reset, from 0
	uint32_t wall_us;            // the whole frame, sleep included
	uint32_t busy_us;            // wall minus sleep: what has to fit the period
	uint32_t cycles;             // guest cycles the frame covered
	uint32_t ui_frames;          // debugger UI frames drawn during it
	uint32_t us[HP_SECTIONS];    // per section; sums to wall_us
} host_prof_frame_t;

// One frame from the ring, 0 = the most recent. False when out of range.
bool host_prof_recent(int newest_index, host_prof_frame_t *out);

// The worst frames since reset by busy time, worst first. Returns how many.
int  host_prof_worst(host_prof_frame_t *out, int max);

// The section that took the most time in a frame, sleep excluded: the first
// place to look when that frame was slow.
hp_section_t host_prof_dominant(const host_prof_frame_t *f);

// A frame whose busy time exceeds `us` is a spike: it could not have kept real
// speed. The emulator sets this to the machine's frame period; 0 turns
// counting off. Counts are since reset.
void     host_prof_set_spike_us(uint32_t us);
uint32_t host_prof_spike_us(void);
uint32_t host_prof_spikes(void);
// How many spikes each section dominated (see host_prof_dominant()).
uint32_t host_prof_spikes_by(hp_section_t s);

#ifdef __cplusplus
}
#endif

#endif
