// Commander X16 Emulator — save-state slots for the player: ten numbered
// states per program, saved and loaded from the keyboard or the State menu.
//
// A slot is a file, <program>-<N>.x16s (export_state_name), in the save-state
// folder from the debugger's Settings window (EXPORT_STATE; the working
// directory without -imgui). One slot is selected; the host key (hostkey.h)
// with F3 saves to it and with F1 loads from it, deliberately keys apart, and
// with 0..9 selects.
//
// A state is taken at a frame boundary, so a request made while the machine
// runs is carried out at the next one: the same point an x16/step over DAP
// stops at, so slot files and DAP state files are interchangeable. A paused
// machine loads at once, and saves at once if it is paused on a boundary.
#ifndef STATE_SLOTS_H
#define STATE_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATE_SLOTS 10

// The program slot files are named after (-prg); NULL or "" names them x16emu.
void state_slots_set_program(const char *prg_path);

int  state_slots_selected(void);
// Select slot 0..9, and say so in the title bar. Quietly: no message.
void state_slots_select(int slot);
void state_slots_select_quietly(int slot);

// The file for slot 0..9 into `out`, making the folder if it is missing.
bool state_slots_path(int slot, char *out, size_t out_size);
// Whether slot 0..9 has a file, and when it was written.
bool state_slots_exists(int slot, time_t *written);

// Save to or load from slot 0..9, or the selected one for -1. Carried out at
// the next frame boundary, or at once while paused; the outcome is reported
// through state_slots_message().
void state_slots_request_save(int slot);
void state_slots_request_load(int slot);

// The last outcome ("Saved slot 3", or why not), and its age in ms; NULL
// before the first.
const char *state_slots_message(unsigned *age_ms);

// A key pressed with the host key (or, with -ctrlkeys, Ctrl; Cmd in the
// emulator window on macOS): F1 loads, F3 saves, 0..9 selects. True if it was one of
// these, which the caller then keeps from the machine.
bool state_slots_hotkey(SDL_Keycode sym);

// The main loop's hooks. state_slots_frame_end() goes beside
// debug_server_frame_end(); the check at the top of the instruction loop,
// before the debugger's poll, is where a request is carried out.
extern bool state_slots_boundary_pending;
void state_slots_frame_end(void);
void state_slots_at_boundary(void);
#define STATE_SLOTS_POLL()                    \
	do {                                      \
		if (state_slots_boundary_pending)     \
			state_slots_at_boundary();        \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif
