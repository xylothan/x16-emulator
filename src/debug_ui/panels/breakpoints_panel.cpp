// Breakpoints panel — central manager for the debugger's breakpoints, plus a
// "run to address" control. Complements the Disassembly/Source panels (which
// toggle breakpoints inline) by giving one place to review, enable/disable, and
// clear them.
//
// Enable/disable is implemented panel-side (no core struct change): a disabled
// breakpoint is removed from the debugger's live breakPoints[] but kept in this
// panel's tracked list, so it can be re-armed later. The tracked list is
// reconciled with breakPoints[] every frame so breakpoints added/removed from
// the Disassembly/Source panels stay in sync. (Caveat: while disabled, a
// breakpoint shows no gutter marker in those panels since it is not live.)
//
// All state is reached through the shared bridge (debug_ui_bridge.h).
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct TrackedBP {
	int     pc;
	uint8_t bank;
	int     x16Bank;
	bool    enabled;
};

std::vector<TrackedBP> s_tracked;    // panel-owned superset of breakPoints[] (incl. disabled)
int                    s_runto_addr = 0; // hex address for the "Run to" box

bool
active_bp_exists(int pc, uint8_t bank)
{
	for (int i = 0; i < numBreakpoints; i++) {
		if (breakPoints[i].pc == pc && breakPoints[i].bank == bank) {
			return true;
		}
	}
	return false;
}

TrackedBP *
tracked_find(int pc, uint8_t bank)
{
	for (TrackedBP &t : s_tracked) {
		if (t.pc == pc && t.bank == bank) {
			return &t;
		}
	}
	return nullptr;
}

// Reconcile the tracked list with the debugger's live breakPoints[] so that
// breakpoints toggled from other panels are reflected here.
void
sync_tracked(void)
{
	// Live breakpoints → ensure tracked + marked enabled.
	for (int i = 0; i < numBreakpoints; i++) {
		TrackedBP *t = tracked_find(breakPoints[i].pc, breakPoints[i].bank);
		if (t) {
			t->enabled = true;
			t->x16Bank = breakPoints[i].x16Bank;
		} else {
			s_tracked.push_back(TrackedBP{breakPoints[i].pc, breakPoints[i].bank,
			                              breakPoints[i].x16Bank, true});
		}
	}
	// Tracked-enabled entries no longer live were deleted elsewhere → drop them.
	// (Disabled entries are intentionally absent from breakPoints[], so keep.)
	for (size_t i = 0; i < s_tracked.size();) {
		if (s_tracked[i].enabled && !active_bp_exists(s_tracked[i].pc, s_tracked[i].bank)) {
			s_tracked.erase(s_tracked.begin() + (long)i);
		} else {
			++i;
		}
	}
}

void
set_enabled(TrackedBP &t, bool enable)
{
	if (enable && !t.enabled) {
		struct breakpoint bp;
		bp.pc      = t.pc;
		bp.bank    = t.bank;
		bp.x16Bank = t.x16Bank;
		DEBUGAddBreakPoint(bp);
		t.enabled = true;
	} else if (!enable && t.enabled) {
		DEBUGRemoveBreakPoint(t.pc, t.bank);
		t.enabled = false;
	}
}

// ---- Conditional-breakpoint editor -----------------------------------------
const char *const kOperandNames[] = {"A", "X", "Y", "SP", "P", "byte[addr]", "word[addr]"};
const char *const kOpNames[]      = {"==", "!=", "<", "<=", ">", ">="};

struct CondEdit {
	int  pc      = 0;
	int  operand = 0;
	int  op      = 0;
	int  addr    = 0;
	int  value   = 0;
	int  ignore  = 0;
	bool loaded  = false;
};
CondEdit s_edit;

// One-line summary of a breakpoint's condition/ignore for the table cell.
void
cond_summary(char *out, size_t n, const TrackedBP &t)
{
	int      has = 0, operand = 0, op = 0;
	uint16_t oaddr = 0;
	uint32_t value = 0, ignore = 0;
	bool     have = DEBUGGetBreakpointCondition(t.pc, t.bank, &has, &operand, &oaddr, &op, &value, &ignore);
	if (have && has) {
		if (operand == BP_OPND_BYTE)
			snprintf(out, n, "byte[$%04X]%s$%X", oaddr, kOpNames[op], value);
		else if (operand == BP_OPND_WORD)
			snprintf(out, n, "word[$%04X]%s$%X", oaddr, kOpNames[op], value);
		else
			snprintf(out, n, "%s%s$%X", kOperandNames[operand], kOpNames[op], value);
	} else if (have && ignore > 0) {
		snprintf(out, n, "ignore %u", ignore);
	} else {
		snprintf(out, n, "set...");
	}
}

// Per-row popup to edit the condition + ignore count + reset hits.
void
cond_editor_popup(const TrackedBP &t)
{
	if (!ImGui::BeginPopup("cond_edit"))
		return;
	if (!s_edit.loaded) {
		int      has = 0, operand = 0, op = 0;
		uint16_t oaddr = 0;
		uint32_t value = 0, ignore = 0;
		DEBUGGetBreakpointCondition(t.pc, t.bank, &has, &operand, &oaddr, &op, &value, &ignore);
		s_edit.pc      = t.pc;
		s_edit.operand = operand;
		s_edit.op      = op;
		s_edit.addr    = oaddr;
		s_edit.value   = (int)value;
		s_edit.ignore  = (int)ignore;
		s_edit.loaded  = true;
	}

	ImGui::Text("Breakpoint $%04X", (unsigned)(t.pc & 0xFFFF));
	ImGui::Separator();
	ImGui::TextUnformatted("Stop only when:");
	ImGui::SetNextItemWidth(110);
	ImGui::Combo("##opnd", &s_edit.operand, kOperandNames, IM_ARRAYSIZE(kOperandNames));
	ImGui::SameLine();
	if (s_edit.operand == BP_OPND_BYTE || s_edit.operand == BP_OPND_WORD) {
		ImGui::SetNextItemWidth(70);
		ImGui::InputScalar("##caddr", ImGuiDataType_S32, &s_edit.addr, nullptr, nullptr, "%X",
		                   ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::SameLine();
	}
	ImGui::SetNextItemWidth(55);
	ImGui::Combo("##op", &s_edit.op, kOpNames, IM_ARRAYSIZE(kOpNames));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70);
	ImGui::InputScalar("= $##cval", ImGuiDataType_S32, &s_edit.value, nullptr, nullptr, "%X",
	                   ImGuiInputTextFlags_CharsHexadecimal);

	ImGui::SetNextItemWidth(90);
	ImGui::InputInt("ignore first N hits", &s_edit.ignore);
	if (s_edit.ignore < 0)
		s_edit.ignore = 0;

	ImGui::Text("Hit count: %u", DEBUGGetBreakpointHits(t.pc, t.bank));
	ImGui::SameLine();
	if (ImGui::SmallButton("Reset hits"))
		DEBUGResetBreakpointHits(t.pc, t.bank);

	ImGui::Separator();
	if (ImGui::Button("Apply")) {
		DEBUGSetBreakpointCondition(t.pc, t.bank, t.x16Bank, s_edit.operand, (uint16_t)s_edit.addr,
		                            s_edit.op, (uint32_t)s_edit.value);
		DEBUGSetBreakpointIgnore(t.pc, t.bank, t.x16Bank, (uint32_t)s_edit.ignore);
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear condition")) {
		DEBUGClearBreakpointCondition(t.pc, t.bank);
		DEBUGSetBreakpointIgnore(t.pc, t.bank, t.x16Bank, (uint32_t)s_edit.ignore);
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Close"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void
breakpoints_panel_render(bool *p_open)
{
	if (ImGui::Begin("Breakpoints", p_open)) {
		sync_tracked();

		// --- Run to address -------------------------------------------------
		// Sets a one-shot internal breakpoint and resumes; the CPU halts when
		// it reaches the address. Only meaningful while paused.
		ImGui::TextUnformatted("Run to $");
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetNextItemWidth(70.0f);
		ImGui::InputScalar("##runto", ImGuiDataType_S32, &s_runto_addr,
		                   nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::SameLine();
		ImGui::BeginDisabled(!DEBUGIsPaused());
		if (ImGui::Button("Go")) {
			DEBUGRunTo((uint16_t)(s_runto_addr & 0xFFFF), 0);
		}
		ImGui::EndDisabled();
		if (!DEBUGIsPaused()) {
			ImGui::SameLine();
			ImGui::TextDisabled("(pause first)");
		}

		ImGui::Separator();

		// --- Breakpoint list ------------------------------------------------
		ImGui::Text("Breakpoints: %d", (int)s_tracked.size());
		ImGui::SameLine();
		ImGui::BeginDisabled(s_tracked.empty());
		if (ImGui::SmallButton("Enable All")) {
			for (TrackedBP &t : s_tracked) set_enabled(t, true);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Disable All")) {
			for (TrackedBP &t : s_tracked) set_enabled(t, false);
		}
		ImGui::SameLine();
		bool clear_all = ImGui::SmallButton("Clear All");
		ImGui::EndDisabled();

		int remove_idx = -1;  // deferred per-row delete

		if (ImGui::BeginTable("bp_table", 6,
		        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("On",      ImGuiTableColumnFlags_WidthFixed, 26.0f);
			ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 56.0f);
			ImGui::TableSetupColumn("Bank",    ImGuiTableColumnFlags_WidthFixed, 34.0f);
			ImGui::TableSetupColumn("Hits",    ImGuiTableColumnFlags_WidthFixed, 46.0f);
			ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 28.0f);
			ImGui::TableHeadersRow();

			for (int i = 0; i < (int)s_tracked.size(); i++) {
				TrackedBP &t = s_tracked[i];
				ImGui::TableNextRow();
				ImGui::PushID(i);

				ImGui::TableNextColumn();
				bool en = t.enabled;
				if (ImGui::Checkbox("##en", &en)) {
					set_enabled(t, en);
				}

				ImGui::TableNextColumn();
				if (t.enabled) {
					ImGui::Text("$%04X", (unsigned)(t.pc & 0xFFFF));
				} else {
					ImGui::TextDisabled("$%04X", (unsigned)(t.pc & 0xFFFF));
				}

				ImGui::TableNextColumn();
				if (t.x16Bank >= 0) {
					ImGui::Text("%d", t.x16Bank);
				} else {
					ImGui::TextDisabled("-");
				}

				ImGui::TableNextColumn();
				ImGui::Text("%u", DEBUGGetBreakpointHits(t.pc, t.bank));

				ImGui::TableNextColumn();
				char summary[48];
				cond_summary(summary, sizeof summary, t);
				if (ImGui::SmallButton(summary)) {
					s_edit.loaded = false;
					ImGui::OpenPopup("cond_edit");
				}
				cond_editor_popup(t);

				ImGui::TableNextColumn();
				if (ImGui::SmallButton("x")) {
					remove_idx = i;
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		if (s_tracked.empty()) {
			ImGui::TextDisabled("No breakpoints. Click the gutter in the");
			ImGui::TextDisabled("Disassembly or Source panel to add one.");
		}

		// Apply deferred mutations after the table is built. Explicit deletes
		// also free the condition/hit entry (enable/disable must NOT).
		if (clear_all) {
			for (TrackedBP &t : s_tracked) {
				if (t.enabled) DEBUGRemoveBreakPoint(t.pc, t.bank);
				DEBUGForgetBreakpoint(t.pc, t.bank);
			}
			s_tracked.clear();
		} else if (remove_idx >= 0 && remove_idx < (int)s_tracked.size()) {
			if (s_tracked[remove_idx].enabled) {
				DEBUGRemoveBreakPoint(s_tracked[remove_idx].pc, s_tracked[remove_idx].bank);
			}
			DEBUGForgetBreakpoint(s_tracked[remove_idx].pc, s_tracked[remove_idx].bank);
			s_tracked.erase(s_tracked.begin() + remove_idx);
		}

		// --- Watchpoints (managed in the Memory panel) ----------------------
		ImGui::Separator();
		ImGui::Text("Watchpoints: %d", debug_wp_count());
		ImGui::TextDisabled("(add/remove data watchpoints in the Memory panel)");
	}
	ImGui::End();
}

DebugPanelRegistration s_reg("Breakpoints", breakpoints_panel_render, true);

} // namespace

