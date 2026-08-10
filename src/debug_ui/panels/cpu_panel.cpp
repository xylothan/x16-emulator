// CPU panel — live registers, status flags, stack, and address watches for the
// in-process ImGui debugger.
//
// Everything lives in the single "CPU" window (one CollapsingHeader per area)
// so the whole panel is contributed by this one already-registered .cpp.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h" // extern "C" struct regs regs; debug_ui_read6502()
#include "debug_ui_widgets.h" // dbgui_hover_value_tooltip / dbgui_value_lines

namespace {

// Hex text-entry flags shared by every register/address input field.
const ImGuiInputTextFlags kHexFlags =
    ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase;

// ----------------------------------------------------------------------------
// CPU-mode helpers. The X16 boots a 65C02; the 65C816 adds a native mode
// (regs.e == 0) with selectable 8/16-bit accumulator (M flag) and index (X
// flag) widths. In emulation mode (regs.e == 1) it behaves like the 6502, so
// the widths are forced to 8-bit regardless of the stored M/X status bits.
// ----------------------------------------------------------------------------
bool cpu_native() { return regs.is65c816 && !regs.e; }
bool acc_is16()   { return cpu_native() && !(regs.status & FLAG_MEMORY_WIDTH); }
bool idx_is16()   { return cpu_native() && !(regs.status & FLAG_INDEX_WIDTH); }

char ascii_of(uint8_t b) { return (b >= 0x20 && b < 0x7f) ? (char)b : '.'; }

// Width (in pixels) sized to hold `digits` hex characters plus a little padding.
float hex_width(int digits)
{
    return ImGui::CalcTextSize("F").x * (float)(digits + 1) +
           ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
}

bool reg_u8(const char *label, uint8_t *p, const char *desc = nullptr)
{
    ImGui::SetNextItemWidth(hex_width(2));
    bool changed = ImGui::InputScalar(label, ImGuiDataType_U8, p, nullptr, nullptr, "%02X", kHexFlags);
    dbgui_hover_value_tooltip(desc, *p, 1);
    return changed;
}

bool reg_u16(const char *label, uint16_t *p, const char *desc = nullptr)
{
    ImGui::SetNextItemWidth(hex_width(4));
    bool changed = ImGui::InputScalar(label, ImGuiDataType_U16, p, nullptr, nullptr, "%04X", kHexFlags);
    dbgui_hover_value_tooltip(desc, *p, 2);
    return changed;
}

// A single P-status flag rendered as a checkbox that reads/writes regs.status.
// `label` carries a "##id" suffix so single-letter flags keep unique ImGui IDs.
// `name`/`desc` drive a hover tooltip that explains what the flag means.
void flag_checkbox(const char *label, uint8_t mask, const char *name, const char *desc)
{
    bool v = (regs.status & mask) != 0;
    if (ImGui::Checkbox(label, &v)) {
        if (v) {
            regs.status |= mask;
        } else {
            regs.status = (uint8_t)(regs.status & ~mask);
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        int bit = 0;
        for (uint8_t m = mask; m > 1; m >>= 1) {
            bit++;
        }
        ImGui::BeginTooltip();
        ImGui::Text("%s  (bit %d, mask $%02X)", name, bit, mask);
        ImGui::Separator();
        ImGui::TextUnformatted(desc);
        ImGui::TextDisabled("current value: %d", (regs.status & mask) ? 1 : 0);
        ImGui::EndTooltip();
    }
}

// ----------------------------------------------------------------------------
// Registers
// ----------------------------------------------------------------------------
void draw_registers()
{
    if (!ImGui::CollapsingHeader("Registers", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::PushID("registers");

    const bool is816  = regs.is65c816;
    const bool native = cpu_native();

    const char *mode = !is816 ? "65C02 (6502)"
                              : (regs.e ? "65C816 (emulation)" : "65C816 (native)");
    ImGui::Text("Mode: %s", mode);
    if (is816) {
        ImGui::SameLine();
        ImGui::TextDisabled("(M=%d X=%d E=%d)", (regs.status >> 5) & 1,
                            (regs.status >> 4) & 1, regs.e ? 1 : 0);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The register set follows the CPU mode: A/X/Y are 8-bit in\n"
                          "6502/emulation, and widen (A->C 16-bit) in 65C816 native when\n"
                          "the M/X flags select 16-bit. 65C816-only registers appear only\n"
                          "in 65C816 modes.");
    }
    ImGui::Separator();

    reg_u16("PC##rPC", &regs.pc, "PC — Program Counter\nAddress of the next instruction to execute.");

    // Accumulator adapts to the current width: 16-bit -> a single 'C' (A = low
    // byte, B = high byte); 8-bit -> 'A'. B is folded in (shown in the tooltip),
    // never a separate field.
    if (acc_is16()) {
        char desc[128];
        snprintf(desc, sizeof desc,
                 "C — 16-bit accumulator\nA (low) = $%02X   B (high) = $%02X   (C = B:A)",
                 regs.a, regs.b);
        reg_u16("C##rC", &regs.c, desc);
    } else {
        char desc[128];
        if (is816) {
            snprintf(desc, sizeof desc,
                     "A — accumulator (8-bit)\nHidden high byte B = $%02X (used when the "
                     "accumulator is 16-bit).",
                     regs.b);
        } else {
            snprintf(desc, sizeof desc, "A — accumulator (8-bit)");
        }
        reg_u8("A##rA", &regs.a, desc);
    }

    // Index registers X/Y widen with the X flag (16-bit in native when X=0).
    if (idx_is16()) {
        reg_u16("X##rX", &regs.x, "X — index register (16-bit)");
        ImGui::SameLine();
        reg_u16("Y##rY", &regs.y, "Y — index register (16-bit)");
    } else {
        reg_u8("X##rX", &regs.xl, "X — index register (8-bit)");
        ImGui::SameLine();
        reg_u8("Y##rY", &regs.yl, "Y — index register (8-bit)");
    }

    // Stack pointer: page-1 8-bit form in 6502/emulation, full 16-bit in native.
    {
        uint16_t sp_disp = (uint16_t)(regs.sp | (is816 ? 0 : 0x100));
        if (reg_u16("SP##rSP", &sp_disp,
                    "SP — Stack Pointer\n6502/emulation: page-1 ($01xx). 65C816 native: 16-bit.")) {
            regs.sp = is816 ? sp_disp : (uint16_t)(sp_disp & 0xFF);
        }
    }

    // 65C816-only registers appear only in 65C816 modes (not shown at all on the
    // 65C02). DP is native-only; DB/K/E exist in emulation + native.
    if (is816) {
        ImGui::Separator();
        if (native) {
            reg_u16("DP  (Direct Page)##rDP", &regs.dp,
                    "DP — Direct Page register\nBase of the direct (zero) page. 65C816 native only.");
        }
        reg_u8("DB  (Data Bank)##rDB", &regs.db,
               "DB — Data Bank register\nHigh byte (bank) for data memory accesses.");
        ImGui::SameLine();
        reg_u8("K  (Program Bank)##rK", &regs.k,
               "K — Program Bank register\nHigh byte (bank) of the 24-bit program counter.");

        bool e = regs.e != 0;
        if (ImGui::Checkbox("E  (Emulation)##rE", &e)) {
            regs.e = e ? 1 : 0;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("E — Emulation flag\n1 = 6502 emulation mode; 0 = 65C816 native.");
        }
    }

    ImGui::PopID();
}

// ----------------------------------------------------------------------------
// Status flags
// ----------------------------------------------------------------------------
void draw_flags()
{
    if (!ImGui::CollapsingHeader("Status Flags", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::PushID("flags");

    ImGui::Text("P = $%02X", regs.status);
    ImGui::SameLine();
    ImGui::TextDisabled(regs.is65c816 ? "  [N V M X D I Z C]" : "  [N V - B D I Z C]");
    ImGui::SameLine();
    ImGui::TextDisabled("(hover a flag for details)");

    flag_checkbox("N##fN", FLAG_SIGN, "N — Negative",
                  "Set when bit 7 of the result is set (result looks negative).");
    ImGui::SameLine();
    flag_checkbox("V##fV", FLAG_OVERFLOW, "V — Overflow",
                  "Set on signed arithmetic overflow (ADC/SBC), or by BIT/CLV.");
    ImGui::SameLine();
    if (regs.is65c816) {
        flag_checkbox("M##fM", FLAG_MEMORY_WIDTH, "M — Memory/Accumulator width",
                      "65C816 native only. 0 = 16-bit accumulator/memory, 1 = 8-bit.");
        ImGui::SameLine();
        flag_checkbox("X##fX", FLAG_INDEX_WIDTH, "X — Index register width",
                      "65C816 native only. 0 = 16-bit X/Y, 1 = 8-bit. Forced 1 in emulation.");
        ImGui::SameLine();
    } else {
        flag_checkbox("-##fConst", FLAG_CONSTANT, "- — Unused",
                      "Unused on the 65C02; conventionally reads as 1.");
        ImGui::SameLine();
        flag_checkbox("B##fBrk", FLAG_BREAK, "B — Break",
                      "Distinguishes a BRK/PHP push from an IRQ/NMI push on the stack.");
        ImGui::SameLine();
    }
    flag_checkbox("D##fD", FLAG_DECIMAL, "D — Decimal",
                  "Enables BCD (binary-coded decimal) mode for ADC/SBC.");
    ImGui::SameLine();
    flag_checkbox("I##fI", FLAG_INTERRUPT, "I — IRQ disable",
                  "1 masks maskable IRQs; NMIs are unaffected.");
    ImGui::SameLine();
    flag_checkbox("Z##fZ", FLAG_ZERO, "Z — Zero",
                  "Set when the result of an operation is zero.");
    ImGui::SameLine();
    flag_checkbox("C##fC", FLAG_CARRY, "C — Carry",
                  "Carry/borrow out of bit 7 (bit 15 in 16-bit), and the bit shifted by shifts/rotates.");

    if (regs.is65c816) {
        bool e = regs.e != 0;
        if (ImGui::Checkbox("E  (Emulation)##fE", &e)) {
            regs.e = e ? 1 : 0;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("E — Emulation mode\n"
                              "1 = 6502 emulation (8-bit, page-1 stack); 0 = 65C816 native.");
        }
    }

    ImGui::PopID();
}

// ----------------------------------------------------------------------------
// Stack
// ----------------------------------------------------------------------------
void draw_stack()
{
    if (!ImGui::CollapsingHeader("Stack", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::PushID("stack");

    // On the 6502/65C02 the SP is an 8-bit offset into page 1; on the 65C816 it
    // is already the full 16-bit stack address.
    uint16_t sp_actual = (uint16_t)(regs.sp | (regs.is65c816 ? 0 : 0x100));
    uint16_t sp_top    = (uint16_t)(sp_actual + 1); // most recently pushed byte
    bool page1_stack   = !cpu_native();             // 6502/emulation stay in page 1

    ImGui::Text("SP = $%04X   top-of-stack = $%04X", sp_actual, sp_top);

    static int rows = 16;
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("rows", &rows, 4, 64);

    const ImVec4 hi(1.0f, 0.85f, 0.35f, 1.0f);
    float line = ImGui::GetTextLineHeightWithSpacing();
    int visible = rows < 16 ? rows : 16;

    if (ImGui::BeginChild("stack_scroll", ImVec2(0.0f, line * (visible + 1.5f)), true)) {
        if (ImGui::BeginTable("stack_tbl", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Addr");
            ImGui::TableSetupColumn("Byte");
            ImGui::TableSetupColumn("ASCII");
            ImGui::TableHeadersRow();

            for (int i = 0; i < rows; i++) {
                uint32_t addr = (uint32_t)sp_top + (uint32_t)i;
                if (page1_stack && addr > 0x01FF) {
                    break; // used stack cannot extend past $01FF in page 1
                }
                uint16_t a16  = (uint16_t)addr;
                uint8_t  byte = debug_ui_read6502(a16, 0, DEBUG_UI_CURRENT_BANK);
                bool     top  = (i == 0);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (top) {
                    ImGui::TextColored(hi, "> $%04X", a16);
                } else {
                    ImGui::Text("  $%04X", a16);
                }
                ImGui::TableSetColumnIndex(1);
                if (top) {
                    ImGui::TextColored(hi, "%02X", byte);
                } else {
                    ImGui::Text("%02X", byte);
                }
                ImGui::TableSetColumnIndex(2);
                if (top) {
                    ImGui::TextColored(hi, "%c", ascii_of(byte));
                } else {
                    ImGui::Text("%c", ascii_of(byte));
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::PopID();
}

// ----------------------------------------------------------------------------
// Watch list — user-managed address watches (address-based; symbolic evaluation
// is the DAP server's job, out of scope here).
// ----------------------------------------------------------------------------
struct WatchEntry {
    uint16_t addr = 0x0000;
    int      len  = 1;   // bytes to show, 1..16
    int      bank = -1;  // x16 RAM/ROM bank, -1 == current
};

void draw_watch()
{
    if (!ImGui::CollapsingHeader("Watch", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::PushID("watch");

    static std::vector<WatchEntry> entries;

    if (ImGui::Button("+ Add")) {
        entries.push_back(WatchEntry());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("address-based watches (bank -1 = current)");

    if (ImGui::BeginTable("watch_tbl", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Addr");
        ImGui::TableSetupColumn("Len");
        ImGui::TableSetupColumn("Bank");
        ImGui::TableSetupColumn("Hex");
        ImGui::TableSetupColumn("Dec / ASCII");
        ImGui::TableHeadersRow();

        int remove = -1;
        for (int i = 0; i < (int)entries.size(); i++) {
            ImGui::PushID(i);
            WatchEntry &w = entries[i];

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::SmallButton("x")) {
                remove = i;
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(hex_width(4));
            ImGui::InputScalar("##addr", ImGuiDataType_U16, &w.addr, nullptr, nullptr,
                               "%04X", kHexFlags);

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(hex_width(3));
            ImGui::InputInt("##len", &w.len, 0);
            if (w.len < 1) w.len = 1;
            if (w.len > 16) w.len = 16;

            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(hex_width(3));
            ImGui::InputInt("##bank", &w.bank, 0);
            if (w.bank < -1) w.bank = -1;
            if (w.bank > 255) w.bank = 255;

            uint8_t bytes[16];
            for (int j = 0; j < w.len; j++) {
                bytes[j] = debug_ui_read6502((uint16_t)(w.addr + j), 0, (int16_t)w.bank);
            }

            ImGui::TableSetColumnIndex(4);
            char hexbuf[16 * 3 + 1];
            int  hp = 0;
            for (int j = 0; j < w.len; j++) {
                hp += snprintf(hexbuf + hp, sizeof(hexbuf) - hp, "%02X ", bytes[j]);
            }
            ImGui::TextUnformatted(hexbuf);
            {
                uint32_t hv = 0;
                int      hb = w.len < 4 ? w.len : 4;
                for (int j = 0; j < hb; j++)
                    hv |= (uint32_t)bytes[j] << (8 * j); // little-endian
                dbgui_hover_value_tooltip("Watch value (little-endian)", hv, hb);
            }

            ImGui::TableSetColumnIndex(5);
            uint32_t val = 0;
            for (int j = 0; j < w.len && j < 4; j++) {
                val |= (uint32_t)bytes[j] << (8 * j); // little-endian
            }
            char abuf[17];
            for (int j = 0; j < w.len; j++) {
                abuf[j] = ascii_of(bytes[j]);
            }
            abuf[w.len] = '\0';
            ImGui::Text("%u  \"%s\"", val, abuf);

            ImGui::PopID();
        }
        ImGui::EndTable();

        if (remove >= 0) {
            entries.erase(entries.begin() + remove);
        }
    }

    ImGui::PopID();
}

void
cpu_panel_render(bool *p_open)
{
    if (ImGui::Begin("CPU", p_open)) {
        draw_registers();
        draw_flags();
        draw_stack();
        draw_watch();
    }
    ImGui::End();
}

} // namespace

static DebugPanelRegistration s_reg("CPU", cpu_panel_render, true);
