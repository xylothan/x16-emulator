// Commander X16 Emulator — debugger user preferences.
//
// One place for the debugger's behavioural toggles, so panels read a single
// source of truth instead of each keeping its own static, and so the user can
// see and change them together (System > Settings).
//
// Preferences persist in imgui.ini through an ImGuiSettingsHandler, alongside
// the window layout ImGui already stores there — no extra config file.
#ifndef DEBUG_UI_SETTINGS_H
#define DEBUG_UI_SETTINGS_H

#include <stdbool.h>

struct DebugUiSettings {
    // --- Appearance ---------------------------------------------------------
    // Global interface scale: the whole debugger, not just panel text. Per-panel
    // Ctrl+wheel zoom stacks on top of this. Scales widget metrics (padding,
    // spacing, scrollbars) as well as the font, so menus, tab bars and buttons
    // grow with it instead of staying tiny.
    float ui_scale = 1.0f;

    // --- Navigation ---------------------------------------------------------
    // Where execution stops is not always where the user is looking. These make
    // the debugger follow the PC between the Source and Disassembly views.
    bool auto_switch_disasm = true;  // stop with no source -> focus Disassembly
    bool auto_switch_source = true;  // stop with source    -> focus Source

    // --- Execution ----------------------------------------------------------
    bool follow_interrupts = false;  // stepping stops at a handler that fires
    bool break_on_interrupt = false; // stop on every interrupt entry

    // --- Memory -------------------------------------------------------------
    bool  mem_highlight_changes = true;  // pulse bytes that changed
    float mem_highlight_seconds = 1.5f;  // how long the pulse takes to fade

    // --- Audio --------------------------------------------------------------
    bool audio_hold_on_pause = true; // freeze audio panels instead of blanking

    // --- Safety -------------------------------------------------------------
    // Most register edits are recoverable pokes. A few are not: they change
    // machine state the running program has already committed to, and it will
    // usually crash rather than cope. Those are gated behind this and are off by
    // default, so nobody reaches for one thinking it is a display option.
    //
    // The E (emulation) flag is the clearest case - see cpu_panel.cpp.
    bool allow_breaking_changes = false;
};

// The live settings. Mutating these takes effect immediately.
DebugUiSettings &debug_ui_settings();

// Install the imgui.ini persistence hook. Call once, before ImGui loads its
// ini file (i.e. before the first NewFrame).
void debug_ui_settings_register(void);

// The Settings window itself (System > Settings).
void debug_ui_draw_settings_window(bool *p_open);

// Note that a preference changed so it gets written back to imgui.ini. Panels
// that expose the same toggle call this instead of pulling in imgui_internal.h.
void debug_ui_settings_mark_dirty(void);

#endif
