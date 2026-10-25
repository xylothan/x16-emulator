// Commander X16 Emulator — ImGui debugger: the Profile panel's "Callers &
// callees" tab. See callers_view.h; the arithmetic is prof_report_routine_view().

#include "callers_view.h"

#include "imgui.h"
#include "debug_ui_bridge.h"  // debug_ui_request_goto
#include "debug_ui_widgets.h" // dbgui_text_width, dbgui_col_width, DBGUI_TABLE_FLAGS_RESIZABLE

#include "../perf_hist.h"
#include "../profile_session.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

int              g_row      = -1;   // selected report row
bool             g_show_req = false;
uint32_t         g_gen      = ~0u;  // report generation the selection belongs to
std::vector<int> g_back, g_fwd;     // navigation history (rows)

// The view for g_row, rebuilt when the selection or the report changes.
prof_routine_view_t g_view;
int                 g_view_row = -2;
uint32_t            g_view_gen = ~0u;
char                g_pick_filter[64] = "";

void
sync_generation(void)
{
	if (g_gen != profile_session_report_generation()) {
		g_gen = profile_session_report_generation();
		g_row = -1;
		g_back.clear();
		g_fwd.clear();
	}
}

const prof_routine_view_t *
view_for(const prof_report_t *r, int row)
{
	if (g_view_row != row || g_view_gen != profile_session_report_generation()) {
		prof_routine_view_free(&g_view);
		g_view_row = row;
		g_view_gen = profile_session_report_generation();
		if (!prof_report_routine_view(r, row, &g_view))
			return nullptr;
	}
	return &g_view;
}

void
navigate(int row)
{
	if (row == g_row || row < 0)
		return;
	if (g_row >= 0)
		g_back.push_back(g_row);
	g_fwd.clear();
	g_row = row;
}

// "12.3% (2.05 ms per frame)"
void
share_text(uint64_t cycles, uint64_t of, uint32_t frames, char *buf, size_t n)
{
	const double ms = (double)cycles / (double)(frames ? frames : 1) / (profile_session_mhz() * 1000.0);
	snprintf(buf, n, "%5.1f%%  (%.2f ms/frame)", of ? 100.0 * (double)cycles / (double)of : 0.0, ms);
}

// A share bar filling the cell's width in proportion to `frac`, with text on it.
void
share_bar(float frac, const char *text, ImU32 col)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float  w = ImGui::GetContentRegionAvail().x;
	const float  h = ImGui::GetTextLineHeight();
	ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w * std::min(1.0f, std::max(0.0f, frac)), p.y + h), col);
	ImGui::TextUnformatted(text);
}

void
goto_node(const prof_report_t *r, int node)
{
	const prof_node_t &n = r->nodes[node];
	if (n.key >= PH_KEYS)
		return;
	uint8_t  region, bank;
	uint16_t pc;
	perf_hist_key_decode(n.key, &region, &bank, &pc);
	debug_ui_request_goto(pc, region == PH_REGION_LOW ? 0 : bank);
}

const char *
edge_name(const prof_report_t *r, const prof_edge_t &e, char *buf, size_t n)
{
	if (e.row >= 0) {
		snprintf(buf, n, "%s", r->rows[e.row].name);
	} else {
		char nm[PROF_NAME_MAX + 16];
		profile_session_node_name(r, e.node, nm, sizeof nm);
		if (!strcmp(nm, "[main]"))
			snprintf(buf, n, "[main]  (top level: no caller seen)");
		else if (!strcmp(nm, "[IRQ]"))
			snprintf(buf, n, "[IRQ]  (an interrupt)");
		else
			snprintf(buf, n, "%s", nm);
	}
	return buf;
}

// One side's table: callers or callees.
void
edge_table(const char *id, const prof_report_t *r, const prof_edge_t *e, int n, uint64_t total,
           uint32_t frames, bool callees, uint64_t own, ImU32 col)
{
	if (!ImGui::BeginTable(id, 3,
	                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
	                           DBGUI_TABLE_FLAGS_RESIZABLE))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Share of its time",
	                        ImGuiTableColumnFlags_WidthFixed, dbgui_text_width("100.0%  (99.99 ms/frame)") + 8);
	ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Calls", "9999999"));
	ImGui::TableSetupColumn("Routine", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	const char *tips[3] = {
		callees ? "How much of the selected routine's total time went into this call:\n"
		          "the callee's total time, counting only calls made from it."
		        : "How much of the selected routine's total time this caller is\n"
		          "responsible for: the time of the calls it made.",
		callees ? "How many times the selected routine called it." : "How many times it called the selected routine.",
		"Click to move to it. Double-click to open its source.",
	};
	for (int c = 0; c < 3; c++) {
		ImGui::TableSetColumnIndex(c);
		ImGui::TableHeader(ImGui::TableGetColumnName(c));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tips[c]);
	}
	char text[64];
	if (callees && own) {
		// Its own code first: the part of its time no callee explains.
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		share_text(own, total, frames, text, sizeof text);
		share_bar(total ? (float)((double)own / (double)total) : 0.0f, text, IM_COL32(120, 120, 120, 90));
		ImGui::TableSetColumnIndex(1);
		ImGui::TextDisabled("-");
		ImGui::TableSetColumnIndex(2);
		ImGui::TextDisabled("its own code");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Time in the selected routine's own instructions, not in anything it called.");
	}
	for (int i = 0; i < n; i++) {
		ImGui::TableNextRow();
		ImGui::PushID(i);
		ImGui::TableSetColumnIndex(0);
		share_text(e[i].cycles, total, frames, text, sizeof text);
		share_bar(total ? (float)((double)e[i].cycles / (double)total) : 0.0f, text, col);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%u", e[i].calls);
		ImGui::TableSetColumnIndex(2);
		char name[PROF_NAME_MAX + 48];
		edge_name(r, e[i], name, sizeof name);
		if (ImGui::Selectable(name, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				goto_node(r, e[i].node);
			else if (e[i].row >= 0)
				navigate(e[i].row);
		}
		if (ImGui::IsItemHovered()) {
			char src[300];
			const prof_node_t &nd = r->nodes[e[i].node];
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(name);
			if (nd.key < PH_KEYS && profile_session_key_source(nd.key, src, sizeof src))
				ImGui::TextDisabled("%s", src);
			ImGui::TextDisabled(e[i].row >= 0 ? "Click: show its callers and callees.  Double-click: open the source."
			                                  : "Not a routine: the top of the call tree.");
			ImGui::EndTooltip();
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

bool
contains_ci(const char *hay, const char *needle)
{
	if (!needle[0])
		return true;
	const size_t n = strlen(needle);
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (i == n)
			return true;
	}
	return false;
}

} // namespace

void
profile_select_routine(int row, bool show)
{
	navigate(row);
	if (show)
		g_show_req = true;
}

int
profile_selected_routine(void)
{
	return g_row;
}

bool
profile_take_show_request(void)
{
	const bool s = g_show_req;
	g_show_req   = false;
	return s;
}

void
profile_callers_draw(const prof_report_t *r)
{
	if (!r || r->node_count <= 2) {
		ImGui::TextDisabled("No call tree in this recording. Tick \"Who called what\" before starting one.");
		return;
	}
	sync_generation();
	const uint32_t frames = profile_session_report_frames() ? profile_session_report_frames() : 1;

	// Nothing chosen yet: the routine with the most total time (other than the
	// roots, which have no row).
	if (g_row < 0 || g_row >= r->count) {
		uint64_t best = 0;
		for (int i = 0; i < r->count; i++)
			if (r->rows[i].calls && r->rows[i].incl_cycles > best) {
				best  = r->rows[i].incl_cycles;
				g_row = i;
			}
		if (g_row < 0)
			g_row = 0;
	}

	// Toolbar: back, forward, and a picker.
	ImGui::BeginDisabled(g_back.empty());
	if (ImGui::ArrowButton("##back", ImGuiDir_Left)) {
		g_fwd.push_back(g_row);
		g_row = g_back.back();
		g_back.pop_back();
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Back to the previous routine.");
	ImGui::SameLine();
	ImGui::BeginDisabled(g_fwd.empty());
	if (ImGui::ArrowButton("##fwd", ImGuiDir_Right)) {
		g_back.push_back(g_row);
		g_row = g_fwd.back();
		g_fwd.pop_back();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_combo_width("wwwwwwwwwwwwwwwwwwwwwwwwwwww"));
	if (ImGui::BeginCombo("##pick", r->rows[g_row].name, ImGuiComboFlags_HeightLarge)) {
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##pickfilter", "type to filter", g_pick_filter, sizeof g_pick_filter);
		// Routines that were called, heaviest total first.
		std::vector<int> order;
		for (int i = 0; i < r->count; i++)
			if (r->rows[i].calls && contains_ci(r->rows[i].name, g_pick_filter))
				order.push_back(i);
		std::sort(order.begin(), order.end(),
		          [r](int a, int b) { return r->rows[a].incl_cycles > r->rows[b].incl_cycles; });
		for (int i : order) {
			char item[PROF_NAME_MAX + 32];
			snprintf(item, sizeof item, "%s##%d", r->rows[i].name, i);
			if (ImGui::Selectable(item, i == g_row)) {
				navigate(i);
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Pick a routine. Or right-click one in the Routines, Call tree or Flame\n"
		                  "graph tab and choose \"Callers & callees\".");

	const prof_routine_view_t *v = view_for(r, g_row);
	const prof_row_t          &row = r->rows[g_row];
	const uint64_t             all = r->main_cycles + r->irq_cycles;
	if (!v || v->total == 0) {
		ImGui::TextDisabled("%s was never called (it ran only as part of another routine's code).", row.name);
		return;
	}

	// The routine itself.
	char tot[64], own[64];
	share_text(v->total, all, frames, tot, sizeof tot);
	share_text(v->own, all, frames, own, sizeof own);
	ImGui::Separator();
	ImGui::TextUnformatted(row.name);
	ImGui::SameLine();
	if (ImGui::SmallButton("Open source"))
		debug_ui_request_goto(row.addr, row.region == PH_REGION_LOW ? 0 : row.bank);
	ImGui::TextDisabled("Total time %s of the recording.  Own time %s.  Called %u times.", tot, own, v->calls);

	// Callers left, callees right.
	const float h = std::max(120.0f, ImGui::GetContentRegionAvail().y);
	if (ImGui::BeginTable("##sides", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted("Called by");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Who called it. The shares add up to its total time.");
		edge_table("callers", r, v->callers, v->ncallers, v->total, frames, false, 0, IM_COL32(90, 140, 230, 110));
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted("It calls");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("What it called, plus its own code. The shares add up to its total time.");
		edge_table("callees", r, v->callees, v->ncallees, v->total, frames, true, v->own, IM_COL32(230, 140, 70, 110));
		ImGui::EndTable();
	}
}
