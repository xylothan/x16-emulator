// Commander X16 Emulator — debugger user preferences. See debug_ui_settings.h.

#include "imgui.h"
#include "imgui_internal.h" // ImGuiSettingsHandler
#include "debug_ui_settings.h"
#include "debug_ui_bridge.h"
#include "debug_info_page.h"
#include "export_page.h"
#include "source_folders.h"
#include "../perf_budget.h"
#include "../source_view.h"

#include <ctype.h>

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
        { "PerfAlwaysOn",        &s.perf_always_on,         nullptr, nullptr },
        { "PerfTargetFps",       nullptr, &s.perf_target_fps, nullptr },
        { "PerfIdleMode",        nullptr, nullptr, &s.perf_idle_mode },
        { "PerfCapacity",        nullptr, nullptr, &s.perf_capacity },
        { "PerfGraphFrames",     nullptr, nullptr, &s.perf_graph_frames },
        { "ProfileExportStamp",  &s.profile_export_stamp,   nullptr, nullptr },
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
    // Text-valued keys first: a path does not parse as a number.
    {
        DebugUiSettings &ds = debug_ui_settings();
        static const struct { const char *key; std::string DebugUiSettings::*field; } text[] = {
            { "ExportProfileDir=",    &DebugUiSettings::export_profile_dir },
            { "ExportScreenshotDir=", &DebugUiSettings::export_screenshot_dir },
            { "ExportDumpDir=",       &DebugUiSettings::export_dump_dir },
            { "ProfileExportName=",   &DebugUiSettings::profile_export_name },
        };
        for (const auto &t : text) {
            const size_t n = strlen(t.key);
            if (strncmp(line, t.key, n) == 0) {
                ds.*(t.field) = line + n;
                return;
            }
        }
    }
    if (strncmp(line, "SettingsPage=", 13) == 0) {
        debug_ui_settings_set_page(line + 13);
        return;
    }
    if (strncmp(line, "SourceFolder=", 13) == 0) {
        if (line[13])
            debug_ui_settings().source_folders.push_back(line + 13);
        return;
    }
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
    // Saved source folders join -srcpath and the debug info's own folder.
    for (const std::string &dir : s.source_folders)
        source_view_add_path(dir.c_str());
    if (!s.source_folders.empty())
        source_view_retry_missing();
    debug_ui_apply_export_dirs();
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
    for (const std::string &dir : debug_ui_settings().source_folders)
        buf->appendf("SourceFolder=%s\n", dir.c_str());
    {
        const DebugUiSettings &ds = debug_ui_settings();
        buf->appendf("ExportProfileDir=%s\n", ds.export_profile_dir.c_str());
        buf->appendf("ExportScreenshotDir=%s\n", ds.export_screenshot_dir.c_str());
        buf->appendf("ExportDumpDir=%s\n", ds.export_dump_dir.c_str());
        buf->appendf("ProfileExportName=%s\n", ds.profile_export_name.c_str());
    }
    const char *page = debug_ui_settings_page();
    if (page && page[0])
        buf->appendf("SettingsPage=%s\n", page);
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
//
// One window, not a dockable panel: a list of pages on the left, grouped, and
// the selected page's settings on the right. Every change applies at once and
// is saved; there is no OK or Cancel. New pages are one row in PAGES.
// ---------------------------------------------------------------------------
namespace {

bool        s_open = false;
std::string s_page;          // the page shown, by name; saved as SettingsPage=
char        s_search[64] = "";

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

// ─── Pages ──────────────────────────────────────────────────────────────────

void
page_appearance()
{
    DebugUiSettings &s = debug_ui_settings();
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
    ImGui::TextDisabled("Ctrl+wheel over a panel zooms that panel on top of this.");
}

void
page_navigation()
{
    DebugUiSettings &s = debug_ui_settings();
    pref_checkbox("Switch to Disassembly when there is no source", &s.auto_switch_disasm,
                  "Stopping in code the debug info doesn't cover (KERNAL ROM, a\n"
                  "trampoline, an interrupt handler in ROM) leaves the Source panel\n"
                  "showing an unrelated file. With this on, the Disassembly panel is\n"
                  "brought to the front instead.");
    pref_checkbox("Switch back to Source when there is source", &s.auto_switch_source,
                  "Returning to code the debug info covers brings the Source panel\n"
                  "back to the front.");
}

void
page_stepping()
{
    DebugUiSettings &s = debug_ui_settings();
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

void
page_safety()
{
    DebugUiSettings &s = debug_ui_settings();
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

void
page_memory()
{
    DebugUiSettings &s = debug_ui_settings();
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

void
page_audio()
{
    DebugUiSettings &s = debug_ui_settings();
    pref_checkbox("Hold audio panels while paused", &s.audio_hold_on_pause,
                  "Pausing silences the machine, so the PSG/YM2151/PCM meters and\n"
                  "scopes would otherwise read as silence. With this on they keep\n"
                  "showing the state from the moment execution stopped.");
}

void
page_io_capture()
{
    DebugUiSettings &s = debug_ui_settings();
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
    pref_checkbox("Index the filesystem in an SD card image", &s.io_fat_autoindex,
                  "Parse the FAT inside an attached image when it is attached, so\n"
                  "block traffic resolves to filenames. The emulator itself never\n"
                  "does this - to it an SD card is 512-byte blocks and nothing more.\n"
                  "While on, the index is rebuilt whenever the filesystem changes.\n\n"
                  "Off means the SD tab still works, it just reports raw LBAs, and\n"
                  "an index built by hand is marked STALE instead of refreshing.");
}

void
page_io_devices()
{
    ImGui::TextDisabled("Nothing is recorded until Capture I/O events is on and at least one device is ticked.");
    if (debug_ui_io_device_toggles())
        debug_ui_settings_mark_dirty();
}

void
page_perf()
{
    DebugUiSettings &s = debug_ui_settings();
    pref_checkbox("Keep profiling when hidden", &s.perf_always_on,
                  "Profiling costs the running machine about 0.4 ms of host time\n"
                  "per frame, so it normally runs only while the Performance panel is\n"
                  "on screen. Turn this on to keep recording regardless -- to build up\n"
                  "history while you look at other panels.");
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputFloat("Target FPS", &s.perf_target_fps, 0.0f, 0.0f, "%.4g")) {
        s.perf_target_fps = s.perf_target_fps < 1.0f ? 1.0f : s.perf_target_fps > 1000.0f ? 1000.0f : s.perf_target_fps;
        perf_budget_set_target_fps(s.perf_target_fps);
        debug_ui_settings_mark_dirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The frame rate the program aims for: its budget per frame is\n"
                          "1/this. 60 for most games; less for one that runs at 30.");
    static const char *modes[] = {"Auto (WAI + spin detection)", "Markers and idle zones only",
                                  "Nothing is idle"};
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Idle detection", &s.perf_idle_mode, modes, 3)) {
        perf_budget_set_idle_mode((perf_idle_mode_t)s.perf_idle_mode);
        debug_ui_settings_mark_dirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How waiting is told apart from work.\n\n"
                          "Auto catches WAI and a tight loop that stores nothing, which covers\n"
                          "polling $9F27 and polling a flag an IRQ handler sets. It CANNOT see\n"
                          "a wait loop that stores or calls a subroutine -- mark those as an\n"
                          "idle zone, or bracket the real work with $9FBC markers.");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("History (frames)", &s.perf_capacity, 120, 7200)) {
        perf_budget_set_capacity(s.perf_capacity);
        debug_ui_settings_mark_dirty();
    }
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderInt("Graph frames", &s.perf_graph_frames, 30, 1024))
        debug_ui_settings_mark_dirty();
}

struct Page {
    const char *group;
    const char *name;
    const char *about;    // one line under the title
    const char *keywords; // also matched by the search box
    void (*draw)();
};

// In the order shown. Group by what a setting is about, not by panel.
const Page PAGES[] = {
    {"General", "Appearance", "How big the debugger is drawn.", "interface scale font size dpi zoom",
     page_appearance},
    {"General", "Navigation", "Keep the visible panel pointed at wherever execution stopped.",
     "switch disassembly source follow focus", page_navigation},
    {"Debugging", "Stepping & interrupts", "What stepping and breaking do when an interrupt fires.",
     "irq follow break step interrupt handler", page_stepping},
    {"Debugging", "Debug info", "Which .dbg files are loaded, and switching to another build's.",
     "dbg debug info symbols reload replace wrong build cc65", debug_info_draw_page},
    {"Debugging", "Source folders", "Where the debugger looks for the source files the debug info names.",
     "srcpath path search missing files folder", source_folders_draw_page},
    {"Debugging", "Safety", "Edits that can crash a running program are locked unless allowed here.",
     "breaking modifications emulation flag 65c816 unlock", page_safety},
    {"Files", "Export folders", "Where screenshots, memory dumps and profile exports are written.",
     "export folder path directory screenshot dump profile save output", export_draw_page},
    {"Memory & Audio", "Memory", "The Memory panel.", "highlight changed bytes fade pulse", page_memory},
    {"Memory & Audio", "Audio", "The PSG, YM2151 and PCM panels.", "pause hold meters scopes psg ym pcm",
     page_audio},
    {"I/O trace", "Capture",
     "Feeds the I/O panel's Activity log. The one debugger feature the running machine pays for.",
     "io trace capture ring events capacity sd fat index", page_io_capture},
    {"I/O trace", "Devices", "Which devices the I/O trace records. Rates differ by orders of magnitude.",
     "via vera spi ym emu midi open bus sd files i2c joystick", page_io_devices},
    {"Performance", "Frame budget",
     "The Performance panel's work-vs-waiting recorder: what a frame's budget is, and how much history to keep.",
     "fps target idle wai history graph budget profiling", page_perf},
};

const Page *
find_page(const std::string &name)
{
    for (const Page &p : PAGES)
        if (name == p.name)
            return &p;
    return nullptr;
}

bool
contains_ci(const char *hay, const char *needle)
{
    if (!needle[0])
        return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}

bool
matches(const Page &p, const char *q)
{
    return contains_ci(p.name, q) || contains_ci(p.group, q) || contains_ci(p.keywords, q) ||
           contains_ci(p.about, q);
}

} // namespace

void
debug_ui_settings_mark_dirty(void)
{
    ImGui::MarkIniSettingsDirty();
}

void
debug_ui_open_settings(const char *page)
{
    s_open = true;
    if (page && find_page(page)) {
        s_page = page;
        s_search[0] = 0;
    }
    ImGui::SetWindowFocus("Settings###X16Settings");
}

bool
debug_ui_settings_is_open(void)
{
    return s_open;
}

void
debug_ui_draw_settings_window(void)
{
    if (!s_open) {
        return;
    }
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(780, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings###X16Settings", &s_open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const Page *cur = find_page(s_page);
    if (!cur) {
        cur    = &PAGES[0];
        s_page = cur->name;
    }

    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

    // ── The list of pages ───────────────────────────────────────────────────
    ImGui::BeginChild("##settings_nav", ImVec2(ImGui::GetFontSize() * 13.0f, -footer),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##settings_search", "Search settings", s_search, sizeof s_search);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show only the pages whose name, group or settings mention this.");
    ImGui::Spacing();
    int         shown       = 0;
    const Page *first_shown = nullptr;
    bool        cur_shown   = false;
    const char *group       = nullptr;
    bool        group_open  = false;
    for (const Page &p : PAGES) {
        if (!matches(p, s_search))
            continue;
        if (!group || strcmp(group, p.group) != 0) {
            group = p.group;
            ImGui::SetNextItemOpen(true, s_search[0] ? ImGuiCond_Always : ImGuiCond_Once);
            group_open = ImGui::TreeNodeEx(p.group, ImGuiTreeNodeFlags_SpanAvailWidth |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen);
        }
        shown++;
        if (!first_shown)
            first_shown = &p;
        if (&p == cur)
            cur_shown = true;
        if (!group_open)
            continue;
        ImGui::Indent();
        if (ImGui::Selectable(p.name, &p == cur)) {
            s_page = p.name;
            debug_ui_settings_mark_dirty();
        }
        ImGui::Unindent();
    }
    if (!shown)
        ImGui::TextDisabled("No settings match.");
    ImGui::EndChild();
    // Searching away from the page shown moves to the first that matches.
    if (shown && !cur_shown) {
        cur    = first_shown;
        s_page = cur->name;
    }

    // ── The page ────────────────────────────────────────────────────────────
    ImGui::SameLine();
    ImGui::BeginChild("##settings_page", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (shown) {
        ImGui::TextDisabled("%s  >", cur->group);
        ImGui::SameLine();
        ImGui::TextUnformatted(cur->name);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", cur->about);
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushID(cur->name);
        cur->draw();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ── Footer ──────────────────────────────────────────────────────────────
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Changes apply at once and are saved in imgui.ini.");
    const float close_w = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - close_w);
    if (ImGui::Button("Close"))
        s_open = false;

    ImGui::End();
}

void
debug_ui_settings_set_page(const char *page)
{
    s_page = page ? page : "";
}

const char *
debug_ui_settings_page(void)
{
    return s_page.c_str();
}
