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

#include <algorithm>
#include <vector>

namespace {

int   g_frames     = 600;
float g_seconds    = 0.0f;
bool  g_on_marker  = false;
bool  g_rec_budget = false;
bool  g_rec_vram   = false;
bool  g_rec_tree   = true;
char  g_filter[64] = "";
char  g_export[260] = "profile";

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
	ImGui::Checkbox("Call tree", &g_rec_tree);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Also record who called what: inclusive time, calls and worst\n"
		                  "call per routine, and the call tree. Costs a little at each\n"
		                  "JSR, RTS and interrupt.");
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
		c.recorders = CAP_REC_HIST | (g_rec_tree ? CAP_REC_TREE : 0) |
		              (g_rec_budget ? CAP_REC_BUDGET : 0) | (g_rec_vram ? CAP_REC_VRAM : 0);
		cap_start(&c);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("The debugger goes quiet while it records.");
	if (st == CAP_DONE && cap_end_reason() == CAP_END_FAILED)
		ImGui::TextColored(COL_AMBIG, "The last capture could not arm its recorders.");
}

void
draw_rows(const prof_report_t *r, uint64_t total, uint32_t frames)
{
	const bool tree = r->node_count > 2;
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
	bool                     rebuilt = false;
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
		rebuilt = true;
	}

	if (!ImGui::BeginTable("profrows", 8,
	                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
	                           ImGuiTableFlags_BordersInnerV | DBGUI_TABLE_FLAGS_RESIZABLE))
		return;
	const ImGuiTableColumnFlags num = ImGuiTableColumnFlags_WidthFixed |
	                                  ImGuiTableColumnFlags_PreferSortDescending;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Self %", num | ImGuiTableColumnFlags_DefaultSort,
	                        dbgui_col_width("Self %", "99.99"));
	ImGui::TableSetupColumn("Incl %", num, dbgui_col_width("Incl %", "100.00"));
	ImGui::TableSetupColumn("IRQ %", num, dbgui_col_width("IRQ %", "99.99"));
	ImGui::TableSetupColumn("Cycles/frame", num | ImGuiTableColumnFlags_NoSort,
	                        dbgui_col_width("Cycles/frame", "9999999"));
	ImGui::TableSetupColumn("Calls", num, dbgui_col_width("Calls", "99999999"));
	ImGui::TableSetupColumn("Cyc/ins", num, dbgui_col_width("Cyc/ins", "9.99"));
	ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Where", "RAM 255:$BFFF"));

	// Sort the view when the user picks a column, or when it was rebuilt.
	if (ImGuiTableSortSpecs *sp = ImGui::TableGetSortSpecs()) {
		if ((sp->SpecsDirty || rebuilt) && sp->SpecsCount > 0) {
			const int  col  = sp->Specs[0].ColumnIndex;
			const bool desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
			std::stable_sort(view, view + view_n, [r, col, desc](int a, int b) {
				const prof_row_t &x = r->rows[a], &y = r->rows[b];
				int c = 0;
				auto cmp = [](double p, double q) { return p < q ? -1 : p > q ? 1 : 0; };
				switch (col) {
					case 0: c = cmp((double)(x.main_cycles + x.irq_cycles),
					                (double)(y.main_cycles + y.irq_cycles)); break;
					case 1: c = cmp((double)x.incl_cycles, (double)y.incl_cycles); break;
					case 2: c = cmp((double)x.irq_cycles, (double)y.irq_cycles); break;
					case 4: c = cmp((double)x.calls, (double)y.calls); break;
					case 5: c = cmp(x.instructions ? (double)(x.main_cycles + x.irq_cycles) / (double)x.instructions : 0.0,
					                y.instructions ? (double)(y.main_cycles + y.irq_cycles) / (double)y.instructions : 0.0);
					        break;
					case 6: c = strcmp(x.name, y.name); break;
					case 7: c = x.region != y.region ? x.region - y.region
					          : x.bank != y.bank     ? x.bank - y.bank
					                                 : x.addr - y.addr;
					        break;
					default: c = a - b; break;
				}
				if (c == 0)
					return a < b; // the report's own order (heaviest first)
				return desc ? c > 0 : c < 0;
			});
			sp->SpecsDirty = false;
		}
	}
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
			if (tree && row.calls)
				ImGui::Text("%.2f", total ? 100.0 * (double)row.incl_cycles / (double)total : 0.0);
			else
				ImGui::TextDisabled("-");
			if (tree && row.calls && ImGui::IsItemHovered())
				ImGui::SetTooltip("From each call's entry to its return, callees included.\n"
				                  "Worst call %u cycles, worst frame %u.",
				                  row.call_max, row.frame_max);
			ImGui::TableSetColumnIndex(2);
			if (row.irq_cycles)
				ImGui::Text("%.2f", total ? 100.0 * (double)row.irq_cycles / (double)total : 0.0);
			else
				ImGui::TextDisabled("-");
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%llu", (unsigned long long)(t / frames));
			ImGui::TableSetColumnIndex(4);
			if (tree && row.calls)
				ImGui::Text("%u", row.calls);
			else
				ImGui::TextDisabled("-");
			ImGui::TableSetColumnIndex(5);
			// No 6502 instruction is shorter than 2 cycles: near 1 means WAI.
			if (cpi > 0.0 && cpi < 1.5)
				ImGui::TextColored(COL_WAIT, "%.2f", cpi);
			else
				ImGui::Text("%.2f", cpi);
			if (cpi > 0.0 && cpi < 1.5 && ImGui::IsItemHovered())
				ImGui::SetTooltip("Under 2 cycles per instruction: the CPU sat in WAI here,\n"
				                  "ticking one cycle per step. Waiting, not working.");
			ImGui::TableSetColumnIndex(6);
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
			ImGui::TableSetColumnIndex(7);
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

// ─── Call tree ──────────────────────────────────────────────────────────────

// Children of each node, heaviest first, rebuilt when the report changes.
std::vector<std::vector<int>> g_kids;
uint32_t                      g_kids_gen = 0;
bool                          g_kids_ok  = false;

void
draw_node(const prof_report_t *r, int i, int depth, uint64_t total, uint32_t frames)
{
	const prof_node_t &n = r->nodes[i];
	char name[PROF_NAME_MAX + 16];
	profile_session_node_name(r, i, name, sizeof name);
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow |
	                        ImGuiTreeNodeFlags_OpenOnDoubleClick;
	if (g_kids[i].empty())
		fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	// Open the heavy paths by default, so the hot spot is visible at once.
	if (depth < 8 && total && n.incl * 5 >= total)
		fl |= ImGuiTreeNodeFlags_DefaultOpen;
	ImGui::PushID(i);
	const bool open = ImGui::TreeNodeEx("node", fl, "%s", name);
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::Text("%s: %llu cycles inclusive, %llu self", name, (unsigned long long)n.incl,
		            (unsigned long long)n.self);
		ImGui::Text("%u calls, worst call %u cycles, worst frame %u", n.calls, n.call_max,
		            n.frame_max);
		if (n.row >= 0)
			ImGui::TextDisabled("Click to open in Disassembly.");
		ImGui::EndTooltip();
	}
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && n.row >= 0) {
		uint8_t  region, bank;
		uint16_t pc;
		perf_hist_key_decode(n.key, &region, &bank, &pc);
		debug_ui_request_goto(pc, 0);
	}
	ImGui::TableSetColumnIndex(1);
	ImGui::Text("%.2f", total ? 100.0 * (double)n.incl / (double)total : 0.0);
	ImGui::TableSetColumnIndex(2);
	ImGui::Text("%.2f", total ? 100.0 * (double)n.self / (double)total : 0.0);
	ImGui::TableSetColumnIndex(3);
	ImGui::Text("%llu", (unsigned long long)(n.incl / frames));
	ImGui::TableSetColumnIndex(4);
	if (n.parent >= 0)
		ImGui::Text("%u", n.calls);
	else
		ImGui::TextDisabled("-");
	ImGui::TableSetColumnIndex(5);
	if (n.parent >= 0)
		ImGui::Text("%u", n.call_max);
	else
		ImGui::TextDisabled("-");
	ImGui::PopID();
	if (open && !g_kids[i].empty()) {
		for (int c : g_kids[i])
			draw_node(r, c, depth + 1, total, frames);
		ImGui::TreePop();
	}
}

void
draw_tree(const prof_report_t *r, uint64_t total, uint32_t frames)
{
	// Keyed by report generation, not by pointer: a new report's tree often
	// lands in the old one's freed block with the same node count.
	if (!g_kids_ok || g_kids_gen != profile_session_report_generation()) {
		g_kids_ok  = true;
		g_kids_gen = profile_session_report_generation();
		g_kids.assign((size_t)r->node_count, {});
		for (int i = 0; i < r->node_count; i++)
			if (r->nodes[i].parent >= 0)
				g_kids[(size_t)r->nodes[i].parent].push_back(i);
		for (auto &k : g_kids)
			std::sort(k.begin(), k.end(),
			          [r](int a, int b) { return r->nodes[a].incl > r->nodes[b].incl; });
	}
	if (!profile_session_tree_consistent())
		ImGui::TextColored(COL_AMBIG, "The tree's totals do not match the histogram's.");
	if (!ImGui::BeginTable("proftree", 6,
	                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                           ImGuiTableFlags_BordersInnerV | DBGUI_TABLE_FLAGS_RESIZABLE))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Incl %", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Incl %", "100.00"));
	ImGui::TableSetupColumn("Self %", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Self %", "99.99"));
	ImGui::TableSetupColumn("Cycles/frame", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Cycles/frame", "9999999"));
	ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Calls", "99999999"));
	ImGui::TableSetupColumn("Worst call", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Worst call", "99999999"));
	ImGui::TableHeadersRow();
	draw_node(r, 0, 0, total, frames); // [main]
	draw_node(r, 1, 0, total, frames); // [IRQ]
	ImGui::EndTable();
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
	const prof_budget_summary_t *bs = profile_session_budget_summary();
	if (bs->have)
		ImGui::TextDisabled("Work %.1f%%, waiting %.1f%%; worst frame %u of %u cycles, %u over "
		                    "budget.",
		                    bs->total ? 100.0 * (double)(bs->total - bs->idle) / (double)bs->total
		                              : 0.0,
		                    bs->total ? 100.0 * (double)bs->idle / (double)bs->total : 0.0,
		                    bs->work_max, bs->budget, bs->overruns);

	// Export, beside the working directory unless a path is given.
	static char export_msg[sizeof g_export + 48] = "";
	ImGui::SetNextItemWidth(dbgui_col_width("", "wwwwwwwwwwwwwwwwwwww"));
	ImGui::InputText("##profexport", g_export, sizeof g_export);
	ImGui::SameLine();
	if (r->node_count > 2 && ImGui::Button("Save .folded")) {
		char path[sizeof g_export + 16];
		snprintf(path, sizeof path, "%s.folded", g_export);
		const int n = profile_session_export_folded(path);
		snprintf(export_msg, sizeof export_msg, "%s %s", n < 0 ? "Could not write" : "Wrote", path);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Collapsed stacks with self cycles: open in speedscope.app\n"
		                  "or feed to flamegraph.pl.");
	ImGui::SameLine();
	if (ImGui::Button("Save .json")) {
		char path[sizeof g_export + 16];
		snprintf(path, sizeof path, "%s.json", g_export);
		const int n = profile_session_export_json(path);
		snprintf(export_msg, sizeof export_msg, "%s %s", n < 0 ? "Could not write" : "Wrote", path);
	}
	if (export_msg[0]) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", export_msg);
	}

	if (r->node_count <= 2) {
		draw_rows(r, total, frames);
		return;
	}
	if (ImGui::BeginTabBar("proftabs")) {
		if (ImGui::BeginTabItem("Routines")) {
			draw_rows(r, total, frames);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Call tree")) {
			draw_tree(r, total, frames);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
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
