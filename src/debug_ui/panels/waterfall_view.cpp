// Commander X16 Emulator — ImGui debugger: the Profile panel's Waterfall tab.
// See waterfall_view.h.
//
// Like the rest of the Profile panel this is never drawn while a capture runs
// (the debugger goes quiet), so building spans and drawing thousands of ticks
// costs the measured machine nothing.

#include "waterfall_view.h"

#include "imgui.h"
#include "debug_ui_bridge.h"  // debug_ui_request_goto
#include "debug_ui_widgets.h" // dbgui_text_width, DBGUI_TABLE_FLAGS_RESIZABLE

#include "../perf_hist.h"
#include "../profile_cct.h"
#include "../profile_session.h"
#include "../waterfall.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

// VGA: 525 lines a frame. Composite would be 262.5, but the axis is a guide,
// and the frame's own length gives cycles per line either way.
constexpr int LINES_PER_FRAME = 525;
constexpr int MAX_FRAMES      = 64;
constexpr int MAX_CPU_ROWS    = 16;
constexpr int MAX_IRQ_ROWS    = 6;

struct Span {
	uint32_t c0, c1;
	int      node;
	int      thread; // 0 main, 1 IRQ
	int      depth;
};

// Everything derived from the selected frame, rebuilt when it changes.
struct FrameView {
	uint32_t                         gen      = ~0u;
	uint32_t                         number   = ~0u;
	const wf_frame_t                *frame    = nullptr;
	std::vector<Span>                spans;
	int                              depth[2] = {0, 0};
	std::vector<std::vector<int>>    lane_events; // per lane: event indices, in time order
};

FrameView g_fv;
int       g_pick     = -1;   // index into the frames list; -1 = choose the default
double    g_view_c0  = 0.0;  // first visible cycle
double    g_view_len = 0.0;  // visible cycles; 0 = fit the frame
bool      g_dragging = false;
double    g_drag_c   = 0.0;
float     g_press_x  = 0.0f; // where the left button went down, for click vs. drag
double    g_sel_c0 = 0.0, g_sel_c1 = 0.0; // selection, cycles; empty when equal

// How the axis is labelled. Every tooltip shows all four regardless.
enum TimeUnit { TU_MS = 0, TU_LINES, TU_PERCENT, TU_CYCLES };
int g_unit = TU_MS;
const char *const UNIT_NAMES[] = {"Milliseconds", "Scanlines", "% of the frame", "CPU cycles"};

// ─── Building ───────────────────────────────────────────────────────────────

struct WalkCtx {
	const prof_report_t *r;
	FrameView           *fv;
	std::vector<int>     open[2]; // indices into spans, per thread
};

int
walk_parent(void *ctx, int node)
{
	const prof_report_t *r = static_cast<WalkCtx *>(ctx)->r;
	return (node >= 0 && node < r->node_count) ? r->nodes[node].parent : -1;
}

void
walk_emit(void *vctx, wf_out_kind_t kind, int thread, uint32_t cycle, int node, const wf_event_t *ev)
{
	(void)ev;
	WalkCtx *c = static_cast<WalkCtx *>(vctx);
	if (kind == WF_OUT_BEGIN) {
		Span s{cycle, cycle, node, thread, (int)c->open[thread].size()};
		c->open[thread].push_back((int)c->fv->spans.size());
		c->fv->spans.push_back(s);
		if (s.depth + 1 > c->fv->depth[thread])
			c->fv->depth[thread] = s.depth + 1;
	} else if (kind == WF_OUT_END && !c->open[thread].empty()) {
		c->fv->spans[(size_t)c->open[thread].back()].c1 = cycle;
		c->open[thread].pop_back();
	}
}

void
build(FrameView &fv, const prof_report_t *r, const wf_frame_t *f)
{
	fv.frame = f;
	fv.number = f->number;
	fv.spans.clear();
	fv.depth[0] = fv.depth[1] = 0;
	fv.lane_events.assign(WF_LANES, {});
	for (int i = 0; i < f->count; i++) {
		const int lane = wf_event_lane(&f->events[i]);
		if (lane != WF_LANE_CPU && lane < WF_LANES)
			fv.lane_events[(size_t)lane].push_back(i);
	}
	if (r->node_count > 2) {
		WalkCtx c{r, &fv, {}};
		wf_walk_frame(f, walk_parent, CCT_NODE_IRQ, walk_emit, &c);
	}
	// The roots ([main], [IRQ]) span everything and say nothing: drop them,
	// and lift everything else one row.
	std::vector<Span> kept;
	kept.reserve(fv.spans.size());
	fv.depth[0] = fv.depth[1] = 0;
	for (Span s : fv.spans) {
		if (s.depth == 0)
			continue;
		s.depth--;
		kept.push_back(s);
		if (s.depth + 1 > fv.depth[s.thread])
			fv.depth[s.thread] = s.depth + 1;
	}
	fv.spans.swap(kept);
	if (fv.depth[0] > MAX_CPU_ROWS)
		fv.depth[0] = MAX_CPU_ROWS;
	if (fv.depth[1] > MAX_IRQ_ROWS)
		fv.depth[1] = MAX_IRQ_ROWS;
}

// ─── Helpers ────────────────────────────────────────────────────────────────

ImU32
node_color(int node, float alpha = 1.0f)
{
	uint32_t h = (uint32_t)node * 2654435761u;
	const float hue = (float)(h >> 8 & 0xFFFF) / 65535.0f;
	return ImColor::HSV(hue, 0.45f, 0.80f, alpha);
}

ImU32
lane_color(int lane)
{
	return ImColor::HSV((float)(lane * 0.137f - (int)(lane * 0.137f)), 0.65f, 0.95f);
}

// What each row is, for its label's tooltip.
const char *
lane_help(int lane)
{
	switch (lane) {
		case WF_LANE_VERA_REG:   return "Writes to VERA's registers ($9F20-$9F3C): address setup, scroll,\nlayer and display settings, FX. Not the data ports: those bytes are\nsorted below by where in VRAM they landed.";
		case WF_LANE_PSG:        return "Writes to the PSG sound registers in VRAM ($1F9C0-$1F9FF).";
		case WF_LANE_PALETTE:    return "Writes to the palette in VRAM ($1FA00-$1FBFF).";
		case WF_LANE_SPRITES:    return "Writes to the sprite attribute table in VRAM ($1FC00-$1FFFF).";
		case WF_LANE_L0_MAP:     return "Writes to layer 0's tile map (which tile goes where).";
		case WF_LANE_L0_TILES:   return "Writes to layer 0's tile graphics, font, or bitmap.";
		case WF_LANE_L1_MAP:     return "Writes to layer 1's tile map (which tile goes where).";
		case WF_LANE_L1_TILES:   return "Writes to layer 1's tile graphics, font, or bitmap.";
		case WF_LANE_VRAM_OTHER: return "Writes to VRAM outside every layer's map and tile area, as the\nlayer registers stood at the time: sprite graphics, spare buffers.";
		case WF_LANE_PCM:        return "Bytes pushed into VERA's PCM audio buffer ($9F3D).";
		case WF_LANE_SPI:        return "SD card traffic through VERA's SPI port ($9F3E/$9F3F).";
		case WF_LANE_YM:         return "Writes to the YM2151 FM sound chip, labelled by register.";
		case WF_LANE_VIA1:       return "Writes to VIA 1 ($9F00): joystick/keyboard lines, I2C, timers.";
		case WF_LANE_VIA2:       return "Writes to VIA 2 ($9F10).";
		case WF_LANE_IO_OTHER:   return "Writes to the rest of the I/O page: emulator registers, MIDI,\nexpansion cards.";
		default:                 return "";
	}
}

void
goto_key(uint32_t key)
{
	uint8_t  region, bank;
	uint16_t pc;
	perf_hist_key_decode(key, &region, &bank, &pc);
	debug_ui_request_goto(pc, region == PH_REGION_LOW ? 0 : bank);
}

void
key_tooltip_lines(uint32_t key)
{
	char name[96], where[40], src[300];
	profile_session_key_name(key, name, sizeof name);
	profile_session_key_where(key, where, sizeof where);
	ImGui::Text("%s  (%s)", name, where);
	if (profile_session_key_source(key, src, sizeof src))
		ImGui::TextDisabled("%s", src);
}

// ─── Time ───────────────────────────────────────────────────────────────────
//
// A frame is a fixed span of emulated time: at 8 MHz, 134,400 CPU cycles =
// 16.8 ms = 525 VGA scanlines of 32 us. Cycles are what the recorder counts;
// these turn them into the units a person thinks in.

struct TimeBase {
	double cyc_per_us;   // the CPU clock in MHz
	double cyc_per_line; // this frame's length / 525
	double length;       // cycles in this frame
};

double
to_unit(const TimeBase &tb, int unit, double cyc)
{
	switch (unit) {
		case TU_MS:      return cyc / (tb.cyc_per_us * 1000.0);
		case TU_LINES:   return cyc / tb.cyc_per_line;
		case TU_PERCENT: return 100.0 * cyc / tb.length;
		default:         return cyc;
	}
}

double
from_unit(const TimeBase &tb, int unit, double v)
{
	return v / to_unit(tb, unit, 1.0);
}

// The 1-2-5 step at least `raw` long, so ticks land on round numbers.
double
nice_step(double raw)
{
	if (raw <= 0)
		return 1;
	const double p = std::pow(10.0, std::floor(std::log10(raw)));
	const double m = raw / p;
	return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}

// An axis label for `v` in `unit`, with as many decimals as the step needs.
void
axis_label(int unit, double v, double step, char *buf, size_t n)
{
	const int dec = step >= 1 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
	switch (unit) {
		case TU_MS:      snprintf(buf, n, "%.*f ms", dec, v); break;
		case TU_LINES:   snprintf(buf, n, "line %.0f", v); break;
		case TU_PERCENT: snprintf(buf, n, "%.*f%%", dec, v); break;
		default:
			if (step >= 1000)
				snprintf(buf, n, "%.*fk", step >= 10000 ? 0 : 1, v / 1000.0);
			else
				snprintf(buf, n, "%.0f", v);
			break;
	}
}

// A moment, in every unit: "4.210 ms, line 131.5, 25.0% of the frame, cycle 33,600".
void
moment(const TimeBase &tb, double cyc, char *buf, size_t n)
{
	snprintf(buf, n, "%.3f ms, line %.1f, %.1f%% of the frame, cycle %.0f",
	         to_unit(tb, TU_MS, cyc), to_unit(tb, TU_LINES, cyc), to_unit(tb, TU_PERCENT, cyc), cyc);
}

// A length of time, in every unit: "0.512 ms (16.0 lines, 3.0% of the frame, 4,096 cycles)".
void
span_len(const TimeBase &tb, double cyc, char *buf, size_t n)
{
	const double us = cyc / tb.cyc_per_us;
	if (us < 1000.0)
		snprintf(buf, n, "%.1f us (%.1f lines, %.1f%% of the frame, %.0f cycles)", us,
		         to_unit(tb, TU_LINES, cyc), to_unit(tb, TU_PERCENT, cyc), cyc);
	else
		snprintf(buf, n, "%.3f ms (%.1f lines, %.1f%% of the frame, %.0f cycles)", us / 1000.0,
		         to_unit(tb, TU_LINES, cyc), to_unit(tb, TU_PERCENT, cyc), cyc);
}

// ─── Range table ────────────────────────────────────────────────────────────

struct RangeAcc {
	const prof_report_t       *r;
	std::vector<uint64_t>      by_node; // cycles innermost
	uint32_t                   writes[WF_LANES] = {};
};

void
range_node(void *ctx, int node, uint32_t cycles)
{
	RangeAcc *a = static_cast<RangeAcc *>(ctx);
	if (node >= 0 && (size_t)node < a->by_node.size())
		a->by_node[(size_t)node] += cycles;
}

void
range_write(void *ctx, const wf_event_t *ev)
{
	RangeAcc *a = static_cast<RangeAcc *>(ctx);
	const int lane = wf_event_lane(ev);
	if (lane >= 0 && lane < WF_LANES)
		a->writes[lane]++;
}

void
draw_selection(const prof_report_t *r, const wf_frame_t *f, const TimeBase &tb)
{
	const uint32_t c0 = (uint32_t)std::max(0.0, std::min(g_sel_c0, g_sel_c1));
	const uint32_t c1 = (uint32_t)std::min((double)f->length, std::max(g_sel_c0, g_sel_c1));
	if (c1 <= c0)
		return;
	RangeAcc a{r, std::vector<uint64_t>((size_t)(r->node_count > 0 ? r->node_count : 1), 0)};
	wf_frame_range(f, c0, c1, range_node, range_write, &a);

	char from[96], len[96];
	snprintf(from, sizeof from, "%.3f ms (line %.0f)", to_unit(tb, TU_MS, c0), to_unit(tb, TU_LINES, c0));
	span_len(tb, c1 - c0, len, sizeof len);
	ImGui::Text("Selection from %s, lasting %s", from, len);
	ImGui::SameLine();
	if (ImGui::SmallButton("Zoom to selection")) {
		g_view_c0  = c0;
		g_view_len = c1 - c0;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Clear"))
		g_sel_c0 = g_sel_c1 = 0;

	// Routines by cycles innermost in the range: what the CPU was doing.
	std::vector<int> order;
	for (int i = 0; i < (int)a.by_node.size(); i++)
		if (a.by_node[(size_t)i])
			order.push_back(i);
	std::sort(order.begin(), order.end(),
	          [&](int x, int y) { return a.by_node[(size_t)x] > a.by_node[(size_t)y]; });
	if (ImGui::BeginTable("wfsel", 3,
	                      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
	                          ImGuiTableFlags_ScrollY | DBGUI_TABLE_FLAGS_RESIZABLE,
	                      ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Cycles", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("Cycles", "9999999"));
		ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, dbgui_col_width("%", "100.0"));
		ImGui::TableSetupColumn("Routine (innermost)", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		for (int n : order) {
			char name[PROF_NAME_MAX + 16];
			profile_session_node_name(r, n, name, sizeof name);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%llu", (unsigned long long)a.by_node[(size_t)n]);
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.1f", 100.0 * (double)a.by_node[(size_t)n] / (double)(c1 - c0));
			ImGui::TableSetColumnIndex(2);
			ImGui::PushID(n);
			if (ImGui::Selectable(name, false, ImGuiSelectableFlags_SpanAllColumns) &&
			    r->nodes[n].parent >= 0)
				goto_key(r->nodes[n].key);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::TextDisabled("Writes in range:");
	bool any = false;
	for (int l = 1; l < WF_LANES; l++)
		if (a.writes[l]) {
			ImGui::SameLine();
			ImGui::TextColored(ImColor(lane_color(l)), "%s %u", wf_lane_name(l), a.writes[l]);
			any = true;
		}
	if (!any) {
		ImGui::SameLine();
		ImGui::TextDisabled("none");
	}
}

} // namespace

// ─── The tab ────────────────────────────────────────────────────────────────

void
profile_waterfall_draw(const prof_report_t *r)
{
	if (!r || !profile_session_has_waterfall()) {
		ImGui::TextDisabled("No waterfall in this capture. Tick \"Waterfall\" before starting one.");
		return;
	}
	const wf_frame_t *frames[MAX_FRAMES];
	bool              worst[MAX_FRAMES];
	const int         nf = profile_session_waterfall_frames(frames, worst, MAX_FRAMES);
	if (!nf) {
		ImGui::TextDisabled("The capture kept no frames in detail.");
		return;
	}

	// A new report: pick its worst frame (or its last), and fit it.
	const uint32_t gen = profile_session_report_generation();
	if (gen != g_fv.gen) {
		g_fv.gen = gen;
		g_fv.number = ~0u;
		g_pick = -1;
	}
	if (g_pick < 0 || g_pick >= nf) {
		g_pick = nf - 1;
		uint64_t best = 0;
		for (int i = 0; i < nf; i++)
			if (worst[i] && frames[i]->score >= best) {
				best   = frames[i]->score;
				g_pick = i;
			}
		g_view_len = 0;
		g_sel_c0 = g_sel_c1 = 0;
	}

	// Frame picker.
	char label[96];
	auto describe = [&](int i, char *buf, size_t n) {
		snprintf(buf, n, "frame %u  %d events%s%s", frames[i]->number, frames[i]->count,
		         worst[i] ? "  (worst)" : "", frames[i]->truncated ? "  TRUNCATED" : "");
	};
	describe(g_pick, label, sizeof label);
	ImGui::SetNextItemWidth(dbgui_combo_width("frame 99999  99999 events  (worst)  TRUNCATED"));
	if (ImGui::BeginCombo("##wfframe", label)) {
		for (int i = 0; i < nf; i++) {
			char item[96];
			describe(i, item, sizeof item);
			if (ImGui::Selectable(item, i == g_pick)) {
				g_pick     = i;
				g_view_len = 0;
				g_sel_c0 = g_sel_c1 = 0;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("<") && g_pick > 0) {
		g_pick--;
		g_sel_c0 = g_sel_c1 = 0;
	}
	ImGui::SameLine();
	if (ImGui::Button(">") && g_pick < nf - 1) {
		g_pick++;
		g_sel_c0 = g_sel_c1 = 0;
	}
	ImGui::SameLine();
	if (ImGui::Button("Fit"))
		g_view_len = 0;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_combo_width("% of the frame "));
	ImGui::Combo("##wfunit", &g_unit, UNIT_NAMES, IM_ARRAYSIZE(UNIT_NAMES));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("What the time axis counts in. Tooltips always show every unit.\n"
		                  "A frame lasts about 16.7 ms: 525 scanlines of 32 us each,\n"
		                  "or 134,400 CPU cycles at 8 MHz.");
	ImGui::SameLine();
	ImGui::TextDisabled("Mouse wheel: zoom.  Right-drag: pan.  Drag: select a stretch.  Click: open the code.");

	const wf_frame_t *f = frames[g_pick];
	if (g_fv.number != f->number || g_fv.frame != f)
		build(g_fv, r, f);
	if (f->truncated)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
		                   "This frame overflowed its buffer: events after the first %d are missing.",
		                   f->count);

	const double length = f->length ? (double)f->length : 1.0;
	if (g_view_len <= 0 || g_view_len > length) {
		g_view_c0  = 0;
		g_view_len = length;
	}
	const double   cyc_per_line = length / LINES_PER_FRAME;
	const TimeBase tb{profile_session_mhz(), cyc_per_line, length};

	// Rows: CPU depths, IRQ depths, then each lane that has writes.
	std::vector<int> lanes;
	for (int l = 1; l < WF_LANES; l++)
		if (!g_fv.lane_events[(size_t)l].empty())
			lanes.push_back(l);
	const int   cpu_rows = std::max(1, g_fv.depth[0]);
	const int   irq_rows = g_fv.depth[1];
	const float row_h    = ImGui::GetTextLineHeight() + 4.0f;
	const float axis_h   = ImGui::GetTextLineHeight() + 6.0f;
	const float label_w  = dbgui_text_width("VRAM other ") + 8.0f;
	const int   rows     = cpu_rows + irq_rows + (int)lanes.size();

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  width  = std::max(100.0f, ImGui::GetContentRegionAvail().x);
	const float  height = axis_h + rows * row_h;
	ImGui::InvisibleButton("##wfcanvas", ImVec2(width, height),
	                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool   hovered = ImGui::IsItemHovered();
	ImDrawList  *dl      = ImGui::GetWindowDrawList();
	const float  x0      = origin.x + label_w;
	const float  plot_w  = width - label_w;
	const double sx      = plot_w / g_view_len;
	auto x_of = [&](double c) { return (float)(x0 + (c - g_view_c0) * sx); };
	auto c_of = [&](float x) { return g_view_c0 + (x - x0) / sx; };
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const double mouse_c = c_of(mouse.x);

	// Zoom about the cursor; pan with the right button.
	if (hovered && ImGui::GetIO().MouseWheel != 0.0f && mouse.x >= x0) {
		const double factor = ImGui::GetIO().MouseWheel > 0 ? 0.8 : 1.25;
		const double nl     = std::max(32.0, std::min(length, g_view_len * factor));
		g_view_c0           = mouse_c - (mouse_c - g_view_c0) * (nl / g_view_len);
		g_view_len          = nl;
	}
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
		g_view_c0 -= ImGui::GetIO().MouseDelta.x / sx;
	g_view_c0 = std::max(0.0, std::min(g_view_c0, length - g_view_len));

	// Select by dragging with the left button; a click without a drag opens.
	bool clicked = false;
	if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= x0) {
		g_dragging = true;
		g_drag_c   = mouse_c;
		g_press_x  = mouse.x;
	}
	if (g_dragging) {
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
			g_sel_c0 = g_drag_c;
			g_sel_c1 = mouse_c;
		}
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			if (std::fabs(mouse.x - g_press_x) < 3.0f)
				clicked = true;
			g_dragging = false;
		}
	}

	// Background, axis, row labels.
	dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
	dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(origin.x + width, origin.y + height),
	                  ImGui::GetColorU32(ImGuiCol_FrameBg));
	{
		const float  min_px = dbgui_text_width("88.888 ms") + 12.0f;
		double       step   = nice_step(to_unit(tb, g_unit, min_px / sx));
		if (g_unit == TU_LINES && step < 1)
			step = 1;
		const double v_end = to_unit(tb, g_unit, g_view_c0 + g_view_len);
		for (double v = std::floor(to_unit(tb, g_unit, g_view_c0) / step) * step; v <= v_end + step * 0.001;
		     v += step) {
			const float x = x_of(from_unit(tb, g_unit, v));
			if (x < x0 - 0.5f)
				continue;
			dl->AddLine(ImVec2(x, origin.y + axis_h - 4), ImVec2(x, origin.y + height),
			            ImGui::GetColorU32(ImGuiCol_Separator, 0.5f));
			char t[24];
			axis_label(g_unit, v, step, t, sizeof t);
			dl->AddText(ImVec2(x + 2, origin.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
		}
	}
	bool vblank_tip = false;
	// Where the picture ends: VGA draws 480 visible lines, then the beam is in
	// the vertical blank until the frame wraps. Work done after this line is
	// done while nothing is being drawn.
	{
		const float xv = x_of(480 * cyc_per_line);
		if (xv >= x0 && xv <= origin.x + width) {
			dl->AddLine(ImVec2(xv, origin.y + axis_h - 4), ImVec2(xv, origin.y + height),
			            IM_COL32(255, 200, 80, 140), 1.5f);
			dl->AddText(ImVec2(xv + 3, origin.y + axis_h + 1), IM_COL32(255, 200, 80, 200), "vblank");
		}
		vblank_tip = hovered && std::fabs(mouse.x - xv) < 4.0f && mouse.y < origin.y + axis_h + row_h;
		if (vblank_tip)
			ImGui::SetTooltip("Line 480: the last visible line of the picture. From here to the end\n"
			                  "of the frame the screen is in vertical blank, the usual time to\n"
			                  "update VRAM without tearing.");
	}
	auto row_y = [&](int row) { return origin.y + axis_h + row * row_h; };
	for (int i = 0; i < rows; i++) {
		const char *name = i < cpu_rows ? (i == 0 ? "CPU" : "")
		                 : i < cpu_rows + irq_rows ? (i == cpu_rows ? "IRQ" : "")
		                 : wf_lane_name(lanes[(size_t)(i - cpu_rows - irq_rows)]);
		dl->AddText(ImVec2(origin.x + 2, row_y(i) + 2), ImGui::GetColorU32(ImGuiCol_Text), name);
		if (i == cpu_rows || i == cpu_rows + irq_rows)
			dl->AddLine(ImVec2(origin.x, row_y(i)), ImVec2(origin.x + width, row_y(i)),
			            ImGui::GetColorU32(ImGuiCol_Separator));
	}

	// Spans.
	dl->PushClipRect(ImVec2(x0, origin.y), ImVec2(origin.x + width, origin.y + height), true);
	const Span *hover_span = nullptr;
	for (const Span &s : g_fv.spans) {
		const int max_d = s.thread ? irq_rows : cpu_rows;
		if (s.depth >= max_d)
			continue;
		const float a = x_of(s.c0), b = x_of(s.c1);
		if (b < x0 || a > origin.x + width)
			continue;
		const int   row = (s.thread ? cpu_rows : 0) + s.depth;
		const float y   = row_y(row);
		const float bx  = std::max(b, a + 1.0f);
		dl->AddRectFilled(ImVec2(a, y + 1), ImVec2(bx, y + row_h - 1), node_color(s.node));
		if (bx - a > 24.0f) {
			char name[PROF_NAME_MAX + 16];
			profile_session_node_name(r, s.node, name, sizeof name);
			const float tx = std::max(a, x0) + 3.0f;
			dl->PushClipRect(ImVec2(a, y), ImVec2(bx, y + row_h), true);
			dl->AddText(ImVec2(tx, y + 2), IM_COL32(20, 20, 20, 255), name);
			dl->PopClipRect();
		}
		if (hovered && mouse.y >= y && mouse.y < y + row_h && mouse.x >= a && mouse.x < bx)
			hover_span = &s;
	}

	// Ticks, at most one per pixel column per lane.
	const wf_event_t *hover_ev = nullptr;
	float             hover_dx = 4.0f;
	for (size_t li = 0; li < lanes.size(); li++) {
		const int   lane = lanes[li];
		const float y    = row_y(cpu_rows + irq_rows + (int)li);
		const ImU32 col  = lane_color(lane);
		int         last_px = INT_MIN; // no column drawn yet
		const auto &ev   = g_fv.lane_events[(size_t)lane];
		// First visible event, by binary search on cycle.
		size_t lo = 0, hi = ev.size();
		while (lo < hi) {
			const size_t m = (lo + hi) / 2;
			if (f->events[ev[m]].cycle < g_view_c0) lo = m + 1; else hi = m;
		}
		for (size_t k = lo; k < ev.size(); k++) {
			const wf_event_t *e = &f->events[ev[k]];
			const float       x = x_of(e->cycle);
			if (x > origin.x + width)
				break;
			const int px = (int)x;
			if (px != last_px) {
				dl->AddLine(ImVec2(x, y + 2), ImVec2(x, y + row_h - 2), col);
				last_px = px;
			}
			if (hovered && mouse.y >= y && mouse.y < y + row_h && std::abs(mouse.x - x) < hover_dx) {
				hover_dx = std::abs(mouse.x - x);
				hover_ev = e;
			}
		}
	}

	// Selection overlay.
	if (g_sel_c0 != g_sel_c1) {
		const float a = x_of(std::min(g_sel_c0, g_sel_c1)), b = x_of(std::max(g_sel_c0, g_sel_c1));
		dl->AddRectFilled(ImVec2(a, origin.y), ImVec2(b, origin.y + height), IM_COL32(120, 170, 255, 50));
		dl->AddRect(ImVec2(a, origin.y), ImVec2(b, origin.y + height), IM_COL32(120, 170, 255, 160));
	}
	if (hovered && mouse.x >= x0)
		dl->AddLine(ImVec2(mouse.x, origin.y), ImVec2(mouse.x, origin.y + height),
		            ImGui::GetColorU32(ImGuiCol_Text, 0.35f));
	dl->PopClipRect();
	dl->PopClipRect();

	// Row labels explain their row.
	if (hovered && mouse.x < x0 && mouse.y >= row_y(0)) {
		const int row = (int)((mouse.y - row_y(0)) / row_h);
		if (row >= 0 && row < cpu_rows)
			ImGui::SetTooltip("Which routine the CPU was running. A routine called from another\n"
			                  "is drawn one row below its caller, under the stretch of time it\n"
			                  "ran in.");
		else if (row >= cpu_rows && row < cpu_rows + irq_rows)
			ImGui::SetTooltip("Interrupt handlers (IRQ): code the hardware ran in between, while\n"
			                  "the routine above it was paused.");
		else if (row >= cpu_rows + irq_rows && row < rows)
			ImGui::SetTooltip("%s\nEach tick is one write.", lane_help(lanes[(size_t)(row - cpu_rows - irq_rows)]));
	}

	// Hover and click.
	if (hovered && !g_dragging && mouse.x >= x0) {
		if (hover_ev) {
			const int lane = wf_event_lane(hover_ev);
			ImGui::BeginTooltip();
			if (lane >= WF_LANE_PSG && lane <= WF_LANE_VRAM_OTHER)
				ImGui::Text("%s write: VRAM $%X%04X = $%02X%s", wf_lane_name(lane),
				            (hover_ev->flags & WF_F_VRAM_HI) ? 1u : 0u, (unsigned)hover_ev->target,
				            (unsigned)hover_ev->value, (hover_ev->flags & WF_F_FX4) ? " (FX, 4 bytes)" : "");
			else
				ImGui::Text("%s write: reg $%02X = $%02X", wf_lane_name(lane), (unsigned)hover_ev->target,
				            (unsigned)hover_ev->value);
			char when[128];
			moment(tb, hover_ev->cycle, when, sizeof when);
			ImGui::TextDisabled("At %s%s", when,
			                    (hover_ev->flags & WF_F_IRQ) ? "; in an interrupt handler" : "");
			key_tooltip_lines(wf_event_key(hover_ev));
			ImGui::TextDisabled("Click to open the writing instruction.");
			ImGui::EndTooltip();
			if (clicked)
				goto_key(wf_event_key(hover_ev));
		} else if (hover_span) {
			const prof_node_t &n = r->nodes[hover_span->node];
			char name[PROF_NAME_MAX + 16];
			profile_session_node_name(r, hover_span->node, name, sizeof name);
			ImGui::BeginTooltip();
			ImGui::Text("%s", name);
			char when[128], len[96];
			moment(tb, hover_span->c0, when, sizeof when);
			span_len(tb, hover_span->c1 - hover_span->c0, len, sizeof len);
			ImGui::TextDisabled("Ran for %s", len);
			ImGui::TextDisabled("starting at %s", when);
			if (n.key < PH_KEYS)
				key_tooltip_lines(n.key);
			ImGui::TextDisabled("Over the whole capture: %u calls, worst %u cycles.", n.calls, n.call_max);
			ImGui::EndTooltip();
			if (clicked && n.key < PH_KEYS)
				goto_key(n.key);
		} else if (mouse.x >= x0 && !vblank_tip) {
			char when[128];
			moment(tb, mouse_c, when, sizeof when);
			ImGui::SetTooltip("%s", when);
		}
	}

	{
		char len[96];
		span_len(tb, length, len, sizeof len);
		ImGui::TextDisabled("This frame lasted %s. %d events, %d devices written. %d frames kept in detail.",
		                    len, f->count, (int)lanes.size(), nf);
	}
	if (g_sel_c0 != g_sel_c1)
		draw_selection(r, f, tb);
}
