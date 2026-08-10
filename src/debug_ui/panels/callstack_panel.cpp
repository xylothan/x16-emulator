// Commander X16 Emulator — ImGui "Call Stack" panel.
//
// The 6502/65C816 has no frame pointer, so this heuristically unwinds the
// hardware stack: frame 0 is the current PC, then it scans the stack for
// candidate return addresses whose preceding instruction is a JSR ($20). Each
// plausible caller is resolved to a symbol + source line. Click a frame to jump
// the Disassembly + Memory views there. Heuristic by nature (a stray stack byte
// pair can occasionally look like a return address), so treat deep frames as a
// hint, not gospel — but the JSR-precedes check keeps false positives low.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_widgets.h" // regs, debug_ui_read6502, dbg_info_*, request_goto

#include <stdint.h>
#include <stdio.h>

extern "C" {
#include "dbg_info.h" // dbg_info_addr_to_source_nearest (not in the bridge)
}

namespace {

struct Frame {
    uint16_t addr;  // frame 0: current PC; callers: return address
    bool     is_pc; // frame 0 marker
};

void
callstack_panel_render(bool *p_open)
{
    if (!ImGui::Begin("Call Stack", p_open)) {
        dbgui_window_end();
        return;
    }
    dbgui_window_zoom("callstack");

    const uint8_t bank   = regs.k;
    const bool    native = regs.is65c816 && !regs.e;

    // Frame 0 = current PC; then heuristically unwind.
    Frame frames[64];
    int   nf     = 0;
    frames[nf++] = {regs.pc, true};

    // Stack scan range. 6502/emulation: page 1 ($0100-$01FF). Native: 16-bit SP,
    // scan a bounded window upward from SP+1.
    uint32_t sp    = regs.sp;
    uint32_t start = native ? (sp + 1) : (0x100 + (sp & 0xFF) + 1);
    uint32_t end   = native ? (sp + 1 + 256) : 0x200;

    for (uint32_t a = start; a + 1 < end && nf < 64;) {
        uint8_t  lo     = debug_ui_read6502((uint16_t)a, 0, DEBUG_UI_CURRENT_BANK);
        uint8_t  hi     = debug_ui_read6502((uint16_t)(a + 1), 0, DEBUG_UI_CURRENT_BANK);
        uint16_t pushed = (uint16_t)(lo | (hi << 8)); // JSR pushes (return-1)
        // A JSR ($20) at pushed-2 means execution resumes at pushed+1.
        uint8_t opc = debug_ui_read6502((uint16_t)(pushed - 2), bank, DEBUG_UI_CURRENT_BANK);
        if (opc == 0x20) {
            frames[nf++] = {(uint16_t)(pushed + 1), false};
            a += 2; // consume the return-address pair
        } else {
            a += 1;
        }
    }

    if (!dbg_info_is_loaded()) {
        ImGui::TextDisabled("(no .dbg loaded — addresses only; load debug info for source/symbols)");
    }
    ImGui::TextDisabled("Heuristic unwind (JSR-based). Click a frame to go there.");

    if (ImGui::BeginTable("callstack", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("#", "99"));
        ImGui::TableSetupColumn("Address / Symbol", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < nf; i++) {
            const Frame &f = frames[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            if (f.is_pc)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%d", i);
            else
                ImGui::Text("%d", i);

            ImGui::TableSetColumnIndex(1);
            const char *label = nullptr;
            char        buf[128];
            bool        has_src =
                dbg_info_addr_to_source_banked(f.addr, (int)memory_get_ram_bank(), nullptr, nullptr);
            const char *nosrc = has_src ? "" : "  (no src)";

            // Frame 0 is the PC and often sits exactly on a label. Caller frames
            // are return addresses - the instruction after a JSR - which almost
            // never are, so an exact lookup left every caller anonymous: a frame
            // showed its symbol only while it was frame 0, then blanked the
            // moment a deeper call pushed it down. Fall back to the label the
            // address falls inside and show the offset from it.
            uint16_t    label_at = 0;
            if (dbg_info_addr_to_label(f.addr, &label) && label) {
                snprintf(buf, sizeof buf, "$%04X  %s%s", f.addr, label, nosrc);
            } else if (has_src && debug_ui_addr_to_label_nearest(f.addr, &label, &label_at) && label) {
                // Only when the address is really covered by the debug info -
                // the nearest label of an uncovered address is meaningless.
                snprintf(buf, sizeof buf, "$%04X  %s+%u", f.addr, label,
                         (unsigned)(f.addr - label_at));
            } else {
                snprintf(buf, sizeof buf, "$%04X%s", f.addr, nosrc);
            }
            if (ImGui::Selectable(buf, f.is_pc, ImGuiSelectableFlags_SpanAllColumns))
                debug_ui_request_goto(f.addr, bank);

            ImGui::TableSetColumnIndex(2);
            const char *file = nullptr;
            int         line = 0;
            if (dbg_info_addr_to_source_banked(f.addr, (int)memory_get_ram_bank(), &file, &line) && file) {
                const char *base = file;
                for (const char *p = file; *p; ++p)
                    if (*p == '/' || *p == '\\')
                        base = p + 1;
                ImGui::Text("%s:%d", base, line);
            } else {
                // No debug info for this frame — say WHY rather than just "-",
                // so a KERNAL call or a low-RAM far-call thunk (e.g. JSRFAR at
                // $02xx) is obvious instead of looking like missing symbols.
                const char *what;
                if (f.addr >= 0xC000)
                    what = "[KERNAL/ROM] no source";
                else if (f.addr >= 0xA000)
                    what = "[banked] no source";
                else if (f.addr < 0x0800)
                    what = "[stub/KERNAL RAM] no source";
                else
                    what = "[NOSRC]";
                ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.78f, 1.0f), "%s", what);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("$%04X is not covered by the loaded .dbg.\n"
                                      "$C000-$FFFF is banked ROM (KERNAL/BASIC);\n"
                                      "$0200-$07FF usually holds KERNAL RAM routines\n"
                                      "such as the JSRFAR far-call trampoline.", f.addr);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    dbgui_window_end();
}

DebugPanelRegistration s_reg("Call Stack", callstack_panel_render, true);

} // namespace
