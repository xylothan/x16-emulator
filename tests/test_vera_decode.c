// VERA graphics decoding over a fake VRAM.
// Field layouts: x16-docs, VERA Programmer's Reference; src/video.c.

#include "support/harness.h"

#include "../src/vera_decode.h"

#include <string.h>

static uint8_t  g_vram[VERA_VRAM_SIZE];
static uint32_t g_out_of_range;

static uint8_t
fake_read(uint32_t address)
{
	if (address >= VERA_VRAM_SIZE) {
		g_out_of_range++;
		return 0xEE;
	}
	return g_vram[address];
}

static vera_layer_t
layer_of(uint8_t config, uint8_t mapbase, uint8_t tilebase)
{
	const uint8_t r[VERA_LAYER_REG_COUNT] = { config, mapbase, tilebase, 0, 0, 0, 0 };
	return vera_layer_decode(r);
}

static void
test_palette(void)
{
	const vera_color_t c = vera_palette_decode(0xA5, 0xF3);
	check_eq(c.r, 3, "red is the low nibble of byte 1");
	check_eq(c.g, 0xA, "green is the high nibble of byte 0");
	check_eq(c.b, 5, "blue is the low nibble of byte 0");
	check_eq(c.rgb, 0x3A5, "12-bit value");
	check_eq(vera_color_abgr(c), 0xFF55AA33u, "ABGR by nibble replication");
}

static void
test_color_index(void)
{
	check_eq(vera_color_index(0, 0x40, true), 0, "0 takes no offset");
	check_eq(vera_color_index(5, 0x40, false), 0x45, "1-15 take the offset");
	check_eq(vera_color_index(5, 0x40, true), 0xC5, "T256C sets bit 7");
	check_eq(vera_color_index(16, 0x40, true), 16, "16 and up are unchanged");
	check_eq(vera_color_index(15, 0xF0, false), 0xFF, "the top of the range");
}

static void
test_sprite_fields(void)
{
	const uint8_t a[8] = { 0x34, 0x8A, 0x21, 0x01, 0x43, 0x02, 0x5B, 0x97 };
	const vera_sprite_t s = vera_sprite_decode(a);
	check_eq(s.address, (0x34u << 5) | (0xAu << 13), "address");
	check_eq((uint32_t)s.mode, 1, "bit 7 of byte 1 is 8bpp");
	check_eq((uint32_t)s.x, 0x121, "x");
	check_eq((uint32_t)s.y, 0x243, "y");
	check_eq((uint32_t)s.w, 16, "width code 1");
	check_eq((uint32_t)s.h, 32, "height code 2");
	check_eq((uint32_t)s.zdepth, 2, "z-depth");
	check_eq((uint32_t)s.collision, 0x50, "collision mask");
	check(s.hflip, "h flip");
	check(s.vflip, "v flip");
	check_eq((uint32_t)s.pal_off, 0x70, "palette offset");
}

static void
test_sprite_negative_position(void)
{
	uint8_t a[8] = { 0 };
	a[2] = 0xF8; a[3] = 0x03;
	a[4] = 0xFF; a[5] = 0x03;
	vera_sprite_t s = vera_sprite_decode(a);
	check(s.x == -8, "x within a sprite's width of 1024 is negative");
	check(s.y == -1, "y likewise");
	a[2] = 0xF7;
	s = vera_sprite_decode(a);
	check(s.x == 0x3F7, "one further is not");
}

static void
test_sprite_pixels(void)
{
	memset(g_vram, 0, sizeof g_vram);
	uint8_t a[8] = { 0x10, 0x00, 0, 0, 0, 0, 0, 0 };
	vera_sprite_t s = vera_sprite_decode(a);
	g_vram[s.address + 4 + 1] = 0x9C;
	check_eq((uint32_t)vera_sprite_pixel(fake_read, &s, 2, 1), 9, "4bpp even x is the high nibble");
	check_eq((uint32_t)vera_sprite_pixel(fake_read, &s, 3, 1), 0xC, "4bpp odd x is the low nibble");
	a[1] = 0x80;
	s = vera_sprite_decode(a);
	g_vram[s.address + 8 * 2 + 3] = 0x7B;
	check_eq((uint32_t)vera_sprite_pixel(fake_read, &s, 3, 2), 0x7B, "8bpp");
}

static void
test_sprite_pixels_stay_in_vram(void)
{
	memset(g_vram, 0, sizeof g_vram);
	g_out_of_range = 0;
	const uint8_t a[8] = { 0xFF, 0x8F, 0, 0, 0, 0, 0, 0xF0 };
	const vera_sprite_t s = vera_sprite_decode(a);
	check_eq(s.address, 0x1FFE0, "the highest sprite address");
	g_vram[(s.address + 63u * 64 + 63) & VERA_VRAM_MASK] = 0x42;
	check_eq((uint32_t)vera_sprite_pixel(fake_read, &s, 63, 63), 0x42, "a sprite past the end wraps");
	check_eq(g_out_of_range, 0, "and reads nothing out of range");
}

static void
test_layer_fields(void)
{
	const uint8_t r[VERA_LAYER_REG_COUNT] = { 0x6A, 0x80, 0x93, 0x34, 0xF2, 0x78, 0xF5 };
	const vera_layer_t L = vera_layer_decode(r);
	check_eq((uint32_t)L.bpp, 4, "depth 2 is 4bpp");
	check(!L.bitmap_mode && !L.text_mode, "a tile mode");
	check(L.text_256c, "T256C");
	check_eq((uint32_t)L.mapw, 128, "map width code 2");
	check_eq((uint32_t)L.maph, 64, "map height code 1");
	check_eq(L.map_base, 0x80u << 9, "map base");
	check_eq(L.tile_base, 0x90u << 9, "tile base drops the size bits");
	check_eq((uint32_t)L.tilew, 16, "tile width");
	check_eq((uint32_t)L.tileh, 16, "tile height");
	check_eq((uint32_t)L.tile_size_log2, 4 + 4 + 2 - 3, "tile size");
	check_eq((uint32_t)L.hscroll, 0x234, "h scroll");
	check_eq((uint32_t)L.vscroll, 0x578, "v scroll");
}

static void
test_bitmap_layer(void)
{
	const uint8_t r[VERA_LAYER_REG_COUNT] = { 0x07, 0, 0x01, 0x34, 0x05, 0x78, 0x01 };
	const vera_layer_t L = vera_layer_decode(r);
	check(L.bitmap_mode && !L.text_mode, "bit 2 is bitmap");
	check_eq((uint32_t)L.bpp, 8, "8bpp");
	check_eq((uint32_t)L.bitmap_w, 640, "TILEW selects 640");
	check_eq((uint32_t)L.bitmap_pal_off, 0x50, "palette offset from HSCROLL_H");
	check(L.hscroll == 0 && L.vscroll == 0, "bitmap modes do not scroll");

	const vera_layer_t one = layer_of(0x04, 0, 0);
	check(one.bitmap_mode && !one.text_mode, "1bpp bitmap is not text");
}

static void
test_enable_bits(void)
{
	check(vera_layer_enabled(0x11, 0), "layer 0");
	check(!vera_layer_enabled(0x11, 1), "layer 1 off");
	check(vera_layer_enabled(0x22, 1), "layer 1 on NTSC");
	check(!vera_layer_enabled(0x30, 0), "output mode 0 shows no layer");
	check(vera_sprites_enabled(0x41), "sprites");
	check(!vera_sprites_enabled(0x40), "output mode 0 shows no sprites");
	check(!vera_sprites_enabled(0x31), "sprite bit clear");
}

static void
test_text_entries(void)
{
	const vera_layer_t L = layer_of(0x10, 0x10, 0x20);
	check(L.text_mode, "1bpp tiles are text");

	vera_map_entry_t e = vera_map_decode(&L, 0x2000, 0x41, 0x6E);
	check_eq((uint32_t)e.tile, 0x41, "byte 0 is the character");
	check_eq((uint32_t)e.fg, 0xE, "foreground");
	check_eq((uint32_t)e.bg, 0x6, "background");
	check_eq(e.tile_address, L.tile_base + 0x41u * 8, "the glyph");
	check(!e.hflip && !e.vflip && e.pal_off == 0, "byte 1 is not flips or an offset");

	const vera_layer_t c256 = layer_of(0x18, 0x10, 0x20);
	e = vera_map_decode(&c256, 0x2000, 0x41, 0x6E);
	check_eq((uint32_t)e.fg, 0x6E, "T256C foreground is all of byte 1");
	check_eq((uint32_t)e.bg, 0, "T256C background is 0");
}

static void
test_tile_entries(void)
{
	const int depths[3] = { 1, 2, 3 };
	for (int i = 0; i < 3; i++) {
		const vera_layer_t L = layer_of((uint8_t)depths[i], 0x10, 0x23);
		const vera_map_entry_t e = vera_map_decode(&L, 0x2000, 0x34, 0xBE);
		check_eq((uint32_t)e.tile, 0x234, "ten-bit tile index");
		check_eq((uint32_t)e.pal_off, 0xB0, "palette offset");
		check(e.vflip, "bit 3 is v flip");
		check(e.hflip, "bit 2 is h flip");
		check_eq(e.tile_address, (L.tile_base + (0x234u << (5 + depths[i]))) & VERA_VRAM_MASK,
		         "16x16 tile stride");
	}
}

static void
test_tile_pixels_and_flips(void)
{
	memset(g_vram, 0, sizeof g_vram);
	const vera_layer_t L = layer_of(0x02, 0x10, 0x00); // 4bpp, 8x8
	g_vram[32 + 4 * 2 + 1] = 0x7D;                     // tile 1, row 2, pixels 2 and 3

	vera_map_entry_t e = vera_map_decode(&L, 0, 1, 0x00);
	check_eq((uint32_t)vera_map_pixel(fake_read, &L, &e, 2, 2), 7, "high nibble is the left pixel");
	check_eq((uint32_t)vera_map_pixel(fake_read, &L, &e, 3, 2), 0xD, "low nibble is the right");

	e = vera_map_decode(&L, 0, 1, 0x04);
	check_eq((uint32_t)vera_map_pixel(fake_read, &L, &e, 5, 2), 7, "h flip mirrors x");
	check_eq((uint32_t)vera_map_pixel(fake_read, &L, &e, 4, 2), 0xD, "and the other pixel");

	e = vera_map_decode(&L, 0, 1, 0x08);
	check_eq((uint32_t)vera_map_pixel(fake_read, &L, &e, 2, 5), 7, "v flip mirrors y");

	e = vera_map_decode(&L, 0, 1, 0x0C);
	check_eq((uint32_t)vera_map_pixel(fake_read, &L, &e, 5, 5), 7, "both");
}

static void
test_packed_depths(void)
{
	memset(g_vram, 0, sizeof g_vram);
	g_vram[0x100] = 0xB1; // 10 11 00 01
	check_eq((uint32_t)vera_packed_pixel(fake_read, 0x100, 8, 1, 0, 0), 1, "1bpp leftmost is bit 7");
	check_eq((uint32_t)vera_packed_pixel(fake_read, 0x100, 8, 1, 1, 0), 0, "1bpp next");
	check_eq((uint32_t)vera_packed_pixel(fake_read, 0x100, 8, 2, 0, 0), 2, "2bpp first");
	check_eq((uint32_t)vera_packed_pixel(fake_read, 0x100, 8, 2, 3, 0), 1, "2bpp last");
	check_eq((uint32_t)vera_packed_pixel(fake_read, 0x100, 8, 4, 1, 0), 1, "4bpp second");
	check_eq((uint32_t)vera_packed_pixel(fake_read, 0x100, 8, 8, 0, 0), 0xB1, "8bpp");
}

static void
test_map_wraps_at_end_of_vram(void)
{
	memset(g_vram, 0, sizeof g_vram);
	g_out_of_range = 0;
	const vera_layer_t L = layer_of(0xF2, 0xFF, 0x00); // 256x256 at $1FE00
	check_eq(L.map_base, 0x1FE00, "map base");
	check(vera_map_wraps(&L), "the map runs past the end");

	check_eq(vera_map_address(&L, 255, 0), 0x1FFFE, "the last entry before the end");
	check_eq(vera_map_address(&L, 0, 1), 0x00000, "the next row wraps to 0");

	g_vram[0x1FFFF] = 0x12;
	g_vram[0x00000] = 0x77;
	g_vram[0x00001] = 0x03;
	vera_map_entry_t e = vera_map_read(fake_read, &L, 0, 1);
	check_eq((uint32_t)e.byte0, 0x77, "byte 0 from the wrapped address");
	check_eq((uint32_t)e.tile, 0x377, "and its tile");

	const vera_layer_t exact = layer_of(0x02, 0xFC, 0x00); // 32x32 at $1F800
	check(!vera_map_wraps(&exact), "a map ending exactly at the end does not wrap");
	const vera_layer_t over = layer_of(0x02, 0xFD, 0x00);  // 32x32 at $1FA00
	check(vera_map_wraps(&over), "one 512-byte step later it does");

	for (int row = 0; row < 256; row += 17) {
		for (int col = 0; col < 256; col += 13) {
			e = vera_map_read(fake_read, &L, col, row);
			(void)vera_map_pixel(fake_read, &L, &e, 15, 15);
		}
	}
	check_eq(g_out_of_range, 0, "nothing was read out of range");

	const vera_layer_t fits = layer_of(0x00, 0x00, 0x00);
	check(!vera_map_wraps(&fits), "a 32x32 map at 0 fits");
}

static void
test_map_entry_straddling_the_end(void)
{
	memset(g_vram, 0, sizeof g_vram);
	g_out_of_range = 0;
	vera_layer_t L = layer_of(0x02, 0xFF, 0x00);
	L.map_base = 0x1FFFF;
	g_vram[0x1FFFF] = 0x21;
	g_vram[0x00000] = 0x01;
	const vera_map_entry_t e = vera_map_read(fake_read, &L, 0, 0);
	check_eq((uint32_t)e.tile, 0x121, "byte 1 of an entry at the last byte is at 0");
	check_eq(g_out_of_range, 0, "and the read stays in range");
}

static void
test_tile_data_wraps(void)
{
	memset(g_vram, 0, sizeof g_vram);
	g_out_of_range = 0;
	const vera_layer_t L = layer_of(0x03, 0x00, 0xFF); // 8bpp, 16x16, tiles at $1F800
	const vera_map_entry_t e = vera_map_decode(&L, 0, 0xFF, 0x03);
	const uint32_t raw = L.tile_base + (0x3FFu << L.tile_size_log2);
	check(raw >= VERA_VRAM_SIZE, "the unmasked tile address is past the end");
	check_eq(e.tile_address, raw & VERA_VRAM_MASK, "the tile address wraps");
	(void)vera_map_pixel(fake_read, &L, &e, 15, 15);
	check_eq(g_out_of_range, 0, "tile data stays in range");
}

int
main(void)
{
	test_palette();
	test_color_index();
	test_sprite_fields();
	test_sprite_negative_position();
	test_sprite_pixels();
	test_sprite_pixels_stay_in_vram();
	test_layer_fields();
	test_bitmap_layer();
	test_enable_bits();
	test_text_entries();
	test_tile_entries();
	test_tile_pixels_and_flips();
	test_packed_depths();
	test_map_wraps_at_end_of_vram();
	test_map_entry_straddling_the_end();
	test_tile_data_wraps();
	return x16_test_summary("vera_decode");
}
