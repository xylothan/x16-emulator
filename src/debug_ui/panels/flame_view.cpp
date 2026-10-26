// Commander X16 Emulator — ImGui debugger: the Profile panel's Flame graph tab.
// See flame_view.h.
//
// Drawn top-down (an "icicle"): the whole recording at the top, each routine's
// callees beneath it, widest first. Nothing is laid out ahead of time: each
// frame walks the tree from the focused node, and a box narrower than a pixel
// is not drawn, nor is anything under it, so even a large tree costs only as
// much as what is visible.

#include "flame_view.h"

#include "imgui.h"
#include "debug_ui_bridge.h"  // debug_ui_request_goto
#include "debug_ui_widgets.h" // dbgui_text_width, dbgui_field_width

#include "../perf_hist.h"
#include "../profile_cct.h"
#include "../profile_session.h"
#include "callers_view.h"

#include <algorithm>
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

// Children of each node, heaviest first, rebuilt per report.
std::vector<std::vector<int>> g_kids;
uint32_t                      g_gen   = ~0u;
int                           g_focus = -1; // node the view is zoomed into; -1 = everything
int                           g_ctx   = -1; // node the right-click menu is for
char                          g_find[64] = "";

void
rebuild(const prof_report_t *r)
{
	if (g_gen == profile_session_report_generation() && (int)g_kids.size() == r->node_count)
		return;
	g_gen   = profile_session_report_generation();
	g_focus = -1;
	g_kids.assign((size_t)r->node_count, {});
	for (int i = 0; i < r->node_count; i++)
		if (r->nodes[i].parent >= 0)
			g_kids[(size_t)r->nodes[i].parent].push_back(i);
	for (auto &k : g_kids)
		std::sort(k.begin(), k.end(), [r](int a, int b) { return r->nodes[a].incl > r->nodes[b].incl; });
}

// A routine keeps its colour wherever it appears, so the eye can follow it
// from caller to caller. Keyed by its report row (or, unnamed, its address).
ImU32
routine_color(const prof_report_t *r, int node, bool dim)
{
	const prof_node_t &n = r->nodes[node];
	const uint32_t     k = n.row >= 0 ? (uint32_t)n.row * 2654435761u : n.key * 40503u;
	const float hue = (float)((k >> 8) & 0xFFFF) / 65535.0f * 0.17f + 0.0f; // reds to yellows
	return ImColor::HSV(hue, dim ? 0.15f : 0.60f, dim ? 0.45f : 0.95f);
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

struct Ctx {
	const prof_report_t *r;
	ImDrawList          *dl;
	ImVec2               origin;
	float                width, row_h;
	uint64_t             total;    // cycles in the recording
	uint32_t             frames;
	double               mhz;
	int                  hover = -1;
	int                  max_depth = 0;
};

void
draw_box(Ctx &c, int node, float x0, float x1, int depth, bool in_match)
{
	if (x1 - x0 < 1.0f)
		return;
	const prof_node_t &n = c.r->nodes[node];
	char name[PROF_NAME_MAX + 16];
	profile_session_node_name(c.r, node, name, sizeof name);
	const bool searching = g_find[0] != 0;
	const bool match     = searching && contains_ci(name, g_find);
	const bool lit       = !searching || match || in_match;

	const float y = c.origin.y + depth * c.row_h;
	if (depth + 1 > c.max_depth)
		c.max_depth = depth + 1;
	const ImVec2 a(c.origin.x + x0, y), b(c.origin.x + x1, y + c.row_h - 1);
	c.dl->AddRectFilled(a, ImVec2(std::max(b.x - 1, a.x + 1), b.y), routine_color(c.r, node, !lit));
	if (x1 - x0 > 28.0f) {
		c.dl->PushClipRect(a, ImVec2(b.x - 2, b.y), true);
		c.dl->AddText(ImVec2(a.x + 3, y + 1), lit ? IM_COL32(25, 25, 25, 255) : IM_COL32(90, 90, 90, 255), name);
		c.dl->PopClipRect();
	}
	const ImVec2 m = ImGui::GetIO().MousePos;
	if (m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y)
		c.hover = node;

	// Callees, left to right, widest first; what is left over is own time.
	if (n.incl == 0)
		return;
	float cx = x0;
	for (int k : g_kids[(size_t)node]) {
		const float w = (float)((double)(x1 - x0) * (double)c.r->nodes[k].incl / (double)n.incl);
		if (w < 1.0f)
			break; // sorted: everything after is narrower still
		draw_box(c, k, cx, cx + w, depth + 1, in_match || match);
		cx += w;
	}
}

// Total time as share and as time per frame: "12.3% (2.05 ms/frame)".
void
time_text(const Ctx &c, uint64_t cycles, char *buf, size_t n)
{
	const double ms = (double)cycles / (double)(c.frames ? c.frames : 1) / (c.mhz * 1000.0);
	snprintf(buf, n, "%.1f%% (%.2f ms per frame)", c.total ? 100.0 * (double)cycles / (double)c.total : 0.0, ms);
}

} // namespace

void
profile_flame_draw(const prof_report_t *r)
{
	if (!r || r->node_count <= 2) {
		ImGui::TextDisabled("No call tree in this recording. Tick \"Who called what\" before starting one.");
		return;
	}
	rebuild(r);
	if (g_focus >= r->node_count)
		g_focus = -1;

	Ctx c;
	c.r      = r;
	c.total  = r->main_cycles + r->irq_cycles;
	c.frames = profile_session_report_frames() ? profile_session_report_frames() : 1;
	c.mhz    = profile_session_mhz();

	// Toolbar: where we are, search.
	ImGui::AlignTextToFramePadding();
	if (g_focus < 0) {
		ImGui::TextUnformatted("Showing everything.");
	} else {
		if (ImGui::SmallButton("Everything"))
			g_focus = -1;
		// The path down to the focused box, each step clickable.
		std::vector<int> path;
		for (int p = g_focus; p >= 0; p = r->nodes[p].parent)
			path.push_back(p);
		for (int i = (int)path.size() - 1; i >= 0 && g_focus >= 0; i--) {
			char name[PROF_NAME_MAX + 16];
			profile_session_node_name(r, path[(size_t)i], name, sizeof name);
			ImGui::SameLine(0, 2);
			ImGui::TextDisabled(">");
			ImGui::SameLine(0, 2);
			ImGui::PushID(i);
			if (ImGui::SmallButton(name))
				g_focus = path[(size_t)i];
			ImGui::PopID();
		}
	}
	ImGui::SameLine(0, 20);
	ImGui::SetNextItemWidth(dbgui_field_width("wwwwwwwwwwwwwwww"));
	ImGui::InputTextWithHint("##flamefind", "highlight routine", g_find, sizeof g_find);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Light up every box whose name contains this; dim the rest.");
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Each box is a routine, as wide as its total time: itself plus everything\n"
		                  "it called. The boxes under it are the routines it called, widest first;\n"
		                  "the gap to their right is its own time. A wide box with nothing wide\n"
		                  "beneath it is where the time actually goes.\n\n"
		                  "Click a box to zoom into it. Double-click to open its source.\n"
		                  "The same routine has the same colour wherever it is called from.");

	// The canvas. Rows below the focus can be many: scroll vertically.
	c.row_h = ImGui::GetTextLineHeight() + 4.0f;
	ImGui::BeginChild("flame", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	c.dl     = ImGui::GetWindowDrawList();
	c.origin = ImGui::GetCursorScreenPos();
	c.width  = std::max(50.0f, ImGui::GetContentRegionAvail().x);

	if (g_focus < 0) {
		// A top bar for the whole recording, then [main] and [IRQ] side by side.
		const float y = c.origin.y;
		c.dl->AddRectFilled(c.origin, ImVec2(c.origin.x + c.width - 1, y + c.row_h - 1), IM_COL32(150, 150, 150, 255));
		c.dl->AddText(ImVec2(c.origin.x + 3, y + 1), IM_COL32(20, 20, 20, 255), "the whole recording");
		const float w_main = (float)((double)c.width * (double)r->nodes[CCT_NODE_MAIN].incl / (double)(c.total ? c.total : 1));
		draw_box(c, CCT_NODE_MAIN, 0, w_main, 1, false);
		draw_box(c, CCT_NODE_IRQ, w_main, c.width, 1, false);
	} else {
		draw_box(c, g_focus, 0, c.width, 0, false);
	}
	if (c.max_depth < 2)
		c.max_depth = 2;
	ImGui::InvisibleButton("##flamecanvas", ImVec2(c.width, c.max_depth * c.row_h));
	const bool item_hovered = ImGui::IsItemHovered();

	if (item_hovered && c.hover >= 0) {
		const prof_node_t &n = r->nodes[c.hover];
		char name[PROF_NAME_MAX + 16], tot[64], own[64];
		profile_session_node_name(r, c.hover, name, sizeof name);
		time_text(c, n.incl, tot, sizeof tot);
		time_text(c, n.self, own, sizeof own);
		ImGui::BeginTooltip();
		ImGui::Text("%s", name);
		ImGui::Text("Total time %s", tot);
		ImGui::Text("Own time   %s", own);
		if (n.parent >= 0 && n.key < PH_KEYS) {
			const double slow_ms = (double)n.call_max / (c.mhz * 1000.0);
			ImGui::Text("%u calls along this path; slowest single call %.3f ms", n.calls, slow_ms);
			char where[40], src[300];
			profile_session_key_where(n.key, where, sizeof where);
			ImGui::TextDisabled("%s", where);
			if (profile_session_key_source(n.key, src, sizeof src))
				ImGui::TextDisabled("%s", src);
		}
		// The call path, outermost first.
		std::vector<int> path;
		for (int p = n.parent; p >= 0; p = r->nodes[p].parent)
			path.push_back(p);
		if (!path.empty()) {
			ImGui::Separator();
			ImGui::TextDisabled("Called through:");
			for (int i = (int)path.size() - 1; i >= 0; i--) {
				char pn[PROF_NAME_MAX + 16];
				profile_session_node_name(r, path[(size_t)i], pn, sizeof pn);
				ImGui::TextDisabled("  %*s%s", ((int)path.size() - 1 - i) * 2, "", pn);
			}
		}
		ImGui::TextDisabled("Click: zoom in.  Double-click: open the source.  Right-click: more.");
		ImGui::EndTooltip();
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && n.key < PH_KEYS) {
			uint8_t  region, bank;
			uint16_t pc;
			perf_hist_key_decode(n.key, &region, &bank, &pc);
			profile_goto(pc, region == PH_REGION_LOW ? 0 : bank);
		} else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
			g_focus = c.hover;
		}
	}
	if (item_hovered && c.hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
		g_ctx = c.hover;
		ImGui::OpenPopup("flamectx");
	}
	if (ImGui::BeginPopup("flamectx")) {
		if (g_ctx >= 0 && g_ctx < r->node_count) {
			const prof_node_t &n = r->nodes[g_ctx];
			if (ImGui::MenuItem("Callers & callees", nullptr, false, n.row >= 0))
				profile_select_routine(n.row, true);
			if (ImGui::MenuItem("Zoom in"))
				g_focus = g_ctx;
			if (ImGui::MenuItem("Open source", nullptr, false, n.key < PH_KEYS)) {
				uint8_t  region, bank;
				uint16_t pc;
				perf_hist_key_decode(n.key, &region, &bank, &pc);
				profile_goto(pc, region == PH_REGION_LOW ? 0 : bank);
			}
		}
		ImGui::EndPopup();
	}
	ImGui::EndChild();

	if (g_find[0]) {
		// Every match counted once: skip one inside another match, whose time
		// its ancestor's total already includes.
		uint64_t found = 0;
		for (int i = 0; i < r->node_count; i++) {
			char nm[PROF_NAME_MAX + 16];
			profile_session_node_name(r, i, nm, sizeof nm);
			if (!contains_ci(nm, g_find))
				continue;
			bool inside = false;
			for (int p = r->nodes[i].parent; p >= 0 && !inside; p = r->nodes[p].parent) {
				char pn[PROF_NAME_MAX + 16];
				profile_session_node_name(r, p, pn, sizeof pn);
				inside = contains_ci(pn, g_find);
			}
			if (!inside)
				found += r->nodes[i].incl;
		}
		char tot[64];
		time_text(c, found, tot, sizeof tot);
		ImGui::TextDisabled("Matching \"%s\": %s of the recording.", g_find, tot);
	}
}
