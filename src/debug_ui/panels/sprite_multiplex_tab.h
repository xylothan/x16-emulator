// The sprite multiplexing views, drawn as a tab inside the VERA panel.
//
// These live in their own translation unit rather than in vera_panel.cpp
// because they are a good deal of code with their own state -- a trace buffer,
// a composited texture, a set of view options -- and vera_panel.cpp is already
// large. Kept as a tab rather than a panel of its own because what it shows is
// VERA sprite state, and it belongs next to the static sprite table it exists
// to explain.

#ifndef SPRITE_MULTIPLEX_TAB_H
#define SPRITE_MULTIPLEX_TAB_H

#include <stdint.h>

// Draws the tab body. `pal` is the caller's already-built 256-entry ABGR8888
// palette, shared so the VERA panel builds it once for all of its tabs.
void draw_sprite_multiplex_tab(const uint32_t pal[256]);

#endif // SPRITE_MULTIPLEX_TAB_H
