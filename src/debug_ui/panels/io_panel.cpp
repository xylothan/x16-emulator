// I/O panel — what the machine is doing to its ports, and to files.
//
// The other panels answer "what is the state of X". This one mostly answers
// "what just happened", because I/O is a conversation: a command goes out, a
// status comes back, a block is read, a file is opened. A register dump of the
// SD card taken between commands tells you almost nothing; the sequence tells
// you everything.
//
// Tabs:
//   * Activity  — the io_trace ring: raw register accesses and decoded device
//                 events, with per-device capture gating.
//   * SD Card   — the SPI card's command state, status bits and counters.
//   * Files     — the two file paths the machine can use, side by side.
//   * Joysticks — four slots, decoded to named buttons.
//   * VIA       — both VIAs, registers and timers.
//   * I2C       — the bus state machine and the SMC and RTC behind it.
//   * Serial    — the IEC lines.
//
// ON THE TWO FILE PATHS, because this is the panel where the difference bites:
// when the emulator serves files from the host filesystem (the default), it
// sees real filenames and this panel lists them. When an SD card image is
// attached instead, the ROM's own FAT driver runs and the emulator sees nothing
// but 512-byte blocks -- so the filenames in that half come from sdcard_fat.c
// parsing the image ourselves, and are only as fresh as the last index build.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_settings.h"
#include "debug_ui_widgets.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

// ---------------------------------------------------------------------------
// Capture gating
// ---------------------------------------------------------------------------

// The settings own the capture preferences; io_trace owns the mask the hot path
// actually tests. Pushing one into the other once per frame keeps the hot path
// reading a single word, rather than chasing the settings struct on every I/O
// access.
void
apply_capture_settings()
{
    const DebugUiSettings &s = debug_ui_settings();

    uint32_t mask = 0;
    if (s.io_cap_via) {
        mask |= IO_TRACE_BIT(IO_DEV_VIA1) | IO_TRACE_BIT(IO_DEV_VIA2);
    }
    if (s.io_cap_vera) {
        mask |= IO_TRACE_BIT(IO_DEV_VERA);
    }
    if (s.io_cap_spi) {
        mask |= IO_TRACE_BIT(IO_DEV_SPI);
    }
    if (s.io_cap_ym) {
        mask |= IO_TRACE_BIT(IO_DEV_YM);
    }
    if (s.io_cap_emu) {
        mask |= IO_TRACE_BIT(IO_DEV_EMU);
    }
    if (s.io_cap_midi) {
        mask |= IO_TRACE_BIT(IO_DEV_MIDI);
    }
    if (s.io_cap_openbus) {
        mask |= IO_TRACE_BIT(IO_DEV_OPENBUS);
    }
    if (s.io_cap_sd) {
        mask |= IO_TRACE_BIT(IO_DEV_SDCARD);
    }
    if (s.io_cap_files) {
        mask |= IO_TRACE_BIT(IO_DEV_IEEE);
    }
    if (s.io_cap_i2c) {
        mask |= IO_TRACE_BIT(IO_DEV_I2C);
    }
    if (s.io_cap_joy) {
        mask |= IO_TRACE_BIT(IO_DEV_JOYSTICK);
    }
    // Serial has no decoded emitter yet; its raw VIA traffic already shows.
    mask |= IO_TRACE_BIT(IO_DEV_SERIAL);

    io_trace_device_mask = mask;
    io_trace_enabled     = s.io_trace_capture;

    // The index is built by sdcard_attach(), which is C and cannot see the
    // settings struct; this flag is how the preference reaches it.
    sdcard_fat_autoindex = s.io_fat_autoindex;

    if (io_trace_capacity() != 0 && io_trace_capacity() != s.io_trace_capacity) {
        io_trace_init(s.io_trace_capacity);
        // io_trace clamps; write back what was actually allocated so the
        // settings field shows the real number rather than the wish.
        debug_ui_settings().io_trace_capacity = io_trace_capacity();
    }
}

// ---------------------------------------------------------------------------
// Activity
// ---------------------------------------------------------------------------

char s_filter[64]   = "";
bool s_autoscroll   = true;
bool s_show_reads   = true;
bool s_show_writes  = true;

const ImVec4 COL_READ    = ImVec4(0.55f, 0.78f, 1.00f, 1.0f);
const ImVec4 COL_WRITE   = ImVec4(1.00f, 0.72f, 0.42f, 1.0f);
const ImVec4 COL_DECODED = ImVec4(0.62f, 0.90f, 0.62f, 1.0f);
const ImVec4 COL_DIM     = ImVec4(0.70f, 0.70f, 0.78f, 1.0f);

bool
event_matches(const io_event_t &ev)
{
    if (ev.kind == IO_EVENT_ACCESS) {
        if (ev.is_write && !s_show_writes) {
            return false;
        }
        if (!ev.is_write && !s_show_reads) {
            return false;
        }
    }
    if (s_filter[0] == '\0') {
        return true;
    }

    // Match against what the user can actually see in the row, so a filter that
    // hides a visible row would be a bug rather than a subtlety.
    char addr[16];
    snprintf(addr, sizeof(addr), "$%04X", ev.addr);
    const char *dev = io_trace_device_name((io_device_t)ev.device);

    return (strstr(dev, s_filter) != nullptr) ||
           (ev.text[0] != '\0' && strstr(ev.text, s_filter) != nullptr) ||
           (ev.has_addr && strstr(addr, s_filter) != nullptr);
}

void
draw_capture_controls()
{
    DebugUiSettings &s = debug_ui_settings();
    bool             changed = false;

    changed |= ImGui::Checkbox("Capture", &s.io_trace_capture);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Record I/O events. This is the one debugger feature\n"
                          "that costs the running machine anything, so it can be\n"
                          "turned off entirely.");
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        io_trace_clear();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &s_autoscroll);

    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(dbgui_field_width("wwwwwwwwwwww"));
    ImGui::InputTextWithHint("##iofilter", "filter", s_filter, sizeof(s_filter));

    ImGui::SameLine(0, 16);
    ImGui::Checkbox("R", &s_show_reads);
    ImGui::SameLine();
    ImGui::Checkbox("W", &s_show_writes);

    // Per-device gating. VERA is the reason this row exists: leaving it on
    // means the ring holds one frame of data-port traffic and nothing else.
    ImGui::TextUnformatted("Devices:");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("VIA", &s.io_cap_via);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("VERA", &s.io_cap_vera);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off by default. VERA's data ports are touched thousands\n"
                          "of times per frame; capturing them fills the ring with\n"
                          "VERA and pushes everything else out within a frame or two.\n"
                          "The SD data path at $9F3E/$9F3F is captured separately as\n"
                          "SPI so it survives this being off.");
    }
    ImGui::SameLine();
    changed |= ImGui::Checkbox("SPI", &s.io_cap_spi);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("YM", &s.io_cap_ym);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Emu", &s.io_cap_emu);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("MIDI", &s.io_cap_midi);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Open bus", &s.io_cap_openbus);

    // Decoded events, on their own row and split per device: SD and file
    // activity is a handful of events per operation, while I2C and the
    // joysticks are polled about sixty times a second each and would push
    // everything else out of the ring while the machine sat idle.
    ImGui::TextUnformatted("Decoded:");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("SD", &s.io_cap_sd);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("SD card commands, responses, and each block read or\n"
                          "written -- resolved to a filename when the image is indexed.");
    }
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Files", &s.io_cap_files);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Host-filesystem file opens, closes and DOS commands,\n"
                          "with real filenames.");
    }
    ImGui::SameLine();
    changed |= ImGui::Checkbox("I2C##cap", &s.io_cap_i2c);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off by default. One event per completed bus transaction,\n"
                          "but the keyboard is polled continuously -- about sixty a\n"
                          "second even at a READY prompt.");
    }
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Joystick", &s.io_cap_joy);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off by default. One event per latch, which is once per\n"
                          "frame whether or not anything is pressed.");
    }

    if (changed) {
        debug_ui_settings_mark_dirty();
    }

    const int      count   = io_trace_count();
    const uint32_t dropped = io_trace_dropped();
    ImGui::TextColored(COL_DIM, "%d/%d events", count, io_trace_capacity());
    if (dropped > 0) {
        ImGui::SameLine();
        ImGui::TextColored(COL_DIM, "· %u overwritten", dropped);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The ring wrapped. Raise the capacity in\n"
                              "System > Settings, or narrow what is captured.");
        }
    }
}

void
draw_activity_tab()
{
    draw_capture_controls();
    ImGui::Separator();

    if (ImGui::BeginTable("io_activity", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerV | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Seq", "88888888"));
        ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("PC", "88:8888"));
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Device", "Joystick"));
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Addr", "$FFFF"));
        ImGui::TableSetupColumn("R/W", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("R/W", "W"));
        ImGui::TableSetupColumn("Value / event", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Oldest first, so the log reads downward like any other log and
        // auto-scroll means "follow the newest".
        const int total = io_trace_count();
        for (int i = 0; i < total; i++) {
            const io_event_t *evp = io_trace_at(i);
            if (evp == nullptr) {
                continue;
            }
            const io_event_t &ev = *evp;
            if (!event_matches(ev)) {
                continue;
            }

            const bool decoded = (ev.kind == IO_EVENT_DECODED);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(COL_DIM, "%u", ev.seq);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("cycle %u", ev.cycles);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(COL_DIM, "%02X:%04X", ev.pc_bank, ev.pc);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(io_trace_device_name((io_device_t)ev.device));

            ImGui::TableSetColumnIndex(3);
            if (ev.has_addr) {
                ImGui::Text("$%04X", ev.addr);
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableSetColumnIndex(4);
            if (decoded && !ev.has_addr) {
                ImGui::TextUnformatted(" ");
            } else {
                ImGui::TextColored(ev.is_write ? COL_WRITE : COL_READ, "%s",
                                   ev.is_write ? "W" : "R");
            }

            ImGui::TableSetColumnIndex(5);
            if (decoded) {
                ImGui::TextColored(COL_DECODED, "%s", ev.text);
            } else {
                ImGui::Text("$%02X", ev.value);
                dbgui_hover_value_tooltip(nullptr, ev.value, 1);
            }
        }

        if (s_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------

// The SPI command set the X16's ROM actually uses. Anything else shows as its
// number, which is more honest than inventing a name for it.
const char *
sd_command_name(uint8_t cmd, bool is_acmd)
{
    if (is_acmd) {
        switch (cmd) {
            case 41: return "ACMD41 SD_SEND_OP_COND";
            default: return nullptr;
        }
    }
    switch (cmd) {
        case 0:  return "CMD0 GO_IDLE_STATE";
        case 1:  return "CMD1 SEND_OP_COND";
        case 8:  return "CMD8 SEND_IF_COND";
        case 9:  return "CMD9 SEND_CSD";
        case 10: return "CMD10 SEND_CID";
        case 12: return "CMD12 STOP_TRANSMISSION";
        case 16: return "CMD16 SET_BLOCKLEN";
        case 17: return "CMD17 READ_SINGLE_BLOCK";
        case 18: return "CMD18 READ_MULTIPLE_BLOCK";
        case 24: return "CMD24 WRITE_BLOCK";
        case 25: return "CMD25 WRITE_MULTIPLE_BLOCK";
        case 55: return "CMD55 APP_CMD";
        case 58: return "CMD58 READ_OCR";
        default: return nullptr;
    }
}

void
row_label(const char *label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(COL_DIM, "%s", label);
    ImGui::TableSetColumnIndex(1);
}

void
draw_sdcard_tab()
{
    sdcard_debug_state_t sd;
    sdcard_debug_get_state(&sd);

    if (!sd.attached) {
        ImGui::TextColored(COL_DIM, "No SD card image attached.");
        ImGui::TextWrapped("Without an image the machine takes its files from the host "
                           "filesystem instead, which is the default. See the Files tab, "
                           "where those show up by name.");
        return;
    }

    if (ImGui::BeginTable("sd_state", 2, DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_text_width("Multiblock read "));
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        row_label("Image");
        ImGui::TextUnformatted(sd.image_path);
        row_label("Size");
        ImGui::Text("%lld bytes", (long long)sd.image_size);
        row_label("Chip select");
        ImGui::TextUnformatted(sd.selected ? "asserted" : "idle");

        row_label("Last command");
        {
            const char *name = sd_command_name(sd.last_cmd, sd.last_cmd_is_acmd);
            if (name != nullptr) {
                ImGui::TextUnformatted(name);
            } else {
                ImGui::Text("%sCMD%u", sd.last_cmd_is_acmd ? "A" : "", sd.last_cmd);
            }
        }

        row_label("Block (LBA)");
        ImGui::Text("%u", sd.last_lba);
        dbgui_hover_value_tooltip("byte offset", sd.last_lba * 512u, 4);

        row_label("R1 status");
        {
            // R1 is a bitfield the ROM branches on, so showing which bits are
            // set is the whole point; a hex byte would need decoding by hand.
            ImGui::Text("idle=%d  initialised=%d", sd.is_idle ? 1 : 0,
                        sd.is_initialized ? 1 : 0);
        }

        row_label("Multiblock read");
        ImGui::TextUnformatted(sd.ongoing_multiblock_read ? "in progress" : "no");

        row_label("Response");
        ImGui::Text("%d / %d bytes sent", sd.response_counter, sd.response_length);

        row_label("Commands");
        ImGui::Text("%u", sd.commands);
        row_label("Blocks read");
        ImGui::Text("%llu  (%llu bytes)", (unsigned long long)sd.blocks_read,
                    (unsigned long long)sd.bytes_read);
        row_label("Blocks written");
        ImGui::Text("%llu  (%llu bytes)", (unsigned long long)sd.blocks_written,
                    (unsigned long long)sd.bytes_written);

        ImGui::EndTable();
    }

    ImGui::Separator();

    // What the last block access actually was, in filesystem terms.
    ImGui::TextUnformatted("Last block resolves to:");
    ImGui::SameLine();
    if (!sdcard_fat_ready()) {
        ImGui::TextColored(COL_DIM, "(image not indexed)");
    } else {
        const sdcard_fat_file_t *file   = nullptr;
        uint64_t                 offset = 0;
        switch (sdcard_fat_lookup(sd.last_lba, &file, &offset)) {
            case SDCARD_FAT_REGION_FILE:
            case SDCARD_FAT_REGION_DIR:
                if (file != nullptr) {
                    ImGui::TextColored(COL_DECODED, "%s + %llu", file->path,
                                       (unsigned long long)offset);
                } else {
                    ImGui::TextColored(COL_DIM, "(unnamed)");
                }
                break;
            case SDCARD_FAT_REGION_MBR: ImGui::TextColored(COL_DIM, "<MBR>"); break;
            case SDCARD_FAT_REGION_RESERVED: ImGui::TextColored(COL_DIM, "<boot/reserved>"); break;
            case SDCARD_FAT_REGION_FAT: ImGui::TextColored(COL_DIM, "<FAT>"); break;
            case SDCARD_FAT_REGION_ROOTDIR: ImGui::TextColored(COL_DIM, "<root directory>"); break;
            case SDCARD_FAT_REGION_FREE: ImGui::TextColored(COL_DIM, "<unallocated>"); break;
            default: ImGui::TextColored(COL_DIM, "<unknown>"); break;
        }
    }

    // SPI is how the card is reached at all, so its state belongs beside the
    // card rather than buried under VERA.
    ImGui::Separator();
    vera_spi_debug_state_t spi;
    vera_spi_debug_get_state(&spi);
    ImGui::Text("SPI  select=%d  busy=%d  autotx=%d  out=$%02X  in=$%02X",
                spi.ss ? 1 : 0, spi.busy ? 1 : 0, spi.autotx ? 1 : 0,
                spi.sending_byte, spi.received_byte);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

char s_file_filter[64] = "";

void
draw_sd_files()
{
    sdcard_debug_state_t sd;
    sdcard_debug_get_state(&sd);

    if (!sd.attached) {
        ImGui::TextColored(COL_DIM, "No SD card image attached.");
        return;
    }

    if (ImGui::SmallButton(sdcard_fat_ready() ? "Rebuild index" : "Build index")) {
        sdcard_reindex();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Parse the filesystem inside the image so block traffic\n"
                          "can be named. The emulator itself never does this -- it\n"
                          "only ever sees 512-byte blocks.");
    }

    if (!sdcard_fat_ready()) {
        ImGui::SameLine();
        ImGui::TextColored(COL_DIM, "not indexed");
        return;
    }

    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "%s · %u bytes/cluster · %d entries",
                       sdcard_fat_type_name(), sdcard_fat_bytes_per_cluster(),
                       sdcard_fat_file_count());

    if (sdcard_fat_is_stale()) {
        ImGui::SameLine();
        ImGui::TextColored(COL_WRITE, "· STALE");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The machine wrote to the FAT or a directory since this\n"
                              "index was built, so names and extents may no longer be\n"
                              "right. Rebuild to refresh. The index is never refreshed\n"
                              "automatically: quietly serving a wrong filename would be\n"
                              "worse than saying so.");
        }
    }
    if (sdcard_fat_truncated()) {
        ImGui::SameLine();
        ImGui::TextColored(COL_WRITE, "· TRUNCATED");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The image holds more files than the index will hold.\n"
                              "What is listed is correct; it is just not everything.");
        }
    }

    ImGui::SetNextItemWidth(dbgui_field_width("wwwwwwwwwwwwwwww"));
    ImGui::InputTextWithHint("##sdfilter", "filter", s_file_filter, sizeof(s_file_filter));

    if (ImGui::BeginTable("sd_files", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Size", "888888888"));
        ImGui::TableSetupColumn("Read", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Read", "888888888"));
        ImGui::TableSetupColumn("Written", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Written", "888888888"));
        ImGui::TableHeadersRow();

        // Two passes so the files the machine has actually touched come first.
        // The list is an inventory of the image, but the question being asked
        // is almost always "what did it read", and that answer should not be
        // somewhere in the middle of a few hundred alphabetical entries.
        const int n = sdcard_fat_file_count();
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < n; i++) {
                const sdcard_fat_file_t *f = sdcard_fat_file_at(i);
                if (f == nullptr || f->path == nullptr) {
                    continue;
                }
                const bool touched = (f->bytes_read > 0 || f->bytes_written > 0);
                if (touched != (pass == 0)) {
                    continue;
                }
                if (s_file_filter[0] != '\0' && strstr(f->path, s_file_filter) == nullptr) {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (touched) {
                    ImGui::TextColored(COL_DECODED, "%s", f->path);
                } else {
                    ImGui::TextColored(f->is_dir ? COL_DIM
                                                 : ImGui::GetStyle().Colors[ImGuiCol_Text],
                                       "%s", f->path);
                }

                ImGui::TableSetColumnIndex(1);
                if (f->is_dir) {
                    ImGui::TextColored(COL_DIM, "<dir>");
                } else {
                    ImGui::Text("%u", f->size);
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", (unsigned long long)f->bytes_read);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", (unsigned long long)f->bytes_written);
            }
        }
        ImGui::EndTable();
    }
}

void
draw_host_files()
{
    ieee_debug_state_t ie;
    ieee_debug_get_state(&ie);

    if (!ie.using_hostfs) {
        ImGui::TextColored(COL_DIM,
                           "Host filesystem access is off; the machine is using the SD image.");
        return;
    }

    ImGui::Text("Unit %d", ie.ieee_unit);
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "· cwd %s", ie.hostfscwd);
    ImGui::Text("Bus: %s%s%s  active channel %d",
                ie.listening ? "LISTENING " : "",
                ie.talking ? "TALKING " : "",
                (!ie.listening && !ie.talking) ? "idle" : "",
                ie.channel);

    if (ie.cmdlen > 0) {
        ImGui::TextColored(COL_DIM, "Last command: %s", ie.cmd);
    }
    if (ie.error_str[0] != '\0') {
        ImGui::TextColored(COL_WRITE, "Status: %s", ie.error_str);
    }

    if (ImGui::BeginTable("host_files", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Ch", "88"));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Mode", "read/write"));
        ImGui::TableSetupColumn("Read", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Read", "888888888"));
        ImGui::TableSetupColumn("Written", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Written", "888888888"));
        ImGui::TableHeadersRow();

        for (int i = 0; i < 16; i++) {
            const ieee_channel_debug_t &c = ie.channels[i];
            // An unopened channel with nothing ever transferred is noise.
            if (!c.is_open && c.name[0] == '\0' && c.bytes_read == 0 && c.bytes_written == 0) {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            if (c.is_open) {
                ImGui::TextColored(COL_DECODED, "%s", c.name);
            } else {
                ImGui::TextColored(COL_DIM, "%s", c.name[0] != '\0' ? c.name : "(closed)");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(c.read && c.write ? "read/write"
                                   : c.read         ? "read"
                                   : c.write        ? "write"
                                                    : "-");
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", c.bytes_read);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", c.bytes_written);
        }
        ImGui::EndTable();
    }
}

void
draw_files_tab()
{
    // Deliberately shown together rather than as one merged list: which of the
    // two is live is a property of how the emulator was started, and a reader
    // chasing a missing file needs to see which half is even in play.
    if (ImGui::BeginTabBar("files_sub")) {
        if (ImGui::BeginTabItem("Host filesystem")) {
            draw_host_files();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SD image")) {
            draw_sd_files();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------
// Joysticks
// ---------------------------------------------------------------------------

struct JoyBit {
    uint16_t    mask;
    const char *name;
};

const JoyBit JOY_BITS[] = {
    {JOY_BIT_DPAD_UP, "Up"},       {JOY_BIT_DPAD_DOWN, "Down"},
    {JOY_BIT_DPAD_LEFT, "Left"},   {JOY_BIT_DPAD_RIGHT, "Right"},
    {JOY_BIT_A, "A"},              {JOY_BIT_B, "B"},
    {JOY_BIT_X, "X"},              {JOY_BIT_Y, "Y"},
    {JOY_BIT_LEFT_SHOULDER, "L"},  {JOY_BIT_RIGHT_SHOULDER, "R"},
    {JOY_BIT_SELECT, "Select"},    {JOY_BIT_START, "Start"},
};

void
draw_joystick_tab()
{
    joystick_debug_state_t js;
    joystick_debug_get_state(&js);

    ImGui::Text("Latch %s   VIA1 data $%02X", js.latch ? "high" : "low", js.data);
    dbgui_hover_value_tooltip("Joystick_data", js.data, 1);
    ImGui::Separator();

    for (int i = 0; i < NUM_JOYSTICKS; i++) {
        const joystick_slot_debug_t &s = js.slots[i];
        ImGui::PushID(i);

        ImGui::Text("Slot %d", i);
        ImGui::SameLine();
        if (!s.enabled) {
            ImGui::TextColored(COL_DIM, "disabled");
        } else if (!s.controller_bound) {
            ImGui::TextColored(COL_DIM, "enabled, no controller");
        } else {
            ImGui::TextColored(COL_DECODED, "connected");
        }
        ImGui::SameLine();
        ImGui::TextColored(COL_DIM, "· mask $%04X · shift $%04X", s.button_mask, s.shift_mask);

        if (s.enabled) {
            // The wire protocol is active-low, so a pressed button reads as a
            // ZERO bit. Showing the raw mask alone would have every reader
            // inverting it in their head on every glance.
            for (size_t b = 0; b < sizeof(JOY_BITS) / sizeof(JOY_BITS[0]); b++) {
                const bool pressed = (s.button_mask & JOY_BITS[b].mask) == 0;
                if (b != 0) {
                    ImGui::SameLine();
                }
                if (pressed) {
                    ImGui::TextColored(COL_DECODED, "%s", JOY_BITS[b].name);
                } else {
                    ImGui::TextColored(COL_DIM, "%s", JOY_BITS[b].name);
                }
            }
        }

        ImGui::PopID();
        ImGui::Separator();
    }

    ImGui::TextColored(COL_DIM, "Buttons are active-low on the wire: a pressed button is a 0 bit.");
}

// ---------------------------------------------------------------------------
// VIA
// ---------------------------------------------------------------------------

const char *const VIA_REG_NAMES[16] = {
    "ORB/IRB", "ORA/IRA", "DDRB", "DDRA", "T1C-L", "T1C-H", "T1L-L", "T1L-H",
    "T2C-L",   "T2C-H",   "SR",   "ACR",  "PCR",   "IFR",   "IER",   "ORA/IRA*",
};

void
draw_one_via(int which)
{
    via_debug_state_t v;
    via_debug_get_state(which, &v);

    if (!v.exists) {
        ImGui::TextColored(COL_DIM, "VIA%d is not present in this machine configuration.",
                           which + 1);
        return;
    }

    if (ImGui::BeginTable("via_regs", 4,
                          ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Reg", "88"));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Name", "ORA/IRA*"));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Value", "$FF"));
        ImGui::TableSetupColumn("Binary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int r = 0; r < 15; r++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(COL_DIM, "%X", r);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(VIA_REG_NAMES[r]);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("$%02X", v.regs[r]);
            dbgui_hover_value_tooltip(VIA_REG_NAMES[r], v.regs[r], 1);
            ImGui::TableSetColumnIndex(3);
            char bits[16];
            dbgui_format_binary(bits, sizeof(bits), v.regs[r], 8);
            ImGui::TextColored(COL_DIM, "%s", bits);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("T1 %5u %s   latch %u", v.timer_count[0],
                v.timer_running[0] ? "running" : "stopped", v.timer_latch[0]);
    ImGui::Text("T2 %5u %s   latch %u", v.timer_count[1],
                v.timer_running[1] ? "running" : "stopped", v.timer_latch[1]);
    ImGui::TextColored(COL_DIM, "PB7 %d", v.pb7_output ? 1 : 0);

    if (which == 0) {
        ImGui::Separator();
        ImGui::TextColored(COL_DIM,
                           "VIA1 port A carries the joystick latch (PA2) and clock (PA3);\n"
                           "the I2C bus to the SMC and RTC hangs off port B.");
    }
}

void
draw_via_tab()
{
    if (ImGui::BeginTabBar("via_sub")) {
        if (ImGui::BeginTabItem("VIA1")) {
            draw_one_via(0);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("VIA2")) {
            draw_one_via(1);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------
// I2C / SMC / RTC
// ---------------------------------------------------------------------------

void
draw_i2c_tab()
{
    i2c_debug_state_t i2c;
    i2c_debug_get_state(&i2c);

    ImGui::TextUnformatted("Bus");
    if (ImGui::BeginTable("i2c_state", 2, DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_text_width("Transactions "));
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        row_label("State");
        if (i2c.state < 0) {
            ImGui::TextUnformatted("stopped");
        } else if (i2c.state == 0) {
            ImGui::TextUnformatted("start");
        } else {
            ImGui::Text("bit %d of 8", i2c.state);
        }

        row_label("Device");
        if (i2c.device_name[0] != '\0') {
            ImGui::Text("$%02X (%s)", i2c.device, i2c.device_name);
        } else {
            ImGui::Text("$%02X", i2c.device);
        }

        row_label("Direction");
        ImGui::TextUnformatted(i2c.read_mode ? "reading from device" : "writing to device");
        row_label("Byte");
        ImGui::Text("$%02X  (#%d in transaction)", i2c.value, i2c.count);
        row_label("Lines");
        ImGui::Text("CLK in %d   DATA in %d   DATA out %d", i2c.clk_in, i2c.data_in,
                    i2c.data_out);
        row_label("Transactions");
        ImGui::Text("%u started, %u completed", i2c.transactions_started,
                    i2c.transactions_completed);
        row_label("Bytes");
        ImGui::Text("%u read, %u written", i2c.bytes_read, i2c.bytes_written);
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("SMC");
    smc_debug_state_t smc;
    smc_debug_get_state(&smc);
    ImGui::Text("read op $%02X   activity LED $%02X   offset %u", smc.default_read_op,
                smc.activity_led, smc.i2c_data_pos);
    ImGui::Text("keyboard buffer %u   mouse buffer %u   mouse packets %u", smc.kbd_fill,
                smc.mse_fill, smc.mse_count);
    if (smc.requested_reset) {
        ImGui::TextColored(COL_WRITE, "reset requested");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("RTC");
    rtc_debug_state_t rtc;
    rtc_debug_get_state(&rtc);
    ImGui::Text("20%02d-%02d-%02d %02d:%02d:%02d  %s  %s", rtc.year, rtc.month, rtc.day,
                rtc.hours, rtc.minutes, rtc.seconds, rtc.h24 ? "24h" : "12h",
                rtc.running ? "running" : "stopped");
    ImGui::TextColored(COL_DIM, "register offset %u   NVRAM %s", rtc.i2c_data_pos,
                       rtc.nvram_dirty ? "modified" : "clean");
}

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------

void
draw_serial_tab()
{
    ImGui::TextUnformatted("IEC serial bus lines");
    if (ImGui::BeginTable("serial_lines", 2, DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_text_width("DATA out "));
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);
        row_label("ATN in");
        ImGui::Text("%d", serial_port.in.atn);
        row_label("CLK in");
        ImGui::Text("%d", serial_port.in.clk);
        row_label("DATA in");
        ImGui::Text("%d", serial_port.in.data);
        row_label("CLK out");
        ImGui::Text("%d", serial_port.out.clk);
        row_label("DATA out");
        ImGui::Text("%d", serial_port.out.data);
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextWrapped("These are the physical IEC lines. Devices attached over the host "
                       "filesystem do not use them -- their traffic is intercepted above "
                       "this level and appears in the Files tab.");
}

// ---------------------------------------------------------------------------

void
io_panel_render(bool *p_open)
{
    if (!ImGui::Begin("I/O", p_open)) {
        dbgui_window_end();
        return;
    }
    dbgui_window_zoom("io");

    apply_capture_settings();

    if (ImGui::BeginTabBar("io_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
        if (ImGui::BeginTabItem("Activity")) {
            draw_activity_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SD Card")) {
            draw_sdcard_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Files")) {
            draw_files_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Joysticks")) {
            draw_joystick_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("VIA")) {
            draw_via_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("I2C")) {
            draw_i2c_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Serial")) {
            draw_serial_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    dbgui_window_end();
}

DebugPanelRegistration s_reg("I/O", io_panel_render, true);

} // namespace
