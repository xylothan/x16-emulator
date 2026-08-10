// Commander X16 Emulator — shared inline ImGui helpers for the debugger panels.
//
// Header-only (no CMake entry needed): value tooltips that show a number as hex,
// decimal, signed decimal, binary (nibble-grouped) and ASCII. Used by the CPU,
// Memory and VERA panels so hovering any value gives a quick multi-base view.
#ifndef DEBUG_UI_WIDGETS_H
#define DEBUG_UI_WIDGETS_H

#include "imgui.h"
#include <stdint.h>
#include <stddef.h>

// Format the low `bits` of v as binary, grouped in nibbles: e.g. "1010 0101".
static inline void
dbgui_format_binary(char *out, size_t outsz, uint32_t v, int bits)
{
    size_t o = 0;
    for (int i = bits - 1; i >= 0 && o + 2 < outsz; i--) {
        out[o++] = ((v >> i) & 1u) ? '1' : '0';
        if (i != 0 && (i % 4) == 0 && o + 1 < outsz)
            out[o++] = ' ';
    }
    out[o] = '\0';
}

// Render the value lines (hex / decimal / signed / binary / ASCII). Does NOT
// open a tooltip window itself — call between BeginTooltip()/EndTooltip() or
// inline. `bytes` is the value width (1..4).
static inline void
dbgui_value_lines(uint32_t v, int bytes)
{
    if (bytes < 1)
        bytes = 1;
    if (bytes > 4)
        bytes = 4;
    int      bits = bytes * 8;
    uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    v &= mask;

    int32_t s = (int32_t)v; // sign-extend from the value's width
    if (bits < 32 && (v & (1u << (bits - 1))))
        s = (int32_t)(v | ~mask);

    char bin[48];
    dbgui_format_binary(bin, sizeof bin, v, bits);

    ImGui::Text("Hex   : $%0*X", bytes * 2, (unsigned)v);
    ImGui::Text("Dec   : %u", (unsigned)v);
    ImGui::Text("Signed: %d", (int)s);
    ImGui::Text("Bin   : %s", bin);
    if (bytes == 1) {
        unsigned char c = (unsigned char)(v & 0xFF);
        if (c >= 0x20 && c < 0x7f)
            ImGui::Text("Char  : '%c'", c);
        else
            ImGui::Text("Char  : .");
    }
}

// If the LAST-submitted item is hovered, show a value tooltip, optionally
// prefixed with a name/description line. Call immediately after the widget.
static inline void
dbgui_hover_value_tooltip(const char *desc, uint32_t v, int bytes)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    ImGui::BeginTooltip();
    if (desc && desc[0]) {
        ImGui::TextUnformatted(desc);
        ImGui::Separator();
    }
    dbgui_value_lines(v, bytes);
    ImGui::EndTooltip();
}

#endif // DEBUG_UI_WIDGETS_H
