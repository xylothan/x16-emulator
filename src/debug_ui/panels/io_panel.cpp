// I/O panel — what the machine is doing to its ports, and to files.
//
// The other panels answer "what is the state of X". This one mostly answers
// "what just happened", because I/O is a conversation: a command goes out, a
// status comes back, a block is read, a file is opened. A register dump of the
// SD card taken between commands tells you almost nothing; the sequence tells
// you everything.
//
// ON THE TWO FILE PATHS, because this is the panel where the difference bites:
// the emulator can serve files from the host filesystem OR from an attached SD
// card image, and by default those are mutually exclusive -- passing -sdcard
// turns host-filesystem access off unless -hostfsdev asks for it back. Only the
// host path knows filenames; an SD image is 512-byte blocks, with the
// filesystem parsed by ROM inside the emulated machine. So the SD half of the
// Files tab depends on sdcard_fat.c parsing the image on our behalf.
//
// ON TOOLTIPS: this panel describes hardware most people do not have memorised,
// and a bare hex byte with no way to find out what it means is not debugging
// information. Anything that is not self-evident carries a "(?)" or a hover.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_settings.h"
#include "debug_ui_widgets.h"
#include "io_reg_info.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

const ImVec4 COL_READ    = ImVec4(0.55f, 0.78f, 1.00f, 1.0f);
const ImVec4 COL_WRITE   = ImVec4(1.00f, 0.72f, 0.42f, 1.0f);
const ImVec4 COL_DECODED = ImVec4(0.62f, 0.90f, 0.62f, 1.0f);
const ImVec4 COL_DIM     = ImVec4(0.70f, 0.70f, 0.78f, 1.0f);
const ImVec4 COL_BAD     = ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
const ImVec4 COL_ON      = ImVec4(0.45f, 0.95f, 0.50f, 1.0f);

// Tooltip on whatever widget was just submitted.
void
tip(const char *text)
{
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A "(?)" the user can hover. Used wherever a value would otherwise be a bare
// number with no way to find out what it means.
void
help(const char *text)
{
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("(?)");
    tip(text);
}

// A label/value row in a two-column table.
void
row_label(const char *label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(COL_DIM, "%s", label);
    ImGui::TableSetColumnIndex(1);
}

// A lit/unlit indicator, for things that are physically a lamp or a bus line.
void
lamp(bool on, const char *label)
{
    ImGui::TextColored(on ? COL_ON : COL_DIM, "%s %s", on ? "\xe2\x97\x8f" : "\xe2\x97\x8b", label);
}

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

char s_filter[64]  = "";
bool s_autoscroll  = true;
bool s_show_reads  = true;
bool s_show_writes = true;

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

    return (strstr(io_trace_device_name((io_device_t)ev.device), s_filter) != nullptr) ||
           (ev.text[0] != '\0' && strstr(ev.text, s_filter) != nullptr) ||
           (ev.has_addr && strstr(addr, s_filter) != nullptr) ||
           (ev.has_addr && strstr(io_reg_name(ev.addr), s_filter) != nullptr);
}

struct DeviceToggle {
    const char *label;
    bool DebugUiSettings::*field;
    const char *tip;
};

// Every one of these is off by default. The tooltips exist because "should I
// tick VERA?" is only answerable if you know both what it adds over the VERA
// panel and what it costs.
const DeviceToggle RAW_TOGGLES[] = {
    {"VIA", &DebugUiSettings::io_cap_via,
     "$9F00-$9F1F. Both VIAs: the joystick latch and clock, the I2C lines to the "
     "SMC and RTC, the IEC serial lines, and the two timers.\n\n"
     "Adds over the VIA tab: the order and timing of accesses. The tab shows the "
     "registers as they are now; this shows the polling loop that got them there.\n\n"
     "Rate: moderate. The KERNAL touches the VIA every frame."},
    {"VERA", &DebugUiSettings::io_cap_vera,
     "$9F20-$9F3F. All 32 VERA registers.\n\n"
     "Adds over the VERA tab: that tab decodes current state, but says nothing "
     "about the sequence that produced it. Only the log shows the address-then-data "
     "handshake, a mid-frame register change, or a raster split as it happens.\n\n"
     "Rate: VERY HIGH. DATA0/DATA1 auto-increment, so a screen fill is one access "
     "per byte -- thousands per frame. Expect this alone to fill the ring."},
    {"SPI", &DebugUiSettings::io_cap_spi,
     "$9F3E/$9F3F only. These are VERA registers, but they are also the only path "
     "to the SD card, so they are captured separately -- ticking SPI does not drag "
     "in VERA's data ports.\n\n"
     "Adds over the SD Card tab: the raw byte stream, for when you need the SPI "
     "conversation itself rather than the decoded command.\n\n"
     "Rate: high while the card is busy -- one access per byte, 512 per block."},
    {"YM", &DebugUiSettings::io_cap_ym,
     "$9F40-$9F5F. The YM2151's register-select latch and data port.\n\n"
     "Adds over the YM2151 tab: which registers a music driver writes and in what "
     "order, per frame. The tab shows the resulting channel state.\n\n"
     "Rate: moderate; a music driver typically writes a burst every frame."},
    {"Emu", &DebugUiSettings::io_cap_emu,
     "$9FB0-$9FBF. The emulator's own control registers: debug toggles, the cycle "
     "counter, character output, and the \"16\" detection bytes.\n\n"
     "Nothing else shows these. Useful for watching a program probe for the "
     "emulator or drive its recording controls.\n\n"
     "Rate: very low."},
    {"MIDI", &DebugUiSettings::io_cap_midi,
     "The MIDI card, if one is fitted. Its base address is configurable, so this "
     "follows wherever it was placed.\n\n"
     "Rate: low, and zero when no card is fitted."},
    {"Open bus", &DebugUiSettings::io_cap_openbus,
     "Addresses in the I/O page that no device answers -- reads return $9F.\n\n"
     "Almost always noise, but it is the one way to catch a program reading a "
     "device it thinks exists and silently getting garbage back.\n\n"
     "Rate: normally zero."},
};

const DeviceToggle DECODED_TOGGLES[] = {
    {"SD", &DebugUiSettings::io_cap_sd,
     "Decoded SD card activity: each command by name (CMD17 READ_SINGLE_BLOCK), and "
     "every 512-byte block read or written -- resolved to a filename and offset when "
     "the image has been indexed.\n\n"
     "This is the one to leave on when you care about disk access.\n\n"
     "Rate: low. A handful per command, one per block."},
    {"Files", &DebugUiSettings::io_cap_files,
     "Host-filesystem file access with real filenames: opens, closes, directory "
     "listings, DOS commands and status codes.\n\n"
     "The Files tab keeps its own always-on history of these, so you do not need "
     "capture on to see them there. This only adds them to this log, in sequence "
     "with everything else.\n\n"
     "Rate: low -- bounded by real file operations."},
    {"I2C", &DebugUiSettings::io_cap_i2c,
     "One event per completed I2C transaction: which device, direction, and how many "
     "bytes moved.\n\n"
     "Rate: about 60/second and CONSTANT. The KERNAL polls the SMC for keyboard input "
     "every frame whether or not a key is pressed, so this will slowly push everything "
     "else out of the ring even on an idle machine."},
    {"Joystick", &DebugUiSettings::io_cap_joy,
     "One event each time the controllers are latched, with the sampled button masks.\n\n"
     "Rate: about 60/second and CONSTANT -- a game reads the pad every frame whether "
     "or not anything is pressed."},
};

void
draw_capture_controls()
{
    DebugUiSettings &s       = debug_ui_settings();
    bool             changed = false;

    changed |= ImGui::Checkbox("Capture", &s.io_trace_capture);
    tip("Record I/O events into the log below.\n\n"
        "Off by default. This is the only debugger feature the running machine pays "
        "for -- a test and a branch on every I/O access, and a copy on every captured "
        "one -- and the I/O page is the busiest address range in the system. Every "
        "other tab in this panel works without it.");

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        io_trace_clear();
    }
    tip("Discard everything currently in the log.");

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &s_autoscroll);
    tip("Follow the newest event. Scroll up to stop following; scroll back to the "
        "bottom to resume.");

    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(dbgui_field_width("wwwwwwwwwwww"));
    ImGui::InputTextWithHint("##iofilter", "filter", s_filter, sizeof(s_filter));
    tip("Show only rows whose device, address, register name or text contains this. "
        "Case-sensitive substring match, e.g. \"DATA0\", \"$9F23\", \"SD\".");

    ImGui::SameLine(0, 16);
    ImGui::Checkbox("R", &s_show_reads);
    tip("Show reads.");
    ImGui::SameLine();
    ImGui::Checkbox("W", &s_show_writes);
    tip("Show writes.");

    ImGui::TextUnformatted("Registers:");
    help("Raw register accesses, by address range. These overlap the dedicated tabs "
         "only in subject, not in content: a tab shows you state, this log shows you "
         "sequence and timing -- what was written, in what order, and from which PC.");
    for (const DeviceToggle &t : RAW_TOGGLES) {
        ImGui::SameLine();
        changed |= ImGui::Checkbox(t.label, &(s.*(t.field)));
        tip(t.tip);
    }

    ImGui::TextUnformatted("Decoded: ");
    help("Device-level events rather than register accesses -- an SD command by name, "
         "a file being opened, an I2C transaction. These are what the raw bytes mean.");
    for (const DeviceToggle &t : DECODED_TOGGLES) {
        ImGui::SameLine();
        changed |= ImGui::Checkbox(t.label, &(s.*(t.field)));
        tip(t.tip);
    }

    if (changed) {
        debug_ui_settings_mark_dirty();
    }

    if (!s.io_trace_capture) {
        ImGui::TextColored(COL_DIM, "Capture is off. Tick Capture, then pick at least one device.");
    } else if (io_trace_device_mask == IO_TRACE_BIT(IO_DEV_SERIAL)) {
        ImGui::TextColored(COL_DIM, "No devices selected, so nothing is being recorded.");
    } else {
        ImGui::TextColored(COL_DIM, "%d/%d events", io_trace_count(), io_trace_capacity());
        const uint32_t dropped = io_trace_dropped();
        if (dropped > 0) {
            ImGui::SameLine();
            ImGui::TextColored(COL_DIM, "\xc2\xb7 %u overwritten", dropped);
            tip("The ring wrapped and older events were discarded. Raise the capacity in "
                "System > Settings, or capture fewer devices.");
        }
    }
}

void
draw_activity_tab()
{
    draw_capture_controls();
    ImGui::Separator();

    if (ImGui::BeginTable("io_activity", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerV | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Seq", "8888888"));
        ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("PC", "88:8888"));
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Device", "Joystick"));
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Addr", "$FFFF"));
        ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Register", "L0_HSCROLL_L"));
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
            tip("Where the CPU was when this happened.");

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(io_trace_device_name((io_device_t)ev.device));

            ImGui::TableSetColumnIndex(3);
            if (ev.has_addr) {
                ImGui::Text("$%04X", ev.addr);
                tip(io_reg_purpose(ev.addr));
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableSetColumnIndex(4);
            if (ev.has_addr) {
                ImGui::TextColored(COL_DIM, "%s", io_reg_name(ev.addr));
                tip(io_reg_purpose(ev.addr));
            } else {
                ImGui::TextUnformatted(" ");
            }

            ImGui::TableSetColumnIndex(5);
            if (decoded && !ev.has_addr) {
                ImGui::TextUnformatted(" ");
            } else {
                ImGui::TextColored(ev.is_write ? COL_WRITE : COL_READ, "%s",
                                   ev.is_write ? "W" : "R");
            }

            ImGui::TableSetColumnIndex(6);
            if (decoded) {
                ImGui::TextColored(COL_DECODED, "%s", ev.text);
            } else {
                ImGui::Text("$%02X", ev.value);
                dbgui_hover_value_tooltip(io_reg_name(ev.addr), ev.value, 1);
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

const char *
sd_command_name(uint8_t cmd, bool is_acmd)
{
    if (is_acmd) {
        return (cmd == 41) ? "ACMD41 SD_SEND_OP_COND" : nullptr;
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

// Shown wherever the absence of a card would otherwise look like a broken
// panel. The distinction is not obvious and costs people real time.
void
explain_no_sdcard()
{
    ImGui::TextColored(COL_DIM, "No SD card image is attached.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Attach one with:");
    ImGui::Indent();
    ImGui::TextColored(COL_DECODED, "x16emu -sdcard mycard.img");
    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::TextWrapped(
        "-fsroot does NOT attach a card. It sets the host directory the machine sees "
        "when it is using host-filesystem access, which is a different path entirely -- "
        "see the Files tab.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "The two are mutually exclusive by default: passing -sdcard turns host-filesystem "
        "access off unless you also pass -hostfsdev to keep it. So a machine either has a "
        "card and talks to it in 512-byte blocks, or has no card and gets its files by "
        "name from the host.");
}

void
draw_sdcard_tab()
{
    sdcard_debug_state_t sd;
    sdcard_debug_get_state(&sd);

    if (!sd.attached) {
        explain_no_sdcard();
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
        help("SPI slave select. The card only listens while this is asserted, so an idle "
             "line means the machine is not talking to it at all.");

        row_label("Last command");
        {
            const char *name = sd_command_name(sd.last_cmd, sd.last_cmd_is_acmd);
            if (name != nullptr) {
                ImGui::TextUnformatted(name);
            } else {
                ImGui::Text("%sCMD%u", sd.last_cmd_is_acmd ? "A" : "", sd.last_cmd);
            }
            help("The most recent command decoded from the SPI stream. It persists after "
                 "the command completes, so this is the last thing asked of the card, not "
                 "necessarily something in progress.");
        }

        row_label("Block (LBA)");
        ImGui::Text("%u", sd.last_lba);
        dbgui_hover_value_tooltip("byte offset", sd.last_lba * 512u, 4);
        help("Logical block address of the last block command. Blocks are 512 bytes, so "
             "the byte offset into the image is this times 512.");

        row_label("R1 status");
        ImGui::Text("idle=%d  initialised=%d", sd.is_idle ? 1 : 0, sd.is_initialized ? 1 : 0);
        help("The two R1 response bits this emulator models. \"idle\" means the card is in "
             "its power-up idle state and will refuse data commands; the ROM clears it "
             "during start-up with ACMD41.");

        row_label("Multiblock read");
        ImGui::TextUnformatted(sd.ongoing_multiblock_read ? "in progress" : "no");
        help("CMD18 streams blocks until CMD12 stops it. While this is in progress the "
             "card keeps sending without being asked again.");

        row_label("Response");
        ImGui::Text("%d / %d bytes sent", sd.response_counter, sd.response_length);
        help("How far through its reply the card is. A stalled transfer usually shows as a "
             "partial count here.");

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

    ImGui::TextUnformatted("Last block resolves to:");
    help("What the last block access was, in filesystem terms. Needs the image to have "
         "been indexed -- see the Files tab.");
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

    ImGui::Separator();
    vera_spi_debug_state_t spi;
    vera_spi_debug_get_state(&spi);
    ImGui::TextUnformatted("SPI");
    help("The VERA SPI port at $9F3E/$9F3F -- the only wire to the card. \"out\" is the "
         "byte being clocked to the card, \"in\" the byte that came back.");
    ImGui::SameLine();
    ImGui::Text("select=%d  busy=%d  auto-tx=%d  out=$%02X  in=$%02X", spi.ss ? 1 : 0,
                spi.busy ? 1 : 0, spi.autotx ? 1 : 0, spi.sending_byte, spi.received_byte);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

char s_file_filter[64] = "";

const char *
ieee_op_label(uint8_t kind)
{
    switch (kind) {
        case IEEE_OP_OPEN: return "open";
        case IEEE_OP_OPEN_FAILED: return "open FAILED";
        case IEEE_OP_CLOSE: return "close";
        case IEEE_OP_DIR: return "dir";
        case IEEE_OP_COMMAND: return "command";
        case IEEE_OP_STATUS: return "status";
        default: return "?";
    }
}

void
draw_host_files()
{
    ieee_debug_state_t ie;
    ieee_debug_get_state(&ie);

    if (!ie.using_hostfs) {
        ImGui::TextColored(COL_DIM, "Host-filesystem access is off.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "The machine is using the attached SD card image instead -- passing -sdcard "
            "turns host access off by default. Its file activity is block traffic rather "
            "than named files; see the SD image sub-tab.");
        ImGui::Spacing();
        ImGui::TextWrapped("Pass -hostfsdev <unit> alongside -sdcard to keep both available.");
        return;
    }

    ImGui::Text("Unit %d", ie.ieee_unit);
    help("The IEC device number the host filesystem answers as. LOAD\"X\",8 talks to unit "
         "8. Change it with -hostfsdev.");
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "\xc2\xb7 %s",
                       ie.hostfscwd[0] != '\0' ? ie.hostfscwd : "(no root set)");
    tip("The host directory the machine currently sees. Set the starting point with "
        "-fsroot.");

    ImGui::Text("Bus: %s%s%s", ie.listening ? "LISTENING " : "", ie.talking ? "TALKING " : "",
                (!ie.listening && !ie.talking) ? "idle" : "");
    help("IEC protocol state. LISTENING means the machine is sending to the drive, TALKING "
         "that it is receiving. Both are transient -- an idle bus between operations is "
         "normal, not a fault.");

    if (ie.error_str[0] != '\0') {
        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        // The DOS status line always starts with a two-digit code; anything
        // other than 00 is a complaint worth colouring.
        const bool ok = (ie.error_str[0] == '0' && ie.error_str[1] == '0');
        ImGui::TextColored(ok ? COL_DECODED : COL_BAD, "%s", ie.error_str);
        help("The DOS status channel -- what the drive would tell you if you read channel "
             "15. \"00,OK\" means the last operation succeeded.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Recent activity");
    help("Completed file operations, kept whether or not I/O capture is on -- file "
         "operations are rare enough to record for free.\n\n"
         "This exists because a file operation erases itself when it finishes: the channel "
         "list below only shows what is open RIGHT NOW, and a directory listing or a failed "
         "open is over within a millisecond. Without this history, doing @$ or opening a "
         "missing file appeared to do nothing at all.");

    if (ie.history_count == 0) {
        ImGui::TextColored(COL_DIM, "Nothing yet. Try LOAD\"$\",8,1 or a DOS command such as @$.");
    } else if (ImGui::BeginTable("host_hist", 5,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     DBGUI_TABLE_FLAGS_RESIZABLE,
                                 ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("What", "open FAILED"));
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Ch", "88"));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Bytes", "r888888 w888888"));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Status", "62,FILE NOT FOUND,00,00"));
        ImGui::TableHeadersRow();

        for (int i = 0; i < ie.history_count; i++) {
            const ieee_history_entry_t &h      = ie.history[i];
            const bool                  failed = (h.kind == IEEE_OP_OPEN_FAILED) ||
                                (h.kind == IEEE_OP_STATUS);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(failed ? COL_BAD : COL_DECODED, "%s", ieee_op_label(h.kind));
            ImGui::TableSetColumnIndex(1);
            if (h.channel >= 0) {
                ImGui::Text("%d", h.channel);
            } else {
                ImGui::TextUnformatted("-");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(h.name);
            ImGui::TableSetColumnIndex(3);
            if (h.kind == IEEE_OP_CLOSE) {
                ImGui::TextColored(COL_DIM, "r%u w%u", h.bytes_read, h.bytes_written);
            } else {
                ImGui::TextUnformatted(" ");
            }
            ImGui::TableSetColumnIndex(4);
            if (h.status[0] != '\0') {
                const bool ok = (h.status[0] == '0' && h.status[1] == '0');
                ImGui::TextColored(ok ? COL_DIM : COL_BAD, "%s", h.status);
            } else {
                ImGui::TextUnformatted(" ");
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Open channels");
    help("What is open right now. Usually empty -- the machine opens a file, reads it and "
         "closes it faster than anyone can look. The history above is the useful view.");

    bool any = false;
    for (int i = 0; i < 16; i++) {
        if (ie.channels[i].is_open || ie.channels[i].name[0] != '\0') {
            any = true;
            break;
        }
    }
    if (!any) {
        ImGui::TextColored(COL_DIM, "None open.");
        return;
    }

    if (ImGui::BeginTable("host_files", 5, ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE)) {
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
            if (!c.is_open && c.name[0] == '\0') {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(c.is_open ? COL_DECODED : COL_DIM, "%s",
                               c.name[0] != '\0' ? c.name : "(closing)");
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
draw_sd_files()
{
    sdcard_debug_state_t sd;
    sdcard_debug_get_state(&sd);

    if (!sd.attached) {
        explain_no_sdcard();
        return;
    }

    ImGui::TextWrapped(
        "The emulator only ever sees 512-byte blocks here -- the filesystem inside the "
        "image is parsed by the ROM running on the emulated machine, not by us. To put "
        "names to that traffic the emulator parses the image itself:");

    if (ImGui::SmallButton(sdcard_fat_ready() ? "Rebuild index" : "Build index")) {
        sdcard_reindex();
    }
    tip("Read the image's partition table, boot sector, FATs and directory tree, and map "
        "every cluster back to a path.");

    if (!sdcard_fat_ready()) {
        ImGui::SameLine();
        ImGui::TextColored(COL_DIM, "not indexed");
        ImGui::Spacing();
        ImGui::TextWrapped("Only FAT16 and FAT32 images can be indexed. If the button leaves "
                           "this saying \"not indexed\", the image is neither.");
        return;
    }

    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "%s \xc2\xb7 %u bytes/cluster \xc2\xb7 %d entries",
                       sdcard_fat_type_name(), sdcard_fat_bytes_per_cluster(),
                       sdcard_fat_file_count());

    if (sdcard_fat_is_stale()) {
        ImGui::SameLine();
        ImGui::TextColored(COL_WRITE, "\xc2\xb7 STALE");
        tip("The machine wrote to the FAT or a directory since this index was built, so "
            "names and extents may no longer be right. Rebuild to refresh.\n\n"
            "The index is never refreshed automatically: quietly serving a filename that "
            "had since become wrong would be worse than saying so.");
    }
    if (sdcard_fat_truncated()) {
        ImGui::SameLine();
        ImGui::TextColored(COL_WRITE, "\xc2\xb7 TRUNCATED");
        tip("The image holds more files than the index will hold. What is listed is "
            "correct; it is just not everything.");
    }

    ImGui::SetNextItemWidth(dbgui_field_width("wwwwwwwwwwwwwwww"));
    ImGui::InputTextWithHint("##sdfilter", "filter", s_file_filter, sizeof(s_file_filter));
    tip("Show only paths containing this text.");
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "files the machine has touched are listed first, in green");

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
draw_files_tab()
{
    // Shown as two sub-tabs rather than one merged list because which of them
    // is live is decided at start-up, and a reader chasing a missing file needs
    // to see which half is even in play.
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

const JoyBit JOY_DPAD[]  = {{JOY_BIT_DPAD_UP, "Up"},
                            {JOY_BIT_DPAD_DOWN, "Down"},
                            {JOY_BIT_DPAD_LEFT, "Left"},
                            {JOY_BIT_DPAD_RIGHT, "Right"}};
const JoyBit JOY_FACE[]  = {{JOY_BIT_A, "A"},
                            {JOY_BIT_B, "B"},
                            {JOY_BIT_X, "X"},
                            {JOY_BIT_Y, "Y"}};
const JoyBit JOY_OTHER[] = {{JOY_BIT_LEFT_SHOULDER, "L"},
                            {JOY_BIT_RIGHT_SHOULDER, "R"},
                            {JOY_BIT_SELECT, "Select"},
                            {JOY_BIT_START, "Start"}};

// Buttons are active-low on the wire, so a pressed button is a ZERO bit.
// Everything below inverts once, here, so no reader has to do it in their head.
void
draw_button_group(const char *label, const JoyBit *bits, size_t count, uint16_t mask)
{
    ImGui::TextColored(COL_DIM, "%-6s", label);
    for (size_t b = 0; b < count; b++) {
        ImGui::SameLine();
        if ((mask & bits[b].mask) == 0) {
            ImGui::TextColored(COL_ON, "[%s]", bits[b].name);
        } else {
            ImGui::TextDisabled("%s", bits[b].name);
        }
    }
}

void
draw_joystick_tab()
{
    joystick_debug_state_t js;
    joystick_debug_get_state(&js);

    ImGui::TextUnformatted("How the machine reads a controller");
    help("The controllers are shift registers, not a parallel port.\n\n"
         "The VIA asserts NESLATCH (PA2), which snapshots every controller's buttons at "
         "once. It then pulses NESCLK (PA3) sixteen times, and each pulse shifts the next "
         "button bit onto that slot's data line -- NESDAT0..3 on PA7..PA4. All four "
         "controllers are read in parallel, one bit at a time.\n\n"
         "That is why the raw byte at $9F01 is hard to read directly: it holds one bit of "
         "four different controllers, and which bit depends on how far through the shift "
         "sequence you are. The decoded buttons below are the useful view.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Latch (PA2)");
    ImGui::SameLine();
    lamp(js.latch, js.latch ? "asserted -- sampling now" : "idle");
    help("While asserted, the controllers reload their shift registers from the live "
         "button state. A game pulses this once per frame.");

    ImGui::Text("Controller data bits: $%02X", js.data);
    dbgui_hover_value_tooltip("joystick data", js.data, 1);
    help("The joystick contribution to VIA1 port A -- bits 7-4, one per controller slot, "
         "carrying whichever bit is currently being shifted out. Bits 3-0 are always zero "
         "here; the I2C lines that share port A are merged in separately when the CPU "
         "actually reads $9F01, so this is not the whole byte the CPU sees.");

    ImGui::Separator();

    for (int i = 0; i < NUM_JOYSTICKS; i++) {
        const joystick_slot_debug_t &s = js.slots[i];
        ImGui::PushID(i);

        ImGui::Text("Slot %d", i);
        ImGui::SameLine();
        if (!s.enabled) {
            ImGui::TextDisabled("disabled");
            help("Not enabled. Enable a slot with -joy1 .. -joy4 on the command line. A "
                 "disabled slot reads to the machine as \"nothing pressed\".");
        } else if (!s.controller_bound) {
            ImGui::TextColored(COL_DIM, "enabled, no gamepad connected");
            help("The slot exists as far as the machine is concerned, but no host gamepad "
                 "is bound to it, so it reports nothing pressed.");
        } else {
            ImGui::TextColored(COL_ON, "connected");
        }

        if (s.enabled) {
            ImGui::Indent();
            draw_button_group("D-pad", JOY_DPAD, sizeof(JOY_DPAD) / sizeof(JOY_DPAD[0]),
                              s.button_mask);
            draw_button_group("Face", JOY_FACE, sizeof(JOY_FACE) / sizeof(JOY_FACE[0]),
                              s.button_mask);
            draw_button_group("Other", JOY_OTHER, sizeof(JOY_OTHER) / sizeof(JOY_OTHER[0]),
                              s.button_mask);

            ImGui::TextColored(COL_DIM, "raw $%04X   shifting $%04X", s.button_mask,
                               s.shift_mask);
            help("Raw values, for when you are debugging the read routine rather than the "
                 "input.\n\n"
                 "\"raw\" is the latched button state, and a pressed button is a ZERO bit "
                 "because the wire is active-low -- so an idle pad reads $FFFF.\n\n"
                 "\"shifting\" is what remains to be clocked out. It advances on every "
                 "NESCLK pulse, so part-way through a read it will not match the raw value.");
            ImGui::Unindent();
        }

        ImGui::PopID();
        ImGui::Separator();
    }
}

// ---------------------------------------------------------------------------
// VIA
// ---------------------------------------------------------------------------

void
draw_via_ports(int which, const via_debug_state_t &v)
{
    // Pin-level view: the register bytes are only meaningful once you know what
    // each bit is soldered to, and on VIA1 every bit is something different --
    // I2C, the joysticks and the IEC bus all share two ports.
    ImGui::TextUnformatted("Port pins");
    help("What each bit of each port is physically connected to, and whether the VIA is "
         "currently driving it (output) or reading it (input) -- which is what the data "
         "direction register decides.\n\n"
         "Level is shown only for output pins. An input pin's level is composed at read "
         "time from whatever device is on the other end, so the register file the debugger "
         "snapshots does not hold it.");

    if (ImGui::BeginTable("via_pins", 4, ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("Pin", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Pin", "PA0"));
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Dir", "output"));
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Level", "0"));
        ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const struct {
            char    port;
            uint8_t data;
            uint8_t ddr;
        } ports[2] = {{'A', v.regs[1], v.regs[3]}, {'B', v.regs[0], v.regs[2]}};

        for (int p = 0; p < 2; p++) {
            for (int bit = 7; bit >= 0; bit--) {
                const bool is_out = (ports[p].ddr >> bit) & 1;
                const bool level  = (ports[p].data >> bit) & 1;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("P%c%d", ports[p].port, bit);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(COL_DIM, "%s", is_out ? "output" : "input");
                ImGui::TableSetColumnIndex(2);
                if (is_out) {
                    ImGui::TextColored(level ? COL_ON : COL_DIM, "%d", level ? 1 : 0);
                } else {
                    // The snapshot carries the output latch, which says nothing
                    // about an input pin -- on VIA1 the joystick and IEC input
                    // pins are composed by via_read() from other devices
                    // entirely. Showing the latch here would be a lamp stuck at
                    // whatever was last written, presented as a live reading.
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(io_via_pin_name(which, ports[p].port, bit));
            }
        }
        ImGui::EndTable();
    }
}

void
draw_one_via(int which, const char *name)
{
    via_debug_state_t v;
    via_debug_get_state(which, &v);

    if (!v.exists) {
        ImGui::TextColored(COL_DIM, "%s is not fitted.", name);
        ImGui::Spacing();
        ImGui::TextWrapped("The second VIA is optional on the X16 and drives the user port. "
                           "This emulator wires its pins to nothing, so it is only useful "
                           "for exercising the timers and interrupts.");
        return;
    }

    const uint8_t acr = v.regs[0x0b];
    const uint8_t ifr = v.regs[0x0d];
    const uint8_t ier = v.regs[0x0e];

    ImGui::TextUnformatted("Timers");
    help("Two 16-bit counters running down at the system clock. They are the usual source "
         "of periodic interrupts on this machine.");

    ImGui::Indent();
    ImGui::Text("T1  %5u  %s", v.timer_count[0], v.timer_running[0] ? "running" : "stopped");
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "reload %u \xc2\xb7 %s", v.timer_latch[0],
                       (acr & 0x40) ? "free-running" : "one-shot");
    help("Timer 1. In one-shot mode it counts down once, raises its interrupt flag and "
         "stops. In free-running mode it reloads from the latch and repeats, which is how "
         "a steady interrupt tick is produced. ACR bit 6 chooses between them.\n\n"
         "Writing T1C_H starts it; writing T1L_H only changes the reload value.");

    ImGui::Text("T2  %5u  %s", v.timer_count[1], v.timer_running[1] ? "running" : "stopped");
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "reload %u \xc2\xb7 %s", v.timer_latch[1],
                       (acr & 0x20) ? "counting PB6 pulses" : "one-shot");
    help("Timer 2 is one-shot only. ACR bit 5 switches it from counting clock cycles to "
         "counting falling edges on PB6, which is how external pulses get measured.");

    ImGui::TextUnformatted("PB7 output");
    ImGui::SameLine();
    if (acr & 0x80) {
        lamp(v.pb7_output, v.pb7_output ? "high" : "low");
    } else {
        ImGui::TextDisabled("disabled");
    }
    help("With ACR bit 7 set, timer 1 toggles pin PB7 on every underflow, turning the timer "
         "into a square-wave generator that needs no CPU attention at all. With it clear, "
         "PB7 is an ordinary port pin.");
    ImGui::Unindent();

    ImGui::Separator();
    ImGui::TextUnformatted("Interrupts");
    help("A source raises its flag in IFR. That only pulls the CPU's IRQ line if the same "
         "bit is also enabled in IER -- so a set flag on its own does not mean an interrupt "
         "fired. Look for a bit that is set in both columns.");

    if (ImGui::BeginTable("via_irq", 4, ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("Bit", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Bit", "8"));
        ImGui::TableSetupColumn("Flag", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Flag", "set"));
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Enabled", "yes"));
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int bit = 6; bit >= 0; bit--) {
            const bool flag = (ifr >> bit) & 1;
            const bool en   = (ier >> bit) & 1;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(COL_DIM, "%d", bit);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(flag ? COL_WRITE : COL_DIM, "%s", flag ? "set" : "-");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(en ? COL_ON : COL_DIM, "%s", en ? "yes" : "no");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(io_via_irq_bit_name(bit));
        }
        ImGui::EndTable();
    }
    // Bit 7 of IFR is synthesised by via_read() and never stored, so testing it
    // in the snapshot would report "idle" even while the VIA is holding the
    // IRQ line down. Compute it the way the VIA's own irq function does.
    const bool irq_asserted = (ifr & ier & 0x7f) != 0;
    ImGui::TextColored(irq_asserted ? COL_WRITE : COL_DIM, "IRQ line: %s",
                       irq_asserted ? "asserted" : "idle");
    help("Set when any flag above is both raised and enabled. While it is asserted, this "
         "VIA is holding the CPU's interrupt line down.");

    ImGui::Separator();
    draw_via_ports(which, v);

    ImGui::Separator();
    ImGui::TextUnformatted("Registers");
    help("The raw 65C22 register file -- hover a name for what it does.\n\n"
         "Several of these have side effects when the CPU reads them: reading a timer "
         "counter clears its interrupt flag. The debugger reads a snapshot instead, so "
         "looking here changes nothing.");

    if (ImGui::BeginTable("via_regs", 4, ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Addr", "$FFFF"));
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Name", "ORA/IRA"));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Value", "$FF"));
        ImGui::TableSetupColumn("Binary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const uint16_t base = (which == 0) ? 0x9f00 : 0x9f10;
        for (int r = 0; r < 15; r++) {
            const uint16_t addr = (uint16_t)(base + r);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(COL_DIM, "$%04X", addr);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(io_reg_name(addr));
            tip(io_reg_purpose(addr));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("$%02X", v.regs[r]);
            dbgui_hover_value_tooltip(io_reg_name(addr), v.regs[r], 1);
            ImGui::TableSetColumnIndex(3);
            char bits[16];
            dbgui_format_binary(bits, sizeof(bits), v.regs[r], 8);
            ImGui::TextColored(COL_DIM, "%s", bits);
        }
        ImGui::EndTable();
    }
}

void
draw_via_tab()
{
    if (ImGui::BeginTabBar("via_sub")) {
        if (ImGui::BeginTabItem("VIA1")) {
            ImGui::TextWrapped("VIA1 is where the I2C bus, the joysticks and the IEC serial "
                               "bus all meet -- three unrelated things sharing two 8-bit "
                               "ports.");
            ImGui::Separator();
            draw_one_via(0, "VIA1");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("VIA2")) {
            draw_one_via(1, "VIA2");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------
// I2C, and the SMC and RTC behind it
// ---------------------------------------------------------------------------

void
draw_i2c_bus()
{
    i2c_debug_state_t i2c;
    i2c_debug_get_state(&i2c);

    ImGui::TextWrapped("A two-wire bus hanging off VIA1 port A. Exactly two devices answer on "
                       "it: the SMC and the RTC. Everything the keyboard, mouse, power button "
                       "and clock do passes through here.");
    ImGui::Separator();

    if (ImGui::BeginTable("i2c_state", 2, DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_text_width("Transactions "));
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        row_label("State");
        if (i2c.state < 0) {
            ImGui::TextUnformatted("idle");
        } else if (i2c.state == 0) {
            ImGui::TextUnformatted("start condition");
        } else {
            ImGui::Text("shifting bit %d of 8", i2c.state);
        }
        help("Where the bus is within the current byte. Transfers are bit-at-a-time, so "
             "catching it mid-byte is normal. \"idle\" means no transaction is in flight, "
             "which is where the bus spends most of its time.");

        row_label("Device");
        if (i2c.device_name[0] != '\0') {
            ImGui::Text("$%02X (%s)", i2c.device, i2c.device_name);
        } else if (i2c.device != 0) {
            ImGui::TextColored(COL_BAD, "$%02X (not acknowledged)", i2c.device);
        } else {
            ImGui::TextDisabled("none");
        }
        help("Which device the current transaction is addressed to. $42 is the SMC and $6F "
             "the RTC; anything else goes unacknowledged, which usually means software is "
             "probing for hardware that is not there.");

        row_label("Direction");
        ImGui::TextUnformatted(i2c.read_mode ? "reading from device" : "writing to device");

        row_label("Byte");
        ImGui::Text("$%02X", i2c.value);
        ImGui::SameLine();
        ImGui::TextColored(COL_DIM, "(byte %d of this transaction)", i2c.count);
        help("The byte being shifted in or out, and how far into the transaction it is. The "
             "first byte written is normally the register number.");

        row_label("Lines");
        ImGui::Text("SCL in %d   SDA in %d   SDA out %d", i2c.clk_in, i2c.data_in, i2c.data_out);
        help("The raw bus wires. SCL is the clock, always driven by the machine. SDA carries "
             "data in both directions, so it has a separate in and out here.");

        row_label("Transactions");
        ImGui::Text("%u started, %u completed", i2c.transactions_started,
                    i2c.transactions_completed);
        help("Counted since start-up. A growing gap between the two means transactions are "
             "being abandoned part-way, which normally indicates a bus problem.");

        row_label("Bytes");
        ImGui::Text("%u read, %u written", i2c.bytes_read, i2c.bytes_written);
        ImGui::EndTable();
    }
}

void
draw_smc()
{
    smc_debug_state_t smc;
    smc_debug_get_state(&smc);

    ImGui::TextWrapped("The System Management Controller: a small microcontroller that owns "
                       "the keyboard, the mouse, the power and reset buttons and the case "
                       "LEDs. The main CPU talks to it over I2C at address $42.");
    ImGui::Separator();

    if (ImGui::BeginTable("smc_state", 2, DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_text_width("Default read reg "));
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        row_label("Default read reg");
        ImGui::Text("$%02X", smc.default_read_op);
        ImGui::SameLine();
        ImGui::TextColored(COL_DECODED, "%s", io_smc_reg_name(smc.default_read_op));
        tip(io_smc_reg_purpose(smc.default_read_op));
        help("Which register a bare read returns -- one where the machine reads from the SMC "
             "without first writing a register number. The KERNAL sets this once at start-up "
             "so its keyboard polling loop can be a single short read.\n\n"
             "$41 is the power-on default, and returns a keyboard byte.");

        row_label("Transfer offset");
        ImGui::Text("%u", smc.i2c_data_pos);
        help("How many bytes into the current I2C transfer the SMC is. It resets at the start "
             "of each transaction: byte 0 is the register number, byte 1 the value. Anything "
             "other than 0 means a transfer is in flight right now.");

        row_label("Activity LED");
        lamp(smc.activity_led >= 128, smc.activity_led >= 128 ? "on" : "off");
        help("The case's disk-activity lamp. Real hardware dims it by PWM, but this emulator "
             "models it as simply on or off, switching at a value of 128.");

        row_label("Keyboard buffer");
        ImGui::Text("%u byte%s waiting", smc.kbd_fill, smc.kbd_fill == 1 ? "" : "s");
        help("Key events the SMC is holding until the machine collects them. It is a 16-byte "
             "ring; if this sits full, the machine has stopped polling.");

        row_label("Mouse buffer");
        ImGui::Text("%u byte%s waiting", smc.mse_fill, smc.mse_fill == 1 ? "" : "s");
        help("Mouse movement waiting to be collected. Packets are three bytes, or four when "
             "a wheel is reported.");

        row_label("Mouse packets");
        ImGui::Text("%u", smc.mse_count);
        ImGui::EndTable();
    }

    if (smc.requested_reset) {
        ImGui::TextColored(COL_BAD, "Reset requested");
        help("The SMC has been told to reboot the machine and is waiting for the emulator to "
             "act on it.");
    }
}

void
draw_rtc()
{
    rtc_debug_state_t rtc;
    rtc_debug_get_state(&rtc);

    ImGui::TextWrapped("A battery-backed real-time clock (MCP7940N) on the same I2C bus, at "
                       "address $6F. It also carries 64 bytes of non-volatile memory that "
                       "survive a power cycle.");
    ImGui::Separator();

    if (ImGui::BeginTable("rtc_state", 2, DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_text_width("Transfer offset "));
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch);

        row_label("Date and time");
        ImGui::Text("20%02d-%02d-%02d  %02d:%02d:%02d", rtc.year, rtc.month, rtc.day, rtc.hours,
                    rtc.minutes, rtc.seconds);
        help("The clock's own idea of the time. It is stored on the device as BCD and shown "
             "here decoded. The year field only holds 00-99, meaning 2000-2099.");

        row_label("Oscillator");
        lamp(rtc.running, rtc.running ? "running" : "stopped");
        help("Whether the clock is ticking. Stopped is the normal state for an RTC that has "
             "never been set -- the machine starts it when it first writes the time.");

        row_label("Hour format");
        ImGui::TextUnformatted(rtc.h24 ? "24-hour" : "12-hour");
        help("A flag in the hours register. It only affects how the device reports the hour, "
             "not how it counts.");

        row_label("Day of week");
        ImGui::Text("%d", rtc.day_of_week);
        help("1-7, exactly as stored. The RTC does not derive this from the date -- whatever "
             "set the time chose it, so it is free to disagree with the date beside it.");

        row_label("Transfer offset");
        ImGui::Text("%u", rtc.i2c_data_pos);
        help("How many bytes into the current I2C transfer the RTC is. Byte 0 selects the "
             "register, so anything other than 0 means a transfer is in progress.");

        row_label("NVRAM");
        ImGui::Text("64 bytes, %s", rtc.nvram_dirty ? "modified" : "unmodified");
        help("Non-volatile storage at RTC registers $20-$5F. The emulator writes it back to "
             "the file given by -nvram. \"modified\" means there are changes not yet saved.");
        ImGui::EndTable();
    }
}

void
draw_i2c_tab()
{
    if (ImGui::BeginTabBar("i2c_sub")) {
        if (ImGui::BeginTabItem("Bus")) {
            draw_i2c_bus();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SMC")) {
            draw_smc();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("RTC")) {
            draw_rtc();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------

void
draw_serial_line(const char *name, int level, const char *what)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(COL_DIM, "%s", name);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(level ? COL_ON : COL_DIM, "%d", level);
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(what);
}

void
draw_serial_tab()
{
    ImGui::TextWrapped("The IEC serial bus -- the Commodore disk-drive and printer bus. Three "
                       "wires, driven bit by bit in software, wired to VIA1 port B.");
    ImGui::Spacing();
    ImGui::TextWrapped("Host-filesystem file access does NOT use these lines. It is "
                       "intercepted at the KERNAL level, well above the wire, so the bus stays "
                       "idle while files are being read. These only move when real IEC "
                       "signalling happens, which needs -serial and a device on the bus.");
    ImGui::Separator();

    if (ImGui::BeginTable("serial_lines", 3,
                          ImGuiTableFlags_RowBg | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Line", "DATA out"));
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed,
                                dbgui_col_width("Level", "0"));
        ImGui::TableSetupColumn("What it is for", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        draw_serial_line("ATN in", serial_port.in.atn,
                         "Attention. The computer asserts it to interrupt every device on the "
                         "bus and announce that a command, not data, is coming next.");
        draw_serial_line("CLK in", serial_port.in.clk,
                         "Clock, as driven by the computer. Each transition clocks one bit of "
                         "the byte being sent.");
        draw_serial_line("DATA in", serial_port.in.data,
                         "Data, as driven by the computer.");
        draw_serial_line("CLK out", serial_port.out.clk,
                         "Clock, as driven by the attached device when it is the talker.");
        draw_serial_line("DATA out", serial_port.out.data,
                         "Data, as driven by the attached device -- both when sending bytes "
                         "and when holding the line to say it is not ready yet.");
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextWrapped("The bus is open-collector and active-low: a device pulls a line down "
                       "to assert it, so 0 is the asserted state and an undriven line floats "
                       "to 1.");
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
