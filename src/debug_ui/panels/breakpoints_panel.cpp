// Breakpoints panel — central manager for the debugger's breakpoints, plus a
// "run to address" control. Complements the Disassembly/Source panels (which
// toggle breakpoints inline) by giving one place to add, review, enable/disable,
// and clear them.
//
// Enable/disable belongs to the core: a disabled breakpoint keeps its entry,
// its owners, its condition and its hit count, and simply does not stop the
// machine. This panel used to implement it by deleting the entry and
// remembering it in a list of its own, which meant a disabled breakpoint
// vanished from the table and so showed no gutter marker anywhere else.
//
// The list below is therefore a view of the core's table, rebuilt each frame,
// rather than a superset of it with a lifetime of its own.
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

std::vector<TrackedBP> s_tracked;    // a view of the core's table, rebuilt each frame
int                    s_runto_addr = 0; // hex address for the "Run to" box

// "Add breakpoint" row state. See add_breakpoint_row().
int  s_add_addr         = 0;
int  s_add_bank         = 0;
bool s_add_bank_touched = false; // has the user chosen a bank by hand?

// A breakpoint is identified by (pc, bank, x16Bank), because the same $A000 in
// two RAM banks is two different breakpoints. Keying on (pc, bank) alone
// collapsed them into one row: the count under-reported, "Clear All" left the
// other one armed and the list repopulated itself on the next frame, and the
// enable checkbox flipped back on by itself because the survivor was still
// found.
void
sync_tracked(void)
{
	s_tracked.clear();
	const int n = debug_bp_count();
	s_tracked.reserve((size_t)(n < 0 ? 0 : n));
	for (int i = 0; i < n; i++) {
		const struct breakpoint *b = debug_bp_at(i);
		if (!b) {
			break;
		}
		s_tracked.push_back(TrackedBP{b->pc, b->bank, b->x16Bank, b->enabled});
	}
}

void
set_enabled(TrackedBP &t, bool enable)
{
	if (DEBUGSetBreakpointEnabled(t.pc, t.bank, t.x16Bank, enable)) {
		t.enabled = enable;
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

// Highest bank number that exists for the window `addr` falls in. RAM banking is
// sized at runtime by -ram, so this is a real limit, not a constant.
int
max_bank_for(uint16_t addr)
{
	const int n = (addr >= 0xC000) ? memory_get_num_rom_banks() : (int)num_ram_banks;
	return (n > 0 ? n : 1) - 1;
}

int
clamp_bank(int bank, uint16_t addr)
{
	const int hi = max_bank_for(addr);
	return bank < 0 ? 0 : (bank > hi ? hi : bank);
}

// Add a breakpoint by typing its address.
//
// Everywhere else in the debugger a breakpoint is set by pointing at a line -
// the gutter, or F9 on the cursor - which is no help for an address you cannot
// currently see: ROM, a bank that is not mapped in, or code that has not been
// reached yet and so has no disassembly to click. The Memory-writes section
// below has always had a row like this; execution breakpoints did not, and the
// only address box on this panel drove "Run to", which is a one-shot resume and
// not a breakpoint at all.
void
add_breakpoint_row(void)
{
	const uint16_t addr   = (uint16_t)(s_add_addr & 0xFFFF);
	const bool     banked = addr >= 0xA000;
	const int      live   = debug_current_x16_bank((int)addr, 0);

	// Until the user picks a bank the field tracks the mapping the machine is
	// in, so typing an address alone sets exactly the breakpoint clicking the
	// gutter would have set. Leaving the banked window forgets the choice; a
	// chosen bank is re-clamped because the address can move between the RAM
	// and ROM windows under it, and those are different depths.
	if (!banked) {
		s_add_bank_touched = false;
	} else if (!s_add_bank_touched) {
		s_add_bank = live >= 0 ? live : 0;
	} else {
		s_add_bank = clamp_bank(s_add_bank, addr);
	}

	ImGui::TextUnformatted("Break at $");
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::SetNextItemWidth(dbgui_field_width("FFFF"));
	const bool submit = ImGui::InputScalar("##bpaddr", ImGuiDataType_S32, &s_add_addr, nullptr, nullptr, "%04X",
	                                       ImGuiInputTextFlags_CharsHexadecimal |
	                                           ImGuiInputTextFlags_AutoSelectAll |
	                                           ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SetItemTooltip("Address to break at, in HEX. \"10\" means $10 (16), not ten.\n"
	                      "Press Enter to add.");

	// $A000 and up is read through a bank window, so an address there is not one
	// breakpoint but one per bank. Below it there is nothing to choose.
	ImGui::SameLine();
	ImGui::BeginDisabled(!banked);
	ImGui::TextUnformatted("in bank");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(dbgui_field_width("255") + ImGui::GetFrameHeight() * 2.0f);
	int bank = s_add_bank;
	if (ImGui::InputInt("##bpbank", &bank)) {
		s_add_bank         = clamp_bank(bank, addr);
		s_add_bank_touched = true;
	}
	ImGui::SetItemTooltip("Which bank the address lives in, in DECIMAL. Defaults to the bank\n"
	                      "mapped in right now - the one the gutter would have used.\n"
	                      "$A000-$BFFF selects a RAM bank, $C000-$FFFF a ROM bank.");
	ImGui::EndDisabled();

	ImGui::SameLine();
	const bool add = ImGui::Button("Add##bp") || submit;
	ImGui::SetItemTooltip("You can also click the gutter in the Disassembly or Source panel,\n"
	                      "or press F9 on the cursor line.");
	ImGui::SameLine();
	if (addr >= 0xC000) {
		ImGui::TextDisabled("(ROM)");
	} else if (banked) {
		ImGui::TextDisabled("(RAM)");
	} else {
		ImGui::TextDisabled("(unbanked)");
	}
	ImGui::SetItemTooltip("Addresses below $A000 are the same memory whatever is banked in,\n"
	                      "so the bank does not apply to them.");

	if (add) {
		struct breakpoint bp = {};
		bp.pc                = (int)addr;
		bp.bank              = 0;
		bp.x16Bank           = banked ? s_add_bank : DEBUG_BANK_ANY;
		DEBUGAddBreakPoint(bp);
		sync_tracked(); // show it in the table below this frame, not next
	}
}

void
draw_breakpoints()
{
	// --- Add by address ---------------------------------------------------
	add_breakpoint_row();

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
		ImGui::SetItemTooltip("Type an address above, or click the gutter in the Disassembly\n"
		                      "or Source panel.");
	}

	// Apply deferred mutations after the table is built. Explicit deletes
	// also free the condition/hit entry (enable/disable must NOT).
	if (clear_all) {
		for (TrackedBP &t : s_tracked) {
			DEBUGRemoveBreakPoint(t.pc, t.bank, t.x16Bank);
			DEBUGForgetBreakpoint(t.pc, t.bank, t.x16Bank);
		}
		s_tracked.clear();
	} else if (remove_idx >= 0 && remove_idx < (int)s_tracked.size()) {
		DEBUGRemoveBreakPoint(s_tracked[remove_idx].pc, s_tracked[remove_idx].bank, s_tracked[remove_idx].x16Bank);
		DEBUGForgetBreakpoint(s_tracked[remove_idx].pc, s_tracked[remove_idx].bank, s_tracked[remove_idx].x16Bank);
		s_tracked.erase(s_tracked.begin() + remove_idx);
	}

	// --- Run to address ---------------------------------------------------
	// Sets a one-shot internal breakpoint and resumes; the CPU halts when it
	// reaches the address. Only meaningful while paused.
	//
	// Below the list, not above it: this leaves nothing behind and never
	// appears in the table, so leading with it made the panel's only address
	// box the one thing on it that does not set a breakpoint.
	ImGui::Separator();
	ImGui::TextUnformatted("Run to $");
	ImGui::SetItemTooltip("One-shot: resume, stop once at this address, and forget it.\n"
	                      "Nothing is added to the list above.");
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::SetNextItemWidth(dbgui_field_width("FFFF"));
	const bool runto_submit = ImGui::InputScalar("##runto", ImGuiDataType_S32, &s_runto_addr,
	                                             nullptr, nullptr, "%04X",
	                                             ImGuiInputTextFlags_CharsHexadecimal |
	                                                 ImGuiInputTextFlags_AutoSelectAll |
	                                                 ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SetItemTooltip("Address to run to, in HEX. Press Enter to go.");
	ImGui::SameLine();
	ImGui::BeginDisabled(!DEBUGIsPaused());
	if (ImGui::Button("Go") || (runto_submit && DEBUGIsPaused())) {
		DEBUGRunTo((uint16_t)(s_runto_addr & 0xFFFF), 0, DEBUG_OWNER_UI);
	}
	ImGui::EndDisabled();
	if (!DEBUGIsPaused()) {
		ImGui::SameLine();
		ImGui::TextDisabled("(pause first)");
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
		debug_wp_delete((uint16_t)remove_addr, remove_bank);
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
		                      "Type an address below, click the gutter in the Disassembly or\n"
		                      "Source panel, or press F9 on the cursor line. \"Run to $\" at the\n"
		                      "bottom is a one-shot stop that sets nothing.");
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
