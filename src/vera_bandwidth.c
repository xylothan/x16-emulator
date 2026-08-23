// Commander X16 Emulator — VERA VRAM bandwidth accounting.
//
// See vera_bandwidth.h for the model, the RTL it is quoted from, and -- most
// importantly -- the list of places it is knowingly wrong.
//
// No emulator globals, and no <math.h>: see the header for why both matter.

#include "vera_bandwidth.h"

#include <stdlib.h>
#include <string.h>

bool vera_bandwidth_active = false;

// Ownership, mirroring perf_budget.c.
static bool owners[VERA_BW_OWNER_COUNT];

static uint16_t scanlines_per_frame = 525;

// Two buffers: the frame being drawn, and the last one finished. Small enough
// (about 6 KB each) to be static, unlike sprite_trace's residency maps -- there
// is nothing to allocate and so nothing that can fail to allocate.
static vera_bw_line_t cur_lines[VERA_BW_LINES];
static vera_bw_line_t pub_lines[VERA_BW_LINES];

static vera_bw_frame_t cur_frame;
static vera_bw_frame_t pub_frame;
static bool            have_frame = false;

// ─── The model ──────────────────────────────────────────────────────────────

// layer_renderer.v:42-51. color_depth 0..3 means 1, 2, 4 and 8 bpp, and
// mode = {bitmap_mode, color_depth} (layer_renderer.v:39).
uint8_t
vera_bandwidth_pixels_per_word(bool bitmap_mode, uint8_t color_depth, uint8_t tile_width)
{
	const uint8_t mode = (uint8_t)((bitmap_mode ? 4 : 0) | (color_depth & 3));
	switch (mode) {
		case 0: return tile_width ? 16 : 8; // 1bpp tile
		case 1: return tile_width ? 16 : 8; // 2bpp tile
		case 2: return 8;                   // 4bpp tile
		case 3: return 4;                   // 8bpp tile
		case 4: return 32;                  // 1bpp bitmap
		case 5: return 16;                  // 2bpp bitmap
		case 6: return 8;                   // 4bpp bitmap
		default: return 4;                  // 8bpp bitmap
	}
}

// layer_renderer.v:68-77. Words of pixel data one tile row costs. Bitmap modes
// report 1 because they step a word at a time with no tile structure at all.
uint8_t
vera_bandwidth_words_per_tile_line(bool bitmap_mode, uint8_t color_depth, uint8_t tile_width)
{
	const uint8_t mode = (uint8_t)((bitmap_mode ? 4 : 0) | (color_depth & 3));
	switch (mode) {
		case 0: return 1;                  // 1bpp tile
		case 1: return 1;                  // 2bpp tile
		case 2: return tile_width ? 2 : 1; // 4bpp tile
		case 3: return tile_width ? 4 : 2; // 8bpp tile
		default: return 1;                 // every bitmap mode
	}
}

// Bus clocks a sprite's fetches occupy. See the header for why this conversion
// has to exist: sprite_trace measures render time, this module measures bus
// time, and the two are not the same clock.
//
// One 32-bit word per 8 pixels at 4 bpp, per 4 at 8 bpp -- the emulator's own
// sprite loop uses the same divisor as vram_fetch_mask in render_sprite_line().
// Rounded up, because a sprite whose width is not a whole number of words still
// fetches the last partial one.
uint16_t
vera_bandwidth_sprite_bus_clocks(uint16_t sprite_width, uint8_t color_mode)
{
	const uint16_t per_word = (uint16_t)(color_mode ? 4 : 8);
	const uint16_t words    = (uint16_t)((sprite_width + per_word - 1) / per_word);
	return (uint16_t)(words * VERA_BW_ACCESS_CLOCKS);
}

uint16_t
vera_bandwidth_layer_line_fetches(const vera_bw_layer_t *layer)
{
	if (layer == NULL || !layer->enabled)
		return 0;

	const uint8_t depth = (uint8_t)(layer->color_depth & 3);
	const uint8_t tw    = layer->tile_width ? 1 : 0;

	if (layer->bitmap_mode) {
		// No map, and -- the part that surprises people -- no dependence on the
		// 320/640 bitmap width either. layer_renderer.v has NO scale input (see
		// its port list at :3-33); DC_HSCALE is applied by the composer when it
		// reads the line buffer. The renderer just fills the buffer until
		// line_done at 640 pixels (layer_renderer.v:488), so a 320-wide bitmap
		// fetches exactly as many words as a 640-wide one and simply runs off
		// the end of its own line. Halving the width buys VRAM, not bus time.
		const uint8_t ppw = vera_bandwidth_pixels_per_word(true, depth, tw);
		return (uint16_t)(VERA_BW_LAYER_PIXELS / ppw);
	}

	// Tile modes. The renderer starts at lb_wridx_start = 0 - subtile_hscroll
	// (layer_renderer.v:499-501) and stops at 640, so a layer scrolled off a
	// tile boundary renders up to one tile more than the screen strictly needs.
	const uint8_t  tile_px = (uint8_t)(tw ? 16 : 8);
	const uint16_t tiles   = (uint16_t)((VERA_BW_LAYER_PIXELS / tile_px) + (layer->scrolled ? 1 : 0));

	// A 32-bit word holds two 16-bit map entries (layer_renderer.v:113), and
	// the state machine only returns to FETCH_MAP on every other tile
	// (layer_renderer.v:305). Which tile is "every other" depends on the
	// scrolled tile counter's parity, so this can be one out at the margin --
	// limit 3 in the header.
	const uint16_t map_fetches  = (uint16_t)((tiles + 1) / 2);
	const uint16_t data_fetches = (uint16_t)(tiles * vera_bandwidth_words_per_tile_line(false, depth, tw));

	return (uint16_t)(map_fetches + data_fetches);
}

// ─── Recording ──────────────────────────────────────────────────────────────

void
vera_bandwidth_line_(uint16_t line, const vera_bw_layer_t *l0, const vera_bw_layer_t *l1)
{
	if (line >= VERA_BW_LINES)
		return;

	vera_bw_line_t *s = &cur_lines[line];

	// REPLACE, never accumulate. A mid-line register split makes video.c render
	// the same scanline more than once and the last pass is what stays on
	// screen, so the last pass is what this records -- the same rule
	// sprite_trace_line_begin() follows. Port accesses are left alone: those
	// are CPU events that really happened, not a rendering pass that got
	// superseded.
	s->layer_fetches[0] = vera_bandwidth_layer_line_fetches(l0);
	s->layer_fetches[1] = vera_bandwidth_layer_line_fetches(l1);
	s->rendered         = true;
}

void
vera_bandwidth_note_port_(bool write, uint8_t vram_bytes, uint16_t line)
{
	if (write)
		cur_frame.port_writes++;
	else
		cur_frame.port_reads++;

	cur_frame.port_accesses++;
	cur_frame.port_vram_bytes += vram_bytes;

	if (line < VERA_BW_LINES)
		cur_lines[line].port_accesses++;
}

void
vera_bandwidth_note_fx_cache_write_(uint16_t line)
{
	(void)line;
	cur_frame.fx_cache_writes++;
}

void
vera_bandwidth_note_fx_affine_(uint16_t line)
{
	// The affine prefetch reads the tile map and then the tile, so it costs one
	// access beyond an ordinary port read (video.c fx_affine_prefetch()).
	cur_frame.fx_affine_fetches++;
	cur_frame.port_accesses++;
	if (line < VERA_BW_LINES)
		cur_lines[line].port_accesses++;
}

// ─── Frame boundary ─────────────────────────────────────────────────────────

static int
cmp_u16(const void *a, const void *b)
{
	const uint16_t x = *(const uint16_t *)a;
	const uint16_t y = *(const uint16_t *)b;
	return (x > y) - (x < y);
}

// Derive every frame-level figure from the line array in one pass, rather than
// keeping running totals. A line can be rendered more than once, and running
// totals would count those lines twice; a single pass over the finished array
// cannot. sprite_trace_frame_end() derives its per-generation totals the same
// way, for the same reason.
static void
summarise(vera_bw_frame_t *f, const vera_bw_line_t *lines)
{
	static uint16_t sorted[VERA_BW_LINES];
	uint16_t        n = 0;

	f->layer_fetches[0] = f->layer_fetches[1] = 0;
	f->layer_peak_line[0] = f->layer_peak_line[1] = 0;
	f->layer_peak_fetches[0] = f->layer_peak_fetches[1] = 0;
	f->peak_line = f->peak_clocks = 0;
	f->total_clocks = f->total_fetches = 0;
	f->lines_over = 0;

	for (uint16_t i = 0; i < VERA_BW_LINES; i++) {
		const vera_bw_line_t *s = &lines[i];
		// A line counts if anything touched the bus on it. Testing `rendered`
		// alone would drop CPU port traffic on a line no layer drew -- a
		// program that switches the layers off and pushes VRAM through $9F23
		// would then report as using no bandwidth at all.
		if (!s->rendered && s->port_accesses == 0)
			continue;

		const uint32_t fetches = (uint32_t)s->layer_fetches[0] + s->layer_fetches[1] + s->port_accesses;
		const uint32_t clocks  = vera_bw_line_clocks(s);

		for (int l = 0; l < 2; l++) {
			f->layer_fetches[l] += s->layer_fetches[l];
			if (s->layer_fetches[l] > f->layer_peak_fetches[l]) {
				f->layer_peak_fetches[l] = s->layer_fetches[l];
				f->layer_peak_line[l]    = i;
			}
		}

		f->total_fetches += fetches;
		f->total_clocks  += clocks;

		if (clocks > f->peak_clocks) {
			f->peak_clocks = (uint16_t)(clocks > 0xFFFF ? 0xFFFF : clocks);
			f->peak_line   = i;
		}
		if (clocks > VERA_BW_LINE_CLOCKS)
			f->lines_over++;

		sorted[n++] = (uint16_t)(clocks > 0xFFFF ? 0xFFFF : clocks);
	}

	f->lines_active   = n;
	f->layer_bytes[0] = f->layer_fetches[0] * VERA_BW_BYTES_PER_ACCESS;
	f->layer_bytes[1] = f->layer_fetches[1] * VERA_BW_BYTES_PER_ACCESS;

	if (n == 0) {
		f->p95_clocks = f->mean_clocks = 0;
		return;
	}

	qsort(sorted, n, sizeof sorted[0], cmp_u16);

	// Integer percentile: no <math.h>, and rounding a rank does not need it.
	// Index (95*(n-1) + 99)/100 is the ceiling of the 95th-percentile rank.
	f->p95_clocks  = sorted[(95u * (n - 1u) + 99u) / 100u];
	f->mean_clocks = (uint16_t)(f->total_clocks / n);
}

void
vera_bandwidth_frame_end(uint32_t frame)
{
	if (!vera_bandwidth_active)
		return;

	cur_frame.frame = frame;
	summarise(&cur_frame, cur_lines);

	memcpy(&pub_frame, &cur_frame, sizeof pub_frame);
	memcpy(pub_lines, cur_lines, sizeof pub_lines);
	have_frame = true;

	memset(&cur_frame, 0, sizeof cur_frame);
	memset(cur_lines, 0, sizeof cur_lines);
}

// ─── Configuration ──────────────────────────────────────────────────────────

void
vera_bandwidth_set_enabled(bool enabled)
{
	if (enabled == vera_bandwidth_active)
		return;
	vera_bandwidth_active = enabled;
	vera_bandwidth_reset();
}

bool
vera_bandwidth_is_enabled(void)
{
	return vera_bandwidth_active;
}

void
vera_bandwidth_arm(vera_bw_owner_t owner, bool want)
{
	if ((unsigned)owner >= VERA_BW_OWNER_COUNT)
		return;
	owners[owner] = want;

	bool any = false;
	for (int i = 0; i < VERA_BW_OWNER_COUNT; i++)
		any = any || owners[i];

	vera_bandwidth_set_enabled(any);
}

bool
vera_bandwidth_owner_wants(vera_bw_owner_t owner)
{
	return (unsigned)owner < VERA_BW_OWNER_COUNT && owners[owner];
}

void
vera_bandwidth_reset(void)
{
	memset(&cur_frame, 0, sizeof cur_frame);
	memset(&pub_frame, 0, sizeof pub_frame);
	memset(cur_lines, 0, sizeof cur_lines);
	memset(pub_lines, 0, sizeof pub_lines);
	have_frame = false;
}

void
vera_bandwidth_configure(uint16_t lines)
{
	if (lines > 0)
		scanlines_per_frame = lines;
}

uint16_t
vera_bandwidth_scanlines_per_frame(void)
{
	return scanlines_per_frame;
}

// ─── Reading ────────────────────────────────────────────────────────────────

bool
vera_bandwidth_last_frame(vera_bw_frame_t *out)
{
	if (!have_frame || out == NULL)
		return false;
	memcpy(out, &pub_frame, sizeof *out);
	return true;
}

bool
vera_bandwidth_current_frame(vera_bw_frame_t *out)
{
	if (out == NULL || !vera_bandwidth_active)
		return false;
	memcpy(out, &cur_frame, sizeof *out);
	summarise(out, cur_lines);
	return true;
}

const vera_bw_line_t *
vera_bandwidth_last_lines(uint16_t *count)
{
	if (!have_frame) {
		if (count)
			*count = 0;
		return NULL;
	}
	if (count)
		*count = VERA_BW_LINES;
	return pub_lines;
}
