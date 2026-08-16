// The row of a layer that each display line shows.
//
// tests/test_vera_layer.c pins the layer registers and the base addresses they
// name. This pins the other half of the vertical path: given a display line
// and VSCROLL, which row of the tilemap is fetched and which line inside the
// tile is drawn.
//
// The emulator carries the answer as one number, the layer row, recorded per
// scanline for the debug views (video.c:1326-1328). The RTL carries it as two:
// a wrapped tile row and the line within the tile. This file derives the RTL's
// two from the emulator's one and compares them, over every display line
// rather than a chosen few, so an error that only shows up at a wrap boundary
// has nowhere to hide.
//
// Horizontal is still not covered. The RTL adds a pre-shifted HSCROLL to a
// per-line tile counter (layer_renderer.v:95) while video.c shifts after
// adding (video.c:574, 587); those differ on carry when HSCROLL is not tile
// aligned, and settling it needs rendered pixels, which means the framebuffer
// seam. Nothing here claims to reach it.
//
// ORACLE: B -- X16Community/vera-module Verilog, tag v47.0.2
// (45cc1f053376dae12173ea63612820e4d289c0da), modelled directly below from the
// quoted lines. layer_renderer.v is identical at v48.0.1 and at the tip.

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <stdio.h>
#include <stdint.h>

#define VERA_CTRL    0x05
#define VERA_DC_VIDEO 0x09

#define L0_CONFIG    0x0D
#define L0_VSCROLL_L 0x12
#define L0_VSCROLL_H 0x13
#define L0_TILEBASE  0x0F

#define DISPLAY_LINES 480

static void
step_frame(void)
{
	for (unsigned i = 0; i < 525; i++) {
		video_step(25.0f, 801.0f, false);
	}
}

// layer_renderer.v:81-82
//     wire [11:0] scrolled_line_idx = {3'b000, line_idx} + vscroll;
//     wire [7:0] vmap_idx = tile_height ? {scrolled_line_idx[11:4]} : scrolled_line_idx[10:3];
//
// layer_renderer.v:84-90
//     reg [7:0] wrapped_vmap_idx;
//     always @* case (map_height)
//         2'd0: wrapped_vmap_idx = {3'b0, vmap_idx[4:0]}; // 32
//         2'd1: wrapped_vmap_idx = {2'b0, vmap_idx[5:0]}; // 64
//         2'd2: wrapped_vmap_idx = {1'b0, vmap_idx[6:0]}; // 128
//         2'd3: wrapped_vmap_idx = {      vmap_idx[7:0]}; // 256
//     endcase
//
// The sum is twelve bits, so it wraps at 4096 pixel rows before anything else
// happens to it.
static unsigned
rtl_wrapped_vmap_idx(unsigned line_idx, unsigned vscroll, unsigned tile_height, unsigned map_height)
{
	const unsigned scrolled = (line_idx + vscroll) & 0xFFF;
	const unsigned vmap     = (tile_height ? (scrolled >> 4) : (scrolled >> 3)) & 0xFF;

	return vmap & ((1u << (5 + map_height)) - 1);
}

// The line within the tile is the low bits of the same twelve-bit sum: three
// for an 8-pixel tile, four for a 16-pixel one.
static unsigned
rtl_line_in_tile(unsigned line_idx, unsigned vscroll, unsigned tile_height)
{
	const unsigned scrolled = (line_idx + vscroll) & 0xFFF;

	return scrolled & (tile_height ? 0xFu : 0x7u);
}

// map_height and tile_height as the register fields, not as pixel counts.
static void
configure_layer0(unsigned map_height, unsigned tile_height, unsigned vscroll)
{
	video_write(VERA_CTRL, 0x00);
	video_write(VERA_DC_VIDEO, 0x31);  // VGA output, both layers on

	// 4bpp tile mode, so the layer is neither text nor bitmap and takes its
	// scroll from the registers below.
	video_write(L0_CONFIG, (uint8_t)((map_height << 6) | (2 << 4) | 0x02));
	video_write(L0_TILEBASE, (uint8_t)(0xFC | (tile_height << 1)));
	video_write(L0_VSCROLL_L, (uint8_t)(vscroll & 0xFF));
	video_write(L0_VSCROLL_H, (uint8_t)((vscroll >> 8) & 0x0F));
}

// The renderer takes the layer row from one register generation and the layout
// from another, so a change needs a couple of lines to settle. Two frames puts
// every line of the second one past that.
static void
settle(void)
{
	step_frame();
	step_frame();
}

static void
check_every_line_against_the_rtl(unsigned map_height, unsigned tile_height, unsigned vscroll)
{
	char what[128];
	char first[192];

	configure_layer0(map_height, tile_height, vscroll);
	settle();

	first[0] = '\0';
	unsigned mismatches = 0;
	unsigned examined   = 0;

	for (uint16_t line = 0; line < DISPLAY_LINES; line++) {
		uint16_t row    = 0;
		uint16_t eff_y  = 0;

		if (!video_get_layer_line_state(0, line, NULL, &eff_y, NULL, &row)) {
			continue;
		}
		examined++;

		const unsigned want_tile_row = rtl_wrapped_vmap_idx(eff_y, vscroll, tile_height, map_height);
		const unsigned want_in_tile  = rtl_line_in_tile(eff_y, vscroll, tile_height);

		// Deliberately not masked to the map height. video.c:587 shifts the row
		// down and puts it straight into the map index, so whatever wrapping
		// the map height calls for has to be in the row already. Masking here
		// would supply it and hide its absence.
		const unsigned got_tile_row = row >> (3 + tile_height);
		const unsigned got_in_tile  = row & ((1u << (3 + tile_height)) - 1);

		if (got_tile_row != want_tile_row || got_in_tile != want_in_tile) {
			mismatches++;
			if (first[0] == '\0') {
				snprintf(first, sizeof first,
				         "line %u (eff_y %u): tile row %u want %u, line in tile %u want %u",
				         line, eff_y, got_tile_row, want_tile_row, got_in_tile, want_in_tile);
			}
		}
	}

	if (first[0] != '\0') {
		printf("      %s\n", first);
	}

	// Without this the sweep would pass by examining nothing: an accessor that
	// reported every line as unrendered leaves no mismatches to count.
	snprintf(what, sizeof what,
	         "all %u display lines are recorded (map_height %u, tile_height %u, vscroll %u)",
	         DISPLAY_LINES, map_height, tile_height, vscroll);
	check_eq(examined, DISPLAY_LINES, what);

	snprintf(what, sizeof what,
	         "every row matches the RTL (map_height %u, tile_height %u, vscroll %u)",
	         map_height, tile_height, vscroll);
	check_eq(mismatches, 0, what);
}

// The composer's effective layer Y, which the sweep above feeds to the RTL
// model as line_idx. At unit vertical scale over a full-height window it is the
// display line itself; asserting it makes the sweep's input a known quantity
// rather than whatever the composer happened to produce.
static void
test_the_layer_y_is_the_display_line_at_unit_scale(void)
{
	video_reset();
	configure_layer0(0, 0, 0);
	settle();

	unsigned wrong = 0;
	for (uint16_t line = 0; line < DISPLAY_LINES; line++) {
		uint16_t eff_y = 0xFFFF;
		if (!video_get_layer_line_state(0, line, NULL, &eff_y, NULL, NULL)) {
			wrong++;
			continue;
		}
		if (eff_y != line) {
			wrong++;
		}
	}
	check_eq(wrong, 0, "the layer Y follows the display line one for one");
}

// Every map height against every tile height, with no scroll: the wrap point
// is then the map height alone.
static void
test_the_row_wraps_at_the_map_height(void)
{
	for (unsigned map_height = 0; map_height < 4; map_height++) {
		for (unsigned tile_height = 0; tile_height < 2; tile_height++) {
			video_reset();
			check_every_line_against_the_rtl(map_height, tile_height, 0);
		}
	}
}

// VSCROLL moves the start of the sweep. 4095 is the largest the twelve-bit
// register holds and puts the sum over the wrap for most of the frame; 300 and
// 7 land it mid-tile, where an off-by-one between the tile row and the line
// inside the tile shows up.
static void
test_vscroll_shifts_the_row(void)
{
	static const unsigned scrolls[] = { 1, 7, 300, 4095 };

	for (unsigned i = 0; i < sizeof scrolls / sizeof scrolls[0]; i++) {
		video_reset();
		check_every_line_against_the_rtl(0, 0, scrolls[i]);

		video_reset();
		check_every_line_against_the_rtl(3, 1, scrolls[i]);
	}
}

int
main(void)
{
	test_the_layer_y_is_the_display_line_at_unit_scale();
	test_the_row_wraps_at_the_map_height();
	test_vscroll_shifts_the_row();
	return x16_test_summary("vera_layer_rows");
}
