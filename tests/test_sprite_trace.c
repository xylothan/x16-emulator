// Sprite multiplexing trace: generation lifetimes, the effective/phantom/late
// classification, VERA budget accounting and burst costing.
//
// sprite_trace has no dependency on video.c -- it is fed scalars -- so these
// drive it directly rather than standing a VERA up around it. Each test builds
// a frame the way render_sprite_line() would: advance to open the frame, then
// line_begin / note_slot / line_end per scanline, then advance again to close
// and publish it.

#include "../src/sprite_trace.h"
#include "support/harness.h"

#include <stdio.h>
#include <string.h>

#define W8 0
#define W16 1
#define W32 2
#define W64 3

static uint8_t attrs[SPRITE_TRACE_SLOTS][8];

static void
set_attr(int slot, uint32_t addr, int x, int y, int wcode, int hcode, int z, int pal)
{
	uint8_t *a = attrs[slot];
	a[0]       = (uint8_t)((addr >> 5) & 0xff);
	a[1]       = (uint8_t)((addr >> 13) & 0x0f);
	a[2]       = (uint8_t)(x & 0xff);
	a[3]       = (uint8_t)((x >> 8) & 3);
	a[4]       = (uint8_t)(y & 0xff);
	a[5]       = (uint8_t)((y >> 8) & 3);
	a[6]       = (uint8_t)((z & 3) << 2);
	a[7]       = (uint8_t)(((hcode & 3) << 6) | ((wcode & 3) << 4) | (pal & 0x0f));
}

static void
clear_attrs(void)
{
	memset(attrs, 0, sizeof(attrs));
}

// Rewrite a slot's attributes the way a raster handler would: eight stores,
// each reported to the trace with the value it replaced.
static void
write_slot(int slot, uint16_t line, uint32_t cycle, uint32_t addr, int x, int y, int wcode,
           int hcode, int z, int pal)
{
	uint8_t old[8];
	memcpy(old, attrs[slot], 8);
	set_attr(slot, addr, x, y, wcode, hcode, z, pal);
	for (int b = 0; b < 8; b++) {
		sprite_trace_note_write((uint8_t)slot, (uint8_t)b, old[b], attrs[slot][b], line,
		                        cycle + (uint32_t)b * 5);
	}
}

// One scanline of rendering. `drawing_slot` is the slot that put pixels down on
// this line, or -1 for none. Every slot is reported as evaluated, which is what
// the real renderer does until its budget runs out.
static void
render_line_with(uint16_t line, uint32_t cycle, int drawing_slot, uint16_t budget)
{
	sprite_trace_line_begin(line, cycle);
	uint16_t drawn = 0;
	for (int slot = 0; slot < SPRITE_TRACE_SLOTS; slot++) {
		uint8_t flags = SPRITE_TRACE_EVALUATED;
		if (slot == drawing_slot) {
			flags |= SPRITE_TRACE_ONSCREEN | SPRITE_TRACE_DREW;
			drawn++;
		}
		sprite_trace_note_slot(line, (uint8_t)slot, attrs[slot],
		                       (uint16_t)(slot == drawing_slot ? budget : 1), flags);
	}
	sprite_trace_line_end(line, budget, budget > SPRITE_TRACE_LINE_BUDGET - 1,
	                      SPRITE_TRACE_NO_LINE, SPRITE_TRACE_SLOTS, drawn);
}

static void
test_disabled_records_nothing(void)
{
	sprite_trace_set_enabled(false);
	check(sprite_trace_last_frame() == NULL, "no frame while tracing is off");
	check(sprite_trace_memory_usage() == 0, "no memory held while tracing is off");
}

static void
test_a_static_sprite_is_one_effective_sprite(void)
{
	clear_attrs();
	check(sprite_trace_set_enabled(true), "tracing enables");

	set_attr(0, 0x4000, 100, 10, W16, W16, 1, 0);
	sprite_trace_frame_advance(1, 0, attrs);
	for (uint16_t y = 0; y < 40; y++) {
		render_line_with(y, y * 100u, (y >= 10 && y < 26) ? 0 : -1, 40);
	}
	sprite_trace_frame_advance(2, 4000, attrs);

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "a completed frame is published");
	if (!f) {
		return;
	}
	check_eq(f->summary.frame, 1u, "the published frame is the one that just ended");
	check_eq(f->summary.effective_sprites, 1u, "one sprite drew");
	check_eq(f->summary.slots_used, 1u, "one slot drew");
	check_eq(f->summary.slots_multiplexed, 0u, "nothing was multiplexed");
	check_eq(f->summary.phantom_writes, 0u, "no writes were wasted");
	check(!f->summary.multiplexing, "multiplexing is not reported");

	// The 127 unused slots are carried, never written, and must not be counted
	// as wasted work.
	uint16_t drew = 0;
	for (uint16_t i = 0; i < f->gen_count; i++) {
		if (f->gens[i].flags & SPRITE_GEN_DREW) {
			drew++;
			check_eq(f->gens[i].lines_drawn, 16u, "it drew on all 16 of its lines");
			check_eq(f->gens[i].first_line, 10u, "first line matches its Y");
			check_eq(f->gens[i].last_line, 25u, "last line matches its Y plus height");
		}
	}
	check_eq(drew, 1u, "exactly one generation drew");
}

static void
test_one_slot_reused_counts_as_two_sprites(void)
{
	clear_attrs();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");

	// Slot 0 carries sprite A over the top of the frame, then the handler hands
	// the slot to sprite B half way down -- 128 hardware slots, 2 sprites.
	set_attr(0, 0x4000, 10, 0, W64, W64, 1, 0);
	sprite_trace_frame_advance(1, 0, attrs);

	for (uint16_t y = 0; y < 64; y++) {
		render_line_with(y, y * 100u, 0, 200);
	}
	write_slot(0, 64, 6400, 0x8000, 20, 64, W64, W64, 1, 0);
	for (uint16_t y = 64; y < 128; y++) {
		render_line_with(y, y * 100u, 0, 200);
	}
	sprite_trace_frame_advance(2, 12800, attrs);

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the multiplexed frame is published");
	if (!f) {
		return;
	}
	check_eq(f->summary.effective_sprites, 2u, "one slot showed two sprites");
	check_eq(f->summary.slots_used, 1u, "still only one hardware slot");
	check_eq(f->summary.slots_multiplexed, 1u, "that slot is reported as multiplexed");
	check_eq(f->summary.max_gens_in_slot, 2u, "two values were held");
	check(f->summary.multiplexing, "multiplexing is reported");
	check_eq(f->summary.phantom_writes, 0u, "both values were displayed");
	check_eq(f->summary.late_writes, 0u, "neither write was late");

	// The residency map must show the handover, not one sprite over the whole
	// frame: that separation is the entire point of the trace.
	uint16_t top    = f->line_gen[(size_t)32 * SPRITE_TRACE_SLOTS + 0];
	uint16_t bottom = f->line_gen[(size_t)96 * SPRITE_TRACE_SLOTS + 0];
	check(top != bottom, "the slot maps to different values above and below the split");
	if (top < f->gen_count && bottom < f->gen_count) {
		check_eq(f->gens[top].address, 0x4000u, "the top band is sprite A");
		check_eq(f->gens[bottom].address, 0x8000u, "the bottom band is sprite B");
		check_eq(f->gens[bottom].index_in_slot, 1u, "sprite B is the slot's second value");
		// The handler stored all eight bytes, but only the address high bits,
		// the X low byte and the Y low byte differ between the two sprites.
		// Separating the two is what makes redundant stores visible.
		check_eq(f->gens[bottom].writes, 3u, "three of the eight stores changed anything");
		check_eq(f->summary.mutations, 8u, "all eight were still paid for");
		check_eq(f->gens[bottom].write_line, 64u, "it was stamped with the line it completed on");
	}
}

static void
test_a_write_that_never_draws_is_phantom(void)
{
	clear_attrs();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");

	sprite_trace_frame_advance(1, 0, attrs);
	for (uint16_t y = 0; y < 20; y++) {
		render_line_with(y, y * 100u, -1, 20);
	}
	// Written, then the beam never draws it: a slot update that achieved
	// nothing, which reading the attribute table cannot reveal.
	write_slot(3, 20, 2000, 0x6000, 40, 400, W16, W16, 1, 0);
	for (uint16_t y = 20; y < 40; y++) {
		render_line_with(y, y * 100u, -1, 20);
	}
	sprite_trace_frame_advance(2, 4000, attrs);

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}
	check_eq(f->summary.effective_sprites, 0u, "nothing drew");
	check_eq(f->summary.phantom_writes, 1u, "the wasted write is counted once");
	check_eq(f->summary.mutations, 8u, "all eight stores were seen");
}

static void
test_a_write_after_its_own_y_span_is_late(void)
{
	clear_attrs();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");

	sprite_trace_frame_advance(1, 0, attrs);
	for (uint16_t y = 0; y < 300; y++) {
		render_line_with(y, y * 100u, -1, 20);
	}
	// The sprite is meant to appear at y=10..25, but the handler only gets
	// round to storing it at line 300 -- the classic multiplexer bug.
	write_slot(5, 300, 30000, 0x6000, 40, 10, W16, W16, 1, 0);
	for (uint16_t y = 300; y < 320; y++) {
		render_line_with(y, y * 100u, -1, 20);
	}
	sprite_trace_frame_advance(2, 32000, attrs);

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}
	check_eq(f->summary.late_writes, 1u, "the late write is identified");

	bool flagged = false;
	for (uint16_t i = 0; i < f->gen_count; i++) {
		if (f->gens[i].slot == 5 && (f->gens[i].flags & SPRITE_GEN_LATE)) {
			flagged = true;
		}
	}
	check(flagged, "the generation itself carries the late flag");
}

static void
test_budget_overrun_is_reported(void)
{
	clear_attrs();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");

	sprite_trace_frame_advance(1, 0, attrs);
	for (uint16_t y = 0; y < 10; y++) {
		// Line 4 demands more sprite work than VERA can fetch in a scanline.
		render_line_with(y, y * 100u, 0, (uint16_t)(y == 4 ? 1500 : 300));
	}
	sprite_trace_frame_advance(2, 1000, attrs);

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}
	check_eq(f->summary.lines_exhausted, 1u, "one scanline is over budget");
	check_eq(f->summary.peak_line, 4u, "the peak is the overloaded line");
	check_eq(f->summary.peak_budget, 1500u, "the peak records the demand, not the ceiling");
	check(f->lines[4].exhausted, "the line is marked exhausted");
	check(!f->lines[3].exhausted, "a line within budget is not");
}

static void
test_writes_are_costed_in_bursts(void)
{
	clear_attrs();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");
	sprite_trace_set_burst_gap(128);

	sprite_trace_frame_advance(1, 0, attrs);
	sprite_trace_line_begin(0, 0);
	// Two visits to the raster handler, 40 cycles apart internally and 4000
	// cycles apart from each other.
	for (uint32_t i = 0; i < 5; i++) {
		sprite_trace_note_write(0, (uint8_t)i, 0, (uint8_t)(i + 1), 0, 1000 + i * 10);
	}
	for (uint32_t i = 0; i < 5; i++) {
		sprite_trace_note_write(1, (uint8_t)i, 0, (uint8_t)(i + 1), 0, 5000 + i * 10);
	}
	sprite_trace_line_end(0, 10, false, SPRITE_TRACE_NO_LINE, 128, 0);
	sprite_trace_frame_advance(2, 10000, attrs);

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}
	check_eq(f->summary.mutations, 10u, "every store is counted");
	check_eq(f->summary.mutation_bursts, 2u, "the stores fall into two bursts");
	check_eq(f->summary.mutation_cycles, 80u, "each burst spans 40 cycles");
	check_eq(f->summary.burst_gap, 128u, "the gap the costing used is reported");

	// A wider gap merges them into one burst spanning the whole gap.
	sprite_trace_set_burst_gap(8192);
	sprite_trace_frame_advance(3, 10000, attrs);
	sprite_trace_line_begin(0, 10000);
	for (uint32_t i = 0; i < 5; i++) {
		sprite_trace_note_write(0, (uint8_t)i, 0, (uint8_t)(i + 1), 0, 11000 + i * 10);
	}
	for (uint32_t i = 0; i < 5; i++) {
		sprite_trace_note_write(1, (uint8_t)i, 0, (uint8_t)(i + 1), 0, 15000 + i * 10);
	}
	sprite_trace_line_end(0, 10, false, SPRITE_TRACE_NO_LINE, 128, 0);
	sprite_trace_frame_advance(4, 20000, attrs);

	f = sprite_trace_last_frame();
	if (f) {
		check_eq(f->summary.mutation_bursts, 1u, "a wide gap merges the two visits");
		check_eq(f->summary.mutation_cycles, 4040u, "the merged burst spans both");
	}
	sprite_trace_set_burst_gap(128);
}

static void
test_a_redundant_store_costs_but_does_not_split(void)
{
	clear_attrs();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");

	set_attr(0, 0x4000, 10, 0, W16, W16, 1, 0);
	sprite_trace_frame_advance(1, 0, attrs);
	render_line_with(0, 0, 0, 40);

	// Storing the value that is already there is real bus traffic and real
	// cycles, but it changes nothing the beam will draw.
	for (int b = 0; b < 8; b++) {
		sprite_trace_note_write(0, (uint8_t)b, attrs[0][b], attrs[0][b], 1, 100);
	}
	for (uint16_t y = 1; y < 16; y++) {
		render_line_with(y, y * 100u, 0, 40);
	}
	sprite_trace_frame_advance(2, 1600, attrs);

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}
	check_eq(f->summary.mutations, 8u, "the redundant stores are still counted");
	check_eq(f->summary.effective_sprites, 1u, "but no second generation was opened");
	check_eq(f->summary.max_gens_in_slot, 1u, "the slot held one value throughout");
}

static void
test_disable_releases_the_buffers(void)
{
	check(sprite_trace_set_enabled(true), "tracing enables");
	check(sprite_trace_memory_usage() > 0, "buffers are held while tracing");
	sprite_trace_set_enabled(false);
	check_eq((uint32_t)sprite_trace_memory_usage(), 0u, "buffers are released when it stops");
	check(sprite_trace_last_frame() == NULL, "no frame is served once disabled");
}

int
main(void)
{
	test_disabled_records_nothing();
	test_a_static_sprite_is_one_effective_sprite();
	test_one_slot_reused_counts_as_two_sprites();
	test_a_write_that_never_draws_is_phantom();
	test_a_write_after_its_own_y_span_is_late();
	test_budget_overrun_is_reported();
	test_writes_are_costed_in_bursts();
	test_a_redundant_store_costs_but_does_not_split();
	test_disable_releases_the_buffers();
	return x16_test_summary("sprite_trace");
}
