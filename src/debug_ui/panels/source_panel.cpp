// Commander X16 Emulator — ImGui "Source" panel.
//
// Source-level view for the in-process Dear ImGui debugger (-imgui). Maps the
// CPU PC -> source file+line via the cc65 .dbg info and shows the source with
// the current line highlighted, following the PC as you step.
//
// Multiple source files can be open at once as TABS along the top: the file the
// PC is in is added + selected automatically (in Follow mode), and you can open
// any other file the debug info knows about via "Open..." to set breakpoints /
// run-to before the PC ever gets there. "Open..." can also load another .dbg
// module found on disk (the .dbg directory / fsroot) so code that will be LOADed
// later can be prepared ahead of time.
//
// Only this file is owned by the SOURCE panel. C emulator headers are pulled in
// via extern "C" (they lack C++ guards); `regs` + execution control come from
// debug_ui_bridge.h.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"  // regs, DEBUGRunTo/DEBUGIsPaused, breakpoints, dbg_info_label_to_addr
#include "debug_ui_widgets.h" // dbgui_value_lines
#include "debug_ui_insn_tooltip.h" // dbgui_instruction_tooltip_body

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <string>
#include <vector>

// --- C emulator core surface (headers have no C++ guards) ---------------------
extern "C" {
#include "dbg_info.h"    // dbg_info_* : PC<->source mapping + file enumeration
#include "source_view.h" // source_view_* : locate + cache source files on disk
#include "insn_info.h"   // insn_info_* : what an instruction does, for hover help
#include "disasm.h"      // disasm() : re-decode at the PC for the live preview
extern uint8_t *fsroot_path; // host FS root (main.c) — scanned for .dbg modules
}

// Breakpoint + bank surface (struct breakpoint, DEBUGAddBreakPoint,
// DEBUGRemoveBreakPoint, breakPoints, numBreakpoints, memory_get_ram_bank,
// memory_get_rom_bank) comes from debug_ui_bridge.h — do NOT redeclare it here.

// Case-insensitive compare; MSVC spells the POSIX one differently.
static int
dbgui_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb || !ca)
            return ca - cb;
    }
}

// --- Panel state --------------------------------------------------------------
static std::vector<std::string> s_tabs;          // open source-file tabs (by .dbg name)
static std::string              s_pcFile;         // file the PC currently maps to
static int                      s_pcLine     = 0; // 1-based PC line within s_pcFile
static std::string              s_lastPcFile;     // for edge-detecting PC movement
static int                      s_lastPcLine = 0;
static bool                     s_follow       = true; // auto-follow the PC while stepping
static bool                     s_addedDbgPath = false;

// Scroll request applied to whichever tab body renders this frame (only the
// selected tab renders). Follow requests it on the PC tab; Goto forces it.
//
// The request is tagged with the file it was issued for. Only the matching tab
// may consume it, so a request raised for a tab that isn't rendering yet (a
// cross-panel goto, or Go to PC while another tab is selected) survives until
// that tab actually draws instead of being swallowed by whichever tab happened
// to render first. It is also dropped if the target tab never appears, rather
// than being applied later to the wrong file.
static bool s_scrollRequest = false;
static bool s_scrollForce   = false;
static int  s_scrollTarget  = 0;
static std::string s_scrollFile;  // file the request belongs to ("" = any/current)
static int  s_scrollAge     = 0;  // frames the request has waited for its tab
static std::string s_selectFile;  // force-select this tab next frame (cross-panel goto)
static int  s_gotoLine      = 0;
static std::string s_activeFile;  // file whose body rendered last frame (the visible tab)

// Destination of the most recent deliberate navigation, highlighted on arrival
// so you can see where you were sent, then faded out so it does not linger as a
// second "current line".
static std::string s_flashFile;
static int         s_flashLine = 0;
static double      s_flashTime = 0.0;
constexpr double   SRC_FLASH_SECONDS = 3.0;

// Raise a scroll request for `file` (nullptr/"" = whichever tab is showing).
// `force` centres unconditionally rather than only when the line is off-screen.
// `flash` briefly highlights the destination; pass false when the line already
// carries the current-line highlight, which would make the flash redundant.
static void
src_request_scroll(const char *file, int line, bool force, bool flash = true)
{
    if (line < 1)
        return;
    s_scrollRequest = true;
    s_scrollForce   = force;
    s_scrollTarget  = line;
    s_scrollFile    = file ? file : "";
    s_scrollAge     = 0;

    // Deliberate navigation (goto line, go to PC, clicking a symbol or a call
    // frame) drops you in the middle of a file with nothing marking where you
    // landed. Flag the destination so it can be highlighted on arrival.
    if (force && flash) {
        s_flashFile = file ? file : "";
        s_flashLine = line;
        s_flashTime = ImGui::GetTime();
    }
}

// Basename of a path (after the last / or \).
static const char *
src_basename(const char *f)
{
    const char *b = f;
    for (const char *p = f; *p; ++p)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

// Add a file as a tab if not already open.
static void
src_ensure_tab(const char *file)
{
    if (!file || !file[0])
        return;
    for (const std::string &t : s_tabs)
        if (t == file)
            return;
    s_tabs.push_back(file);
}

// Find a user breakpoint at (addr, bank 0). Returns index or -1.
static int
src_find_bp(uint16_t addr)
{
    for (int i = 0; i < numBreakpoints; i++) {
        if (breakPoints[i].pc == (int)addr && breakPoints[i].bank == 0)
            return i;
    }
    return -1;
}

// Does a source line map to an address that currently has a breakpoint?
static bool
src_line_has_bp(const char *file, int line)
{
    if (numBreakpoints <= 0)
        return false;
    uint16_t addr;
    if (!debug_ui_source_to_addr(file, line, &addr))
        return false;
    return src_find_bp(addr) >= 0;
}

// Toggle a breakpoint on a 1-based source line.
static void
src_toggle_bp(const char *file, int line)
{
    uint16_t addr;
    if (!file || !file[0] || !debug_ui_source_to_addr(file, line, &addr))
        return; // line has no generated code — nothing to toggle
    // Resolve the bank first: it keys the remove as well as the add.
    int x16Bank = (addr >= 0xA000)
                      ? (addr < 0xC000 ? (int)memory_get_ram_bank()
                                       : (int)memory_get_rom_bank())
                      : -1;
    if (!DEBUGRemoveBreakPoint((int)addr, 0, x16Bank)) {
        struct breakpoint bp;
        bp.pc      = (int)addr;
        bp.bank    = 0;
        bp.x16Bank = x16Bank;
        DEBUGAddBreakPoint(bp);
    }
}

// Copy source text expanding tabs to 4-col stops and sanitising control bytes.
static void
src_expand_tabs(const char *s, char *out, size_t outsz)
{
    size_t col = 0;
    while (*s && col + 1 < outsz) {
        if (*s == '\t') {
            size_t next = (col + 4) & ~(size_t)3;
            while (col < next && col + 1 < outsz)
                out[col++] = ' ';
            s++;
        } else {
            unsigned char c = (unsigned char)*s++;
            out[col++] = (c < 0x20 || c > 0x7E) ? ' ' : (char)c;
        }
    }
    out[col] = '\0';
}

// The "Open..." quick-pick popup: source files known to the loaded .dbg, plus
// any other .dbg modules found on disk (which can be loaded so their sources
// become available before the code LOADs them).
static void
src_render_open_popup(void)
{
    if (!ImGui::BeginPopup("src_open"))
        return;

    ImGui::TextDisabled("Source files in the loaded debug info:");
    ImGui::Separator();
    int fc    = dbg_info_file_count();
    int shown = 0;
    for (int i = 0; i < fc; i++) {
        const char *nm = nullptr;
        if (!dbg_info_file_at(i, &nm) || !nm)
            continue;
        bool isOpen = false;
        for (const std::string &t : s_tabs)
            if (t == nm) { isOpen = true; break; }
        char lbl[300];
        snprintf(lbl, sizeof lbl, "%s%s##openf%d", nm, isOpen ? "   (open)" : "", i);
        if (ImGui::Selectable(lbl))
            src_ensure_tab(nm);
        shown++;
    }
    if (shown == 0)
        ImGui::TextDisabled("  (none)");

    // Other .dbg modules on disk (in the .dbg directory and the fsroot).
    static char paths[64][DBG_INFO_PATH_MAX];
    int         np = 0;
    np += dbg_info_scan_dbg_files(dbg_info_get_dbg_dir(), paths + np, 64 - np);
    if (fsroot_path)
        np += dbg_info_scan_dbg_files((const char *)fsroot_path, paths + np, 64 - np);
    if (np > 0) {
        ImGui::Separator();
        ImGui::TextDisabled("Load another .dbg module (for not-yet-loaded code):");
        for (int i = 0; i < np; i++) {
            char lbl[DBG_INFO_PATH_MAX + 24];
            snprintf(lbl, sizeof lbl, "Load %s##loaddbg%d", src_basename(paths[i]), i);
            if (ImGui::Selectable(lbl)) {
                if (dbg_info_load(paths[i]) == 0)
                    source_view_invalidate();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", paths[i]);
        }
    }

    ImGui::EndPopup();
}

// Resolve the identifier/number token under the mouse on a source line and, if
// it is a known label or a numeric literal, show a tooltip with its address and
// live value (or multi-base for a literal). `line` is the tab-expanded display
// string; `text_x` is its left edge; `glyph_w` the monospace advance.
static void
src_hover_token_tooltip(const char *line, float text_x, float glyph_w, bool is_pc_line)
{
    ImVec2 mp  = ImGui::GetIO().MousePos;
    float  rel = mp.x - text_x;
    if (rel < 0 || glyph_w <= 0)
        return;
    int col = (int)(rel / glyph_w);
    int len = (int)strlen(line);
    if (col < 0 || col >= len)
        return;

    auto is_ident = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '_' || c == '$';
    };
    if (!is_ident(line[col]))
        return;

    int s = col, e = col;
    while (s > 0 && is_ident(line[s - 1]))
        s--;
    while (e < len && is_ident(line[e]))
        e++;
    char tok[64];
    int  tl = 0;
    for (int k = s; k < e && tl < 63; k++)
        tok[tl++] = line[k];
    tok[tl] = '\0';
    if (tl == 0)
        return;

    // Numeric literal ($hex or decimal)?
    uint32_t num    = 0;
    bool     is_num = false;
    if (tok[0] == '$') {
        char *endp = nullptr;
        num        = (uint32_t)strtoul(tok + 1, &endp, 16);
        is_num     = (endp && *endp == '\0' && endp != tok + 1);
    } else if (tok[0] >= '0' && tok[0] <= '9') {
        char *endp = nullptr;
        num        = (uint32_t)strtoul(tok, &endp, 10);
        is_num     = (endp && *endp == '\0');
    }
    if (is_num) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tok);
        ImGui::Separator();
        dbgui_value_lines(num, num > 0xFF ? 2 : 1);
        ImGui::EndTooltip();
        return;
    }

    // An instruction mnemonic? Only treat it as one when it is the first token
    // on the line (after optional whitespace/label), which is where a mnemonic
    // lives in assembly source, so a variable called "and" isn't misread.
    const insn_info_t *insn = insn_info_lookup(tok);
    if (insn) {
        int first = 0;
        while (first < len && (line[first] == ' ' || line[first] == '\t'))
            first++;
        // Skip a leading "label:" if present.
        if (first < len) {
            int c = first;
            while (c < len && is_ident(line[c]))
                c++;
            if (c < len && line[c] == ':') {
                c++;
                while (c < len && (line[c] == ' ' || line[c] == '\t'))
                    c++;
                first = c;
            }
        }
        if (s != first)
            insn = nullptr; // not in mnemonic position
    }
    if (insn) {
        ImGui::BeginTooltip();
        dbgui_instruction_tooltip_body(tok, regs.pc, regs.k, is_pc_line);
        ImGui::EndTooltip();
        return;
    }

    // A name the debug info knows: a program label, or an equate (KERNAL entry
    // points, hardware registers, constants). Equates were previously invisible
    // here, which is why things like JOYGET resolved to nothing.
    uint16_t addr;
    int      kind = DBG_NAME_LABEL;
    if (debug_ui_name_to_value(tok, &addr, &kind)) {
        const bool is_equ = (kind == DBG_NAME_EQUATE);
        uint8_t    b      = debug_ui_read6502(addr, 0, DEBUG_UI_CURRENT_BANK);
        uint8_t    b2     = debug_ui_read6502((uint16_t)(addr + 1), 0, DEBUG_UI_CURRENT_BANK);
        uint16_t   w      = (uint16_t)(b | (b2 << 8));

        ImGui::BeginTooltip();
        ImGui::Text("%s  =  $%04X", tok, addr);
        ImGui::SameLine();
        ImGui::TextDisabled(is_equ ? "(equate)" : "(label)");

        // A label the disassembler also knows about at that address is worth
        // naming, since equates often alias a routine entry point.
        const char *at = nullptr;
        if (dbg_info_addr_to_label(addr, &at) && at && dbgui_stricmp(at, tok) != 0) {
            ImGui::TextDisabled("also known as %s", at);
        }

        ImGui::Separator();
        ImGui::TextDisabled("byte [$%04X]", addr);
        dbgui_value_lines(b, 1);
        ImGui::Separator();
        ImGui::TextDisabled("word [$%04X] (LE)", addr);
        dbgui_value_lines(w, 2);
        ImGui::EndTooltip();
    }
}

// Render one file's source body inside its tab. `isPcFile` marks the tab the PC
// is currently in (gets the current-line highlight + follow scrolling).
static void
src_render_body(const char *file, bool isPcFile, int pcLine, bool pcMoved)
{
    s_activeFile = file ? file : "";
    const ImVec4 orange(1.0f, 0.78f, 0.35f, 1.0f);

    const source_file_t *sf        = source_view_get(file);
    int                  lineCount = (sf && sf->found) ? sf->count : 0;

    if (!sf || !sf->found) {
        ImGui::TextColored(orange, "%s   (source file not found)", src_basename(file));
        ImGui::TextWrapped("Could not locate '%s' on disk. Add a search directory "
                           "with -srcpath <dir>.",
                           file);
        return;
    }

    if (isPcFile)
        ImGui::TextColored(orange, "%s   line %d / %d", src_basename(file), pcLine, lineCount);
    else
        ImGui::TextColored(orange, "%s   %d lines", src_basename(file), lineCount);
    ImGui::Separator();

    // Follow: recenter on the current line when the PC moves (PC tab only).
    //
    // Stepping and breaking want different things. While stepping, the line
    // creeps along and yanking the view to centre on every step is disorienting,
    // so only scroll once it would leave the window. But arriving at a
    // breakpoint, a break-from-code (STP / DEBUGBreakToDebugger), a watchpoint
    // or a manual pause is a jump to somewhere new - there the line must be
    // centred, or it turns up wherever the old scroll position happened to leave
    // it and you have to hunt for the highlight.
    if (isPcFile && s_follow && pcMoved) {
        const char *why = debug_server_last_stop_reason();
        const bool  stepping = (why && strcmp(why, "step") == 0);
        src_request_scroll(file, pcLine, !stepping, /*flash=*/false);
    }

    ImGui::BeginChild("srcbody", ImVec2(0, 0), 0, ImGuiWindowFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    ImDrawList *dl   = ImGui::GetWindowDrawList();
    float       rowH = ImGui::GetTextLineHeight();

    char maxnum[16];
    snprintf(maxnum, sizeof maxnum, "%d", lineCount > 0 ? lineCount : 1);
    float markerW = ImGui::CalcTextSize("0").x * 1.8f;
    float digitsW = ImGui::CalcTextSize(maxnum).x;
    float spaceW  = ImGui::CalcTextSize(" ").x;
    float glyphW  = ImGui::CalcTextSize("0").x; // monospace advance, for hover hit-testing
    float textX   = markerW + digitsW + spaceW * 2.0f;

    const ImU32 colNum  = IM_COL32(130, 130, 130, 255);
    const ImU32 colText = IM_COL32(210, 210, 210, 255);
    const ImU32 colCur  = IM_COL32(255, 240, 120, 255);
    const ImU32 curBg   = IM_COL32(38, 60, 120, 255);
    const ImU32 hoverBg = IM_COL32(70, 70, 70, 150);
    const ImU32 bpCol   = IM_COL32(230, 70, 70, 255);

    // Reveal a requested line (follow recenters only when off-screen; goto forces).
    // Only the tab the request was raised for may consume it, so a request for a
    // tab that hasn't rendered yet isn't swallowed here. Clamp to the file's
    // length so an out-of-range line still lands somewhere sensible.
    if (s_scrollRequest && s_scrollTarget >= 1 &&
        (s_scrollFile.empty() || s_scrollFile == file)) {
        int target_line = s_scrollTarget;
        if (target_line > lineCount)
            target_line = lineCount > 0 ? lineCount : 1;

        float childH  = ImGui::GetWindowHeight();
        float sY      = ImGui::GetScrollY();
        float top     = (target_line - 1) * rowH;
        bool  visible = (top >= sY + rowH) && (top <= sY + childH - 2 * rowH);
        if (s_scrollForce || !visible) {
            float target = top - childH * 0.5f + rowH * 0.5f;
            if (target < 0)
                target = 0;
            ImGui::SetScrollY(target);
        }
        s_scrollRequest = false;
        s_scrollFile.clear();
    }

    ImGuiListClipper clipper;
    clipper.Begin(lineCount, rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            int  lineNo = i + 1;
            bool isCur  = isPcFile && (lineNo == pcLine);
            // Recently navigated-to line in this file (fades out).
            const double flashAge = ImGui::GetTime() - s_flashTime;
            const bool   isFlash  = (s_flashLine == lineNo) && (s_flashFile == file) &&
                                  (flashAge >= 0.0) && (flashAge < SRC_FLASH_SECONDS);

            ImGui::PushID(i);
            ImVec2 p0   = ImGui::GetCursorScreenPos();
            float  rowW = ImGui::GetContentRegionAvail().x;
            if (rowW < textX + 10.0f)
                rowW = textX + 10.0f;

            bool pressed = ImGui::InvisibleButton("row", ImVec2(rowW, rowH));
            bool hovered = ImGui::IsItemHovered();

            // Right-click a line: run-to / toggle breakpoint (works before the
            // PC ever reaches this file, so breakpoints can be pre-armed).
            if (ImGui::BeginPopupContextItem("srcctx")) {
                uint16_t addr;
                bool     mappable = debug_ui_source_to_addr(file, lineNo, &addr);
                ImGui::BeginDisabled(!mappable || !DEBUGIsPaused());
                if (ImGui::MenuItem("Run to here"))
                    DEBUGRunTo(addr, 0);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!mappable);
                if (ImGui::MenuItem("Toggle breakpoint"))
                    src_toggle_bp(file, lineNo);
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }

            // Row background: current line, then the just-navigated-to line,
            // then hover feedback. The navigation flash fades so it can't be
            // mistaken for a second current line.
            if (isCur) {
                dl->AddRectFilled(p0, ImVec2(p0.x + rowW, p0.y + rowH), curBg);
            } else if (isFlash) {
                const float k = 1.0f - (float)(flashAge / SRC_FLASH_SECONDS);
                const int   a = (int)(40.0f + 150.0f * k);
                dl->AddRectFilled(p0, ImVec2(p0.x + rowW, p0.y + rowH),
                                  IM_COL32(150, 110, 30, a));
                dl->AddRect(p0, ImVec2(p0.x + rowW, p0.y + rowH),
                            IM_COL32(240, 200, 90, (int)(80.0f + 175.0f * k)));
            } else if (hovered) {
                dl->AddRectFilled(p0, ImVec2(p0.x + rowW, p0.y + rowH), hoverBg);
            }

            // Breakpoint marker (filled) / hover hint (hollow) in the gutter.
            // The hint only appears while the mouse is actually over the gutter,
            // since that is the only place a click sets a breakpoint.
            ImVec2 center(p0.x + markerW * 0.5f, p0.y + rowH * 0.5f);
            float  radius = rowH * 0.30f;
            const float mouseX  = ImGui::GetIO().MousePos.x - p0.x;
            const bool  inGutter = hovered && mouseX >= 0.0f && mouseX < markerW;
            if (src_line_has_bp(file, lineNo)) {
                dl->AddCircleFilled(center, radius, bpCol);
            } else if (inGutter) {
                uint16_t tmp;
                if (debug_ui_source_to_addr(file, lineNo, &tmp))
                    dl->AddCircle(center, radius, IM_COL32(230, 70, 70, 150));
            }

            // Right-aligned line number.
            char num[16];
            snprintf(num, sizeof num, "%d", lineNo);
            float numRight = p0.x + markerW + digitsW;
            float numW     = ImGui::CalcTextSize(num).x;
            dl->AddText(ImVec2(numRight - numW, p0.y), colNum, num);

            // Source text (current line highlighted).
            char buf[1024];
            src_expand_tabs(sf->lines[lineNo - 1], buf, sizeof buf);
            dl->AddText(ImVec2(p0.x + textX, p0.y), isCur ? colCur : colText, buf);

            // Hover a mnemonic/label/number token → tooltip explaining it. On
            // the PC's line the instruction tooltip also predicts the result.
            if (hovered)
                src_hover_token_tooltip(buf, p0.x + textX, glyphW, isCur);

            ImGui::PopID();

            // Only a click in the gutter toggles a breakpoint. The row-wide
            // button stays (it drives hover feedback, the context menu and the
            // hover tooltips), but clicking the text itself must not arm a
            // breakpoint - that made it far too easy to set one by accident
            // while just clicking around the source.
            if (pressed) {
                const float clickX = ImGui::GetIO().MousePos.x - p0.x;
                if (clickX >= 0.0f && clickX < markerW)
                    src_toggle_bp(file, lineNo);
            }
        }
    }
    clipper.End();

    ImGui::PopStyleVar();
    ImGui::EndChild();
}

static void
source_panel_render(bool *p_open)
{
    if (!ImGui::Begin("Source", p_open)) {
        dbgui_window_end();
        return;
    }
    dbgui_window_zoom("source"); // Ctrl+wheel zooms this window's text only

    const ImVec4 orange(1.0f, 0.78f, 0.35f, 1.0f);

    // No debug info at all: explain how to get some, render nothing else.
    if (!dbg_info_is_loaded()) {
        ImGui::TextColored(orange, "No debug info loaded.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "The Source view needs cc65 debug info (a .dbg file) that maps "
            "addresses to source lines.");
        ImGui::Spacing();
        ImGui::BulletText("Start the emulator with:  -dbgfile <program.dbg>");
        ImGui::BulletText("If the .s/.c files aren't beside the .dbg:  -srcpath <dir>");
        ImGui::BulletText("A LOADed program's matching .dbg is picked up automatically.");
        dbgui_window_end();
        return;
    }

    // Resolve PC -> source. Only the non-banked program bank (regs.k == 0) is
    // covered by the 16-bit .dbg map; banked/ROM code is not.
    const char *pcf     = nullptr;
    int         pcl     = 0;
    bool        mapping = (regs.k == 0) &&
                   dbg_info_addr_to_source_banked(regs.pc, (int)memory_get_ram_bank(), &pcf, &pcl);

    // The PC often sits in code with no debug info: the KERNAL, or a small
    // trampoline copied into low RAM (e.g. JSRFAR at $02xx used for far calls).
    // Showing nothing at all leaves no clue where execution is, so fall back to
    // the nearest return address on the stack that DOES map and display that,
    // clearly flagged as the calling line rather than the PC.
    bool     viaCaller = false;
    uint16_t callerPc  = 0;
    if (!mapping) {
        const bool native = regs.is65c816 && !regs.e;
        uint32_t   sp     = regs.sp;
        uint32_t   from   = native ? (sp + 1) : (0x100 + (sp & 0xFF) + 1);
        uint32_t   to     = native ? (sp + 1 + 256) : 0x200;
        for (uint32_t a = from; a + 1 < to && !viaCaller; a++) {
            uint8_t  lo     = debug_ui_read6502((uint16_t)a, 0, DEBUG_UI_CURRENT_BANK);
            uint8_t  hi     = debug_ui_read6502((uint16_t)(a + 1), 0, DEBUG_UI_CURRENT_BANK);
            uint16_t pushed = (uint16_t)(lo | (hi << 8));
            uint8_t  opc    = debug_ui_read6502((uint16_t)(pushed - 2), regs.k, DEBUG_UI_CURRENT_BANK);
            if (opc != 0x20 && opc != 0xFC)
                continue;                       // not a JSR return frame
            uint16_t ret = (uint16_t)(pushed + 1);
            const char *cf = nullptr;
            int         cl = 0;
            if (dbg_info_addr_to_source_banked(ret, (int)memory_get_ram_bank(), &cf, &cl) && cf) {
                pcf = cf; pcl = cl; callerPc = ret;
                viaCaller = true;
            }
        }
    }

    if ((mapping || viaCaller) && pcf) {
        s_pcFile = pcf;
        s_pcLine = pcl;
        if (!s_addedDbgPath) {
            source_view_add_path(dbg_info_get_dbg_dir());
            s_addedDbgPath = true;
        }
        src_ensure_tab(s_pcFile.c_str()); // keep the PC's file open as a tab
    }

    bool pcMoved = (mapping || viaCaller) && (s_pcLine != s_lastPcLine || s_pcFile != s_lastPcFile);

    // Cross-panel goto (clicking a Call Stack / Symbols / Breakpoints entry):
    // open that address's source file and jump to its line, so selecting a
    // frame lands you in the code rather than only moving the memory view.
    {
        uint16_t gaddr;
        uint8_t  gbank;
        if (debug_ui_peek_goto(&gaddr, &gbank)) {
            const char *gf = nullptr;
            int         gl = 0;
            if (dbg_info_addr_to_source_banked(gaddr, (int)memory_get_ram_bank(), &gf, &gl) && gf) {
                if (!s_addedDbgPath) {
                    source_view_add_path(dbg_info_get_dbg_dir());
                    s_addedDbgPath = true;
                }
                src_ensure_tab(gf);
                s_selectFile    = gf;
                src_request_scroll(gf, gl, true);
            }
        }
    }

    // --- Toolbar ---
    ImGui::Checkbox("Follow PC", &s_follow);
    ImGui::SameLine();
    if (ImGui::Button("Go to PC")) {
        // Jump to wherever the PC maps right now, without changing Follow PC.
        if ((mapping || viaCaller) && !s_pcFile.empty()) {
            src_ensure_tab(s_pcFile.c_str());
            s_selectFile    = s_pcFile;
            src_request_scroll(s_pcFile.c_str(), s_pcLine, true);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Jump to the line the PC maps to (or its nearest\n"
                          "mapped caller when the PC has no source).");
    ImGui::SameLine(0, 16);
    if (ImGui::Button("Open..."))
        ImGui::OpenPopup("src_open");
    src_render_open_popup();
    ImGui::SameLine(0, 16);
    ImGui::TextUnformatted("Goto line:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    bool go = false;
    if (ImGui::InputInt("##gotoln", &s_gotoLine, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
        go = true;
    ImGui::SameLine();
    if (ImGui::Button("Go"))
        go = true;
    if (go && s_gotoLine >= 1) {
        // Target the tab that is actually showing (recorded when it last
        // rendered), so the jump lands in the file the user is looking at
        // instead of whichever tab happens to draw next. Fall back to the PC's
        // file when nothing has rendered yet.
        const std::string &target = !s_activeFile.empty() ? s_activeFile : s_pcFile;
        if (!target.empty()) {
            src_ensure_tab(target.c_str());
            s_selectFile = target;
            src_request_scroll(target.c_str(), s_gotoLine, true);
        } else {
            src_request_scroll(nullptr, s_gotoLine, true);
        }
    }
    if (!mapping && regs.k != 0) {
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("PC in bank %02X (no source map)", regs.k);
    } else if (viaCaller) {
        ImGui::SameLine(0, 16);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                           "PC $%04X has no source (KERNAL/trampoline) — showing caller $%04X",
                           regs.pc, callerPc);
    } else if (!mapping) {
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("PC $%04X — no source mapping", regs.pc);
    }

    ImGui::Separator();

    if (s_tabs.empty()) {
        ImGui::TextWrapped("No source file open yet. Use \"Open...\" to pick one, or "
                           "step into code covered by the loaded .dbg.");
        s_lastPcLine = s_pcLine;
        s_lastPcFile = s_pcFile;
        dbgui_window_end();
        return;
    }

    // --- Tabs: one per open source file ---
    int closeIdx = -1;
    if (ImGui::BeginTabBar("src_tabs",
                           ImGuiTabBarFlags_AutoSelectNewTabs |
                               ImGuiTabBarFlags_Reorderable |
                               ImGuiTabBarFlags_DrawSelectedOverline |
                               ImGuiTabBarFlags_TabListPopupButton)) {
        for (int i = 0; i < (int)s_tabs.size(); i++) {
            const std::string &f    = s_tabs[i];
            bool               isPc = (mapping || viaCaller) && (f == s_pcFile);

            ImGuiTabItemFlags flags = 0;
            if (s_follow && isPc && pcMoved)
                flags |= ImGuiTabItemFlags_SetSelected; // snap to PC while following
            if (!s_selectFile.empty() && f == s_selectFile)
                flags |= ImGuiTabItemFlags_SetSelected; // cross-panel goto target

            // Display shows a "> " marker on the PC's file; the ID (after ###)
            // is the stable full name so toggling the marker never recreates it.
            char label[300];
            snprintf(label, sizeof label, "%s%s###srctab%s", isPc ? "> " : "",
                     src_basename(f.c_str()), f.c_str());

            bool open = true;
            ImGui::PushID(f.c_str());
            if (ImGui::BeginTabItem(label, &open, flags)) {
                src_render_body(f.c_str(), isPc, s_pcLine, pcMoved);
                ImGui::EndTabItem();
            }
            ImGui::PopID();
            if (!open)
                closeIdx = i;
        }
        ImGui::EndTabBar();
    }
    s_selectFile.clear(); // one-shot: only force tab selection for this frame
    if (closeIdx >= 0)
        s_tabs.erase(s_tabs.begin() + closeIdx);

    // Drop a scroll request whose target tab never rendered (closed, or never
    // opened) instead of letting it fire later against an unrelated file.
    if (s_scrollRequest && ++s_scrollAge > 4) {
        s_scrollRequest = false;
        s_scrollFile.clear();
    }

    s_lastPcLine = s_pcLine;
    s_lastPcFile = s_pcFile;

    dbgui_window_end();
}

static DebugPanelRegistration s_reg("Source", source_panel_render, true);
