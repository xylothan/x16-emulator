// Commander X16 Emulator
// A log of everything the host feeds the machine, stamped with where in the
// machine's time it arrived, so a run can be replayed from the state it began at.
//
// Anything that changes the machine and cannot be logged ends the recording,
// so a replay stops where it would otherwise go wrong.

#ifndef _INPUT_LOG_H_
#define _INPUT_LOG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	INPUT_LOG_IDLE,
	INPUT_LOG_RECORDING,
	INPUT_LOG_REPLAYING,
} input_log_mode_t;

// Where in a pass of the main loop an event reached the machine, in the order
// they come at one clock value: the frame-end block, the per-instruction
// post-step, then the top of the next pass (which is also where a paused
// machine sits).
typedef enum {
	INPUT_SITE_FRAME,
	INPUT_SITE_POST,
	INPUT_SITE_TOP,
} input_site_t;

typedef enum {
	INPUT_EV_KBD,         // a: byte for the keyboard buffer
	INPUT_EV_MOUSE_DOWN,  // a: button
	INPUT_EV_MOUSE_UP,    // a: button
	INPUT_EV_MOUSE_MOVE,  // x, y
	INPUT_EV_MOUSE_WHEEL, // x: wheel
	INPUT_EV_MOUSE_SEND,
	INPUT_EV_JOY_SET,     // a: slot, x: active-low mask
	INPUT_EV_JOY_CLEAR,   // a: slot
	INPUT_EV_JOY_PHYS,    // a: slot, x: active-low mask, y: bound
	INPUT_EV_PASTE,       // a: character for the KERNAL keyboard queue
} input_event_kind_t;

typedef struct {
	uint64_t cycle; // since the recording began
	int32_t  x, y;
	uint8_t  kind, site, a;
} input_event_t;

typedef enum {
	INPUT_REPLAY_NONE,
	INPUT_REPLAY_RUNNING,
	INPUT_REPLAY_VERIFIED,   // reached the end with the recorded machine hash
	INPUT_REPLAY_UNVERIFIED, // reached the end, where no hash could be taken
	INPUT_REPLAY_MISMATCH,   // reached the end with another machine hash
	INPUT_REPLAY_DIVERGED,   // an event's point in time was passed
	INPUT_REPLAY_CANCELLED,
} input_replay_result_t;

typedef struct {
	void (*apply)(const input_event_t *e);
	bool (*hash)(uint64_t *out);
	void (*replay_end)(void);
} input_log_hooks_t;

typedef struct {
	input_log_mode_t      mode;
	bool                  has_recording;
	uint32_t              events;
	uint32_t              applied;
	uint64_t              cycle;     // now, while recording or replaying
	uint64_t              end_cycle;
	bool                  end_hash_valid;
	input_replay_result_t result;
	uint64_t              diverged_event_cycle;
	uint64_t              diverged_at_cycle;
	const char           *end_reason;
} input_log_status_t;

extern uint8_t input_log_mode;
extern bool    input_log_applying;
// True while a rewind runs the machine to its target: nothing leaves the host.
extern bool    input_log_seeking;

static inline bool
input_log_is_recording(void)
{
	return input_log_mode == INPUT_LOG_RECORDING;
}

// True while a replay owns the machine's inputs, for anything but the replay.
static inline bool
input_log_host_blocked(void)
{
	return input_log_mode == INPUT_LOG_REPLAYING && !input_log_applying;
}

void input_log_init(const uint32_t *clock, const input_log_hooks_t *hooks);

void input_log_set_site(input_site_t site);
void input_log_note(input_event_kind_t kind, uint8_t a, int32_t x, int32_t y);
void input_log_note_at(input_site_t site, input_event_kind_t kind, uint8_t a, int32_t x, int32_t y);
// Once a frame while active: the machine's clock is 32 bits and wraps.
void input_log_tick(void);

bool input_log_start(void);
// Ends a recording or cancels a replay. The reason must outlive the log.
void input_log_end(const char *reason);
// As above, where the machine has already changed, so no end hash can be taken.
void input_log_end_unverified(const char *reason);
// The machine must be where the recording began.
bool input_log_replay_start(void);
// Replays from a state partway through the recording, which must hash to hash:
// event_index events were already in it at cycle. Host controller state from
// before that point is applied first, since a state does not hold it.
bool input_log_replay_from(uint32_t event_index, uint64_t cycle, uint64_t hash);
void input_log_replay(input_site_t site);

// Where a recording stands, for a state taken now.
void input_log_position(uint64_t *cycle, uint32_t *event_index);
// Changes with every recording started.
uint32_t input_log_generation(void);
// After a replay reached the end of its recording: records on from there, in
// the same generation.
bool input_log_resume_recording(void);
// Called whenever a replay finishes, after the replay_end hook.
void input_log_set_finish_hook(void (*hook)(input_replay_result_t result));

void input_log_get_status(input_log_status_t *out);

#define INPUT_LOG_REPLAY(site)                         \
	do {                                               \
		if (input_log_mode == INPUT_LOG_REPLAYING) {   \
			input_log_replay(site);                    \
		}                                              \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif
