// Commander X16 Emulator — ImGui "History" panel.
//
// The rewind history as a strip: one tick per keyframe, gaps where no state
// could be taken, and a marker where the machine stands. Clicking the strip
// jumps to the nearest keyframe while the machine is halted.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_settings.h"
#include "debug_ui_image.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

extern "C" {
#include "input_log.h"
#include "keyframes.h"
#include "keyframes_host.h"
}

namespace {

double
cycles_to_seconds(int64_t cycles)
{
    const int khz = timing_native_khz();
    return khz > 0 ? (double)cycles / ((double)khz * 1000.0) : 0.0;
}

// Why the strip cannot be clicked, or NULL when it can.
const char *
jump_blocked(void)
{
    if (!keyframes_host_enabled())
        return "Rewind history is off.";
    if (!DEBUGIsPaused())
        return "Only while the machine is halted.";
    if (!keyframes_host_can_jump())
        return "No history to jump in yet. Run the machine to build some.";
    return NULL;
}

void
keyframe_tooltip(uint32_t index, uint32_t count, uint64_t now, const char *blocked)
{
    const keyframe_t *k = keyframes_get(index);
    ImGui::BeginTooltip();
    ImGui::Text("Keyframe %u of %u", index, count);
    ImGui::Text("%+.2f s from here", cycles_to_seconds((int64_t)k->cycle - (int64_t)now));
    ImGui::TextDisabled("cycle %llu, %u input events, state %.0f KB", (unsigned long long)k->cycle, k->event_index,
                        (double)k->size / 1024.0);
    static DebugUiImage    s_image;
    static const uint32_t *s_shown       = nullptr;
    static uint64_t        s_shown_cycle = 0;
    if (k->thumb && debug_ui_image_available() && s_image.ensure(KEYFRAMES_THUMB_W, KEYFRAMES_THUMB_H)) {
        if (s_shown != k->thumb || s_shown_cycle != k->cycle) {
            s_image.update(k->thumb);
            s_shown       = k->thumb;
            s_shown_cycle = k->cycle;
        }
        ImGui::Image(s_image.id(), ImVec2(KEYFRAMES_THUMB_W * 2.0f, KEYFRAMES_THUMB_H * 2.0f));
    }
    if (blocked)
        ImGui::TextDisabled("%s", blocked);
    else
        ImGui::TextUnformatted("Click to go there.");
    ImGui::EndTooltip();
}

void
status_line(const keyframes_status_t &s, const input_log_status_t &log, uint64_t lo, uint64_t hi)
{
    if (!keyframes_host_enabled()) {
        ImGui::TextDisabled("Off. Turn it on to step back, reverse continue and jump here.");
        return;
    }
    if (const char *why = keyframes_host_blocked()) {
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.35f, 1.0f), "Not recording: %s", why);
        return;
    }
    if (!keyframes_host_usable() || s.count == 0) {
        if (log.end_reason && log.has_recording)
            ImGui::TextDisabled("Recording ended (%s). History restarts at the next frame.", log.end_reason);
        else
            ImGui::TextDisabled("No history yet. Run the machine to build some.");
        return;
    }
    ImGui::Text("%u keyframes, %.1f of %.0f MB, %.1f s held", s.count, (double)s.bytes / (1024.0 * 1024.0),
                (double)s.budget / (1024.0 * 1024.0), cycles_to_seconds((int64_t)(hi - lo)));
    ImGui::SameLine();
    if (log.mode == INPUT_LOG_REPLAYING)
        ImGui::TextDisabled("(replaying; records on from the end)");
    else
        ImGui::TextDisabled("(recording)");
}

void
history_panel_render(bool *p_open)
{
    if (!ImGui::Begin("History", p_open)) {
        ImGui::End();
        return;
    }

    bool on = keyframes_host_enabled();
    if (ImGui::Checkbox("Keep rewind history", &on))
        debug_ui_set_rewind(on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Also in System > Settings > Stepping & interrupts.");

    keyframes_status_t s;
    keyframes_get_status(&s);
    input_log_status_t log;
    input_log_get_status(&log);
    const bool usable = keyframes_host_usable() && s.count > 0;

    uint64_t now = 0, lo = 0, hi = 1;
    if (usable) {
        uint32_t events;
        input_log_position(&now, &events);
        lo = keyframes_get(0)->cycle;
        hi = log.mode == INPUT_LOG_REPLAYING ? log.end_cycle : now;
        if (now > hi)
            hi = now;
        if (hi <= lo)
            hi = lo + 1;
    }
    status_line(s, log, lo, hi);

    const char *blocked   = jump_blocked();
    const bool  clickable = usable && !blocked;

    const float  h    = ImGui::GetTextLineHeight() * 2.6f;
    const float  w    = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0   = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImVec2(w > 40.0f ? w : 40.0f, h);
    ImGui::InvisibleButton("##history_strip", size);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImDrawList *dl    = ImGui::GetWindowDrawList();
    const ImU32 bg    = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 tick  = usable ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 gap   = IM_COL32(220, 80, 70, 160);
    const ImU32 mark  = IM_COL32(255, 200, 60, 255);
    const ImU32 hot   = IM_COL32(89, 166, 255, 255);
    const ImVec2 p1   = ImVec2(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, bg, 3.0f);

    // Right edge is the newest point. The width shown only grows, so a full
    // ring dropping its oldest keyframe slides the strip rather than
    // respacing it; a new history starts the scale again.
    static uint64_t s_shown = 0;
    static uint32_t s_gen   = 0;
    if (!usable || input_log_generation() != s_gen)
        s_shown = 0;
    s_gen = input_log_generation();
    if (hi - lo > s_shown)
        s_shown = hi - lo;
    const uint64_t first_shown = hi > s_shown ? hi - s_shown : 0;
    const float  span = size.x - 8.0f;
    const double den  = (double)(s_shown ? s_shown : 1);
    auto         x_of = [&](uint64_t c) {
        const double back = c >= hi ? 0.0 : (double)(hi - c) / den;
        return p0.x + 4.0f + (float)((1.0 - (back > 1.0 ? 1.0 : back)) * span);
    };
    if (usable) {
        const double secs = cycles_to_seconds((int64_t)s_shown);
        static const double steps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
        double step = steps[0];
        for (double st : steps) {
            step = st;
            if (secs / st <= 8.0)
                break;
        }
        const double per_s = secs > 0 ? span / secs : 0;
        const ImU32  rule  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        for (int n = 1; per_s > 0 && n * step <= secs; n++) {
            const float x = p0.x + 4.0f + span - (float)(n * step * per_s);
            dl->AddLine(ImVec2(x, p1.y - size.y * 0.22f), ImVec2(x, p1.y), rule, 1.0f);
            char label[16];
            snprintf(label, sizeof label, step < 1 ? "-%.1fs" : "-%.0fs", n * step);
            dl->AddText(ImVec2(x + 2.0f, p1.y - ImGui::GetTextLineHeight()), rule, label);
        }
    }

    int nearest = -1;
    if (usable) {
        for (uint32_t i = 0; i < s.gaps; i++) {
            const keyframe_gap_t *g = keyframes_gap(i);
            if (g->last_cycle < first_shown || g->first_cycle > hi)
                continue;
            dl->AddRectFilled(ImVec2(x_of(g->first_cycle), p0.y + 2.0f),
                              ImVec2(fmaxf(x_of(g->last_cycle), x_of(g->first_cycle) + 2.0f), p1.y - 2.0f), gap);
        }
        float best = 1e9f;
        const float mx = ImGui::GetIO().MousePos.x;
        for (uint32_t i = 0; i < s.count; i++) {
            const float x = x_of(keyframes_get(i)->cycle);
            dl->AddLine(ImVec2(x, p0.y + size.y * 0.15f), ImVec2(x, p1.y - size.y * 0.45f), tick, 1.0f);
            if (hovered && fabsf(x - mx) < best) {
                best    = fabsf(x - mx);
                nearest = (int)i;
            }
        }
        if (nearest >= 0 && clickable) {
            const float x = x_of(keyframes_get((uint32_t)nearest)->cycle);
            dl->AddLine(ImVec2(x, p0.y + 2.0f), ImVec2(x, p1.y - 2.0f), hot, 3.0f);
        }
        const float xn = x_of(now);
        dl->AddLine(ImVec2(xn, p0.y), ImVec2(xn, p1.y), mark, 2.0f);
        dl->AddTriangleFilled(ImVec2(xn - 5.0f, p0.y), ImVec2(xn + 5.0f, p0.y), ImVec2(xn, p0.y + 6.0f), mark);
    }

    if (hovered) {
        if (!usable) {
            ImGui::SetTooltip("%s", blocked ? blocked : "No history to show.");
        } else if (nearest >= 0) {
            keyframe_tooltip((uint32_t)nearest, s.count, now, blocked);
        }
    }
    if (clicked && clickable && nearest >= 0)
        debug_ui_queue_history_move(HistoryMove::Jump, (unsigned)nearest);

    ImGui::TextDisabled("Ticks are keyframes, red is a gap with none, yellow is where the machine is.");
    ImGui::End();
}

DebugPanelRegistration s_reg("History", history_panel_render, true);

} // namespace
