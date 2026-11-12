// Commander X16 Emulator — save-state slots for the player. See state_slots.h.

#include "state_slots.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "debug_server.h"
#include "debugger.h"
#include "export_paths.h"
#include "savestate.h"
#include "timing.h"

#define NOTE_MS 4000 // how long an outcome stays in the title bar

typedef enum { REQ_NONE, REQ_SAVE, REQ_LOAD } request_t;

static char      program[1024] = "";
static int       selected      = 0;
static request_t pending       = REQ_NONE;
static int       pending_slot  = 0;

static char     message[600] = "";
static uint32_t message_at   = 0;
static bool     have_message = false;

bool state_slots_boundary_pending = false;

// ─── Outcomes ────────────────────────────────────────────────────────────────

static const char *
title_note(void)
{
	return have_message && SDL_GetTicks() - message_at < NOTE_MS ? message : NULL;
}

static void
say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof message, fmt, ap);
	va_end(ap);
	message_at   = SDL_GetTicks();
	have_message = true;
	printf("%s\n", message);
	fflush(stdout);
	timing_refresh_title();
}

const char *
state_slots_message(unsigned *age_ms)
{
	if (!have_message)
		return NULL;
	if (age_ms)
		*age_ms = SDL_GetTicks() - message_at;
	return message;
}

// ─── Slots ───────────────────────────────────────────────────────────────────

void
state_slots_set_program(const char *prg_path)
{
	snprintf(program, sizeof program, "%s", prg_path ? prg_path : "");
	timing_title_note = title_note;
}

int
state_slots_selected(void)
{
	return selected;
}

static bool
valid(int slot)
{
	return slot >= 0 && slot < STATE_SLOTS;
}

void
state_slots_select_quietly(int slot)
{
	if (valid(slot))
		selected = slot;
}

void
state_slots_select(int slot)
{
	if (!valid(slot))
		return;
	selected = slot;
	say("Slot %d selected%s", slot, state_slots_exists(slot, NULL) ? "" : " (empty)");
}

bool
state_slots_path(int slot, char *out, size_t out_size)
{
	char name[300];
	return export_state_name(program, slot, name, sizeof name) && export_path(EXPORT_STATE, name, out, out_size);
}

bool
state_slots_exists(int slot, time_t *written)
{
	// Where the file would be, without making the folder just to look.
	char name[300], path[1400];
	if (!export_state_name(program, slot, name, sizeof name))
		return false;
	const char *dir = export_dir(EXPORT_STATE);
	if (dir[0])
		snprintf(path, sizeof path, "%s%c%s", dir, strchr(dir, '\\') ? '\\' : '/', name);
	else
		snprintf(path, sizeof path, "%s", name);
#ifdef _WIN32
	struct _stat64 st;
	if (_stat64(path, &st) != 0 || !(st.st_mode & _S_IFREG))
		return false;
#else
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return false;
#endif
	if (written)
		*written = st.st_mtime;
	return true;
}

// ─── Doing it ────────────────────────────────────────────────────────────────

static void
save_now(int slot)
{
	char path[1400], err[512];
	if (!state_slots_path(slot, path, sizeof path)) {
		say("Slot %d not saved: cannot make the folder for %s", slot, path);
		return;
	}
	if (savestate_file_save(path, err, sizeof err) == 0) {
		say("Slot %d not saved: %s", slot, err);
		return;
	}
	say("Saved slot %d", slot);
	printf("  to %s\n", path);
}

static bool
load_now(int slot)
{
	char path[1400], err[512];
	if (!state_slots_exists(slot, NULL)) {
		say("Slot %d is empty", slot);
		return false;
	}
	if (!state_slots_path(slot, path, sizeof path)) {
		say("Slot %d not loaded: no path for it", slot);
		return false;
	}
	size_t size = 0;
	if (!savestate_file_load(path, &size, err, sizeof err)) {
		say("Slot %d not loaded: %s", slot, err);
		return false;
	}
	say("Loaded slot %d", slot);
	printf("  from %s\n", path);
	return true;
}

static void
request(request_t what, int slot)
{
	if (slot < 0)
		slot = selected;
	if (!valid(slot))
		return;
	const char *verb = what == REQ_SAVE ? "saved" : "loaded";
	if (debug_server_step_pending()) {
		say("Slot %d not %s: a debugger client is stepping the machine", slot, verb);
		return;
	}
	if (!DEBUGIsPaused()) {
		// At the next frame boundary; a second request before then replaces it.
		pending      = what;
		pending_slot = slot;
		return;
	}
	if (what == REQ_LOAD) {
		if (load_now(slot)) {
			// The debugger's idea of where the machine stopped follows the load,
			// as after an x16/loadState.
			DEBUGBreakToDebugger();
			DEBUGSetStopReason("restore");
			debug_server_notify_stopped("restore");
		}
	} else if (savestate_at_frame_boundary()) {
		save_now(slot);
	} else {
		say("Slot %d not saved: the machine is paused part-way through a frame; continue, or step to the end of one",
		    slot);
	}
}

void
state_slots_request_save(int slot)
{
	request(REQ_SAVE, slot);
}

void
state_slots_request_load(int slot)
{
	request(REQ_LOAD, slot);
}

bool
state_slots_hotkey(SDL_Keycode sym)
{
	if (sym == SDLK_F1) {
		state_slots_request_load(-1);
		return true;
	}
	if (sym == SDLK_F3) {
		state_slots_request_save(-1);
		return true;
	}
	if (sym >= SDLK_0 && sym <= SDLK_9) {
		state_slots_select((int)(sym - SDLK_0));
		return true;
	}
	return false;
}

// ─── Main loop hooks ─────────────────────────────────────────────────────────

void
state_slots_frame_end(void)
{
	state_slots_boundary_pending = true;
}

void
state_slots_at_boundary(void)
{
	state_slots_boundary_pending = false;
	savestate_mark_frame_boundary();
	const request_t what = pending;
	pending              = REQ_NONE;
	if (what == REQ_SAVE)
		save_now(pending_slot);
	else if (what == REQ_LOAD)
		load_now(pending_slot);
}
