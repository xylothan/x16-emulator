// Commander X16 Emulator — the host key. See hostkey.h.

#include "hostkey.h"

#include <string.h>

#ifdef __APPLE__
static int  host    = 231; // SDL_SCANCODE_RGUI, Right Command: a Mac has no Scroll Lock
#else
static int  host    = 71; // SDL_SCANCODE_SCROLLLOCK
#endif
static bool held    = false;
static bool chorded = false; // another key went down while it was held
static bool latched = false;
static bool taken[HOSTKEY_SCANCODES];

void
hostkey_set(int scancode)
{
	if (scancode > 0 && scancode < HOSTKEY_SCANCODES) {
		host    = scancode;
		held    = false;
		latched = false;
		memset(taken, 0, sizeof taken);
	}
}

int
hostkey_scancode(void)
{
	return host;
}

bool
hostkey_held(void)
{
	return held;
}

bool
hostkey_latched(void)
{
	return latched;
}

void
hostkey_unlatch(void)
{
	latched = false;
}

hostkey_action
hostkey_event(bool down, int scancode, bool repeat)
{
	if (scancode <= 0 || scancode >= HOSTKEY_SCANCODES)
		return HOSTKEY_PASS;

	if (scancode == host) {
		if (down) {
			if (!repeat) {
				held    = true;
				chorded = false;
			}
		} else if (held) {
			held = false;
			if (!chorded)
				latched = !latched; // a tap on its own
		}
		return HOSTKEY_EAT;
	}

	if (!down) {
		if (taken[scancode]) {
			taken[scancode] = false;
			return HOSTKEY_EAT;
		}
		return HOSTKEY_PASS;
	}

	if (taken[scancode])
		return HOSTKEY_EAT; // held down from a command: its repeats are not more commands
	if (!held && !latched)
		return HOSTKEY_PASS;

	if (repeat)
		return HOSTKEY_EAT; // went down before the host key did: the guest has it and gets its release
	taken[scancode] = true;
	if (held)
		chorded = true;
	if (!held && scancode == HOSTKEY_ESCAPE) {
		latched = false;
		return HOSTKEY_EAT;
	}
	return HOSTKEY_COMMAND;
}
