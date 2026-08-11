// Commander X16 Emulator — shared instruction tooltip for the debugger panels.
//
// Both the Source and Disassembly panels want the same answer to "what does
// this instruction do, and what will it do right now": a description from
// insn_info.c, the flags it touches, and — when the instruction is the one at
// the PC — a prediction of its effect.
//
// The prediction is strictly read-only. It re-decodes at the address with
// disasm() and reads operands through debug_ui_read6502(), which is the
// side-effect-free debug path, so hovering can never perturb VERA/VIA state.
//
// Header-only, like debug_ui_widgets.h, so it needs no CMake entry.
#ifndef DEBUG_UI_INSN_TOOLTIP_H
#define DEBUG_UI_INSN_TOOLTIP_H

#include "imgui.h"
#include "debug_ui_bridge.h"

#include <stdint.h>
#include <ctype.h>

extern "C" {
#include "insn_info.h"
#include "disasm.h"
#include "dbg_info.h"
}

// Body of the instruction tooltip (no Begin/EndTooltip — the caller owns that,
// so it can combine this with other content).
//
//   mnemonic  : the instruction's mnemonic, as disasm.c spells it
//   addr/bank : where the instruction lives
//   at_pc     : predict the outcome (only meaningful when this IS the PC)
static inline void
dbgui_instruction_tooltip_body(const char *mnemonic, uint16_t addr, uint8_t bank, bool at_pc)
{
    const insn_info_t *insn = insn_info_lookup(mnemonic);
    if (!insn) {
        return;
    }

    ImGui::Text("%s - %s", insn->mnemonic, insn->title);
    ImGui::TextDisabled("%s", insn->summary);

    char flags[32];
    insn_info_flag_text(insn->flags_set, flags, sizeof flags);
    if (flags[0]) {
        ImGui::Separator();
        ImGui::TextDisabled("affects: %s", flags);
    }

    if (!at_pc) {
        return;
    }

    char          dis[128];
    int32_t       eff     = -1;
    const int16_t x16bank = (int16_t)DEBUG_UI_CURRENT_BANK;
    const int     nbytes  = disasm(addr, bank, nullptr, dis, sizeof dis, x16bank,
                                   regs.status, &eff);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "At the PC: %s", dis);

    const bool acc16 = regs.is65c816 && !regs.e && !(regs.status & FLAG_MEMORY_WIDTH);
    const bool idx16 = regs.is65c816 && !regs.e && !(regs.status & FLAG_INDEX_WIDTH);
    const int  w     = acc16 ? 4 : 2;

    uint16_t memval   = 0;
    bool     have_mem = false;
    if (eff >= 0) {
        uint8_t lo = debug_ui_read6502((uint16_t)eff, 0, x16bank);
        uint8_t hi = debug_ui_read6502((uint16_t)(eff + 1), 0, x16bank);
        memval     = acc16 ? (uint16_t)(lo | (hi << 8)) : lo;
        have_mem   = true;
        ImGui::TextDisabled("operand $%04X = $%0*X", (unsigned)eff, w, memval);
    }

    const ImVec4 will(0.45f, 0.95f, 0.45f, 1.0f);
    const ImVec4 wont(0.95f, 0.70f, 0.30f, 1.0f);
    const char   m0 = (char)tolower((unsigned char)mnemonic[0]);
    const char   m1 = mnemonic[1] ? (char)tolower((unsigned char)mnemonic[1]) : '\0';
    const char   m2 = mnemonic[1] && mnemonic[2] ? (char)tolower((unsigned char)mnemonic[2]) : '\0';

    switch (insn->kind) {
    case INSN_KIND_BRANCH:
        if (insn->branch_flag) {
            const uint8_t f     = insn->branch_flag;
            const bool    set   = (regs.status & f) != 0;
            const bool    taken = (set == insn->branch_when_set);
            const char   *fname = f == INSN_FLAG_Z   ? "Z"
                                  : f == INSN_FLAG_C ? "C"
                                  : f == INSN_FLAG_N ? "N"
                                                     : "V";
            // disasm() reports no effective address for relative branches, so
            // work the target out from the displacement.
            uint16_t target = (uint16_t)(eff >= 0 ? eff : 0);
            if (eff < 0 && nbytes >= 2) {
                const int8_t d = (int8_t)debug_ui_read6502((uint16_t)(addr + 1), bank, x16bank);
                target = (uint16_t)(addr + nbytes + d);
            }
            ImGui::Text("tests %s (currently %d), branches when %s", fname, set ? 1 : 0,
                        insn->branch_when_set ? "set" : "clear");
            if (taken)
                ImGui::TextColored(will, "WILL branch to $%04X", (unsigned)target);
            else
                ImGui::TextColored(wont, "will NOT branch - continues at $%04X",
                                   (unsigned)((addr + nbytes) & 0xFFFF));
            const char *tn = nullptr;
            if (taken && dbg_info_addr_to_label(target, &tn) && tn)
                ImGui::TextDisabled("target: %s", tn);
            ImGui::TextDisabled("A=$%02X X=$%02X Y=$%02X P=$%02X", regs.a, regs.xl, regs.yl,
                                regs.status);
        }
        break;

    case INSN_KIND_LOAD:
        if (have_mem) {
            const char *reg = m2 == 'a' ? "A" : (m2 == 'x' ? "X" : "Y");
            ImGui::TextColored(will, "%s will become $%0*X", reg, w, memval);
            ImGui::TextDisabled("then Z=%d N=%d", memval == 0 ? 1 : 0,
                                (memval & (acc16 ? 0x8000 : 0x80)) ? 1 : 0);
        }
        break;

    case INSN_KIND_STORE:
        if (eff >= 0) {
            const uint16_t v = m2 == 'a'   ? (acc16 ? regs.c : regs.a)
                               : m2 == 'x' ? (idx16 ? regs.x : regs.xl)
                               : m2 == 'y' ? (idx16 ? regs.y : regs.yl)
                                           : 0; // stz
            ImGui::TextColored(will, "$%04X will become $%0*X", (unsigned)eff, w, v);
            if (have_mem && v == memval)
                ImGui::TextDisabled("(no change - already that value)");
        }
        break;

    case INSN_KIND_COMPARE:
        if (have_mem) {
            const uint16_t r = m1 == 'm'   ? (acc16 ? regs.c : regs.a)
                               : m2 == 'x' ? (idx16 ? regs.x : regs.xl)
                                           : (idx16 ? regs.y : regs.yl);
            ImGui::Text("compares $%0*X with $%0*X", w, r, w, memval);
            ImGui::TextColored(will, "result: %s (Z=%d C=%d)",
                               r == memval ? "equal" : (r > memval ? "greater" : "less"),
                               r == memval ? 1 : 0, r >= memval ? 1 : 0);
        }
        break;

    case INSN_KIND_JUMP:
    case INSN_KIND_RETURN:
        if (eff >= 0) {
            ImGui::TextColored(will, "transfers control to $%04X", (unsigned)eff);
            const char *nm = nullptr;
            if (dbg_info_addr_to_label((uint16_t)eff, &nm) && nm)
                ImGui::TextDisabled("target: %s", nm);
        }
        break;

    case INSN_KIND_INCDEC:
        if (have_mem) {
            const bool     up = (m0 == 'i');
            const unsigned nv = (unsigned)((up ? memval + 1 : memval - 1) &
                                           (acc16 ? 0xFFFF : 0xFF));
            ImGui::TextColored(will, "$%04X: $%0*X -> $%0*X", (unsigned)eff, w, memval, w, nv);
        }
        break;

    default:
        break;
    }

    ImGui::TextDisabled("%d byte%s", nbytes, nbytes == 1 ? "" : "s");
}

// Extract the leading mnemonic from a disassembled line ("lda $26" -> "lda").
// Returns false when the line doesn't start with one.
static inline bool
dbgui_mnemonic_of(const char *disasm_line, char *out, size_t outsz)
{
    if (!disasm_line || !out || outsz == 0) {
        return false;
    }
    while (*disasm_line == ' ' || *disasm_line == '\t') {
        disasm_line++;
    }
    size_t n = 0;
    while (disasm_line[n] && !isspace((unsigned char)disasm_line[n]) && n + 1 < outsz) {
        out[n] = disasm_line[n];
        n++;
    }
    out[n] = '\0';
    return n > 0;
}

#endif
