// YM2151 panel — live FM synthesis inspection for the Commander X16.
//
// Tabs:
//   * Channels  — the 8 OPM channels: note, pan, algorithm, key-on, EG and level.
//   * Operators — all parameters of the M1/M2/C1/C2 slots of the lit channels.
//   * Algorithm — live connection diagrams for the lit channels' algorithms.
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
#include "audio_decode.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr double YM_CLOCK_HZ = AUDIO_YM_CLOCK_HZ;
constexpr double YM_SAMPLE_HZ = AUDIO_YM_SAMPLE_HZ;
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
// The datasheet's names for the four operator slots. They are names, not roles:
// which operators are heard (carriers) depends on ALG. C2 is a carrier in every
// algorithm, and in ALG 7 all four are.
static const char *s_slot_long[OP_COUNT] = { "Modulator 1", "Modulator 2", "Carrier 1", "Carrier 2" };
static const char *s_slot_legend =
    "M1, M2, C1, C2 are the datasheet's names for a channel's four operators\n"
    "(Modulator 1/2, Carrier 1/2). They are names, not roles: ALG decides\n"
    "which ones are heard (carriers) and which only shape another's tone\n"
    "(modulators). C2 is always heard; in ALG 7 all four are.";
static const char *s_eg_names[5] = { "-", "Attack", "Decay", "Sustain", "Release" };
static const char *s_lfo_waves[4] = { "saw", "square", "triangle", "noise" };

inline uint8_t ym_reg(int reg) { return YM_debug_get_reg((uint8_t)reg); }
inline uint8_t ch_reg(int base, int ch) { return ym_reg(base + ch); }
inline uint8_t op_reg(int base, int slot, int ch) { return ym_reg(base + slot * 8 + ch); }
inline int op_index(int slot, int ch) { return slot * 8 + ch; }

using NoteInfo = audio_ym_note;

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

// The operator's live output level: envelope plus total level, as ymfm's
// envelope_attenuation() combines them (TL is 0.75 dB a step, the envelope
// 0.09375 dB a unit, so TL counts 8 units). The debugger bridge's
// eg_attenuation is the envelope ALONE, so TL has to be added here or a
// carrier at TL 127 would read as full volume. LFO AM is not included.
float operator_level(int slot, int ch)
{
    ym_debug_operator op = {};
    if (!YM_debug_get_operator(op_index(slot, ch), &op)) {
        return 0.0f;
    }
    int att = (int)op.eg_attenuation + ((op_reg(0x60, slot, ch) & 0x7f) << 3);
    if (att > 0x3ff) att = 0x3ff;
    return clamp01(1.0f - (float)att / 1023.0f);
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
    audio_ym_note_decode(kc, kf, out);
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
        ImGui::SetItemTooltip("%s (%s): %s", s_slot_names[slot], s_slot_long[slot],
                              (mask & (1u << slot)) ? "keyed on" : "keyed off");
    }
}

// Channel toggles for the tabs that show some channels: one button per channel,
// lit when shown, plus All / None. The row never goes away, so the set can
// always be changed. Turning a channel on also makes it the selected channel
// that the other tabs follow. A channel whose operators are all silent is
// drawn dimmed, like its Channels row.
void draw_channel_toggles(uint8_t *mask)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Channels");
    ImGui::SetItemTooltip("Click channels to show or hide them; any number can be lit.");
    const ImVec4 lit = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    for (int ch = 0; ch < YM_NUM_CHANNELS; ++ch) {
        ImGui::SameLine();
        char id[8];
        snprintf(id, sizeof(id), "%d", ch);
        const bool on = (*mask >> ch) & 1;
        const bool quiet = channel_effectively_silent(ch);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, lit);
        if (quiet) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
        if (ImGui::Button(id)) {
            *mask ^= (uint8_t)(1u << ch);
            if (!on) s_selected_ch = ch;
        }
        if (quiet) ImGui::PopStyleVar();
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Channel %d, ALG %u%s - click to %s", ch, (unsigned)alg_for_ch(ch),
                              quiet ? " (silent)" : "", on ? "hide" : "show");
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::SmallButton("All")) *mask = 0xff;
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) *mask = 0x00;
}

// Seeds a toggle set from the selected channel the first time a tab is shown.
inline void seed_toggles(uint8_t *mask, bool *seeded)
{
    if (!*seeded) {
        *mask = (uint8_t)(1u << s_selected_ch);
        *seeded = true;
    }
}

inline int popcount8(uint8_t m)
{
    int n = 0;
    for (; m; m &= (uint8_t)(m - 1)) ++n;
    return n;
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
    ImGui::TextDisabled("Operators: M1/M2 = Modulator 1/2, C1/C2 = Carrier 1/2 (hover for more).");
    ImGui::SetItemTooltip("%s", s_slot_legend);
    const uint8_t mute = YM_debug_get_mute_mask();
    const uint8_t client_mute = YM_debug_get_client_mute_mask();
    if (dbgui_unmute_all_button(mute)) {
        YM_debug_set_mute_mask(0);
    }
    dbgui_client_mute_note(client_mute);
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
          "Which of the channel's four operators are keyed on (bright) or off\n"
          "(dim): M1/M2 = Modulator 1/2, C1/C2 = Carrier 1/2. Set by register\n"
          "$08, whose bits run M1 C1 M2 C2. Key-on starts an operator's attack\n"
          "and resets its phase; key-off starts its release." },
        { "EG",        eg_w,
          "Envelope state of each operator: A attack, D first decay (D1R),\n"
          "S second decay (D2R, after the D1L level), R release, - idle." },
        { "Level",     level_w,
          "Output level of the channel's loudest carrier: TL plus the envelope\n"
          "(LFO tremolo not included)." },
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
            const bool client_muted = (client_mute >> ch) & 1;
            const int mute_cols = dbgui_mute_row_begin(muted, client_muted);
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
            if (dbgui_mute_button(ch, muted, client_muted)) {
                YM_debug_set_mute_mask((uint8_t)(YM_debug_get_mute_mask() ^ (1u << ch)));
            }
        }
        ImGui::EndTable();
    }
}

// ---- Envelope timing model -------------------------------------------------
// Mirrors ymfm's OPM envelope generator (ymfm_fm.ipp clock_envelope(),
// ymfm_opm.cpp cache_operator_data()) closely enough to give real phase times:
//
//   * The EG steps once every 3 samples (EG_CLOCK_DIVIDER = 3).
//   * 6-bit rates: AR, D1R, D2R are reg*2, RR is reg*4+2, each plus the key
//     scaling bonus (KC>>2) >> (KS^3), clamped to 63; a register value of 0
//     stays 0 (the phase never moves).
//   * At rate r, attenuation updates every 2^(11 - r/4) EG steps (every step
//     from r=44 up), adding the next nibble of s_eg_inc[r] — so the time halves
//     every 4 rate steps, i.e. every 2 steps of AR/D1R/D2R.
//   * Attack is exponential: att -= ceil((att+1) * inc / 16). Rates 62-63 start
//     at full volume on key-on.
//   * D1L 0-14 is a level of D1L*32 units (3 dB a step); 15 means 31*32.
//
// The starting phase of the global EG counter is unknowable from here, so a
// slow rate's first update can land up to one interval early or late.
constexpr double EG_STEP_HZ = YM_SAMPLE_HZ / 3.0;

// Copy of ymfm's attenuation_increment() table (ymfm_fm.ipp).
static const uint32_t s_eg_inc[64] = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010, 0x10101010, 0x10101010, 0x11101110, 0x11101110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242, 0x44424442,
    0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888,
};

inline int eg_inc(int rate, int idx) { return (int)((s_eg_inc[rate] >> (4 * idx)) & 0xf); }

// EG steps between attenuation updates at this rate.
inline double eg_update_interval(int rate)
{
    const int shift = rate >> 2;
    return (shift >= 11) ? 1.0 : (double)(1u << (11 - shift));
}

// Average attenuation units added per EG step by a linear (non-attack) phase.
double eg_units_per_step(int rate)
{
    if (rate <= 0) return 0.0;
    int sum = 0;
    for (int i = 0; i < 8; ++i) sum += eg_inc(rate, i);
    return ((double)sum / 8.0) / eg_update_interval(rate);
}

// Milliseconds for a linear phase to cover `units`; negative means never.
double eg_linear_ms(int rate, int units)
{
    if (units <= 0) return 0.0;
    const double ups = eg_units_per_step(rate);
    if (ups <= 0.0) return -1.0;
    return (double)units / ups / EG_STEP_HZ * 1000.0;
}

// Milliseconds for the attack to go from silence to full; negative means never.
double eg_attack_ms(int rate)
{
    if (rate >= 62) return 0.0;
    if (eg_units_per_step(rate) <= 0.0) return -1.0;
    int att = 0x3ff;
    int updates = 0;
    for (int idx = 0; att > 0 && updates < 100000; idx = (idx + 1) & 7, ++updates) {
        att -= ((att + 1) * eg_inc(rate, idx) + 15) >> 4;
        if (att < 0) att = 0;
    }
    return (double)updates * eg_update_interval(rate) / EG_STEP_HZ * 1000.0;
}

struct EnvModel {
    int    tl_units;       // TL in envelope units (TL*8)
    int    sustain_units;  // D1L as an envelope level
    int    rates[4];       // effective 6-bit AR, D1R, D2R, RR
    double attack_ms;      // silence -> peak
    double decay_ms;       // peak -> D1L
    double sustain_ms;     // D1L -> silence while held
    double release_ms;     // D1L -> silence after key-off
    double d2r_db_per_s;   // how fast the held level keeps falling
};

int eg_effective(int raw, int ksr) { return (raw == 0) ? 0 : ((raw + ksr > 63) ? 63 : raw + ksr); }

void env_model(int slot, int ch, EnvModel *m)
{
    const uint8_t tl  = op_reg(0x60, slot, ch) & 0x7f;
    const uint8_t r80 = op_reg(0x80, slot, ch);
    const uint8_t d1r = op_reg(0xa0, slot, ch) & 0x1f;
    const uint8_t d2r = op_reg(0xc0, slot, ch) & 0x1f;
    const uint8_t re0 = op_reg(0xe0, slot, ch);
    const int keycode = (ch_reg(0x28, ch) & 0x7f) >> 2;
    const int ksr = keycode >> (((r80 >> 6) & 3) ^ 3);
    int d1l = (re0 >> 4) & 0x0f;
    if (d1l == 15) d1l = 31;

    m->tl_units = tl << 3;
    m->sustain_units = d1l << 5;
    m->rates[0] = eg_effective((r80 & 0x1f) * 2, ksr);
    m->rates[1] = eg_effective(d1r * 2, ksr);
    m->rates[2] = eg_effective(d2r * 2, ksr);
    m->rates[3] = eg_effective((re0 & 0x0f) * 4 + 2, ksr);
    m->attack_ms  = eg_attack_ms(m->rates[0]);
    m->decay_ms   = eg_linear_ms(m->rates[1], m->sustain_units);
    m->sustain_ms = eg_linear_ms(m->rates[2], 0x3ff - m->sustain_units);
    m->release_ms = eg_linear_ms(m->rates[3], 0x3ff - m->sustain_units);
    m->d2r_db_per_s = eg_units_per_step(m->rates[2]) * EG_STEP_HZ * 0.09375;
}

void format_ms(char *out, size_t n, double ms)
{
    if (ms < 0.0)          snprintf(out, n, "never");
    else if (ms < 0.05)    snprintf(out, n, "instant");
    else if (ms < 1000.0)  snprintf(out, n, "%.1f ms", ms);
    else                   snprintf(out, n, "%.2f s", ms / 1000.0);
}

// Width share of a phase: log of its real time, so 1 ms and 10 s both fit and
// the same time always draws the same width in every operator's sketch.
float env_time_frac(double ms)
{
    if (ms < 0.0) return 1.0f;
    const double f = log1p(ms / 2.0) / log1p(10000.0 / 2.0);
    return (float)(f > 1.0 ? 1.0 : f);
}

// Envelope sketch. X is time on a log scale per phase (attack | decay to D1L |
// a fixed one-second hold | release), Y is attenuation in dB, 0 dB at the top
// and the chip's 96 dB floor at the bottom, with TL included. The dot is the
// operator's live envelope position. Exact times are in the tooltip.
void draw_env_shape(int slot, int ch, ImVec2 size)
{
    ImGui::PushID(slot * 16 + ch);
    ImGui::InvisibleButton("##env", size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(16, 18, 22, 255));
    dl->AddRect(p0, p1, IM_COL32(70, 78, 90, 255));

    EnvModel m;
    env_model(slot, ch, &m);

    const float l = p0.x + 2.0f, r = p1.x - 2.0f;
    const float t = p0.y + 2.0f, b = p1.y - 2.0f;
    const float w = r - l;
    auto y_of = [&](int env) {
        int a = env + m.tl_units;
        if (a > 0x3ff) a = 0x3ff;
        return t + (b - t) * (float)a / 1023.0f;
    };

    // Phase widths: attack, decay and release share the box by log time; the
    // hold is a fixed slice showing one second of D2R.
    const float xa = l + w * 0.22f * env_time_frac(m.attack_ms);
    const float xd = xa + w * 0.24f * env_time_frac(m.decay_ms);
    const float xs = xd + w * 0.22f;
    const float xr = xs + w * 0.32f * env_time_frac(m.release_ms);

    const ImU32 line = IM_COL32(120, 190, 255, 255);
    ImVec2 seg[4][2];
    int nseg = 0;
    if (m.attack_ms < 0.0) {
        // AR 0: the level never rises, so the operator stays silent.
        seg[nseg][0] = ImVec2(l, b); seg[nseg][1] = ImVec2(r, b); ++nseg;
    } else {
        // Sustain level reached after a second of D2R (clamped at silence).
        double held = (double)m.sustain_units;
        int peak_env = 0, hold_env;
        if (m.decay_ms < 0.0) {
            // D1R 0: the level stays at the peak and never reaches D1L.
            hold_env = 0;
        } else {
            held += eg_units_per_step(m.rates[2]) * EG_STEP_HZ;
            hold_env = (held > 1023.0) ? 0x3ff : (int)held;
        }
        const int hold_start = (m.decay_ms < 0.0) ? peak_env : m.sustain_units;
        seg[nseg][0] = ImVec2(l, b);              seg[nseg][1] = ImVec2(xa, y_of(peak_env));  ++nseg;
        seg[nseg][0] = seg[nseg - 1][1];          seg[nseg][1] = ImVec2(xd, y_of(hold_start)); ++nseg;
        seg[nseg][0] = seg[nseg - 1][1];          seg[nseg][1] = ImVec2(xs, y_of(hold_env));   ++nseg;
        seg[nseg][0] = seg[nseg - 1][1];          seg[nseg][1] = ImVec2(xr, b);                ++nseg;
        // Key-off marker.
        dl->AddLine(ImVec2(xs, t), ImVec2(xs, b), IM_COL32(90, 96, 110, 255), 1.0f);
    }
    for (int i = 0; i < nseg; ++i) dl->AddLine(seg[i][0], seg[i][1], line, 1.5f);

    // Live position: the point on the current phase's segment at the
    // operator's actual envelope level.
    ym_debug_operator op = {};
    if (nseg == 4 && YM_debug_get_operator(op_index(slot, ch), &op) &&
        op.eg_state >= YM_EG_ATTACK && op.eg_state <= YM_EG_RELEASE && op.eg_attenuation < 0x3ff) {
        const int s = op.eg_state - YM_EG_ATTACK;
        const float y0 = seg[s][0].y, y1 = seg[s][1].y;
        const float y = y_of(op.eg_attenuation);
        const float f = (fabsf(y1 - y0) > 0.5f) ? clamp01((y - y0) / (y1 - y0)) : 0.5f;
        const ImVec2 dot(seg[s][0].x + (seg[s][1].x - seg[s][0].x) * f, y);
        dl->AddCircleFilled(dot, 2.5f, eg_color(op.eg_state));
    }

    if (hovered && ImGui::BeginTooltip()) {
        char a[24], d[24], s[24], rr[24];
        format_ms(a, sizeof(a), m.attack_ms);
        format_ms(d, sizeof(d), m.decay_ms);
        format_ms(s, sizeof(s), m.sustain_ms);
        format_ms(rr, sizeof(rr), m.release_ms);
        ImGui::Text("Ch %d %s (%s) envelope", ch, s_slot_names[slot], s_slot_long[slot]);
        ImGui::Separator();
        ImGui::Text("Attack   %-10s silence to peak", a);
        ImGui::Text("Decay    %-10s peak to D1L (-%.0f dB)", d, (double)m.sustain_units * 0.09375);
        if (m.d2r_db_per_s > 0.0)
            ImGui::Text("Hold     %-10s D1L to silence, %.1f dB/s while held", s, m.d2r_db_per_s);
        else
            ImGui::Text("Hold     stays at D1L while the key is held (D2R 0)");
        ImGui::Text("Release  %-10s D1L to silence after key-off", rr);
        ImGui::Text("Peak     -%.2f dB (TL)", (double)m.tl_units * 0.09375);
        ImGui::Separator();
        ImGui::TextDisabled("Times from ymfm's EG rates, incl. key scaling at the\n"
                            "current note. Width is log time; the slice after the\n"
                            "grey key-off line shows the release. Dot = live position.");
        ImGui::EndTooltip();
    }
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
    draw_env_shape(slot, ch, ImVec2(160.0f, 30.0f));
}

void draw_operators_tab()
{
    static uint8_t shown = 0;
    static bool seeded = false;
    seed_toggles(&shown, &seeded);
    draw_channel_toggles(&shown);
    ImGui::TextDisabled("Envelope: attack | decay to D1L | 1 s held | release. Time is log scale, level in dB. "
                        "Hover for exact times.");
    // FIXED COLUMN WIDTHS, fitted to each column's widest value. Declared with
    // no width, every column stretched to an equal share, so a one-digit DT1
    // got as much room as the envelope sketch and the sketch was clipped.
    // ScrollX lets a narrow window scroll instead of squeezing; the Slot
    // column stays frozen so each row stays identified.
    const ColHelp cols[15] = {
        { "Slot",   dbgui_col_width("Slot", "Ch 0 M1"),
          "Channel and operator: M1/M2 = Modulator 1/2, C1/C2 = Carrier 1/2.\n"
          "Those are slot names; ALG decides which are actually heard. C2 is\n"
          "always a carrier, and on channel 7 can be the noise." },
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
          "Live output level: TL plus the envelope (LFO tremolo not included)." },
        { "Envelope", dbgui_col_fit("Envelope", 160.0f),
          "The envelope from AR, D1R, D1L, D2R, RR, TL and KS at the current\n"
          "note: attack | decay to D1L | one second held | release, on a log\n"
          "time scale with level in dB. The dot is the live position. Hover a\n"
          "sketch for its phase times." },
    };
    // New ID: imgui.ini keeps column widths by table ID, and the old ID's
    // saved width would keep the sketch column at its former size.
    if (ImGui::BeginTable("ym_ops_v2", 15,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE |
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        setup_columns(cols, 15);
        headers_with_help(cols, 15);
        for (int ch = 0; ch < YM_NUM_CHANNELS; ++ch) {
            if (!((shown >> ch) & 1)) continue;
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

// Slot mask of the operators that `slot` feeds (modulates) in this algorithm.
uint8_t alg_targets_of(int alg, int slot)
{
    uint8_t mask = 0;
    for (int dst_op = 1; dst_op < OP_COUNT; ++dst_op) {
        const uint8_t srcs = source_mask(s_alg[alg].inputs[dst_op]);
        for (int src_op = 0; src_op < OP_COUNT; ++src_op) {
            if ((srcs & (1u << src_op)) && kOpToSlot[src_op] == slot) mask |= (uint8_t)(1u << kOpToSlot[dst_op]);
        }
    }
    return mask;
}

void append_slot_list(char *out, size_t n, uint8_t mask)
{
    bool first = true;
    for (int s = 0; s < OP_COUNT; ++s) {
        if (!(mask & (1u << s))) continue;
        const size_t len = strlen(out);
        snprintf(out + len, n - len, "%s%s", first ? "" : ", ", s_slot_names[s]);
        first = false;
    }
}

void op_box_tooltip(int ch, int alg, int slot, uint8_t fb)
{
    const bool carrier = (carriers_for_alg(alg) & (1u << slot)) != 0;
    const uint8_t feeds = alg_targets_of(alg, slot);
    uint8_t fed_by = 0;
    for (int s = 0; s < OP_COUNT; ++s) {
        if (alg_targets_of(alg, s) & (1u << slot)) fed_by |= (uint8_t)(1u << s);
    }
    ym_debug_operator op = {};
    YM_debug_get_operator(op_index(slot, ch), &op);
    const uint8_t st = (op.eg_state <= YM_EG_RELEASE) ? op.eg_state : 0;

    ImGui::BeginTooltip();
    ImGui::Text("%s - %s, channel %d", s_slot_names[slot], s_slot_long[slot], ch);
    char buf[64] = "";
    if (carrier) {
        ImGui::TextUnformatted("Carrier: its output is heard.");
    } else {
        append_slot_list(buf, sizeof(buf), feeds);
        ImGui::Text("Modulator: not heard; bends the tone of %s.", buf);
    }
    buf[0] = '\0';
    append_slot_list(buf, sizeof(buf), fed_by);
    if (fed_by) ImGui::Text("Modulated by %s.", buf);
    if (slot == 0 && fb) ImGui::Text("Feeds back into itself (FB %u).", (unsigned)fb);
    ImGui::Text("Level %.0f%% (%s), TL %u, MUL %u", operator_level(slot, ch) * 100.0f,
                s_eg_names[st], (unsigned)(op_reg(0x60, slot, ch) & 0x7f), (unsigned)(op_reg(0x40, slot, ch) & 0x0f));
    ImGui::EndTooltip();
}

// Box positions for each algorithm in grid cells, indexed by slot (M1, M2, C1,
// C2). Signal flows left to right, every carrier is the rightmost box in its
// row so its line to the output bus crosses nothing, and M1 has open space
// above it for the feedback loop.
struct AlgLayout {
    float col[OP_COUNT];
    float row[OP_COUNT];
    int cols;
    int rows;
};

static const AlgLayout s_alg_layout[8] = {
    // col: M1    M2    C1    C2        row: M1    M2    C1    C2
    {{ 0.0f, 2.0f, 1.0f, 3.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, 4, 1},  // M1 -> C1 -> M2 -> C2
    {{ 0.0f, 1.0f, 0.0f, 2.0f }, { 0.0f, 0.5f, 1.0f, 0.5f }, 3, 2},  // (M1 + C1) -> M2 -> C2
    {{ 1.0f, 1.0f, 0.0f, 2.0f }, { 0.0f, 1.0f, 1.0f, 0.5f }, 3, 2},  // (M1 + (C1 -> M2)) -> C2
    {{ 0.0f, 1.0f, 1.0f, 2.0f }, { 0.0f, 1.0f, 0.0f, 0.5f }, 3, 2},  // ((M1 -> C1) + M2) -> C2
    {{ 0.0f, 0.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, 2, 2},  // (M1 -> C1) + (M2 -> C2)
    {{ 0.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 0.0f, 2.0f }, 2, 3},  // M1 -> C1, M2, C2
    {{ 0.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 2.0f }, 2, 3},  // (M1 -> C1) + M2 + C2
    {{ 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 2.0f, 3.0f }, 1, 4},  // M1 + M2 + C1 + C2
};

// Where each channel's boxes and output bus were last drawn, relative to the canvas.
static ImVec2 s_box_pos[YM_NUM_CHANNELS][OP_COUNT];
static float  s_bus_x[YM_NUM_CHANNELS];
static bool   s_layout_valid[YM_NUM_CHANNELS];

// The connection diagram for one channel. `compact` drops the per-box level
// text so eight fit on screen; `titled` labels the canvas with its channel,
// for when several are on screen. Returns true when clicked.
bool draw_alg_diagram(int ch, ImVec2 canvas, bool compact, bool titled = false)
{
    const int alg = alg_for_ch(ch);
    const uint8_t fb = (ch_reg(0x20, ch) >> 3) & 7;
    ImGui::PushID(ch);
    const bool clicked = ImGui::InvisibleButton("alg_canvas", canvas);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 20, 255));
    dl->AddRect(p0, p1, (titled && ch == s_selected_ch) ? IM_COL32(150, 170, 210, 255) : IM_COL32(70, 78, 90, 255));
    if (titled) {
        char title[32];
        snprintf(title, sizeof(title), "Ch %d  ALG %d", ch, alg);
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), IM_COL32(200, 205, 215, 255), title);
    }

    const AlgLayout &lay = s_alg_layout[alg];
    const ImVec2 box = compact ? ImVec2(40.0f, 22.0f) : ImVec2(70.0f, 42.0f);
    const float fb_lift = compact ? 22.0f : 60.0f;
    const float fb_peak = fb_lift * 0.75f;  // a cubic Bezier rises 3/4 of its control points' lift
    const float fb_label_h = compact ? 0.0f : 14.0f;

    // Rows share the height left under the title and M1's feedback loop.
    const float top = (titled ? 20.0f : 4.0f) + fb_peak + fb_label_h + box.y * 0.5f;
    const float bottom = canvas.y - 6.0f - box.y * 0.5f;
    const float row_pitch = lay.rows > 1 ? fminf((bottom - top) / (float)(lay.rows - 1), box.y * 2.2f) : 0.0f;
    const float row0 = 0.5f * (top + bottom) - 0.5f * row_pitch * (float)(lay.rows - 1);

    // Columns, output bus, OUT arrow and label, centred as one block.
    const float left_room = 10.0f;  // the feedback loop bulges past a compact M1's sides
    const float bus_gap = compact ? 12.0f : 18.0f;
    const float out_len = compact ? 26.0f : 44.0f;
    const float label_w = ImGui::CalcTextSize("OUT").x + 8.0f;
    const float fixed_w = left_room + box.x + bus_gap + out_len + label_w;
    const float col_pitch = lay.cols > 1
        ? fmaxf(box.x + 16.0f, fminf((canvas.x - fixed_w) / (float)(lay.cols - 1), box.x * 2.4f))
        : 0.0f;
    const float block_w = fixed_w + col_pitch * (float)(lay.cols - 1);
    const float col0 = fmaxf(0.0f, 0.5f * (canvas.x - block_w)) + left_room + box.x * 0.5f;
    const float bus_target = col0 + col_pitch * (float)(lay.cols - 1) + box.x * 0.5f + bus_gap;

    // Glide toward the layout rather than jump, so a box can be followed when a
    // channel changes ALG. Positions are kept relative to the canvas.
    const float k = s_layout_valid[ch] ? 1.0f - expf(-ImGui::GetIO().DeltaTime * 18.0f) : 1.0f;
    ImVec2 c[OP_COUNT];
    for (int s = 0; s < OP_COUNT; ++s) {
        ImVec2 &cur = s_box_pos[ch][s];
        cur.x += (col0 + lay.col[s] * col_pitch - cur.x) * k;
        cur.y += (row0 + lay.row[s] * row_pitch - cur.y) * k;
        c[s] = ImVec2(p0.x + cur.x, p0.y + cur.y);
    }
    s_bus_x[ch] += (bus_target - s_bus_x[ch]) * k;
    s_layout_valid[ch] = true;
    const float bus_x = p0.x + s_bus_x[ch];

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
        dl->AddBezierCubic(a, ImVec2(a.x - 45.0f, a.y - fb_lift), ImVec2(b.x + 45.0f, b.y - fb_lift), b, IM_COL32(235, 170, 80, 255), 2.0f);
        if (!compact) dl->AddText(ImVec2(c[0].x - 8.0f, a.y - fb_peak - 14.0f), IM_COL32(235, 185, 100, 255), "FB");
    }
    // Each carrier runs straight right into a vertical output bus, which feeds
    // OUT. The layouts keep every carrier rightmost in its row, so no output
    // line passes behind a box.
    float y_min = 0.0f, y_max = 0.0f;
    bool any = false;
    for (int slot = 0; slot < OP_COUNT; ++slot) {
        if ((carriers & (1u << slot)) == 0) continue;
        dl->AddLine(ImVec2(c[slot].x + box.x * 0.5f, c[slot].y), ImVec2(bus_x, c[slot].y), out_col, 2.0f);
        y_min = any ? fminf(y_min, c[slot].y) : c[slot].y;
        y_max = any ? fmaxf(y_max, c[slot].y) : c[slot].y;
        any = true;
    }
    if (y_max > y_min) dl->AddLine(ImVec2(bus_x, y_min - 1.0f), ImVec2(bus_x, y_max + 1.0f), out_col, 2.0f);
    const ImVec2 out(bus_x + out_len, 0.5f * (y_min + y_max));
    draw_arrow(dl, ImVec2(bus_x, out.y), out, out_col);
    dl->AddText(ImVec2(out.x + 6.0f, out.y - 8.0f), out_col, "OUT");

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
        if (compact) snprintf(label, sizeof(label), "%s", s_slot_names[slot]);
        else         snprintf(label, sizeof(label), "%s\n%.0f%%", s_slot_names[slot], lvl * 100.0f);
        dl->AddText(ImVec2(b0.x + (compact ? 5.0f : 8.0f), b0.y + (compact ? 4.0f : 5.0f)), IM_COL32(235, 240, 245, 255), label);
        if (hovered && ImGui::IsMouseHoveringRect(b0, b1)) op_box_tooltip(ch, alg, slot, fb);
    }
    return clicked;
}

void draw_algorithm_tab()
{
    static uint8_t shown = 0;
    static bool seeded = false;
    seed_toggles(&shown, &seeded);
    draw_channel_toggles(&shown);
    ImGui::TextDisabled("M1/M2 = Modulator 1/2, C1/C2 = Carrier 1/2. Green boxes are heard (carriers); "
                        "blue ones only shape another's tone. Brightness = live level. Hover a box.");
    ImGui::SetItemTooltip("%s", s_slot_legend);

    const int n = popcount8(shown);
    if (n == 0) {
        ImGui::TextDisabled("No channels lit - click a channel number above.");
        return;
    }
    if (n == 1) {
        int ch = 0;
        while (!((shown >> ch) & 1)) ++ch;
        const int alg = alg_for_ch(ch);
        ImGui::Text("Channel %d  ALG %d: %s", ch, alg, s_alg[alg].desc);
        draw_alg_diagram(ch, ImVec2(ImGui::GetContentRegionAvail().x, 330.0f), false);
        return;
    }
    // Two lit channels still get full-size boxes; more drop to compact ones.
    const bool compact = n > 2;
    const float h = compact ? 150.0f : 280.0f;  // room for ALG 7's four rows
    if (ImGui::BeginTable("ym_alg_grid", 2, ImGuiTableFlags_SizingStretchSame)) {
        int cell = 0;
        for (int ch = 0; ch < YM_NUM_CHANNELS; ++ch) {
            if (!((shown >> ch) & 1)) continue;
            if ((cell % 2) == 0) ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(cell % 2);
            ++cell;
            if (draw_alg_diagram(ch, ImVec2(ImGui::GetContentRegionAvail().x, h), compact, true)) s_selected_ch = ch;
        }
        ImGui::EndTable();
    }
}

void draw_global_tab()
{
    const uint8_t r0f = ym_reg(0x0f);
    const uint8_t r14 = ym_reg(0x14);
    const uint8_t r18 = ym_reg(0x18);
    const uint8_t r1b = ym_reg(0x1b);
    uint8_t lfo_am = 0, lfo_pm = 0;
    YM_debug_get_lfo_depths(&lfo_am, &lfo_pm);
    const uint16_t ta = (uint16_t)((ym_reg(0x10) << 2) | (ym_reg(0x11) & 3));
    const uint8_t tb = ym_reg(0x12);
    const double ta_ms = audio_ym_timer_a_ms(ta);
    const double tb_ms = audio_ym_timer_b_ms(tb);
    const uint8_t status = YM_debug_get_status();

    ImGui::Text("Clock: %.0f Hz   ymfm sample rate: %.3f Hz", YM_CLOCK_HZ, YM_SAMPLE_HZ);
    ImGui::Separator();
    ImGui::Text("LFO: rate $%02X, waveform %s, AM depth %u, PM depth %u", r18, s_lfo_waves[r1b & 3], (unsigned)lfo_am, (unsigned)lfo_pm);
    ImGui::TextDisabled("$19 writes select AM (bit7=0) or PM (bit7=1); both depths are read from the chip.");
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
    ImGui::TextDisabled("Shadowed registers show writes as the chip took them; one replaced before its slot came round never shows.");
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
    dbgui_client_mute_note(YM_debug_get_client_mute_mask());
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
            if (dbgui_scope_mute_toggle((YM_debug_get_mute_mask() >> ch) & 1,
                                        (YM_debug_get_client_mute_mask() >> ch) & 1)) {
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
    if (dbgui_scope_mute_toggle((YM_debug_get_mute_mask() >> s_selected_ch) & 1,
                                (YM_debug_get_client_mute_mask() >> s_selected_ch) & 1)) {
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
