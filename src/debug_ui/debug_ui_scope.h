// Commander X16 Emulator — shared oscilloscope / meter helpers for the audio
// debugger panels (PSG, YM2151, PCM).
//
// Header-only, like debug_ui_widgets.h, so it needs no CMake entry.
//
// Everything draws through ImDrawList (no ImPlot dependency). The traces are
// decimated to at most one column per pixel, so the cost is bounded by the
// widget's on-screen width rather than by the sample count: a 2048-sample
// window in a 200 px wide scope costs 200 columns, not 2048 line segments.
#ifndef DEBUG_UI_SCOPE_H
#define DEBUG_UI_SCOPE_H

#include "imgui.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

// DEBUGIsPaused() — audio panels hold their display while the machine is
// stopped (see dbgui_audio_hold below).
#include "debug_ui_bridge.h"
#include "debug_ui_settings.h"

// Vertical gain sentinel: scale the trace to the window's own peak each frame.
#define DBGUI_SCOPE_AUTO_GAIN 0.0f

// Find a rising zero crossing so a periodic waveform stays still instead of
// sliding across the widget. Searches the part of `samples` that precedes the
// displayed window, newest crossing first, and returns the index the window
// should start at. Falls back to the newest `window` samples when the signal
// never crosses (DC, silence, or a period longer than the search range).
static inline int
dbgui_scope_trigger_offset(const int16_t *samples, int count, int window)
{
    int latest = count - window; // no trigger: show the newest `window` samples
    if (latest <= 0) {
        return 0;
    }
    for (int i = latest; i > 0; --i) {
        if (samples[i - 1] < 0 && samples[i] >= 0) {
            return i;
        }
    }
    return latest;
}

// Draw one oscilloscope trace.
//
//   samples/count : sample history, oldest first (as returned by *_scope_read)
//   size          : widget size in pixels; 0 in either axis fills the available
//                   width / uses a default height
//   window        : how many of the newest samples to display (clamped to count)
//   gain          : pixels per unit / 32768.0; DBGUI_SCOPE_AUTO_GAIN auto-scales
//   trigger       : align to a rising zero crossing
//   overlay       : optional text drawn in the top-left corner
//
// Returns the peak magnitude (0..32768) of the displayed window, which callers
// can reuse for a level readout without rescanning.
static inline int
dbgui_scope(const char *id, const int16_t *samples, int count, ImVec2 size,
            ImU32 color, int window, float gain, bool trigger, const char *overlay)
{
    if (size.x <= 0.0f) {
        size.x = ImGui::GetContentRegionAvail().x;
    }
    if (size.x < 16.0f) {
        size.x = 16.0f;
    }
    if (size.y <= 0.0f) {
        size.y = ImGui::GetTextLineHeight() * 4.0f;
    }

    ImGui::PushID(id);
    ImGui::InvisibleButton("##scope", size);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImGui::PopID();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 255));

    // Grid: 4 columns / 4 rows, with a brighter centre line for the zero axis.
    const ImU32 grid = IM_COL32(52, 58, 68, 255);
    const ImU32 axis = IM_COL32(90, 100, 115, 255);
    for (int i = 1; i < 4; ++i) {
        float x = p0.x + (p1.x - p0.x) * (float)i / 4.0f;
        float y = p0.y + (p1.y - p0.y) * (float)i / 4.0f;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), grid);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), (i == 2) ? axis : grid);
    }
    dl->AddRect(p0, p1, IM_COL32(70, 78, 90, 255));

    if (samples == NULL || count <= 0) {
        return 0;
    }
    if (window <= 0 || window > count) {
        window = count;
    }

    const int start = trigger ? dbgui_scope_trigger_offset(samples, count, window)
                              : (count - window);

    // Peak of the displayed window: drives auto-gain and the return value.
    int peak = 0;
    for (int i = 0; i < window; ++i) {
        int v = samples[start + i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
    }

    const float half = (p1.y - p0.y) * 0.5f - 1.0f;
    const float mid  = (p0.y + p1.y) * 0.5f;
    float       scale;
    if (gain > 0.0f) {
        scale = half * gain;
    } else {
        // Auto: fill the widget, but keep near-silence flat instead of
        // amplifying dither into a full-height mess.
        scale = (peak > 64) ? (half / (float)peak) : (half / 32768.0f);
    }

    dl->PushClipRect(p0, p1, true);

    const int columns = (int)(p1.x - p0.x);
    if (window <= columns) {
        // Fewer samples than pixels: draw the actual points.
        for (int i = 1; i < window; ++i) {
            float xa = p0.x + (p1.x - p0.x) * (float)(i - 1) / (float)(window - 1);
            float xb = p0.x + (p1.x - p0.x) * (float)i / (float)(window - 1);
            float ya = mid - (float)samples[start + i - 1] * scale;
            float yb = mid - (float)samples[start + i] * scale;
            dl->AddLine(ImVec2(xa, ya), ImVec2(xb, yb), color, 1.0f);
        }
    } else {
        // More samples than pixels: one min/max bar per column, the usual
        // waveform-envelope look, and bounded by the widget width.
        for (int c = 0; c < columns; ++c) {
            int lo = (int)((int64_t)c * window / columns);
            int hi = (int)((int64_t)(c + 1) * window / columns);
            if (hi <= lo) {
                hi = lo + 1;
            }
            int vmin = 32767, vmax = -32768;
            for (int i = lo; i < hi && i < window; ++i) {
                int v = samples[start + i];
                if (v < vmin) {
                    vmin = v;
                }
                if (v > vmax) {
                    vmax = v;
                }
            }
            float x  = p0.x + (float)c + 0.5f;
            float ya = mid - (float)vmax * scale;
            float yb = mid - (float)vmin * scale;
            if (yb - ya < 1.0f) {
                yb = ya + 1.0f;
            }
            dl->AddLine(ImVec2(x, ya), ImVec2(x, yb), color, 1.0f);
        }
    }

    dl->PopClipRect();

    if (overlay && overlay[0]) {
        dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), IM_COL32(190, 198, 210, 220), overlay);
    }
    return peak;
}

// A horizontal peak-level bar. `frac` is 0..1 (values above 1 are clamped and
// shown in red so clipping is obvious).
static inline void
dbgui_level_meter(const char *id, float frac, ImVec2 size, ImU32 color)
{
    if (size.x <= 0.0f) {
        size.x = 80.0f;
    }
    if (size.y <= 0.0f) {
        size.y = ImGui::GetTextLineHeight();
    }

    bool clipped = frac > 1.0f;
    if (clipped) {
        frac = 1.0f;
    }
    if (frac < 0.0f) {
        frac = 0.0f;
    }

    ImGui::PushID(id);
    ImGui::InvisibleButton("##meter", size);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImGui::PopID();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 26, 30, 255));
    if (frac > 0.0f) {
        ImVec2 fill(p0.x + (p1.x - p0.x) * frac, p1.y);
        dl->AddRectFilled(p0, fill, clipped ? IM_COL32(220, 80, 70, 255) : color);
    }
    dl->AddRect(p0, p1, IM_COL32(70, 78, 90, 255));
}

// Shared "Scope" tab controls: time base, vertical gain, trigger and freeze.
// Keeping them here means the three panels present identical controls, and each
// panel only has to own the state.
struct DbgScopeControls {
    int   window  = 512;   // samples displayed
    float gain    = DBGUI_SCOPE_AUTO_GAIN;
    bool  trigger = true;
    bool  freeze  = false;
};

// Draw the control row. `max_window` is the ring depth (PSG_SCOPE_SAMPLES etc).
static inline void
dbgui_scope_controls(DbgScopeControls *c, int max_window)
{
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt("Window", &c->window, 32, max_window, "%d smp");
    ImGui::SameLine();

    bool autoscale = (c->gain <= 0.0f);
    if (ImGui::Checkbox("Auto gain", &autoscale)) {
        c->gain = autoscale ? DBGUI_SCOPE_AUTO_GAIN : 1.0f;
    }
    if (!autoscale) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Gain", &c->gain, 0.05f, 16.0f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Trigger", &c->trigger);
    ImGui::SetItemTooltip("Align the trace to a rising zero crossing so a steady "
                          "tone stands still.");
    ImGui::SameLine();
    ImGui::Checkbox("Freeze", &c->freeze);
    ImGui::SetItemTooltip("Stop refreshing the traces. Capture also stops, since "
                          "it is only armed while the debugger reads it.");
}

// Should an audio panel keep showing its last captured state instead of
// re-reading?
//
// Pausing the machine silences audio: nothing is rendered, peak meters read
// back zero and the scope rings stop filling, so a panel that re-reads while
// paused wipes out exactly the state the user paused to look at. Holding the
// last live capture keeps the display meaningful, and is what you want from a
// debugger — the values shown are the ones in effect at the moment of the
// pause.
static inline bool
dbgui_audio_hold(void)
{
    return DEBUGIsPaused() && debug_ui_settings().audio_hold_on_pause;
}

// Note next to the controls explaining that the display is held. Call after the
// control row when dbgui_audio_hold() is true.
static inline void
dbgui_audio_hold_note(void)
{
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f), "held (paused)");
    ImGui::SetItemTooltip("The machine is paused, so audio is silent and nothing new is\n"
                          "captured. These are the values from the moment it stopped.");
}

// Note next to the controls for traces computed from chip state rather than
// captured. Scope capture is self-arming and nothing renders audio while
// paused, so a scope first opened at a breakpoint has an empty ring; the sound
// chips' own state still describes what they are about to emit, so the panels
// project that instead of showing a dead display.
static inline void
dbgui_audio_predicted_note(const char *what)
{
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.70f, 0.80f, 1.00f, 1.0f), "projected");
    ImGui::SetItemTooltip("Paused with no captured audio, so these traces are computed from\n"
                          "%s - what it would emit next, using the same\n"
                          "renderer live audio uses.\n\n"
                          "It is a projection, not a recording: change the chip state and it\n"
                          "updates immediately. Real captured audio replaces it on resume.",
                          what);
}

// ─── Host-output mute ───────────────────────────────────────────────────────
// The PSG and YM panels can mute a voice on the host output. The chip keeps
// playing it exactly as before (see psg_debug_set_mute_mask and
// YM_debug_set_mute_mask); only what reaches the speakers changes.

#define DBGUI_MUTE_COLOR IM_COL32(200, 60, 60, 255)

// Speaker glyph in a `size` x `size` box at `p0`: a small box and a flared cone,
// then either two sound-wave arcs (playing) or a slash through the whole glyph
// (muted) - the usual volume / mute iconography, drawn so it needs no icon font.
static inline void
dbgui_draw_speaker(ImDrawList *dl, ImVec2 p0, float size, ImU32 col, bool muted)
{
    const float s  = size;
    const float x  = p0.x + s * 0.10f;
    const float cy = p0.y + s * 0.5f;

    dl->AddRectFilled(ImVec2(x, cy - s * 0.14f), ImVec2(x + s * 0.20f, cy + s * 0.14f), col);
    dl->AddQuadFilled(ImVec2(x + s * 0.20f, cy - s * 0.14f), ImVec2(x + s * 0.46f, cy - s * 0.36f),
                      ImVec2(x + s * 0.46f, cy + s * 0.36f), ImVec2(x + s * 0.20f, cy + s * 0.14f), col);

    const float th = (s >= 18.0f) ? 1.8f : 1.4f;
    if (!muted) {
        const ImVec2 c(x + s * 0.48f, cy);
        dl->PathArcTo(c, s * 0.16f, -0.85f, 0.85f, 8);
        dl->PathStroke(col, ImDrawFlags_None, th);
        dl->PathArcTo(c, s * 0.32f, -0.85f, 0.85f, 12);
        dl->PathStroke(col, ImDrawFlags_None, th);
    } else {
        // Dark under-stroke first so the slash stays readable over the cone.
        const ImVec2 a(p0.x + s * 0.12f, p0.y + s * 0.12f);
        const ImVec2 b(p0.x + s * 0.88f, p0.y + s * 0.88f);
        dl->AddLine(a, b, IM_COL32(20, 20, 24, 255), th + 2.0f);
        dl->AddLine(a, b, DBGUI_MUTE_COLOR, th + 0.4f);
    }
}

// Speaker toggle for a voice-table row: speaker with sound waves while audible,
// speaker with a red slash while muted. Returns true when clicked; the caller
// flips its bit. Rows with a SpanAllColumns selectable must create it with
// ImGuiSelectableFlags_AllowOverlap and draw this afterwards, or the selectable
// takes the click.
//
// The ID is scoped under "mute" rather than pushed as the bare voice number:
// TableHeadersRow() pushes each header's column index as its ID, so voice 0 in
// column 0 would otherwise collide with that column's header.
static inline bool
dbgui_mute_button(int voice, bool muted)
{
    ImGui::PushID("mute");
    ImGui::PushID(voice);
    const float  sz = ImGui::GetTextLineHeight() + 2.0f;
    const bool   clicked = ImGui::InvisibleButton("##spk", ImVec2(sz, sz));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) {
        dl->AddRectFilled(p0, ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    const ImU32 col = muted ? IM_COL32(150, 150, 158, 255) : ImGui::GetColorU32(ImGuiCol_Text);
    dbgui_draw_speaker(dl, p0, sz, col, muted);
    ImGui::SetItemTooltip(muted ? "Muted on the host output; the chip still plays it.\nClick to unmute."
                                : "Mute this voice on the host output.\nThe chip keeps playing it; only the speakers stop hearing it.");
    ImGui::PopID();
    ImGui::PopID();
    return clicked;
}

// Call straight after dbgui_scope() for a voice's trace: marks it when muted
// and returns true on a right-click, which toggles mute (left-click already
// selects the voice in both panels).
static inline bool
dbgui_scope_mute_toggle(bool muted)
{
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    if (muted) {
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        ImDrawList  *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, IM_COL32(60, 10, 10, 90));
        dl->AddRect(p0, p1, DBGUI_MUTE_COLOR, 0.0f, 0, 2.0f);
        const float sz = ImGui::GetTextLineHeight() + 2.0f;
        dbgui_draw_speaker(dl, ImVec2(p1.x - sz - 3.0f, p0.y + 2.0f), sz, IM_COL32(200, 200, 208, 255), true);
    }
    ImGui::SetItemTooltip(muted ? "Right-click to unmute" : "Right-click to mute on the host output");
    return clicked;
}

// Muted voice-table row. Call after TableNextRow() and before the row's
// SpanAllColumns selectable; it tints the row and returns how many style
// colours it pushed, which the caller pops straight after the selectable.
//
// The tint alone is not enough: the selected row's highlight (ImGuiCol_Header)
// is drawn by the selectable OVER the row background, so a muted row that was
// also the selected one showed as selected, not muted. While muted, the
// selectable's own highlight colours turn red too, so the row reads as muted
// in every state - plain, hovered and selected.
static inline int
dbgui_mute_row_begin(bool muted)
{
    if (!muted) {
        return 0;
    }
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(120, 30, 30, 90));
    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(170, 45, 45, 150));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(190, 60, 60, 170));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(210, 70, 70, 200));
    return 3;
}

// "Unmute all (n)" button. ALWAYS DRAWN - disabled while nothing is muted -
// and always the width of its widest label. A control that appears, vanishes
// or changes width moves everything after it, so the panel jumped under the
// pointer the moment a voice was muted or unmuted. Returns true when clicked.
static inline bool
dbgui_unmute_all_button(unsigned mask)
{
    int n = 0;
    for (unsigned m = mask; m; m &= m - 1) {
        ++n;
    }
    char label[48];
    // "###" keeps one ID while the visible count changes.
    snprintf(label, sizeof label, "Unmute all (%d)###unmute_all", n);
    const ImGuiStyle &st = ImGui::GetStyle();
    const float w = ImGui::CalcTextSize("Unmute all (16)").x + st.FramePadding.x * 2.0f;

    ImGui::BeginDisabled(mask == 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, 0.0f)); // SmallButton look
    const bool clicked = ImGui::Button(label, ImVec2(w, 0.0f));
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(mask ? "Unmute every voice muted on the host output."
                               : "Nothing is muted. Click a voice's speaker icon, or right-click\n"
                                 "its scope trace, to mute it on the host output.");
    return clicked && mask != 0;
}

#endif // DEBUG_UI_SCOPE_H
