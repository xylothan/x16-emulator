// VERA panel — graphical VRAM viewers for the Commander X16's VERA.
//
// Tabs:
//   * Palette  — the 256-entry VERA palette ($1FA00..$1FBFF) as a 16x16 grid.
//   * Tiles    — a freeform tile/character viewer (base addr, bpp, tile size).
//   * Sprites  — the 128 sprite attribute entries ($1FC00..$1FFFF), decoded.
//   * Bitmap   — a layer's bitmap-mode image (from the layer registers).
//   * Tilemap  — a layer's tile/text map (from the layer registers).
//
// Technique: each viewer decodes VRAM into an RGBA (ABGR8888) pixel buffer,
// uploads it to a STREAMING SDL_Texture via the debugger's SDL_Renderer, and
// shows it with ImGui::Image. Textures are function-static and only recreated
// when their dimensions change, so there is no per-frame allocation/leak. Only
// the active tab decodes each frame (ImGui::BeginTabItem gates the work), so the
// cost of the heavier viewers is only paid when they are visible. All VRAM
// access goes through video_space_read()/debug_ui_vram_read_range(), which mask
// the address into the 17-bit VRAM space, so reads can never go out of bounds.
#include "imgui.h"
#include "debug_ui_panels.h"
#include "debug_ui_bridge.h"
#include "debug_ui.h"
#include "debug_ui_widgets.h" // dbgui_hover_value_tooltip / dbgui_format_binary

#include <SDL.h>
#include <stdint.h>
#include <string.h>

namespace {

constexpr uint32_t ADDR_PALETTE     = 0x1FA00; // 256 * 2 bytes, 12-bit RGB
constexpr uint32_t ADDR_SPRITE_ATTR = 0x1FC00; // 128 * 8 bytes
constexpr int      NUM_SPRITES      = 128;

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Expand a 4-bit VERA color channel (0..15) to 8 bits by nibble replication,
// exactly like refresh_palette() in video.c (x<<4 | x).
inline uint8_t expand4(uint8_t v) { return (uint8_t)((v << 4) | v); }

// Build the 256-entry palette as ABGR8888 (alpha forced opaque). VERA stores
// each entry as two bytes: byte0 = GGGGBBBB, byte1 = 0000RRRR.
void
build_palette(uint32_t out[256])
{
    for (int i = 0; i < 256; ++i) {
        uint8_t b0 = video_space_read(ADDR_PALETTE + i * 2);     // GGGGBBBB
        uint8_t b1 = video_space_read(ADDR_PALETTE + i * 2 + 1); // 0000RRRR
        uint8_t r  = expand4(b1 & 0x0f);
        uint8_t g  = expand4((b0 >> 4) & 0x0f);
        uint8_t b  = expand4(b0 & 0x0f);
        out[i] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }
}

// A lazily-created, resizable STREAMING texture. Kept as a file-/function-static
// so it lives for the app's lifetime; recreated only when the size changes.
struct GfxTexture {
    SDL_Texture *tex = nullptr;
    int          w   = 0;
    int          h   = 0;

    bool ensure(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return false;
        if (tex && w == width && h == height)
            return true;
        if (tex) {
            SDL_DestroyTexture(tex);
            tex = nullptr;
        }
        SDL_Renderer *ren = debug_ui_get_renderer();
        if (!ren)
            return false;
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!tex)
            return false;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest); // crisp zoomed pixels
        w = width;
        h = height;
        return true;
    }

    void update(const uint32_t *pixels)
    {
        if (tex)
            SDL_UpdateTexture(tex, nullptr, pixels, w * (int)sizeof(uint32_t));
    }

    ImTextureID id() const { return (ImTextureID)(intptr_t)tex; }
};

// A reusable, growable RGBA scratch buffer for building images.
struct PixelBuffer {
    uint32_t *data = nullptr;
    size_t    cap  = 0;

    uint32_t *ensure(size_t count)
    {
        if (count > cap) {
            free(data);
            data = (uint32_t *)malloc(count * sizeof(uint32_t));
            cap  = data ? count : 0;
        }
        return data;
    }
};

// Decode one indexed color to a display pixel. index 0 stays transparent when
// `transparent0` is set (sprites); otherwise it renders as palette[0] (tiles).
inline uint32_t
resolve_color(const uint32_t pal[256], int idx, int palette_offset, bool transparent0)
{
    if (idx == 0 && transparent0)
        return 0; // fully transparent
    int ci = idx;
    if (ci > 0 && ci < 16)
        ci += palette_offset;
    return pal[ci & 0xff];
}

// ---------------------------------------------------------------------------
// Palette tab
// ---------------------------------------------------------------------------
void
draw_palette_tab()
{
    ImGui::TextUnformatted("VERA palette — $1FA00..$1FBFF (256 entries, 12-bit RGB)");
    ImGui::Spacing();

    uint32_t pal[256];
    build_palette(pal);

    const ImGuiColorEditFlags flags =
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha |
        ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoBorder;
    const ImVec2 sw(20.0f, 20.0f);

    for (int i = 0; i < 256; ++i) {
        ImGui::PushID(i);
        uint32_t c = pal[i];
        uint8_t  r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
        ImVec4   col(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
        ImGui::ColorButton("##sw", col, flags, sw);
        if (ImGui::IsItemHovered()) {
            uint8_t b0 = video_space_read(ADDR_PALETTE + i * 2);
            uint8_t b1 = video_space_read(ADDR_PALETTE + i * 2 + 1);
            ImGui::SetTooltip("Index %3d ($%02X)\nRGB $%X%X%X  (%d, %d, %d)\nVRAM $%05X: $%02X $%02X",
                              i, i, (b1 & 0x0f), (b0 >> 4) & 0x0f, b0 & 0x0f, r, g, b,
                              ADDR_PALETTE + i * 2, b0, b1);
        }
        if ((i % 16) != 15)
            ImGui::SameLine();
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Tile / character viewer tab
// ---------------------------------------------------------------------------
void
draw_tiles_tab(const uint32_t pal[256])
{
    static int   base       = 0x00000;
    static int   bpp_idx    = 2;   // 0:1bpp 1:2bpp 2:4bpp 3:8bpp
    static int   tw_idx     = 0;   // 0:8   1:16
    static int   th_idx     = 0;   // 0:8   1:16
    static int   cols       = 16;
    static int   rows       = 16;
    static int   pal_off    = 0;   // 0..15 (times 16)
    static float zoom       = 2.0f;
    static int   selected   = -1;

    static const int   bpp_vals[4] = { 1, 2, 4, 8 };
    static const char *bpp_names   = "1 bpp\0" "2 bpp\0" "4 bpp\0" "8 bpp\0";
    static const char *dim_names   = "8\0" "16\0";

    // Row 1 — base address (type any hex address to jump straight there) + format.
    // A plain "%05X" field (no "$" in the format) keeps hex text-entry unambiguous.
    ImGui::TextUnformatted("Base $");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::PushItemWidth(90);
    ImGui::InputScalar("##tilebase", ImGuiDataType_S32, &base, nullptr, nullptr, "%05X",
                       ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::Combo("Depth", &bpp_idx, bpp_names);
    ImGui::SameLine();
    ImGui::Combo("W", &tw_idx, dim_names);
    ImGui::SameLine();
    ImGui::Combo("H", &th_idx, dim_names);
    ImGui::PopItemWidth();

    // Geometry (computed here so the navigation controls below can use it).
    const int bpp = bpp_vals[bpp_idx];
    const int tw  = tw_idx ? 16 : 8;
    const int th  = th_idx ? 16 : 8;
    const int bytes_per_tile = tw * th * bpp / 8;
    cols = clampi(cols, 1, 64);
    rows = clampi(rows, 1, 64);
    const int row_step  = cols * bytes_per_tile;
    const int page_step = cols * rows * bytes_per_tile;

    base = clampi(base, 0, 0x1FFFF);

    // Row 2 — jump by tile index + alignment.
    int tile_no = bytes_per_tile ? base / bytes_per_tile : 0;
    ImGui::PushItemWidth(110);
    if (ImGui::InputInt("Tile #", &tile_no, 1, 16))
        base = clampi(tile_no * bytes_per_tile, 0, 0x1FFFF);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::SmallButton("Align"))          // snap to the current tile stride
        base -= base % bytes_per_tile;
    ImGui::SameLine();
    if (ImGui::SmallButton("Align $800"))     // snap to VERA tile-base granularity (2KB)
        base &= ~0x7FF;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Type a hex address in Base, or a tile index in Tile #, to jump.\n"
                          "Step with the buttons below, or drag the scrollbar for fast scrolling.\n"
                          "-1/+1 nudge by one byte; Align snaps to the tile stride ($800 = VERA tilebase).");

    // Row 3 — stepping: home / page / row / tile / byte-nudge.
    if (ImGui::SmallButton("|< Home")) base = 0;
    ImGui::SameLine(); if (ImGui::SmallButton("<< Page")) base -= page_step;
    ImGui::SameLine(); if (ImGui::SmallButton("< Row"))   base -= row_step;
    ImGui::SameLine(); if (ImGui::SmallButton("< Tile"))  base -= bytes_per_tile;
    ImGui::SameLine(); if (ImGui::SmallButton("-1"))      base -= 1;
    ImGui::SameLine(); if (ImGui::SmallButton("+1"))      base += 1;
    ImGui::SameLine(); if (ImGui::SmallButton("Tile >"))  base += bytes_per_tile;
    ImGui::SameLine(); if (ImGui::SmallButton("Row >"))   base += row_step;
    ImGui::SameLine(); if (ImGui::SmallButton("Page >>")) base += page_step;
    base = clampi(base, 0, 0x1FFFF);

    // Row 4 — full-width fast-scroll bar across all of VRAM (tile-aligned).
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderInt("##tilescroll", &base, 0, 0x1FFFF, "scroll -> $%05X"))
        base -= base % bytes_per_tile; // keep tiles aligned while dragging
    ImGui::PopItemWidth();
    base = clampi(base, 0, 0x1FFFF);

    // Row 5 — grid geometry.
    ImGui::PushItemWidth(110);
    ImGui::SliderInt("Columns", &cols, 1, 64);
    ImGui::SameLine();
    ImGui::SliderInt("Rows", &rows, 1, 64);
    ImGui::SameLine();
    ImGui::SliderInt("Pal offset", &pal_off, 0, 15);
    ImGui::SameLine();
    ImGui::SliderFloat("Zoom", &zoom, 1.0f, 8.0f, "%.0fx");
    ImGui::PopItemWidth();

    cols = clampi(cols, 1, 64);
    rows = clampi(rows, 1, 64);
    const int img_w = cols * tw;
    const int img_h = rows * th;
    const int mask  = (1 << bpp) - 1;

    static PixelBuffer buf;
    uint32_t *px = buf.ensure((size_t)img_w * img_h);
    if (!px)
        return;

    for (int t = 0; t < cols * rows; ++t) {
        const int      tx        = t % cols;
        const int      ty        = t / cols;
        const uint32_t tile_addr = (uint32_t)base + (uint32_t)t * bytes_per_tile;
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                const uint32_t bitpos = (uint32_t)(y * tw + x) * bpp;
                const uint8_t  byte   = video_space_read(tile_addr + (bitpos >> 3));
                const int      shift  = 8 - bpp - (int)(bitpos & 7);
                const int      idx    = (byte >> shift) & mask;
                const uint32_t color  = resolve_color(pal, idx, pal_off << 4, false);
                px[(size_t)(ty * th + y) * img_w + (tx * tw + x)] = color;
            }
        }
    }

    static GfxTexture gt;
    if (!gt.ensure(img_w, img_h))
        return;
    gt.update(px);

    ImGui::Separator();
    const ImVec2 img_pos = ImGui::GetCursorScreenPos();
    ImGui::Image(gt.id(), ImVec2(img_w * zoom, img_h * zoom));

    int hovered = -1;
    if (ImGui::IsItemHovered()) {
        const ImVec2 m  = ImGui::GetIO().MousePos;
        const int    lx = (int)((m.x - img_pos.x) / zoom);
        const int    ly = (int)((m.y - img_pos.y) / zoom);
        const int    htx = lx / tw;
        const int    hty = ly / th;
        if (htx >= 0 && htx < cols && hty >= 0 && hty < rows) {
            hovered = hty * cols + htx;
            const uint32_t a = (uint32_t)base + (uint32_t)hovered * bytes_per_tile;
            ImGui::SetTooltip("Tile %d  (col %d, row %d)\nVRAM $%05X..$%05X",
                              hovered, htx, hty, a, a + bytes_per_tile - 1);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                selected = hovered;
        }
    }

    if (selected >= 0 && selected < cols * rows) {
        const uint32_t a = (uint32_t)base + (uint32_t)selected * bytes_per_tile;
        ImGui::Text("Selected tile %d — VRAM $%05X (%d bytes/tile)", selected, a, bytes_per_tile);
    } else {
        ImGui::TextUnformatted("Hover a tile for its address; click to select.");
    }
}

// ---------------------------------------------------------------------------
// Sprite viewer tab
// ---------------------------------------------------------------------------
struct SpriteInfo {
    uint32_t address;
    int      mode; // 0:4bpp 1:8bpp
    int      w, h;
    int      x, y;
    int      zdepth;
    int      pal_off;
    bool     hflip, vflip;
    int      collision;
};

SpriteInfo
decode_sprite_attr(int i)
{
    uint8_t a[8];
    for (int b = 0; b < 8; ++b)
        a[b] = video_space_read(ADDR_SPRITE_ATTR + i * 8 + b);

    SpriteInfo s;
    s.address   = ((uint32_t)a[0] << 5) | ((uint32_t)(a[1] & 0x0f) << 13);
    s.mode      = (a[1] >> 7) & 1;
    s.w         = 1 << (((a[7] >> 4) & 3) + 3);
    s.h         = 1 << (((a[7] >> 6) & 3) + 3);
    s.pal_off   = (a[7] & 0x0f) << 4;
    s.zdepth    = (a[6] >> 2) & 3;
    s.collision = a[6] & 0xf0;
    s.hflip     = a[6] & 1;
    s.vflip     = (a[6] >> 1) & 1;
    s.x         = a[2] | ((a[3] & 3) << 8);
    s.y         = a[4] | ((a[5] & 3) << 8);
    if (s.x >= 0x400 - s.w)
        s.x -= 0x400;
    if (s.y >= 0x400 - s.h)
        s.y -= 0x400;
    return s;
}

// Decode sprite `s` into the atlas cell at (cx, cy). Cells are 64x64 and the
// atlas is atlas_w wide; the sprite is drawn top-left in its cell.
void
decode_sprite_pixels(const SpriteInfo &s, const uint32_t pal[256],
                     uint32_t *atlas, int atlas_w, int cx, int cy)
{
    for (int y = 0; y < s.h; ++y) {
        for (int x = 0; x < s.w; ++x) {
            int idx;
            if (s.mode) { // 8bpp
                idx = video_space_read(s.address + (uint32_t)y * s.w + x);
            } else {      // 4bpp, high nibble = left pixel
                uint8_t byte = video_space_read(s.address + (uint32_t)y * (s.w / 2) + (x / 2));
                idx = (x & 1) ? (byte & 0x0f) : (byte >> 4);
            }
            atlas[(size_t)(cy + y) * atlas_w + (cx + x)] =
                resolve_color(pal, idx, s.pal_off, true);
        }
    }
}

void
draw_sprites_tab(const uint32_t pal[256])
{
    static bool  hide_disabled = false;
    static float zoom          = 2.0f;

    ImGui::Checkbox("Hide disabled (z=0)", &hide_disabled);
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    ImGui::SliderFloat("Preview zoom", &zoom, 1.0f, 4.0f, "%.0fx");
    ImGui::PopItemWidth();

    // One atlas texture holds every sprite: 16 columns x 8 rows of 64x64 cells.
    constexpr int CELL       = 64;
    constexpr int ATLAS_COLS = 16;
    constexpr int ATLAS_ROWS = 8;
    constexpr int ATLAS_W    = CELL * ATLAS_COLS; // 1024
    constexpr int ATLAS_H    = CELL * ATLAS_ROWS; // 512

    static PixelBuffer buf;
    uint32_t *atlas = buf.ensure((size_t)ATLAS_W * ATLAS_H);
    if (!atlas)
        return;
    memset(atlas, 0, (size_t)ATLAS_W * ATLAS_H * sizeof(uint32_t));

    SpriteInfo info[NUM_SPRITES];
    for (int i = 0; i < NUM_SPRITES; ++i) {
        info[i] = decode_sprite_attr(i);
        const int cx = (i % ATLAS_COLS) * CELL;
        const int cy = (i / ATLAS_COLS) * CELL;
        decode_sprite_pixels(info[i], pal, atlas, ATLAS_W, cx, cy);
    }

    static GfxTexture gt;
    if (!gt.ensure(ATLAS_W, ATLAS_H))
        return;
    gt.update(atlas);

    ImGui::Separator();
    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                   DBGUI_TABLE_FLAGS_RESIZABLE;
    if (ImGui::BeginTable("sprites", 9, tflags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Image");
        ImGui::TableSetupColumn("Addr");
        ImGui::TableSetupColumn("Mode");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("X,Y");
        ImGui::TableSetupColumn("Z");
        ImGui::TableSetupColumn("PalOff");
        ImGui::TableSetupColumn("Flip");
        ImGui::TableHeadersRow();

        for (int i = 0; i < NUM_SPRITES; ++i) {
            const SpriteInfo &s = info[i];
            if (hide_disabled && s.zdepth == 0)
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", i);

            ImGui::TableNextColumn();
            const int cx = (i % ATLAS_COLS) * CELL;
            const int cy = (i / ATLAS_COLS) * CELL;
            ImVec2 uv0((float)cx / ATLAS_W, (float)cy / ATLAS_H);
            ImVec2 uv1((float)(cx + s.w) / ATLAS_W, (float)(cy + s.h) / ATLAS_H);
            ImGui::Image(gt.id(), ImVec2(s.w * zoom, s.h * zoom), uv0, uv1);

            ImGui::TableNextColumn();
            ImGui::Text("$%05X", s.address);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.mode ? "8bpp" : "4bpp");
            ImGui::TableNextColumn();
            ImGui::Text("%dx%d", s.w, s.h);
            ImGui::TableNextColumn();
            ImGui::Text("%d,%d", s.x, s.y);
            ImGui::TableNextColumn();
            ImGui::Text("%d", s.zdepth);
            ImGui::TableNextColumn();
            ImGui::Text("$%02X", s.pal_off);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.hflip && s.vflip ? "HV" : s.hflip ? "H" : s.vflip ? "V" : "-");
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Layer helpers (shared by Bitmap + Tilemap tabs)
// ---------------------------------------------------------------------------
struct LayerRegs {
    int      color_depth;   // 0..3
    int      bpp;           // 1,2,4,8
    bool     bitmap_mode;
    bool     text_mode;
    bool     text_256c;
    uint32_t map_base;
    uint32_t tile_base;
    int      mapw, maph;    // in tiles
    int      tilew, tileh;  // in pixels
    int      tile_size_log2;
    int      bitmap_w;      // 320 or 640
    int      bitmap_pal_off;
    int      hscroll, vscroll;
};

LayerRegs
decode_layer_regs(const uint8_t r[7])
{
    uint8_t r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4], r5 = r[5], r6 = r[6];

    LayerRegs L;
    L.color_depth    = r0 & 3;
    L.bpp            = 1 << L.color_depth;
    L.bitmap_mode    = (r0 & 4) != 0;
    L.text_mode      = (L.color_depth == 0) && !L.bitmap_mode;
    L.text_256c      = (r0 & 8) != 0;
    L.map_base       = (uint32_t)r1 << 9;
    L.tile_base      = (uint32_t)(r2 & 0xFC) << 9;
    L.mapw           = 1 << (5 + ((r0 >> 4) & 3));
    L.maph           = 1 << (5 + ((r0 >> 6) & 3));
    L.tilew          = 1 << (3 + (r2 & 1));
    L.tileh          = 1 << (3 + ((r2 >> 1) & 1));
    L.tile_size_log2 = (3 + (r2 & 1)) + (3 + ((r2 >> 1) & 1)) + L.color_depth - 3;
    L.bitmap_w       = (r2 & 1) ? 640 : 320;
    L.bitmap_pal_off = (r4 & 0x0f) << 4;
    L.hscroll        = r3 | ((r4 & 0x0f) << 8);
    L.vscroll        = r5 | ((r6 & 0x0f) << 8);
    return L;
}

LayerRegs
read_layer_regs(int layer)
{
    const uint8_t base = layer == 0 ? 0x0D : 0x14;
    uint8_t r[7];
    for (int i = 0; i < 7; ++i)
        r[i] = video_read(base + i, true);
    return decode_layer_regs(r);
}

// True when two snapshots describe the same tile geometry, i.e. a per-scanline
// snapshot can be substituted for the live one without changing image layout.
bool
same_layer_geometry(const LayerRegs &a, const LayerRegs &b)
{
    return a.color_depth == b.color_depth && a.bitmap_mode == b.bitmap_mode &&
           a.text_mode == b.text_mode && a.text_256c == b.text_256c &&
           a.mapw == b.mapw && a.maph == b.maph &&
           a.tilew == b.tilew && a.tileh == b.tileh;
}

// ---------------------------------------------------------------------------
// Raster-split support
//
// A program can rewrite the layer registers part-way down a frame (typically
// from a line IRQ) so that different horizontal bands of the screen use a
// different MAPBASE/TILEBASE/scroll. A single register snapshot therefore only
// describes the band that happened to be active when the debugger looked, and
// every band below it decodes as garbage. video_get_layer_line_state() reports
// what each scanline actually rendered with, so we index those snapshots by the
// layer row each scanline displayed and decode each pixel row with the
// registers that produced it.
// ---------------------------------------------------------------------------
// Largest layer height VERA can produce: MAPH (256) * TILEH (16).
constexpr int MAX_LAYER_ROWS = 256 * 16;

struct RasterRowRegs {
    uint8_t regs[MAX_LAYER_ROWS][7];
    bool    valid[MAX_LAYER_ROWS];
    bool    any_split; // layer registers changed part-way down the frame
    bool    any_valid; // at least one scanline mapped onto a layer row
};

void
build_raster_row_regs(int layer, const LayerRegs &live, RasterRowRegs &out)
{
    memset(out.valid, 0, sizeof(out.valid));
    out.any_split = false;
    out.any_valid = false;

    const uint16_t lines      = video_get_scanline_count();
    bool           have_first = false;
    uint8_t        first[7]   = {0};

    for (uint16_t line = 0; line < lines; ++line) {
        uint8_t  r[7];
        uint16_t eff_y   = 0;
        bool     enabled = false;
        if (!video_get_layer_line_state((uint8_t)layer, line, r, &eff_y, &enabled))
            continue;
        if (!enabled)
            continue;

        if (!have_first) {
            memcpy(first, r, sizeof(first));
            have_first = true;
        } else if (memcmp(first, r, sizeof(first)) != 0) {
            out.any_split = true;
        }

        // Only substitute snapshots that keep the image layout identical;
        // anything else would change the texture geometry mid-frame.
        const LayerRegs LL = decode_layer_regs(r);
        if (!same_layer_geometry(LL, live))
            continue;

        // Which row of the layer image this scanline showed. VERA forces the
        // scroll registers to 0 in bitmap mode, so eff_y is the row directly.
        int ly;
        if (LL.bitmap_mode) {
            ly = eff_y;
        } else {
            const int layer_h = LL.maph * LL.tileh;
            ly                = ((int)eff_y + LL.vscroll) & (layer_h - 1);
        }
        if (ly < 0 || ly >= MAX_LAYER_ROWS || out.valid[ly])
            continue; // first scanline to show this row wins

        memcpy(out.regs[ly], r, sizeof(out.regs[ly]));
        out.valid[ly] = true;
        out.any_valid = true;
    }
}

// ---------------------------------------------------------------------------
// Bitmap viewer tab
// ---------------------------------------------------------------------------
void
draw_bitmap_tab(const uint32_t pal[256])
{
    static int   layer         = 0;
    static int   height        = 240;
    static float zoom          = 1.0f;
    static bool  follow_raster = true;
    static bool  auto_fit      = true;

    // Default the height to whatever the composer is actually displaying, so a
    // 320x200-style mode shows 200 rows rather than a fixed guess. Any manual
    // edit turns auto-fit off; Fit turns it back on.
    int fit_h = 0;
    video_get_active_layer_size(nullptr, &fit_h);
    if (auto_fit && fit_h > 0)
        height = clampi(fit_h, 1, 480);

    ImGui::PushItemWidth(120);
    ImGui::Combo("Layer", &layer, "Layer 0\0Layer 1\0");
    ImGui::SameLine();
    // DragInt rather than SliderInt: a 120px slider cannot address every value
    // in a 1..480 range, so dragging skips numbers.
    if (ImGui::DragInt("Height", &height, 1.0f, 8, 480))
        auto_fit = false;
    ImGui::SameLine();
    ImGui::SliderFloat("Zoom", &zoom, 1.0f, 4.0f, "%.1fx");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Fit"))
        auto_fit = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Match the active video mode (%d lines)", fit_h);

    LayerRegs L = read_layer_regs(layer);
    ImGui::Text("depth=%dbpp  bitmap=%s  tile_base(data)=$%05X  width=%d  pal_off=$%02X",
                L.bpp, L.bitmap_mode ? "yes" : "no", L.tile_base, L.bitmap_w, L.bitmap_pal_off);
    if (!L.bitmap_mode) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                           "Layer %d is not in bitmap mode; showing raw decode anyway.", layer);
    }

    ImGui::Checkbox("Follow raster", &follow_raster);
    ImGui::SameLine();
    ImGui::TextDisabled("(decode each row with the registers that rendered it)");

    static RasterRowRegs rr;
    build_raster_row_regs(layer, L, rr);
    const bool raster_active = follow_raster && rr.any_valid;

    if (rr.any_split) {
        ImGui::TextColored(raster_active ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.7f, 0.2f, 1),
                           raster_active ? "Raster split detected - rows decoded per scanline."
                                         : "Raster split detected - enable \"Follow raster\".");
    }

    const int img_w = L.bitmap_w;
    const int img_h = clampi(height, 1, 480);
    const int mask  = (1 << L.bpp) - 1;

    static PixelBuffer buf;
    uint32_t *px = buf.ensure((size_t)img_w * img_h);
    if (!px)
        return;

    for (int y = 0; y < img_h; ++y) {
        // Registers in effect on the scanline that displayed this bitmap row.
        LayerRegs LR = L;
        if (raster_active && y < MAX_LAYER_ROWS && rr.valid[y])
            LR = decode_layer_regs(rr.regs[y]);

        const uint32_t row_base = LR.tile_base + (uint32_t)((y * img_w * LR.bpp) >> 3);
        for (int x = 0; x < img_w; ++x) {
            const uint32_t bitpos = (uint32_t)x * LR.bpp;
            const uint8_t  byte   = video_space_read(row_base + (bitpos >> 3));
            const int      shift  = 8 - LR.bpp - (int)(bitpos & 7);
            const int      idx    = (byte >> shift) & mask;
            px[(size_t)y * img_w + x] = resolve_color(pal, idx, LR.bitmap_pal_off, false);
        }
    }

    static GfxTexture gt;
    if (!gt.ensure(img_w, img_h))
        return;
    gt.update(px);

    ImGui::Separator();
    ImGui::BeginChild("bmp", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image(gt.id(), ImVec2(img_w * zoom, img_h * zoom));
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Tilemap viewer tab
// ---------------------------------------------------------------------------
void
draw_tilemap_tab(const uint32_t pal[256])
{
    static int   layer         = 0;
    static int   max_cols      = 32;
    static int   max_rows      = 32;
    static float zoom          = 1.0f;
    static bool  follow_raster = true;
    static bool  auto_fit      = true;

    ImGui::PushItemWidth(120);
    ImGui::Combo("Layer", &layer, "Layer 0\0Layer 1\0");
    ImGui::PopItemWidth();

    LayerRegs L = read_layer_regs(layer);

    // Default the viewport to the tile area the composer is actually showing,
    // so e.g. a 320x200 tile mode opens at 40x25 instead of a fixed 32x32.
    // Any manual edit turns auto-fit off; Fit turns it back on.
    int fit_w = 0, fit_h = 0;
    video_get_active_layer_size(&fit_w, &fit_h);
    const int fit_cols = L.tilew > 0 ? (fit_w + L.tilew - 1) / L.tilew : 0;
    const int fit_rows = L.tileh > 0 ? (fit_h + L.tileh - 1) / L.tileh : 0;
    if (auto_fit && fit_cols > 0 && fit_rows > 0) {
        max_cols = clampi(fit_cols, 1, 128);
        max_rows = clampi(fit_rows, 1, 128);
    }

    ImGui::PushItemWidth(120);
    // DragInt rather than SliderInt: a 120px slider cannot address every value
    // in a 1..128 range, so dragging skips numbers.
    if (ImGui::DragInt("Max cols", &max_cols, 0.25f, 1, 128))
        auto_fit = false;
    ImGui::SameLine();
    if (ImGui::DragInt("Max rows", &max_rows, 0.25f, 1, 128))
        auto_fit = false;
    ImGui::SameLine();
    ImGui::SliderFloat("Zoom", &zoom, 1.0f, 4.0f, "%.1fx");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Fit"))
        auto_fit = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Match the active video mode (%dx%d px = %dx%d tiles)",
                          fit_w, fit_h, fit_cols, fit_rows);

    ImGui::Text("mode=%s  depth=%dbpp  map_base=$%05X  tile_base=$%05X  map=%dx%d  tile=%dx%d",
                L.bitmap_mode ? "bitmap" : (L.text_mode ? (L.text_256c ? "text256" : "text") : "tile"),
                L.bpp, L.map_base, L.tile_base, L.mapw, L.maph, L.tilew, L.tileh);

    if (L.bitmap_mode) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                           "Layer %d is in bitmap mode — use the Bitmap tab.", layer);
        return;
    }

    ImGui::Checkbox("Follow raster", &follow_raster);
    ImGui::SameLine();
    ImGui::TextDisabled("(decode each row with the registers that rendered it)");

    static RasterRowRegs rr;
    build_raster_row_regs(layer, L, rr);
    const bool raster_active = follow_raster && rr.any_valid;

    if (rr.any_split) {
        if (raster_active)
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
                               "Raster split detected - rows decoded per scanline.");
        else
            ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                               "Raster split detected - rows outside the active band will "
                               "decode incorrectly. Enable \"Follow raster\".");
    }

    // Clamp rendered region so the texture stays a sane size.
    int cols = clampi(max_cols < L.mapw ? max_cols : L.mapw, 1, 128);
    int rows = clampi(max_rows < L.maph ? max_rows : L.maph, 1, 128);
    while ((long)cols * L.tilew > 2048)
        --cols;
    while ((long)rows * L.tileh > 2048)
        --rows;

    const int img_w = cols * L.tilew;
    const int img_h = rows * L.tileh;
    const int mask  = (1 << L.bpp) - 1;

    static PixelBuffer buf;
    uint32_t *px = buf.ensure((size_t)img_w * img_h);
    if (!px)
        return;

    for (int ty = 0; ty < rows; ++ty) {
        for (int y = 0; y < L.tileh; ++y) {
            // Registers in effect on the scanline that displayed this pixel row.
            const int layer_row = ty * L.tileh + y;
            LayerRegs LR        = L;
            if (raster_active && layer_row < MAX_LAYER_ROWS && rr.valid[layer_row])
                LR = decode_layer_regs(rr.regs[layer_row]);

            uint32_t *dst = px + (size_t)(ty * L.tileh + y) * img_w;

            for (int tx = 0; tx < cols; ++tx) {
                const uint32_t map_addr = LR.map_base + (uint32_t)(ty * LR.mapw + tx) * 2;
                const uint8_t  byte0    = video_space_read(map_addr);
                const uint8_t  byte1    = video_space_read(map_addr + 1);

                if (LR.text_mode) {
                    // Map entry: byte0 = char, byte1 = fg/bg colors (or full fg in 256c).
                    const int      fg         = LR.text_256c ? byte1 : (byte1 & 0x0f);
                    const int      bg         = LR.text_256c ? 0 : ((byte1 >> 4) & 0x0f);
                    const uint32_t glyph_base = LR.tile_base + (uint32_t)byte0 * (LR.tilew * LR.tileh / 8);
                    for (int x = 0; x < LR.tilew; ++x) {
                        const uint32_t bitpos = (uint32_t)(y * LR.tilew + x);
                        const uint8_t  b      = video_space_read(glyph_base + (bitpos >> 3));
                        const int      on     = (b >> (7 - (bitpos & 7))) & 1;
                        dst[tx * L.tilew + x] = pal[on ? fg : bg];
                    }
                } else {
                    // Tile mode: byte0 low tile index, byte1 = pal offset + flips + index hi.
                    const int      tile_index = byte0 | ((byte1 & 3) << 8);
                    const int      pal_off    = byte1 & 0xf0;
                    const bool     vflip      = (byte1 >> 3) & 1;
                    const bool     hflip      = (byte1 >> 2) & 1;
                    const uint32_t tile_start = LR.tile_base + ((uint32_t)tile_index << LR.tile_size_log2);
                    const int      sy         = vflip ? (LR.tileh - 1 - y) : y;
                    for (int x = 0; x < LR.tilew; ++x) {
                        const int      sx     = hflip ? (LR.tilew - 1 - x) : x;
                        const uint32_t bitpos = (uint32_t)(sy * LR.tilew + sx) * LR.bpp;
                        const uint8_t  byte   = video_space_read(tile_start + (bitpos >> 3));
                        const int      shift  = 8 - LR.bpp - (int)(bitpos & 7);
                        const int      idx    = (byte >> shift) & mask;
                        dst[tx * L.tilew + x] = resolve_color(pal, idx, pal_off, false);
                    }
                }
            }
        }
    }

    static GfxTexture gt;
    if (!gt.ensure(img_w, img_h))
        return;
    gt.update(px);

    if (cols < L.mapw || rows < L.maph)
        ImGui::TextDisabled("Showing %dx%d of %dx%d tiles (raise Max cols/rows).",
                            cols, rows, L.mapw, L.maph);

    ImGui::Separator();
    ImGui::BeginChild("tmap", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image(gt.id(), ImVec2(img_w * zoom, img_h * zoom));
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Registers — the 32 external VERA registers ($9F20-$9F3F) with names, hex,
// binary, and a decoded summary of the most useful fields.
// ---------------------------------------------------------------------------
static const char *
vera_reg_name(int reg, int dcsel)
{
    static const char *fixed[0x20] = {
        "ADDR_L", "ADDR_M", "ADDR_H", "DATA0", "DATA1", "CTRL", "IEN", "ISR",
        "IRQLINE_L", nullptr, nullptr, nullptr, nullptr, // 0x09-0x0C: DCSEL-dependent
        "L0_CONFIG", "L0_MAPBASE", "L0_TILEBASE", "L0_HSCROLL_L", "L0_HSCROLL_H",
        "L0_VSCROLL_L", "L0_VSCROLL_H", "L1_CONFIG", "L1_MAPBASE", "L1_TILEBASE",
        "L1_HSCROLL_L", "L1_HSCROLL_H", "L1_VSCROLL_L", "L1_VSCROLL_H",
        "AUDIO_CTRL", "AUDIO_RATE", "AUDIO_DATA", "SPI_DATA", "SPI_CTRL"
    };
    if (reg >= 0x09 && reg <= 0x0C) {
        static const char *dc0[4] = {"DC_VIDEO", "DC_HSCALE", "DC_VSCALE", "DC_BORDER"};
        static const char *dc1[4] = {"DC_HSTART", "DC_HSTOP", "DC_VSTART", "DC_VSTOP"};
        if (dcsel == 0)
            return dc0[reg - 0x09];
        if (dcsel == 1)
            return dc1[reg - 0x09];
        return "FX";
    }
    return (reg >= 0 && reg < 0x20 && fixed[reg]) ? fixed[reg] : "?";
}

static void
draw_registers_tab()
{
    // CTRL selects which of the DCSEL-banked / data ports are visible.
    uint8_t ctrl    = video_read(0x05, true);
    int     addrsel = ctrl & 1;
    int     dcsel   = (ctrl >> 1) & 0x3f;

    ImGui::Text("CTRL $%02X    ADDRSEL = %d    DCSEL = %d", ctrl, addrsel, dcsel);
    ImGui::TextDisabled("VERA external registers $9F20-$9F3F. Hover a value for hex/dec/bin.");
    ImGui::Separator();

    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
                             DBGUI_TABLE_FLAGS_RESIZABLE;
    if (ImGui::BeginTable("vera_regs", 4, tflags, ImVec2(0, 300))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Reg");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Hex");
        ImGui::TableSetupColumn("Binary");
        ImGui::TableHeadersRow();

        for (int r = 0; r < 0x20; r++) {
            // 0x09-0x0C are write-only FX registers when DCSEL>=2 — reading a
            // write-only VERA register logs a warning.
            bool fx_wo = (r >= 0x09 && r <= 0x0C && dcsel >= 2);
            // AUDIO_CTRL (0x1B) runs audio_render() and SPI_DATA (0x1E) starts a
            // real SPI byte transfer *on read* — video_read ignores debugOn for
            // these, so passively polling them every frame would corrupt audio /
            // SD-card I/O. Never read them here; show a placeholder instead.
            bool side_effect = (r == 0x1B || r == 0x1E);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("$9F%02X", 0x20 + r);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(vera_reg_name(r, dcsel));

            if (fx_wo || side_effect) {
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("--");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled(fx_wo ? "(write-only)" : "(read has side effects)");
            } else {
                uint8_t v = video_read((uint8_t)r, true);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("$%02X", v);
                dbgui_hover_value_tooltip(vera_reg_name(r, dcsel), v, 1);
                ImGui::TableSetColumnIndex(3);
                char bin[12];
                dbgui_format_binary(bin, sizeof bin, v, 8);
                ImGui::TextUnformatted(bin);
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Decoded", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Active data-port address + increment code.
        uint8_t  al   = video_read(0x00, true);
        uint8_t  am   = video_read(0x01, true);
        uint8_t  ah   = video_read(0x02, true);
        uint32_t addr = ((uint32_t)(ah & 1) << 16) | ((uint32_t)am << 8) | al;
        ImGui::Text("Data port %d address: $%05X   (increment code %d)", addrsel, addr, (ah >> 3) & 0x1f);

        if (dcsel == 0) {
            uint8_t            dcv      = video_read(0x09, true);
            static const char *omode[4] = {"disabled", "VGA", "NTSC", "RGB/component"};
            ImGui::Text("DC_VIDEO $%02X: output=%s  layer0=%s  layer1=%s  sprites=%s", dcv,
                        omode[dcv & 3], (dcv & 0x10) ? "ON" : "off",
                        (dcv & 0x20) ? "ON" : "off", (dcv & 0x40) ? "ON" : "off");
            uint8_t hs = video_read(0x0A, true);
            uint8_t vs = video_read(0x0B, true);
            ImGui::Text("Scale: H=%d (%.2fx)  V=%d (%.2fx)    Border color=%d", hs, hs / 128.0f,
                        vs, vs / 128.0f, video_read(0x0C, true));
        } else if (dcsel == 1) {
            ImGui::Text("Active area: H %d..%d,  V %d..%d", video_read(0x09, true) << 2,
                        video_read(0x0A, true) << 2, video_read(0x0B, true) << 1,
                        video_read(0x0C, true) << 1);
        } else {
            ImGui::TextDisabled("DCSEL=%d selects FX registers; set DCSEL=0 to see DC_VIDEO etc.", dcsel);
        }

        static const char *depth[4] = {"1bpp", "2bpp", "4bpp", "8bpp"};
        for (int layer = 0; layer < 2; layer++) {
            int     base  = layer == 0 ? 0x0D : 0x14;
            uint8_t cfg   = video_read((uint8_t)base, true);
            uint8_t mapb  = video_read((uint8_t)(base + 1), true);
            uint8_t tileb = video_read((uint8_t)(base + 2), true);
            ImGui::Text("Layer %d $%02X: %s%s%s  map %dx%d  mapbase $%05X  tilebase $%05X  tile %dx%d",
                        layer, cfg, depth[cfg & 3], (cfg & 4) ? " bitmap" : "",
                        (cfg & 8) ? " T256C" : "", 32 << ((cfg >> 4) & 3), 32 << ((cfg >> 6) & 3),
                        (uint32_t)mapb << 9, (uint32_t)(tileb & 0xFC) << 9, 8 << (tileb & 1),
                        8 << ((tileb >> 1) & 1));
        }
    }
}

// ---------------------------------------------------------------------------
// Panel entry point
// ---------------------------------------------------------------------------
void
vera_panel_render(bool *p_open)
{
    if (ImGui::Begin("VERA", p_open)) {
        dbgui_window_zoom("vera");
        if (debug_ui_get_renderer() == nullptr) {
            ImGui::TextUnformatted("Debugger renderer unavailable.");
            dbgui_window_end();
            return;
        }

        uint32_t pal[256];
        build_palette(pal);

        if (ImGui::BeginTabBar("vera_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Registers")) {
                draw_registers_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Palette")) {
                draw_palette_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tiles")) {
                draw_tiles_tab(pal);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sprites")) {
                draw_sprites_tab(pal);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Bitmap")) {
                draw_bitmap_tab(pal);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tilemap")) {
                draw_tilemap_tab(pal);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    dbgui_window_end();
}

} // namespace

static DebugPanelRegistration s_reg("VERA", vera_panel_render, true);
