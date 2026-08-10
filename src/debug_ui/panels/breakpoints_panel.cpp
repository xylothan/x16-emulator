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
#include "debug_ui_widgets.h"

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
		DEBUGRemoveBreakPoint(t.pc, t.bank, t.x16Bank);
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
	bool     have = DEBUGGetBreakpointCondition(t.pc, t.bank, t.x16Bank, &has, &operand, &oaddr, &op, &value, &ignore);
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
		DEBUGGetBreakpointCondition(t.pc, t.bank, t.x16Bank, &has, &operand, &oaddr, &op, &value, &ignore);
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
	ImGui::SetNextItemWidth(dbgui_combo_width("byte at $"));
	ImGui::Combo("##opnd", &s_edit.operand, kOperandNames, IM_ARRAYSIZE(kOperandNames));
	ImGui::SetItemTooltip("What to test: a CPU register, or the byte/word at an address.");
	ImGui::SameLine();
	if (s_edit.operand == BP_OPND_BYTE || s_edit.operand == BP_OPND_WORD) {
		ImGui::SetNextItemWidth(dbgui_field_width("FFFF"));
		ImGui::InputScalar("##caddr", ImGuiDataType_S32, &s_edit.addr, nullptr, nullptr, "%X",
		                   ImGuiInputTextFlags_CharsHexadecimal);
		ImGui::SetItemTooltip("Address to read, in HEX. \"10\" means $10 (16), not ten.");
		ImGui::SameLine();
	}
	ImGui::SetNextItemWidth(dbgui_combo_width(">="));
	ImGui::Combo("##op", &s_edit.op, kOpNames, IM_ARRAYSIZE(kOpNames));
	ImGui::SetItemTooltip("Comparison to apply.");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_field_width("FFFF"));
	ImGui::InputScalar("= $##cval", ImGuiDataType_S32, &s_edit.value, nullptr, nullptr, "%X",
	                   ImGuiInputTextFlags_CharsHexadecimal);
	ImGui::SetItemTooltip("Value to compare against, in HEX (hence the $).\n"
	                      "\"10\" means $10 (16), not ten.");
	ImGui::SameLine();
	ImGui::TextDisabled("(hex)");
	ImGui::SetItemTooltip("Both the address and value fields above are hexadecimal.");

	ImGui::SetNextItemWidth(dbgui_field_width("99999") + ImGui::GetFrameHeight() * 2.0f);
	ImGui::InputInt("ignore first N hits", &s_edit.ignore);
	ImGui::SetItemTooltip("Decimal, unlike the fields above. Let the breakpoint pass this\n"
	                      "many matching hits before it actually stops - useful for reaching\n"
	                      "a particular iteration of a loop.");
	if (s_edit.ignore < 0)
		s_edit.ignore = 0;

	ImGui::Text("Hit count: %u", DEBUGGetBreakpointHits(t.pc, t.bank, t.x16Bank));
	ImGui::SetItemTooltip("Times this breakpoint's condition has matched since it was set or\n"
	                      "last reset. Only counts while the machine is running - sitting at\n"
	                      "the breakpoint does not add to it.");
	ImGui::SameLine();
	if (ImGui::SmallButton("Reset hits"))
		DEBUGResetBreakpointHits(t.pc, t.bank, t.x16Bank);

	ImGui::Separator();
	if (ImGui::Button("Apply")) {
		DEBUGSetBreakpointCondition(t.pc, t.bank, t.x16Bank, s_edit.operand, (uint16_t)s_edit.addr,
		                            s_edit.op, (uint32_t)s_edit.value);
		DEBUGSetBreakpointIgnore(t.pc, t.bank, t.x16Bank, (uint32_t)s_edit.ignore);
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear condition")) {
		DEBUGClearBreakpointCondition(t.pc, t.bank, t.x16Bank);
		DEBUGSetBreakpointIgnore(t.pc, t.bank, t.x16Bank, (uint32_t)s_edit.ignore);
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Close"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Breakpoints - break when execution reaches an address.
// ---------------------------------------------------------------------------
void
draw_breakpoints()
{
	// --- Run to address ---------------------------------------------------
	// Sets a one-shot internal breakpoint and resumes; the CPU halts when
	// it reaches the address. Only meaningful while paused.
	ImGui::TextUnformatted("Run to $");
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::SetNextItemWidth(dbgui_field_width("FFFF"));
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
	        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
	            DBGUI_TABLE_FLAGS_RESIZABLE)) {
		ImGui::TableSetupColumn("On",      ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_fit("On", ImGui::GetFrameHeight()));
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_width("Address", "$FFFF"));
		ImGui::TableSetupColumn("Bank",    ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_width("Bank", "255"));
		ImGui::TableSetupColumn("Hits",    ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_width("Hits", "99999"));
		ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_fit("", dbgui_field_width("x")));
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
			ImGui::Text("%u", DEBUGGetBreakpointHits(t.pc, t.bank, t.x16Bank));

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
		ImGui::TextDisabled("No breakpoints.");
		ImGui::SetItemTooltip("Click the gutter in the Disassembly or Source panel to add one.");
	}

	// Apply deferred mutations after the table is built. Explicit deletes
	// also free the condition/hit entry (enable/disable must NOT).
	if (clear_all) {
		for (TrackedBP &t : s_tracked) {
			if (t.enabled) DEBUGRemoveBreakPoint(t.pc, t.bank, t.x16Bank);
			DEBUGForgetBreakpoint(t.pc, t.bank, t.x16Bank);
		}
		s_tracked.clear();
	} else if (remove_idx >= 0 && remove_idx < (int)s_tracked.size()) {
		if (s_tracked[remove_idx].enabled) {
			DEBUGRemoveBreakPoint(s_tracked[remove_idx].pc, s_tracked[remove_idx].bank, s_tracked[remove_idx].x16Bank);
		}
		DEBUGForgetBreakpoint(s_tracked[remove_idx].pc, s_tracked[remove_idx].bank, s_tracked[remove_idx].x16Bank);
		s_tracked.erase(s_tracked.begin() + remove_idx);
	}
}

// ---------------------------------------------------------------------------
// Watchpoints — break when the program WRITES to an address or range.
//
// These are a different trigger from the PC breakpoints above (which fire when
// execution reaches an address), but they are the same idea to a user: a
// condition that stops the machine. They used to be creatable from the memory
// view with no way to see or manage them, which is why they live here now.
//
// Note the trigger is a write, not a change: storing the same value back still
// fires. The optional value filter narrows that.
// ---------------------------------------------------------------------------
const char *
wp_op_name(int op)
{
	static const char *names[] = { "==", "!=", "<", "<=", ">", ">=" };
	return (op >= 0 && op < 6) ? names[op] : "==";
}

void
draw_watchpoints()
{
	// --- Add ---------------------------------------------------------------
	static int wp_addr = 0;
	static int wp_len  = 1;

	ImGui::TextUnformatted("Break on write to $");
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::SetNextItemWidth(dbgui_field_width("FFFF"));
	ImGui::InputScalar("##wpaddr", ImGuiDataType_S32, &wp_addr, nullptr, nullptr, "%04X",
	                   ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AutoSelectAll);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("First address to cover (hex).");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_field_width("65535") + ImGui::GetFrameHeight() * 2.0f);
	ImGui::InputInt("len", &wp_len);
	if (wp_len < 1) wp_len = 1;
	if (wp_len > 0xFFFF) wp_len = 0xFFFF;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("How many consecutive bytes to cover. A write anywhere in the\n"
		                  "range triggers the break.");
	ImGui::SameLine();
	ImGui::BeginDisabled(debug_wp_count() >= MAX_WATCHPOINTS);
	if (ImGui::Button("Add")) {
		DEBUGAddWatchPoint((uint16_t)(wp_addr & 0xFFFF), (uint16_t)wp_len);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("You can also right-click a byte (or a drag-selected range)\n"
		                  "in the Memory panel and choose \"Break on write\".");
	ImGui::SameLine();
	ImGui::TextDisabled("(%d / %d used)", debug_wp_count(), MAX_WATCHPOINTS);

	ImGui::Separator();

	if (debug_wp_count() == 0) {
		ImGui::TextDisabled("None set.");
		ImGui::SetItemTooltip("Add one above, or right-click an address (or a drag-selected\n"
		                      "range) in the Memory panel and choose \"Break on write\".");
		return;
	}

	// --- List --------------------------------------------------------------
	int remove_addr = -1;
	int remove_bank = DEBUG_BANK_ANY;

	if (ImGui::BeginTable("wp_table", 6,
	        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
	            DBGUI_TABLE_FLAGS_RESIZABLE)) {
		ImGui::TableSetupColumn("On",      ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_fit("On", ImGui::GetFrameHeight()));
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_width("Address", "$FFFF"));
		ImGui::TableSetupColumn("Len",     ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_width("Len", "65535"));
		ImGui::TableSetupColumn("Range",   ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_width("Range", "$FFFF-$FFFF"));
		ImGui::TableSetupColumn("Only when written value", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,
		                        dbgui_col_fit("", dbgui_field_width("x")));
		ImGui::TableHeadersRow();

		for (int i = 0; i < debug_wp_count(); i++) {
			const struct watchpoint *w = debug_wp_at(i);
			if (!w)
				break;
			// Edits go through the core's setters rather than writing the
			// table, which is private to debug_core.c. They are keyed on the
			// watchpoint's OWN bank, not DEBUG_BANK_ANY: a watch set on one
			// RAM bank's $A100 would otherwise not be found, and the edit
			// would silently do nothing.
			const uint16_t w_addr = w->addr;
			const int      w_bank = w->x16Bank;
			ImGui::PushID(i);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			bool active = w->active;
			if (ImGui::Checkbox("##on", &active))
				debug_wp_set_active(w_addr, w_bank, active);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Disable without deleting.");

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("$%04X", w->addr);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Click to show this address in the Memory panel.");
			if (ImGui::IsItemClicked())
				debug_ui_request_goto(w->addr, 0);

			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%u", (unsigned)w->len);

			ImGui::TableSetColumnIndex(3);
			if (w->len > 1)
				ImGui::TextDisabled("$%04X-$%04X", w->addr, (unsigned)(w->addr + w->len - 1));
			else
				ImGui::TextDisabled("-");

			// Optional filter: only break when the value being written matches.
			ImGui::TableSetColumnIndex(4);
			bool has = w->has_value;
			if (ImGui::Checkbox("##hasval", &has)) {
				if (has)
					debug_wp_set_value(w_addr, w_bank, w->op, w->value);
				else
					debug_wp_clear_value(w_addr, w_bank);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Only break when the value being written satisfies a test,\n"
				                  "rather than on every write.");
			if (w->has_value) {
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dbgui_combo_width(">="));
				int op = w->op;
				if (ImGui::BeginCombo("##op", wp_op_name(op))) {
					for (int k = 0; k < 6; k++) {
						if (ImGui::Selectable(wp_op_name(k), op == k))
							debug_wp_set_value(w_addr, w_bank, k, w->value);
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(dbgui_field_width("FF"));
				int val = w->value;
				if (ImGui::InputScalar("##val", ImGuiDataType_S32, &val, nullptr, nullptr, "%02X",
				                       ImGuiInputTextFlags_CharsHexadecimal |
				                           ImGuiInputTextFlags_AutoSelectAll)) {
					debug_wp_set_value(w_addr, w_bank, w->op, (uint8_t)(val & 0xFF));
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Value to compare the written byte against (hex).");
			} else {
				ImGui::SameLine();
				ImGui::TextDisabled("any write");
			}

			ImGui::TableSetColumnIndex(5);
			if (ImGui::SmallButton("x")) {
				remove_addr = w->addr;
				remove_bank = w_bank;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Delete this watchpoint.");

			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (remove_addr >= 0) {
		debug_wp_remove((uint16_t)remove_addr, remove_bank);
	}
}

void
breakpoints_panel_render(bool *p_open)
{
	if (ImGui::Begin("Breakpoints", p_open)) {
		dbgui_window_zoom("breakpoints");
		sync_tracked();

		// Collapsible sections rather than tabs or fixed-height panes: these are
		// two different triggers for the same job (stop the machine) and you
		// usually want both in view, but either can be folded away when you are
		// only using one. Matches how the CPU panel is organised.
		//
		// "Watchpoint" is avoided as a label because the CPU panel's Watch list
		// already owns that word for something else entirely - values you are
		// keeping an eye on, which never stop execution.
		char exec_hdr[80];
		snprintf(exec_hdr, sizeof exec_hdr, "Execution (%d)###exec", (int)s_tracked.size());
		const bool exec_open = ImGui::CollapsingHeader(exec_hdr, ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::SetItemTooltip("Break when the PC reaches an address.\n\n"
		                      "Click the gutter in the Disassembly or Source panel to add one,\n"
		                      "or use \"Run to $\" below for a one-shot stop.");
		if (exec_open) {
			draw_breakpoints();
		}

		char wp_hdr[80];
		snprintf(wp_hdr, sizeof wp_hdr, "Memory writes (%d)###wp", debug_wp_count());
		const bool wp_open = ImGui::CollapsingHeader(wp_hdr, ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::SetItemTooltip("Break when the program writes to an address - the way to find\n"
		                      "what is clobbering a value.\n\n"
		                      "Any store counts, including one that writes the same value back,\n"
		                      "so this is break-on-write, not break-on-change. Use the value\n"
		                      "filter on a row to narrow it.");
		if (wp_open) {
			draw_watchpoints();
		}
	}
	dbgui_window_end();
}

DebugPanelRegistration s_reg("Breakpoints", breakpoints_panel_render, true);

} // namespace
