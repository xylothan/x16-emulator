// Commander X16 Emulator — ImGui "Log" panel.
//
// What logpoints have printed, from any client, oldest first. Reads the ring by
// sequence number, so looking at it takes nothing from the DAP or MCP readers.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_widgets.h"

#include <stdint.h>
#include <stdio.h>

namespace {

const char *const kOwnerNames[DEBUG_OWNER_COUNT] = {
	"cli", "ui", "dap", "dap", "dap", "dap", "step", "mcp",
};

uint64_t s_view_from  = 0;     // lines before this were cleared from the view
bool     s_autoscroll = true;

void
log_panel_render(bool *p_open)
{
	if (!ImGui::Begin("Log", p_open)) {
		dbgui_window_end();
		return;
	}
	dbgui_window_zoom("log");

	const dbg_log_ring_t *ring   = logpoint_ring();
	const uint64_t        oldest = dbg_log_ring_oldest(ring);
	const uint64_t        first  = s_view_from > oldest ? s_view_from : oldest;
	const uint64_t        lost   = s_view_from < oldest ? oldest - s_view_from : 0;

	ImGui::Checkbox("Follow", &s_autoscroll);
	ImGui::SetItemTooltip("Keep the newest line in view.");
	ImGui::SameLine();
	ImGui::BeginDisabled(first >= ring->next);
	if (ImGui::SmallButton("Clear"))
		s_view_from = ring->next;
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Clear this view. DAP and MCP clients still read these lines.");
	ImGui::SameLine();
	ImGui::Text("%llu lines", (unsigned long long)(ring->next - first));
	ImGui::SameLine();
	if (lost)
		ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.30f, 1.0f), "%llu lost", (unsigned long long)lost);
	else
		ImGui::TextDisabled("none lost");
	ImGui::SetItemTooltip("The last %d lines are kept. Older ones are overwritten.", DBG_LOG_RING);
	ImGui::Separator();

	if (ImGui::BeginChild("##log_lines", ImVec2(0, 0), ImGuiChildFlags_None,
	                      ImGuiWindowFlags_HorizontalScrollbar)) {
		if (first >= ring->next) {
			ImGui::TextDisabled("Nothing logged. Give a breakpoint a message in the Breakpoints panel.");
		} else {
			ImGuiListClipper clip;
			clip.Begin((int)(ring->next - first));
			while (clip.Step()) {
				for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
					const dbg_log_entry_t *e = dbg_log_ring_at(ring, first + (uint64_t)i);
					if (!e)
						continue;
					ImGui::TextDisabled("$%04X", (unsigned)(e->pc & 0xFFFF));
					ImGui::SameLine();
					ImGui::TextDisabled("%-4s", e->owner < DEBUG_OWNER_COUNT ? kOwnerNames[e->owner] : "?");
					ImGui::SameLine();
					ImGui::TextUnformatted(e->text);
				}
			}
			clip.End();
			if (s_autoscroll)
				ImGui::SetScrollHereY(1.0f);
		}
	}
	ImGui::EndChild();

	dbgui_window_end();
}

DebugPanelRegistration s_reg("Log", log_panel_render, true);

} // namespace
