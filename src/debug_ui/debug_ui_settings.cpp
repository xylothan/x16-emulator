// Commander X16 Emulator — debugger user preferences. See debug_ui_settings.h.

#include "imgui.h"
#include "imgui_internal.h" // ImGuiSettingsHandler
#include "debug_ui_settings.h"
#include "debug_ui_bridge.h"

#include <stdio.h>
#include <string.h>

DebugUiSettings &
debug_ui_settings()
{
    static DebugUiSettings s;
    return s;
}

// ---------------------------------------------------------------------------
// imgui.ini persistence
//
// ImGui lets an application add its own [Type][Name] section to the ini file.
// One entry per setting keeps the file readable and means an unknown key from a
// newer build is simply ignored rather than corrupting anything.
// ---------------------------------------------------------------------------
namespace {

struct SettingField {
    const char *key;
    bool       *b;
    float      *f;
    int        *i;
};

// Built lazily because it points into the settings singleton.
const SettingField *
fields(int *count)
{
    DebugUiSettings &s = debug_ui_settings();
    static const SettingField table[] = {
        { "AutoSwitchDisasm",    &s.auto_switch_disasm,     nullptr, nullptr },
        { "AutoSwitchSource",    &s.auto_switch_source,     nullptr, nullptr },
        { "FollowInterrupts",    &s.follow_interrupts,      nullptr, nullptr },
        { "BreakOnInterrupt",    &s.break_on_interrupt,     nullptr, nullptr },
        { "MemHighlightChanges", &s.mem_highlight_changes,  nullptr, nullptr },
        { "AudioHoldOnPause",    &s.audio_hold_on_pause,    nullptr, nullptr },
        { "MemHighlightSeconds", nullptr, &s.mem_highlight_seconds, nullptr },
        { "UiScale",             nullptr, &s.ui_scale, nullptr },
        { "AllowBreakingChanges", &s.allow_breaking_changes, nullptr, nullptr },
        { "IoTraceCapture",      &s.io_trace_capture,       nullptr, nullptr },
        { "IoTraceCapacity",     nullptr, nullptr, &s.io_trace_capacity },
        { "IoCapVia",            &s.io_cap_via,             nullptr, nullptr },
        { "IoCapVera",           &s.io_cap_vera,            nullptr, nullptr },
        { "IoCapSpi",            &s.io_cap_spi,             nullptr, nullptr },
        { "IoCapYm",             &s.io_cap_ym,              nullptr, nullptr },
        { "IoCapEmu",            &s.io_cap_emu,             nullptr, nullptr },
        { "IoCapMidi",           &s.io_cap_midi,            nullptr, nullptr },
        { "IoCapOpenBus",        &s.io_cap_openbus,         nullptr, nullptr },
        { "IoCapSd",             &s.io_cap_sd,              nullptr, nullptr },
        { "IoCapFiles",          &s.io_cap_files,           nullptr, nullptr },
        { "IoCapI2c",            &s.io_cap_i2c,             nullptr, nullptr },
        { "IoCapJoy",            &s.io_cap_joy,             nullptr, nullptr },
        { "IoFatAutoIndex",      &s.io_fat_autoindex,       nullptr, nullptr },
    };
    *count = (int)(sizeof(table) / sizeof(table[0]));
    return table;
}

void *
settings_read_open(ImGuiContext *, ImGuiSettingsHandler *, const char *name)
{
    // Single anonymous section; returning non-null tells ImGui to feed us lines.
    return strcmp(name, "Prefs") == 0 ? (void *)1 : nullptr;
}

void
settings_read_line(ImGuiContext *, ImGuiSettingsHandler *, void *, const char *line)
{
    char key[64];
    float value = 0.0f;
    if (sscanf(line, "%63[^=]=%f", key, &value) != 2) {
        return;
    }
    int                 n = 0;
    const SettingField *t = fields(&n);
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].key, key) != 0) {
            continue;
        }
        if (t[i].b) {
            *t[i].b = (value != 0.0f);
        } else if (t[i].f) {
            *t[i].f = value;
        } else if (t[i].i) {
            *t[i].i = (int)value;
        }
        return;
    }
}

// Push the loaded execution prefs into the debugger core, which owns the
// actual behaviour.
void
settings_apply_to_core()
{
    DebugUiSettings &s = debug_ui_settings();
    DEBUGSetFollowInterrupts(s.follow_interrupts);
    DEBUGSetBreakOnInterrupt(s.break_on_interrupt);
}

void
settings_apply_all(ImGuiContext *, ImGuiSettingsHandler *)
{
    settings_apply_to_core();
}

void
settings_write_all(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf)
{
    int                 n = 0;
    const SettingField *t = fields(&n);
    buf->appendf("[%s][Prefs]\n", handler->TypeName);
    for (int i = 0; i < n; i++) {
        if (t[i].b) {
            buf->appendf("%s=%d\n", t[i].key, *t[i].b ? 1 : 0);
        } else if (t[i].f) {
            buf->appendf("%s=%.3f\n", t[i].key, *t[i].f);
        } else if (t[i].i) {
            buf->appendf("%s=%d\n", t[i].key, *t[i].i);
        }
    }
    buf->append("\n");
}

} // namespace

void
debug_ui_settings_register(void)
{
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (!ctx) {
        return;
    }
    ImGuiSettingsHandler h;
    h.TypeName   = "X16Debugger";
    h.TypeHash   = ImHashStr("X16Debugger");
    h.ReadOpenFn = settings_read_open;
    h.ReadLineFn = settings_read_line;
    h.ApplyAllFn = settings_apply_all;
    h.WriteAllFn = settings_write_all;
    ctx->SettingsHandlers.push_back(h);
}

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------
namespace {

// Checkbox that marks the ini dirty so the change is persisted.
bool
pref_checkbox(const char *label, bool *v, const char *help)
{
    const bool changed = ImGui::Checkbox(label, v);
    if (help && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", help);
    }
    if (changed) {
        ImGui::MarkIniSettingsDirty();
    }
    return changed;
}

} // namespace

void
debug_ui_settings_mark_dirty(void)
{
    ImGui::MarkIniSettingsDirty();
}

void
debug_ui_draw_settings_window(bool *p_open)
{
    if (!*p_open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", p_open)) {
        ImGui::End();
        return;
    }

    DebugUiSettings &s = debug_ui_settings();

    ImGui::TextDisabled("Preferences are saved in imgui.ini and restored next run.");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Scales the whole debugger - menus, tabs, buttons and spacing,");
        ImGui::TextDisabled("not just panel text. Ctrl+wheel over a panel zooms that panel");
        ImGui::TextDisabled("on top of this.");
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderFloat("Interface scale", &s.ui_scale, 0.75f, 2.50f, "%.2fx")) {
            debug_ui_settings_mark_dirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The font is re-rasterised at the new size rather than stretched,\n"
                              "so text stays sharp. Useful on a high-DPI display where the\n"
                              "debugger would otherwise be unreadably small.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##uiscale")) {
            s.ui_scale = 1.0f;
            debug_ui_settings_mark_dirty();
        }
    }

    if (ImGui::CollapsingHeader("Navigation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Keep the visible panel pointed at wherever execution stopped.");
        pref_checkbox("Switch to Disassembly when there is no source", &s.auto_switch_disasm,
                      "Stopping in code the debug info doesn't cover (KERNAL ROM, a\n"
                      "trampoline, an interrupt handler in ROM) leaves the Source panel\n"
                      "showing an unrelated file. With this on, the Disassembly panel is\n"
                      "brought to the front instead.");
        pref_checkbox("Switch back to Source when there is source", &s.auto_switch_source,
                      "Returning to code the debug info covers brings the Source panel\n"
                      "back to the front.");
    }

    if (ImGui::CollapsingHeader("Execution", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (pref_checkbox("Follow interrupts while stepping", &s.follow_interrupts,
                          "An interrupt that fires in the middle of a step normally runs to\n"
                          "completion invisibly. With this on, stepping stops at the\n"
                          "handler's first instruction so you can watch it.")) {
            DEBUGSetFollowInterrupts(s.follow_interrupts);
        }
        if (pref_checkbox("Break on every interrupt", &s.break_on_interrupt,
                          "Stop the machine whenever any interrupt is taken, even while\n"
                          "free-running. Useful for catching the first entry to a handler.")) {
            DEBUGSetBreakOnInterrupt(s.break_on_interrupt);
        }
    }

    if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen)) {
        pref_checkbox("Highlight changed bytes", &s.mem_highlight_changes,
                      "Pulse the background of a byte when its value changes, so the\n"
                      "effect of an instruction is obvious while stepping.");
        ImGui::BeginDisabled(!s.mem_highlight_changes);
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderFloat("Fade time", &s.mem_highlight_seconds, 0.25f, 5.0f, "%.2f s")) {
            ImGui::MarkIniSettingsDirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How long a changed byte stays tinted before fading out.");
        }
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        pref_checkbox("Hold audio panels while paused", &s.audio_hold_on_pause,
                      "Pausing silences the machine, so the PSG/YM2151/PCM meters and\n"
                      "scopes would otherwise read as silence. With this on they keep\n"
                      "showing the state from the moment execution stopped.");
    }

    if (ImGui::CollapsingHeader("I/O trace", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Feeds the Activity log in the I/O panel. This is the one");
        ImGui::TextDisabled("debugger feature the running machine pays for, so it can be");
        ImGui::TextDisabled("turned off outright.");
        pref_checkbox("Capture I/O events", &s.io_trace_capture,
                      "Record register accesses and decoded device events.\n"
                      "With this off the I/O panel still shows live device state;\n"
                      "it just has no history to show.");

        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("Events retained", &s.io_trace_capacity, 256, 65536)) {
            debug_ui_settings_mark_dirty();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Size of the ring buffer. Changing it discards what is\n"
                              "currently held, because ordering against a new ring is\n"
                              "not meaningful.");
        }

        ImGui::TextDisabled("Per-device capture is on the I/O panel's Activity tab.");
        pref_checkbox("Index the filesystem in an SD card image", &s.io_fat_autoindex,
                      "Parse the FAT inside an attached image when it is attached, so\n"
                      "block traffic resolves to filenames. The emulator itself never\n"
                      "does this - to it an SD card is 512-byte blocks and nothing more.\n\n"
                      "Off means the SD tab still works, it just reports raw LBAs.");
    }

    if (ImGui::CollapsingHeader("Safety", ImGuiTreeNodeFlags_DefaultOpen)) {
        pref_checkbox("Allow breaking modifications", &s.allow_breaking_changes,
                      "Off by default. Unlocks the few register edits that change machine\n"
                      "state the running program has already committed to - it will usually\n"
                      "crash rather than cope.\n\n"
                      "Currently gates the E (emulation) flag on the 65C816. Clearing E\n"
                      "moves interrupts from $FFFE to $FFEE, and the stock X16 ROM has no\n"
                      "native-mode vectors there, so the next IRQ (60 Hz) runs off into\n"
                      "whatever bytes happen to sit at $FFEE.\n\n"
                      "Turn this on for hand-driving the CPU, not for a live program.");
        if (s.allow_breaking_changes) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f), "(unlocked)");
        }
    }

    ImGui::End();
}
