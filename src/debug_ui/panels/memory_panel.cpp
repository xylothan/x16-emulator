// Commander X16 Emulator — ImGui debugger "Memory" panel.
//
// A hex/ASCII viewer + editor built on imgui_club's MemoryEditor. Because X16
// memory is banked/virtual there is no single contiguous buffer, so we use
// MemoryEditor's callback form (ReadFn/WriteFn) with a dummy base pointer and
// treat the byte offset as an address.
//
// Two views (tabs):
//   * CPU  — the 64K CPU address space, with RAM ($A000-$BFFF) and ROM
//            ($C000-$FFFF) bank selectors, a Goto box, PC + watchpoint
//            highlighting, and set/clear of memory-write watchpoints.
//   * VRAM — VERA video memory, 0..0x1FFFF.
//
// All emulator state is reached through debug_ui_bridge.h (never core headers).
#include <stddef.h>
#include <stdint.h>
#include <cctype>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui_widgets.h"    // dbgui_value_lines
#include "debug_ui_settings.h"   // user preferences (highlight toggle + fade)
#include "imgui_memory_editor.h" // imgui_club MemoryEditor

// ---------------------------------------------------------------------------
// debug_ui_write6502 — declared (extern "C") in debug_ui_bridge.h. Defined here
// for now; wraps the core write6502() so writes go through the *intended* path
// (I/O side effects + watchpoints). For banked RAM/ROM, write6502() targets the
// currently mapped bank, so we temporarily point the mapping at the bank the
// user is viewing and restore it. The emulator is paused in the debugger while
// editing, so this bank juggling is not observable by the running program.
// ---------------------------------------------------------------------------
extern "C" void
debug_ui_write6502(uint16_t address, uint8_t value, uint8_t bank, int16_t x16Bank)
{
    if (x16Bank >= 0 && address >= 0xA000 && address < 0xC000) { // banked RAM
        uint8_t saved = memory_get_ram_bank();
        memory_set_ram_bank((uint8_t)x16Bank);
        write6502(address, bank, value);
        memory_set_ram_bank(saved);
    } else if (x16Bank >= 0 && address >= 0xC000) { // banked ROM / cartridge
        uint8_t saved = memory_get_rom_bank();
        memory_set_rom_bank((uint8_t)x16Bank);
        write6502(address, bank, value);
        memory_set_rom_bank(saved);
    } else {
        write6502(address, bank, value);
    }
}

namespace {

constexpr size_t   CPU_SIZE  = 0x10000;  // 64K CPU address space
constexpr size_t   VRAM_SIZE = 0x20000;  // 128K VERA video memory
constexpr int      MAX_WP    = MAX_WATCHPOINTS;

// Background for a selected / navigated-to byte range. The editor's default is
// white at 20% alpha, which reads as a washed-out grey and is easy to lose in a
// wall of hex. This is close to the Source panel's current-line blue so "this is
// what you picked" looks the same across panels, and stays translucent so the
// hex digits remain legible on top of it.
constexpr ImU32    SEL_HIGHLIGHT = IM_COL32(70, 130, 245, 140);

// UI-selected banks for the banked CPU regions. Lazily initialised to the CPU's
// current mapping so the view opens on what the machine is actually running.
int  g_ram_bank   = 0;
int  g_rom_bank   = 0;
bool g_banks_init = false;

// The 65C816 CPU bank byte for the CPU tab (X16 GS / gen2). 0 = the classic X16
// map (I/O + windowed $A000-$BFFF/$C000-$FFFF banks); 1..num_banks-1 = a flat
// 64K RAM page. Only meaningful/shown in gen2.
int  g_cpu_bank   = 0;

uint16_t g_cpu_goto  = 0x0000; // CPU goto target
uint32_t g_vram_goto = 0x00000; // VRAM goto target

// Which bank applies to a CPU address given the current selectors.
int16_t
cpu_bank_for(uint16_t addr)
{
    if (addr >= 0xA000 && addr < 0xC000) return (int16_t)g_ram_bank; // RAM bank
    if (addr >= 0xC000)                  return (int16_t)g_rom_bank; // ROM bank
    return DEBUG_UI_CURRENT_BANK;                                    // low RAM / I/O
}


// ---- Recent-write tracking -------------------------------------------------
// Stepping shows you the instruction but not its effect, so diff memory and
// pulse whatever changed, fading the tint out over a second or two (the
// Cheat-Engine idiom). The fade runs on wall-clock time rather than a count of
// stops, so it reads the same whether you are single-stepping or watching the
// machine run.
//
// Scope is the CPU address space including whichever banked window is mapped -
// i.e. what the program is actually touching. VRAM is deliberately excluded: it
// churns every frame and would be a solid wall of colour.
//
// Cost is bounded by only re-diffing on a new stop, or at a capped rate while
// running - never once per frame.
struct MemChangeTracker {
    uint8_t  prev[0x10000];
    float    when[0x10000];   // time the byte last changed; 0 = never
    bool     primed = false;
    uint32_t last_stop_cycle = 0;
    double   last_scan_time  = 0.0;
    // Which banks the baseline was captured through, so a change of viewpoint
    // can be told apart from the program writing memory.
    int      baseline_ram_bank = -1;
    int      baseline_rom_bank = -1;
    int      baseline_cpu_bank = -1;

    void scan(double now)
    {
        if (!primed) {
            for (int a = 0; a < 0x10000; ++a) {
                prev[a] = debug_ui_read6502((uint16_t)a, 0, cpu_bank_for((uint16_t)a));
            }
            memset(when, 0, sizeof when);
            primed = true;
            return;
        }
        for (int a = 0; a < 0x10000; ++a) {
            const uint8_t v = debug_ui_read6502((uint16_t)a, 0, cpu_bank_for((uint16_t)a));
            if (v != prev[a]) {
                prev[a] = v;
                when[a] = (float)now;
            }
        }
    }

    void update()
    {
        const double now = ImGui::GetTime();

        // Changing which bank the view reads makes the whole windowed region
        // look different from the baseline. That is a change of viewpoint, not
        // the program writing memory, so re-baseline silently instead of
        // flooding the view with red.
        if (g_ram_bank != baseline_ram_bank || g_rom_bank != baseline_rom_bank ||
            g_cpu_bank != baseline_cpu_bank) {
            baseline_ram_bank = g_ram_bank;
            baseline_rom_bank = g_rom_bank;
            baseline_cpu_bank = g_cpu_bank;
            reset();
        }

        if (DEBUGIsPaused()) {
            // One diff per stop: re-scanning every frame while parked would
            // find nothing and just burn cycles.
            if (primed && clockticks6502 == last_stop_cycle) {
                return;
            }
            last_stop_cycle = clockticks6502;
            last_scan_time  = now;
            scan(now);
            return;
        }

        // Free-running: often enough to look live, rarely enough that a 64K
        // compare cannot dominate the frame.
        if (now - last_scan_time < 0.05) {
            return;
        }
        last_scan_time  = now;
        last_stop_cycle = clockticks6502;
        scan(now);
    }

    // Tint for an address, or 0 once it has fully faded.
    ImU32 color(uint16_t addr, float fade_seconds) const
    {
        const float t0 = when[addr];
        if (t0 <= 0.0f || fade_seconds <= 0.0f) {
            return 0;
        }
        const float age = (float)ImGui::GetTime() - t0;
        if (age < 0.0f || age >= fade_seconds) {
            return 0;
        }
        const float k     = 1.0f - (age / fade_seconds);    // 1 at the moment of change
        const int   alpha = (int)(30.0f + 195.0f * k * k);  // ease out: a sharp pulse
        return IM_COL32(220, 40, 40, alpha);
    }

    void reset()
    {
        primed = false;
        memset(when, 0, sizeof when);
    }
};

MemChangeTracker g_mem_changes;

// ---- MemoryEditor callbacks -----------------------------------------------

ImU8
cpu_read_fn(const ImU8 *, size_t off, void *)
{
    uint16_t addr = (uint16_t)off;
    // gen2 flat RAM page (CPU bank byte != 0): read flat, bypassing the windows.
    if (g_cpu_bank != 0)
        return debug_ui_read6502(addr, (uint8_t)g_cpu_bank, DEBUG_UI_CURRENT_BANK);
    return debug_ui_read6502(addr, 0, cpu_bank_for(addr));
}

void
cpu_write_fn(ImU8 *, size_t off, ImU8 d, void *)
{
    uint16_t addr = (uint16_t)off;
    if (g_cpu_bank != 0)
        write6502(addr, (uint8_t)g_cpu_bank, d); // flat RAM page
    else
        debug_ui_write6502(addr, d, 0, cpu_bank_for(addr));
}

// Background tint: green for the current PC, red for watchpoint-covered bytes,
// amber for bytes written since the last stop (fading over the following stops).
ImU32
cpu_bg_color_fn(const ImU8 *, size_t off, void *)
{
    uint16_t addr = (uint16_t)off;
    // PC highlight only when the viewed CPU bank matches the program bank (K).
    if (addr == regs.pc && g_cpu_bank == regs.k) return IM_COL32(60, 140, 60, 140);
    // Watchpoints only apply to bank 0 (write watchpoints fire on bank-0 writes).
    if (g_cpu_bank == 0 && DEBUGCheckWatchPoint(addr)) return IM_COL32(170, 60, 60, 130);
    if (debug_ui_settings().mem_highlight_changes && g_cpu_bank == 0) {
        ImU32 c = g_mem_changes.color(addr, debug_ui_settings().mem_highlight_seconds);
        if (c) return c;
    }
    return 0;
}

ImU8
vram_read_fn(const ImU8 *, size_t off, void *)
{
    return video_space_read((uint32_t)off);
}

void
vram_write_fn(ImU8 *, size_t off, ImU8 d, void *)
{
    video_space_write((uint32_t)off, d);
}


// ---- Banked memory view (RAM or ROM) ---------------------------------------
// Browse any RAM/ROM bank's window directly, independent of the bank currently
// mapped into the CPU space. Assets (music/art) and code overlays live in
// banked RAM; this makes every bank viewable/searchable one at a time, plus an
// optional search across all banks at once.
int      g_bank_region = 0;      // 0 = RAM ($A000-$BFFF), 1 = ROM ($C000-$FFFF)
int      g_bank_sel    = 0;      // selected bank
uint16_t g_bank_goto   = 0xA000; // goto target (real address within the window)
bool     g_bank_all    = false;  // search across all banks at once

uint16_t bank_base() { return g_bank_region == 0 ? 0xA000 : 0xC000; }
size_t   bank_win()  { return g_bank_region == 0 ? 0x2000 : 0x4000; }

int
bank_count()
{
    if (g_bank_region == 0)
        return num_ram_banks > 0 ? (int)num_ram_banks : 1;
    int n = memory_get_num_rom_banks();
    return n > 0 ? n : 1;
}

ImU8
bank_read_fn(const ImU8 *, size_t off, void *)
{
    return debug_ui_read6502((uint16_t)(bank_base() + off), 0, (int16_t)g_bank_sel);
}

void
bank_write_fn(ImU8 *, size_t off, ImU8 d, void *)
{
    debug_ui_write6502((uint16_t)(bank_base() + off), d, 0, (int16_t)g_bank_sel);
}

ImU8
bank_read_at(uint32_t a)
{
    return debug_ui_read6502((uint16_t)(bank_base() + a), 0, (int16_t)g_bank_sel);
}

// Flat read across ALL banks of the current region: flat = bank*win + off.
ImU8
bank_read_at_flat(uint32_t flat)
{
    size_t   w    = bank_win();
    int      bank = (int)(flat / w);
    uint32_t off  = (uint32_t)(flat % w);
    return debug_ui_read6502((uint16_t)(bank_base() + off), 0, (int16_t)bank);
}

// A dummy base pointer for the callback-driven editors (never dereferenced:
// every access in MemoryEditor is guarded by ReadFn/WriteFn).
unsigned char g_dummy_base = 0;

MemoryEditor &
cpu_editor()
{
    static MemoryEditor ed;
    static bool init = false;
    if (!init) {
        ed.ReadFn            = cpu_read_fn;
        ed.WriteFn           = cpu_write_fn;
        ed.BgColorFn         = cpu_bg_color_fn;
        ed.HighlightColor    = SEL_HIGHLIGHT;
        ed.OptShowDataPreview = true;
        ed.OptShowOptions     = true;
        ed.PreviewDataType    = ImGuiDataType_U8; // default the data preview to uint8
        init = true;
    }
    return ed;
}

MemoryEditor &
vram_editor()
{
    static MemoryEditor ed;
    static bool init = false;
    if (!init) {
        ed.ReadFn             = vram_read_fn;
        ed.WriteFn            = vram_write_fn;
        ed.HighlightColor     = SEL_HIGHLIGHT;
        ed.OptShowDataPreview = true;
        ed.OptShowOptions     = true;
        ed.PreviewDataType    = ImGuiDataType_U8; // default the data preview to uint8
        ed.OptAddrDigitsCount = 5; // 0x1FFFF needs 5 hex digits
        init = true;
    }
    return ed;
}

MemoryEditor &
bank_editor()
{
    static MemoryEditor ed;
    static bool init = false;
    if (!init) {
        ed.ReadFn             = bank_read_fn;
        ed.WriteFn            = bank_write_fn;
        ed.HighlightColor     = SEL_HIGHLIGHT;
        ed.OptShowDataPreview = true;
        ed.OptShowOptions     = true;
        ed.PreviewDataType    = ImGuiDataType_U8;
        init = true;
    }
    return ed;
}

// ---- Search ----------------------------------------------------------------
// Find a hex-byte sequence ("A9 00 8D") or an ASCII string in the currently
// viewed space, with Find Next / Find Prev and match highlighting.

struct SearchState {
    char buf[64] = "";
    bool hex     = true;   // hex-bytes vs ASCII
    long last    = -1;     // address of the last match (start of the run)
    bool notfound = false; // last search failed
};

SearchState g_cpu_search;
SearchState g_vram_search;
SearchState g_bank_search;

// Plain-address readers for the search (bank-aware for CPU space).
ImU8 cpu_read_at(uint32_t a)  {
    if (g_cpu_bank != 0) return debug_ui_read6502((uint16_t)a, (uint8_t)g_cpu_bank, DEBUG_UI_CURRENT_BANK);
    return debug_ui_read6502((uint16_t)a, 0, cpu_bank_for((uint16_t)a));
}
ImU8 vram_read_at(uint32_t a) { return video_space_read(a); }

// Parse "A9 00 8D" / "A9008D" into bytes (whitespace ignored, nibbles paired).
bool
parse_hex_pattern(const char *s, std::vector<uint8_t> &out)
{
    out.clear();
    int hi = -1;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) continue;
        if (!isxdigit(c)) return false;
        int v = (c <= '9') ? (c - '0') : (tolower(c) - 'a' + 10);
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back((uint8_t)((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) out.push_back((uint8_t)hi); // trailing single nibble → low byte
    return !out.empty();
}

bool
parse_ascii_pattern(const char *s, std::vector<uint8_t> &out)
{
    out.clear();
    for (const char *p = s; *p; ++p) out.push_back((uint8_t)*p);
    return !out.empty();
}

// Scan for `pat` starting at candidate position `from`, wrapping over the whole
// space. dir >= 0 searches forward, dir < 0 backward. Returns the match start or
// -1. Only called on a button press, so a linear scan is fine.
long
mem_find(ImU8 (*readfn)(uint32_t), size_t size, const std::vector<uint8_t> &pat, long from, int dir)
{
    size_t m = pat.size();
    if (m == 0 || size < m) return -1;
    long span = (long)(size - m) + 1;   // candidate start positions [0, size-m]
    from = ((from % span) + span) % span;
    for (long k = 0; k < span; k++) {
        long i = (dir >= 0) ? ((from + k) % span) : (((from - k) % span + span) % span);
        bool ok = true;
        for (size_t j = 0; j < m; j++) {
            if (readfn((uint32_t)i + (uint32_t)j) != pat[j]) { ok = false; break; }
        }
        if (ok) return i;
    }
    return -1;
}

// Search UI row for one tab. Highlights the match in `ed` on success.
void
render_search(MemoryEditor &ed, SearchState &st, ImU8 (*readfn)(uint32_t), size_t size)
{
    ImGui::SetNextItemWidth(160);
    bool enter = ImGui::InputText("##search", st.buf, sizeof(st.buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::Checkbox("hex", &st.hex);
    ImGui::SameLine();
    bool next = ImGui::Button("Find Next") || enter;
    ImGui::SameLine();
    bool prev = ImGui::Button("Find Prev");

    if (next || prev) {
        std::vector<uint8_t> pat;
        bool ok = st.hex ? parse_hex_pattern(st.buf, pat) : parse_ascii_pattern(st.buf, pat);
        st.notfound = false;
        if (ok) {
            long from = (st.last < 0) ? (next ? 0 : (long)size) : (next ? st.last + 1 : st.last - 1);
            long m = mem_find(readfn, size, pat, from, next ? +1 : -1);
            if (m >= 0) {
                st.last = m;
                ed.GotoAddrAndHighlight((size_t)m, (size_t)m + pat.size());
            } else {
                st.notfound = true;
            }
        } else {
            st.notfound = true;
        }
    }
    if (st.notfound) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "no match");
    } else if (st.last >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("@ $%lX", (unsigned long)st.last);
    }
}

// ---- Tabs ------------------------------------------------------------------

// Drag-selection over a memory view.
//
// The vendored MemoryEditor only tracks a single byte (DataPreviewAddr), but it
// does publish what the mouse is over, which is enough to build a range on top
// of it without forking the widget. A plain click still edits a byte as before;
// only an actual drag starts a selection.
struct MemSelection {
    bool     active  = false;  // a completed selection exists
    bool     dragging = false;
    size_t   anchor  = 0;
    size_t   lo      = 0;
    size_t   hi      = 0;      // inclusive

    size_t   count() const { return active ? (hi - lo + 1) : 0; }

    void clear() { active = false; dragging = false; }

    // Call every frame, after the editor has drawn (so MouseHovered is current).
    void update(const MemoryEditor &ed)
    {
        const bool over = ed.MouseHovered;
        const size_t at = ed.MouseHoveredAddr;

        if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            anchor   = at;
            dragging = true;
            active   = false; // a fresh click drops any previous selection
        }
        if (dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (over) {
                lo     = anchor < at ? anchor : at;
                hi     = anchor < at ? at : anchor;
                active = (hi > lo); // a one-byte "range" is just a click
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            dragging = false;
        }
    }

    // Call before the editor draws.
    //
    // Clicking a byte puts it into edit mode, and the resulting InputText
    // becomes ImGui's ActiveId. ImGui::IsItemHovered() returns false for every
    // other item while something is active, so the editor stops reporting
    // MouseHovered as soon as the drag leaves the first byte - which meant a
    // selection could never grow past one address.
    //
    // So cancel edit mode as soon as the mouse actually drags. The InputText is
    // then never submitted, nothing takes ActiveId, and hover keeps working for
    // the rest of the drag. A plain click never crosses the drag threshold, so
    // it still opens the byte editor exactly as before.
    void apply(MemoryEditor &ed) const
    {
        if (active) {
            ed.HighlightMin = lo;
            ed.HighlightMax = hi + 1; // editor's range is [min, max)
        }
        if (dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ed.DataEditingAddr      = (size_t)-1;
            ed.DataEditingTakeFocus = false;
        }
    }
};

// The address a context menu should act on: the selection start if there is
// one, else whatever the mouse is over, else the currently selected byte.
uint16_t
mem_context_addr(const MemoryEditor &ed, const MemSelection &sel, uint16_t fallback)
{
    if (sel.active) {
        return (uint16_t)sel.lo;
    }
    if (ed.MouseHovered) {
        return (uint16_t)ed.MouseHoveredAddr;
    }
    if (ed.DataPreviewAddr != (size_t)-1) {
        return (uint16_t)ed.DataPreviewAddr;
    }
    return fallback;
}

// Right-click menu over a memory view. Kept generic so every tab can offer the
// same actions against its own address space and bank. `base` is added to
// editor offsets to form real addresses (0 for the CPU view, the window base
// for the banked view).
void
mem_context_menu(const char *id, MemSelection &sel, int16_t bank, uint16_t addr, uint16_t base)
{
    // The editor draws into a child window, so open the popup against the
    // hovered child rather than the panel window.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(id);
    }
    if (!ImGui::BeginPopup(id)) {
        return;
    }

    const size_t   n     = sel.count();
    const uint16_t start = n ? (uint16_t)(base + sel.lo) : addr;
    const uint16_t end   = n ? (uint16_t)(base + sel.hi) : addr;

    if (n > 1) {
        ImGui::TextDisabled("$%04X-$%04X  (%d bytes)", start, end, (int)n);
    } else {
        ImGui::TextDisabled("$%04X", start);
    }
    ImGui::Separator();

    const uint16_t len = (uint16_t)(n > 1 ? n : 1);

    if (ImGui::MenuItem(n > 1 ? "Add range to watch" : "Add to watch")) {
        debug_ui_request_watch(start, bank, len);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(n > 1 ? "Add %d bytes from $%04X to the CPU panel's watch list\n"
                                  "(bank %d). Ranges over 16 bytes become several rows."
                                : "Add $%04X to the CPU panel's watch list (bank %d).",
                          n > 1 ? (int)n : (int)start, n > 1 ? (int)start : (int)bank, (int)bank);
    }

    if (ImGui::MenuItem("Copy address")) {
        char buf[24];
        if (n > 1)
            snprintf(buf, sizeof buf, "$%04X-$%04X", start, end);
        else
            snprintf(buf, sizeof buf, "$%04X", start);
        ImGui::SetClipboardText(buf);
    }

    ImGui::Separator();

    // A write watchpoint halts the machine when the running program STORES to
    // the address - the way to find out what is clobbering a value. Note it
    // triggers on any write, even one that stores the same value back, so it is
    // "break on write" rather than "break on change".
    const bool has_wp = DEBUGCheckWatchPoint(start);
    if (ImGui::MenuItem(has_wp ? "Stop breaking on write" : "Break on write")) {
        if (has_wp)
            DEBUGRemoveWatchPoint(start);
        else
            DEBUGAddWatchPoint(start, len);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Halt the machine as soon as the program writes to %s,\n"
                          "so you can see what is doing the writing.\n\n"
                          "Fires on any store, including one that writes back the same\n"
                          "value - it is not a change detector.\n"
                          "(%d of %d in use.)",
                          n > 1 ? "this range" : "this address", debug_wp_count(), MAX_WP);
    }

    if (sel.active) {
        ImGui::Separator();
        if (ImGui::MenuItem("Clear selection")) {
            sel.clear();
        }
    }

    ImGui::EndPopup();
}

// Hover tooltip for the byte under the mouse (uses MemoryEditor's public
// MouseHovered / MouseHoveredAddr). Shows the byte as hex/dec/signed/bin/ASCII
// plus the 16-bit little-endian word at that address, for quick inspection.
// `base` is added to the (0-based) editor offset for the displayed address.
void
mem_hover_tooltip(MemoryEditor &ed, ImU8 (*readfn)(uint32_t), size_t size, int addr_digits, uint32_t base)
{
    if (!ed.MouseHovered)
        return;
    uint32_t off = (uint32_t)ed.MouseHoveredAddr;
    uint8_t  b   = readfn(off);
    uint32_t w   = b;
    if (off + 1 < size)
        w |= (uint32_t)readfn(off + 1) << 8;
    ImGui::BeginTooltip();
    ImGui::Text("Addr  : $%0*X", addr_digits, (unsigned)(base + off));
    ImGui::Separator();
    ImGui::TextDisabled("byte");
    dbgui_value_lines(b, 1);
    ImGui::Separator();
    ImGui::TextDisabled("word (LE)");
    dbgui_value_lines(w, 2);
    ImGui::EndTooltip();
}

void
render_cpu_tab()
{
    MemoryEditor &ed = cpu_editor();

    // Diff the CPU space whenever execution stops, so changed bytes can be
    // tinted (see MemChangeTracker).
    if (debug_ui_settings().mem_highlight_changes) {
        g_mem_changes.update();
    }

    if (!g_banks_init) {
        g_ram_bank   = memory_get_ram_bank();
        g_rom_bank   = memory_get_rom_bank();
        g_banks_init = true;
    }

    // Honor a cross-panel goto request (from Symbols / Call Stack panels).
    {
        uint16_t gaddr;
        uint8_t  gbank;
        if (debug_ui_peek_goto(&gaddr, &gbank)) {
            g_cpu_goto = gaddr;
            if (is_gen2) g_cpu_bank = gbank; // gbank is the 65C816 program bank (K)
            ed.GotoAddrAndHighlight(gaddr, gaddr);
        }
    }

    // Status line.
    if (is_gen2)
        ImGui::Text("K $%02X  PC $%02X:%04X   A $%02X  X $%02X  Y $%02X  SP $%04X",
                    regs.k, regs.k, regs.pc, regs.a, regs.xl, regs.yl, regs.sp);
    else
        ImGui::Text("PC $%04X   A $%02X  X $%02X  Y $%02X  SP $%04X",
                    regs.pc, regs.a, regs.xl, regs.yl, regs.sp);

    // Goto / navigation.
    //
    // Deliberately NOT EnterReturnsTrue: with that flag ImGui only applies the
    // typed text when Enter is pressed and discards it on any other
    // deactivation, so typing an address and then clicking Goto threw the
    // address away and jumped to the previous one. Without it the value is
    // applied as it is edited, and IsItemDeactivatedAfterEdit() gives us the
    // Enter/commit trigger. AutoSelectAll so typing replaces rather than
    // appends to the address already shown.
    ImGui::SetNextItemWidth(80);
    ImGui::InputScalar("##cpugoto", ImGuiDataType_U16, &g_cpu_goto, nullptr, nullptr,
                       "%04X",
                       ImGuiInputTextFlags_CharsHexadecimal |
                           ImGuiInputTextFlags_AutoSelectAll);
    bool go = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Address to jump to (hex). Press Enter or click Goto.");
    ImGui::SameLine();
    if (ImGui::Button("Goto")) go = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll the view to the address on the left.");
    ImGui::SameLine();
    if (ImGui::Button("Goto PC")) { g_cpu_goto = regs.pc; if (is_gen2) g_cpu_bank = regs.k; go = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll to the current program counter ($%04X).", regs.pc);
    if (go) ed.GotoAddrAndHighlight(g_cpu_goto, g_cpu_goto);

    // Highlighting is a preference, so it lives in System > Settings rather than
    // being duplicated here. Clearing is an action, so it belongs on the panel.
    DebugUiSettings &st = debug_ui_settings();
    if (st.mem_highlight_changes) {
        ImGui::SameLine(0, 16);
        if (ImGui::SmallButton("Clear highlights")) {
            g_mem_changes.reset();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Forget the current change highlights and re-baseline from\n"
                              "here. Changed bytes pulse red and fade over %.2f s;\n"
                              "turn that off or retune it in System > Settings.",
                              st.mem_highlight_seconds);
    }

    // CPU bank byte (X16 GS / gen2): 0 = classic map (I/O + windowed banks),
    // 1..num_banks-1 = a flat 64K RAM page selected by the 65C816 bank byte.
    if (is_gen2) {
        int bank_max = num_banks > 0 ? (int)num_banks - 1 : 255;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("CPU bank (K)", &g_cpu_bank)) {
            if (g_cpu_bank < 0) g_cpu_bank = 0;
            if (g_cpu_bank > bank_max) g_cpu_bank = bank_max;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Bank 0")) g_cpu_bank = 0;
        ImGui::SameLine();
        if (ImGui::SmallButton("Sync K")) g_cpu_bank = regs.k;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("X16 GS 24-bit RAM. Bank 0 is the classic map (I/O at $9F00,\n"
                              "windowed $A000/$C000 banks below). Banks 1-%d are flat 64K RAM\n"
                              "pages addressed by the 65C816 bank byte. The RAM/ROM window\n"
                              "selectors below only apply in bank 0.", bank_max);
    }

    // Bank selectors.
    int ram_max = num_ram_banks > 0 ? (int)num_ram_banks - 1 : 255;
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("RAM bank ($A000)", &g_ram_bank)) {
        if (g_ram_bank < 0) g_ram_bank = 0;
        if (g_ram_bank > ram_max) g_ram_bank = ram_max;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Which RAM bank this view reads through the $A000-$BFFF window.\n"
                          "Independent of the bank the machine currently has mapped\n"
                          "(currently %d), so you can inspect one bank while it runs in\n"
                          "another.", (int)memory_get_ram_bank());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("ROM bank ($C000)", &g_rom_bank)) {
        if (g_rom_bank < 0) g_rom_bank = 0;
        if (g_rom_bank > 255) g_rom_bank = 255;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Which ROM bank this view reads through the $C000-$FFFF window.\n"
                          "The machine currently has bank %d mapped.", (int)memory_get_rom_bank());
    ImGui::SameLine();
    if (ImGui::SmallButton("Sync banks")) {
        g_ram_bank = memory_get_ram_bank();
        g_rom_bank = memory_get_rom_bank();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap the two selectors above to whatever the machine has mapped\n"
                          "right now (RAM %d, ROM %d), so the view matches what the running\n"
                          "program sees.", (int)memory_get_ram_bank(), (int)memory_get_rom_bank());

    // Write watchpoints used to be set from a row here. That is now done by
    // right-clicking the byte (or a drag-selected range, which supplies the
    // length), and the Breakpoints panel lists and manages them - so the row
    // was three-way redundant and is gone.

    // Search.
    render_search(ed, g_cpu_search, cpu_read_at, CPU_SIZE);

    ImGui::Separator();
    // In gen2 show 24-bit addresses (bank<<16 | offset); 16-bit otherwise.
    uint32_t addr_base = is_gen2 ? ((uint32_t)g_cpu_bank << 16) : 0u;
    ed.OptAddrDigitsCount = is_gen2 ? 6 : 0; // 0 = auto (16-bit)
    static MemSelection sel;
    sel.apply(ed);
    ed.DrawContents(&g_dummy_base, CPU_SIZE, addr_base);
    sel.update(ed);
    mem_hover_tooltip(ed, cpu_read_at, CPU_SIZE, is_gen2 ? 6 : 4, addr_base);
    {
        const uint16_t at = mem_context_addr(ed, sel, g_cpu_goto);
        mem_context_menu("cpu_ctx", sel, cpu_bank_for(at), at, 0);
    }
    if (sel.count() > 1) {
        ImGui::TextDisabled("Selected $%04X-$%04X (%d bytes) - right-click for actions.",
                            (unsigned)sel.lo, (unsigned)sel.hi, (int)sel.count());
    }
}

void
render_vram_tab()
{
    MemoryEditor &ed = vram_editor();

    ImGui::TextDisabled("VERA video memory  $00000-$1FFFF");
    ImGui::SetNextItemWidth(90);
    bool go = false;
    ImGui::InputScalar("##vramgoto", ImGuiDataType_U32, &g_vram_goto, nullptr, nullptr,
                       "%05X",
                       ImGuiInputTextFlags_CharsHexadecimal |
                           ImGuiInputTextFlags_AutoSelectAll);
    go = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("VRAM address to jump to (hex, $00000-$1FFFF).");
    ImGui::SameLine();
    if (ImGui::Button("Goto")) go = true;
    if (go) {
        if (g_vram_goto >= VRAM_SIZE) g_vram_goto = VRAM_SIZE - 1;
        ed.GotoAddrAndHighlight(g_vram_goto, g_vram_goto);
    }

    // Search.
    render_search(ed, g_vram_search, vram_read_at, VRAM_SIZE);

    ImGui::Separator();
    ed.DrawContents(&g_dummy_base, VRAM_SIZE, 0x0000);
    mem_hover_tooltip(ed, vram_read_at, VRAM_SIZE, 5, 0);
}

// Banked-memory search: within the selected bank, or (all-banks) flat across
// every bank of the current region, jumping to the matching bank on a hit.
void
render_bank_search(MemoryEditor &ed, uint16_t base, size_t win, int nbanks)
{
    SearchState &st = g_bank_search;
    ImGui::SetNextItemWidth(160);
    bool enter = ImGui::InputText("##banksearch", st.buf, sizeof(st.buf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::Checkbox("hex", &st.hex);
    ImGui::SameLine();
    if (ImGui::Checkbox("all banks", &g_bank_all))
        st.last = -1; // mode changed: last-match index semantics differ
    ImGui::SameLine();
    bool next = ImGui::Button("Find Next") || enter;
    ImGui::SameLine();
    bool prev = ImGui::Button("Find Prev");

    if (next || prev) {
        std::vector<uint8_t> pat;
        bool ok = st.hex ? parse_hex_pattern(st.buf, pat) : parse_ascii_pattern(st.buf, pat);
        st.notfound = false;
        if (!ok) {
            st.notfound = true;
        } else if (g_bank_all) {
            size_t total = (size_t)nbanks * win;
            long   from  = (st.last < 0) ? (next ? 0 : (long)total)
                                         : (next ? st.last + 1 : st.last - 1);
            long m = mem_find(bank_read_at_flat, total, pat, from, next ? +1 : -1);
            if (m >= 0) {
                st.last    = m;
                g_bank_sel = (int)((size_t)m / win);
                long off   = (long)((size_t)m % win);
                ed.GotoAddrAndHighlight((size_t)off, (size_t)off + pat.size());
            } else {
                st.notfound = true;
            }
        } else {
            long from = (st.last < 0) ? (next ? 0 : (long)win)
                                      : (next ? st.last + 1 : st.last - 1);
            long m = mem_find(bank_read_at, win, pat, from, next ? +1 : -1);
            if (m >= 0) {
                st.last = m;
                ed.GotoAddrAndHighlight((size_t)m, (size_t)m + pat.size());
            } else {
                st.notfound = true;
            }
        }
    }

    if (st.notfound) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "no match");
    } else if (st.last >= 0 && g_bank_all) {
        ImGui::SameLine();
        ImGui::TextDisabled("bank %d @ $%04X", (int)((size_t)st.last / win),
                            (unsigned)(base + (size_t)st.last % win));
    } else if (st.last >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("@ $%04X", (unsigned)(base + (unsigned)st.last));
    }
}

// Banked memory: view any RAM/ROM bank's window directly (independent of what is
// mapped in), stepping through all banks with the selector, and optionally
// searching across every bank at once.
void
render_bank_tab()
{
    MemoryEditor &ed = bank_editor();

    // Region toggle (recompute bank geometry immediately after).
    ImGui::RadioButton("RAM  $A000-$BFFF", &g_bank_region, 0);
    ImGui::SameLine();
    ImGui::RadioButton("ROM  $C000-$FFFF", &g_bank_region, 1);

    int      nbanks   = bank_count();
    int      bank_max = nbanks - 1;
    uint16_t base     = bank_base();
    size_t   win      = bank_win();
    if (g_bank_sel > bank_max)
        g_bank_sel = bank_max;
    if (g_bank_sel < 0)
        g_bank_sel = 0;
    if (g_bank_goto < base || g_bank_goto >= base + win)
        g_bank_goto = base;

    ImGui::Text("%s   bank %d of %d   (%d KB each = %d KB total)",
                g_bank_region == 0 ? "Banked RAM" : "Banked ROM", g_bank_sel, nbanks,
                (int)(win / 1024), (int)(nbanks * win / 1024));
    ImGui::TextDisabled("Browse assets/code in any bank, independent of the mapped bank.");

    // Bank selector: Prev / number / Next.
    if (ImGui::Button("< Prev") && g_bank_sel > 0)
        g_bank_sel--;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("Bank", &g_bank_sel);
    ImGui::SameLine();
    if (ImGui::Button("Next >") && g_bank_sel < bank_max)
        g_bank_sel++;
    if (g_bank_sel < 0)
        g_bank_sel = 0;
    if (g_bank_sel > bank_max)
        g_bank_sel = bank_max;
    ImGui::SameLine();
    ImGui::TextDisabled("(mapped: RAM %d / ROM %d)", (int)memory_get_ram_bank(),
                        (int)memory_get_rom_bank());

    // Goto within the window (accepts a real address in the region).
    ImGui::SetNextItemWidth(80);
    ImGui::InputScalar("##bankgoto", ImGuiDataType_U16, &g_bank_goto, nullptr, nullptr,
                       "%04X",
                       ImGuiInputTextFlags_CharsHexadecimal |
                           ImGuiInputTextFlags_AutoSelectAll);
    bool go = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Address within this bank's window ($%04X-$%04X).", base,
                          (unsigned)(base + win - 1));
    ImGui::SameLine();
    if (ImGui::Button("Goto"))
        go = true;
    if (go) {
        uint16_t a = g_bank_goto;
        if (a < base)
            a = base;
        if (a >= base + win)
            a = (uint16_t)(base + win - 1);
        ed.GotoAddrAndHighlight((size_t)(a - base), (size_t)(a - base));
    }

    render_bank_search(ed, base, win, nbanks);

    ImGui::Separator();
    ed.ReadOnly = (g_bank_region == 1); // ROM is not editable
    static MemSelection bsel;
    bsel.apply(ed);
    ed.DrawContents(&g_dummy_base, win, base);
    bsel.update(ed);
    mem_hover_tooltip(ed, bank_read_at, win, 4, base);
    // Editor offsets are window-relative here, so add the window base back to
    // get a real address for the menu.
    {
        const uint16_t at =
            (uint16_t)(base + mem_context_addr(ed, bsel, (uint16_t)(g_bank_goto - base)));
        mem_context_menu("bank_ctx", bsel, (int16_t)g_bank_sel, at, base);
    }
    if (bsel.count() > 1) {
        ImGui::TextDisabled("Selected $%04X-$%04X (%d bytes) - right-click for actions.",
                            (unsigned)(base + bsel.lo), (unsigned)(base + bsel.hi),
                            (int)bsel.count());
    }
}

void
memory_panel_render(bool *p_open)
{
    if (ImGui::Begin("Memory", p_open)) {
        dbgui_window_zoom("memory");
        // A pending cross-panel goto targets the CPU view: bring that tab forward.
        ImGuiTabItemFlags cpu_flags =
            debug_ui_peek_goto(nullptr, nullptr) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabBar("##mem_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("CPU", nullptr, cpu_flags)) {
                render_cpu_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Banked")) {
                render_bank_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("VRAM")) {
                render_vram_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    dbgui_window_end();
}

} // namespace

static DebugPanelRegistration s_reg("Memory", memory_panel_render, true);
