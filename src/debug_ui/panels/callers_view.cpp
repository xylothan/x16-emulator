// Commander X16 Emulator — ImGui debugger: the Profile panel's "Callers &
// callees" tab. See callers_view.h; the arithmetic is prof_report_routine_view().

#include "callers_view.h"

#include "imgui.h"
#include "debug_ui_bridge.h"  // debug_ui_request_goto
#include "debug_ui_widgets.h" // dbgui_text_width, dbgui_col_width, DBGUI_TABLE_FLAGS_RESIZABLE

#include "../perf_hist.h"
#include "../profile_session.h"

#include <algorithm>
#include <cmath>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// Open code from the Profile panel, unless a saved run is shown: its addresses
// describe the program as it was, and memory now may hold something else.
static inline void
profile_goto(uint16_t addr, uint8_t bank)
{
	if (!profile_session_run_loaded())
		debug_ui_request_goto(addr, bank);
}

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
bool                g_graph = false; // draw boxes and arrows instead of the two lists

// The graph for g_row, rebuilt when the selection or the report changes.
prof_graph_t g_gr;
int          g_gr_row = -2;
uint32_t     g_gr_gen = ~0u;
constexpr int GRAPH_PER_COL = 6;

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
	profile_goto(pc, region == PH_REGION_LOW ? 0 : bank);
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

// A routine's colour, the same wherever it is drawn.
ImU32
routine_color(int row, float alpha = 1.0f)
{
	if (row < 0)
		return ImGui::GetColorU32(ImGuiCol_FrameBgHovered, alpha);
	const uint32_t h = (uint32_t)row * 2654435761u;
	return ImColor::HSV((float)(h >> 8 & 0xFFFF) / 65535.0f, 0.40f, 0.78f, alpha);
}

const prof_graph_t *
graph_for(const prof_report_t *r, int row)
{
	if (g_gr_row != row || g_gr_gen != profile_session_report_generation()) {
		prof_graph_free(&g_gr);
		g_gr_row = row;
		g_gr_gen = profile_session_report_generation();
		if (!prof_report_graph(r, row, GRAPH_PER_COL, &g_gr))
			return nullptr;
	}
	return &g_gr;
}

void
box_name(const prof_report_t *r, const prof_gbox_t &b, char *buf, size_t n)
{
	if (b.row >= 0)
		snprintf(buf, n, "%s", r->rows[b.row].name);
	else
		profile_session_node_name(r, b.node, buf, (int)n);
}

void
draw_graph(const prof_report_t *r, uint32_t frames)
{
	const prof_graph_t *g = graph_for(r, g_row);
	if (!g || g->nboxes == 0) {
		ImGui::TextDisabled("No graph for this routine.");
		return;
	}
	const uint64_t all = r->main_cycles + r->irq_cycles;
	auto own_of = [&](const prof_gbox_t &b) -> uint64_t {
		return b.row >= 0 ? r->rows[b.row].main_cycles + r->rows[b.row].irq_cycles : 0;
	};
	uint64_t max_own = 1, max_edge = 1;
	for (int i = 0; i < g->nboxes; i++)
		max_own = std::max(max_own, own_of(g->boxes[i]));
	for (int i = 0; i < g->nedges; i++)
		max_edge = std::max(max_edge, g->edges[i].cycles);

	// Layout: five columns; boxes stacked heaviest first, centred vertically.
	const float lh     = ImGui::GetTextLineHeight();
	const float head_h = lh + 8.0f;
	const float min_h = lh * 2.0f + 8.0f, max_h = lh * 5.0f;
	const float gap   = 10.0f;
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float  col_w = std::max(120.0f, avail.x / PROF_GRAPH_COLS);
	const float  box_w = std::min(col_w - 36.0f, ImGui::GetFontSize() * 13.0f);
	std::vector<float> h((size_t)g->nboxes), y((size_t)g->nboxes);
	std::vector<std::vector<int>> cols(PROF_GRAPH_COLS);
	for (int i = 0; i < g->nboxes; i++) {
		// Box size is own time: the square root, so a routine with a tenth of
		// the time is still easy to see.
		h[(size_t)i] = min_h + (max_h - min_h) * (float)std::sqrt((double)own_of(g->boxes[i]) / (double)max_own);
		cols[(size_t)g->boxes[i].col].push_back(i);
	}
	float tallest = 0.0f;
	for (auto &c : cols) {
		std::stable_sort(c.begin(), c.end(), [g](int a, int b) { return g->boxes[a].weight > g->boxes[b].weight; });
		float t = 0.0f;
		for (int i : c)
			t += h[(size_t)i] + gap;
		tallest = std::max(tallest, t);
	}
	const float canvas_h = std::max(avail.y - 4.0f, head_h + tallest + lh * 2.0f);
	ImGui::BeginChild("##graph", ImVec2(0, std::max(160.0f, avail.y)), ImGuiChildFlags_None,
	                  ImGuiWindowFlags_HorizontalScrollbar);
	const ImVec2 o = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##graphcanvas", ImVec2(col_w * PROF_GRAPH_COLS, canvas_h));
	const bool   hovered = ImGui::IsItemHovered();
	const ImVec2 mouse   = ImGui::GetIO().MousePos;
	ImDrawList  *dl      = ImGui::GetWindowDrawList();
	auto box_x = [&](int col) { return o.x + col * col_w + (col_w - box_w) * 0.5f; };
	for (auto &c : cols) {
		float t = 0.0f;
		for (int i : c)
			t += h[(size_t)i] + gap;
		float at = o.y + head_h + std::max(0.0f, (canvas_h - head_h - t) * 0.5f);
		for (int i : c) {
			y[(size_t)i] = at;
			at += h[(size_t)i] + gap;
		}
	}

	// Column headings.
	static const char *const heads[PROF_GRAPH_COLS] = {"Callers of those", "Called by", "Routine", "It calls",
	                                                   "Called by those"};
	for (int c = 0; c < PROF_GRAPH_COLS; c++) {
		const float tw = ImGui::CalcTextSize(heads[c]).x;
		dl->AddText(ImVec2(o.x + c * col_w + (col_w - tw) * 0.5f, o.y + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled),
		            heads[c]);
	}

	// Arrows first, under the boxes. Width is the time along the arrow.
	int    hover_edge = -1;
	float  hover_d    = 8.0f;
	for (int i = 0; i < g->nedges; i++) {
		const prof_gedge_t &e = g->edges[i];
		const prof_gbox_t  &a = g->boxes[e.from], &b = g->boxes[e.to];
		const float         w = 1.0f + 6.0f * (float)((double)e.cycles / (double)max_edge);
		const bool          caller_side = b.col <= PROF_GRAPH_CENTRE;
		const ImU32 col = caller_side ? IM_COL32(90, 140, 230, 200) : IM_COL32(230, 140, 70, 200);
		ImVec2      p0, p1, c0, c1;
		if (b.col > a.col) {
			// Forward: right side to left side.
			p0 = ImVec2(box_x(a.col) + box_w, y[(size_t)e.from] + h[(size_t)e.from] * 0.5f);
			p1 = ImVec2(box_x(b.col), y[(size_t)e.to] + h[(size_t)e.to] * 0.5f);
			const float dx = (p1.x - p0.x) * 0.5f;
			c0 = ImVec2(p0.x + dx, p0.y);
			c1 = ImVec2(p1.x - dx, p1.y);
		} else {
			// Back toward the centre (recursion, a callee calling back): under both.
			p0 = ImVec2(box_x(a.col) + box_w * 0.5f, y[(size_t)e.from] + h[(size_t)e.from]);
			p1 = ImVec2(box_x(b.col) + box_w * 0.5f, y[(size_t)e.to] + h[(size_t)e.to]);
			const float drop = lh * 2.5f;
			c0 = ImVec2(p0.x, std::max(p0.y, p1.y) + drop);
			c1 = ImVec2(p1.x, std::max(p0.y, p1.y) + drop);
		}
		dl->AddBezierCubic(p0, c0, c1, p1, col, w);
		// The arrowhead, along the curve's last stretch.
		const ImVec2 d(p1.x - c1.x, p1.y - c1.y);
		const float  len = std::sqrt(d.x * d.x + d.y * d.y);
		if (len > 0.5f) {
			const ImVec2 u(d.x / len, d.y / len), nrm(-u.y, u.x);
			const float  s = 6.0f + w;
			dl->AddTriangleFilled(p1, ImVec2(p1.x - u.x * s + nrm.x * s * 0.5f, p1.y - u.y * s + nrm.y * s * 0.5f),
			                      ImVec2(p1.x - u.x * s - nrm.x * s * 0.5f, p1.y - u.y * s - nrm.y * s * 0.5f), col);
		}
		// Hover near the curve's middle.
		const float  t3 = 0.125f, t1 = 0.375f; // Bezier at t = 0.5
		const ImVec2 mid(t3 * p0.x + t1 * c0.x + t1 * c1.x + t3 * p1.x, t3 * p0.y + t1 * c0.y + t1 * c1.y + t3 * p1.y);
		const float  dm = std::hypot(mouse.x - mid.x, mouse.y - mid.y);
		if (hovered && dm < hover_d) {
			hover_d    = dm;
			hover_edge = i;
		}
	}

	// Boxes.
	int hover_box = -1;
	for (int i = 0; i < g->nboxes; i++) {
		const prof_gbox_t &b  = g->boxes[i];
		const ImVec2       p0(box_x(b.col), y[(size_t)i]), p1(p0.x + box_w, p0.y + h[(size_t)i]);
		dl->AddRectFilled(p0, p1, routine_color(b.row), 4.0f);
		if (i == 0)
			dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Text), 4.0f, 0, 2.5f);
		char name[PROF_NAME_MAX + 16], line2[64];
		box_name(r, b, name, sizeof name);
		const uint64_t own = own_of(b);
		snprintf(line2, sizeof line2, "own %.1f%%", all ? 100.0 * (double)own / (double)all : 0.0);
		dl->PushClipRect(p0, p1, true);
		dl->AddText(ImVec2(p0.x + 5, p0.y + 3), IM_COL32(20, 20, 20, 255), name);
		dl->AddText(ImVec2(p0.x + 5, p0.y + 3 + lh), IM_COL32(40, 40, 40, 255), line2);
		dl->PopClipRect();
		if (hovered && mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y)
			hover_box = i;
	}
	for (int c = 0; c < PROF_GRAPH_COLS; c++)
		if (g->hidden[c]) {
			char more[48];
			snprintf(more, sizeof more, "+%d more", g->hidden[c]);
			float bottom = o.y + head_h;
			for (int i : cols[(size_t)c])
				bottom = std::max(bottom, y[(size_t)i] + h[(size_t)i]);
			dl->AddText(ImVec2(box_x(c) + 4, bottom + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), more);
		}

	// Hover and click.
	if (hover_box >= 0) {
		const prof_gbox_t &b = g->boxes[hover_box];
		char name[PROF_NAME_MAX + 16], tot[64], own[64], src[300];
		box_name(r, b, name, sizeof name);
		ImGui::BeginTooltip();
		ImGui::TextUnformatted(name);
		if (b.row >= 0) {
			share_text(r->rows[b.row].incl_cycles, all, frames, tot, sizeof tot);
			share_text(own_of(b), all, frames, own, sizeof own);
			ImGui::TextDisabled("Total %s of the recording", tot);
			ImGui::TextDisabled("Own   %s", own);
			ImGui::TextDisabled("Called %u times", r->rows[b.row].calls);
		} else {
			ImGui::TextDisabled("The top of the call tree: no caller was seen.");
		}
		const prof_node_t *nd = b.node >= 0 ? &r->nodes[b.node] : nullptr;
		if (nd && nd->key < PH_KEYS && profile_session_key_source(nd->key, src, sizeof src))
			ImGui::TextDisabled("%s", src);
		if (b.row >= 0)
			ImGui::TextDisabled(hover_box == 0 ? "Double-click: open the source." : "Click: centre on it.");
		ImGui::EndTooltip();
		// A click centres on a box; the second click of a double-click is not
		// another one, since the boxes have moved. Double-click the centre to
		// open its source.
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			if (hover_box == 0 && b.row >= 0) {
				const prof_row_t &rw = r->rows[b.row];
				profile_goto(rw.addr, rw.region == PH_REGION_LOW ? 0 : rw.bank);
			}
		} else if (b.row >= 0 && hover_box != 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
			navigate(b.row);
		}
	} else if (hover_edge >= 0) {
		const prof_gedge_t &e = g->edges[hover_edge];
		char a[PROF_NAME_MAX + 16], b[PROF_NAME_MAX + 16], t[64];
		box_name(r, g->boxes[e.from], a, sizeof a);
		box_name(r, g->boxes[e.to], b, sizeof b);
		share_text(e.cycles, all, frames, t, sizeof t);
		ImGui::SetTooltip("%s calls %s\n%u calls; %s of the recording spent in them.", a, b, e.calls, t);
	}
	ImGui::EndChild();
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
	ImGui::SameLine();
	if (ImGui::RadioButton("Lists", !g_graph))
		g_graph = false;
	ImGui::SameLine();
	if (ImGui::RadioButton("Graph", g_graph))
		g_graph = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Boxes and arrows, two calls each way around the routine. A box's size\n"
		                  "is its own time; an arrow's width is the time spent in those calls.\n"
		                  "Click a box to centre on it.");

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
		profile_goto(row.addr, row.region == PH_REGION_LOW ? 0 : row.bank);
	ImGui::TextDisabled("Total time %s of the recording.  Own time %s.  Called %u times.", tot, own, v->calls);

	if (g_graph) {
		draw_graph(r, frames);
		return;
	}

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
