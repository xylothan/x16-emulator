// Commander X16 Emulator — in-process Dear ImGui debugger implementation.
//
// Owns the ImGui context and the SDL2 + SDLRenderer2 backends. Each
// builds a full-window dock space and iterates the self-registered panel
// registry (debug_ui_panels.h), so panels are added purely by dropping a file
// under panels/.
#include "debug_ui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h" // regs + execution-control (DEBUGContinue/…/DEBUGIsRunning)
#include "debug_ui_settings.h" // user preferences (System → Settings...)
#include "source_folders.h"    // where to look for source files (Settings > Source folders)
#include "debug_ui_image.h"  // panel image textures (SDL_Texture or shared DX12)
#include "debug_ui_dx12.h"   // native DX12 debugger renderer (HAS_DX12 builds)
#include "../host_prof.h"     // host-time accounting: per-panel and UI draw cost
#include "../profile_capture.h" // capture mode: the debugger goes quiet while it records

// How often the debugger repaints its menu bar while a capture records.
static const Uint32 CAPTURE_UI_INTERVAL_MS = 250;

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* for the first-run default layout
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#ifdef HAS_DX12
#include "backends/imgui_impl_dx12.h"
#endif

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static SDL_Window   *s_window      = nullptr;
static SDL_Renderer *s_renderer    = nullptr;
static bool          s_initialized = false;
static bool          s_native_dx12 = false; // renderer backend is imgui_impl_dx12
static bool          s_enabled     = false;
static bool          s_build_default_layout = false; // install a dock layout on first run

// Benchmark configuration (-bench, -uipanels, -uiiotrace). A benchmark has to
// be reproducible, and the user's imgui.ini decides which panels are open and
// which docked tab is in front; it must also never be overwritten by a bench
// run. So a configured run neither loads nor saves the ini, and opens exactly
// the panels it names.
static bool        s_bench_no_ini      = false;
static std::string s_bench_panels;
static bool        s_bench_have_panels = false;
static bool        s_bench_io_capture  = false;
static bool          s_show_demo    = false;         // Help → ImGui Demo (vendored imgui_demo.cpp)

static void debug_ui_latch_goto(void); // defined below; called from debug_ui_render
static void debug_ui_follow_stop_location(void); // ditto

// Found in the wild: this project's saved imgui.ini with a table's lone
// WidthStretch column at `Weight=-nan(ind)` (the Breakpoints panel's
// Execution table - "Condition" is its only stretch column among five fixed
// ones). Exactly how a NaN gets written is still open - the obvious path,
// ImGui's TableUpdateColumnsWeightFromWidth dividing a zero stretch width by
// a zero total, is itself guarded by an assert on that total being positive
// in any build with asserts live, so it must have been produced by an
// asserts-disabled build taking some other route.
//
// Whatever wrote it, loading it back is what breaks things: the column
// renders at ~zero width (hiding the "set condition" button behind it, which
// looks like the feature went missing), and the first column-border drag
// that makes ImGui recompute weights hits IM_ASSERT(column->StretchWeight >
// 0.0f) in imgui_tables.cpp and aborts.
//
// Fixed as plain text, on disk, before ImGui ever parses the file (its ini
// auto-load happens inside the first NewFrame(), which runs after
// debug_ui_init() returns) - rather than by reaching into ImGui's internal
// settings structures after the fact. Plain text keeps this independent of
// how a given ImGui version happens to represent a loaded table's settings
// in memory; it only has to agree with the on-disk format, which is public
// and part of what SaveIniSettingsToDisk documents it writes.
static void
debug_ui_sanitize_ini_file(const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return; // no ini yet (first run) - nothing to sanitize
    }
    std::string text;
    char        buf[4096];
    size_t      n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        text.append(buf, n);
    }
    fclose(f);

    // Every "Weight=" in an imgui.ini belongs to a [Table] column, and every
    // one of those must be a finite, positive float - so any value that
    // isn't gets reset to ImGui's own default (equal weighting, 1.0) with no
    // need to parse out which table or column it belongs to.
    bool         changed = false;
    const char  *needle  = "Weight=";
    const size_t nlen    = strlen(needle);
    size_t       pos     = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        size_t value_start = pos + nlen;
        size_t value_end   = value_start;
        while (value_end < text.size() && text[value_end] != '\r' && text[value_end] != '\n') {
            value_end++;
        }
        const double value = strtod(text.c_str() + value_start, nullptr);
        if (!(value > 0.0) || !std::isfinite(value)) {
            text.replace(value_start, value_end - value_start, "1.0000");
            changed    = true;
            value_end  = value_start + 6; // length of "1.0000"
        }
        pos = value_end;
    }

    if (!changed) {
        return;
    }
    if (FILE *out = fopen(path, "wb")) {
        fwrite(text.data(), 1, text.size(), out);
        fclose(out);
    }
}

// ── Font ladder for per-window zoom ─────────────────────────────────────────
// ImGui's default font is rasterised once at a single size; scaling it (which
// SetWindowFontScale does) resamples the glyph bitmaps and looks awful. Build a
// ladder of real sizes instead, and have the zoom helper choose the nearest, so
// zoomed text is always rendered at its native size.
//
// The face matters as much as the size. ImGui's built-in ProggyClean is a
// pixel font: its outlines are drawn to land exactly on the pixel grid at 13px,
// where every stem is a solid, fully-opaque column. Rasterised at any other
// size the stems straddle pixel boundaries and get antialiased across two
// columns, so the text goes both soft AND dim - no pixel reaches full
// intensity. That is why zooming still looked washed out even once the
// resampling was gone.
//
// So prefer a genuinely scalable monospace face from the system, which is
// hinted to stay crisp at any size, and keep ProggyClean only as a fallback (in
// which case 100% still looks exactly as it always did).
static const char *s_mono_font_candidates[] = {
#if defined(_WIN32)
    "C:\\Windows\\Fonts\\consola.ttf",      // Consolas
    "C:\\Windows\\Fonts\\CascadiaMono.ttf",
    "C:\\Windows\\Fonts\\lucon.ttf",        // Lucida Console
#elif defined(__APPLE__)
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
#endif
};

// The base entry must be first so it stays the default font for anything that
// doesn't opt into zooming.
static const float s_font_sizes[] = {
    13.0f, // base
    9.0f, 10.0f, 11.0f, 12.0f, 14.0f, 15.0f, 16.0f, 18.0f,
    20.0f, 22.0f, 24.0f, 27.0f, 30.0f, 34.0f, 39.0f,
};
static ImFont *s_fonts[IM_ARRAYSIZE(s_font_sizes)] = {};
static bool    s_scalable_font = false;

static const char *
debug_ui_find_mono_font(void)
{
    for (int i = 0; i < IM_ARRAYSIZE(s_mono_font_candidates); i++) {
        if (FILE *f = fopen(s_mono_font_candidates[i], "rb")) {
            fclose(f);
            return s_mono_font_candidates[i];
        }
    }
    return nullptr;
}

static void
debug_ui_build_fonts(ImGuiIO &io)
{
    memset(s_fonts, 0, sizeof(s_fonts));
    const char *path = debug_ui_find_mono_font();
    s_scalable_font  = (path != nullptr);

    for (int i = 0; i < IM_ARRAYSIZE(s_font_sizes); i++) {
        if (s_scalable_font) {
            ImFontConfig cfg;
            // Oversample horizontally so stems land cleanly; a real outline
            // font stays sharp when it is hinted onto the pixel grid.
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            cfg.PixelSnapH  = false;
            s_fonts[i] = io.Fonts->AddFontFromFileTTF(path, s_font_sizes[i], &cfg);
        }
        if (!s_fonts[i]) {
            // Fallback: ImGui's built-in. Pixel-snapped, since ProggyClean is a
            // pixel font and snapping is the best it can do off its native size.
            ImFontConfig cfg;
            cfg.SizePixels = s_font_sizes[i];
            cfg.PixelSnapH = true;
            s_fonts[i]     = io.Fonts->AddFontDefault(&cfg);
            if (i == 0) {
                s_scalable_font = false;
            }
        }
    }
}

// Nearest built font to `size_px`, plus the size it actually is. Used by the
// zoom helper; returns nullptr before fonts are built.
ImFont *
debug_ui_font_for_size(float size_px, float *out_size)
{
    int   best  = 0;
    float bestd = 1e9f;
    for (int i = 0; i < IM_ARRAYSIZE(s_font_sizes); i++) {
        const float d = s_font_sizes[i] > size_px ? s_font_sizes[i] - size_px
                                                  : size_px - s_font_sizes[i];
        if (d < bestd) {
            bestd = d;
            best  = i;
        }
    }
    if (out_size) {
        *out_size = s_font_sizes[best];
    }
    return s_fonts[best];
}

float
debug_ui_font_base_size(void)
{
    // Per-window zoom stacks on top of the global interface scale, so the base
    // it multiplies is the already-scaled size.
    return s_font_sizes[0] * debug_ui_settings().ui_scale;
}

// ── Global interface scale ──────────────────────────────────────────────────
// Per-window zoom only affects a panel's contents; the menu bar, tab bars,
// buttons and all the spacing around them come from the style and the default
// font, so they stayed put no matter how far a panel was zoomed. Applying the
// scale to both makes the whole debugger grow together.
//
// The style is scaled from a pristine copy each time rather than repeatedly, so
// the metrics never drift, and the font is re-selected from the ladder rather
// than stretched, so it stays sharp.
static ImGuiStyle s_style_base;
static bool       s_style_base_valid = false;
static float      s_applied_ui_scale = -1.0f;

static void
debug_ui_apply_ui_scale(void)
{
    if (!s_style_base_valid) {
        return;
    }
    float want = debug_ui_settings().ui_scale;
    if (want < 0.5f) want = 0.5f;
    if (want > 4.0f) want = 4.0f;
    if (want == s_applied_ui_scale) {
        return;
    }
    s_applied_ui_scale = want;

    ImGuiStyle &style = ImGui::GetStyle();
    style             = s_style_base;   // undo any previous scaling
    style.ScaleAllSizes(want);

    // Default font for everything that doesn't opt into per-window zoom: the
    // menu bar, tab bars, popups and every panel that just uses the default.
    ImGui::GetIO().FontDefault = debug_ui_font_for_size(s_font_sizes[0] * want, nullptr);
}

// Everything that is independent of which renderer backend draws the debugger:
// the ImGui context, settings, fonts, style and the first-run layout decision.
// Both debug_ui_init and debug_ui_init_dx12 run this before attaching their
// platform/renderer backends.
// Case-insensitive match of `name` against a comma-separated list.
static bool
bench_list_has(const std::string &list, const char *name)
{
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        const std::string item = list.substr(start, end - start);
        if (!item.empty() && item.size() == strlen(name)) {
            bool same = true;
            for (size_t i = 0; i < item.size() && same; i++) {
                same = tolower((unsigned char)item[i]) == tolower((unsigned char)name[i]);
            }
            if (same) {
                return true;
            }
        }
        start = end + 1;
    }
    return false;
}

static void
debug_ui_apply_bench_config(void)
{
    if (s_bench_have_panels) {
        const bool all = bench_list_has(s_bench_panels, "all");
        for (DebugPanel &panel : debug_ui_panels()) {
            panel.open = all || bench_list_has(s_bench_panels, panel.name);
        }
    }
    if (s_bench_no_ini) {
        // No ini means default settings; force the ones a benchmark depends on.
        DebugUiSettings &s = debug_ui_settings();
        s.io_trace_capture = s_bench_io_capture;
        s.perf_always_on   = false;
        // Every device defaults off, which would leave the Activity log empty
        // and measure nothing. These are the steady-rate ones; VERA is left
        // out, as the panel itself advises, because it would be the only
        // thing in the ring.
        s.io_cap_via   = s_bench_io_capture;
        s.io_cap_ym    = s_bench_io_capture;
        s.io_cap_i2c   = s_bench_io_capture;
        s.io_cap_joy   = s_bench_io_capture;
        s.io_cap_sd    = s_bench_io_capture;
        s.io_cap_files = s_bench_io_capture;
    }
}

extern "C" void
debug_ui_bench_configure(const char *panels, bool io_capture, bool no_ini)
{
    s_bench_have_panels = panels != nullptr;
    s_bench_panels      = panels ? panels : "";
    s_bench_io_capture  = io_capture;
    s_bench_no_ini      = no_ini;
}

static void
debug_ui_create_context(SDL_Window *win)
{
    s_window = win;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // dockable panels
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // keyboard nav

    if (s_bench_no_ini) {
        io.IniFilename = nullptr;
    }

    // Repair a corrupted saved table setting before ImGui's own auto-load
    // (inside the first NewFrame(), further down) ever reads it.
    debug_ui_sanitize_ini_file(io.IniFilename);

    // Our own imgui.ini section for user preferences. Must be installed before
    // ImGui loads the ini (i.e. before the first NewFrame) or the saved values
    // are discarded as unknown.
    debug_ui_settings_register();

    // Fonts for per-window zoom.
    //
    // SetWindowFontScale() stretches the glyph bitmaps that were rasterised at
    // one size, which is why zoomed text used to smear. Instead, rasterise the
    // font at a ladder of real sizes up front and let the zoom helper pick the
    // nearest one, so every zoom level is drawn at its native resolution.
    // Snapping to these sizes means there is never any resampling.
    debug_ui_build_fonts(io);

    ImGui::StyleColorsDark();

    // Tabs: the stock dark theme distinguishes selected from unselected tabs
    // only by a blue vs. slightly-lighter-blue fill, which is hard to read at a
    // glance — especially with several source-file tabs open. Give the selected
    // tab a much brighter, warmer fill with a bold accent overline, and push the
    // unselected tabs down to a dark neutral grey so the contrast is obvious.
    // Set on the style (not per-panel) so EVERY tab bar in the debugger matches.
    {
        ImGuiStyle &st = ImGui::GetStyle();
        const ImVec4 sel      (0.16f, 0.48f, 0.88f, 1.00f); // selected, focused
        const ImVec4 selDim   (0.17f, 0.42f, 0.72f, 1.00f); // selected, unfocused bar
        const ImVec4 idle     (0.16f, 0.17f, 0.19f, 1.00f); // unselected
        const ImVec4 idleDim  (0.12f, 0.13f, 0.14f, 1.00f); // unselected, unfocused bar
        const ImVec4 hover    (0.30f, 0.58f, 0.95f, 1.00f);
        const ImVec4 overline (1.00f, 0.72f, 0.25f, 1.00f); // amber accent bar

        st.Colors[ImGuiCol_Tab]                        = idle;
        st.Colors[ImGuiCol_TabHovered]                 = hover;
        st.Colors[ImGuiCol_TabSelected]                = sel;
        st.Colors[ImGuiCol_TabSelectedOverline]        = overline;
        st.Colors[ImGuiCol_TabDimmed]                  = idleDim;
        st.Colors[ImGuiCol_TabDimmedSelected]          = selDim;
        st.Colors[ImGuiCol_TabDimmedSelectedOverline]  = overline;
        st.TabBarOverlineSize = 3.0f;  // thicker accent bar on the selected tab
        st.TabRounding        = 3.0f;
    }

    // Remember the unscaled style: the interface-scale setting derives from
    // this each time it changes, so repeated changes can't compound and drift.
    s_style_base       = ImGui::GetStyle();
    s_style_base_valid = true;
    s_applied_ui_scale = -1.0f; // force the first apply

    // Detect first run (no imgui.ini yet) so we install a sensible default dock
    // layout once. After that ImGui persists the user's own arrangement and we
    // leave it alone.
    s_build_default_layout = true;
    if (const char *ini = io.IniFilename) {
        if (FILE *f = fopen(ini, "rb")) {
            fclose(f);
            s_build_default_layout = false;
        }
    }

    debug_ui_apply_bench_config();
}

extern "C" bool
debug_ui_init(SDL_Window *win, SDL_Renderer *ren)
{
    if (s_initialized) {
        return true;
    }
    if (win == nullptr || ren == nullptr) {
        return false;
    }

    debug_ui_create_context(win);
    s_renderer = ren;

    if (!ImGui_ImplSDL2_InitForSDLRenderer(win, ren)) {
        fprintf(stderr, "Warning: ImGui_ImplSDL2_InitForSDLRenderer failed.\n");
        ImGui::DestroyContext();
        s_window   = nullptr;
        s_renderer = nullptr;
        return false;
    }
    if (!ImGui_ImplSDLRenderer2_Init(ren)) {
        fprintf(stderr, "Warning: ImGui_ImplSDLRenderer2_Init failed.\n");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        s_window   = nullptr;
        s_renderer = nullptr;
        return false;
    }

    // Panel images become SDL_Textures on this renderer.
    debug_ui_image_use_sdl(ren);

    s_native_dx12 = false;
    s_initialized = true;
    s_enabled     = true;
    return true;
}

extern "C" bool
debug_ui_init_dx12(SDL_Window *win, render_backend_t *backend)
{
#ifdef HAS_DX12
    if (s_initialized) {
        return true;
    }
    if (win == nullptr || backend == nullptr) {
        return false;
    }

    debug_ui_create_context(win);

    // SDL still owns the window and the event stream; only the drawing moves to
    // D3D12, so this is the plain "some D3D renderer" platform init.
    if (!ImGui_ImplSDL2_InitForD3D(win)) {
        fprintf(stderr, "Warning: ImGui_ImplSDL2_InitForD3D failed.\n");
        ImGui::DestroyContext();
        s_window = nullptr;
        return false;
    }
    if (!debug_ui_dx12_init(win, backend)) {
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        s_window = nullptr;
        return false;
    }

    // Panel images become RGBA8888 textures on the shared backend, so their
    // SRVs land in the same shader-visible heap the debugger binds.
    debug_ui_image_use_dx12(backend);

    s_renderer    = nullptr;
    s_native_dx12 = true;
    s_initialized = true;
    s_enabled     = true;
    return true;
#else
    (void)win;
    (void)backend;
    return false;
#endif
}

extern "C" void
debug_ui_process_event(const SDL_Event *ev)
{
    if (!s_initialized || ev == nullptr) {
        return;
    }
    ImGui_ImplSDL2_ProcessEvent(ev);
}

// Menu-bar text fields that always occupy the same width, so values that change
// every frame (run state, PC, cycle and instruction counters) don't shove
// everything after them sideways as they grow and shrink. `widest` is a
// template string sized to the largest value the field can hold.
static void
menubar_fixed_advance(const char *widest, float start_x)
{
    const float w = ImGui::CalcTextSize(widest).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(start_x + w + ImGui::GetStyle().ItemSpacing.x);
}

static void
menubar_fixed_value(const char *widest, const char *fmt, ...)
{
    char buf[64] = "";
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);

    const float x = ImGui::GetCursorPosX();
    ImGui::TextUnformatted(buf);
    menubar_fixed_advance(widest, x);
}

static void
menubar_fixed_colored(const char *widest, ImVec4 color, const char *text)
{
    const float x = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    menubar_fixed_advance(widest, x);
}

// Execution-control button. The shortcut belongs in the tooltip because these
// are the actions people reach for constantly, and there is nowhere else in the
// UI that lists the keys.
//
// The tooltip deliberately survives the disabled state: these buttons are greyed
// out most of the time (stepping needs a halted CPU, Pause needs a running one),
// and "why can't I click this?" is exactly the question worth answering. Plain
// SetItemTooltip() would say nothing there, since ImGui does not consider a
// disabled item hovered unless asked.
static bool
ctrl_button(const char *label, bool enabled, const char *tip)
{
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tip);
    }
    return clicked;
}

// Capture state in the menu bar. While one records, this and the rest of the
// bar are all the debugger draws, so Stop has to live here.
static void
draw_capture_status(void)
{
    const cap_state_t st = cap_state();
    if (st == CAP_RUNNING) {
        ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.0f), "CAPTURING %u frames, %.1f s",
                           cap_frames(), cap_elapsed_s());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop"))
            cap_stop();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("End the capture. The report appears in the Profile panel.");
    } else if (st == CAP_ARMED) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.00f, 1.0f), "capture armed ($F8)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            cap_stop();
    }
}

// Host time per emulated frame, smoothed, with the breakdown on hover. This is
// the "is the debugger slowing the machine down" number: once busy time passes
// the frame period, the machine cannot keep real speed. Reads only the last
// closed frame and the panel table, so it costs nothing worth timing.
static void
draw_host_time(void)
{
    static float s_busy_ms = 0.0f;
    uint32_t     last[HP_SECTIONS], wall_us = 0;
    host_prof_last_frame(last, &wall_us);
    const float busy_ms = (float)(wall_us - last[HP_SLEEP]) / 1000.0f;
    s_busy_ms = s_busy_ms == 0.0f ? busy_ms : s_busy_ms * 0.9f + busy_ms * 0.1f;

    // One VGA frame at native speed, as the emulator models it:
    // 800 * 525 pixels at 25 MHz.
    const float frame_ms = 16.8f;
    const ImVec4 col     = s_busy_ms > frame_ms        ? ImVec4(1.00f, 0.45f, 0.45f, 1.0f)
                         : s_busy_ms > frame_ms * 0.75f ? ImVec4(0.95f, 0.70f, 0.30f, 1.0f)
                                                        : ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    char text[32];
    snprintf(text, sizeof text, "host %.1f ms", (double)s_busy_ms);
    menubar_fixed_colored("host 888.8 ms", col, text);
    if (!ImGui::IsItemHovered()) {
        return;
    }

    ImGui::BeginTooltip();
    ImGui::TextUnformatted("Host time per emulated frame (smoothed), excluding sleep.");
    ImGui::TextDisabled("Above %.1f ms the machine cannot hold real speed. Guest cycle\n"
                        "counts are never affected; only host speed is.",
                        (double)frame_ms);
    ImGui::Separator();
    ImGui::TextUnformatted("Last frame:");
    for (int i = 0; i < HP_SECTIONS; i++) {
        ImGui::Text("  %-9s %8.2f ms", host_prof_section_name((hp_section_t)i),
                    (double)last[i] / 1000.0);
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Panels (recent mean per render):");
    for (int i = 0; i < host_prof_panel_count(); i++) {
        host_prof_panel_t p;
        if (host_prof_panel_get(i, &p) && p.calls) {
            ImGui::Text("  %-12s %8.2f ms", p.name, p.recent_us / 1000.0);
        }
    }
    ImGui::EndTooltip();
}

// Top main menu bar: View/Layout/Help menus plus the execution-control toolbar
// (Continue/Pause/Step*, a run-state indicator, and the live PC:bank). Buttons
// drive the shared debugger execution control (debugger.c) — the same
// transitions F5/F10/F11 and DAP use.
static void
debug_ui_draw_control_bar(void)
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    // View: toggle each registered panel. This is the ONLY way to reopen a
    // panel after its window has been closed (the [x]), so it doubles as the
    // panel visibility manager. Iterates the self-registered registry, so new
    // panels appear here automatically.
    if (ImGui::BeginMenu("View")) {
        for (DebugPanel &panel : debug_ui_panels()) {
            ImGui::MenuItem(panel.name, nullptr, &panel.open);
        }
        ImGui::EndMenu();
    }

    // Layout: rebuild the default dock arrangement on demand (recovers from a
    // messy imgui.ini or a panel dragged somewhere unhelpful).
    if (ImGui::BeginMenu("Layout")) {
        if (ImGui::MenuItem("Reset to Default Layout")) {
            for (DebugPanel &panel : debug_ui_panels()) {
                panel.open = true;             // reopen everything so the reset is visible
            }
            s_build_default_layout = true;     // applied after DockSpaceOverViewport() this frame
        }
        ImGui::EndMenu();
    }

    // System: machine-level controls. Reset defers to the emulator loop via the
    // SMC reset flag (safe, == Ctrl-R). IRQ/NMI are edge-triggered.
    if (ImGui::BeginMenu("System")) {
        if (ImGui::MenuItem("Reset", "Ctrl+Shift+F5")) {
            smc_requested_reset = true;
        }
        ImGui::SetItemTooltip("Reset the machine, as the SMC would. Breakpoints are kept.");
        if (ImGui::MenuItem("Trigger IRQ")) {
            irq6502();
        }
        if (ImGui::MenuItem("Trigger NMI")) {
            machine_nmi();
        }
        ImGui::Separator();

        // Behavioural toggles live in Settings, not scattered through menus.
        if (ImGui::MenuItem("Settings...", nullptr, debug_ui_settings_is_open())) {
            debug_ui_open_settings(nullptr);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Every preference, by group: appearance, stepping, source folders,\n"
                              "memory, audio, the I/O trace and the frame budget.");
        }
        ImGui::EndMenu();
    }

    // Help: the vendored ImGui demo is handy while building/inspecting panels.
    if (ImGui::BeginMenu("Help")) {
        // Not every shortcut has a button - F9 and Ctrl+F10 in particular are
        // keyboard-only - so keep the full list somewhere findable.
        if (ImGui::BeginMenu("Keyboard Shortcuts")) {
            static const struct { const char *key, *what; } keys[] = {
                { "F5",            "Continue" },
                { "Break",         "Pause" },
                { "F11",           "Step Into" },
                { "Shift+F11",     "Step Out" },
                { "F10",           "Step Over" },
                { "Ctrl+F10",      "Run to cursor" },
                { "F9",            "Toggle breakpoint at PC" },
                { "Ctrl+Shift+F5", "Reset machine" },
                { "Ctrl+wheel",    "Zoom the panel under the cursor" },
            };
            if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit)) {
                for (const auto &k : keys) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(k.key);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", k.what);
                }
                ImGui::EndTable();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Show ImGui Demo", nullptr, &s_show_demo);
        ImGui::Separator();
        ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
        ImGui::EndMenu();
    }

    ImGui::Separator();

    const bool running = DEBUGIsRunning();
    const bool paused  = DEBUGIsPaused();

    if (ctrl_button("Continue", paused,
                    "Continue  (F5)\n"
                    "Run until the next breakpoint or pause.\n"
                    "Only while the machine is halted."))
        DEBUGContinue();

    ImGui::SameLine();
    if (ctrl_button("Pause", running,
                    "Pause  (Break key)\n"
                    "Halt the machine wherever it is.\n"
                    "Only while it is running."))
        DEBUGPause();

    ImGui::SameLine();
    if (ctrl_button("Step Into", paused,
                    "Step Into  (F11)\n"
                    "Execute one source line, entering any call. Without C debug\n"
                    "info, or with \"Step by line\" off, one instruction.\n"
                    "Only while the machine is halted."))
        DEBUGStepIntoAuto();
    ImGui::SameLine();
    if (ctrl_button("Step Over", paused,
                    "Step Over  (F10)\n"
                    "Execute one source line, running any call to completion.\n"
                    "Without C debug info, or with \"Step by line\" off, one\n"
                    "instruction. Ctrl+F10 runs to the cursor instead.\n"
                    "Only while the machine is halted."))
        DEBUGStepOverAuto(DEBUG_OWNER_UI);
    ImGui::SameLine();
    if (ctrl_button("Step Out", paused,
                    "Step Out  (Shift+F11)\n"
                    "Run until the current subroutine returns.\n"
                    "Only while the machine is halted."))
        DEBUGStepOut(DEBUG_OWNER_UI);

    // ── Speed ───────────────────────────────────────────────────────────────
    // Absolute target clock, not a bare percentage: the machine's own clock is
    // whatever -mhz selected at startup, so a percentage alone doesn't say how
    // fast anything is actually running. Warp disables the throttle entirely,
    // so the speed buttons are meaningless while it is on.
    ImGui::Separator();
    {
        const int khz    = timing_get_speed_khz();
        const int native = timing_native_khz();

        ImGui::BeginDisabled(warp_mode);
        if (ImGui::SmallButton("-")) {
            timing_step_speed(-1);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slow emulation down");
        ImGui::SameLine();

        // Fixed width: this text changes constantly and anything after it would
        // otherwise shuffle sideways every frame.
        char speed_text[48];
        if (warp_mode) {
            snprintf(speed_text, sizeof speed_text, "warp");
        } else if (khz >= 1000) {
            snprintf(speed_text, sizeof speed_text, "%.2fMHz", khz / 1000.0);
        } else {
            snprintf(speed_text, sizeof speed_text, "%dkHz", khz);
        }
        const bool at_native = (khz == native);
        const bool too_fast  = (khz > native);
        // Blue below the machine's clock, red above it: running faster than the
        // hardware ever does is a legitimate thing to want, but the timing you
        // observe there is not what the real machine would do, so say so.
        const ImVec4 col = warp_mode   ? ImVec4(0.95f, 0.70f, 0.30f, 1.0f)
                           : too_fast  ? ImVec4(1.00f, 0.45f, 0.45f, 1.0f)
                           : at_native ? ImGui::GetStyle().Colors[ImGuiCol_Text]
                                       : ImVec4(0.55f, 0.85f, 1.00f, 1.0f);
        menubar_fixed_colored("888.88MHz", col, speed_text);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Target CPU clock. This machine was started at %.2f MHz (-mhz %d),\n"
                "which is 100%%; the current target is %d%% of that.%s\n\n"
                "%.0f cycles/s, roughly %.0f instructions/s (a 6502-family\n"
                "instruction averages about 4 cycles).\n\n"
                "+/- step through real hardware clocks and the rates between them.",
                native / 1000.0, (int)MHZ, timing_get_speed_percent(),
                too_fast ? "\n\nFaster than this machine's clock: timing you measure here\n"
                           "is NOT what the real hardware would do."
                         : "",
                khz * 1000.0, khz * 1000.0 / 4.0);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) {
            timing_step_speed(1);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Speed emulation up");

        // Only worth offering when it would actually change something.
        if (!at_native) {
            ImGui::SameLine();
            if (ImGui::SmallButton("1x")) {
                timing_reset_speed();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Back to this machine's own clock (%.2f MHz).",
                                  native / 1000.0);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        const bool warp_was_enabled = warp_mode;
        if (warp_was_enabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.45f, 0.15f, 1.0f));
        }
        if (ImGui::SmallButton("Warp")) {
            machine_toggle_warp();
        }
        if (warp_was_enabled) {
            ImGui::PopStyleColor(2);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Run with no speed limit (currently %s)", warp_mode ? "ON" : "off");
        }
    }

    // ── Status ──────────────────────────────────────────────────────────────
    // Everything from here on changes every frame while stepping, so each field
    // reserves the width of its widest value. Without that the whole right-hand
    // side of the bar jitters as the numbers grow and shrink.
    ImGui::Separator();
    if (running) {
        menubar_fixed_colored("RUNNING", ImVec4(0.40f, 0.85f, 0.40f, 1.0f), "RUNNING");
    } else if (paused) {
        menubar_fixed_colored("RUNNING", ImVec4(0.95f, 0.70f, 0.30f, 1.0f), "PAUSED");
    } else {
        menubar_fixed_value("RUNNING", "%s", "-");
    }

    // Interrupt context: make it obvious when the PC is inside a handler, since
    // otherwise stepping through interrupt code looks identical to main code.
    ImGui::SameLine();
    if (cpu_in_interrupt()) {
        char irq_text[16];
        if (cpu_irq_depth() > 1)
            snprintf(irq_text, sizeof irq_text, "IRQ x%d", cpu_irq_depth());
        else
            snprintf(irq_text, sizeof irq_text, "IRQ");
        menubar_fixed_colored("IRQ x88", ImVec4(1.00f, 0.45f, 0.45f, 1.0f), irq_text);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Executing inside an interrupt handler (depth %d).\n"
                              "Returns to $%04X.",
                              cpu_irq_depth(), (unsigned)cpu_irq_return_pc());
        }
    } else {
        menubar_fixed_value("IRQ x88", "%s", "");
    }

    ImGui::Separator();
    menubar_fixed_value("PC 88:8888", "PC %02X:%04X", (unsigned)regs.k, (unsigned)regs.pc);

    ImGui::Separator();
    menubar_fixed_value("cyc 8888888888", "cyc %u", (unsigned)clockticks6502);
    ImGui::SameLine();
    menubar_fixed_value("ins 8888888888", "ins %d", instruction_counter);

    ImGui::Separator();
    draw_host_time();

    if (cap_state() == CAP_RUNNING || cap_state() == CAP_ARMED) {
        ImGui::Separator();
        draw_capture_status();
    }

    ImGui::EndMainMenuBar();
}

// ── Default panel placement ─────────────────────────────────────────────────
// One table drives BOTH the first-run dock layout and the placement of panels
// that are new to an *existing* imgui.ini. When you add a panel to the registry,
// add it here too; anything not listed just floats.
enum DebugDockSlot { DOCK_CENTER = 0, DOCK_RIGHT, DOCK_BOTTOM, DOCK_SLOT_COUNT };

struct DebugPanelPlacement {
    const char   *window; // MUST match the panel's ImGui::Begin() title
    DebugDockSlot slot;
};

static const DebugPanelPlacement s_placement[] = {
    {"Disassembly",  DOCK_CENTER},
    {"Source",       DOCK_CENTER},
    {"CPU",          DOCK_RIGHT},
    {"Breakpoints",  DOCK_RIGHT},
    {"Symbols",      DOCK_RIGHT},
    {"Call Stack",   DOCK_RIGHT},
    {"Memory",       DOCK_BOTTOM},
    {"VERA",         DOCK_BOTTOM},
    {"PSG",          DOCK_BOTTOM},
    {"YM2151",       DOCK_BOTTOM},
    {"PCM",          DOCK_BOTTOM},
    {"Performance",  DOCK_BOTTOM},
    // Added last in this slot so Memory stays the tab the bottom group opens
    // on -- the loop below takes the first entry per slot as that slot's front.
    {"I/O",          DOCK_BOTTOM},
    {"Profile",      DOCK_BOTTOM},
};

// A representative, long-established window for each slot. Used to find the
// slot's *live* dock node: the node ids minted by debug_ui_build_default_layout
// only exist on a fresh run, whereas a returning user's nodes come from
// imgui.ini and can be anything.
static const char *s_slot_anchor[DOCK_SLOT_COUNT] = { "Disassembly", "CPU", "Memory" };

// Resolve a slot to the dock node it currently occupies, or 0 if unknown.
static ImGuiID
debug_ui_slot_node(DebugDockSlot slot)
{
    const char *anchor = s_slot_anchor[slot];
    if (ImGuiWindow *w = ImGui::FindWindowByName(anchor)) {
        if (w->DockId != 0) {
            return w->DockId;
        }
    }
    // The anchor may not have been submitted yet this frame; its saved settings
    // still carry the node id.
    if (ImGuiWindowSettings *s = ImGui::FindWindowSettingsByID(ImHashStr(anchor))) {
        return s->DockId;
    }
    return 0;
}

// The tab id ImGui will mint for a window, without needing the window to exist.
// ImGuiWindow's constructor does TabId = GetID("#TAB") seeded with the window
// id (imgui.cpp), and on a first run no panel has been submitted yet at the
// point the default layout is built.
static ImGuiID
debug_ui_window_tab_id(const char *window)
{
    if (ImGuiWindow *w = ImGui::FindWindowByName(window)) {
        return w->TabId;
    }
    return ImHashStr("#TAB", 0, ImHashStr(window));
}

// Give a panel its intended home, but ONLY when imgui.ini has no entry for it.
//
// This is the fix for "I updated and the new panels were all floating": a panel
// added after the user's layout was saved has no saved dock id, so ImGui floats
// it and the user has to dock it by hand. ImGuiCond_FirstUseEver is exactly the
// right guard — ImGui clears that condition for any window it loaded settings
// for (see CreateNewWindow), so an existing arrangement is never touched, and a
// panel the user deliberately floated stays floating.
static void
debug_ui_place_new_panel(const char *window)
{
    for (const DebugPanelPlacement &p : s_placement) {
        if (strcmp(p.window, window) == 0) {
            if (ImGuiID node = debug_ui_slot_node(p.slot)) {
                ImGui::SetNextWindowDockID(node, ImGuiCond_FirstUseEver);
            }
            return;
        }
    }
}

// First-run only (or on Layout → Reset): arrange the panels into a usable
// layout instead of a pile of overlapping windows. Disassembly + Source tabbed
// in the center, CPU + Breakpoints tabbed on the right, Memory + VERA + the
// audio panels tabbed across the bottom.
//
// Which tab each group *opens on* is set explicitly at the end. Left alone,
// ImGui selects whichever tab was added last (DockNodeUpdateTabBar), and that
// order is an accident of static registration order across the panel
// translation units — which is why a fresh layout came up showing PCM at the
// bottom and Call Stack on the right rather than Memory and CPU.
static void
debug_ui_build_default_layout(ImGuiID dockspace_id)
{
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspace_id;
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.34f, nullptr, &center);

    const ImGuiID node[DOCK_SLOT_COUNT] = { center, right, bottom };
    for (const DebugPanelPlacement &p : s_placement) {
        ImGui::DockBuilderDockWindow(p.window, node[p.slot]);
    }
    ImGui::DockBuilderFinish(dockspace_id);

    // The first panel listed for a slot is the one that slot opens on.
    const char *front[DOCK_SLOT_COUNT] = {};
    for (const DebugPanelPlacement &p : s_placement) {
        if (front[p.slot] != nullptr) {
            continue;
        }
        front[p.slot] = p.window;
        if (ImGuiDockNode *n = ImGui::DockBuilderGetNode(node[p.slot])) {
            n->SelectedTabId = debug_ui_window_tab_id(p.window);
        }
    }

    // On a reset the previously focused panel is still the nav window, and
    // DockNodeUpdateTabBar hands its node's selection back to it — which would
    // undo the choice above for whichever group it sits in. Focusing the center
    // panel keeps that override on the one node whose default it agrees with.
    if (front[DOCK_CENTER]) {
        ImGui::SetWindowFocus(front[DOCK_CENTER]);
    }
}

extern "C" bool
debug_ui_render(void)
{
    if (!s_initialized) {
        return false;
    }

    // While a capture records, the debugger goes quiet: it rebuilds its frame
    // at most every CAPTURE_UI_INTERVAL_MS, and then only the menu bar -- the
    // capture status and its Stop button. Every panel costs the emulator's own
    // thread to draw, and a capture exists to measure the machine without that.
    const bool quiet = cap_running();
    if (quiet) {
        static Uint32 last_quiet = 0;
        const Uint32  now        = SDL_GetTicks();
        if (now - last_quiet < CAPTURE_UI_INTERVAL_MS) {
            return true;
        }
        last_quiet = now;
    }

    // Interface scale is read from settings and applied to the style + default
    // font. Must happen before NewFrame, which latches the current font.
    debug_ui_apply_ui_scale();

#ifdef HAS_DX12
    if (s_native_dx12) {
        ImGui_ImplDX12_NewFrame();
    } else
#endif
    {
        ImGui_ImplSDLRenderer2_NewFrame();
    }
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Publish any cross-panel goto requested last frame so every panel sees it.
    debug_ui_latch_goto();

    // Point the user at wherever execution actually stopped (Source vs
    // Disassembly), before panels render so the focus applies this frame.
    debug_ui_follow_stop_location();

    // Execution-control bar first: it lives in the main menu bar, which shrinks
    // the viewport work area so the dock space sits neatly below it.
    debug_ui_draw_control_bar();

    // Full-window dock space so every panel is dockable/draggable. On first run
    // (no imgui.ini) install a default layout; afterwards ImGui restores the
    // user's saved arrangement.
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();
    if (s_build_default_layout) {
        s_build_default_layout = false;
        debug_ui_build_default_layout(dockspace_id);
    }

    // Iterate the self-registered panel registry. No central list to edit:
    // each panel added a file-static DebugPanelRegistration (see
    // debug_ui_panels.h). Panels missing from imgui.ini get docked into their
    // intended slot first, so a newly added panel never turns up floating.
    // Recorder settings follow the preference, not whichever panel is drawn.
    debug_ui_apply_io_capture();

    for (DebugPanel &panel : debug_ui_panels()) {
        if (!quiet && panel.open && panel.render) {
            debug_ui_place_new_panel(panel.name);
            const uint64_t t0 = host_prof_now();
            panel.render(&panel.open);
            host_prof_panel(panel.name, host_prof_now() - t0);
        }
    }

    // After the panels, so it knows whether Performance was drawn this frame.
    // During a capture it was not, so the panel's own recorders stand down; a
    // capture that wants them arms them under its own owner.
    debug_ui_perf_recorders_tick();

    if (!quiet) {
        // Optional vendored demo window (Help → Show ImGui Demo).
        if (s_show_demo) {
            ImGui::ShowDemoWindow(&s_show_demo);
        }

        // User preferences (System → Settings...).
        debug_ui_draw_settings_window();
    }

    ImGui::Render();

    // From here on it is turning the frame into GPU work and presenting it.
    // The caller has ui_build open; this nests inside it.
    host_prof_ui_frame();
    host_prof_begin(HP_UI_DRAW);

#ifdef HAS_DX12
    if (s_native_dx12) {
        // A failure here is the secondary swap chain's, not the emulator's:
        // the caller drops this window back to the SDLRenderer2 path.
        const bool ok = debug_ui_dx12_render();
        host_prof_end(HP_UI_DRAW);
        return ok;
    }
#endif

    ImGuiIO &io = ImGui::GetIO();
    SDL_RenderSetScale(s_renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(s_renderer, 24, 24, 24, 255);
    SDL_RenderClear(s_renderer);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_renderer);
    SDL_RenderPresent(s_renderer);
    host_prof_end(HP_UI_DRAW);
    return true;
}

extern "C" void
debug_ui_shutdown(void)
{
    if (!s_initialized) {
        return;
    }
    // Panel textures first: in the native mode they live on the shared DX12
    // backend, and their descriptors have to go back before anything that
    // shares that heap is torn down.
    debug_ui_image_release_all();
#ifdef HAS_DX12
    if (s_native_dx12) {
        debug_ui_dx12_shutdown();
    } else
#endif
    {
        ImGui_ImplSDLRenderer2_Shutdown();
    }
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    s_window      = nullptr;
    s_renderer    = nullptr;
    s_native_dx12 = false;
    s_initialized = false;
    s_enabled     = false;
}

extern "C" bool
debug_ui_is_enabled(void)
{
    return s_enabled;
}

extern "C" void
debug_ui_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

extern "C" SDL_Renderer *
debug_ui_get_renderer(void)
{
    return s_renderer;
}

extern "C" bool
debug_ui_want_capture_keyboard(void)
{
    if (!s_initialized) {
        return false;
    }
    return ImGui::GetIO().WantCaptureKeyboard;
}

// True only while an ImGui text field is actively accepting text (an InputText
// is focused). Unlike WantCaptureKeyboard — which is true whenever the debugger
// window merely has keyboard-nav focus — this stays false when the user is just
// looking at panels, so the F5/F9/F10/F11 debug shortcuts (which ImGui never
// uses for text or nav) still fire. Only genuine text entry (goto/search boxes)
// suppresses them.
extern "C" bool
debug_ui_want_text_input(void)
{
    if (!s_initialized) {
        return false;
    }
    return ImGui::GetIO().WantTextInput;
}

// ── Run-to-cursor + PC breakpoint toggle (keyboard shortcuts) ───────────────
// The Disassembly panel publishes the instruction under the mouse here every
// frame; the Ctrl+F10 shortcut consumes it. These live here (not in a panel) so
// the C event pump in video.c can reach them through the plain C ABI, and so
// they share the same breakpoint/run-to plumbing every panel uses (the bridge).
static uint16_t s_cursor_addr  = 0;
static uint8_t  s_cursor_bank  = 0;
static bool     s_cursor_valid = false;

extern "C" void
debug_ui_set_cursor(uint16_t addr, uint8_t bank, bool valid)
{
    s_cursor_addr  = addr;
    s_cursor_bank  = bank;
    s_cursor_valid = valid;
}

extern "C" bool
debug_ui_run_to_cursor(void)
{
    if (!s_cursor_valid || !DEBUGIsPaused()) {
        return false;
    }
    DEBUGRunTo(s_cursor_addr, s_cursor_bank, DEBUG_OWNER_UI);
    return true;
}

extern "C" void
debug_ui_toggle_breakpoint_at_pc(void)
{
    // Toggle: remove one at (pc,bank,x16Bank) if present, else add one there.
    // The bank has to be worked out before the remove, not just for the add:
    // debug_bp_find() matches x16Bank exactly, so removing with a different
    // selector than the one it was stored under finds nothing.
    int x16 = -1;
    // Match getCurrentBank()/x16bank_for(): a banked window only applies in
    // program bank 0 (getCurrentBank returns -1 for any k != 0), so guarding
    // on regs.k == 0 keeps the breakpoint matchable instead of dead.
    if (regs.pc >= 0xA000 && regs.k == 0) {
        x16 = (regs.pc < 0xC000) ? (int)memory_get_ram_bank()
                                 : (int)memory_get_rom_bank();
    }
    // The core's own toggle, so this window's F9 behaves exactly like the SDL
    // debugger's: delete whatever is there whoever asked for it, or create one
    // owned by the UI. Deleting only the UI's claim would leave a breakpoint a
    // DAP client also wanted armed, and the key looking broken.
    debug_bp_toggle_for((int)regs.pc, regs.k, x16, DEBUG_OWNER_UI);
}

// ── Cross-panel "goto" service ──────────────────────────────────────────────
// A one-shot navigation target published by e.g. the Symbols or Call Stack
// panels (debug_ui_request_goto) and consumed by the Disassembly + Memory
// panels (debug_ui_peek_goto). A request made during frame N is latched at the
// top of frame N+1 and stays visible to EVERY panel for that whole frame, so
// panel render order doesn't matter.
static uint16_t s_goto_req_addr    = 0;
static uint8_t  s_goto_req_bank    = 0;
static bool     s_goto_req_pending = false;
static uint16_t s_goto_addr        = 0;
static uint8_t  s_goto_bank        = 0;
static bool     s_goto_active      = false;

extern "C" void
debug_ui_request_goto(uint16_t addr, uint8_t bank)
{
    s_goto_req_addr    = addr;
    s_goto_req_bank    = bank;
    s_goto_req_pending = true;
}

extern "C" bool
debug_ui_peek_goto(uint16_t *addr, uint8_t *bank)
{
    if (!s_goto_active) {
        return false;
    }
    if (addr) *addr = s_goto_addr;
    if (bank) *bank = s_goto_bank;
    return true;
}

// Latch a pending goto request into the frame-visible slot. Called once at the
// top of debug_ui_render before any panel runs.
static void
debug_ui_latch_goto(void)
{
    s_goto_active = s_goto_req_pending;
    if (s_goto_req_pending) {
        s_goto_addr        = s_goto_req_addr;
        s_goto_bank        = s_goto_req_bank;
        s_goto_req_pending = false;
    }
}

// ── Follow the stop location between Source and Disassembly ─────────────────
// Breaking somewhere the debug info doesn't cover (KERNAL ROM, a trampoline, an
// interrupt handler) leaves the Source panel showing an unrelated file while the
// PC is elsewhere entirely. Bring the panel that can actually show the code to
// the front instead.
//
// Only acts when the stop location changes, so it never fights a tab the user
// deliberately selected, and only while paused, so it doesn't thrash on every
// frame of a free run.
static void
debug_ui_follow_stop_location(void)
{
    const DebugUiSettings &st = debug_ui_settings();
    if (!st.auto_switch_disasm && !st.auto_switch_source) {
        return;
    }

    static bool     had_stop = false;
    static uint16_t last_pc  = 0;
    static uint8_t  last_k   = 0;

    if (!DEBUGIsPaused()) {
        had_stop = false; // re-arm: the next stop is a fresh location
        return;
    }
    if (had_stop && regs.pc == last_pc && regs.k == last_k) {
        return;
    }
    had_stop = true;
    last_pc  = regs.pc;
    last_k   = regs.k;

    const char *file = nullptr;
    int         line = 0;
    const bool  has_source =
        (regs.k == 0) &&
        dbg_info_addr_to_source_banked(regs.pc, (int)memory_get_ram_bank(), &file, &line) &&
        file != nullptr;

    if (has_source) {
        if (st.auto_switch_source) {
            ImGui::SetWindowFocus("Source");
        }
    } else {
        if (st.auto_switch_disasm) {
            ImGui::SetWindowFocus("Disassembly");
        }
    }
}

// ── Cross-panel "add to watch" ──────────────────────────────────────────────
// The watch list lives in the CPU panel, but the natural place to ask for a
// watch is wherever you found the address. Same one-frame latch as the goto
// request above so panel render order doesn't matter.
static uint16_t s_watch_req_addr    = 0;
static int16_t  s_watch_req_bank    = -1;
static uint16_t s_watch_req_len     = 1;
static bool     s_watch_req_pending = false;

extern "C" void
debug_ui_request_watch(uint16_t addr, int16_t bank, uint16_t len)
{
    s_watch_req_addr    = addr;
    s_watch_req_bank    = bank;
    s_watch_req_len     = len ? len : 1;
    s_watch_req_pending = true;
}

extern "C" bool
debug_ui_take_watch_request(uint16_t *addr, int16_t *bank, uint16_t *len)
{
    if (!s_watch_req_pending) {
        return false;
    }
    if (addr) *addr = s_watch_req_addr;
    if (bank) *bank = s_watch_req_bank;
    if (len)  *len  = s_watch_req_len;
    s_watch_req_pending = false;
    return true;
}
