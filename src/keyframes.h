// Commander X16 Emulator
// A ring of whole-machine states, oldest dropped first, held to a byte budget.
// Knows nothing of the machine: a state comes from a hook. A frame where none
// could be taken is recorded as a gap.

#ifndef _KEYFRAMES_H_
#define _KEYFRAMES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYFRAMES_MAX        4096u
#define KEYFRAMES_MAX_GAPS   32u
#define KEYFRAMES_REASON_LEN 96u

typedef struct {
	uint64_t    cycle;       // on the input log's clock
	uint32_t    event_index; // input log events already applied in this state
	uint64_t    hash;
	size_t      size;
	const void *data;
} keyframe_t;

typedef struct {
	uint64_t first_cycle;
	uint64_t last_cycle;
	uint32_t frames;
	char     reason[KEYFRAMES_REASON_LEN];
} keyframe_gap_t;

typedef struct {
	// The state stays valid until the next call. On false, why says the reason.
	bool (*capture)(const void **data, size_t *len, uint64_t *hash, char *why, size_t why_len);
} keyframes_hooks_t;

typedef struct {
	bool     armed;
	uint32_t count;
	size_t   bytes;
	size_t   budget;
	uint32_t interval;
	uint64_t captured;
	uint64_t evicted;
	uint64_t refused_frames;
	uint32_t gaps;
	uint32_t gaps_dropped;
} keyframes_status_t;

extern bool keyframes_armed;

void keyframes_init(const keyframes_hooks_t *hooks);

// Empties the ring and arms it, one state every interval frames.
bool keyframes_start(uint32_t interval_frames, size_t budget_bytes);
// Disarms. What the ring holds stays.
void keyframes_stop(void);
// Arms again, keeping what the ring holds.
void keyframes_resume(void);
void keyframes_clear(void);

// One frame boundary. Takes a state when one is due, and retries each frame
// after a refusal.
void keyframes_frame(uint64_t cycle, uint32_t event_index);
bool keyframes_capture(uint64_t cycle, uint32_t event_index);

// 0 is the oldest. NULL past the newest.
const keyframe_t     *keyframes_get(uint32_t index);
const keyframe_gap_t *keyframes_gap(uint32_t index);
void                  keyframes_get_status(keyframes_status_t *out);

#ifdef __cplusplus
}
#endif

#endif
