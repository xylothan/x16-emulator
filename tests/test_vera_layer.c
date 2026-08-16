// VERA's layer registers, and the addresses derived from them.
//
// $9F2D-$9F33 (layer 0) and $9F34-$9F3A (layer 1) carry the map and tile base
// addresses, the map and tile geometry, and the scroll offsets. Every one of
// them is a shift or a bit field, which is the kind of arithmetic that goes
// wrong quietly: a wrong shift puts the tilemap somewhere plausible.
//
// Two things are checked. The registers themselves -- what a write stores and
// what a read reports -- and the two base addresses the emulator derives from
// them, observed through video_is_tilemap_address() and
// video_is_tiledata_address(). Those two are what the debugger uses to label
// VRAM, so a wrong base mislabels a memory view as well as rendering wrongly.
//
// Not covered: the horizontal tile-column arithmetic and the per-row strides
// within a tile. Both are only visible in rendered pixels, and video.c's
// framebuffer has no accessor -- that seam belongs to the backend work, not
// here. Nothing below claims to reach them.
//
// ORACLE: B -- X16Community/vera-module Verilog, tag v47.0.2
// (45cc1f053376dae12173ea63612820e4d289c0da), quoted inline.
// layer_renderer.v is identical at v48.0.1 and at the tip: its last change was
// a37697832a (2024-01-17), which v48.0.1 already contains.

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <stdint.h>

#define L0_CONFIG   0x0D
#define L0_MAPBASE  0x0E
#define L0_TILEBASE 0x0F
#define L0_HSCROLL_L 0x10
#define L0_HSCROLL_H 0x11
#define L0_VSCROLL_L 0x12
#define L0_VSCROLL_H 0x13

// $9F34-$9F3A repeat the same seven registers for layer 1.
#define LAYER_STRIDE 7

static uint8_t
reg_of(unsigned layer, uint8_t l0_reg)
{
	return (uint8_t)(l0_reg + layer * LAYER_STRIDE);
}

static void
write_layer(unsigned layer, uint8_t l0_reg, uint8_t value)
{
	video_write(reg_of(layer, l0_reg), value);
}

static uint8_t
read_layer(unsigned layer, uint8_t l0_reg)
{
	return video_read(reg_of(layer, l0_reg), false);
}

// top.v:211-217 (layer 0; :219-225 repeat it for layer 1)
//     5'h0D: rddata = {l0_map_height_r, l0_map_width_r, l0_attr_mode_r, l0_bitmap_mode_r, l0_color_depth_r};
//     5'h0E: rddata = l0_map_baseaddr_r;
//     5'h0F: rddata = {l0_tile_baseaddr_r[7:2], l0_tile_height_r, l0_tile_width_r};
//     5'h10: rddata = l0_hscroll_r[7:0];
//     5'h12: rddata = l0_vscroll_r[7:0];
//
// Every bit of these five has a field behind it, so each one round-trips whole.
static void
test_the_full_width_layer_registers_round_trip(void)
{
	static const uint8_t regs[] = {
		L0_CONFIG, L0_MAPBASE, L0_TILEBASE, L0_HSCROLL_L, L0_VSCROLL_L
	};

	video_reset();

	for (unsigned layer = 0; layer < 2; layer++) {
		for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
			// A pattern with both nibbles populated and no byte repeated, so a
			// register wired to its neighbour shows up.
			const uint8_t v = (uint8_t)(0x9C + layer * 0x10 + i);

			write_layer(layer, regs[i], v);
			check_eq(read_layer(layer, regs[i]), v, "a layer register reports what was written");
		}
	}
}

// top.v:215,217
//     5'h11: rddata = {4'b0, l0_hscroll_r[11:8]};
//     5'h13: rddata = {4'b0, l0_vscroll_r[11:8]};
//
// top.v:392-400
//     if (do_write && access_addr == 5'h11) begin
//         l0_hscroll_next[11:8] = write_data[3:0];
//     end
//     ...
//     if (do_write && access_addr == 5'h13) begin
//         l0_vscroll_next[11:8] = write_data[3:0];
//     end
//
// The scroll registers are twelve bits, so the high byte of each holds four.
// The other four have nowhere to be stored and read back as zero.
static void
test_the_scroll_high_bytes_are_four_bits_wide(void)
{
	static const uint8_t highs[] = { L0_HSCROLL_H, L0_VSCROLL_H };

	video_reset();

	for (unsigned layer = 0; layer < 2; layer++) {
		for (unsigned i = 0; i < sizeof highs / sizeof highs[0]; i++) {
			write_layer(layer, highs[i], 0x0A);
			check_eq(read_layer(layer, highs[i]), 0x0A, "the low four bits are stored");

			write_layer(layer, highs[i], 0xFF);
			check_divergent(read_layer(layer, highs[i]) == 0x0F,
			                "the top four bits of a scroll high byte read as zero",
			                "video.c:2733 and video.c:2741 return the stored byte whole; "
			                "top.v:215 and top.v:217 read {4'b0, scroll[11:8]} and the write "
			                "at top.v:392-400 takes only write_data[3:0]");
		}
	}
}

// Each layer has its own copy of all seven. Written separately so a shared
// index or a wrong stride between the two banks shows up as a bleed.
static void
test_the_two_layers_are_independent(void)
{
	video_reset();

	write_layer(0, L0_CONFIG, 0x39);
	write_layer(1, L0_CONFIG, 0xC6);
	check_eq(read_layer(0, L0_CONFIG), 0x39, "layer 0 keeps its own config");
	check_eq(read_layer(1, L0_CONFIG), 0xC6, "layer 1 keeps its own");

	write_layer(0, L0_MAPBASE, 0x11);
	write_layer(1, L0_MAPBASE, 0x22);
	check_eq(read_layer(0, L0_MAPBASE), 0x11, "and its own map base");
	check_eq(read_layer(1, L0_MAPBASE), 0x22, "separately from the other");
}

// Put both layers on the same geometry and base, so the two ranges coincide
// and a boundary is unambiguous. video_is_tilemap_address() answers for either
// layer, so overlapping them deliberately is what makes the edges testable.
static void
set_both_layers(uint8_t config, uint8_t mapbase, uint8_t tilebase)
{
	for (unsigned layer = 0; layer < 2; layer++) {
		write_layer(layer, L0_CONFIG, config);
		write_layer(layer, L0_MAPBASE, mapbase);
		write_layer(layer, L0_TILEBASE, tilebase);
	}
}

// layer_renderer.v:107
//     wire [14:0] map_addr = {map_baseaddr, 7'b0} + map_idx[15:1];
//
// The register lands in the top eight bits of a 15-bit word address over
// 32-bit words, so the byte address it names is the register times 512.
static void
test_mapbase_places_the_tilemap_at_512_byte_units(void)
{
	video_reset();
	set_both_layers(0x00, 0x20, 0xFC);        // 32x32 map at 0x20 * 512

	check(!video_is_tilemap_address(0x3FFF), "below the base is not tilemap");
	check(video_is_tilemap_address(0x4000), "the base itself is");
	check(video_is_tilemap_address(0x47FF), "and so is the last byte of a 32x32 map");
	check(!video_is_tilemap_address(0x4800), "one past the end is not");

	// A different register value must move it by the same unit, or the shift is
	// right only at zero.
	set_both_layers(0x00, 0x21, 0xFC);
	check(video_is_tilemap_address(0x4200), "the next register value is 512 bytes further on");
	check(!video_is_tilemap_address(0x41FF), "and the byte below it is outside");
}

// layer_renderer.v:100-103
//     2'd0: map_idx = {3'b0, wrapped_vmap_idx, scrolled_htile_cnt[4:0]}; // 32
//     2'd1: map_idx = {2'b0, wrapped_vmap_idx, scrolled_htile_cnt[5:0]}; // 64
//     2'd2: map_idx = {1'b0, wrapped_vmap_idx, scrolled_htile_cnt[6:0]}; // 128
//     2'd3: map_idx = {      wrapped_vmap_idx, scrolled_htile_cnt[7:0]}; // 256
//
// layer_renderer.v:86-89 masks the row index to the map height the same way.
// Both dimensions are 32 << the field, and an entry is two bytes, so the map
// occupies 2 << (mapw_log2 + maph_log2) bytes.
static void
test_the_map_extent_follows_width_and_height(void)
{
	video_reset();

	for (unsigned mh = 0; mh < 4; mh++) {
		for (unsigned mw = 0; mw < 4; mw++) {
			const uint8_t config = (uint8_t)((mh << 6) | (mw << 4));
			const uint32_t bytes = 2u << ((5 + mw) + (5 + mh));

			set_both_layers(config, 0x00, 0xFC);

			check(video_is_tilemap_address((int)(bytes - 1)),
			      "the last byte of the map is inside it");

			// 256x256 fills the whole 128K, so there is no byte past the end to
			// look at and the check would be measuring the address space.
			if (bytes < 0x20000u) {
				check(!video_is_tilemap_address((int)bytes),
				      "and the byte after it is outside");
			}
		}
	}
}

// top.v:382-388
//     if (do_write && access_addr == 5'h0F) begin
//         l0_tile_baseaddr_next[7:2] = write_data[7:2];
//         l0_tile_baseaddr_next[1:0] = 0;
//
//         l0_tile_height_next = write_data[1];
//         l0_tile_width_next  = write_data[0];
//     end
//
// layer_renderer.v:188
//     wire [14:0] tile_addr = {tile_baseaddr, 7'b0} + tile_addr_xbpp;
//
// Bits 1 and 0 are the tile size, not address bits, and the base they leave
// behind is forced even to 2K.
static void
test_tilebase_ignores_the_two_size_bits(void)
{
	video_reset();
	set_both_layers(0x00, 0xF0, 0x00);
	check(video_is_tiledata_address(0x0000), "tile data starts at zero with base zero");

	// Both size bits set. The base must not move with them.
	set_both_layers(0x00, 0xF0, 0x03);
	check(video_is_tiledata_address(0x0000), "the tile size bits do not move the base");

	// The first value that does move it, and by 2K rather than 512 bytes,
	// because the two bits below it are not part of the field.
	set_both_layers(0x00, 0xF0, 0x04);
	check(!video_is_tiledata_address(0x07FF), "the first base step clears the 2K below it");
	check(video_is_tiledata_address(0x0800), "and lands 2K up");
}

int
main(void)
{
	test_the_full_width_layer_registers_round_trip();
	test_the_scroll_high_bytes_are_four_bits_wide();
	test_the_two_layers_are_independent();
	test_mapbase_places_the_tilemap_at_512_byte_units();
	test_the_map_extent_follows_width_and_height();
	test_tilebase_ignores_the_two_size_bits();
	return x16_test_summary("vera_layer");
}
