// VERA PCM panel — FIFO sample playback debugger for the Commander X16.
//
// Tabs:
//   * Status — side-effect-free decode of AUDIO_CTRL/AUDIO_RATE, FIFO fill,
//              AFLOW, current raw samples and playback phase.
//   * FIFO   — non-consuming, ring-aware view of queued AUDIO_DATA bytes plus a
//              raw waveform preview of what is about to be fetched.
//   * Scope  — self-arming oscilloscope of the post-volume PCM output.
//
// Technique: this panel reads the PCM block through pcm_debug_get_state() and
// pcm_debug_peek_fifo() instead of polling VERA register $9F3B/AUDIO_CTRL.
// Reading AUDIO_CTRL via video_read(0x1B) runs audio_render() as a side effect,
// so a passive debugger panel must not do it. Only the active tab does work
// (ImGui::BeginTabItem gates each tab), and pcm_debug_scope_read() is called
// only from the Scope tab because that read arms capture for about half a second.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_scope.h"
#include "debug_ui_widgets.h"

#include <stdint.h>
#include <stdio.h>

namespace {

constexpr double PCM_AUDIO_CLOCK = 25000000.0 / 512.0;
constexpr int    MAX_FIFO_BYTES  = PCM_FIFO_SIZE;
constexpr int    MAX_FIFO_FRAMES = PCM_FIFO_SIZE;

const uint8_t VOLUME_LUT[16] = {0, 1, 2, 3, 4, 5, 6, 8, 11, 14, 18, 23, 30, 38, 49, 64};

struct PcmFormat {
    const char *name;
    int         bytes_per_frame;
    bool        stereo;
    bool        bits16;
};

PcmFormat
pcm_format(uint8_t ctrl)
{
    static const PcmFormat formats[4] = {
        {"mono 8-bit", 1, false, false},
        {"stereo 8-bit", 2, true, false},
        {"mono 16-bit", 2, false, true},
        {"stereo 16-bit", 4, true, true},
    };
    return formats[(ctrl >> 4) & 3];
}

int16_t
decode_u8(uint8_t v)
{
    return (int16_t)((uint16_t)v << 8);
}

int16_t
decode_s16le(const uint8_t *p)
{
    uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    return (int16_t)v;
}

void
decode_frame(const uint8_t *p, const PcmFormat &fmt, int16_t *left, int16_t *right)
{
    if (!fmt.bits16) {
        *left = decode_u8(p[0]);
        *right = fmt.stereo ? decode_u8(p[1]) : *left;
        return;
    }

    *left = decode_s16le(p);
    *right = fmt.stereo ? decode_s16le(p + 2) : *left;
}

float
sample_level(int16_t v)
{
    int mag = (v < 0) ? -(int)v : (int)v;
    return (float)mag / 32768.0f;
}

double
effective_rate_hz(uint8_t rate)
{
    return PCM_AUDIO_CLOCK * (double)rate / 128.0;
}

void
text_ctrl_value(uint8_t ctrl)
{
    char bin[12];
    dbgui_format_binary(bin, sizeof bin, ctrl, 8);
    ImGui::Text("AUDIO_CTRL stored: $%02X  %s", ctrl, bin);
    dbgui_hover_value_tooltip("Stored bits 5-0: format + volume. Bits 7/6 are read-status/reset controls.", ctrl, 1);
}

void
draw_status_tab()
{
    pcm_debug_state s = {};
    pcm_debug_get_state(&s);

    const PcmFormat fmt = pcm_format(s.ctrl);
    const int volume = s.ctrl & 0x0f;
    const double rate_hz = effective_rate_hz(s.rate);
    const unsigned usable = (s.fifo_size > 0) ? (s.fifo_size - 1u) : 0u;
    const float fill_frac = (usable > 0) ? (float)s.fifo_cnt / (float)usable : 0.0f;
    const unsigned frames = s.fifo_cnt / (unsigned)fmt.bytes_per_frame;
    const double buffered_ms = (rate_hz > 0.0) ? ((double)frames * 1000.0 / rate_hz) : 0.0;

    text_ctrl_value(s.ctrl);
    ImGui::Text("Format: %s  (%d byte%s/frame)", fmt.name, fmt.bytes_per_frame,
                fmt.bytes_per_frame == 1 ? "" : "s");
    ImGui::Text("Volume: %d  LUT=%u/64  linear %.4f", volume, (unsigned)VOLUME_LUT[volume],
                (double)VOLUME_LUT[volume] / 64.0);
    ImGui::Text("Loop: %s", s.loop ? "enabled" : "disabled");
    ImGui::Separator();

    ImGui::Text("AUDIO_RATE effective divider: %u", (unsigned)s.rate);
    ImGui::Text("Playback rate: %s", s.rate == 0 ? "stopped" : "running");
    if (s.rate != 0) {
        ImGui::SameLine();
        ImGui::Text("(%.3f Hz)", rate_hz);
    }
    ImGui::TextDisabled("AUDIO_RATE writes above 128 are clamped as 256 - value; this is the already-clamped divider.");
    ImGui::Separator();

    char overlay[80];
    snprintf(overlay, sizeof overlay, "%u / %u usable bytes", s.fifo_cnt, usable);
    ImGui::ProgressBar(fill_frac, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::Text("FIFO: %u queued / %u bytes physical (%u usable)", s.fifo_cnt, s.fifo_size, usable);
    ImGui::Text("Frames queued: %u   Buffered: %.3f ms", frames, buffered_ms);
    ImGui::Text("Read index: %u   Write index: %u", s.fifo_rdidx, s.fifo_wridx);

    ImVec4 aflow = s.almost_empty ? ImVec4(1.0f, 0.35f, 0.20f, 1.0f)
                                  : ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
    ImGui::TextColored(aflow, "AFLOW / almost empty IRQ: %s (fifo_cnt < 1024)",
                       s.almost_empty ? "ASSERTED" : "clear");
    if (s.fifo_cnt > 0 && s.fifo_cnt < (unsigned)fmt.bytes_per_frame) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.20f, 1.0f),
                           "Underrun risk: fewer bytes queued than one %s frame; emulator will reset the FIFO mid-frame.",
                           fmt.name);
    }
    ImGui::Separator();

    ImGui::Text("Current raw fetched sample (pre-volume): L=%d  R=%d", (int)s.cur_l, (int)s.cur_r);
    ImGui::TextUnformatted("L");
    ImGui::SameLine();
    dbgui_level_meter("cur_l", sample_level(s.cur_l), ImVec2(160.0f, 0.0f), IM_COL32(90, 190, 255, 255));
    ImGui::SameLine();
    ImGui::TextUnformatted("R");
    ImGui::SameLine();
    dbgui_level_meter("cur_r", sample_level(s.cur_r), ImVec2(160.0f, 0.0f), IM_COL32(255, 175, 80, 255));
    ImGui::Text("Phase accumulator: $%02X (%u)", (unsigned)s.phase, (unsigned)s.phase);

    ImGui::Spacing();
    ImGui::TextDisabled("This panel reads the PCM block directly, not VERA $9F3B/AUDIO_CTRL; reading AUDIO_CTRL renders audio.");
    ImGui::TextDisabled("cur_l/cur_r are raw fetched samples before volume scaling. Scope traces show post-volume output.");
}

unsigned
peek_fifo_bytes(uint8_t *bytes, unsigned fifo_cnt)
{
    unsigned want = fifo_cnt;
    if (want > (unsigned)MAX_FIFO_BYTES) {
        want = (unsigned)MAX_FIFO_BYTES;
    }
    return pcm_debug_peek_fifo(bytes, 0, want);
}

int
build_fifo_preview(const uint8_t *bytes, unsigned byte_count, const PcmFormat &fmt,
                   int16_t *left, int16_t *right)
{
    const unsigned frames = byte_count / (unsigned)fmt.bytes_per_frame;
    unsigned n = frames;
    if (n > (unsigned)MAX_FIFO_FRAMES) {
        n = (unsigned)MAX_FIFO_FRAMES;
    }
    for (unsigned i = 0; i < n; ++i) {
        decode_frame(bytes + i * (unsigned)fmt.bytes_per_frame, fmt, &left[i], &right[i]);
    }
    return (int)n;
}

void
draw_fifo_hex_table(const uint8_t *bytes, unsigned byte_count, const PcmFormat &fmt)
{
    const unsigned rows = (byte_count + (unsigned)fmt.bytes_per_frame - 1u) / (unsigned)fmt.bytes_per_frame;
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                            DBGUI_TABLE_FLAGS_RESIZABLE;
    if (!ImGui::BeginTable("pcm_fifo_bytes", 4, flags, ImVec2(0.0f, 300.0f))) {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Offset");
    ImGui::TableSetupColumn("Hex bytes");
    ImGui::TableSetupColumn("Left");
    ImGui::TableSetupColumn("Right");
    ImGui::TableHeadersRow();

    for (unsigned row = 0; row < rows; ++row) {
        const unsigned off = row * (unsigned)fmt.bytes_per_frame;
        const unsigned remain = byte_count - off;
        const bool complete = remain >= (unsigned)fmt.bytes_per_frame;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("+%04X", off);

        ImGui::TableSetColumnIndex(1);
        char hex[16] = {};
        char *out = hex;
        size_t left_chars = sizeof hex;
        for (int i = 0; i < fmt.bytes_per_frame && off + (unsigned)i < byte_count; ++i) {
            int wrote = snprintf(out, left_chars, "%s%02X", i == 0 ? "" : " ", (unsigned)bytes[off + (unsigned)i]);
            if (wrote <= 0 || (size_t)wrote >= left_chars) {
                break;
            }
            out += wrote;
            left_chars -= (size_t)wrote;
        }
        ImGui::TextUnformatted(hex);

        ImGui::TableSetColumnIndex(2);
        if (complete) {
            int16_t l = 0;
            int16_t r = 0;
            decode_frame(bytes + off, fmt, &l, &r);
            ImGui::Text("%d", (int)l);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", (int)r);
        } else {
            ImGui::TextDisabled("(partial)");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("(partial)");
        }
    }
    ImGui::EndTable();
}

void
draw_fifo_tab()
{
    pcm_debug_state s = {};
    pcm_debug_get_state(&s);
    const PcmFormat fmt = pcm_format(s.ctrl);

    ImGui::Text("Queued FIFO bytes from read pointer. Format: %s (%d byte%s/frame).",
                fmt.name, fmt.bytes_per_frame, fmt.bytes_per_frame == 1 ? "" : "s");
    ImGui::TextDisabled("Preview decodes raw queued data about to be fetched; Scope shows what is playing after volume scaling.");

    if (s.fifo_cnt == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("FIFO is empty.");
        return;
    }

    static uint8_t bytes[MAX_FIFO_BYTES];
    static int16_t preview_l[MAX_FIFO_FRAMES];
    static int16_t preview_r[MAX_FIFO_FRAMES];

    const unsigned got = peek_fifo_bytes(bytes, s.fifo_cnt);
    const int preview_count = build_fifo_preview(bytes, got, fmt, preview_l, preview_r);
    const int preview_window = (preview_count < 512) ? preview_count : 512;

    if (preview_count > 0) {
        ImGui::Text("Waveform preview: %d complete frame%s decoded from %u queued byte%s.",
                    preview_count, preview_count == 1 ? "" : "s", got, got == 1 ? "" : "s");
        dbgui_scope("fifo_preview_l", preview_l, preview_count, ImVec2(0.0f, 90.0f),
                    IM_COL32(90, 190, 255, 255), preview_window, DBGUI_SCOPE_AUTO_GAIN, false, "queued L/raw");
        if (fmt.stereo) {
            dbgui_scope("fifo_preview_r", preview_r, preview_count, ImVec2(0.0f, 90.0f),
                        IM_COL32(255, 175, 80, 255), preview_window, DBGUI_SCOPE_AUTO_GAIN, false, "queued R/raw");
        }
    } else {
        ImGui::TextDisabled("Not enough queued bytes for one complete frame.");
    }

    ImGui::Separator();
    draw_fifo_hex_table(bytes, got, fmt);
}

void
draw_scope_tab()
{
    static DbgScopeControls ctl;
    static int16_t left[PCM_SCOPE_SAMPLES];
    static int16_t right[PCM_SCOPE_SAMPLES];
    static unsigned left_count = 0;
    static unsigned right_count = 0;
    static bool captured = false; // buffers hold real audio, not a projection

    dbgui_scope_controls(&ctl, PCM_SCOPE_SAMPLES);
    const bool held = dbgui_audio_hold();
    bool       predicted = false;

    if (!ctl.freeze && !held) {
        left_count = pcm_debug_scope_read(PCM_SCOPE_L, left, PCM_SCOPE_SAMPLES);
        right_count = pcm_debug_scope_read(PCM_SCOPE_R, right, PCM_SCOPE_SAMPLES);
        captured = (left_count > 0 || right_count > 0);
    } else if (held && !captured && !ctl.freeze) {
        // Nothing captured, but the FIFO already holds the samples that are
        // about to play, so show those rather than an empty display.
        left_count = right_count = pcm_debug_scope_predict(left, right, PCM_SCOPE_SAMPLES);
        predicted = true;
    }

    if (held) {
        if (predicted) {
            dbgui_audio_predicted_note("the samples queued in the PCM FIFO");
        } else {
            dbgui_audio_hold_note();
        }
    }
    ImGui::TextDisabled("Reading these traces arms PCM capture; when this tab is hidden, capture self-disarms after about 0.5 s.");

    int peak_l = dbgui_scope("pcm_scope_l", left, (int)left_count, ImVec2(0.0f, 120.0f),
                             IM_COL32(90, 190, 255, 255), ctl.window, ctl.gain, ctl.trigger, "PCM L post-volume");
    dbgui_level_meter("pcm_scope_l_meter", (float)peak_l / 32768.0f, ImVec2(-1.0f, 0.0f),
                      IM_COL32(90, 190, 255, 255));
    int peak_r = dbgui_scope("pcm_scope_r", right, (int)right_count, ImVec2(0.0f, 120.0f),
                             IM_COL32(255, 175, 80, 255), ctl.window, ctl.gain, ctl.trigger, "PCM R post-volume");
    dbgui_level_meter("pcm_scope_r_meter", (float)peak_r / 32768.0f, ImVec2(-1.0f, 0.0f),
                      IM_COL32(255, 175, 80, 255));
}

// ---------------------------------------------------------------------------
// Panel entry point
// ---------------------------------------------------------------------------
void
pcm_panel_render(bool *p_open)
{
    if (ImGui::Begin("PCM", p_open)) {
        dbgui_window_zoom("pcm");

        if (ImGui::BeginTabBar("pcm_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Status")) {
                draw_status_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("FIFO")) {
                draw_fifo_tab();
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

static DebugPanelRegistration s_reg("PCM", pcm_panel_render, true);
