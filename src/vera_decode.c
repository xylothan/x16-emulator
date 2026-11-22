#include "vera_decode.h"

vera_color_t
vera_palette_decode(uint8_t byte0, uint8_t byte1)
{
	vera_color_t c;
	c.r   = byte1 & 0x0f;
	c.g   = byte0 >> 4;
	c.b   = byte0 & 0x0f;
	c.rgb = (uint16_t)((c.r << 8) | (c.g << 4) | c.b);
	return c;
}

uint32_t
vera_color_abgr(vera_color_t color)
{
	const uint32_t r = (uint32_t)color.r * 0x11u;
	const uint32_t g = (uint32_t)color.g * 0x11u;
	const uint32_t b = (uint32_t)color.b * 0x11u;
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}

int
vera_color_index(int value, int palette_offset, bool t256c)
{
	int index = value;
	if (value > 0 && value < 16) {
		index += palette_offset;
		if (t256c) {
			index |= 0x80;
		}
	}
	return index & 0xff;
}

vera_sprite_t
vera_sprite_decode(const uint8_t a[8])
{
	vera_sprite_t s;
	s.address   = ((uint32_t)a[0] << 5) | ((uint32_t)(a[1] & 0x0f) << 13);
	s.mode      = (a[1] >> 7) & 1;
	s.w         = 1 << (((a[7] >> 4) & 3) + 3);
	s.h         = 1 << (((a[7] >> 6) & 3) + 3);
	s.pal_off   = (a[7] & 0x0f) << 4;
	s.zdepth    = (a[6] >> 2) & 3;
	s.collision = a[6] & 0xf0;
	s.hflip     = (a[6] & 1) != 0;
	s.vflip     = ((a[6] >> 1) & 1) != 0;
	s.x         = a[2] | ((a[3] & 3) << 8);
	s.y         = a[4] | ((a[5] & 3) << 8);
	if (s.x >= 0x400 - s.w) {
		s.x -= 0x400;
	}
	if (s.y >= 0x400 - s.h) {
		s.y -= 0x400;
	}
	return s;
}

int
vera_sprite_pixel(vera_read_fn read, const vera_sprite_t *s, int x, int y)
{
	if (s->mode) {
		return read((s->address + (uint32_t)y * s->w + (uint32_t)x) & VERA_VRAM_MASK);
	}
	const uint8_t byte = read((s->address + (uint32_t)y * (s->w / 2) + (uint32_t)(x / 2)) & VERA_VRAM_MASK);
	return (x & 1) ? (byte & 0x0f) : (byte >> 4);
}

int
vera_packed_pixel(vera_read_fn read, uint32_t start, int width, int bpp, int x, int y)
{
	const uint32_t bitpos = ((uint32_t)y * width + (uint32_t)x) * bpp;
	const uint8_t  byte   = read((start + (bitpos >> 3)) & VERA_VRAM_MASK);
	const int      shift  = 8 - bpp - (int)(bitpos & 7);
	return (byte >> shift) & ((1 << bpp) - 1);
}

vera_layer_t
vera_layer_decode(const uint8_t r[VERA_LAYER_REG_COUNT])
{
	vera_layer_t L;
	L.color_depth    = r[0] & 3;
	L.bpp            = 1 << L.color_depth;
	L.bitmap_mode    = (r[0] & 4) != 0;
	L.text_mode      = L.color_depth == 0 && !L.bitmap_mode;
	L.text_256c      = (r[0] & 8) != 0;
	L.map_base       = (uint32_t)r[1] << 9;
	L.tile_base      = (uint32_t)(r[2] & 0xFC) << 9;
	L.mapw           = 1 << (5 + ((r[0] >> 4) & 3));
	L.maph           = 1 << (5 + ((r[0] >> 6) & 3));
	L.tilew          = 1 << (3 + (r[2] & 1));
	L.tileh          = 1 << (3 + ((r[2] >> 1) & 1));
	L.tile_size_log2 = (3 + (r[2] & 1)) + (3 + ((r[2] >> 1) & 1)) + L.color_depth - 3;
	L.bitmap_w       = (r[2] & 1) ? 640 : 320;
	L.bitmap_pal_off = (r[4] & 0x0f) << 4;
	L.hscroll        = L.bitmap_mode ? 0 : (r[3] | ((r[4] & 0x0f) << 8));
	L.vscroll        = L.bitmap_mode ? 0 : (r[5] | ((r[6] & 0x0f) << 8));
	return L;
}

bool
vera_layer_enabled(uint8_t dc_video, int layer)
{
	return (dc_video & 3) != 0 && ((dc_video >> (4 + (layer & 1))) & 1) != 0;
}

bool
vera_sprites_enabled(uint8_t dc_video)
{
	return (dc_video & 3) != 0 && (dc_video & 0x40) != 0;
}

uint32_t
vera_map_address(const vera_layer_t *L, int col, int row)
{
	return (L->map_base + ((uint32_t)row * L->mapw + (uint32_t)col) * 2) & VERA_VRAM_MASK;
}

bool
vera_map_wraps(const vera_layer_t *L)
{
	return L->map_base + (uint32_t)L->mapw * L->maph * 2 > VERA_VRAM_SIZE;
}

vera_map_entry_t
vera_map_decode(const vera_layer_t *L, uint32_t address, uint8_t byte0, uint8_t byte1)
{
	vera_map_entry_t e;
	e.address = address & VERA_VRAM_MASK;
	e.byte0   = byte0;
	e.byte1   = byte1;
	if (L->text_mode) {
		e.tile    = byte0;
		e.pal_off = 0;
		e.hflip   = false;
		e.vflip   = false;
		e.fg      = L->text_256c ? byte1 : (byte1 & 0x0f);
		e.bg      = L->text_256c ? 0 : (byte1 >> 4);
	} else {
		e.tile    = byte0 | ((byte1 & 3) << 8);
		e.pal_off = byte1 & 0xf0;
		e.vflip   = ((byte1 >> 3) & 1) != 0;
		e.hflip   = ((byte1 >> 2) & 1) != 0;
		e.fg      = 0;
		e.bg      = 0;
	}
	e.tile_address = (L->tile_base + ((uint32_t)e.tile << L->tile_size_log2)) & VERA_VRAM_MASK;
	return e;
}

vera_map_entry_t
vera_map_read(vera_read_fn read, const vera_layer_t *L, int col, int row)
{
	const uint32_t address = vera_map_address(L, col, row);
	const uint8_t  byte0   = read(address);
	const uint8_t  byte1   = read((address + 1) & VERA_VRAM_MASK);
	return vera_map_decode(L, address, byte0, byte1);
}

int
vera_map_pixel(vera_read_fn read, const vera_layer_t *L, const vera_map_entry_t *e, int x, int y)
{
	const int sx = e->hflip ? L->tilew - 1 - x : x;
	const int sy = e->vflip ? L->tileh - 1 - y : y;
	return vera_packed_pixel(read, e->tile_address, L->tilew, L->bpp, sx, sy);
}
