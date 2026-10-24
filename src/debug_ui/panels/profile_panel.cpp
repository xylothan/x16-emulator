// Commander X16 Emulator — ImGui debugger: Profile panel.
//
// Start a capture, play, and read where the guest's time went by routine once
// it ends. While the capture runs this panel -- like every other -- is not
// drawn at all (debug_ui_render() goes quiet), so it costs the machine nothing
// while it is being measured; the report is built once, when the capture ends.
// See profile_capture.h and profile_report.h.

#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"  // debug_ui_request_goto
#include "debug_ui_widgets.h" // dbgui_window_zoom/end, dbgui_col_width

#include "../perf_hist.h"
#include "../profile_session.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

int   g_frames     = 600;
float g_seconds    = 0.0f;
bool  g_on_marker  = false;
bool  g_rec_budget = false;
bool  g_rec_vram   = false;
char  g_filter[64] = "";

const ImVec4 COL_DIM(0.60f, 0.60f, 0.60f, 1.0f);
const ImVec4 COL_WAIT(0.55f, 0.75f, 1.00f, 1.0f);
const ImVec4 COL_AMBIG(1.00f, 0.75f, 0.35f, 1.0f);

void
draw_controls(void)
{
	const cap_state_t st = cap_state();
	if (st == CAP_RUNNING || st == CAP_ARMED) {
		if (st == CAP_ARMED)
			ImGui::TextColored(COL_WAIT, "Armed: waiting for the guest to write $F8 to $9FBC.");
		else
			ImGui::Text("Capturing: %u frames, %.1f s", cap_frames(), cap_elapsed_s());
		ImGui::SameLine();
		if (ImGui::Button(st == CAP_ARMED ? "Cancel" : "Stop"))
			cap_stop();
		return;
	}

	ImGui::SetNextItemWidth(dbgui_col_width("", "99999999"));
	ImGui::InputInt("frames", &g_frames, 60, 600);
	if (g_frames < 0)
		g_frames = 0;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Stop after this many emulated frames. 0 = no frame limit.");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_col_width("", "99999.9"));
	ImGui::InputFloat("seconds", &g_seconds, 1.0f, 10.0f, "%.1f");
	if (g_seconds < 0.0f)
		g_seconds = 0.0f;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Stop after this much host time. 0 = no time limit.\n"
		                  "With both limits at 0 the capture runs until Stop.");

	ImGui::Checkbox("Start on guest marker", &g_on_marker);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Arm, and start only when the program writes $F8 to $9FBC;\n"
		                  "$F9 stops it. Brackets exactly the code you want measured.");
	ImGui::SameLine();
	ImGui::Checkbox("Cycle budget", &g_rec_budget);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Also record work vs. waiting per frame (perf_budget).\n"
		                  "Costs the running machine a little more.");
	ImGui::SameLine();
	ImGui::Checkbox("VRAM bandwidth", &g_rec_vram);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Also record VERA's VRAM traffic per scanline.");

	if (ImGui::Button("Start capture")) {
		cap_config_t c = {};
		c.frames    = (uint32_t)g_frames;
		c.seconds   = g_seconds;
		c.on_marker = g_on_marker;
		c.recorders = CAP_REC_HIST | (g_rec_budget ? CAP_REC_BUDGET : 0) |
		              (g_rec_vram ? CAP_REC_VRAM : 0);
		cap_start(&c);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("The debugger goes quiet while it records.");
	if (st == CAP_DONE && cap_end_reason() == CAP_END_FAILED)
		ImGui::TextColored(COL_AMBIG, "The last capture could not arm its recorders.");
}

void
draw_report(void)
{
	const prof_report_t *r = profile_session_report();
	if (!r) {
		ImGui::TextDisabled("No report yet. Start a capture; the report appears when it ends.");
		return;
	}
	const uint64_t total  = r->main_cycles + r->irq_cycles;
	const uint32_t frames = profile_session_report_frames() ? profile_session_report_frames() : 1;
	ImGui::Text("Last capture: %s, %u frames, %.1f s.", cap_end_name(cap_end_reason()), frames,
	            cap_elapsed_s());
	ImGui::TextDisabled("%llu cycles per frame (%.1f%% in interrupts), %d routines/pages, "
	                    "%d named.",
	                    (unsigned long long)(total / frames),
	                    total ? 100.0 * (double)r->irq_cycles / (double)total : 0.0, r->count,
	                    r->named_rows);
	if (r->ambiguous_rows)
		ImGui::TextColored(COL_AMBIG, "%d rows marked * name code whose RAM bank the debug info "
		                              "did not establish.", r->ambiguous_rows);

	ImGui::SetNextItemWidth(dbgui_col_width("", "wwwwwwwwwwwwwwww"));
	ImGui::InputTextWithHint("##proffilter", "filter", g_filter, sizeof g_filter);

	// The filtered view, rebuilt only when the report or the filter changes.
	// A new report can reuse the old one's allocation and row count, so its
	// totals are part of the key too.
	static const prof_row_t *seen_rows = nullptr;
	static int               seen_count = -1;
	static uint64_t          seen_total = 0;
	static char              seen_filter[64] = "\x01";
	static int              *view = nullptr;
	static int               view_n = 0, view_cap = 0;
	if (seen_rows != r->rows || seen_count != r->count || seen_total != total + r->instructions ||
	    strcmp(seen_filter, g_filter) != 0) {
		seen_rows  = r->rows;
		seen_count = r->count;
		seen_total = total + r->instructions;
		snprintf(seen_filter, sizeof seen_filter, "%s", g_filter);
		if (view_cap < r->count) {
			delete[] view;
			view_cap = r->count;
			view     = new int[view_cap > 0 ? view_cap : 1];
		}
		view_n = 0;
		for (int i = 0; i < r->count; i++)
			if (!g_filter[0] || strstr(r->rows[i].name, g_filter))
				view[view_n++] = i;
	}

	if (!ImGui::BeginTable("profrows", 6,
	                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                           ImGuiTableFlags_BordersInnerV | DBGUI_TABLE_FLAGS_RESIZABLE))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Self %", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Self %", "99.99"));
	ImGui::TableSetupColumn("IRQ %", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("IRQ %", "99.99"));
	ImGui::TableSetupColumn("Cycles/frame", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Cycles/frame", "9999999"));
	ImGui::TableSetupColumn("Cyc/ins", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Cyc/ins", "9.99"));
	ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Where", "RAM 255:$BFFF"));
	ImGui::TableHeadersRow();

	ImGuiListClipper clipper;
	clipper.Begin(view_n);
	while (clipper.Step()) {
		for (int v = clipper.DisplayStart; v < clipper.DisplayEnd; v++) {
			const prof_row_t &row = r->rows[view[v]];
			const uint64_t    t   = row.main_cycles + row.irq_cycles;
			const double      cpi = row.instructions ? (double)t / (double)row.instructions : 0.0;
			ImGui::TableNextRow();
			ImGui::PushID(view[v]);
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%.2f", total ? 100.0 * (double)t / (double)total : 0.0);
			ImGui::TableSetColumnIndex(1);
			if (row.irq_cycles)
				ImGui::Text("%.2f", total ? 100.0 * (double)row.irq_cycles / (double)total : 0.0);
			else
				ImGui::TextDisabled("-");
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%llu", (unsigned long long)(t / frames));
			ImGui::TableSetColumnIndex(3);
			// No 6502 instruction is shorter than 2 cycles: near 1 means WAI.
			if (cpi > 0.0 && cpi < 1.5)
				ImGui::TextColored(COL_WAIT, "%.2f", cpi);
			else
				ImGui::Text("%.2f", cpi);
			if (cpi > 0.0 && cpi < 1.5 && ImGui::IsItemHovered())
				ImGui::SetTooltip("Under 2 cycles per instruction: the CPU sat in WAI here,\n"
				                  "ticking one cycle per step. Waiting, not working.");
			ImGui::TableSetColumnIndex(4);
			char label[PROF_NAME_MAX + 4];
			snprintf(label, sizeof label, "%s%s", row.name, row.ambiguous ? " *" : "");
			if (ImGui::Selectable(label, false,
			                      ImGuiSelectableFlags_SpanAllColumns |
			                          ImGuiSelectableFlags_AllowDoubleClick) &&
			    ImGui::IsMouseDoubleClicked(0))
				debug_ui_request_goto(row.addr, 0);
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::Text("%llu main + %llu IRQ cycles, %llu instructions",
				            (unsigned long long)row.main_cycles, (unsigned long long)row.irq_cycles,
				            (unsigned long long)row.instructions);
				if (!row.named)
					ImGui::TextDisabled("No label covers this: grouped by 256-byte page.");
				if (row.ambiguous)
					ImGui::TextColored(COL_AMBIG, "The debug info did not establish this code's "
					                              "RAM bank,\nso the name may belong to another "
					                              "bank's code at this address.");
				ImGui::TextDisabled("Double-click to open in Disassembly%s.",
				                    row.region == PH_REGION_RAM ? " (shows the RAM bank mapped now)"
				                                                : "");
				ImGui::EndTooltip();
			}
			ImGui::TableSetColumnIndex(5);
			if (row.region == PH_REGION_RAM)
				ImGui::TextDisabled("RAM %u:$%04X", (unsigned)row.bank, (unsigned)row.addr);
			else if (row.region == PH_REGION_ROM)
				ImGui::TextDisabled("ROM %u:$%04X", (unsigned)row.bank, (unsigned)row.addr);
			else
				ImGui::TextDisabled("$%04X", (unsigned)row.addr);
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

void
profile_panel_render(bool *p_open)
{
	if (!ImGui::Begin("Profile", p_open)) {
		dbgui_window_end();
		return;
	}
	dbgui_window_zoom("profile");
	draw_controls();
	ImGui::Separator();
	draw_report();
	dbgui_window_end();
}

DebugPanelRegistration s_reg("Profile", profile_panel_render, true);

} // namespace
