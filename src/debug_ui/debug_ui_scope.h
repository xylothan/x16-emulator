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

#endif // DEBUG_UI_SCOPE_H
