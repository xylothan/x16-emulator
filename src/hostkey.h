// Commander X16 Emulator — the host key.
//
// One key, Scroll Lock (Right Command on macOS) unless set otherwise, that turns the keyboard into
// emulator commands. Held, the next key is a command (Host+F3 saves a state);
// tapped on its own, it latches command mode until it is tapped again or Esc
// is pressed. Neither the host key nor any key pressed while it is in effect
// reaches the X16, so every ordinary key and Ctrl combination stays the guest's.
//
// This is only the bookkeeping, kept free of SDL so it can be tested alone.
// Scancodes are SDL's, which are USB HID usage numbers. video.c owns the
// commands themselves.

#ifndef HOSTKEY_H
#define HOSTKEY_H

#include <stdbool.h>

#define HOSTKEY_SCANCODES 512 // SDL_NUM_SCANCODES
#define HOSTKEY_ESCAPE    41  // SDL_SCANCODE_ESCAPE

typedef enum {
	HOSTKEY_PASS,    // the guest's: hand it to the X16 as usual
	HOSTKEY_EAT,     // ours, and nothing to do: drop it
	HOSTKEY_COMMAND, // a command key: carry it out, and do not hand it on
} hostkey_action;

void hostkey_set(int scancode);
int  hostkey_scancode(void);

// One key event, in the order they arrive. A key whose press was taken is
// taken on its release too, whenever that comes, and a key the guest saw go
// down sees its release however the host key has moved since, so the X16 is
// never left with a key stuck down or released twice.
hostkey_action hostkey_event(bool down, int scancode, bool repeat);

bool hostkey_held(void);
bool hostkey_latched(void);
void hostkey_unlatch(void);

#endif
