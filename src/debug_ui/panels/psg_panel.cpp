// PSG panel — live VERA PSG voice, register and oscilloscope views.
//
// Tabs:
//   * Voices    — one row per PSG voice, decoded from the core's shadow state.
//   * Registers — the 64 raw PSG bytes ($1F9C0..$1F9FF), with bit summaries.
//   * Scope     — self-armed per-voice and summed-output oscilloscope traces.
//
// Technique: the expensive work is gated by ImGui::BeginTabItem(), so hidden
// tabs do not decode or refresh buffers. Voice peaks are read exactly once per
// visible Voices/Scope frame because psg_debug_get_channel() resets them. Scope
// capture is self-arming, so psg_debug_scope_read() is called only from the
// Scope tab; when hidden or frozen, capture naturally lapses in the audio core.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_scope.h"
#include "debug_ui_widgets.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

namespace {

constexpr uint32_t PSG_BASE_ADDR     = 0x1F9C0;
constexpr double   PSG_SAMPLE_RATE   = 25000000.0 / 512.0;
constexpr double   PSG_PHASE_STEPS   = 131072.0;
constexpr float    PSG_PEAK_FULLSCALE = 16352.0f;
constexpr float    LEVEL_METER_W      = 92.0f;

struct PsgSnapshot {
    psg_debug_channel ch[PSG_NUM_CHANNELS];
    int               audible = 0;
};

const char *
waveform_name(uint8_t wf)
{
    static const char *names[] = { "Pulse", "Saw", "Tri", "Noise" };
    return names[wf & 3];
}

ImU32
waveform_color(uint8_t wf, bool silent)
{
    if (silent)
        return IM_COL32(95, 100, 108, 255);
    switch (wf & 3) {
        case PSG_WF_PULSE:    return IM_COL32(100, 190, 255, 255);
        case PSG_WF_SAWTOOTH: return IM_COL32(255, 185, 90, 255);
        case PSG_WF_TRIANGLE: return IM_COL32(130, 225, 145, 255);
        default:              return IM_COL32(220, 130, 255, 255);
    }
}

bool
voice_silent(const psg_debug_channel &c)
{
    return c.vol_raw == 0 || (!c.left && !c.right);
}

double
freq_hz(uint16_t freq)
{
    return (double)freq * PSG_SAMPLE_RATE / PSG_PHASE_STEPS;
}

void
format_note(double hz, char *out, size_t outsz)
{
    if (hz <= 0.0) {
        snprintf(out, outsz, "--");
        return;
    }

    static const char *names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const double semis = 69.0 + 12.0 * log(hz / 440.0) / log(2.0);
    const int    note  = (int)floor(semis + 0.5);
    const int    idx   = ((note % 12) + 12) % 12;
    const int    oct   = note / 12 - 1;
    int cents = (int)floor((semis - (double)note) * 100.0 + ((semis >= (double)note) ? 0.5 : -0.5));
    // Pad name+octave to a constant width so the cents value stays put.
    char nb[12];
    snprintf(nb, sizeof nb, "%s%d", names[idx], oct);
    snprintf(out, outsz, "%-4s %+3d c", nb, cents);
}

void
read_channels(PsgSnapshot *snap)
{
    snap->audible = 0;
    for (int ch = 0; ch < PSG_NUM_CHANNELS; ++ch) {
        if (!psg_debug_get_channel(ch, &snap->ch[ch])) {
            snap->ch[ch] = {};
        }
        if (!voice_silent(snap->ch[ch])) {
            ++snap->audible;
        }
    }
}

// Voice state for this frame. psg_debug_get_channel() resets the peak meters as
// it reads, and nothing refills them while the machine is paused, so hold the
// last live snapshot rather than reporting silence. The first read always
// happens even when paused, otherwise a panel opened while stopped would show
// zeros until execution resumed.
const PsgSnapshot &
current_channels()
{
    static PsgSnapshot held;
    static bool        primed = false;
    if (!primed || !dbgui_audio_hold()) {
        read_channels(&held);
        primed = true;
    }
    return held;
}

int
audible_from_regs()
{
    int audible = 0;
    for (int ch = 0; ch < PSG_NUM_CHANNELS; ++ch) {
        uint8_t v = psg_debug_get_reg((uint8_t)(ch * 4 + 2));
        if ((v & 0x3F) != 0 && (v & 0xC0) != 0) {
            ++audible;
        }
    }
    return audible;
}

void
show_status_line()
{
    ImGui::Text("Sample %.3f Hz   audible voices %2d/%2d   noise LFSR $%04X",
                PSG_SAMPLE_RATE, audible_from_regs(), PSG_NUM_CHANNELS,
                psg_debug_get_noise_state());
}

void
text_value(const char *desc, const char *fmt, uint32_t v, int bytes)
{
    ImGui::Text(fmt, v);
    dbgui_hover_value_tooltip(desc, v, bytes);
}

// Width for a fixed-size table column, measured from a worst-case SAMPLE string
// rather than from the live cell contents.
//
// This matters: with ImGuiTableFlags_SizingFixedFit and no explicit width, each
// column re-fits to whatever it currently holds, so a value going from "0.000"
// to "762.939" (or "unused" to "63 (50.00%)") resized the column and shifted
// every column after it — the table visibly jittered while audio played. Sizing
// from a constant sample makes the layout depend only on the font, so it holds
// still. Columns are also Resizable, so a user can still widen one by hand.
float
col_w(const char *sample)
{
    return ImGui::CalcTextSize(sample).x + ImGui::GetStyle().CellPadding.x * 2.0f;
}

void
format_pw(const psg_debug_channel &c, char *out, size_t outsz)
{
    // Fixed field widths so the text does not shift around inside its column
    // as the value changes.
    switch (c.waveform & 3) {
        case PSG_WF_PULSE:
            snprintf(out, outsz, "%2u (%5.2f%%)", c.pw, (double)(c.pw + 1) * 100.0 / 128.0);
            break;
        case PSG_WF_SAWTOOTH:
        case PSG_WF_TRIANGLE:
            snprintf(out, outsz, "mask $%02X", (unsigned)((c.pw ^ 0x3F) & 0x3F));
            break;
        default:
            snprintf(out, outsz, "unused");
            break;
    }
}

const char *
reg_summary(int byte, uint8_t v)
{
    // Fixed field widths: these strings sit in a fixed-width table column and
    // are re-rendered every frame, so growing/shrinking numbers would jitter.
    static char buf[96];
    switch (byte & 3) {
        case 0:
            snprintf(buf, sizeof buf, "frequency low byte");
            break;
        case 1:
            snprintf(buf, sizeof buf, "frequency high byte");
            break;
        case 2:
            snprintf(buf, sizeof buf, "R=%-3s L=%-3s volume=%2u",
                     (v & 0x80) ? "on" : "off", (v & 0x40) ? "on" : "off",
                     (unsigned)(v & 0x3F));
            break;
        default:
            snprintf(buf, sizeof buf, "wave=%-5s PW/shape=%2u",
                     waveform_name((uint8_t)(v >> 6)), (unsigned)(v & 0x3F));
            break;
    }
    return buf;
}

void
draw_voice_detail(int selected, const psg_debug_channel &c)
{
    uint8_t regs[4];
    for (int i = 0; i < 4; ++i) {
        regs[i] = psg_debug_get_reg((uint8_t)(selected * 4 + i));
    }

    char note[32];
    char pw[48];
    const double hz = freq_hz(c.freq);
    format_note(hz, note, sizeof note);
    format_pw(c, pw, sizeof pw);

    ImGui::Separator();
    ImGui::Text("Voice %d detail", selected);

    // A fixed-height child: the detail body has an optional status note, one or
    // two PW lines and a noise-only line, so its natural height changes with the
    // selected voice's waveform. Reserving the space keeps everything below from
    // jumping as you click between voices.
    const float detail_h = ImGui::GetTextLineHeightWithSpacing() * 14.0f;
    if (!ImGui::BeginChild("psg_voice_detail", ImVec2(0.0f, detail_h), ImGuiChildFlags_None)) {
        ImGui::EndChild();
        return;
    }

    if (!c.left && !c.right) {
        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                           "L and R are both disabled: the core holds phase at 0 (normal off state).");
    } else if (c.vol_raw == 0) {
        ImGui::TextDisabled("Volume is zero: phase runs but output is silent.");
    } else {
        ImGui::TextDisabled("Voice is audible.");
    }

    if (ImGui::BeginTable("psg_detail_regs", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Addr",   ImGuiTableColumnFlags_WidthFixed, col_w("$1F9FF_"));
        ImGui::TableSetupColumn("Hex",    ImGuiTableColumnFlags_WidthFixed, col_w("$FF_"));
        ImGui::TableSetupColumn("Bits",   ImGuiTableColumnFlags_WidthFixed, col_w("1111 1111_"));
        ImGui::TableSetupColumn("Decode", ImGuiTableColumnFlags_WidthFixed, col_w("R=off L=off volume=63__"));
        ImGui::TableHeadersRow();
        for (int i = 0; i < 4; ++i) {
            char bits[16];
            dbgui_format_binary(bits, sizeof bits, regs[i], 8);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            text_value("VRAM mirror address", "$%05X", PSG_BASE_ADDR + (uint32_t)selected * 4u + (uint32_t)i, 4);
            ImGui::TableSetColumnIndex(1);
            text_value("raw PSG shadow byte", "$%02X", regs[i], 1);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(bits);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(reg_summary(i, regs[i]));
        }
        ImGui::EndTable();
    }

    ImGui::Text("Frequency: $%04X / %5u = %10.6f Hz", c.freq, c.freq, hz);
    dbgui_hover_value_tooltip("16-bit frequency word", c.freq, 2);
    ImGui::Text("Nearest note: %s", note);

    if ((c.waveform & 3) == PSG_WF_PULSE) {
        ImGui::Text("PW: %s duty; core compares (phase >> 10) > PW.", pw);
        ImGui::TextDisabled("Duty cycle only applies to the Pulse waveform.");
    } else if ((c.waveform & 3) == PSG_WF_SAWTOOTH || (c.waveform & 3) == PSG_WF_TRIANGLE) {
        ImGui::Text("PW: %2u, shape mask $%02X", c.pw, (unsigned)((c.pw ^ 0x3F) & 0x3F));
        ImGui::TextDisabled("For Saw/Triangle this is an XOR wave-shaping mask, not duty cycle.");
    } else {
        ImGui::Text("PW: unused by Noise voices.");
        ImGui::TextDisabled("Noise latches a 6-bit value from the shared LFSR each phase wrap.");
    }

    ImGui::Text("Volume: raw %2u -> LUT %3u", (unsigned)c.vol_raw, (unsigned)c.vol_lut);
    ImGui::Text("Pan: L %-3s  R %-3s", c.left ? "on" : "off", c.right ? "on" : "off");
    ImGui::Text("Live phase: $%05X", (unsigned)(c.phase & 0x1FFFFu));
    dbgui_hover_value_tooltip("17-bit phase accumulator", c.phase & 0x1FFFFu, 4);
    ImGui::Text("Last sample: %6d   peak: %5u", (int)c.last_sample, (unsigned)c.peak);
    if ((c.waveform & 3) == PSG_WF_NOISE) {
        ImGui::Text("Latched noise value: %2u   shared LFSR: $%04X",
                    (unsigned)c.noiseval, (unsigned)psg_debug_get_noise_state());
    }
    ImGui::EndChild();
}

void
draw_voices_tab(int *selected_voice)
{
    PsgSnapshot snap = current_channels();
    if (dbgui_audio_hold()) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f), "held (paused)");
        ImGui::SetItemTooltip("The machine is paused, so audio is silent and the peak meters\n"
                              "are no longer being fed. These are the values from the moment\n"
                              "it stopped.");
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("psg_voices", 10, flags, ImVec2(0, 310))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Samples are the widest value each column can ever hold, so the layout
        // never depends on the live values (see col_w).
        ImGui::TableSetupColumn("Ch",    ImGuiTableColumnFlags_WidthFixed, col_w("Ch__"));
        ImGui::TableSetupColumn("Wave",  ImGuiTableColumnFlags_WidthFixed, col_w("Noise_"));
        ImGui::TableSetupColumn("Freq",  ImGuiTableColumnFlags_WidthFixed, col_w("$FFFF / 65535_"));
        ImGui::TableSetupColumn("Hz",    ImGuiTableColumnFlags_WidthFixed, col_w("24414.062_"));
        ImGui::TableSetupColumn("Note",  ImGuiTableColumnFlags_WidthFixed, col_w("A#10 -50 c_"));
        ImGui::TableSetupColumn("PW",    ImGuiTableColumnFlags_WidthFixed, col_w("63 (50.00%)_"));
        ImGui::TableSetupColumn("Vol",   ImGuiTableColumnFlags_WidthFixed, col_w("63 -> 511_"));
        ImGui::TableSetupColumn("L",     ImGuiTableColumnFlags_WidthFixed, col_w("on_"));
        ImGui::TableSetupColumn("R",     ImGuiTableColumnFlags_WidthFixed, col_w("on_"));
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, LEVEL_METER_W);
        ImGui::TableHeadersRow();

        for (int ch = 0; ch < PSG_NUM_CHANNELS; ++ch) {
            const psg_debug_channel &c = snap.ch[ch];
            const bool silent = voice_silent(c);
            char note[32], pw[48], label[16];
            format_note(freq_hz(c.freq), note, sizeof note);
            format_pw(c, pw, sizeof pw);
            snprintf(label, sizeof label, "%2d", ch);

            ImGui::TableNextRow();
            if (silent) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.48f);
            }

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(label, *selected_voice == ch, ImGuiSelectableFlags_SpanAllColumns)) {
                *selected_voice = ch;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(waveform_name(c.waveform));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("$%04X / %5u", (unsigned)c.freq, (unsigned)c.freq);
            dbgui_hover_value_tooltip("16-bit frequency word", c.freq, 2);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%9.3f", freq_hz(c.freq));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(note);
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(pw);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%2u -> %3u", (unsigned)c.vol_raw, (unsigned)c.vol_lut);
            dbgui_hover_value_tooltip("volume register value", c.vol_raw, 1);
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(c.left ? "on" : "-");
            ImGui::TableSetColumnIndex(8);
            ImGui::TextUnformatted(c.right ? "on" : "-");
            ImGui::TableSetColumnIndex(9);
            char meter_id[16];
            snprintf(meter_id, sizeof meter_id, "##lvl%d", ch);
            dbgui_level_meter(meter_id, (float)c.peak / PSG_PEAK_FULLSCALE,
                              ImVec2(LEVEL_METER_W, ImGui::GetTextLineHeight()), waveform_color(c.waveform, silent));

            if (silent) {
                ImGui::PopStyleVar();
            }
        }
        ImGui::EndTable();
    }

    if (*selected_voice < 0 || *selected_voice >= PSG_NUM_CHANNELS) {
        *selected_voice = 0;
    }
    draw_voice_detail(*selected_voice, snap.ch[*selected_voice]);
}

void
draw_registers_tab()
{
    ImGui::TextDisabled("VERA mirrors these at $1F9C0-$1F9FF; this panel reads the PSG shadow, which psg_reset() clears.");

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("psg_regs", 5, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Voice",  ImGuiTableColumnFlags_WidthFixed, col_w("15 +3_"));
        ImGui::TableSetupColumn("Addr",   ImGuiTableColumnFlags_WidthFixed, col_w("$1F9FF_"));
        ImGui::TableSetupColumn("Hex",    ImGuiTableColumnFlags_WidthFixed, col_w("$FF_"));
        ImGui::TableSetupColumn("Bits",   ImGuiTableColumnFlags_WidthFixed, col_w("1111 1111_"));
        ImGui::TableSetupColumn("Decode", ImGuiTableColumnFlags_WidthFixed, col_w("R=off L=off volume=63__"));
        ImGui::TableHeadersRow();

        for (int ch = 0; ch < PSG_NUM_CHANNELS; ++ch) {
            for (int i = 0; i < 4; ++i) {
                const int reg = ch * 4 + i;
                const uint8_t v = psg_debug_get_reg((uint8_t)reg);
                char bits[16];
                dbgui_format_binary(bits, sizeof bits, v, 8);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%2d +%d", ch, i);
                ImGui::TableSetColumnIndex(1);
                text_value("VRAM mirror address", "$%05X", PSG_BASE_ADDR + (uint32_t)reg, 4);
                ImGui::TableSetColumnIndex(2);
                text_value("raw PSG shadow byte", "$%02X", v, 1);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(bits);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(reg_summary(i, v));
            }
        }
        ImGui::EndTable();
    }
}

void
read_scope_buffers(int16_t buffers[PSG_SCOPE_STREAMS][PSG_SCOPE_SAMPLES], unsigned counts[PSG_SCOPE_STREAMS])
{
    for (int i = 0; i < PSG_SCOPE_STREAMS; ++i) {
        counts[i] = psg_debug_scope_read(i, buffers[i], PSG_SCOPE_SAMPLES);
    }
}

void
draw_scope_tab(int *selected_voice)
{
    static DbgScopeControls ctl;
    static int16_t          buffers[PSG_SCOPE_STREAMS][PSG_SCOPE_SAMPLES];
    static unsigned         counts[PSG_SCOPE_STREAMS];
    static bool             captured = false; // buffers hold real audio, not a projection

    PsgSnapshot snap = current_channels();

    dbgui_scope_controls(&ctl, PSG_SCOPE_SAMPLES);
    // Only refresh the traces when the machine is actually producing audio; a
    // read while paused would drain the rings to nothing and blank the display.
    const bool held = dbgui_audio_hold();
    bool       predicted = false;
    if (!ctl.freeze && !held) {
        read_scope_buffers(buffers, counts);
        captured = false;
        for (int i = 0; i < PSG_SCOPE_STREAMS; ++i) {
            if (counts[i] > 0) {
                captured = true;
                break;
            }
        }
    } else if (held && !captured && !ctl.freeze) {
        // Paused with nothing ever captured - capture is self-arming, so a scope
        // first opened at a breakpoint has an empty ring. The voice registers
        // still describe the waveform completely, so project it forward instead
        // of showing the user a dead panel.
        const unsigned n = psg_debug_scope_predict(buffers, PSG_SCOPE_SAMPLES);
        for (int i = 0; i < PSG_SCOPE_STREAMS; ++i) {
            counts[i] = n;
        }
        predicted = true;
    }
    if (held) {
        if (predicted) {
            dbgui_audio_predicted_note("the current PSG voice registers");
        } else {
            dbgui_audio_hold_note();
        }
    } else if (ctl.freeze) {
        ImGui::SameLine();
        ImGui::TextDisabled("frozen; capture will lapse");
    }

    const float mini_h = ImGui::GetTextLineHeight() * 4.0f;
    if (ImGui::BeginTable("psg_scope_grid", 4, ImGuiTableFlags_SizingStretchSame)) {
        for (int ch = 0; ch < PSG_NUM_CHANNELS; ++ch) {
            const psg_debug_channel &c = snap.ch[ch];
            const bool silent = voice_silent(c);
            char id[24], overlay[32];
            snprintf(id, sizeof id, "psg_scope_%d", ch);
            snprintf(overlay, sizeof overlay, "%02d %s%s", ch, waveform_name(c.waveform),
                     (ch == *selected_voice) ? " *" : "");

            ImGui::TableNextColumn();
            dbgui_scope(id, buffers[ch], (int)counts[ch], ImVec2(0, mini_h),
                        waveform_color(c.waveform, silent), ctl.window, ctl.gain, ctl.trigger, overlay);
            if (ImGui::IsItemClicked()) {
                *selected_voice = ch;
            }
        }
        ImGui::EndTable();
    }

    if (*selected_voice < 0 || *selected_voice >= PSG_NUM_CHANNELS) {
        *selected_voice = 0;
    }

    ImGui::Separator();
    char overlay[64];
    snprintf(overlay, sizeof overlay, "Voice %02d %s", *selected_voice, waveform_name(snap.ch[*selected_voice].waveform));
    ImGui::Text("Selected voice %d", *selected_voice);
    dbgui_scope("psg_scope_selected", buffers[*selected_voice], (int)counts[*selected_voice],
                ImVec2(0, ImGui::GetTextLineHeight() * 8.0f),
                waveform_color(snap.ch[*selected_voice].waveform, voice_silent(snap.ch[*selected_voice])),
                ctl.window, ctl.gain, ctl.trigger, overlay);

    ImGui::TextUnformatted("Summed PSG output");
    dbgui_scope("psg_scope_mix_l", buffers[PSG_SCOPE_MIX_L], (int)counts[PSG_SCOPE_MIX_L],
                ImVec2(0, ImGui::GetTextLineHeight() * 5.0f), IM_COL32(100, 210, 255, 255),
                ctl.window, ctl.gain, ctl.trigger, "Mix L");
    dbgui_scope("psg_scope_mix_r", buffers[PSG_SCOPE_MIX_R], (int)counts[PSG_SCOPE_MIX_R],
                ImVec2(0, ImGui::GetTextLineHeight() * 5.0f), IM_COL32(255, 170, 105, 255),
                ctl.window, ctl.gain, ctl.trigger, "Mix R");
}

// ---------------------------------------------------------------------------
// Panel entry point
// ---------------------------------------------------------------------------
void
psg_panel_render(bool *p_open)
{
    static int selected_voice = 0;

    if (ImGui::Begin("PSG", p_open)) {
        dbgui_window_zoom("psg");
        show_status_line();

        if (ImGui::BeginTabBar("psg_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Voices")) {
                draw_voices_tab(&selected_voice);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Registers")) {
                draw_registers_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Scope")) {
                draw_scope_tab(&selected_voice);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    dbgui_window_end();
}

} // namespace

static DebugPanelRegistration s_reg("PSG", psg_panel_render, true);
