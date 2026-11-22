// Commander X16 Emulator — ImGui "Call Stack" panel.
//
// The 6502/65C816 has no frame pointer, so call_stack_unwind() guesses at the
// frames: the current PC, then stack bytes that point just past a JSR ($20).
// Each plausible caller is resolved to a symbol + source line. Click a frame to jump
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
#include "call_stack.h"
#include "dbg_info.h" // dbg_info_addr_to_source_nearest (not in the bridge)
}

namespace {

uint8_t
read_for_unwind(uint16_t address, uint8_t bank)
{
    return debug_ui_read6502(address, bank, DEBUG_UI_CURRENT_BANK);
}

void
callstack_panel_render(bool *p_open)
{
    if (!ImGui::Begin("Call Stack", p_open)) {
        dbgui_window_end();
        return;
    }
    dbgui_window_zoom("callstack");

    const uint8_t bank = regs.k;

    uint16_t  frames[CALL_STACK_MAX];
    const int nf = call_stack_unwind(regs.pc, regs.sp, bank, regs.is65c816 && !regs.e,
                                     read_for_unwind, frames);

    if (!dbg_info_is_loaded() && dbg_info_symbol_count() == 0) {
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
            const uint16_t addr  = frames[i];
            const bool     is_pc = i == 0;
            const uint16_t site  = call_stack_site(is_pc, addr);
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            if (is_pc)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%d", i);
            else
                ImGui::Text("%d", i);

            ImGui::TableSetColumnIndex(1);
            const char *label = nullptr;
            char        buf[128];
            bool        has_src =
                dbg_info_addr_to_source_banked(site, (int)memory_get_ram_bank(), nullptr, nullptr);
            const char *nosrc = has_src ? "" : "  (no src)";

            // Frame 0 is the PC and often sits exactly on a label. Caller frames
            // are return addresses - the instruction after a JSR - which almost
            // never are, so an exact lookup left every caller anonymous: a frame
            // showed its symbol only while it was frame 0, then blanked the
            // moment a deeper call pushed it down. Hence the offset form. The
            // name comes from the JSR, because the instruction after it can
            // already be the next routine.
            dbg_addr_t  label_at = 0;
            if (dbg_info_frame_label(site, (int)memory_get_ram_bank(), &label, &label_at)) {
                if (label_at == addr)
                    snprintf(buf, sizeof buf, "$%04X  %s%s", addr, label, nosrc);
                else
                    snprintf(buf, sizeof buf, "$%04X  %s+%u", addr, label,
                             (unsigned)(uint16_t)(addr - label_at));
            } else {
                snprintf(buf, sizeof buf, "$%04X%s", addr, nosrc);
            }
            if (ImGui::Selectable(buf, is_pc, ImGuiSelectableFlags_SpanAllColumns))
                debug_ui_request_goto(site, bank);

            ImGui::TableSetColumnIndex(2);
            const char *file = nullptr;
            int         line = 0;
            if (dbg_info_addr_to_source_banked(site, (int)memory_get_ram_bank(), &file, &line) && file) {
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
                if (site >= 0xC000)
                    what = "[KERNAL/ROM] no source";
                else if (site >= 0xA000)
                    what = "[banked] no source";
                else if (site < 0x0800)
                    what = "[stub/KERNAL RAM] no source";
                else
                    what = "[NOSRC]";
                ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.78f, 1.0f), "%s", what);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("$%04X is not covered by the loaded .dbg.\n"
                                      "$C000-$FFFF is banked ROM (KERNAL/BASIC);\n"
                                      "$0200-$07FF usually holds KERNAL RAM routines\n"
                                      "such as the JSRFAR far-call trampoline.", site);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    dbgui_window_end();
}

DebugPanelRegistration s_reg("Call Stack", callstack_panel_render, true);

} // namespace
