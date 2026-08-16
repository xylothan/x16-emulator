// Commander X16 Emulator — shared inline ImGui helpers for the debugger panels.
//
// Header-only (no CMake entry needed): value tooltips that show a number as hex,
// decimal, signed decimal, binary (nibble-grouped) and ASCII. Used by the CPU,
// Memory and VERA panels so hovering any value gives a quick multi-base view.
#ifndef DEBUG_UI_WIDGETS_H
#define DEBUG_UI_WIDGETS_H

// Font ladder used by dbgui_window_zoom (defined in debug_ui.cpp): zoom picks a
// font rasterised at the wanted size rather than scaling one bitmap.
ImFont *debug_ui_font_for_size(float size_px, float *out_size);
float   debug_ui_font_base_size(void);

#include "imgui.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ── Font-relative sizing ────────────────────────────────────────────────────
// Widths must be derived from the font that is actually active, never written
// as pixel constants. The interface scale and per-window zoom both change the
// font size, so a literal width that fitted at 100% clips its contents once the
// UI is scaled up - which is exactly how the breakpoint/watchpoint headers
// ended up truncated.
//
// Pass the widest string the element has to hold ("$FFFF", "00:0000", ...) and
// these measure it in the current font.

// Plain text.
static inline float
dbgui_text_width(const char *sample)
{
    return ImGui::CalcTextSize(sample ? sample : "").x;
}

// InputText / InputScalar / InputInt: the text plus the frame's inner padding.
static inline float
dbgui_field_width(const char *sample)
{
    return dbgui_text_width(sample) + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// Combo: a field plus the drop-down arrow ImGui draws inside the frame.
static inline float
dbgui_combo_width(const char *sample)
{
    return dbgui_field_width(sample) + ImGui::GetFrameHeight();
}

// ── Commit-on-Enter for numeric fields ──────────────────────────────────────
// ImGuiInputTextFlags_EnterReturnsTrue is only supported by the InputText
// family. InputScalar asserts on it (imgui_widgets.cpp:3666), and InputInt and
// friends wrap InputScalar, so a debug build aborts the moment such a field is
// drawn. Release builds compile the assert out and the flag happens to work,
// because InputScalar passes its flags straight through to InputText -- which
// makes this a combination that works only by accident and only where the
// assertion is disabled.
//
// Call this immediately after the field instead. Deactivation-after-edit plus
// an Enter in the same frame is the same condition, without the unsupported
// flag: ImGui deactivates the item when Enter commits it, and the key state is
// still readable that frame.
static inline bool
dbgui_committed_with_enter()
{
    return ImGui::IsItemDeactivatedAfterEdit() &&
           (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
}

// Table column sized to fit `content_w`, and its header text, plus the cell
// padding on both sides. Pass content_w from one of the helpers above (or
// ImGui::GetFrameHeight() for a lone checkbox).
static inline float
dbgui_col_fit(const char *header, float content_w)
{
    const float hw = dbgui_text_width(header);
    if (hw > content_w) {
        content_w = hw;
    }
    return content_w + ImGui::GetStyle().CellPadding.x * 2.0f;
}

// Column holding plain text, e.g. an address or a count.
static inline float
dbgui_col_width(const char *header, const char *sample)
{
    return dbgui_col_fit(header, dbgui_text_width(sample));
}

// Flags every data table should carry: let the user drag column borders (and
// get ImGui's header context menu, which offers "size all columns to fit").
// Default widths still track the font; an explicit resize overrides them.
#define DBGUI_TABLE_FLAGS_RESIZABLE (ImGuiTableFlags_Resizable | ImGuiTableFlags_ContextMenuInBody)

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

// Per-window font zoom. Call once immediately after Begin() (inside the window)
// and pass a stable key for that window. Ctrl+mouse-wheel over the window zooms
// its text only, leaving every other panel alone; Ctrl+middle-click resets to
// 100%. The wheel event is consumed while Ctrl is held so the window does not
// also scroll. Returns the active scale.
//
// Zoom selects a font that was rasterised at the wanted size rather than
// scaling one bitmap — which is what SetWindowFontScale does, and why zoomed
// text used to smear — so the requested scale snaps to the nearest built size.
// Frame padding is not scaled; that trade-off is fine for source/hex views.
//
// IMPORTANT: the pushed font must be popped before ImGui::End(), and panels have
// several early-exit paths, so panels call dbgui_window_end() instead of
// ImGui::End() and the pop is handled there.
static int dbgui_zoom_pushed = 0;

static inline float
dbgui_window_zoom(const char *key)
{
    // Small fixed table — a handful of debugger panels, no allocation needed.
    struct Entry { const char *key; float scale; };
    static Entry  entries[16];
    static int    count = 0;

    Entry *e = nullptr;
    for (int i = 0; i < count; i++) {
        if (entries[i].key == key || (entries[i].key && key && !strcmp(entries[i].key, key))) {
            e = &entries[i];
            break;
        }
    }
    if (!e && count < (int)(sizeof entries / sizeof entries[0])) {
        entries[count].key   = key;
        entries[count].scale = 1.0f;
        e = &entries[count++];
    }
    if (!e)
        return 1.0f;

    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        if (io.KeyCtrl && io.MouseWheel != 0.0f) {
            e->scale += io.MouseWheel * 0.1f;
            if (e->scale < 0.5f) e->scale = 0.5f;
            if (e->scale > 3.0f) e->scale = 3.0f;
            io.MouseWheel = 0.0f; // consume: don't scroll the window too
        }
        if (io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            e->scale = 1.0f;
    }

    const float base = debug_ui_font_base_size();
    float       got  = base;
    ImFont     *font = debug_ui_font_for_size(base * e->scale, &got);
    if (font) {
        ImGui::PushFont(font);
        dbgui_zoom_pushed++;
        e->scale = got / base; // report what was actually applied
    }
    return e->scale;
}

// End a panel window. Pops any font dbgui_window_zoom() pushed, then calls
// ImGui::End(). Panels use this at EVERY exit path (including the one taken
// when Begin() returns false, where nothing was pushed).
static inline void
dbgui_window_end(void)
{
    while (dbgui_zoom_pushed > 0) {
        ImGui::PopFont();
        dbgui_zoom_pushed--;
    }
    ImGui::End();
}

// Show the current zoom as a small right-aligned hint (only when not 100%).
static inline void
dbgui_zoom_hint(float scale)
{
    if (scale == 1.0f)
        return;
    ImGui::SameLine();
    ImGui::TextDisabled("(%.0f%%)", scale * 100.0f);
}

#endif // DEBUG_UI_WIDGETS_H
