// Commander X16 Emulator — ImGui "Performance" panel.
//
// Answers the question a program author actually has: at 60 fps (or 30, or
// whatever they are targeting) how much of the frame is their code using, and
// how much room is left?
//
// The accounting is not done here. src/perf_budget.c owns the cycle budget and
// src/vera_bandwidth.c owns the VRAM bus figures, because the same numbers are
// served over DAP and neither surface should have its own idea of what a frame
// cost. This file is a reader and a control panel.
//
// CPU CYCLES AND VERA BANDWIDTH SHARE THIS PANEL on purpose. They are two
// halves of "what is eating my frame?", and the frames worth looking at are the
// ones where they disagree.
//
// PROFILING IS ARMED BY OPENING THE PANEL, and disarmed by closing it, in the
// same spirit as the audio scope rings that self-arm on read. It is the one
// debugger feature besides the I/O trace that costs the running machine
// anything per instruction, so it should not be running when nobody is looking
// at it. Settings > "keep profiling when the panel is closed" overrides this,
// for the case where a DAP client is being fed and the panel is shut.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_widgets.h"
#include "debug_ui_settings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace {

// The windows the statistics table reports, matching what the DAP surface
// offers so that the panel and a client never disagree about what "30s" means.
struct Window {
	const char *name;
	float       seconds; // <= 0 means the whole ring
};
const Window k_windows[] = {
	{"1s", 1.0f}, {"10s", 10.0f}, {"30s", 30.0f}, {"60s", 60.0f}, {"session", 0.0f},
};
const int k_window_count = (int)(sizeof k_windows / sizeof k_windows[0]);

// Graph history, refilled each frame from the ring.
float  g_graph[1024];
int    g_graph_n = 0;

// Zone editor scratch.
char     g_zone_name[PERF_ZONE_NAME_MAX] = "";
int      g_zone_start                    = 0x0800;
int      g_zone_end                      = 0x0900;
int      g_zone_bank                     = -1;
bool     g_zone_idle                     = false;
int      g_symbol_pick                   = -1;

ImVec4
utilization_color(float pct)
{
	if (pct >= 100.0f)
		return ImVec4(1.00f, 0.35f, 0.35f, 1.0f); // over budget
	if (pct >= 90.0f)
		return ImVec4(1.00f, 0.75f, 0.30f, 1.0f); // no headroom left
	return ImVec4(0.45f, 0.90f, 0.50f, 1.0f);
}

// Cycles as a share of a scanline, which is the unit X16 programs are written
// in far more often than raw cycles.
void
cycles_tooltip(uint32_t cycles)
{
	if (!ImGui::IsItemHovered())
		return;
	const uint32_t per_line = perf_budget_cycles_per_scanline();
	ImGui::BeginTooltip();
	ImGui::Text("%u cycles", (unsigned)cycles);
	if (per_line)
		ImGui::Text("%.1f scanlines", (double)cycles / (double)per_line);
	const uint32_t khz = (uint32_t)MHZ * 1000u;
	if (khz)
		ImGui::Text("%.3f ms", (double)cycles / (double)khz);
	ImGui::EndTooltip();
}

void
draw_budget_header(void)
{
	DebugUiSettings &s = debug_ui_settings();

	const uint32_t per_frame = perf_budget_cycles_per_frame();
	const uint32_t budget    = perf_budget_budget_cycles();
	const int      span      = perf_budget_frames_per_budget();

	ImGui::SetNextItemWidth(dbgui_field_width("000.0"));
	if (ImGui::InputFloat("Target FPS", &s.perf_target_fps, 0.0f, 0.0f, "%.4g")) {
		if (s.perf_target_fps < 1.0f)
			s.perf_target_fps = 1.0f;
		if (s.perf_target_fps > 1000.0f)
			s.perf_target_fps = 1000.0f;
		perf_budget_set_target_fps(s.perf_target_fps);
		debug_ui_settings_mark_dirty();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
		    "How many vsyncs your frame work is allowed to span.\n\n"
		    "The frame period is fixed by VERA and asking for a lower rate\n"
		    "does not make frames longer -- it means the work may take more\n"
		    "than one of them, so the budget grows instead.");
	}
	ImGui::SameLine();
	for (int i = 0; i < 3; i++) {
		const float presets[3] = {60.0f, 30.0f, 20.0f};
		char        label[16];
		snprintf(label, sizeof label, "%g", (double)presets[i]);
		if (i)
			ImGui::SameLine();
		if (ImGui::SmallButton(label)) {
			s.perf_target_fps = presets[i];
			perf_budget_set_target_fps(presets[i]);
			debug_ui_settings_mark_dirty();
		}
	}

	ImGui::Text("Machine %u MHz  |  vsync %.2f Hz  |  %u cycles/frame  |  %u/scanline",
	            (unsigned)MHZ, (double)perf_budget_vsync_hz(), (unsigned)per_frame,
	            (unsigned)perf_budget_cycles_per_scanline());
	ImGui::Text("Budget: %u cycles", (unsigned)budget);
	cycles_tooltip(budget);
	if (span > 1) {
		ImGui::SameLine();
		ImGui::TextDisabled("(%d frames' worth)", span);
	}

	if (warp_mode) {
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
		                   "Warp mode: guest cycles are still real, host time is not.");
	}
	if (timing_get_speed_khz() != timing_native_khz()) {
		ImGui::TextDisabled("Speed override active (%d kHz). The budget follows the "
		                    "machine's native %d kHz.",
		                    timing_get_speed_khz(), timing_native_khz());
	}
}

void
draw_current_frame(void)
{
	perf_frame_t f;
	if (!perf_budget_last_frame(&f)) {
		ImGui::TextDisabled("(waiting for the first frame)");
		return;
	}

	const uint32_t budget = perf_budget_budget_cycles();
	const uint32_t work   = f.period_work;
	const float    pct    = budget ? 100.0f * (float)work / (float)budget : 0.0f;

	// A stacked bar: work (of which interrupt) against idle, drawn to the width
	// of the BUDGET rather than the frame, so overrunning visibly runs past the
	// end instead of quietly rescaling.
	const float full   = ImGui::GetContentRegionAvail().x;
	const float height = ImGui::GetFrameHeight();
	const ImVec2 p0    = ImGui::GetCursorScreenPos();
	ImDrawList  *dl    = ImGui::GetWindowDrawList();

	const float scale   = budget ? full / (float)budget : 0.0f;
	const float w_irq   = (float)f.irq * scale;
	const float w_work  = (float)work * scale;
	const float w_total = (float)f.total * scale;

	dl->AddRectFilled(p0, ImVec2(p0.x + full, p0.y + height),
	                  ImGui::GetColorU32(ImGuiCol_FrameBg));
	// Idle first, as the backdrop the work sits on.
	dl->AddRectFilled(p0, ImVec2(p0.x + (w_total < full ? w_total : full), p0.y + height),
	                  ImGui::GetColorU32(ImVec4(0.25f, 0.30f, 0.38f, 1.0f)));
	dl->AddRectFilled(p0, ImVec2(p0.x + (w_work < full ? w_work : full), p0.y + height),
	                  ImGui::GetColorU32(utilization_color(pct)));
	dl->AddRectFilled(p0, ImVec2(p0.x + (w_irq < full ? w_irq : full), p0.y + height),
	                  ImGui::GetColorU32(ImVec4(0.55f, 0.45f, 0.85f, 1.0f)));
	// The budget line, so "how close am I" is readable at a glance.
	dl->AddLine(ImVec2(p0.x + full - 1.0f, p0.y), ImVec2(p0.x + full - 1.0f, p0.y + height),
	            ImGui::GetColorU32(ImVec4(1, 1, 1, 0.7f)), 2.0f);

	char overlay[96];
	snprintf(overlay, sizeof overlay, "%.1f%% of budget   %u / %u cycles", (double)pct,
	         (unsigned)work, (unsigned)budget);
	const ImVec2 ts = ImGui::CalcTextSize(overlay);
	dl->AddText(ImVec2(p0.x + (full - ts.x) * 0.5f, p0.y + (height - ts.y) * 0.5f),
	            ImGui::GetColorU32(ImGuiCol_Text), overlay);
	ImGui::Dummy(ImVec2(full, height));

	const uint32_t idle    = f.idle;
	const uint32_t main_cy = work > f.irq ? work - f.irq : 0;
	const uint32_t per_line = perf_budget_cycles_per_scanline();

	ImGui::Text("work %u", (unsigned)work);
	cycles_tooltip(work);
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(0.55f, 0.45f, 0.85f, 1.0f), " | irq %u", (unsigned)f.irq);
	cycles_tooltip(f.irq);
	ImGui::SameLine();
	ImGui::Text(" | main %u", (unsigned)main_cy);
	cycles_tooltip(main_cy);
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(0.55f, 0.62f, 0.72f, 1.0f), " | idle %u", (unsigned)idle);
	cycles_tooltip(idle);

	if (work < budget) {
		const uint32_t spare = budget - work;
		ImGui::TextColored(utilization_color(pct), "Headroom: %u cycles%s", (unsigned)spare,
		                   per_line ? "" : "");
		if (per_line) {
			ImGui::SameLine();
			ImGui::TextDisabled("(%.1f scanlines)", (double)spare / (double)per_line);
		}
	} else {
		ImGui::TextColored(utilization_color(pct), "OVER BUDGET by %u cycles",
		                   (unsigned)(work - budget));
		if (per_line) {
			ImGui::SameLine();
			ImGui::TextDisabled("(%.1f scanlines)",
			                    (double)(work - budget) / (double)per_line);
		}
	}
	ImGui::Text("Instructions: %u   |   host %.2f ms", (unsigned)f.instructions,
	            (double)f.host_us / 1000.0);
}

void
draw_graph(void)
{
	DebugUiSettings &s = debug_ui_settings();

	int want = s.perf_graph_frames;
	if (want < 30)
		want = 30;
	if (want > (int)(sizeof g_graph / sizeof g_graph[0]))
		want = (int)(sizeof g_graph / sizeof g_graph[0]);

	// perf_budget_frames() would be a second copy of the ring; the panel only
	// needs the work figure, so it walks the window API instead.
	const int have = perf_budget_frame_count();
	int       n    = want < have ? want : have;
	if (n <= 0) {
		ImGui::TextDisabled("(no frames yet)");
		return;
	}

	perf_frame_t f;
	g_graph_n = 0;
	// Oldest to newest, so the graph reads left-to-right like a timeline.
	for (int i = n - 1; i >= 0; i--) {
		if (perf_budget_frame_at(i, &f))
			g_graph[g_graph_n++] = (float)f.period_work;
	}
	if (g_graph_n <= 0) {
		ImGui::TextDisabled("(no frames yet)");
		return;
	}

	const float budget = (float)perf_budget_budget_cycles();
	float       top    = budget * 1.25f;
	for (int i = 0; i < g_graph_n; i++)
		if (g_graph[i] > top)
			top = g_graph[i] * 1.05f;

	const float  height = ImGui::GetTextLineHeight() * 6.0f;
	const ImVec2 p0     = ImGui::GetCursorScreenPos();
	const float  width  = ImGui::GetContentRegionAvail().x;

	ImGui::PlotHistogram("##perfgraph", g_graph, g_graph_n, 0, nullptr, 0.0f, top,
	                     ImVec2(width, height));

	// The budget line over the top: without it the histogram says how much the
	// work varies but not whether any of it mattered.
	if (top > 0.0f) {
		const float y = p0.y + height * (1.0f - budget / top);
		ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + width, y),
		                                    ImGui::GetColorU32(ImVec4(1.0f, 0.4f, 0.4f, 0.9f)),
		                                    1.5f);
	}
	ImGui::TextDisabled("%d frames, oldest left. Red line is the budget.", g_graph_n);
}

void
stat_cell(uint32_t v, uint32_t budget)
{
	const float pct = budget ? 100.0f * (float)v / (float)budget : 0.0f;
	ImGui::TextColored(utilization_color(pct), "%u", (unsigned)v);
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::Text("%u cycles (%.1f%% of budget)", (unsigned)v, (double)pct);
		const uint32_t per_line = perf_budget_cycles_per_scanline();
		if (per_line)
			ImGui::Text("%.1f scanlines", (double)v / (double)per_line);
		ImGui::EndTooltip();
	}
}

void
draw_statistics(void)
{
	const uint32_t budget = perf_budget_budget_cycles();

	if (!ImGui::BeginTable("perfstats", 9,
	                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
	                           DBGUI_TABLE_FLAGS_RESIZABLE))
		return;

	ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Window", "session"));
	ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Frames", "99999"));
	ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Mean", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("p50", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("p95", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("p99", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Over", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Over", "9999 (99%)"));
	ImGui::TableHeadersRow();

	for (int i = 0; i < k_window_count; i++) {
		perf_window_t w;
		if (!perf_budget_window(k_windows[i].seconds, &w))
			continue;

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(k_windows[i].name);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%d", w.frames);

		ImGui::TableSetColumnIndex(2); stat_cell(w.budget_work.min, budget);
		ImGui::TableSetColumnIndex(3); stat_cell(w.budget_work.mean, budget);
		ImGui::TableSetColumnIndex(4); stat_cell(w.budget_work.p50, budget);
		ImGui::TableSetColumnIndex(5); stat_cell(w.budget_work.p95, budget);
		ImGui::TableSetColumnIndex(6); stat_cell(w.budget_work.p99, budget);
		ImGui::TableSetColumnIndex(7); stat_cell(w.budget_work.max, budget);

		ImGui::TableSetColumnIndex(8);
		if (w.overruns > 0) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%d (%.1f%%)", w.overruns,
			                   (double)w.overrun_pct);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Worst was frame %u, %u cycles over budget.",
				                  (unsigned)w.worst_frame, (unsigned)w.worst_overrun);
		} else {
			ImGui::TextDisabled("-");
		}
	}
	ImGui::EndTable();
	ImGui::TextDisabled("Work per budget period, in guest cycles. "
	                    "Colour is share of budget.");
}

void
draw_breakdown(void)
{
	const uint32_t budget = perf_budget_budget_cycles();
	const int      nzones = perf_budget_zone_count();
	if (nzones <= 0) {
		ImGui::TextDisabled("No zones or marker regions yet. Define a zone below, or have "
		                    "the guest write region markers to $9FBC.");
		return;
	}

	if (!ImGui::BeginTable("perfzones", 6,
	                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
	                           DBGUI_TABLE_FLAGS_RESIZABLE))
		return;
	ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("Range", "$FFFF-$FFFF"));
	ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("Mean 30s", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("p95 30s", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
	                        dbgui_col_width("", "Remove"));
	ImGui::TableHeadersRow();

	perf_frame_t last;
	const bool   have_last = perf_budget_last_frame(&last);

	for (int i = 0; i < nzones; i++) {
		perf_zone_t z;
		if (!perf_budget_zone_get(i, &z))
			continue;
		ImGui::TableNextRow();
		ImGui::PushID(i);

		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(z.name);
		if (z.is_idle) {
			ImGui::SameLine();
			ImGui::TextDisabled("(idle)");
		}

		ImGui::TableSetColumnIndex(1);
		if (z.is_marker)
			ImGui::TextDisabled("marker");
		else if (z.bank >= 0)
			ImGui::Text("$%04X-$%04X b%d", z.start, z.end, z.bank);
		else
			ImGui::Text("$%04X-$%04X", z.start, z.end);

		ImGui::TableSetColumnIndex(2);
		if (have_last && i < PERF_RING_ZONES)
			stat_cell(last.zone[i], budget);
		else
			ImGui::TextDisabled("-");

		perf_stat_t st;
		const bool  have_stat = perf_budget_zone_window(i, 30.0f, &st);
		ImGui::TableSetColumnIndex(3);
		if (have_stat)
			stat_cell(st.mean, budget);
		else
			ImGui::TextDisabled("-");
		ImGui::TableSetColumnIndex(4);
		if (have_stat)
			stat_cell(st.p95, budget);
		else
			ImGui::TextDisabled("-");

		ImGui::TableSetColumnIndex(5);
		if (ImGui::SmallButton("Remove"))
			perf_budget_zone_remove(i);

		ImGui::PopID();
	}
	ImGui::EndTable();

	if (nzones > PERF_RING_ZONES) {
		ImGui::TextDisabled("Only the first %d zones keep per-frame history; the rest "
		                    "report live figures only.", PERF_RING_ZONES);
	}
}

void
draw_zone_editor(void)
{
	ImGui::SetNextItemWidth(dbgui_field_width("a_reasonably_long_name"));
	ImGui::InputText("Name", g_zone_name, sizeof g_zone_name);

	ImGui::SetNextItemWidth(dbgui_field_width("$FFFF"));
	ImGui::InputInt("Start", &g_zone_start, 0, 0, ImGuiInputTextFlags_CharsHexadecimal);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_field_width("$FFFF"));
	ImGui::InputInt("End", &g_zone_end, 0, 0, ImGuiInputTextFlags_CharsHexadecimal);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_field_width("-1"));
	ImGui::InputInt("Bank", &g_zone_bank, 0, 0);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("-1 matches any bank.");
	ImGui::SameLine();
	ImGui::Checkbox("Idle", &g_zone_idle);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Cycles in this range are WAITING, not work.\n"
		                  "Use this for a wait loop the automatic detector cannot see --\n"
		                  "one that stores to memory, or calls a subroutine.");
	}

	// Picking a zone straight off a symbol is the reason to bother with a zone
	// editor at all: nobody knows the address range of sprite_update by heart,
	// and the .dbg already does.
	if (dbg_info_is_loaded()) {
		const int nsym = dbg_info_symbol_count();
		if (ImGui::Button("From symbol...")) {
			ImGui::OpenPopup("perf_symbol_pick");
			g_symbol_pick = -1;
		}
		if (ImGui::BeginPopup("perf_symbol_pick")) {
			static char filter[64] = "";
			ImGui::SetNextItemWidth(dbgui_field_width("a_reasonably_long_name"));
			ImGui::InputText("Filter", filter, sizeof filter);
			ImGui::BeginChild("perf_symbol_list", ImVec2(320, 240));
			for (int i = 0; i < nsym; i++) {
				const char *name = nullptr;
				dbg_addr_t  addr = 0;
				if (!dbg_info_symbol_at(i, &name, &addr) || !name)
					continue;
				if (filter[0] && !strstr(name, filter))
					continue;
				char label[96];
				snprintf(label, sizeof label, "%s  $%04X", name, (unsigned)(addr & 0xFFFF));
				if (ImGui::Selectable(label)) {
					// A symbol has no recorded size, so the zone runs to the
					// next symbol above it. Honest, and usually right for a
					// routine; the fields stay editable either way.
					dbg_addr_t next = 0x10000;
					for (int j = 0; j < nsym; j++) {
						const char *n2 = nullptr;
						dbg_addr_t  a2 = 0;
						if (dbg_info_symbol_at(j, &n2, &a2) && a2 > addr && a2 < next)
							next = a2;
					}
					snprintf(g_zone_name, sizeof g_zone_name, "%s", name);
					g_zone_start = (int)(addr & 0xFFFF);
					g_zone_end   = (int)(next > 0xFFFF ? 0x10000 : next);
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		}
		ImGui::SameLine();
	}

	const bool valid = g_zone_end > g_zone_start && g_zone_start >= 0 && g_zone_end <= 0x10000;
	ImGui::BeginDisabled(!valid);
	if (ImGui::Button("Add zone")) {
		perf_budget_zone_add(g_zone_name[0] ? g_zone_name : "zone", (uint16_t)g_zone_start,
		                     (uint16_t)(g_zone_end > 0xFFFF ? 0xFFFF : g_zone_end),
		                     (int16_t)g_zone_bank, g_zone_idle);
		g_zone_name[0] = '\0';
	}
	ImGui::EndDisabled();
	if (!valid) {
		ImGui::SameLine();
		ImGui::TextDisabled("(end must be above start)");
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear all"))
		perf_budget_zone_clear();
}

void
draw_controls(void)
{
	DebugUiSettings &s = debug_ui_settings();

	if (ImGui::Button("Reset statistics"))
		perf_budget_reset();
	ImGui::SameLine();

	const char *modes[] = {"Auto (WAI + spin detection)", "Markers and idle zones only",
	                       "Nothing is idle"};
	ImGui::SetNextItemWidth(dbgui_combo_width("Auto (WAI + spin detection)"));
	if (ImGui::Combo("Idle detection", &s.perf_idle_mode, modes, 3)) {
		perf_budget_set_idle_mode((perf_idle_mode_t)s.perf_idle_mode);
		debug_ui_settings_mark_dirty();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
		    "How waiting is told apart from work.\n\n"
		    "Auto catches WAI and a tight loop that stores nothing, which covers\n"
		    "polling $9F27 and polling a flag an IRQ handler sets. It CANNOT see\n"
		    "a wait loop that stores or calls a subroutine -- mark those as an\n"
		    "idle zone, or bracket the real work with $9FBC markers.");
	}

	if (ImGui::Checkbox("Keep profiling when this panel is closed", &s.perf_always_on))
		debug_ui_settings_mark_dirty();
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Profiling costs the running machine an add and a table lookup\n"
		                  "per instruction, so it is normally armed only while this panel\n"
		                  "is open. Turn this on to keep a DAP client fed with the panel shut.");
	}

	ImGui::SetNextItemWidth(dbgui_field_width("99999"));
	if (ImGui::SliderInt("History (frames)", &s.perf_capacity, 120, 7200)) {
		perf_budget_set_capacity(s.perf_capacity);
		debug_ui_settings_mark_dirty();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_field_width("99999"));
	if (ImGui::SliderInt("Graph frames", &s.perf_graph_frames, 30, 1024))
		debug_ui_settings_mark_dirty();
}

// ---------------------------------------------------------------------------
// Bandwidth: VERA's side of "what is eating my frame?".
//
// The CPU budget above and this share one panel deliberately. The interesting
// frames are the ones where the two disagree -- code with cycles to spare that
// still cannot get its pixels through the bus, or a bus with room left over
// while the CPU drowns.
//
// The unit is CLOCKS OF ONE SCANLINE, not a percentage of VERA's total
// bandwidth. A frame uses maybe a fifth of the chip's 100 MB/s, so a
// percent-of-peak reading would sit near 20% forever and tell nobody anything
// -- the same trap as reporting raw cycles-per-frame, which is always 100%.
// The scanline is the window that genuinely runs out.
void
draw_bandwidth(void)
{
	vera_bw_frame_t f;
	if (!vera_bandwidth_last_frame(&f)) {
		ImGui::TextDisabled("No completed frame yet.");
		return;
	}

	const float peak_pct = 100.0f * (float)f.peak_clocks / (float)VERA_BW_LINE_CLOCKS;

	ImGui::Text("Peak scanline:");
	ImGui::SameLine();
	ImGui::TextColored(utilization_color(peak_pct), "%u of %d clocks (%.1f%%)",
	                   (unsigned)f.peak_clocks, VERA_BW_LINE_CLOCKS, peak_pct);
	ImGui::SameLine();
	ImGui::TextDisabled("on line %u", (unsigned)f.peak_line);

	ImGui::Text("p95 %u   mean %u   over budget: %u line(s)", (unsigned)f.p95_clocks,
	            (unsigned)f.mean_clocks, (unsigned)f.lines_over);

	// ── Layers ──────────────────────────────────────────────────────────────
	if (ImGui::BeginTable("bw_layers", 4,
	                      ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Source");
		ImGui::TableSetupColumn("Fetches/frame");
		ImGui::TableSetupColumn("Bytes/frame");
		ImGui::TableSetupColumn("Peak line");
		ImGui::TableHeadersRow();

		for (int i = 0; i < 2; i++) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Layer %d", i);
			ImGui::TableNextColumn();
			ImGui::Text("%u", (unsigned)f.layer_fetches[i]);
			ImGui::TableNextColumn();
			ImGui::Text("%u", (unsigned)f.layer_bytes[i]);
			ImGui::TableNextColumn();
			ImGui::Text("%u on line %u", (unsigned)f.layer_peak_fetches[i],
			            (unsigned)f.layer_peak_line[i]);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("CPU port");
		ImGui::TableNextColumn();
		ImGui::Text("%u", (unsigned)f.port_accesses);
		ImGui::TableNextColumn();
		ImGui::Text("%u", (unsigned)f.port_vram_bytes);
		ImGui::TableNextColumn();
		ImGui::TextDisabled("-");

		ImGui::EndTable();
	}

	// ── The data port, which is the part a program controls directly ────────
	ImGui::Spacing();
	const uint32_t moved = f.port_reads + f.port_writes;
	ImGui::Text("Data port ($9F23/$9F24): %u bytes written, %u read", (unsigned)f.port_writes,
	            (unsigned)f.port_reads);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("An 8 MHz CPU can issue roughly 64 accesses a scanline\n"
		                  "(a 4-cycle sta is 12.5 VERA clocks), so about 33 KB a\n"
		                  "frame is the ceiling no program can push past.");
	}
	if (f.fx_cache_writes || f.fx_affine_fetches) {
		const double amp = moved ? (double)f.port_vram_bytes / (double)moved : 0.0;
		ImGui::Text("VERA FX: %u cache write(s), %u affine fetch(es) — %.2fx amplification",
		            (unsigned)f.fx_cache_writes, (unsigned)f.fx_affine_fetches, amp);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Bytes that reached VRAM per byte the CPU pushed.\n"
			                  "An FX cache write moves four bytes for one store,\n"
			                  "which is the whole reason to use it.");
		}
	}

	// ── Per-scanline bars, drawn like the sprite render-time view ───────────
	uint16_t              nlines = 0;
	const vera_bw_line_t *lines  = vera_bandwidth_last_lines(&nlines);
	if (!lines || !nlines)
		return;

	ImGui::Spacing();
	ImGui::TextDisabled("Height is the bus time a scanline wanted. Anything above the white "
	                    "line is more than the scanline has.");

	ImDrawList  *dl     = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  w      = ImGui::GetContentRegionAvail().x;
	const float  h      = 180.0f;
	ImGui::InvisibleButton("bw_lines", ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered();

	dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h), IM_COL32(18, 18, 22, 255));

	// Same scaling rule as the sprite view: track the peak, but never squash
	// the ceiling below three-quarters height, so an ordinary frame does not
	// look alarmingly full.
	const float floor_top = (float)VERA_BW_LINE_CLOCKS * 4.0f / 3.0f;
	float       top       = (float)f.peak_clocks > floor_top ? (float)f.peak_clocks : floor_top;
	const float bw        = w / (float)nlines;

	for (uint16_t line = 0; line < nlines; ++line) {
		const vera_bw_line_t &st = lines[line];
		const uint32_t        clocks = vera_bw_line_clocks(&st);
		if (!clocks)
			continue;
		const float bh  = ((float)clocks / top) * h;
		const ImU32 col = clocks > VERA_BW_LINE_CLOCKS ? IM_COL32(235, 70, 60, 255)
		                                               : IM_COL32(120, 200, 140, 255);
		dl->AddRectFilled(ImVec2(origin.x + line * bw, origin.y + h - bh),
		                  ImVec2(origin.x + line * bw + (bw > 1.0f ? bw : 1.0f), origin.y + h),
		                  col);
	}

	const float ceil_y = origin.y + h - ((float)VERA_BW_LINE_CLOCKS / top) * h;
	dl->AddLine(ImVec2(origin.x, ceil_y), ImVec2(origin.x + w, ceil_y),
	            IM_COL32(255, 255, 255, 200));
	dl->AddText(ImVec2(origin.x + 4, ceil_y - 16), IM_COL32(255, 255, 255, 200),
	            "one scanline (800 clocks)");

	if (hovered && bw > 0.0f) {
		int line = (int)((ImGui::GetMousePos().x - origin.x) / bw);
		if (line < 0)
			line = 0;
		if (line >= (int)nlines)
			line = nlines - 1;
		const vera_bw_line_t &st = lines[line];
		const uint32_t layer_clocks =
		    ((uint32_t)st.layer_fetches[0] + st.layer_fetches[1]) * VERA_BW_ACCESS_CLOCKS;
		const uint32_t port_clocks = (uint32_t)st.port_accesses * VERA_BW_ACCESS_CLOCKS;

		ImGui::BeginTooltip();
		ImGui::Text("line %d", line);
		ImGui::Text("layer 0: %u fetches, layer 1: %u", st.layer_fetches[0], st.layer_fetches[1]);
		ImGui::Text("CPU port: %u access(es)", st.port_accesses);
		ImGui::Text("%u of %d clocks", layer_clocks + port_clocks, VERA_BW_LINE_CLOCKS);

		// What was left for the sprite renderer, which is last on the bus
		// (vram_if.v:142-157). Crossed with the sprite trace HERE rather than
		// inside vera_bandwidth.c, which is what keeps that module free of any
		// dependency on this one.
		const uint32_t taken = layer_clocks + port_clocks;
		const uint32_t left  = taken < VERA_BW_LINE_CLOCKS ? VERA_BW_LINE_CLOCKS - taken : 0;
		ImGui::Separator();
		ImGui::Text("left for sprites: %u clocks", left);
		const sprite_trace_frame_t *sf = sprite_trace_last_frame();
		if (sf && line < SPRITE_TRACE_LINES) {
			const sprite_line_stat_t &ss = sf->lines[line];
			if (ss.demand) {
				ImGui::Text("sprites wanted %u", ss.demand);
				if (ss.demand > left)
					ImGui::TextColored(ImVec4(1, 0.75f, 0.30f, 1),
					                   "more than was free: the sprite model charges\n"
					                   "one clock a fetch, so it flatters this line");
			}
		}
		ImGui::EndTooltip();
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Sprite render time has its own view: VERA panel \xe2\x86\x92 Multiplex "
	                    "\xe2\x86\x92 Render time.");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
		    "Sprites are LAST on VERA's bus (vram_if.v:142-157), so what the\n"
		    "layers and the CPU leave is what they get. The sprite model\n"
		    "charges one clock per fetch, which is the uncontended best case --\n"
		    "on a line the layers have filled, it under-charges.");
	}
}

void
perf_panel_render(bool *p_open)
{
	// Arm profiling for as long as the panel is open. Done before Begin() so a
	// collapsed-but-open panel keeps collecting: the user has said they are
	// interested, and a gap in the history helps nobody.
	static bool armed = false;
	DebugUiSettings &s = debug_ui_settings();
	const bool want = (p_open ? *p_open : true) || s.perf_always_on;
	if (want != armed) {
		perf_budget_arm(PERF_OWNER_UI, want);
		vera_bandwidth_arm(VERA_BW_OWNER_UI, want);
		if (want) {
			perf_budget_set_capacity(s.perf_capacity);
			perf_budget_set_target_fps(s.perf_target_fps);
			perf_budget_set_idle_mode((perf_idle_mode_t)s.perf_idle_mode);
		}
		armed = want;
	}

	if (!ImGui::Begin("Performance", p_open)) {
		dbgui_window_end();
		return;
	}
	dbgui_window_zoom("perf");

	if (!perf_budget_is_enabled()) {
		ImGui::TextDisabled("Profiling is off.");
		dbgui_window_end();
		return;
	}

	draw_budget_header();
	ImGui::Separator();
	draw_current_frame();
	ImGui::Separator();

	if (ImGui::CollapsingHeader("History", ImGuiTreeNodeFlags_DefaultOpen))
		draw_graph();
	if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen))
		draw_statistics();
	if (ImGui::CollapsingHeader("Breakdown", ImGuiTreeNodeFlags_DefaultOpen))
		draw_breakdown();
	if (ImGui::CollapsingHeader("Bandwidth", ImGuiTreeNodeFlags_DefaultOpen))
		draw_bandwidth();
	if (ImGui::CollapsingHeader("Zones"))
		draw_zone_editor();
	if (ImGui::CollapsingHeader("Settings"))
		draw_controls();

	dbgui_window_end();
}

DebugPanelRegistration s_reg("Performance", perf_panel_render, false);

} // namespace
