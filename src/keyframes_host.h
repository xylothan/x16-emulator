// Commander X16 Emulator
// The keyframe ring, wired to save states and the input log. A ring covers one
// input recording. With rewind enabled, a new recording starts at the next frame
// boundary whenever the last one has ended.

#ifndef _KEYFRAMES_HOST_H_
#define _KEYFRAMES_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// True while each frame boundary has work to do.
extern bool keyframes_host_active;

void keyframes_host_init(void);

// Starts a recording with a keyframe where it begins.
bool keyframes_host_start(uint32_t interval_frames, size_t budget_bytes, char *err, size_t err_len);
// Ends the recording and any restarts. What the ring holds stays.
void keyframes_host_stop(void);
// At each frame boundary while active.
void keyframes_host_boundary(void);
// Disarms the ring if its recording has ended.
void keyframes_host_sync(void);
// True while the ring belongs to the input log's current recording.
bool keyframes_host_usable(void);
// Restores a keyframe and replays the recording on from it.
bool keyframes_host_replay(uint32_t index, char *err, size_t err_len);
void keyframes_host_capture_cost(double *mean_us, double *max_us);

// Rewind history on or off. Off discards it.
void        keyframes_host_set_enabled(bool on);
bool        keyframes_host_enabled(void);
// Why no history is being kept, or NULL.
const char *keyframes_host_blocked(void);

// True when a paused machine has an earlier position to go to.
bool keyframes_host_can_step_back(void);
// Begins a seek to the previous instruction. The machine must be paused.
bool keyframes_host_step_back(char *err, size_t err_len);
// Begins a seek to the latest earlier position where a breakpoint or watchpoint
// would have stopped the machine, or else the oldest one held. Conditions apply;
// hit and ignore counts do not, and logpoints do not stop.
bool keyframes_host_reverse_continue(char *err, size_t err_len);

typedef enum {
	KEYFRAMES_SEEK_RUN,            // execute on
	KEYFRAMES_SEEK_RESTART,        // the machine was restored: back to the loop's top
	KEYFRAMES_SEEK_LANDED,         // stop here
	KEYFRAMES_SEEK_HIT_BREAKPOINT, // stop here, on a breakpoint
	KEYFRAMES_SEEK_HIT_WATCH,      // stop here, after a watched access
	KEYFRAMES_SEEK_HISTORY_START,  // stop here, at the oldest position held
	KEYFRAMES_SEEK_FAILED,         // stop here; the position is not the target
} keyframes_seek_t;

// Before each instruction during a seek.
keyframes_seek_t keyframes_host_seek_poll(void);

#ifdef __cplusplus
}
#endif

#endif
