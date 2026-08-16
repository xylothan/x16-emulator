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

    // --- I/O trace ----------------------------------------------------------
    // The I/O panel records a ring of register accesses and decoded device
    // events. Capture is off by default and every device with it: this is the
    // one debugger feature that costs the running machine anything -- a test
    // and a branch on every I/O access, and a copy on every captured one --
    // and the I/O page is the busiest address range in the system. The panel's
    // live state tabs all work without it; only the Activity log needs it.
    bool io_trace_capture = false;
    int  io_trace_capacity = 4096; // events retained; clamped by io_trace.c

    // Per-device capture, all off by default so that switching capture on
    // records nothing until the user says what they are actually looking for.
    // Rates differ by orders of magnitude -- see the tooltips in the panel.
    bool io_cap_via = false;
    bool io_cap_vera = false;
    bool io_cap_spi = false;
    bool io_cap_ym = false;
    bool io_cap_emu = false;
    bool io_cap_midi = false;
    bool io_cap_openbus = false;

    // Decoded device events, split per device rather than lumped together
    // because their rates differ by orders of magnitude.
    bool io_cap_sd = false;
    bool io_cap_files = false;
    bool io_cap_i2c = false;
    bool io_cap_joy = false;

    // Index the filesystem inside an attached SD image, so block traffic can be
    // named. Off means the SD tab still works, it just reports LBAs.
    bool io_fat_autoindex = true;

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
