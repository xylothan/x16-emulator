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
#include "debug_ui_settings.h" // allow_breaking_changes

extern "C" {
#include "disasm.h" // decode whatever the native IRQ vector points at
}

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

// ----------------------------------------------------------------------------
// Native-mode interrupt vector probe.
//
// interrupt6502() picks the vector base from E: $FFF0 in emulation, $FFE0 in
// native (+ the vector number, so IRQ is $FFFE vs $FFEE). A ROM written for a
// 6502-class machine has no reason to populate the native slots, and the stock
// X16 ROM does not - $FFEE just contains whatever code bytes live at that
// offset. Clearing E and resuming therefore runs off into garbage at the next
// IRQ, which is 60 Hz here.
//
// Rather than assert that from memory, read it live and show the user what is
// actually there, disassembled, so the warning is about *their* ROM.
// ----------------------------------------------------------------------------
struct VectorProbe {
    uint16_t native_irq;      // word at $FFEE
    uint16_t emu_irq;         // word at $FFFE
    char     native_disasm[64]; // first instruction at native_irq
    bool     looks_absent;    // heuristic: native slot is not a real vector
};

uint16_t read_word(uint16_t addr)
{
    return (uint16_t)debug_ui_read6502(addr, 0, -1) |
           ((uint16_t)debug_ui_read6502((uint16_t)(addr + 1), 0, -1) << 8);
}

VectorProbe probe_vectors()
{
    VectorProbe p{};
    p.native_irq = read_word(0xFFEE);
    p.emu_irq    = read_word(0xFFFE);

    p.native_disasm[0] = '\0';
    // eff_addr is dereferenced unconditionally by disasm(), so it must be real.
    int32_t eff = -1;
    disasm(p.native_irq, 0, nullptr, p.native_disasm, sizeof p.native_disasm, -1,
           (uint8_t)(FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH), &eff);

    // Heuristic, and labelled as one in the UI. On a machine whose ROM really
    // handles native interrupts the two vectors would be set up alike - the X16
    // points its emulation IRQ at a RAM vector ($038B). A native slot pointing
    // into ROM while the emulation one points at RAM means the native slot was
    // never written and we are just reading ROM code.
    const bool emu_in_ram    = p.emu_irq < 0xC000;
    const bool native_in_rom = p.native_irq >= 0xC000;
    p.looks_absent = (emu_in_ram && native_in_rom) || (p.native_irq == 0x0000) ||
                     (p.native_irq == 0xFFFF);
    return p;
}

// Switch the emulation flag the way XCE does, so the machine is left in a state
// the core agrees with. Poking regs.e alone is not enough: while E is set the
// CPU force-sets M and X on every instruction, and emulation mode also pins the
// stack to page 1 and the index high bytes to zero.
void set_emulation_flag(bool emulation)
{
    if (!regs.is65c816 || (regs.e != 0) == emulation) {
        return;
    }
    regs.e = emulation ? 1 : 0;
    if (emulation) {
        regs.status |= FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH; // 8-bit A and X/Y
        regs.xh = 0;
        regs.yh = 0;
        regs.sp = (uint16_t)(0x0100 | (regs.sp & 0x00FF));   // stack lives in page 1
    }
    // Leaving emulation deliberately leaves M/X set: real hardware does the
    // same, so A/X/Y stay 8-bit until the program (or the user) clears them.
}

// Changing E out from under a running program is a genuinely destructive poke —
// it is not just a display mode. Emulation and native disagree about the
// interrupt vectors ($FFFE vs $FFEE), about how many bytes an interrupt pushes
// (native also pushes PB, so a mismatched RTI returns to a garbage address),
// about whether the stack is confined to page 1, and — once M is cleared —
// about instruction lengths. So: only offer it while the machine is paused, and
// make the user confirm the first time.
bool s_emu_confirm_open   = false;  // a confirmation is queued for this frame
bool s_emu_confirm_target = false;  // value the user asked for
bool s_emu_skip_confirm   = false;  // "don't ask again" for this session

void draw_emulation_toggle(const char *label)
{
    if (!regs.is65c816) {
        return;
    }
    const bool paused   = DEBUGIsPaused();
    const bool unlocked = debug_ui_settings().allow_breaking_changes;
    const bool editable = paused && unlocked;

    bool e = regs.e != 0;
    ImGui::BeginDisabled(!editable);
    if (ImGui::Checkbox(label, &e)) {
        if (s_emu_skip_confirm) {
            set_emulation_flag(e);
        } else {
            s_emu_confirm_open   = true;
            s_emu_confirm_target = e;
        }
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (editable) {
            ImGui::SetTooltip("E - Emulation flag\n"
                              "1 = 6502 emulation mode; 0 = 65C816 native.\n\n"
                              "Applies the same state changes as XCE. Editing is unlocked;\n"
                              "see the confirmation for what this will do to the ROM.");
        } else if (!unlocked) {
            ImGui::SetTooltip("E - Emulation flag (read-only)\n"
                              "1 = 6502 emulation mode; 0 = 65C816 native.\n\n"
                              "The running program owns this flag. Changing it moves interrupts\n"
                              "from $FFFE to $FFEE, and a 6502-era ROM has no handler there, so\n"
                              "the machine crashes at the next IRQ.\n\n"
                              "Enable System > Settings > Safety > \"Allow breaking\n"
                              "modifications\" if you are hand-driving the CPU.");
        } else {
            ImGui::SetTooltip("E - Emulation flag (pause to edit)\n"
                              "Switching CPU mode mid-stream breaks the running program's\n"
                              "stack frames and interrupt vectors.");
        }
    }
}

// Rendered once per frame from cpu_panel_render(), outside the PushID scopes of
// the two places that offer the toggle.
void draw_emulation_confirm_popup()
{
    static const char *kTitle = "Switch CPU mode?";

    if (s_emu_confirm_open) {
        ImGui::OpenPopup(kTitle);
        s_emu_confirm_open = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const bool to_emulation = s_emu_confirm_target;
    ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
                       "Switch to %s mode?", to_emulation ? "6502 emulation" : "65C816 native");
    ImGui::Separator();

    // Read the actual vectors out of the machine in front of the user, rather
    // than asserting what a ROM "usually" does.
    const VectorProbe vp = probe_vectors();
    if (!to_emulation) {
        ImGui::TextUnformatted("Interrupts vector by mode. On this machine, right now:");
        ImGui::Spacing();
        if (ImGui::BeginTable("vec_tbl", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Mode");
            ImGui::TableSetupColumn("IRQ vector");
            ImGui::TableSetupColumn("Points to");
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Emulation (now)");
            ImGui::TableNextColumn(); ImGui::TextUnformatted("$FFFE");
            ImGui::TableNextColumn(); ImGui::Text("$%04X", vp.emu_irq);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.0f), "Native (after)");
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.0f), "$FFEE");
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.0f), "$%04X  %s",
                               vp.native_irq, vp.native_disasm);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (vp.looks_absent) {
            ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.0f),
                               "This ROM does not appear to provide a native IRQ handler.");
            ImGui::TextUnformatted(
                "$FFEE holds ROM code, not a vector, so the next IRQ - 60 times a\n"
                "second - will execute whatever is at the address above. Expect the\n"
                "machine to crash to the monitor almost immediately after resuming.");
            ImGui::TextDisabled("(Judged by the emulation vector pointing at RAM while the native\n"
                                "one points into ROM. Check the addresses yourself above.)");
        } else {
            ImGui::TextUnformatted("The native vector looks populated, but verify it is really a\n"
                                   "handler before resuming.");
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Also note:");
    } else {
        ImGui::TextUnformatted(
            "This changes the CPU out from under the running program. Unless it was\n"
            "about to make the same switch itself, expect it to break:");
    }
    ImGui::Bullet();
    ImGui::TextUnformatted("Native interrupts also push the program bank, so an RTI that\n"
                           "doesn't match its entry mode returns to a garbage address.");
    ImGui::Bullet();
    ImGui::TextUnformatted("Emulation confines the stack to page 1; native does not.");
    ImGui::Bullet();
    ImGui::TextUnformatted("In native, clearing M changes instruction lengths, so the PC\n"
                           "stream desyncs from the code that was assembled for 8-bit.");
    ImGui::Separator();
    ImGui::TextDisabled("Fine for experiments and for driving the CPU by hand; not something\n"
                        "a live program will survive.");
    ImGui::Spacing();
    ImGui::Checkbox("Don't ask again this session", &s_emu_skip_confirm);
    ImGui::Spacing();

    if (ImGui::Button("Switch anyway", ImVec2(130, 0))) {
        set_emulation_flag(to_emulation);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        s_emu_skip_confirm = false;  // cancelling shouldn't arm the skip
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Direct-page base + offset, matching the CPU's direct_page_add() (support.h).
// The X16 r0-r15 pseudo-registers live at D+$02..D+$21; D is 0 under the KERNAL
// ABI but can be non-zero in 65C816 native (GS) code, so honor it.
uint16_t dp_add(uint16_t offset)
{
    if (regs.e && (regs.dp & 0x00FF) == 0)
        return (uint16_t)((regs.dp & 0xFF00) | (uint8_t)((uint8_t)(regs.dp & 0xFF) + (offset & 0xFF)));
    return (uint16_t)(regs.dp + offset);
}

char ascii_of(uint8_t b) { return (b >= 0x20 && b < 0x7f) ? (char)b : '.'; }

// Value of a single hex digit, or -1 if the character isn't one.
int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

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
    // Everything about *why* the register set looks the way it does lives here,
    // rather than as a row of grey annotations next to the fields.
    if (is816) {
        ImGui::SetItemTooltip(
            "M=%d  X=%d  E=%d\n\n"
            "The register set follows the CPU mode.\n"
            "%s\n\n"
            "M (P bit 5) picks the accumulator width, X (P bit 4) the index\n"
            "width; 1 = 8-bit. DP, DB and K are live in both modes.",
            (regs.status >> 5) & 1, (regs.status >> 4) & 1, regs.e ? 1 : 0,
            regs.e ? "Emulation mode re-asserts M and X before every instruction, so\n"
                     "A/X/Y are 8-bit and cannot be widened while E is set. The program\n"
                     "leaves emulation mode with XCE."
                   : "Native mode does not widen the registers by itself - exactly as on\n"
                     "hardware, the program selects the widths with REP/SEP.");
    } else {
        ImGui::SetItemTooltip("A/X/Y are 8-bit. The 65C816-only registers (C, B, DP, DB, K, E)\n"
                              "do not exist on the 65C02 and are not shown.");
    }
    ImGui::Separator();

    reg_u16("PC##rPC", &regs.pc, "PC — Program Counter\nAddress of the next instruction to execute.");
    // SP alongside PC rather than on its own row: it is one field and the two
    // are read together.
    ImGui::SameLine();
    {
        // regs.sp is the full 16-bit stack address in every mode - push8() does
        // write6502(regs.sp, ...) directly - and incsp()/decsp() only ever touch
        // the low byte while E is set. So outside 65C816 native the high byte
        // MUST stay $01, or an edit here would quietly move the stack into zero
        // page and the CPU would keep it there.
        uint16_t sp_disp = (uint16_t)(regs.sp | (is816 ? 0 : 0x0100));
        if (reg_u16("SP##rSP", &sp_disp,
                    native ? "SP - Stack Pointer (16-bit)\n65C816 native: the stack can live anywhere."
                           : "SP - Stack Pointer\nConfined to page 1 ($01xx): pushes and pulls wrap inside\n"
                             "the page. The high byte is held at $01 for you.")) {
            regs.sp = native ? sp_disp : (uint16_t)(0x0100 | (sp_disp & 0x00FF));
        }
    }

    // Accumulator, ordered A B C: the 8-bit accumulator, its hidden high byte,
    // then the two combined. All three are the same storage (LOW_HIGH_UNION), so
    // none of them is ever stale or read-only.
    //
    // C is deliberately shown in emulation mode too. 16-bit *arithmetic* needs
    // M=0 and so is native-only, but XBA, TCD, TDC, TCS and TSC all move the
    // full 16 bits with no M or E check (verified in instructions.h), so C is
    // live state in emulation as well.
    if (is816) {
        reg_u8("A##rA", &regs.a,
               "A - accumulator, low 8 bits of C.\n"
               "The whole accumulator when M selects 8-bit.");
        ImGui::SameLine();
        reg_u8("B##rB", &regs.b,
               "B - accumulator high byte.\n"
               "XBA swaps it with A - which works in emulation mode too, so B is\n"
               "real state whatever the accumulator width is.");
        ImGui::SameLine();
        char cdesc[288];
        snprintf(cdesc, sizeof cdesc,
                 "C - the full 16-bit accumulator, B:A = $%02X%02X\n\n"
                 "%s\n\n"
                 "Not native-only: XBA, TCD, TDC, TCS and TSC move all 16 bits\n"
                 "regardless of M, and work in emulation mode.",
                 regs.b, regs.a,
                 acc_is16() ? "M selects 16-bit, so C is the arithmetic accumulator."
                            : "16-bit arithmetic (LDA, ADC, ...) needs M=0, which needs\n"
                              "native mode, so right now arithmetic uses A alone.");
        reg_u16("C##rC", &regs.c, cdesc);
    } else {
        reg_u8("A##rA", &regs.a, "A - accumulator (8-bit)");
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

    // Width is visible from the M/X checkboxes and explained on the Mode line, so
    // don't repeat it as a grey annotation. The one thing that needs a control is
    // the manual widen, and only when it is both possible and unlocked.
    if (is816 && native && (!acc_is16() || !idx_is16()) &&
        debug_ui_settings().allow_breaking_changes) {
        if (ImGui::SmallButton("Widen now (REP #$30)")) {
            regs.status &= (uint8_t)~(FLAG_MEMORY_WIDTH | FLAG_INDEX_WIDTH);
        }
        ImGui::SetItemTooltip(
            "Clear M and X, the same thing REP #$30 does, to give 16-bit\n"
            "A/X/Y. Real hardware only widens when the program asks;\n"
            "this is a manual poke, so the running code may not expect it.\n\n"
            "It also only sticks while E stays clear: setting E re-asserts\n"
            "M and X before every instruction.");
    }

    // 65C816-only registers appear only in 65C816 modes (not shown at all on the
    // 65C02). DP/DB/K/E all exist in emulation as well as native: emulation
    // narrows the register widths, it does not switch these off.
    if (is816) {
        ImGui::Separator();
        // Direct Page is live in BOTH modes - direct_page_add() reads regs.dp
        // either way. Emulation only changes how the offset wraps. Hiding it in
        // emulation would hide something that is actively moving the X16's
        // virtual registers R0-R15, which are direct-page relative.
        reg_u16("DP##rDP", &regs.dp,
                native ? "DP - Direct Page register\nBase of the direct (zero) page."
                       : "DP - Direct Page register\nBase of the direct (zero) page. Still live in emulation\n"
                         "mode: when its low byte is 0 the offset wraps inside the page\n"
                         "(6502 behaviour), otherwise it adds normally.");
        ImGui::SameLine();
        reg_u8("DB##rDB", &regs.db,
               "DB - Data Bank register\nHigh byte (bank) for data memory accesses.");
        ImGui::SameLine();
        reg_u8("K##rK", &regs.k,
               "K - Program Bank register\nHigh byte (bank) of the 24-bit program counter.");
    }

    ImGui::PopID();
}
// ----------------------------------------------------------------------------
// Virtual registers R0-R15 — the X16's sixteen 16-bit pseudo-registers, a
// software convention shadowed in zero page ($02-$21, little-endian) and used
// throughout the KERNAL / VERA / GRAPH APIs (and by cc65). Shown like CPU
// registers so they can be watched and edited live as 16-bit values.
// ----------------------------------------------------------------------------
void draw_vregs()
{
    if (!ImGui::CollapsingHeader("Virtual Regs (R0-R15)")) {
        return;
    }
    ImGui::PushID("vregs");
    ImGui::TextDisabled("X16 16-bit pseudo-registers (direct-page +$02-$21, little-endian)");

    for (int n = 0; n < 16; n++) {
        uint16_t addr = dp_add((uint16_t)(0x02 + 2 * n));
        uint8_t  lo   = debug_ui_read6502(addr, 0, DEBUG_UI_CURRENT_BANK);
        uint8_t  hi   = debug_ui_read6502((uint16_t)(addr + 1), 0, DEBUG_UI_CURRENT_BANK);
        uint16_t val  = (uint16_t)(lo | (hi << 8));

        char label[16];
        snprintf(label, sizeof label, "R%-2d##vr%d", n, n);
        ImGui::SetNextItemWidth(hex_width(4));
        if (ImGui::InputScalar(label, ImGuiDataType_U16, &val, nullptr, nullptr, "%04X", kHexFlags)) {
            debug_ui_write6502(addr, (uint8_t)(val & 0xFF), 0, DEBUG_UI_CURRENT_BANK);
            debug_ui_write6502((uint16_t)(addr + 1), (uint8_t)(val >> 8), 0, DEBUG_UI_CURRENT_BANK);
        }
        char desc[112];
        snprintf(desc, sizeof desc,
                 "R%d - X16 virtual register\n@ $%04X/$%04X (direct-page relative, little-endian)",
                 n, addr, (uint16_t)(addr + 1));
        dbgui_hover_value_tooltip(desc, val, 2);

        if (n % 2 == 0) {
            ImGui::SameLine(0.0f, 24.0f); // two registers per row
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
    {
        char bits[48];
        dbgui_format_binary(bits, sizeof bits, regs.status, 8);
        ImGui::SetItemTooltip(
            "P - Processor status register\n"
            "$%02X = %s\n"
            "Bit 7..0: %s\n\n"
            "Each bit is editable as a checkbox below. On the 65C816 bits 5 and 4\n"
            "are the M and X width selects; on the 65C02 they are the unused and\n"
            "Break bits, which is why the layout changes with the CPU.",
            regs.status, bits,
            regs.is65c816 ? "N V M X D I Z C" : "N V - B D I Z C");
    }

    flag_checkbox("N##fN", FLAG_SIGN, "N — Negative",
                  "Set when bit 7 of the result is set (result looks negative).");
    ImGui::SameLine();
    flag_checkbox("V##fV", FLAG_OVERFLOW, "V — Overflow",
                  "Set on signed arithmetic overflow (ADC/SBC), or by BIT/CLV.");
    ImGui::SameLine();
    if (regs.is65c816) {
        // While E is set the CPU re-forces M and X to 1 before every
        // instruction, so editing them here would silently revert. Disable them
        // instead of pretending the edit took.
        const bool widths_locked = regs.e != 0;
        ImGui::BeginDisabled(widths_locked);
        flag_checkbox("M##fM", FLAG_MEMORY_WIDTH, "M — Memory/Accumulator width",
                      "65C816 native only. 0 = 16-bit accumulator/memory, 1 = 8-bit.");
        ImGui::SameLine();
        flag_checkbox("X##fX", FLAG_INDEX_WIDTH, "X — Index register width",
                      "65C816 native only. 0 = 16-bit X/Y, 1 = 8-bit. Forced 1 in emulation.");
        ImGui::EndDisabled();
        if (widths_locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Forced to 1 while E (emulation) is set — the CPU rewrites\n"
                              "both flags before every instruction. Clear E to change them.");
        }
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
        draw_emulation_toggle("E  (Emulation)##fE");
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
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  DBGUI_TABLE_FLAGS_RESIZABLE)) {
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
// Interrupts — whether we are inside a handler, and how far away the next
// interrupt is. Stepping through code gives no clue that an interrupt is about
// to fire, so surface the prediction next to the registers.
// ----------------------------------------------------------------------------
void draw_interrupts()
{
    if (!ImGui::CollapsingHeader("Interrupts", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::PushID("irq");

    if (cpu_in_interrupt()) {
        const int  vec  = cpu_irq_last_vector();
        const char *what = vec == 0x0A ? "NMI" : (vec == 0x06 ? "BRK" : (vec == 0x04 ? "COP" : "IRQ"));
        ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.45f, 1.0f),
                           "In %s handler (depth %d)", what, cpu_irq_depth());
        ImGui::SameLine();
        ImGui::TextDisabled("returns to $%04X", (unsigned)cpu_irq_return_pc());
    } else {
        ImGui::TextDisabled("Not in an interrupt");
    }

    // VERA interrupt state + prediction. IEN/ISR tell us what is armed and
    // pending; video_next_irq() converts the scan position into a countdown.
    uint8_t  ien = 0, isr = 0;
    uint16_t irqline = 0;
    video_get_irq_state(&ien, &isr, &irqline);

    ImGui::Text("VERA IEN $%02X", ien);
    ImGui::SameLine();
    ImGui::TextDisabled("[%s%s%s%s]",
                        (ien & 1) ? "VSYNC " : "", (ien & 2) ? "LINE " : "",
                        (ien & 4) ? "SPRCOL " : "", (ien & 8) ? "AFLOW" : "");
    ImGui::SameLine();
    ImGui::Text("ISR $%02X", isr);
    if (ien & 2) {
        ImGui::SameLine();
        ImGui::TextDisabled("line=%u", (unsigned)irqline);
    }

    uint32_t cycles = 0;
    uint8_t  source = 0;
    if (video_next_irq((float)MHZ, &cycles, &source)) {
        // Convert to an instruction estimate too — when stepping, "how many
        // more steps" is the useful unit. ~3.5 cycles/instruction is a decent
        // average for 6502-family code; label it as approximate.
        const unsigned insns = (unsigned)(cycles / 4);
        ImGui::Text("Next: %s in %u cyc", source == 1 ? "VSYNC" : "LINE", (unsigned)cycles);
        ImGui::SameLine();
        ImGui::TextDisabled("(~%u instructions)", insns);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Cycles until the next enabled VERA interrupt, from the\n"
                              "current raster position. The instruction figure is a rough\n"
                              "estimate at ~4 cycles each.");
        }
    } else if (regs.status & FLAG_INTERRUPT) {
        ImGui::TextDisabled("No VERA interrupt armed (and I flag masks IRQs)");
    } else {
        ImGui::TextDisabled("No VERA interrupt armed");
    }

    if (regs.status & FLAG_INTERRUPT) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f), "I");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The I flag is set: maskable IRQs are blocked right now.");
        }
    }

    ImGui::TextDisabled("%u interrupts taken", (unsigned)cpu_irq_count());

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

// The list outlives the collapsing header so a request that arrives while the
// header is closed is still honoured (and pops the header open to show it).
std::vector<WatchEntry> g_watch_entries;

void draw_watch()
{
    // "Add to watch" from the memory view's context menu, possibly a range.
    bool added = false;
    {
        uint16_t waddr = 0;
        int16_t  wbank = -1;
        uint16_t wlen  = 1;
        if (debug_ui_take_watch_request(&waddr, &wbank, &wlen)) {
            // A watch row shows at most 16 bytes, so a longer range becomes
            // several consecutive rows rather than being silently truncated.
            uint16_t left = wlen ? wlen : 1;
            uint16_t at   = waddr;
            while (left > 0 && g_watch_entries.size() < 64) {
                WatchEntry w;
                w.addr = at;
                w.len  = left > 16 ? 16 : (int)left;
                w.bank = wbank;
                g_watch_entries.push_back(w);
                at   = (uint16_t)(at + w.len);
                left = (uint16_t)(left - w.len);
            }
            added = true;
        }
    }
    if (added) {
        ImGui::SetNextItemOpen(true); // reveal what was just added
    }

    if (!ImGui::CollapsingHeader("Watch", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::PushID("watch");

    std::vector<WatchEntry> &entries = g_watch_entries;

    if (ImGui::Button("+ Add")) {
        entries.push_back(WatchEntry());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("address-based watches (bank -1 = current)");

    if (ImGui::BeginTable("watch_tbl", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingFixedFit | DBGUI_TABLE_FLAGS_RESIZABLE)) {
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
            // Editable: type new hex bytes and the whole span is written back
            // through the normal write path (I/O side effects, watchpoints),
            // honouring this row's bank. Only commit on Enter/focus-loss so
            // half-typed input is never written.
            char hexbuf[16 * 3 + 1];
            int  hp = 0;
            for (int j = 0; j < w.len; j++) {
                hp += snprintf(hexbuf + hp, sizeof(hexbuf) - hp, "%02X ", bytes[j]);
            }
            if (hp > 0 && hexbuf[hp - 1] == ' ') {
                hexbuf[hp - 1] = '\0'; // drop the trailing separator while editing
            }
            ImGui::SetNextItemWidth(hex_width(w.len * 3 + 1));
            if (ImGui::InputText("##hex", hexbuf, sizeof(hexbuf),
                                 kHexFlags | ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll)) {
                // Accept "AA BB CC", "AABBCC" or a short prefix; missing bytes
                // are left untouched.
                const char *p = hexbuf;
                for (int j = 0; j < w.len; j++) {
                    while (*p == ' ' || *p == '\t')
                        p++;
                    if (!*p)
                        break;
                    int hi = hex_digit(*p++);
                    if (hi < 0)
                        break;
                    int lo = hex_digit(*p);
                    uint8_t v;
                    if (lo < 0) {
                        v = (uint8_t)hi;          // single digit = low nibble
                    } else {
                        v = (uint8_t)((hi << 4) | lo);
                        p++;
                    }
                    debug_ui_write6502((uint16_t)(w.addr + j), v, 0, (int16_t)w.bank);
                }
            }
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
                uint32_t hv = 0;
                int      hb = w.len < 4 ? w.len : 4;
                for (int j = 0; j < hb; j++)
                    hv |= (uint32_t)bytes[j] << (8 * j); // little-endian
                dbgui_hover_value_tooltip("Watch value (little-endian) - editable", hv, hb);
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
        dbgui_window_zoom("cpu");
        draw_registers();
        draw_vregs();
        draw_flags();
        draw_interrupts();
        draw_stack();
        draw_watch();
        draw_emulation_confirm_popup();
    }
    dbgui_window_end();
}

} // namespace

static DebugPanelRegistration s_reg("CPU", cpu_panel_render, true);
