// Commander X16 Emulator — scripted input: joypad and keyboard events at
// given frames, replayed exactly, like a TAS input file.
//
// One command per line (or per ';' on the command line):
//
//     <frame> <command> [arguments]        # comment
//
//   <frame>   an emulated frame number (frame 1 is the first frame the
//             machine completes), or +N: N frames after the previous command.
//             Commands on the same frame run in the order written.
//
//   joy<N> <buttons>|none|off
//             Pad N (1-4) holds exactly this set from now on: names joined
//             with '+' (up down left right a b x y start select l r).
//             `none` holds nothing; `off` hands the pad back to the physical
//             controller.
//   key <name>       press and release a key
//   keydown <name>   hold a key
//   keyup <name>     release it
//             Names are SDL's: Return, Space, Escape, Left, A, F1,
//             "Left Shift" ... (case-insensitive; the rest of the line).
//
// Parsing and playback are separate from the emulator: key names are
// resolved and events delivered through callbacks, so this needs no SDL and is
// unit-tested on its own (tests/test_input_script.c).
#ifndef _INPUT_SCRIPT_H_
#define _INPUT_SCRIPT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	IS_JOY_SET = 0, // hold `mask` (active low, as the pad reports it) on `slot`
	IS_JOY_OFF,     // release `slot` to the physical controller
	IS_KEY_DOWN,
	IS_KEY_UP,
} is_kind_t;

typedef struct {
	uint32_t  frame;
	is_kind_t kind;
	uint8_t   slot;      // pad 0-3 for the joy commands
	uint16_t  mask;      // IS_JOY_SET
	int       scancode;  // IS_KEY_*
	int       line;      // source line, for messages
} is_event_t;

// Key name -> scancode, or a negative value for an unknown name.
typedef int (*is_key_lookup_fn)(const char *name);

// Parse a whole script. On failure returns false and writes "line N: reason"
// into `err`. Events are kept in frame order (stable for equal frames). A
// `key` command becomes a down and an up on the same frame.
bool input_script_parse(const char *text, is_key_lookup_fn lookup, char *err, int errsz);

// Parse and merge into what is already loaded, so several sources (-input,
// -inputscript, -autostart) combine. Each text is its own timeline: its frame
// order is checked on its own, and its first `+N` counts from frame 0.
bool input_script_append(const char *text, is_key_lookup_fn lookup, char *err, int errsz);

void input_script_clear(void);
int  input_script_count(void);
const is_event_t *input_script_at(int i);

typedef struct {
	void (*joy_set)(int slot, uint16_t mask);
	void (*joy_off)(int slot);
	void (*key)(int scancode, bool down);
} is_hooks_t;

// Deliver every event for `frame`. Call once per completed frame, with frames
// counting up from 1; events for frames already passed are not replayed.
void input_script_frame(uint32_t frame, const is_hooks_t *hooks);

#ifdef __cplusplus
}
#endif

#endif
