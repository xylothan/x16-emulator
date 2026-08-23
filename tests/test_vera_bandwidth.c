// Checks for VERA VRAM bandwidth accounting (src/vera_bandwidth.c).
//
// The module's whole job is to say what VERA's VRAM bus did in a frame, and
// almost all of that rests on one pure function turning a layer's mode into a
// fetch count. So the bulk of what follows is that function checked against the
// RTL's own decode tables, mode by mode -- because if those numbers are wrong
// every figure built on them is wrong too, and nothing else in the module would
// notice.
//
// The tables are quoted from X16Community/vera-module at tag v47.0.2, the pin
// tests/fetch_vera_rtl.py fetches. Run it and the citations below can be
// checked line by line.
//
// vera_bandwidth.c reaches for no emulator state, so this drives it directly:
// no CPU, no memory, no video, no SDL.

#include "vera_bandwidth.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

// Colour depth encodings, which are log2(bpp) throughout VERA.
#define BPP1 0
#define BPP2 1
#define BPP4 2
#define BPP8 3

static vera_bw_layer_t
tile_layer(uint8_t depth, uint8_t tile_width, bool scrolled)
{
	vera_bw_layer_t l;
	memset(&l, 0, sizeof l);
	l.enabled     = true;
	l.bitmap_mode = false;
	l.color_depth = depth;
	l.tile_width  = tile_width;
	l.scrolled    = scrolled;
	return l;
}

static vera_bw_layer_t
bitmap_layer(uint8_t depth, uint8_t tile_width)
{
	vera_bw_layer_t l;
	memset(&l, 0, sizeof l);
	l.enabled     = true;
	l.bitmap_mode = true;
	l.color_depth = depth;
	l.tile_width  = tile_width;
	return l;
}

static uint16_t
tile_fetches(uint8_t depth, uint8_t tile_width, bool scrolled)
{
	const vera_bw_layer_t l = tile_layer(depth, tile_width, scrolled);
	return vera_bandwidth_layer_line_fetches(&l);
}

static uint16_t
bitmap_fetches(uint8_t depth, uint8_t tile_width)
{
	const vera_bw_layer_t l = bitmap_layer(depth, tile_width);
	return vera_bandwidth_layer_line_fetches(&l);
}

static void
begin(void)
{
	vera_bandwidth_set_enabled(false);
	vera_bandwidth_configure(525);
	vera_bandwidth_set_enabled(true);
	vera_bandwidth_reset();
}

int
main(void)
{
	// ── The bus geometry these numbers all rest on ──────────────────────────
	{
		check_eq(VERA_BW_LINE_CLOCKS, 800,
		         "a scanline is 800 clocks: video_vga.v:27-31, 640+16+96+48");
		check_eq(VERA_BW_BYTES_PER_ACCESS, 4,
		         "VRAM is 32 bits wide, so an access is four bytes: main_ram.v:7-10");
		check_eq(VERA_BW_ACCESS_CLOCKS, 2,
		         "an access costs strobe plus ack: layer_renderer.v:250-271");
		check_eq(VERA_BW_LAYER_PIXELS, 640,
		         "the layer renderer always fills 640 pixels: layer_renderer.v:488");
	}

	// ── pixels_per_word, against layer_renderer.v:42-51 ─────────────────────
	{
		check_eq(vera_bandwidth_pixels_per_word(false, BPP1, 0), 8,
		         "1bpp 8px tile packs 8 pixels per word");
		check_eq(vera_bandwidth_pixels_per_word(false, BPP1, 1), 16,
		         "1bpp 16px tile packs 16");
		check_eq(vera_bandwidth_pixels_per_word(false, BPP2, 0), 8,
		         "2bpp 8px tile packs 8");
		check_eq(vera_bandwidth_pixels_per_word(false, BPP2, 1), 16,
		         "2bpp 16px tile packs 16");
		check_eq(vera_bandwidth_pixels_per_word(false, BPP4, 0), 8,
		         "4bpp tile packs 8 regardless of tile width");
		check_eq(vera_bandwidth_pixels_per_word(false, BPP4, 1), 8,
		         "4bpp 16px tile still packs 8");
		check_eq(vera_bandwidth_pixels_per_word(false, BPP8, 0), 4,
		         "8bpp tile packs 4");
		check_eq(vera_bandwidth_pixels_per_word(false, BPP8, 1), 4,
		         "8bpp 16px tile still packs 4");

		check_eq(vera_bandwidth_pixels_per_word(true, BPP1, 0), 32,
		         "1bpp bitmap packs 32 pixels per word -- a mode that only "
		         "exists from v47.0.2 (layer_renderer.v:48)");
		check_eq(vera_bandwidth_pixels_per_word(true, BPP2, 0), 16,
		         "2bpp bitmap packs 16");
		check_eq(vera_bandwidth_pixels_per_word(true, BPP4, 0), 8,
		         "4bpp bitmap packs 8");
		check_eq(vera_bandwidth_pixels_per_word(true, BPP8, 0), 4,
		         "8bpp bitmap packs 4");
	}

	// ── words_per_tile_line, against layer_renderer.v:68-77 ─────────────────
	{
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP1, 0), 1, "1bpp 8px tile row is one word");
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP1, 1), 1, "1bpp 16px tile row is one word");
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP2, 0), 1, "2bpp 8px tile row is one word");
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP2, 1), 1, "2bpp 16px tile row is one word");
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP4, 0), 1, "4bpp 8px tile row is one word");
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP4, 1), 2, "4bpp 16px tile row is two");
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP8, 0), 2, "8bpp 8px tile row is two");
		check_eq(vera_bandwidth_words_per_tile_line(false, BPP8, 1), 4, "8bpp 16px tile row is four");
	}

	// ── Tile-mode fetch counts per scanline ─────────────────────────────────
	//
	// 640 pixels / 8px tiles = 80 tiles, one map word per two tiles (40), plus
	// words_per_tile_line each. Unscrolled, so no extra tile.
	{
		check_eq(tile_fetches(BPP1, 0, false), 40 + 80,
		         "1bpp 8px tiles: 40 map + 80 data = 120 fetches a line");
		check_eq(tile_fetches(BPP2, 0, false), 40 + 80,
		         "2bpp 8px tiles cost the same as 1bpp: both are one word a tile");
		check_eq(tile_fetches(BPP4, 0, false), 40 + 80,
		         "4bpp 8px tiles too");
		check_eq(tile_fetches(BPP8, 0, false), 40 + 160,
		         "8bpp 8px tiles need two words a tile: 200");

		check_eq(tile_fetches(BPP1, 1, false), 20 + 40,
		         "1bpp 16px tiles halve the tile count: 60");
		check_eq(tile_fetches(BPP2, 1, false), 20 + 40, "2bpp 16px tiles: 60");
		check_eq(tile_fetches(BPP4, 1, false), 20 + 80,
		         "4bpp 16px tiles need two words a tile: 100");
		check_eq(tile_fetches(BPP8, 1, false), 20 + 160,
		         "8bpp 16px tiles need four: 180");
	}

	// ── The scroll tile ─────────────────────────────────────────────────────
	//
	// lb_wridx_start is 0 - subtile_hscroll (layer_renderer.v:499-501), so a
	// layer scrolled off a tile boundary renders one tile more than the screen
	// needs. It is the single most common configuration in a scrolling game, so
	// it is worth being explicit that it is not free.
	{
		check_eq(tile_fetches(BPP8, 0, true), 41 + 162,
		         "scrolling 8bpp 8px tiles off a boundary costs one more tile: 203");
		check_eq(tile_fetches(BPP8, 0, true) - tile_fetches(BPP8, 0, false), 3,
		         "which is three extra fetches: one map word and two data words");
		check_eq(tile_fetches(BPP1, 0, true), 41 + 81,
		         "and at 1bpp it is 122");
		check_eq(tile_fetches(BPP1, 0, true) - tile_fetches(BPP1, 0, false), 2,
		         "two extra fetches, because a 1bpp tile row is a single word");
	}

	// ── Bitmap fetch counts, and the surprise in them ───────────────────────
	{
		check_eq(bitmap_fetches(BPP1, 0), 20, "1bpp bitmap: 640/32 = 20 fetches");
		check_eq(bitmap_fetches(BPP2, 0), 40, "2bpp bitmap: 640/16 = 40");
		check_eq(bitmap_fetches(BPP4, 0), 80, "4bpp bitmap: 640/8 = 80");
		check_eq(bitmap_fetches(BPP8, 0), 160, "8bpp bitmap: 640/4 = 160");

		// The one people get wrong. layer_renderer has no scale input at all
		// (its port list, :3-33) -- DC_HSCALE is the composer's business when it
		// reads the line buffer. The renderer just fills to line_done at 640
		// (:488), so a 320-wide bitmap fetches exactly as much as a 640-wide
		// one and runs off the end of its own line doing it.
		check_eq(bitmap_fetches(BPP8, 1), bitmap_fetches(BPP8, 0),
		         "a 320-wide bitmap costs the same bus as a 640-wide one: "
		         "halving the width buys VRAM, not bandwidth");
		check_eq(bitmap_fetches(BPP4, 1), bitmap_fetches(BPP4, 0),
		         "and the same at 4bpp");
		check_eq(bitmap_fetches(BPP1, 1), bitmap_fetches(BPP1, 0),
		         "and at 1bpp");
	}

	// ── A disabled or absent layer costs nothing ────────────────────────────
	{
		vera_bw_layer_t off = tile_layer(BPP8, 0, true);
		off.enabled         = false;
		check_eq(vera_bandwidth_layer_line_fetches(&off), 0,
		         "a layer switched off in DC_VIDEO fetches nothing");
		check_eq(vera_bandwidth_layer_line_fetches(NULL), 0,
		         "and a null layer is not a crash");
	}

	// ── The worst case really does fill the scanline ────────────────────────
	//
	// This is the number that justifies reporting per-scanline pressure at all:
	// two 8bpp bitmap layers plus a full sprite line come to exactly H_TOTAL.
	{
		const uint32_t two_layers = 2u * bitmap_fetches(BPP8, 1) * VERA_BW_ACCESS_CLOCKS;
		check_eq(two_layers, 640,
		         "two 8bpp bitmap layers take 640 of the 800 clocks");
		check_eq(two_layers + 160, VERA_BW_LINE_CLOCKS,
		         "and 160 clocks of 4bpp sprites fills the scanline exactly");
	}

	// ── Accumulation over a frame ───────────────────────────────────────────
	{
		begin();
		const vera_bw_layer_t l0 = bitmap_layer(BPP8, 1); // 160 fetches a line
		const vera_bw_layer_t l1 = tile_layer(BPP1, 0, false); // 120 a line

		for (uint16_t y = 0; y < 480; y++)
			vera_bandwidth_line(y, &l0, &l1);
		vera_bandwidth_frame_end(1);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "a finished frame is published");
		check_eq(f.frame, 1, "carrying its ordinal");
		check_eq(f.lines_active, 480, "with every active line accounted");
		check_eq(f.layer_fetches[0], 160u * 480u, "layer 0 fetched 160 a line");
		check_eq(f.layer_fetches[1], 120u * 480u, "layer 1 fetched 120 a line");
		check_eq(f.layer_bytes[0], 160u * 480u * 4u,
		         "and bytes are just fetches times four");
		check_eq(f.peak_clocks, (160u + 120u) * VERA_BW_ACCESS_CLOCKS,
		         "the busiest line wanted 560 of its 800 clocks");
		check_eq(f.mean_clocks, 560, "and every line was the same, so the mean matches");
		check_eq(f.p95_clocks, 560, "as does the 95th percentile");
		check_eq(f.lines_over, 0, "nothing exceeded the scanline window");
		check_eq(f.layer_peak_fetches[0], 160, "layer 0's peak line is a full one");
	}

	// ── A re-rendered line records the last pass, not the sum ───────────────
	//
	// video.c re-renders a scanline when a register changes mid-line, and the
	// last pass is what stays on screen. Accumulating would report a line that
	// used twice the bandwidth it really did.
	{
		begin();
		const vera_bw_layer_t heavy = bitmap_layer(BPP8, 1);   // 160
		const vera_bw_layer_t light = bitmap_layer(BPP1, 0);   // 20
		vera_bw_layer_t       none  = bitmap_layer(BPP1, 0);
		none.enabled                = false;

		vera_bandwidth_line(100, &heavy, &none);
		vera_bandwidth_line(100, &light, &none); // the split re-renders it
		vera_bandwidth_frame_end(2);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "the frame published");
		check_eq(f.layer_fetches[0], 20,
		         "a re-rendered line keeps the last pass, not the sum of passes");
		check_eq(f.lines_active, 1, "and is still one line");
	}

	// ── The data port, which is the part a program controls ─────────────────
	{
		begin();
		for (int i = 0; i < 100; i++)
			vera_bandwidth_note_port(true, 1, 50);
		for (int i = 0; i < 30; i++)
			vera_bandwidth_note_port(false, 1, 50);
		vera_bandwidth_frame_end(3);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "the frame published");
		check_eq(f.port_writes, 100, "100 bytes written through $9F23");
		check_eq(f.port_reads, 30, "and 30 read back");
		check_eq(f.port_accesses, 130, "each byte costs one VRAM access");
		check_eq(f.port_vram_bytes, 130, "and an ordinary write moves one byte");
		// The port lands on a line even though no layer rendered it, because
		// the CPU outranks both layers on the bus (vram_if.v:142-157).
		check_eq(f.peak_line, 50, "port traffic marks the line it landed on");
		check_eq(f.lines_active, 1,
		         "and makes that line active even with both layers off -- a "
		         "program blasting VRAM through $9F23 is using the bus, "
		         "whatever the display is doing");
		check_eq(f.peak_clocks, 130u * VERA_BW_ACCESS_CLOCKS,
		         "costing two clocks an access like any other client");
	}

	// ── VERA FX, where one store is not one byte ────────────────────────────
	{
		begin();
		// A cache write is one store that puts four bytes into VRAM
		// (video.c fx_vram_cache_write x4).
		for (int i = 0; i < 10; i++) {
			vera_bandwidth_note_port(true, 4, 10);
			vera_bandwidth_note_fx_cache_write(10);
		}
		vera_bandwidth_frame_end(4);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "the frame published");
		check_eq(f.port_writes, 10, "ten CPU stores");
		check_eq(f.fx_cache_writes, 10, "all of them FX cache writes");
		check_eq(f.port_vram_bytes, 40,
		         "which moved forty bytes: FX multiplies a store fourfold");
	}

	// ── Affine mode costs a second fetch ────────────────────────────────────
	{
		begin();
		vera_bandwidth_note_port(false, 1, 20);
		vera_bandwidth_note_fx_affine(20);
		vera_bandwidth_frame_end(5);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "the frame published");
		check_eq(f.fx_affine_fetches, 1, "one affine read");
		check_eq(f.port_accesses, 2,
		         "costing two accesses: the map entry and then the tile");
	}

	// ── Lines over the window are counted, not clamped away ─────────────────
	{
		begin();
		const vera_bw_layer_t l0 = bitmap_layer(BPP8, 1); // 160 -> 320 clocks
		const vera_bw_layer_t l1 = bitmap_layer(BPP8, 1); // 160 -> 320 clocks
		vera_bandwidth_line(7, &l0, &l1);
		for (int i = 0; i < 100; i++) // 100 more accesses -> 200 more clocks
			vera_bandwidth_note_port(true, 1, 7);
		vera_bandwidth_frame_end(6);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "the frame published");
		check_eq(f.peak_clocks, 840, "layers and CPU together wanted 840 clocks");
		check(f.peak_clocks > VERA_BW_LINE_CLOCKS,
		      "which is more than the scanline has");
		check_eq(f.lines_over, 1,
		         "and that is reported rather than clamped, because it is the "
		         "whole point: sprites are last on the bus and get what is left");
	}

	// ── Percentiles ─────────────────────────────────────────────────────────
	{
		begin();
		vera_bw_layer_t none = bitmap_layer(BPP1, 0);
		none.enabled         = false;
		const vera_bw_layer_t light = bitmap_layer(BPP1, 0); // 20 -> 40 clocks
		const vera_bw_layer_t heavy = bitmap_layer(BPP8, 1); // 160 -> 320 clocks

		// 99 quiet lines and one loud one: the peak must show it and the mean
		// must not drown it.
		for (uint16_t y = 0; y < 99; y++)
			vera_bandwidth_line(y, &light, &none);
		vera_bandwidth_line(99, &heavy, &none);
		vera_bandwidth_frame_end(7);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "the frame published");
		check_eq(f.lines_active, 100, "a hundred lines rendered");
		check_eq(f.peak_clocks, 320, "the loud line is the peak");
		check_eq(f.peak_line, 99, "and is identified");
		check_eq(f.p95_clocks, 40,
		         "the 95th percentile ignores a single outlier, which is why "
		         "the peak is reported next to it and not instead of it");
		check(f.mean_clocks < 50, "and the mean stays near the quiet lines");
	}

	// ── Per-line detail is available for drawing ────────────────────────────
	{
		begin();
		const vera_bw_layer_t l0 = bitmap_layer(BPP4, 0); // 80
		vera_bw_layer_t       none = bitmap_layer(BPP1, 0);
		none.enabled               = false;
		vera_bandwidth_line(3, &l0, &none);
		vera_bandwidth_note_port(true, 1, 3);
		vera_bandwidth_frame_end(8);

		uint16_t              n     = 0;
		const vera_bw_line_t *lines = vera_bandwidth_last_lines(&n);
		check(lines != NULL, "per-line detail is published for the panel");
		check_eq(n, VERA_BW_LINES, "covering every active scanline");
		check(lines[3].rendered, "the line we drew is marked rendered");
		check_eq(lines[3].layer_fetches[0], 80, "with its fetch count");
		check_eq(lines[3].port_accesses, 1, "and the port access on it");
		check_eq(vera_bw_line_clocks(&lines[3]), 81u * VERA_BW_ACCESS_CLOCKS,
		         "line clocks are derived from both, so they cannot drift apart");
		check(!lines[4].rendered, "and one we did not is not");
	}

	// ── Layer and port traffic on one line must land on ONE line ────────────
	//
	// They are recorded from different places -- the renderer and the CPU's
	// store -- and if those two ever indexed the array differently the peak
	// figure would be built from halves of two different scanlines. Pinned
	// because that is exactly what went wrong once: the layer side was charged
	// to the scaled source row rather than the display line, so in a 320x240
	// mode it landed on line y/2 while the port traffic landed on y.
	{
		begin();
		const vera_bw_layer_t l0 = bitmap_layer(BPP4, 0); // 80 fetches
		vera_bw_layer_t       none = bitmap_layer(BPP1, 0);
		none.enabled               = false;

		vera_bandwidth_line(200, &l0, &none);
		for (int i = 0; i < 20; i++)
			vera_bandwidth_note_port(true, 1, 200);
		vera_bandwidth_frame_end(11);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "the frame published");
		check_eq(f.lines_active, 1,
		         "layer and port traffic on one scanline is one active line");
		check_eq(f.peak_line, 200, "and the peak is that line");
		check_eq(f.peak_clocks, (80u + 20u) * VERA_BW_ACCESS_CLOCKS,
		         "with both contributions summed into it");
	}

	// ── Off means off ───────────────────────────────────────────────────────
	{
		vera_bandwidth_set_enabled(false);
		check(!vera_bandwidth_is_enabled(), "accounting is off");

		const vera_bw_layer_t l0 = bitmap_layer(BPP8, 1);
		vera_bandwidth_line(0, &l0, &l0);
		vera_bandwidth_note_port(true, 1, 0);
		vera_bandwidth_frame_end(9);

		vera_bw_frame_t f;
		check(!vera_bandwidth_last_frame(&f),
		      "and nothing is recorded while it is");
	}

	// ── Ownership, so two consumers cannot switch each other off ────────────
	{
		vera_bandwidth_set_enabled(false);
		vera_bandwidth_arm(VERA_BW_OWNER_UI, true);
		check(vera_bandwidth_is_enabled(), "the panel can switch accounting on");

		vera_bandwidth_arm(VERA_BW_OWNER_DAP, true);
		check(vera_bandwidth_owner_wants(VERA_BW_OWNER_DAP), "so can a DAP client");

		vera_bandwidth_arm(VERA_BW_OWNER_UI, false);
		check(vera_bandwidth_is_enabled(),
		      "closing the panel does not strand the client");
		check(!vera_bandwidth_owner_wants(VERA_BW_OWNER_UI),
		      "though the panel no longer wants it");

		vera_bandwidth_arm(VERA_BW_OWNER_DAP, false);
		check(!vera_bandwidth_is_enabled(),
		      "and it stops once nobody is asking");
	}

	// ── Configuration ───────────────────────────────────────────────────────
	{
		vera_bandwidth_configure(525);
		check_eq(vera_bandwidth_scanlines_per_frame(), 525, "VGA scans 525 lines");
		vera_bandwidth_configure(0);
		check_eq(vera_bandwidth_scanlines_per_frame(), 525,
		         "and a nonsense value is ignored rather than believed");
	}

	// ── Reset ───────────────────────────────────────────────────────────────
	{
		begin();
		const vera_bw_layer_t l0 = bitmap_layer(BPP8, 1);
		vera_bandwidth_line(0, &l0, &l0);
		vera_bandwidth_frame_end(10);

		vera_bw_frame_t f;
		check(vera_bandwidth_last_frame(&f), "a frame is there to forget");
		vera_bandwidth_reset();
		check(!vera_bandwidth_last_frame(&f), "reset forgets it");
		check_eq(vera_bandwidth_scanlines_per_frame(), 525,
		         "while leaving the machine configuration alone");

		vera_bandwidth_set_enabled(false);
	}

	return x16_test_summary("vera_bandwidth");
}
