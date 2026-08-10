// Commander X16 Emulator — ImGui Disassembly panel.
//
// Renders an anchored, bank-aware 65C02/65C816 disassembly around the current
// PC using the code_map core (src/code_map.*): instruction boundaries are
// aligned from live-execution coverage + recorded M/X/E flags, so the listing
// stays correct even through variable-width 65C816 code. Features:
//   * follow-PC auto-centering (toggle) with current-instruction highlight,
//   * a clickable address gutter that toggles breakpoints (with markers),
//   * goto-address, mouse-wheel scrolling, and bytes + mnemonic + effective addr.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_widgets.h" // dbgui_value_lines
#include "debug_ui_insn_tooltip.h" // dbgui_instruction_tooltip_body

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

namespace {

bool     s_follow_pc = true;   // auto-scroll to the current instruction
uint16_t s_view_center = 0;    // address the listing is anchored on
bool     s_view_valid = false; // has s_view_center been initialised?
char     s_goto_buf[8] = "";   // hex goto input
char     s_sym_buf[64] = "";   // go-to-symbol (label name) input
bool     s_show_labels = true; // annotate the listing with .dbg labels

// The RAM/ROM bank window an address reads from (mirrors getCurrentBank()).
int
x16bank_for(uint16_t addr, uint8_t bank, uint8_t rambank, uint8_t rombank)
{
	if (addr >= 0xA000 && bank == 0)
		return addr < 0xC000 ? (int)rambank : (int)rombank;
	return -1;
}

// Index of a breakpoint at (pc, bank, x16bank), or -1.
//
// Delegates to the core rather than re-walking breakPoints[] here. Comparing pc
// and bank only -- which this did -- ignores the bank selector that identifies a
// breakpoint, so the gutter drew a dot for a breakpoint set in a different RAM
// bank; clicking it then failed to find one to remove and silently added a
// second, once per bank the machine happened to be in.
int
find_breakpoint(int pc, uint8_t bank, int x16bank)
{
	return debug_bp_find(pc, bank, x16bank);
}

void
toggle_breakpoint(uint16_t addr, uint8_t bank, int x16bank)
{
	if (!DEBUGRemoveBreakPoint((int)addr, bank, x16bank)) {
		struct breakpoint bp;
		bp.pc      = (int)addr;
		bp.bank    = bank;
		bp.x16Bank = x16bank;
		DEBUGAddBreakPoint(bp);
	}
}

// Advance `center` by `delta` instructions (negative = backward).
uint16_t
move_center(uint16_t center, int delta, uint8_t bank, uint8_t rambank, uint8_t rombank)
{
	while (delta < 0) {
		center = code_map_prev_instruction(center, bank, rambank, rombank);
		delta++;
	}
	while (delta > 0) {
		code_map_line_t ln;
		uint16_t        next = center;
		code_map_disasm_forward(center, bank, rambank, rombank, 1, &ln, 1, &next);
		center = next;
		delta--;
	}
	return center;
}

void
disasm_panel_render(bool *p_open)
{
	if (!ImGui::Begin("Disassembly", p_open)) {
		dbgui_window_end();
		return;
	}
	dbgui_window_zoom("disasm"); // Ctrl+wheel zooms this window's text only

	const uint16_t pc      = regs.pc;
	const uint8_t  bank    = regs.k;
	const uint8_t  rambank = memory_get_ram_bank();
	const uint8_t  rombank = memory_get_rom_bank();

	if (!s_view_valid) {
		s_view_center = pc;
		s_view_valid  = true;
	}

	// Honor a cross-panel goto request (from Symbols / Call Stack panels).
	{
		uint16_t gaddr;
		uint8_t  gbank;
		if (debug_ui_peek_goto(&gaddr, &gbank)) {
			s_view_center = gaddr;
			s_follow_pc   = false;
		}
	}

	// ── Toolbar ──────────────────────────────────────────────────────────
	ImGui::Checkbox("Follow PC", &s_follow_pc);
	ImGui::SameLine();
	if (ImGui::Button("Go to PC")) {
		s_view_center = pc;
		s_follow_pc   = true;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70.0f);
	if (ImGui::InputText("Goto", s_goto_buf, sizeof(s_goto_buf),
	                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
		if (s_goto_buf[0] != '\0') {
			s_view_center = (uint16_t)strtoul(s_goto_buf, nullptr, 16);
			s_follow_pc   = false;
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("K:%02X RAM:%02X ROM:%02X", bank, rambank, rombank);

	// Go-to-symbol + label toggle (cc65 .dbg labels).
	ImGui::SetNextItemWidth(120.0f);
	if (ImGui::InputTextWithHint("##sym", "label", s_sym_buf, sizeof(s_sym_buf),
	                             ImGuiInputTextFlags_EnterReturnsTrue)) {
		uint16_t sym_addr;
		if (s_sym_buf[0] != '\0' && debug_ui_label_to_addr(s_sym_buf, &sym_addr)) {
			s_view_center = sym_addr;
			s_follow_pc   = false;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Go to sym")) {
		uint16_t sym_addr;
		if (s_sym_buf[0] != '\0' && debug_ui_label_to_addr(s_sym_buf, &sym_addr)) {
			s_view_center = sym_addr;
			s_follow_pc   = false;
		}
	}
	ImGui::SameLine();
	ImGui::Checkbox("Labels", &s_show_labels);

	if (s_follow_pc)
		s_view_center = pc;

	ImGui::Separator();

	// ── Listing ──────────────────────────────────────────────────────────
	ImGui::BeginChild("disasm_lines", ImVec2(0, 0), false, ImGuiWindowFlags_NoMove);

	// Fill the visible area: split the rows above/below the anchor so the
	// centered instruction sits in the middle.
	const float line_h = ImGui::GetTextLineHeightWithSpacing();
	int rows = (int)(ImGui::GetContentRegionAvail().y / (line_h > 0 ? line_h : 1.0f));
	if (rows < 8)
		rows = 8;
	if (rows > 500)
		rows = 500;
	const int before = rows / 2;
	const int after  = rows - before;

	static code_map_line_t lines[512];
	int center_index = -1;
	int n = code_map_disasm_window(s_view_center, bank, rambank, rombank,
	                               before, after, lines, 512, &center_index);
	(void)center_index;

	ImDrawList  *draw   = ImGui::GetWindowDrawList();
	const float  glyph  = ImGui::CalcTextSize("0").x;
	const float  gutter = glyph * 2.0f;

	bool cursor_set = false; // did the mouse hover an instruction row this frame?
	for (int i = 0; i < n; i++) {
		const code_map_line_t &ln = lines[i];
		const bool is_pc  = (ln.addr == pc);
		// Same bank the toggle below uses, so the dot and the click agree.
		const int  ln_x16 = x16bank_for(ln.addr, bank, rambank, rombank);
		const int  bpidx  = find_breakpoint((int)ln.addr, bank, ln_x16);
		const bool has_bp = (bpidx >= 0);

		ImGui::PushID(i);

		// Clickable gutter → toggle breakpoint on this address.
		const ImVec2 row_pos = ImGui::GetCursorScreenPos();
		if (ImGui::InvisibleButton("bp", ImVec2(gutter, line_h))) {
			toggle_breakpoint(ln.addr, bank, ln_x16);
		}
		if (has_bp) {
			const ImVec2 c(row_pos.x + gutter * 0.5f, row_pos.y + line_h * 0.5f);
			draw->AddCircleFilled(c, glyph * 0.45f, IM_COL32(230, 60, 60, 255));
		}

		// Build the fixed-width text: bytes + mnemonic (+ effective address).
		char bytes[16];
		int  bl = 0;
		for (int b = 0; b < ln.size && b < 4; b++)
			bl += snprintf(bytes + bl, sizeof(bytes) - (size_t)bl, "%02X ", ln.bytes[b]);
		for (; bl < 12 && bl < (int)sizeof(bytes) - 1; bl++)
			bytes[bl] = ' ';
		bytes[bl] = '\0';

		char eff[48] = "";
		if (ln.eff_addr >= 0) {
			const char *elabel = nullptr;
			if (s_show_labels && dbg_info_addr_to_label((uint16_t)ln.eff_addr, &elabel) && elabel) {
				snprintf(eff, sizeof(eff), "  =%04X %s", (unsigned)ln.eff_addr, elabel);
			} else {
				snprintf(eff, sizeof(eff), "  =%04X", (unsigned)ln.eff_addr);
			}
		}

		// Label at this instruction's own address, shown comment-style at the
		// end so the fixed-width columns stay aligned.
		char lbl[40] = "";
		const char *linelabel = nullptr;
		if (s_show_labels && dbg_info_addr_to_label(ln.addr, &linelabel) && linelabel) {
			snprintf(lbl, sizeof(lbl), "  ; %s", linelabel);
		}

		char text[160];
		snprintf(text, sizeof(text), "%04X: %s %-18s%s%s", ln.addr, bytes, ln.text, eff, lbl);

		// Row on the same line as the gutter; the Selectable also toggles the
		// breakpoint so clicking anywhere on the address line works.
		ImGui::SameLine(0.0f, 0.0f);
		ImVec4 col = is_pc       ? ImVec4(1.00f, 0.92f, 0.40f, 1.0f)   // current PC
		           : ln.recorded ? ImVec4(0.86f, 0.86f, 0.86f, 1.0f)   // executed
		                         : ImVec4(0.55f, 0.55f, 0.58f, 1.0f);  // inferred
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		if (ImGui::Selectable(text, is_pc)) {
			toggle_breakpoint(ln.addr, bank, x16bank_for(ln.addr, bank, rambank, rombank));
		}
		ImGui::PopStyleColor();

		// Publish the hovered instruction as the Ctrl+F10 "run to cursor" target.
		if (ImGui::IsItemHovered()) {
			debug_ui_set_cursor(ln.addr, bank, true);
			cursor_set = true;

			// Same instruction help the Source panel gives, so hovering here
			// explains the instruction and — on the PC's row — predicts what it
			// will do. Followed by the operand's live value.
			char mnem[16];
			const bool have_mnem = dbgui_mnemonic_of(ln.text, mnem, sizeof mnem);

			if (have_mnem || ln.eff_addr >= 0) {
				ImGui::BeginTooltip();
				if (have_mnem) {
					dbgui_instruction_tooltip_body(mnem, ln.addr, bank, is_pc);
				}
				if (ln.eff_addr >= 0) {
					uint16_t ea = (uint16_t)ln.eff_addr;
					uint8_t  b  = debug_ui_read6502(ea, 0, DEBUG_UI_CURRENT_BANK);
					uint8_t  b2 = debug_ui_read6502((uint16_t)(ea + 1), 0, DEBUG_UI_CURRENT_BANK);
					uint16_t w  = (uint16_t)(b | (b2 << 8));
					const char *elabel = nullptr;
					if (have_mnem)
						ImGui::Separator();
					if (dbg_info_addr_to_label(ea, &elabel) && elabel)
						ImGui::Text("[$%04X]  %s", ea, elabel);
					else
						ImGui::Text("[$%04X]", ea);
					ImGui::Separator();
					ImGui::TextDisabled("byte");
					dbgui_value_lines(b, 1);
					ImGui::Separator();
					ImGui::TextDisabled("word (LE)");
					dbgui_value_lines(w, 2);
				}
				ImGui::EndTooltip();
			}
		}

		// Right-click an instruction → run to it (only meaningful while paused).
		if (ImGui::BeginPopupContextItem("disasm_ctx")) {
			ImGui::BeginDisabled(!DEBUGIsPaused());
			if (ImGui::MenuItem("Run to here")) {
				DEBUGRunTo(ln.addr, bank);
			}
			ImGui::EndDisabled();
			if (ImGui::MenuItem("Toggle breakpoint")) {
				toggle_breakpoint(ln.addr, bank, x16bank_for(ln.addr, bank, rambank, rombank));
			}
			ImGui::EndPopup();
		}

		if (is_pc && (s_follow_pc || ImGui::IsWindowAppearing()))
			ImGui::SetScrollHereY(0.5f);

		ImGui::PopID();
	}

	if (!cursor_set)
		debug_ui_set_cursor(0, 0, false); // nothing hovered → invalidate run-to-cursor

	// Mouse-wheel scrolling re-anchors the view (and drops follow-PC).
	if (ImGui::IsWindowHovered()) {
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.0f) {
			s_follow_pc   = false;
			s_view_center = move_center(s_view_center, (int)(-wheel * 3.0f), bank, rambank, rombank);
		}
	}

	ImGui::EndChild();
	dbgui_window_end();
}

DebugPanelRegistration s_reg("Disassembly", disasm_panel_render, true);

} // namespace

