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
constexpr int      MAX_WP    = 16;       // mirrors MAX_WATCHPOINTS

// UI-selected banks for the banked CPU regions. Lazily initialised to the CPU's
// current mapping so the view opens on what the machine is actually running.
int  g_ram_bank   = 0;
int  g_rom_bank   = 0;
bool g_banks_init = false;

uint16_t g_cpu_goto  = 0x0000; // CPU goto/watchpoint target
uint32_t g_vram_goto = 0x00000; // VRAM goto target
int      g_wp_len    = 1;       // watchpoint length in bytes

// Which bank applies to a CPU address given the current selectors.
int16_t
cpu_bank_for(uint16_t addr)
{
    if (addr >= 0xA000 && addr < 0xC000) return (int16_t)g_ram_bank; // RAM bank
    if (addr >= 0xC000)                  return (int16_t)g_rom_bank; // ROM bank
    return DEBUG_UI_CURRENT_BANK;                                    // low RAM / I/O
}

// ---- MemoryEditor callbacks -----------------------------------------------

ImU8
cpu_read_fn(const ImU8 *, size_t off, void *)
{
    uint16_t addr = (uint16_t)off;
    return debug_ui_read6502(addr, 0, cpu_bank_for(addr));
}

void
cpu_write_fn(ImU8 *, size_t off, ImU8 d, void *)
{
    uint16_t addr = (uint16_t)off;
    debug_ui_write6502(addr, d, 0, cpu_bank_for(addr));
}

// Background tint: green for the current PC, red for watchpoint-covered bytes.
ImU32
cpu_bg_color_fn(const ImU8 *, size_t off, void *)
{
    uint16_t addr = (uint16_t)off;
    if (addr == regs.pc)             return IM_COL32(60, 140, 60, 140);
    if (DEBUGCheckWatchPoint(addr))  return IM_COL32(170, 60, 60, 130);
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
ImU8 cpu_read_at(uint32_t a)  { return debug_ui_read6502((uint16_t)a, 0, cpu_bank_for((uint16_t)a)); }
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
            ed.GotoAddrAndHighlight(gaddr, gaddr);
        }
    }

    // Status line.
    ImGui::Text("PC $%04X   A $%02X  X $%02X  Y $%02X  SP $%04X",
                regs.pc, regs.a, regs.xl, regs.yl, regs.sp);

    // Goto / navigation.
    ImGui::SetNextItemWidth(80);
    bool go = ImGui::InputScalar("##cpugoto", ImGuiDataType_U16, &g_cpu_goto, nullptr, nullptr,
                                 "%04X",
                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Goto")) go = true;
    ImGui::SameLine();
    if (ImGui::Button("Goto PC")) { g_cpu_goto = regs.pc; go = true; }
    if (go) ed.GotoAddrAndHighlight(g_cpu_goto, g_cpu_goto);

    // Bank selectors.
    int ram_max = num_ram_banks > 0 ? (int)num_ram_banks - 1 : 255;
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("RAM bank ($A000)", &g_ram_bank)) {
        if (g_ram_bank < 0) g_ram_bank = 0;
        if (g_ram_bank > ram_max) g_ram_bank = ram_max;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("ROM bank ($C000)", &g_rom_bank)) {
        if (g_rom_bank < 0) g_rom_bank = 0;
        if (g_rom_bank > 255) g_rom_bank = 255;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Sync banks")) {
        g_ram_bank = memory_get_ram_bank();
        g_rom_bank = memory_get_rom_bank();
    }

    // Watchpoint controls, targeting the selected byte (fallback: goto addr).
    uint16_t wp_addr = (ed.DataPreviewAddr != (size_t)-1) ? (uint16_t)ed.DataPreviewAddr : g_cpu_goto;
    bool has_wp = DEBUGCheckWatchPoint(wp_addr);
    ImGui::Text("Watchpoint @ $%04X", wp_addr);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("len", &g_wp_len)) {
        if (g_wp_len < 1) g_wp_len = 1;
        if (g_wp_len > 0xFFFF) g_wp_len = 0xFFFF;
    }
    ImGui::SameLine();
    if (has_wp) {
        if (ImGui::Button("Clear WP")) DEBUGRemoveWatchPoint(wp_addr);
    } else {
        if (ImGui::Button("Set WP")) DEBUGAddWatchPoint(wp_addr, (uint16_t)g_wp_len);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d/%d active)", debug_wp_count(), MAX_WP);

    // Search.
    render_search(ed, g_cpu_search, cpu_read_at, CPU_SIZE);

    ImGui::Separator();
    ed.DrawContents(&g_dummy_base, CPU_SIZE, 0x0000);
    mem_hover_tooltip(ed, cpu_read_at, CPU_SIZE, 4, 0);
}

void
render_vram_tab()
{
    MemoryEditor &ed = vram_editor();

    ImGui::TextDisabled("VERA video memory  $00000-$1FFFF");
    ImGui::SetNextItemWidth(90);
    bool go = ImGui::InputScalar("##vramgoto", ImGuiDataType_U32, &g_vram_goto, nullptr, nullptr,
                                 "%05X",
                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
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
    bool go = ImGui::InputScalar("##bankgoto", ImGuiDataType_U16, &g_bank_goto, nullptr, nullptr,
                                 "%04X",
                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
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
    ed.DrawContents(&g_dummy_base, win, base);
    mem_hover_tooltip(ed, bank_read_at, win, 4, base);
}

void
memory_panel_render(bool *p_open)
{
    if (ImGui::Begin("Memory", p_open)) {
        // A pending cross-panel goto targets the CPU view: bring that tab forward.
        ImGuiTabItemFlags cpu_flags =
            debug_ui_peek_goto(nullptr, nullptr) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabBar("##mem_tabs")) {
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
    ImGui::End();
}

} // namespace

static DebugPanelRegistration s_reg("Memory", memory_panel_render, true);
