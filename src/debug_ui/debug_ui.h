// Commander X16 Emulator — in-process Dear ImGui debugger.
//
// C-callable API so the C emulator core (main.c / video.c) can drive the
// ImGui debugger without knowing any C++. The implementation lives in
// debug_ui.cpp; individual panels live under panels/ and self-register (see
// debug_ui_panels.h).
#ifndef DEBUG_UI_H
#define DEBUG_UI_H

#include <stdbool.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the ImGui context + SDL2/SDLRenderer2 backends. `win`/`ren` are a
// dedicated debugger SDL window + renderer (separate from the emulator display
// window), created by the caller (see video.c). Enables docking. Idempotent.
void debug_ui_init(SDL_Window *win, SDL_Renderer *ren);

// Feed one SDL event to ImGui. Call for every event pulled from the pump.
void debug_ui_process_event(const SDL_Event *ev);

// Build and present one debugger frame: NewFrame -> full-window DockSpace ->
// iterate the panel registry -> Render + present on the debugger renderer.
// Call once per emulator frame.
void debug_ui_render(void);

// Destroy backends + ImGui context. Safe to call even if never initialized.
void debug_ui_shutdown(void);

// Reflects the -imgui flag / runtime enable state.
bool debug_ui_is_enabled(void);
void debug_ui_set_enabled(bool enabled);

// The debugger's SDL_Renderer (the one passed to debug_ui_init). Panels that
// need to create SDL_Texture objects for ImGui::Image (e.g. the VERA graphical
// viewers) obtain it here. Returns nullptr before debug_ui_init runs.
SDL_Renderer *debug_ui_get_renderer(void);

// True when ImGui wants to consume keyboard input this frame (e.g. a text field
// is focused). NOTE: with keyboard nav enabled this is ALSO true whenever the
// debugger window merely has focus, so it is too broad for gating function-key
// shortcuts — use debug_ui_want_text_input() for those.
bool debug_ui_want_capture_keyboard(void);

// True only while an ImGui text field is actively accepting text input. Used to
// gate the F5/F9/F10/F11 debug shortcuts so they work whenever the debugger
// window is focused but are suppressed while the user types in a goto/search box.
bool debug_ui_want_text_input(void);

// Run to the "cursor" — the disassembly row currently under the mouse, if any
// (published each frame by the Disassembly panel via debug_ui_set_cursor).
// No-op unless the machine is paused and a valid cursor is set. Returns true if
// a run-to actually started. Backs the Ctrl+F10 "run to cursor" shortcut.
bool debug_ui_run_to_cursor(void);

// Toggle a breakpoint on the current instruction (regs.pc / regs.k). Backs the
// F9 shortcut so it works regardless of which debugger panel has focus.
void debug_ui_toggle_breakpoint_at_pc(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DEBUG_UI_H
