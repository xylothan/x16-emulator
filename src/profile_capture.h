// Commander X16 Emulator — capture mode: record a stretch of running, then
// analyse it.
//
// The live panels answer "what is happening now", and they cost the emulator's
// own thread to draw. A capture answers "where did the time go" for a run you
// choose -- a level, a boss fight, an attract loop -- with the debugger quiet
// while it records, so the game runs at full speed and a human playing it gets
// real timing. The breakdown is built once the capture ends.
//
// This module is only the state machine. Which recorders exist, how they are
// armed and what is built when the capture ends all arrive as callbacks, so it
// needs no emulator state and is unit-tested on its own
// (tests/test_profile_capture.c).
//
//   IDLE ──start──▶ RUNNING ──stop / frames / seconds / marker $F9──▶ DONE
//     │                ▲
//     └─start(on_marker)─▶ ARMED ──marker $F8──┘
//
// ARMED exists so a guest marker can only START a capture the user asked for.
// A stray $F8 from a program that does not know about this must not allocate
// the histogram and start recording on its own.
//
// DONE holds until the next start; the report built at the end stays readable.
//
// THREADING: emulator thread only.
#ifndef _PROFILE_CAPTURE_H_
#define _PROFILE_CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	CAP_IDLE = 0,
	CAP_ARMED,   // waiting for the guest's start marker
	CAP_RUNNING,
	CAP_DONE,
} cap_state_t;

// Recorders a capture can run. The histogram (cycles per bank:address) is the
// one the report is built from; the others are opt-in, because each costs the
// running machine something.
#define CAP_REC_HIST   (1u << 0)
#define CAP_REC_BUDGET (1u << 1) // perf_budget: work vs. waiting per frame
#define CAP_REC_VRAM   (1u << 2) // vera_bandwidth: VRAM traffic per scanline

// $9FBC values the guest writes to start and stop a capture. Outside the
// perf_budget marker protocol, which uses $00-$3F, $80-$BF, $F0, $F1 and $FE.
#define CAP_MARKER_START 0xF8
#define CAP_MARKER_STOP  0xF9

typedef struct {
	uint32_t frames;     // stop after this many emulated frames; 0 = no limit
	double   seconds;    // stop after this much host time; 0 = no limit
	uint32_t recorders;  // CAP_REC_*
	bool     on_marker;  // wait for the guest's $F8 before recording
} cap_config_t;

// Why a capture ended.
typedef enum {
	CAP_END_NONE = 0,
	CAP_END_REQUESTED, // stop from the UI, DAP or the command line
	CAP_END_FRAMES,
	CAP_END_SECONDS,
	CAP_END_MARKER,    // the guest wrote $F9
	CAP_END_FAILED,    // a recorder could not be armed
} cap_end_t;

typedef struct {
	// Arm the recorders in `mask`. False if any could not be armed (e.g. an
	// allocation failed); the capture then ends as CAP_END_FAILED.
	bool (*arm)(uint32_t mask);
	void (*disarm)(uint32_t mask);
	// The capture just ended: build whatever report is wanted, while the
	// recorders still hold their data (disarm runs after this).
	void (*finished)(cap_end_t why);
	// Host seconds, monotonic.
	double (*now_s)(void);
} cap_hooks_t;

void cap_init(const cap_hooks_t *hooks);

// Start, or arm for the guest's start marker. Ignored (false) while one is
// already ARMED or RUNNING.
bool cap_start(const cap_config_t *cfg);

// Stop a running capture (it ends as CAP_END_REQUESTED), or cancel an armed
// one (back to IDLE, nothing recorded).
void cap_stop(void);

// A $9FBC write. Returns true when it was a capture marker ($F8/$F9), which
// the caller must then not pass on to perf_budget.
bool cap_marker(uint8_t value);

// An emulated frame finished. Checks the frame and time limits.
void cap_frame_end(void);

cap_state_t cap_state(void);
cap_end_t   cap_end_reason(void);
const char *cap_end_name(cap_end_t why);
uint32_t    cap_frames(void);     // frames recorded by the current/last capture
double      cap_elapsed_s(void);  // host time it has run / ran
uint32_t    cap_recorders(void);  // what the current/last capture recorded
const cap_config_t *cap_config(void);

// True while recording: the debugger should stay quiet.
static inline bool
cap_running(void)
{
	extern cap_state_t cap_state_now;
	return cap_state_now == CAP_RUNNING;
}

#ifdef __cplusplus
}
#endif

#endif
