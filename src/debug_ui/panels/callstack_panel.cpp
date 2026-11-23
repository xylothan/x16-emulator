// Commander X16 Emulator — ImGui "Call Stack" panel.
//
// The 6502/65C816 has no frame pointer, so call_stack_unwind() guesses at the
// frames: the current PC, then stack bytes that point just past a JSR ($20).
// Each plausible caller is resolved to a symbol + source line. Click a frame to jump
// the Disassembly + Memory views there. Heuristic by nature (a stray stack byte
// pair can occasionally look like a return address), so treat deep frames as a
// hint, not gospel — but the JSR-precedes check keeps false positives low.
// The section below the frames shows the selected frame's C variables.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_widgets.h" // regs, debug_ui_read6502, dbg_info_*, request_goto

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "c_locals_host.h"
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
locals_section(int frame)
{
    ImGui::Separator();
    if (!DEBUGIsPaused()) {
        ImGui::TextDisabled("C locals: pause to read frame %d", frame);
        return;
    }
    c_locals_frame_t f;
    if (!c_locals_host_frame(frame, &f) || f.func < 0) {
        ImGui::TextDisabled("C locals: frame %d is not in a C function", frame);
        return;
    }
    ImGui::Text("C locals: frame %d, %s()", frame, f.name);
    if (f.view.state != C_FRAME_KNOWN) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.35f, 1.0f), "%s", c_frame_state_reason(f.view.state));
    }
    if (f.var_count == 0) {
        ImGui::TextDisabled("(no function-level variables)");
        return;
    }
    c_locals_row_t *rows = c_locals_host_rows(&f);
    if (!rows) {
        ImGui::TextDisabled("(variables unavailable)");
        return;
    }
    if (ImGui::BeginTable("clocals", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | DBGUI_TABLE_FLAGS_RESIZABLE)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Name", "variable"));
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Kind", "parameter"));
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Address", "$FFFF"));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < f.var_count; i++) {
            const c_local_t *l = &rows[i].local;
            char             text[C_LOCALS_SHOW_MAX * 3 + 32];
            c_locals_host_text(&f, &rows[i], text, sizeof text);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(rows[i].var.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(c_locals_row_kind(&rows[i]));
            ImGui::TableSetColumnIndex(2);
            if (l->has_address)
                ImGui::Text("$%04X", l->address);
            else
                ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3);
            if (l->status == C_LOCAL_OK)
                ImGui::TextUnformatted(text);
            else
                ImGui::TextDisabled("%s", text);
        }
        ImGui::EndTable();
    }
    free(rows);
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

    static int      selected;
    static uint16_t selected_pc, selected_sp;
    if (regs.pc != selected_pc || regs.sp != selected_sp || selected >= nf) {
        selected    = 0;
        selected_pc = regs.pc;
        selected_sp = regs.sp;
    }

    if (ImGui::BeginTable("callstack", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollY | DBGUI_TABLE_FLAGS_RESIZABLE,
                          ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.5f))) {
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
            if (ImGui::Selectable(buf, i == selected, ImGuiSelectableFlags_SpanAllColumns)) {
                selected = i;
                debug_ui_request_goto(site, bank);
            }

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

    locals_section(selected);

    dbgui_window_end();
}

DebugPanelRegistration s_reg("Call Stack", callstack_panel_render, true);

} // namespace
