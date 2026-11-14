// YM2151 panel — live FM synthesis inspection for the Commander X16.
//
// Tabs:
//   * Channels  — the 8 OPM channels: note, pan, algorithm, key-on, EG and level.
//   * Operators — all parameters for the selected channel's M1/M2/C1/C2 slots.
//   * Algorithm — a live connection diagram for the selected channel's algorithm.
//   * Global    — LFO, noise, timers, CT pins, status and chip/sample clocks.
//   * Registers — all 256 accepted YM register shadows with compact decodes.
//   * Scope     — per-channel and summed waveforms using the shared scope widget.
//
// Technique: the panel reads only the debugger bridge's side-effect-free YM2151
// shadow/live accessors. Expensive work is gated by ImGui::BeginTabItem(), and
// YM_debug_scope_read() is called only from the Scope tab: reading it self-arms
// the audio core's per-channel capture for a short time, so hidden tabs do not
// keep ymfm doing extra per-sample output work.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_widgets.h"
#include "debug_ui_scope.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr double YM_CLOCK_HZ = 3579545.0;
constexpr double YM_SAMPLE_HZ = YM_CLOCK_HZ / 64.0;
constexpr int    OP_COUNT = 4;

static int s_selected_ch = 0;

struct AlgorithmInfo {
    uint8_t inputs[4];      // opout source selector used for this operator's phase input
    uint8_t carriers_mask;  // bits 0..3: operator output is summed to the channel output
    const char *desc;
};

// Derived from fm_channel<RegisterType>::output_4op() in ymfm_fm.ipp, and kept
// in ymfm's own O1..O4 order so it can be re-checked against that table
// verbatim. ymfm's O order is NOT the OPM register-slot order: ymfm's
// operator_map() gives channel N the operator list (N, N+16, N+8, N+24), and an
// operator's registers live at base + index, so
//
//     O1 -> slot 0 (M1)   O2 -> slot 2 (C1)   O3 -> slot 1 (M2)   O4 -> slot 3 (C2)
//
// Everything else in this panel is slot-indexed (op_reg/op_index use
// base + slot*8 + ch), so `inputs` and `carriers_mask` are permuted through
// kOpToSlot at the point of use rather than being pre-swapped here.
static const AlgorithmInfo s_alg[8] = {
    {{0, 1, 2, 3}, 0x8, "M1 -> C1 -> M2 -> C2 -> out"},
    {{0, 0, 5, 3}, 0x8, "(M1 + C1) -> M2 -> C2 -> out"},
    {{0, 0, 2, 6}, 0x8, "(M1 + (C1 -> M2)) -> C2 -> out"},
    {{0, 1, 0, 7}, 0x8, "((M1 -> C1) + M2) -> C2 -> out"},
    {{0, 1, 0, 3}, 0xA, "(M1 -> C1) + (M2 -> C2) -> out"},
    {{0, 1, 1, 1}, 0xE, "M1 modulates C1, M2 and C2; all three out"},
    {{0, 1, 0, 0}, 0xE, "(M1 -> C1) + M2 + C2 -> out"},
    {{0, 0, 0, 0}, 0xF, "M1 + M2 + C1 + C2 -> out"},
};

// ymfm operator index (O1..O4) -> OPM register slot. Self-inverse.
static const int kOpToSlot[OP_COUNT] = { 0, 2, 1, 3 };

// Re-index a mask of ymfm O-indices into a mask of register slots.
inline uint8_t op_mask_to_slot_mask(uint8_t op_mask)
{
    uint8_t slots = 0;
    for (int o = 0; o < OP_COUNT; ++o) {
        if ((op_mask & (1u << o)) != 0) slots |= (uint8_t)(1u << kOpToSlot[o]);
    }
    return slots;
}

static const char *s_slot_names[OP_COUNT] = { "M1", "M2", "C1", "C2" };
static const char *s_eg_names[5] = { "-", "Attack", "Decay", "Sustain", "Release" };
static const char *s_lfo_waves[4] = { "saw", "square", "triangle", "noise" };

inline uint8_t ym_reg(int reg) { return YM_debug_get_reg((uint8_t)reg); }
inline uint8_t ch_reg(int base, int ch) { return ym_reg(base + ch); }
inline uint8_t op_reg(int base, int slot, int ch) { return ym_reg(base + slot * 8 + ch); }
inline int op_index(int slot, int ch) { return slot * 8 + ch; }

struct NoteInfo {
    bool valid;
    char text[48];
    double hz;
};

float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

uint8_t alg_for_ch(int ch) { return (uint8_t)(ch_reg(0x20, ch) & 0x07); }

// Carriers as a SLOT mask, so it can be indexed the same way as everything else
// in this panel. s_alg holds ymfm O-indices, hence the permutation.
uint8_t carriers_for_alg(int alg) { return op_mask_to_slot_mask(s_alg[alg & 7].carriers_mask); }

float operator_level(int slot, int ch)
{
    ym_debug_operator op = {};
    if (!YM_debug_get_operator(op_index(slot, ch), &op)) {
        return 0.0f;
    }
    return clamp01(1.0f - (float)op.eg_attenuation / 1023.0f);
}

float channel_carrier_level(int ch)
{
    const uint8_t carriers = carriers_for_alg(alg_for_ch(ch));
    float level = 0.0f;
    for (int slot = 0; slot < OP_COUNT; ++slot) {
        if ((carriers & (1u << slot)) != 0) {
            const float l = operator_level(slot, ch);
            if (l > level) level = l;
        }
    }
    return level;
}

bool channel_effectively_silent(int ch)
{
    for (int slot = 0; slot < OP_COUNT; ++slot) {
        ym_debug_operator op = {};
        if (YM_debug_get_operator(op_index(slot, ch), &op) && op.eg_attenuation < 0x3f0) {
            return false;
        }
    }
    return true;
}

ImU32 eg_color(uint8_t state)
{
    switch (state) {
    case YM_EG_ATTACK:  return IM_COL32(105, 220, 125, 255);
    case YM_EG_DECAY:   return IM_COL32(235, 205, 90, 255);
    case YM_EG_SUSTAIN: return IM_COL32(105, 170, 255, 255);
    case YM_EG_RELEASE: return IM_COL32(195, 130, 230, 255);
    default:            return IM_COL32(120, 126, 136, 255);
    }
}

void note_from_kc_kf(uint8_t kc, uint8_t kf, NoteInfo *out)
{
    static const char *names[12] = { "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "C" };
    const int oct = kc >> 4;
    const int low = kc & 0x0f;
    const int semi = (kc & 3) + 3 * ((kc >> 2) & 3);
    out->valid = (low != 3 && low != 7 && low != 11 && low != 15);
    if (!out->valid) {
        out->hz = 0.0;
        snprintf(out->text, sizeof(out->text), "invalid KC %02X", kc);
        return;
    }
    const double fine_semi = (double)(kf >> 2) / 64.0;
    const double midi = 12.0 * (double)(oct + 1) + 1.0 + (double)semi + fine_semi;
    out->hz = 440.0 * pow(2.0, (midi - 69.0) / 12.0);
    snprintf(out->text, sizeof(out->text), "%s%d %+d c  %.2f Hz", names[semi], oct, (int)(fine_semi * 100.0 + 0.5), out->hz);
}

void draw_key_mask(uint8_t mask)
{
    // YM_debug_get_keyon() is already repacked into slot order. Raw $08 bits are
    // M1,C1,M2,C2, which is why the Registers tab calls that out separately.
    for (int slot = 0; slot < OP_COUNT; ++slot) {
        if (slot != 0) ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored((mask & (1u << slot)) ? ImVec4(0.45f, 1.0f, 0.55f, 1.0f)
                                                 : ImVec4(0.35f, 0.36f, 0.38f, 1.0f),
                           "%s", s_slot_names[slot]);
    }
}

void draw_eg_summary(int ch)
{
    for (int slot = 0; slot < OP_COUNT; ++slot) {
        ym_debug_operator op = {};
        uint8_t state = 0;
        if (YM_debug_get_operator(op_index(slot, ch), &op)) {
            state = op.eg_state;
        }
        const char c = (state == YM_EG_ATTACK) ? 'A' : (state == YM_EG_DECAY) ? 'D' :
                       (state == YM_EG_SUSTAIN) ? 'S' : (state == YM_EG_RELEASE) ? 'R' : '-';
        if (slot != 0) ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(ImColor(eg_color(state)), "%c", c);
    }
}

// A table column: header text, default width (0 = stretch), and the help shown
// when hovering the header. ImGui::TableHeadersRow() cannot attach a tooltip
// per header, so headers_with_help() draws the row itself.
struct ColHelp {
    const char *name;
    float       width;
    const char *help;
};

void setup_columns(const ColHelp *cols, int n)
{
    for (int i = 0; i < n; ++i) {
        if (cols[i].width > 0.0f) {
            ImGui::TableSetupColumn(cols[i].name, ImGuiTableColumnFlags_WidthFixed, cols[i].width);
        } else {
            ImGui::TableSetupColumn(cols[i].name, ImGuiTableColumnFlags_WidthStretch);
        }
    }
}

void headers_with_help(const ColHelp *cols, int n)
{
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int i = 0; i < n; ++i) {
        if (!ImGui::TableSetColumnIndex(i)) continue;
        ImGui::PushID(i);
        ImGui::TableHeader(ImGui::TableGetColumnName(i));
        if (cols[i].help) ImGui::SetItemTooltip("%s", cols[i].help);
        ImGui::PopID();
    }
}

void draw_channels_tab()
{
    // Channels are numbered 0-7 throughout this panel, matching the register
    // offsets ($20+ch, $28+ch, ...) and the channel field of the $08 key-on
    // register — which is what you cross-reference while debugging. (The OPM
    // datasheet numbers them 1-8.)
    ImGui::TextDisabled("Channels are 0-7, matching the $20+ch / $28+ch register offsets "
                        "and the $08 key-on channel field.");
    const uint8_t mute = YM_debug_get_mute_mask();
    if (dbgui_unmute_all_button(mute)) {
        YM_debug_set_mute_mask(0);
    }
    // Every column is fixed at the width of the widest thing it can ever hold
    // (and at least its header), so nothing is squeezed or clipped: stretch
    // columns used to share whatever was left and cut the note text short.
    // Multi-part cells are measured as drawn: four labels with 4 px between.
    const float gap4    = 3.0f * 4.0f;
    const float note_w  = dbgui_col_width("KC / note", "$7F  A#10 +100 c  13289.75 Hz !");
    const float keyon_w = dbgui_col_fit("Key-on", dbgui_text_width("M1M2C1C2") + gap4);
    const float eg_w    = dbgui_col_fit("EG", dbgui_text_width("DDDD") + gap4);
    const float level_w = dbgui_col_fit("Level", 80.0f); // dbgui_level_meter below is 80 px
    const ColHelp cols[12] = {
        { "",          ImGui::GetTextLineHeight() + 2.0f,
          "Host-output mute. Click a speaker to silence that channel on the\n"
          "speakers; the chip keeps playing it." },
        { "Ch",        dbgui_col_width("Ch", "00"),
          "Channel 0-7 (the OPM datasheet numbers them 1-8). Click a row to\n"
          "select it for the Operators, Algorithm and Scope tabs." },
        { "KC / note", note_w,
          "Key code, register $28+ch. Bits 6-4 are the octave; bits 3-0 the\n"
          "note, as codes 0-14 with every fourth code unused. Shown with the\n"
          "note it plays. A '!' marks an unused note code." },
        { "KF",        dbgui_col_width("KF", "63"),
          "Key fraction, register $30+ch bits 7-2: 0-63, in 64ths of a\n"
          "semitone above the key code's note. Fine pitch for vibrato,\n"
          "slides and tuning." },
        { "ALG",       dbgui_col_width("ALG", "0"),
          "Algorithm (connection), register $20+ch bits 2-0: how the four\n"
          "operators M1 C1 M2 C2 are wired, from 0 (one serial stack, one\n"
          "carrier) to 7 (four parallel sine carriers). Hover a value for\n"
          "its wiring; the Algorithm tab draws it." },
        { "FB",        dbgui_col_width("FB", "0"),
          "Feedback, register $20+ch bits 5-3: 0-7, how much of M1's own\n"
          "output modulates M1. 0 is a pure sine; higher is brighter and\n"
          "saw-like; 6-7 approach noise." },
        { "Pan",       dbgui_col_width("Pan", "L/R"),
          "Output routing, register $20+ch bit 6 (L) and bit 7 (R).\n"
          "L/R plays in both speakers, L/- left only, -/R right only,\n"
          "-/- nowhere (the channel still runs, but is not heard)." },
        { "PMS",       dbgui_col_width("PMS", "0"),
          "Phase modulation sensitivity, register $38+ch bits 6-4: 0-7, how\n"
          "far the LFO bends this channel's pitch (vibrato). 0 is off." },
        { "AMS",       dbgui_col_width("AMS", "0"),
          "Amplitude modulation sensitivity, register $38+ch bits 1-0: 0-3,\n"
          "how far the LFO moves the level of operators that have AMS-EN set\n"
          "(tremolo). 0 is off." },
        { "Key-on",    keyon_w,
          "Which operators are keyed on (register $08: M1 C1 M2 C2). Key-on\n"
          "starts an operator's attack and resets its phase; key-off starts\n"
          "its release." },
        { "EG",        eg_w,
          "Envelope state of each operator: A attack, D first decay (D1R),\n"
          "S second decay (D2R, after the D1L level), R release, - idle." },
        { "Level",     level_w,
          "Output level of the channel's carriers after TL and the envelope." },
    };
    // Fixed widths plus ScrollX: a window narrower than the table scrolls
    // sideways instead of crushing columns. ScrollX makes the table its own
    // child window, so it needs an explicit height - header plus eight rows,
    // each as tall as the mute button (text line + 2) plus cell padding.
    const ImGuiStyle &st = ImGui::GetStyle();
    const float row_h = ImGui::GetTextLineHeight() + 2.0f + st.CellPadding.y * 2.0f;
    const float tbl_h = row_h * (YM_NUM_CHANNELS + 1) + st.ScrollbarSize + 4.0f;
    // New ID on purpose: imgui.ini keeps per-table column widths by ID, and the
    // widths saved for the old stretch layout would override these defaults.
    if (ImGui::BeginTable("ym_channel_table", 12,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX,
                          ImVec2(0.0f, tbl_h))) {
        ImGui::TableSetupScrollFreeze(2, 1); // keep mute + channel number visible
        setup_columns(cols, 12);
        headers_with_help(cols, 12);
        for (int ch = 0; ch < YM_NUM_CHANNELS; ++ch) {
            const uint8_t r20 = ch_reg(0x20, ch);
            const uint8_t kc = ch_reg(0x28, ch) & 0x7f;
            const uint8_t kf = ch_reg(0x30, ch) & 0xfc;
            const uint8_t r38 = ch_reg(0x38, ch);
            NoteInfo note;
            note_from_kc_kf(kc, kf, &note);
            const bool silent = channel_effectively_silent(ch);

            ImGui::TableNextRow();
            const bool muted = (mute >> ch) & 1;
            const int mute_cols = dbgui_mute_row_begin(muted);
            if (silent) ImGui::BeginDisabled();
            // The selectable spans the whole row, so it must allow overlap for
            // the mute button (drawn last, in column 0) to get its clicks.
            ImGui::TableSetColumnIndex(1);
            char id[16];
            snprintf(id, sizeof(id), "Ch %d", ch);
            if (ImGui::Selectable(id, s_selected_ch == ch,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                s_selected_ch = ch;
            }
            ImGui::PopStyleColor(mute_cols);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("$%02X  %s", kc, note.text);
            if (!note.valid) ImGui::SameLine(), ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "!");
            ImGui::TableSetColumnIndex(3); ImGui::Text("%2u", (unsigned)(kf >> 2));
            ImGui::SetItemTooltip("%u/64 of a semitone above %s", (unsigned)(kf >> 2), note.text);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%u", (unsigned)(r20 & 7));
            ImGui::SetItemTooltip("ALG %u: %s", (unsigned)(r20 & 7), s_alg[r20 & 7].desc);
            ImGui::TableSetColumnIndex(5); ImGui::Text("%u", (unsigned)((r20 >> 3) & 7));
            ImGui::SetItemTooltip(((r20 >> 3) & 7) ? "M1 feeds back into itself at level %u of 7"
                                                   : "No feedback: M1 is a pure sine (%u)", (unsigned)((r20 >> 3) & 7));
            ImGui::TableSetColumnIndex(6); ImGui::Text("%c/%c", (r20 & 0x40) ? 'L' : '-', (r20 & 0x80) ? 'R' : '-');
            ImGui::SetItemTooltip("%s", ((r20 & 0xC0) == 0xC0) ? "Both speakers" : (r20 & 0x40) ? "Left speaker only"
                                       : (r20 & 0x80) ? "Right speaker only" : "Not routed to either speaker");
            ImGui::TableSetColumnIndex(7); ImGui::Text("%u", (unsigned)((r38 >> 4) & 7));
            ImGui::TableSetColumnIndex(8); ImGui::Text("%u", (unsigned)(r38 & 3));
            ImGui::TableSetColumnIndex(9); draw_key_mask(YM_debug_get_keyon(ch));
            ImGui::TableSetColumnIndex(10); draw_eg_summary(ch);
            ImGui::TableSetColumnIndex(11);
            snprintf(id, sizeof(id), "##lev%d", ch);
            dbgui_level_meter(id, channel_carrier_level(ch), ImVec2(80.0f, 0.0f), IM_COL32(80, 190, 110, 255));
            if (silent) ImGui::EndDisabled();

            // Outside the disabled span: muting a channel before it plays is a
            // normal thing to want.
            ImGui::TableSetColumnIndex(0);
            if (dbgui_mute_button(ch, muted)) {
                YM_debug_set_mute_mask((uint8_t)(YM_debug_get_mute_mask() ^ (1u << ch)));
            }
        }
        ImGui::EndTable();
    }
}

void draw_env_shape(int slot, int ch, ImVec2 size)
{
    ImGui::PushID(slot * 16 + ch);
    ImGui::InvisibleButton("##env", size);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(16, 18, 22, 255));
    dl->AddRect(p0, p1, IM_COL32(70, 78, 90, 255));

    const uint8_t tl = op_reg(0x60, slot, ch) & 0x7f;
    const uint8_t ar = op_reg(0x80, slot, ch) & 0x1f;
    const uint8_t d1r = op_reg(0xa0, slot, ch) & 0x1f;
    const uint8_t d2r = op_reg(0xc0, slot, ch) & 0x1f;
    const uint8_t d1l = (op_reg(0xe0, slot, ch) >> 4) & 0x0f;
    const uint8_t rr = op_reg(0xe0, slot, ch) & 0x0f;

    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    const float loud = (float)tl / 127.0f;
    const float sustain = (float)d1l / 15.0f;
    const float x1 = p0.x + w * (0.12f + (31 - ar) * 0.0025f);
    const float x2 = x1 + w * (0.14f + (31 - d1r) * 0.003f);
    const float x3 = x2 + w * (0.18f + (31 - d2r) * 0.002f);
    const float x4 = p1.x - w * (0.08f + (15 - rr) * 0.004f);
    const float y0 = p1.y - 2.0f;
    const float y_peak = p0.y + 2.0f + h * loud;
    const float y_sus = p0.y + 2.0f + h * (loud + (1.0f - loud) * sustain);
    ImVec2 pts[5] = { ImVec2(p0.x + 2.0f, y0), ImVec2(x1, y_peak), ImVec2(x2, y_sus), ImVec2(x3, y_sus), ImVec2(x4, y0) };
    dl->AddPolyline(pts, 5, IM_COL32(120, 190, 255, 255), false, 1.5f);
}

void draw_operator_row(int ch, int slot)
{
    const uint8_t r40 = op_reg(0x40, slot, ch);
    const uint8_t r60 = op_reg(0x60, slot, ch);
    const uint8_t r80 = op_reg(0x80, slot, ch);
    const uint8_t ra0 = op_reg(0xa0, slot, ch);
    const uint8_t rc0 = op_reg(0xc0, slot, ch);
    const uint8_t re0 = op_reg(0xe0, slot, ch);
    ym_debug_operator op = {};
    YM_debug_get_operator(op_index(slot, ch), &op);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0); ImGui::Text("Ch %d %s", ch, s_slot_names[slot]);
    ImGui::TableSetColumnIndex(1); ImGui::Text("%u", (unsigned)((r40 >> 4) & 7));
    ImGui::TableSetColumnIndex(2); ImGui::Text("%u", (unsigned)(r40 & 0x0f));
    ImGui::TableSetColumnIndex(3); ImGui::Text("%u", (unsigned)(r60 & 0x7f));
    ImGui::TableSetColumnIndex(4); ImGui::Text("%u", (unsigned)((r80 >> 6) & 3));
    ImGui::TableSetColumnIndex(5); ImGui::Text("%u", (unsigned)(r80 & 0x1f));
    ImGui::TableSetColumnIndex(6); ImGui::Text("%u", (unsigned)(ra0 & 0x1f));
    ImGui::TableSetColumnIndex(7); ImGui::Text("%u", (unsigned)((rc0 >> 6) & 3));
    ImGui::TableSetColumnIndex(8); ImGui::Text("%u", (unsigned)(rc0 & 0x1f));
    ImGui::TableSetColumnIndex(9); ImGui::Text("%u", (unsigned)((re0 >> 4) & 0x0f));
    ImGui::TableSetColumnIndex(10); ImGui::Text("%u", (unsigned)(re0 & 0x0f));
    ImGui::TableSetColumnIndex(11); ImGui::TextUnformatted((ra0 & 0x80) ? "yes" : "no");
    ImGui::TableSetColumnIndex(12); ImGui::TextColored(ImColor(eg_color(op.eg_state)), "%s", s_eg_names[(op.eg_state <= YM_EG_RELEASE) ? op.eg_state : 0]);
    ImGui::TableSetColumnIndex(13);
    char id[32];
    snprintf(id, sizeof(id), "##att%d_%d", ch, slot);
    dbgui_level_meter(id, operator_level(slot, ch), ImVec2(90.0f, 0.0f), IM_COL32(90, 170, 240, 255));
    ImGui::TableSetColumnIndex(14);
    draw_env_shape(slot, ch, ImVec2(110.0f, 28.0f));
}

void draw_operators_tab()
{
    static bool show_all = false;
    ImGui::Checkbox("Show all 32 operators grouped by channel", &show_all);
    ImGui::TextDisabled("Envelope sketches are qualitative ADSR/TL shapes, not cycle-accurate ymfm EG traces.");
    const int first_ch = show_all ? 0 : s_selected_ch;
    const int last_ch = show_all ? YM_NUM_CHANNELS : (s_selected_ch + 1);
    // FIXED COLUMN WIDTHS, fitted to each column's widest value. Declared with
    // no width, every column stretched to an equal share, so a one-digit DT1
    // got as much room as the envelope sketch and the sketch was clipped.
    // ScrollX lets a narrow window scroll instead of squeezing; the Slot
    // column stays frozen so each row stays identified.
    const ColHelp cols[15] = {
        { "Slot",   dbgui_col_width("Slot", "Ch 0 M1"),
          "Channel and operator. M1/M2 are modulators or carriers depending on\n"
          "ALG; C2 is always a carrier, and on channel 7 can be the noise." },
        { "DT1",    dbgui_col_width("DT1", "7"),
          "Detune 1, register $40 bits 6-4: a small pitch offset, 0-3 up and\n"
          "5-7 down (4 is 0). Two detuned operators beat into a chorus." },
        { "MUL",    dbgui_col_width("MUL", "15"),
          "Frequency multiple, register $40 bits 3-0: the operator runs at\n"
          "MUL x the note (0 means x0.5). On a modulator it sets which\n"
          "harmonics the carrier gets." },
        { "TL",     dbgui_col_width("TL", "127"),
          "Total level, register $60 bits 6-0: attenuation, 0.75 dB a step;\n"
          "0 is loudest, 127 silent. On a carrier it is volume; on a\n"
          "modulator it is brightness." },
        { "KS",     dbgui_col_width("KS", "3"),
          "Key scaling, register $80 bits 7-6: 0-3, how much higher notes\n"
          "speed up this operator's envelope rates." },
        { "AR",     dbgui_col_width("AR", "31"),
          "Attack rate, register $80 bits 4-0: 0-31, how fast the level rises\n"
          "after key-on. 31 is instant; 0 never starts." },
        { "D1R",    dbgui_col_width("D1R", "31"),
          "First decay rate, register $A0 bits 4-0: 0-31, how fast the level\n"
          "falls from the peak toward D1L. 0 holds." },
        { "DT2",    dbgui_col_width("DT2", "3"),
          "Detune 2, register $C0 bits 7-6: a coarse pitch offset for\n"
          "inharmonic, bell-like ratios. 0 is off." },
        { "D2R",    dbgui_col_width("D2R", "31"),
          "Second decay rate, register $C0 bits 4-0: 0-31, how fast the level\n"
          "keeps falling after it reaches D1L, while the key is held. 0 holds." },
        { "D1L",    dbgui_col_width("D1L", "15"),
          "First decay level, register $E0 bits 7-4: 0-15, the level where\n"
          "D1R hands over to D2R, 3 dB a step (15 is 93 dB down)." },
        { "RR",     dbgui_col_width("RR", "15"),
          "Release rate, register $E0 bits 3-0: 0-15, how fast the level falls\n"
          "after key-off. Higher is a shorter tail." },
        { "AMS-EN", dbgui_col_width("AMS-EN", "yes"),
          "Register $A0 bit 7: whether the LFO's amplitude modulation (the\n"
          "channel's AMS) applies to this operator." },
        { "EG state", dbgui_col_width("EG state", "Release"),
          "Where the envelope is now: Attack, Decay (D1R), Sustain (D2R)\n"
          "or Release." },
        { "Atten",  dbgui_col_fit("Atten", 90.0f),
          "Live attenuation: TL plus the envelope, as a level." },
        { "ADSR sketch", dbgui_col_fit("ADSR sketch", 110.0f),
          "A qualitative shape from AR, D1R, D1L, D2R, RR and TL - not a\n"
          "cycle-accurate trace." },
    };
    if (ImGui::BeginTable("ym_ops", 15,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE |
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        setup_columns(cols, 15);
        headers_with_help(cols, 15);
        for (int ch = first_ch; ch < last_ch; ++ch) {
            for (int slot = 0; slot < OP_COUNT; ++slot) draw_operator_row(ch, slot);
        }
        ImGui::EndTable();
    }
}

uint8_t source_mask(uint8_t selector)
{
    switch (selector) {
    case 1: return 0x1;
    case 2: return 0x2;
    case 3: return 0x4;
    case 5: return 0x3;
    case 6: return 0x5;
    case 7: return 0x6;
    default: return 0x0;
    }
}

void draw_arrow(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color)
{
    dl->AddLine(a, b, color, 2.0f);
    const float ang = atan2f(b.y - a.y, b.x - a.x);
    const float len = 8.0f;
    const ImVec2 p1(b.x - cosf(ang - 0.45f) * len, b.y - sinf(ang - 0.45f) * len);
    const ImVec2 p2(b.x - cosf(ang + 0.45f) * len, b.y - sinf(ang + 0.45f) * len);
    dl->AddTriangleFilled(b, p1, p2, color);
}

void draw_algorithm_tab()
{
    const int ch = s_selected_ch;
    const int alg = alg_for_ch(ch);
    const uint8_t fb = (ch_reg(0x20, ch) >> 3) & 7;
    ImGui::Text("Channel %d  ALG %d: %s", ch, alg, s_alg[alg].desc);
    ImGui::TextDisabled("Algorithm table is derived from ymfm_fm.ipp output_4op(). Box brightness follows live EG attenuation.");

    const ImVec2 canvas = ImVec2(ImGui::GetContentRegionAvail().x, 330.0f);
    ImGui::InvisibleButton("alg_canvas", canvas);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 20, 255));
    dl->AddRect(p0, p1, IM_COL32(70, 78, 90, 255));

    ImVec2 c[OP_COUNT] = {
        ImVec2(p0.x + canvas.x * 0.16f, p0.y + canvas.y * 0.42f),
        ImVec2(p0.x + canvas.x * 0.38f, p0.y + canvas.y * 0.25f),
        ImVec2(p0.x + canvas.x * 0.38f, p0.y + canvas.y * 0.62f),
        ImVec2(p0.x + canvas.x * 0.64f, p0.y + canvas.y * 0.42f),
    };
    const ImVec2 box(70.0f, 42.0f);
    const uint8_t carriers = carriers_for_alg(alg);
    const ImU32 mod_col = IM_COL32(140, 170, 220, 255);
    const ImU32 out_col = IM_COL32(110, 230, 130, 255);

    // s_alg[].inputs is indexed by ymfm operator (O1..O4) and source_mask()
    // yields an O-index mask, so both ends of each arrow are permuted into slot
    // space to match the box layout below.
    for (int dst_op = 1; dst_op < OP_COUNT; ++dst_op) {
        const uint8_t srcs = source_mask(s_alg[alg].inputs[dst_op]);
        const int dst = kOpToSlot[dst_op];
        for (int src_op = 0; src_op < OP_COUNT; ++src_op) {
            if ((srcs & (1u << src_op)) != 0) {
                const int src = kOpToSlot[src_op];
                draw_arrow(dl, ImVec2(c[src].x + box.x * 0.5f, c[src].y), ImVec2(c[dst].x - box.x * 0.5f, c[dst].y), mod_col);
            }
        }
    }
    if (fb != 0) {
        const ImVec2 a(c[0].x - 18.0f, c[0].y - box.y * 0.5f);
        const ImVec2 b(c[0].x + 18.0f, c[0].y - box.y * 0.5f);
        dl->AddBezierCubic(a, ImVec2(a.x - 45.0f, a.y - 60.0f), ImVec2(b.x + 45.0f, b.y - 60.0f), b, IM_COL32(235, 170, 80, 255), 2.0f);
        dl->AddText(ImVec2(c[0].x - 18.0f, a.y - 58.0f), IM_COL32(235, 185, 100, 255), "FB");
    }
    const ImVec2 out(p0.x + canvas.x * 0.86f, p0.y + canvas.y * 0.42f);
    for (int slot = 0; slot < OP_COUNT; ++slot) {
        if ((carriers & (1u << slot)) != 0) {
            draw_arrow(dl, ImVec2(c[slot].x + box.x * 0.5f, c[slot].y), out, out_col);
        }
    }
    dl->AddText(ImVec2(out.x + 8.0f, out.y - 8.0f), out_col, "OUT");

    for (int slot = 0; slot < OP_COUNT; ++slot) {
        const float lvl = operator_level(slot, ch);
        const int base = (int)(55.0f + lvl * 150.0f);
        const bool carrier = (carriers & (1u << slot)) != 0;
        const ImU32 fill = carrier ? IM_COL32(35, base, 65, 255) : IM_COL32(35, 55, base, 255);
        const ImVec2 b0(c[slot].x - box.x * 0.5f, c[slot].y - box.y * 0.5f);
        const ImVec2 b1(c[slot].x + box.x * 0.5f, c[slot].y + box.y * 0.5f);
        dl->AddRectFilled(b0, b1, fill, 5.0f);
        dl->AddRect(b0, b1, carrier ? out_col : mod_col, 5.0f, 0, 2.0f);
        char label[32];
        snprintf(label, sizeof(label), "%s\n%.0f%%", s_slot_names[slot], lvl * 100.0f);
        dl->AddText(ImVec2(b0.x + 8.0f, b0.y + 5.0f), IM_COL32(235, 240, 245, 255), label);
    }
}

void draw_global_tab()
{
    const uint8_t r0f = ym_reg(0x0f);
    const uint8_t r14 = ym_reg(0x14);
    const uint8_t r18 = ym_reg(0x18);
    const uint8_t r19 = ym_reg(0x19);
    const uint8_t r1a = ym_reg(0x1a);
    const uint8_t r1b = ym_reg(0x1b);
    const uint16_t ta = (uint16_t)((ym_reg(0x10) << 2) | (ym_reg(0x11) & 3));
    const uint8_t tb = ym_reg(0x12);
    const double ta_ms = (1024.0 - (double)ta) / YM_SAMPLE_HZ * 1000.0;
    const double tb_ms = (256.0 - (double)tb) * 16.0 / YM_SAMPLE_HZ * 1000.0;
    const uint8_t status = YM_debug_get_status();

    ImGui::Text("Clock: %.0f Hz   ymfm sample rate: %.3f Hz", YM_CLOCK_HZ, YM_SAMPLE_HZ);
    ImGui::Separator();
    ImGui::Text("LFO: rate $%02X, waveform %s, AM depth %u, PM depth %u", r18, s_lfo_waves[r1b & 3], (unsigned)(r19 & 0x7f), (unsigned)(r1a & 0x7f));
    ImGui::TextDisabled("$19 writes select AM (bit7=0) or PM (bit7=1); PM is shown from the shadowed internal $1A.");
    ImGui::Text("Noise: %s, frequency %u", (r0f & 0x80) ? "enabled" : "disabled", (unsigned)(r0f & 0x1f));
    ImGui::Text("CT pins: CT1=%u CT2=%u", (unsigned)((r1b >> 6) & 1), (unsigned)((r1b >> 7) & 1));
    ImGui::Separator();
    ImGui::Text("Timer A: %u  approx %.3f ms", (unsigned)ta, ta_ms);
    ImGui::Text("Timer B: %u  approx %.3f ms", (unsigned)tb, tb_ms);
    ImGui::Text("Mode $14: CSM=%u reset TB/TA=%u/%u enable TB/TA=%u/%u load TB/TA=%u/%u",
                (unsigned)((r14 >> 7) & 1), (unsigned)((r14 >> 5) & 1), (unsigned)((r14 >> 4) & 1),
                (unsigned)((r14 >> 3) & 1), (unsigned)((r14 >> 2) & 1), (unsigned)((r14 >> 1) & 1), (unsigned)(r14 & 1));
    ImGui::Separator();
    ImGui::Text("Status $%02X: Timer A ovf=%u  Timer B ovf=%u  busy bit=%u", status, (unsigned)(status & 1), (unsigned)((status >> 1) & 1), (unsigned)((status >> 7) & 1));
    ImGui::Text("YM_debug_is_busy(): %s", YM_debug_is_busy() ? "true" : "false");
}

const char *system_name(int reg)
{
    switch (reg) {
    case 0x01: return "TEST / LFO reset";
    case 0x08: return "Key-on";
    case 0x0f: return "Noise";
    case 0x10: return "Timer A high";
    case 0x11: return "Timer A low";
    case 0x12: return "Timer B";
    case 0x14: return "Timer control";
    case 0x18: return "LFO frequency";
    case 0x19: return "AM/PM depth write";
    case 0x1a: return "PM depth shadow";
    case 0x1b: return "CT / LFO wave";
    default: return "System";
    }
}

void decode_reg(int reg, uint8_t v, char *out, size_t outsz)
{
    if (reg < 0x20) {
        switch (reg) {
        case 0x01: snprintf(out, outsz, "test=$%02X LFO reset=%u", v, (unsigned)((v >> 1) & 1)); break;
        case 0x08: snprintf(out, outsz, "ch=%u raw op bits b3-b6=M1,C1,M2,C2", (unsigned)(v & 7)); break;
        case 0x0f: snprintf(out, outsz, "noise %s freq=%u", (v & 0x80) ? "on" : "off", (unsigned)(v & 0x1f)); break;
        case 0x14: snprintf(out, outsz, "CSM=%u en TB/TA=%u/%u load TB/TA=%u/%u", (unsigned)(v >> 7), (unsigned)((v >> 3) & 1), (unsigned)((v >> 2) & 1), (unsigned)((v >> 1) & 1), (unsigned)(v & 1)); break;
        case 0x18: snprintf(out, outsz, "LFO freq=%u", (unsigned)v); break;
        case 0x19: snprintf(out, outsz, "%s depth write=%u", (v & 0x80) ? "PM" : "AM", (unsigned)(v & 0x7f)); break;
        case 0x1b: snprintf(out, outsz, "CT=%u wave=%s", (unsigned)((v >> 6) & 3), s_lfo_waves[v & 3]); break;
        default: snprintf(out, outsz, "%s", system_name(reg)); break;
        }
    } else if (reg < 0x40) {
        const int ch = reg & 7;
        const int base = reg & 0xf8;
        if (base == 0x20) snprintf(out, outsz, "Ch%d pan L/R=%u/%u FB=%u ALG=%u", ch, (unsigned)((v >> 6) & 1), (unsigned)((v >> 7) & 1), (unsigned)((v >> 3) & 7), (unsigned)(v & 7));
        else if (base == 0x28) snprintf(out, outsz, "Ch%d KC=%u", ch, (unsigned)(v & 0x7f));
        else if (base == 0x30) snprintf(out, outsz, "Ch%d KF=%u", ch, (unsigned)(v >> 2));
        else if (base == 0x38) snprintf(out, outsz, "Ch%d PMS=%u AMS=%u", ch, (unsigned)((v >> 4) & 7), (unsigned)(v & 3));
        else snprintf(out, outsz, "Ch%d unused/shadow", ch);
    } else {
        const int off = reg - 0x40;
        const int group = reg & 0xe0;
        const int slot = (off >> 3) & 3;
        const int ch = off & 7;
        if (group == 0x40) snprintf(out, outsz, "%s Ch%d DT1=%u MUL=%u", s_slot_names[slot], ch, (unsigned)((v >> 4) & 7), (unsigned)(v & 0x0f));
        else if (group == 0x60) snprintf(out, outsz, "%s Ch%d TL=%u", s_slot_names[slot], ch, (unsigned)(v & 0x7f));
        else if (group == 0x80) snprintf(out, outsz, "%s Ch%d KS=%u AR=%u", s_slot_names[slot], ch, (unsigned)((v >> 6) & 3), (unsigned)(v & 0x1f));
        else if (group == 0xa0) snprintf(out, outsz, "%s Ch%d AMS-EN=%u D1R=%u", s_slot_names[slot], ch, (unsigned)((v >> 7) & 1), (unsigned)(v & 0x1f));
        else if (group == 0xc0) snprintf(out, outsz, "%s Ch%d DT2=%u D2R=%u", s_slot_names[slot], ch, (unsigned)((v >> 6) & 3), (unsigned)(v & 0x1f));
        else snprintf(out, outsz, "%s Ch%d D1L=%u RR=%u", s_slot_names[slot], ch, (unsigned)((v >> 4) & 0x0f), (unsigned)(v & 0x0f));
    }
}

void draw_register_range(const char *label, int first, int last)
{
    ImGui::TextUnformatted(label);
    if (ImGui::BeginTable(label, 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Reg", "$FF"));
        ImGui::TableSetupColumn("Layout", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Layout", "1111 1111"));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Name", "AMS-EN / D1R"));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Value", "$FF (255)"));
        ImGui::TableSetupColumn("Decode");
        ImGui::TableHeadersRow();
        for (int reg = first; reg <= last; ++reg) {
            const uint8_t v = ym_reg(reg);
            char bin[16];
            char layout[32];
            char name[64];
            char dec[160];
            dbgui_format_binary(bin, sizeof(bin), v, 8);
            if (reg < 0x20) snprintf(layout, sizeof(layout), "system");
            else if (reg < 0x40) snprintf(layout, sizeof(layout), "ch %d", (reg & 7));
            else snprintf(layout, sizeof(layout), "%s ch %d", s_slot_names[((reg - 0x40) >> 3) & 3], (reg & 7));
            if (reg < 0x20) snprintf(name, sizeof(name), "%s", system_name(reg));
            else if (reg < 0x40) snprintf(name, sizeof(name), "%s", ((reg & 0xf8) == 0x20) ? "Pan/FB/ALG" : ((reg & 0xf8) == 0x28) ? "KC" : ((reg & 0xf8) == 0x30) ? "KF" : ((reg & 0xf8) == 0x38) ? "PMS/AMS" : "unused");
            else snprintf(name, sizeof(name), "%s", ((reg & 0xe0) == 0x40) ? "DT1/MUL" : ((reg & 0xe0) == 0x60) ? "TL" : ((reg & 0xe0) == 0x80) ? "KS/AR" : ((reg & 0xe0) == 0xa0) ? "AMS-EN/D1R" : ((reg & 0xe0) == 0xc0) ? "DT2/D2R" : "D1L/RR");
            decode_reg(reg, v, dec, sizeof(dec));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("$%02X", reg); dbgui_hover_value_tooltip(dec, v, 1);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(layout);
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(3); ImGui::Text("$%02X  %s", v, bin); dbgui_hover_value_tooltip(dec, v, 1);
            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(dec);
        }
        ImGui::EndTable();
    }
}

void draw_registers_tab()
{
    ImGui::TextDisabled("Shadowed registers show only writes the chip accepted; YM_write_reg drops writes while the chip is busy.");
    ImGui::TextDisabled("Per-operator address layout is base + (slot << 3) + channel; slots are M1, M2, C1, C2. Raw $08 key-on bits are M1,C1,M2,C2, but live key masks are normalized.");
    if (ImGui::CollapsingHeader("$00-$1F system", ImGuiTreeNodeFlags_DefaultOpen)) draw_register_range("system_regs", 0x00, 0x1f);
    if (ImGui::CollapsingHeader("$20-$3F per channel", ImGuiTreeNodeFlags_DefaultOpen)) draw_register_range("channel_regs", 0x20, 0x3f);
    if (ImGui::CollapsingHeader("$40-$FF per operator", ImGuiTreeNodeFlags_DefaultOpen)) draw_register_range("operator_regs", 0x40, 0xff);
}

void draw_scope_tab()
{
    static DbgScopeControls ctl;
    static int16_t samples[YM_SCOPE_STREAMS][YM_SCOPE_SAMPLES];
    static unsigned counts[YM_SCOPE_STREAMS] = {};
    static bool captured = false; // buffers hold real audio, not a projection

    dbgui_scope_controls(&ctl, YM_SCOPE_SAMPLES);
    const bool held = dbgui_audio_hold();
    bool       predicted = false;

    if (!ctl.freeze && !held) {
        captured = false;
        for (int stream = 0; stream < YM_SCOPE_STREAMS; ++stream) {
            counts[stream] = YM_debug_scope_read(stream, samples[stream], YM_SCOPE_SAMPLES);
            if (counts[stream] > 0) captured = true;
        }
    } else if (held && !captured && !ctl.freeze) {
        // Envelopes, LFO and operator phase are live chip state, so the chip can
        // be run forward to show what it is about to emit.
        const unsigned n = YM_debug_scope_predict(samples, YM_SCOPE_SAMPLES);
        for (int stream = 0; stream < YM_SCOPE_STREAMS; ++stream) {
            counts[stream] = n;
        }
        predicted = true;
    }

    if (held) {
        if (predicted) {
            dbgui_audio_predicted_note("the YM2151's current envelope and phase state");
        } else {
            dbgui_audio_hold_note();
        }
    }
    ImGui::TextDisabled("Per-channel capture is self-armed only while this Scope tab reads it; hidden tabs let it disarm.");

    if (dbgui_unmute_all_button(YM_debug_get_mute_mask())) {
        YM_debug_set_mute_mask(0);
    }
    if (ImGui::BeginTable("ym_scope_grid", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        for (int ch = 0; ch < YM_NUM_CHANNELS; ++ch) {
            if ((ch % 2) == 0) ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(ch % 2);
            NoteInfo note;
            note_from_kc_kf(ch_reg(0x28, ch) & 0x7f, ch_reg(0x30, ch) & 0xfc, &note);
            char overlay[96];
            snprintf(overlay, sizeof(overlay), "Ch %d ALG %u %s", ch, (unsigned)alg_for_ch(ch), note.text);
            char id[32];
            snprintf(id, sizeof(id), "mini%d", ch);
            dbgui_scope(id, samples[ch], (int)counts[ch], ImVec2(0.0f, 58.0f), IM_COL32(90, 190, 245, 255), ctl.window, ctl.gain, ctl.trigger, overlay);
            if (ImGui::IsItemClicked()) s_selected_ch = ch;
            if (dbgui_scope_mute_toggle((YM_debug_get_mute_mask() >> ch) & 1)) {
                YM_debug_set_mute_mask((uint8_t)(YM_debug_get_mute_mask() ^ (1u << ch)));
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    char overlay[96];
    NoteInfo note;
    note_from_kc_kf(ch_reg(0x28, s_selected_ch) & 0x7f, ch_reg(0x30, s_selected_ch) & 0xfc, &note);
    snprintf(overlay, sizeof(overlay), "Selected Ch %d ALG %u %s", s_selected_ch, (unsigned)alg_for_ch(s_selected_ch), note.text);
    dbgui_scope("selected", samples[s_selected_ch], (int)counts[s_selected_ch], ImVec2(0.0f, 120.0f), IM_COL32(105, 225, 140, 255), ctl.window, ctl.gain, ctl.trigger, overlay);
    if (dbgui_scope_mute_toggle((YM_debug_get_mute_mask() >> s_selected_ch) & 1)) {
        YM_debug_set_mute_mask((uint8_t)(YM_debug_get_mute_mask() ^ (1u << s_selected_ch)));
    }
    ImGui::TextUnformatted("Summed YM output");
    dbgui_scope("mix_l", samples[YM_SCOPE_MIX_L], (int)counts[YM_SCOPE_MIX_L], ImVec2(0.0f, 84.0f), IM_COL32(245, 185, 90, 255), ctl.window, ctl.gain, ctl.trigger, "Mix L");
    dbgui_scope("mix_r", samples[YM_SCOPE_MIX_R], (int)counts[YM_SCOPE_MIX_R], ImVec2(0.0f, 84.0f), IM_COL32(245, 120, 160, 255), ctl.window, ctl.gain, ctl.trigger, "Mix R");
}

void fm_panel_render(bool *p_open)
{
    if (ImGui::Begin("YM2151", p_open)) {
        dbgui_window_zoom("ym2151");
        if (s_selected_ch < 0 || s_selected_ch >= YM_NUM_CHANNELS) s_selected_ch = 0;
        if (ImGui::BeginTabBar("ym2151_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Channels")) {
                draw_channels_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Operators")) {
                draw_operators_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Algorithm")) {
                draw_algorithm_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Global")) {
                draw_global_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Registers")) {
                draw_registers_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Scope")) {
                draw_scope_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    dbgui_window_end();
}

} // namespace

static DebugPanelRegistration s_reg("YM2151", fm_panel_render, true);
