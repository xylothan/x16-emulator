// VERA's graphics state, decoded from its raw bytes: palette entries, sprite
// attributes, layer registers and tile map entries.
//
// Depends on nothing in the emulator. Callers pass the bytes, or a function that
// reads VRAM, and every address is masked to VRAM's 17 bits before it is read.
// The field meanings follow the renderer in video.c.

#ifndef VERA_DECODE_H
#define VERA_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VERA_VRAM_SIZE        0x20000u
#define VERA_VRAM_MASK        0x1FFFFu
#define VERA_PALETTE_ADDR     0x1FA00u
#define VERA_PALETTE_ENTRIES  256
#define VERA_SPRITE_ATTR_ADDR 0x1FC00u
#define VERA_SPRITE_COUNT     128
#define VERA_LAYER_REG_COUNT  7

typedef uint8_t (*vera_read_fn)(uint32_t address);

typedef struct {
	uint8_t  r, g, b; // 0..15
	uint16_t rgb;     // $0RGB
} vera_color_t;

// byte0 = GGGGBBBB, byte1 = xxxxRRRR.
vera_color_t vera_palette_decode(uint8_t byte0, uint8_t byte1);

// ABGR8888, opaque, each channel expanded by nibble replication.
uint32_t vera_color_abgr(vera_color_t color);

// The palette index a pixel value selects. Values 1 to 15 take the palette
// offset, and with T256C set also bit 7. Sprites and 8bpp values above 15 do not.
int vera_color_index(int value, int palette_offset, bool t256c);

typedef struct {
	uint32_t address;
	int      mode; // 0: 4bpp, 1: 8bpp
	int      w, h;
	int      x, y; // signed: the top of the 10-bit range is off the left or top
	int      zdepth;
	int      pal_off;   // added to the colour index: 0, 16 .. 240
	bool     hflip, vflip;
	int      collision; // attribute byte 6 & $F0
} vera_sprite_t;

vera_sprite_t vera_sprite_decode(const uint8_t attr[8]);

// Colour value at (x, y) of the sprite's stored image, before any flip.
int vera_sprite_pixel(vera_read_fn read, const vera_sprite_t *sprite, int x, int y);

// Colour value at (x, y) of an image packed at bpp 1, 2, 4 or 8, `width` pixels
// to the row, most significant bits leftmost.
int vera_packed_pixel(vera_read_fn read, uint32_t start, int width, int bpp, int x, int y);

typedef struct {
	int      color_depth; // 0..3
	int      bpp;         // 1, 2, 4, 8
	bool     bitmap_mode;
	bool     text_mode;   // 1bpp and not bitmap
	bool     text_256c;   // the T256C bit, which every mode reads
	uint32_t map_base;
	uint32_t tile_base;
	int      mapw, maph;   // in tiles
	int      tilew, tileh; // in pixels
	int      tile_size_log2;
	int      bitmap_w;       // 320 or 640
	int      bitmap_pal_off; // 0, 16 .. 240
	int      hscroll, vscroll; // 0 in bitmap mode
} vera_layer_t;

// r holds L_CONFIG, L_MAPBASE, L_TILEBASE, L_HSCROLL_L/H, L_VSCROLL_L/H.
vera_layer_t vera_layer_decode(const uint8_t r[VERA_LAYER_REG_COUNT]);

// From DC_VIDEO. Output mode 0 shows nothing, whatever the enable bits say.
bool vera_layer_enabled(uint8_t dc_video, int layer);
bool vera_sprites_enabled(uint8_t dc_video);

typedef struct {
	uint32_t address;      // of byte0
	uint8_t  byte0, byte1;
	int      tile;         // 8 bits in text mode, 10 in tile modes
	uint32_t tile_address;
	int      pal_off;      // tile modes: 0, 16 .. 240
	bool     hflip, vflip; // tile modes
	int      fg, bg;       // text mode; with T256C, fg is byte1 and bg is 0
} vera_map_entry_t;

// Wraps at the end of VRAM.
uint32_t vera_map_address(const vera_layer_t *layer, int col, int row);

// True when the layer's whole map does not fit before the end of VRAM.
bool vera_map_wraps(const vera_layer_t *layer);

vera_map_entry_t vera_map_decode(const vera_layer_t *layer, uint32_t address, uint8_t byte0,
                                 uint8_t byte1);
vera_map_entry_t vera_map_read(vera_read_fn read, const vera_layer_t *layer, int col, int row);

// Colour value at (x, y) of the entry's tile as displayed, flips applied.
int vera_map_pixel(vera_read_fn read, const vera_layer_t *layer, const vera_map_entry_t *entry,
                   int x, int y);

#ifdef __cplusplus
}
#endif

#endif
