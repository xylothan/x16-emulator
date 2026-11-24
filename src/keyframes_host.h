// Commander X16 Emulator
// The keyframe ring, wired to save states and the input log. A ring covers one
// input recording: it starts with it and stops when it ends.

#ifndef _KEYFRAMES_HOST_H_
#define _KEYFRAMES_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void keyframes_host_init(void);

// Starts a recording with a keyframe where it begins.
bool keyframes_host_start(uint32_t interval_frames, size_t budget_bytes, char *err, size_t err_len);
// At each frame boundary while armed.
void keyframes_host_boundary(void);
// Disarms the ring if its recording has ended.
void keyframes_host_sync(void);
// True while the ring belongs to the input log's current recording.
bool keyframes_host_usable(void);
// Restores a keyframe and replays the recording on from it.
bool keyframes_host_replay(uint32_t index, char *err, size_t err_len);
void keyframes_host_capture_cost(double *mean_us, double *max_us);

#ifdef __cplusplus
}
#endif

#endif
