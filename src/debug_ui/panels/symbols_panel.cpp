// Commander X16 Emulator — ImGui "Symbols" panel.
//
// A filterable, sortable list of the address labels parsed from the loaded cc65
// .dbg info (dbg_info's `lab` records). Each row shows the label's address and
// its live byte + 16-bit word value. Double-click a row to jump the Disassembly
// and Memory views there (via the cross-panel goto service); right-click for
// set-breakpoint / run-to. This is the "where is X / what's in X right now" view.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"  // dbg_info_symbol_*, debug_ui_read6502, DEBUG*, request_goto
#include "debug_ui_widgets.h" // dbgui_value_lines

#include <stdint.h>
#include <string.h>
#include <cctype>
#include <vector>
#include <algorithm>

namespace {

char s_filter[64] = "";

// Case-insensitive substring test.
bool
contains_ci(const char *hay, const char *needle)
{
    if (!needle[0])
        return true;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

// Case-insensitive strcmp (portable; the CRT one differs by platform).
int
sym_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        if (!ca)
            return 0;
    }
}

// Toggle a breakpoint at a symbol's address (CPU program bank 0).
void
sym_toggle_bp(uint16_t addr)
{
    // Resolve the bank first: it keys the remove as well as the add.
    int x16 = (addr >= 0xA000)
                  ? (addr < 0xC000 ? (int)memory_get_ram_bank() : (int)memory_get_rom_bank())
                  : -1;
    if (!DEBUGRemoveBreakPoint((int)addr, 0, x16)) {
        struct breakpoint bp = {};
        bp.pc      = (int)addr;
        bp.bank    = 0;
        bp.x16Bank = x16;
        DEBUGAddBreakPoint(bp);
    }
}

struct SymRow {
    const char *name;
    uint16_t    addr;
};

void
symbols_panel_render(bool *p_open)
{
    if (!ImGui::Begin("Symbols", p_open)) {
        dbgui_window_end();
        return;
    }
    dbgui_window_zoom("symbols");

    const ImVec4 orange(1.0f, 0.78f, 0.35f, 1.0f);

    if (!dbg_info_is_loaded() || dbg_info_symbol_count() == 0) {
        ImGui::TextColored(orange, "No symbols loaded.");
        ImGui::Spacing();
        ImGui::TextWrapped("Symbols come from cc65 .dbg address labels. Start with "
                           "-dbgfile <prog.dbg> (and -srcpath if needed), or LOAD a "
                           "program whose matching .dbg auto-loads.");
        dbgui_window_end();
        return;
    }

    // Toolbar: name filter.
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##symfilter", "filter by name...", s_filter, sizeof s_filter);
    ImGui::SameLine();
    ImGui::TextDisabled("%d symbols", dbg_info_symbol_count());
    ImGui::SameLine();
    ImGui::TextDisabled("(double-click = go to; right-click for more)");

    // Materialize the filtered rows (symbol counts are modest; rebuild per frame).
    static std::vector<SymRow> rows;
    rows.clear();
    int n = dbg_info_symbol_count();
    for (int i = 0; i < n; i++) {
        const char *nm = nullptr;
        uint16_t    a;
        if (debug_ui_symbol_at(i, &nm, &a) && nm && contains_ci(nm, s_filter))
            rows.push_back({nm, a});
    }

    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("symbols", 4, tflags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Byte", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Word", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        // Apply the requested sort (default: by name, ascending).
        if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs()) {
            if (ss->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs &s = ss->Specs[0];
                bool asc = s.SortDirection != ImGuiSortDirection_Descending;
                if (s.ColumnIndex == 0) {
                    std::sort(rows.begin(), rows.end(), [asc](const SymRow &a, const SymRow &b) {
                        int c = sym_stricmp(a.name, b.name);
                        return asc ? c < 0 : c > 0;
                    });
                } else {
                    std::sort(rows.begin(), rows.end(), [asc](const SymRow &a, const SymRow &b) {
                        return asc ? a.addr < b.addr : a.addr > b.addr;
                    });
                }
            }
        }

        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const SymRow &r  = rows[i];
                uint8_t       b  = debug_ui_read6502(r.addr, 0, DEBUG_UI_CURRENT_BANK);
                uint8_t       b2 = debug_ui_read6502((uint16_t)(r.addr + 1), 0, DEBUG_UI_CURRENT_BANK);
                uint16_t      w  = (uint16_t)(b | (b2 << 8));

                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(r.name, false,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    debug_ui_request_goto(r.addr, 0);
                }
                if (ImGui::BeginPopupContextItem("symctx")) {
                    if (ImGui::MenuItem("Go to (disasm + memory)"))
                        debug_ui_request_goto(r.addr, 0);
                    if (ImGui::MenuItem("Toggle breakpoint"))
                        sym_toggle_bp(r.addr);
                    ImGui::BeginDisabled(!DEBUGIsPaused());
                    if (ImGui::MenuItem("Run to here"))
                        DEBUGRunTo(r.addr, 0, DEBUG_OWNER_UI);
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("$%04X", r.addr);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("$%02X", b);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("$%04X", w);

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    dbgui_window_end();
}

DebugPanelRegistration s_reg("Symbols", symbols_panel_render, true);

} // namespace
