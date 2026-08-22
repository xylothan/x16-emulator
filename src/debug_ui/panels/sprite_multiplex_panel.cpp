// Commander X16 Emulator — Sprite Multiplex panel.
//
// The VERA panel's sprite tab shows the attribute table as it stands right now.
// For a program that multiplexes -- rewriting slots from a raster IRQ so 128
// hardware sprites show far more than 128 sprites -- that view is close to
// useless: it shows whichever band of the screen happened to be written last,
// and the rest of the frame is simply gone. This panel shows the frame instead
// of the table.
//
// Everything here reads sprite_trace, which records what render_sprite_line()
// actually did on every scanline. Nothing on screen is inferred from the
// attribute table's current contents.

#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui.h"

#include "sprite_trace.h"

#include <SDL.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int NUM_SLOTS = SPRITE_TRACE_SLOTS;
constexpr int NUM_LINES = SPRITE_TRACE_LINES;

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline uint8_t expand4(uint8_t v) { return (uint8_t)((v << 4) | v); }

void
build_palette(uint32_t out[256])
{
    for (int i = 0; i < 256; ++i) {
        uint8_t b0 = video_space_read(0x1FA00 + i * 2);
        uint8_t b1 = video_space_read(0x1FA00 + i * 2 + 1);
        out[i] = 0xFF000000u | ((uint32_t)expand4(b0 & 0x0f) << 16) |
                 ((uint32_t)expand4((b0 >> 4) & 0x0f) << 8) | (uint32_t)expand4(b1 & 0x0f);
    }
}

// A stable colour per distinct sprite graphic, so the same artwork reused in
// several slots reads as the same thing across the whole ribbon.
ImU32
gen_color(const sprite_gen_t &g, float alpha)
{
    uint32_t h = g.address * 2654435761u;
    h ^= (uint32_t)g.palette_offset * 40503u;
    h ^= (uint32_t)g.color_mode << 19;
    float hue = (float)((h >> 8) & 0xFFFF) / 65535.0f;
    float r, gg, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.98f, r, gg, b);
    return ImGui::GetColorU32(ImVec4(r, gg, b, alpha));
}

struct GfxTexture {
    SDL_Texture *tex = nullptr;
    int          w = 0, h = 0;

    bool ensure(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return false;
        if (tex && w == width && h == height)
            return true;
        if (tex) {
            SDL_DestroyTexture(tex);
            tex = nullptr;
        }
        SDL_Renderer *ren = debug_ui_get_renderer();
        if (!ren)
            return false;
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                width, height);
        if (!tex)
            return false;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
        w = width;
        h = height;
        return true;
    }
    void update(const uint32_t *px) { if (tex) SDL_UpdateTexture(tex, nullptr, px, w * 4); }
    ImTextureID id() const { return (ImTextureID)(intptr_t)tex; }
};

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
bool  s_enabled       = false;
bool  s_alloc_failed  = false;
int   s_burst_gap     = 128;
int   s_px_per_line   = 2;   // ribbon horizontal zoom
int   s_px_per_slot   = 4;   // ribbon vertical zoom
bool  s_only_drawn    = true;
bool  s_mark_overrun  = true;
int   s_selected_gen  = -1;

GfxTexture s_ghost_tex;
uint32_t  *s_ghost_px    = nullptr;
uint32_t   s_ghost_frame = 0xFFFFFFFFu;
bool       s_ghost_valid = false;

const char *
gen_kind(const sprite_gen_t &g)
{
    if (g.flags & SPRITE_GEN_LATE)    return "LATE";
    if (g.flags & SPRITE_GEN_DREW)    return (g.flags & SPRITE_GEN_CARRIED) ? "static" : "muxed";
    if (g.flags & SPRITE_GEN_CARRIED) return "idle";
    return "phantom";
}

// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------
void
draw_overview(const sprite_trace_frame_t *f)
{
    const sprite_frame_summary_t &s = f->summary;

    ImGui::Text("Frame %u", s.frame);
    ImGui::SameLine();
    if (s.multiplexing)
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "— multiplexing detected");
    else
        ImGui::TextDisabled("— no multiplexing (no slot held more than one drawn value)");

    ImGui::Separator();

    // The headline: what the player actually saw, versus the 128 the hardware has.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.9f, 1.0f, 1.0f));
    ImGui::Text("Effective sprites: %u", s.effective_sprites);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    float ratio = s.slots_used ? (float)s.effective_sprites / (float)s.slots_used : 0.0f;
    ImGui::Text("  (%u of 128 slots used, %.1f\xC3\x97 multiplex)", s.slots_used, ratio);

    if (ImGui::BeginTable("mux_summary", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Slots multiplexed");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", s.slots_multiplexed);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Deepest slot");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%u values", s.max_gens_in_slot);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Attribute writes");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u (%u while the beam was in the display)", s.mutations, s.mutations_onscreen);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Raster bursts");
        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", s.mutation_bursts);
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Wasted work");
    if (s.phantom_writes || s.late_writes) {
        if (s.phantom_writes)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "  %u phantom: written to a slot but never drew a pixel",
                               s.phantom_writes);
        if (s.late_writes)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f),
                               "  %u late: completed after the beam had passed their own Y span",
                               s.late_writes);
    } else {
        ImGui::TextDisabled("  none — every value written was displayed");
    }

    // ── Cost, requirement 4a: what the multiplexing costs the 6502 ──────────
    ImGui::Separator();
    ImGui::TextUnformatted("CPU cost of multiplexing");
    float cpu_pct = s.cpu_cycles ? 100.0f * (float)s.mutation_cycles / (float)s.cpu_cycles : 0.0f;
    ImGui::Text("  %u cycles/frame of %u (%.1f%% of the frame budget)",
                s.mutation_cycles, s.cpu_cycles, cpu_pct);
    ImGui::ProgressBar(cpu_pct / 100.0f, ImVec2(-FLT_MIN, 0), "");
    ImGui::TextDisabled("  Summed span of each burst of attribute writes (gap <= %u cycles).",
                        s.burst_gap);
    ImGui::TextDisabled("  A lower bound: a burst's span excludes the handler's entry, exit");
    ImGui::TextDisabled("  and any work following its last store.");
    if (f->mutations_dropped)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                           "  %u writes past the buffer were counted but not timed.",
                           f->mutations_dropped);

    // ── Cost, requirement 4b: VERA's own per-scanline sprite render time ────
    ImGui::Separator();
    ImGui::TextUnformatted("VERA sprite render time");
    ImGui::Text("  Peak: line %u wanted %u of %d clocks", s.peak_line, s.peak_demand,
                SPRITE_TRACE_LINE_BUDGET);
    if (s.lines_exhausted) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                           "  %u scanlines ran out of time — %u sprites were dropped.",
                           s.lines_exhausted, s.sprites_dropped);
        ImGui::TextDisabled("  Dropped exactly as VERA drops them: when render_time_r hits 798");
        ImGui::TextDisabled("  the sprite search and the line renderer both stop, so the sprite");
        ImGui::TextDisabled("  in flight is left part-drawn and every slot after it is skipped.");
        ImGui::TextDisabled("  Move sprites off these lines or make them narrower.");
    } else {
        float head = s.peak_demand ? 100.0f * (float)s.peak_demand / (float)SPRITE_TRACE_LINE_BUDGET
                                   : 0.0f;
        ImGui::TextDisabled("  No scanline ran out of time — worst line is at %.0f%% of it.", head);
    }
}

// ---------------------------------------------------------------------------
// Raster ribbon: slots down, scanlines across. One horizontal band per value a
// slot held, so a multiplexed slot reads as a stack of coloured segments.
// ---------------------------------------------------------------------------
void
draw_ribbon(const sprite_trace_frame_t *f)
{
    ImGui::SliderInt("Line width", &s_px_per_line, 1, 6, "%d px");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderInt("Slot height", &s_px_per_slot, 2, 12, "%d px");
    ImGui::SameLine();
    ImGui::Checkbox("Only drawn", &s_only_drawn);
    ImGui::SameLine();
    ImGui::Checkbox("Mark overrun", &s_mark_overrun);
    ImGui::TextDisabled("Colour identifies the sprite graphic. Hover for the value a slot held "
                        "on that line.");

    const float cw = (float)s_px_per_line;
    const float ch = (float)s_px_per_slot;

    ImGui::BeginChild("ribbon_scroll", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList  *dl     = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(NUM_LINES * cw, NUM_SLOTS * ch);

    ImGui::InvisibleButton("ribbon", size);
    const bool hovered = ImGui::IsItemHovered();

    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(18, 18, 22, 255));

    // Run-length merge along each slot row. Drawing every (line, slot) cell
    // would be 61,440 rectangles a frame; multiplexed content changes only a
    // handful of times per row, so merging collapses it to a few hundred.
    for (int slot = 0; slot < NUM_SLOTS; ++slot) {
        int      run_start = -1;
        uint16_t run_gen   = SPRITE_TRACE_NO_GEN;
        uint8_t  run_flags = 0;

        for (int line = 0; line <= NUM_LINES; ++line) {
            uint16_t g  = SPRITE_TRACE_NO_GEN;
            uint8_t  fl = 0;
            if (line < NUM_LINES) {
                const size_t i = (size_t)line * NUM_SLOTS + slot;
                g  = f->line_gen[i];
                fl = f->line_flags[i];
                if (s_only_drawn && !(fl & SPRITE_TRACE_DREW))
                    g = SPRITE_TRACE_NO_GEN;
            }
            const bool same = (g == run_gen) &&
                              ((fl & SPRITE_TRACE_DREW) == (run_flags & SPRITE_TRACE_DREW)) &&
                              ((fl & SPRITE_TRACE_CUT) == (run_flags & SPRITE_TRACE_CUT));
            if (!same) {
                if (run_start >= 0 && run_gen != SPRITE_TRACE_NO_GEN && run_gen < f->gen_count) {
                    const sprite_gen_t &gen = f->gens[run_gen];
                    float alpha = (run_flags & SPRITE_TRACE_DREW) ? 1.0f : 0.22f;
                    ImU32 col   = gen_color(gen, alpha);
                    ImVec2 a(origin.x + run_start * cw, origin.y + slot * ch);
                    ImVec2 b(origin.x + line * cw, origin.y + slot * ch + ch - 1.0f);
                    dl->AddRectFilled(a, b, col);
                    if (s_mark_overrun && (run_flags & SPRITE_TRACE_CUT))
                        dl->AddRectFilled(a, b, IM_COL32(255, 40, 40, 110));
                    if (s_selected_gen == (int)run_gen)
                        dl->AddRect(a, b, IM_COL32(255, 255, 255, 220));
                }
                run_start = line;
                run_gen   = g;
                run_flags = fl;
            }
        }
    }

    // Scanlines whose sprite demand exceeded the hardware budget.
    if (s_mark_overrun) {
        for (int line = 0; line < NUM_LINES; ++line) {
            if (!f->lines[line].exhausted)
                continue;
            dl->AddRectFilled(ImVec2(origin.x + line * cw, origin.y),
                              ImVec2(origin.x + line * cw + cw, origin.y + 3.0f),
                              IM_COL32(255, 60, 50, 230));
        }
    }

    if (hovered) {
        const ImVec2 mp   = ImGui::GetMousePos();
        const int    line = clampi((int)((mp.x - origin.x) / cw), 0, NUM_LINES - 1);
        const int    slot = clampi((int)((mp.y - origin.y) / ch), 0, NUM_SLOTS - 1);
        const size_t i    = (size_t)line * NUM_SLOTS + slot;
        const uint16_t gi = f->line_gen[i];

        dl->AddLine(ImVec2(origin.x + line * cw, origin.y),
                    ImVec2(origin.x + line * cw, origin.y + size.y),
                    IM_COL32(255, 255, 255, 60));

        ImGui::BeginTooltip();
        ImGui::Text("line %d, slot %d", line, slot);
        if (gi != SPRITE_TRACE_NO_GEN && gi < f->gen_count) {
            const sprite_gen_t &g  = f->gens[gi];
            const uint8_t       fl = f->line_flags[i];
            ImGui::Separator();
            ImGui::Text("value #%u in this slot (%s)", g.index_in_slot, gen_kind(g));
            ImGui::Text("gfx $%05X  %ux%u  %s", g.address, g.width, g.height,
                        g.color_mode ? "8bpp" : "4bpp");
            ImGui::Text("pos %d,%d  z%u  pal+%u", g.x, g.y, g.zdepth, g.palette_offset);
            if (g.write_line == SPRITE_TRACE_NO_LINE)
                ImGui::TextDisabled("carried in from the previous frame");
            else
                ImGui::Text("written on line %u (cycle %u, %u bytes)", g.write_line,
                            g.write_cycle, g.writes);
            ImGui::Text("resident %u lines, drew on %u", g.lines_resident, g.lines_drawn);
            ImGui::Text("this line: %s%s%s", (fl & SPRITE_TRACE_DREW) ? "drew " : "",
                        (fl & SPRITE_TRACE_ONSCREEN) ? "on-screen " : "",
                        (fl & SPRITE_TRACE_CUT) ? "DROPPED (out of render time)" : "");
            ImGui::Text("cost %u VERA clocks", f->line_budget[i]);
        } else {
            ImGui::TextDisabled("slot not reached by the renderer on this line");
        }
        ImGui::Separator();
        ImGui::Text("line wanted %u of %d clocks%s", f->lines[line].demand,
                    SPRITE_TRACE_LINE_BUDGET,
                    f->lines[line].exhausted ? "  — RAN OUT" : "");
        if (f->lines[line].dropped)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1), "%u sprite(s) dropped on this line",
                               f->lines[line].dropped);
        ImGui::EndTooltip();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            s_selected_gen = (gi == SPRITE_TRACE_NO_GEN) ? -1 : (int)gi;
    }

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Budget: per-scanline sprite fetch demand against the hardware ceiling.
// ---------------------------------------------------------------------------
void
draw_budget(const sprite_trace_frame_t *f)
{
    const sprite_frame_summary_t &s = f->summary;
    ImGui::Text("Peak: line %u wanted %u of %d clocks.  %u scanline(s) ran out, %u sprites dropped.",
                s.peak_line, s.peak_demand, SPRITE_TRACE_LINE_BUDGET, s.lines_exhausted,
                s.sprites_dropped);
    ImGui::TextDisabled("Height is the render time a scanline wanted. Anything above the white "
                        "line did not get drawn — on hardware or here.");

    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x;
    const float  h = 220.0f;
    ImGui::InvisibleButton("budget", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), IM_COL32(18, 18, 22, 255));

    // Scale to the peak, but never compress the ceiling below three-quarters
    // height, so an ordinary frame does not look alarmingly full.
    float top = (float)(s.peak_demand > (uint32_t)(SPRITE_TRACE_LINE_BUDGET * 4 / 3)
                            ? s.peak_demand
                            : (uint32_t)(SPRITE_TRACE_LINE_BUDGET * 4 / 3));
    const float bw = w / (float)NUM_LINES;

    for (int line = 0; line < NUM_LINES; ++line) {
        const sprite_line_stat_t &st = f->lines[line];
        if (!st.demand)
            continue;
        float frac = (float)st.demand / top;
        float bh   = frac * h;
        ImU32 col  = st.exhausted ? IM_COL32(235, 70, 60, 255) : IM_COL32(90, 170, 240, 255);
        dl->AddRectFilled(ImVec2(origin.x + line * bw, origin.y + h - bh),
                          ImVec2(origin.x + line * bw + (bw > 1.0f ? bw : 1.0f), origin.y + h),
                          col);
    }

    const float ceil_y = origin.y + h - ((float)SPRITE_TRACE_LINE_BUDGET / top) * h;
    dl->AddLine(ImVec2(origin.x, ceil_y), ImVec2(origin.x + w, ceil_y),
                IM_COL32(255, 255, 255, 200));
    dl->AddText(ImVec2(origin.x + 4, ceil_y - 16), IM_COL32(255, 255, 255, 200),
                "VERA render time (798 clocks)");

    if (hovered) {
        const int line = clampi((int)((ImGui::GetMousePos().x - origin.x) / bw), 0, NUM_LINES - 1);
        const sprite_line_stat_t &st = f->lines[line];
        ImGui::BeginTooltip();
        ImGui::Text("line %d", line);
        ImGui::Text("wanted %u clocks, spent %u of %d", st.demand, st.budget_used,
                    SPRITE_TRACE_LINE_BUDGET);
        ImGui::Text("slots reached: %u, drawing: %u", st.evaluated, st.drawn);
        if (st.exhausted)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.35f, 1),
                               "ran out at slot %u — %u sprite(s) dropped", st.cut_slot,
                               st.dropped);
        ImGui::Text("CPU cycles on this line: %u", st.cpu_cycles);
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------------------
// Effective sprites: one row per value that actually reached the screen.
// ---------------------------------------------------------------------------
void
draw_effective(const sprite_trace_frame_t *f)
{
    static bool show_phantom = true;

    ImGui::Checkbox("Include phantom / late", &show_phantom);
    ImGui::SameLine();
    ImGui::TextDisabled("%u effective this frame", f->summary.effective_sprites);

    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("effective", 10, tf))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Gfx");
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Pos");
    ImGui::TableSetupColumn("Lines");
    ImGui::TableSetupColumn("Drew");
    ImGui::TableSetupColumn("Written");
    ImGui::TableSetupColumn("VERA cyc");
    ImGui::TableHeadersRow();

    for (uint16_t i = 0; i < f->gen_count; ++i) {
        const sprite_gen_t &g = f->gens[i];
        const bool drew = (g.flags & SPRITE_GEN_DREW) != 0;
        if (!drew) {
            // An untouched idle slot is noise; a written value that never drew
            // is a finding.
            if (g.flags & SPRITE_GEN_CARRIED)
                continue;
            if (!show_phantom)
                continue;
        }

        ImGui::TableNextRow();
        if (!drew)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   (g.flags & SPRITE_GEN_LATE) ? IM_COL32(90, 30, 30, 140)
                                                               : IM_COL32(80, 60, 20, 140));

        ImGui::TableSetColumnIndex(0);
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%u##g%u", g.slot, i);
        if (ImGui::Selectable(lbl, s_selected_gen == (int)i, ImGuiSelectableFlags_SpanAllColumns))
            s_selected_gen = (int)i;

        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", g.index_in_slot);
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(gen_color(g, 1.0f)));
        ImGui::TextUnformatted(gen_kind(g));
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(3); ImGui::Text("$%05X", g.address);
        ImGui::TableSetColumnIndex(4); ImGui::Text("%ux%u", g.width, g.height);
        ImGui::TableSetColumnIndex(5); ImGui::Text("%d,%d", g.x, g.y);
        ImGui::TableSetColumnIndex(6);
        if (g.first_line == SPRITE_TRACE_NO_LINE)
            ImGui::TextDisabled("—");
        else
            ImGui::Text("%u..%u", g.first_line, g.last_line);
        ImGui::TableSetColumnIndex(7); ImGui::Text("%u", g.lines_drawn);
        ImGui::TableSetColumnIndex(8);
        if (g.write_line == SPRITE_TRACE_NO_LINE)
            ImGui::TextDisabled("carried");
        else
            ImGui::Text("line %u", g.write_line);
        ImGui::TableSetColumnIndex(9); ImGui::Text("%u", g.budget_cycles);
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// Frame ghost: every effective sprite composited into one image, each drawn
// only on the scanlines it was actually resident and visible for. This is the
// frame the player saw, which no single read of the attribute table can show.
// ---------------------------------------------------------------------------
void
rebuild_ghost(const sprite_trace_frame_t *f)
{
    if (!s_ghost_px)
        s_ghost_px = (uint32_t *)malloc((size_t)640 * 480 * 4);
    if (!s_ghost_px)
        return;
    memset(s_ghost_px, 0, (size_t)640 * 480 * 4);

    uint32_t pal[256];
    build_palette(pal);

    // Walk the residency map so each sprite contributes exactly the scanlines
    // it drew on -- reconstructing from first_line/last_line would paint over
    // gaps where the slot had been handed to a different sprite.
    for (int line = 0; line < NUM_LINES; ++line) {
        for (int slot = 0; slot < NUM_SLOTS; ++slot) {
            const size_t i = (size_t)line * NUM_SLOTS + slot;
            if (!(f->line_flags[i] & SPRITE_TRACE_DREW))
                continue;
            const uint16_t gi = f->line_gen[i];
            if (gi == SPRITE_TRACE_NO_GEN || gi >= f->gen_count)
                continue;
            const sprite_gen_t &g = f->gens[gi];

            int row = line - g.y;
            if (row < 0 || row >= (int)g.height)
                continue;
            if (g.vflip)
                row = (int)g.height - 1 - row;

            for (int sx = 0; sx < (int)g.width; ++sx) {
                const int px = g.x + sx;
                if (px < 0 || px >= 640)
                    continue;
                const int tx = g.hflip ? (int)g.width - 1 - sx : sx;

                int idx;
                if (g.color_mode) {
                    idx = video_space_read(g.address + (uint32_t)row * g.width + tx);
                } else {
                    uint8_t byte = video_space_read(g.address + (uint32_t)row * (g.width / 2) +
                                                    (tx / 2));
                    idx = (tx & 1) ? (byte & 0x0f) : (byte >> 4);
                }
                if (idx == 0)
                    continue;
                if (idx < 16)
                    idx += g.palette_offset;
                s_ghost_px[(size_t)line * 640 + px] = pal[idx & 0xff];
            }
        }
    }
    s_ghost_valid = true;
}

void
draw_ghost(const sprite_trace_frame_t *f)
{
    static bool auto_refresh = true;
    ImGui::Checkbox("Follow live frames", &auto_refresh);
    ImGui::SameLine();
    if (ImGui::Button("Rebuild") || (auto_refresh && f->summary.frame != s_ghost_frame)) {
        rebuild_ghost(f);
        s_ghost_frame = f->summary.frame;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("every sprite the frame showed, on the lines it showed them");

    if (!s_ghost_valid) {
        ImGui::TextDisabled("Nothing composited yet.");
        return;
    }
    if (!s_ghost_tex.ensure(640, 480)) {
        ImGui::TextDisabled("No renderer available.");
        return;
    }
    s_ghost_tex.update(s_ghost_px);

    const float w = ImGui::GetContentRegionAvail().x;
    const float scale = w < 640.0f ? w / 640.0f : 1.0f;
    ImGui::Image(s_ghost_tex.id(), ImVec2(640 * scale, 480 * scale));
}

// ---------------------------------------------------------------------------
void
sprite_multiplex_render(bool *p_open)
{
    ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sprite Multiplex", p_open)) {
        ImGui::End();
        return;
    }

    if (ImGui::Checkbox("Record sprite raster activity", &s_enabled)) {
        s_alloc_failed = !sprite_trace_set_enabled(s_enabled);
        if (s_alloc_failed)
            s_enabled = false;
        s_ghost_frame = 0xFFFFFFFFu;
        s_ghost_valid = false;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Burst gap", &s_burst_gap, 8, 2048, "%d cyc"))
        sprite_trace_set_burst_gap((uint32_t)s_burst_gap);

    if (s_enabled) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu KB)", sprite_trace_memory_usage() / 1024);
    }
    if (s_alloc_failed)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Could not allocate the trace buffers.");

    const sprite_trace_frame_t *f = sprite_trace_last_frame();
    if (!f) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Enable recording and let a frame complete.\n\n"
            "This panel reconstructs a frame from what render_sprite_line() actually did on "
            "each scanline, so a program that rewrites sprite attributes from a raster IRQ "
            "shows up as what it really drew rather than whatever the attribute table happens "
            "to hold when you look at it.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("mux_tabs")) {
        if (ImGui::BeginTabItem("Overview")) { draw_overview(f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Raster ribbon")) { draw_ribbon(f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Budget")) { draw_budget(f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Effective sprites")) { draw_effective(f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Frame ghost")) { draw_ghost(f); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace

static DebugPanelRegistration s_reg("Sprite Multiplex", sprite_multiplex_render, false);
