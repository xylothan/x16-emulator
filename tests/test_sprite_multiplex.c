// Sprite multiplexing, end to end through the real video.c.
//
// test_sprite_trace.c drives the recorder directly with synthetic scanlines.
// This one drives VERA: it writes sprite attributes into VRAM, steps the beam a
// cycle at a time, and rewrites a slot part-way down the frame the way a raster
// IRQ handler would. What it checks is that the trace's answer comes from what
// render_sprite_line() actually put on screen -- so a sprite whose bitmap is
// entirely transparent is *not* counted, even though its attributes are live
// and its Y span is on screen.

#include "support/video_fixture.h"

// SDL redefines main to SDL_main, which makes a plain int main(void) a C4026
// and, under /WX, fatal. Undoing the macro is what lets this test build on
// MSVC; several older test files in this tree still trip over it.
#ifdef main
#undef main
#endif

#include "support/harness.h"

#include "sprite_trace.h"
#include "video.h"

#include <stdint.h>
#include <string.h>

// video.c exports these; the test needs to know where the beam is.
extern uint16_t vga_scan_pos_y;
extern int      frame_count;

#define DC_VIDEO 0x09
#define ADDR_SPRDATA 0x1FC00u
#define SPRITE_GFX_OPAQUE 0x4000u
#define SPRITE_GFX_CLEAR 0x5000u

#define MHZ 8.0f

static void
step_one_cycle(void)
{
	video_step(MHZ, 1, false);
}

// Run until the beam reaches `target`, giving up rather than spinning forever
// if the video mode is not what the test thinks it is.
static bool
step_to_line(uint16_t target)
{
	for (long guard = 0; guard < 2000000; guard++) {
		if (vga_scan_pos_y == target) {
			return true;
		}
		step_one_cycle();
	}
	return false;
}

static bool
step_one_frame(void)
{
	const int start = frame_count;
	for (long guard = 0; guard < 2000000; guard++) {
		if (frame_count != start) {
			return true;
		}
		step_one_cycle();
	}
	return false;
}

static void
write_sprite(int slot, uint32_t addr, int x, int y, int z)
{
	uint8_t a[8];
	memset(a, 0, sizeof(a));
	a[0] = (uint8_t)((addr >> 5) & 0xff);
	a[1] = (uint8_t)((addr >> 13) & 0x0f); // 4bpp
	a[2] = (uint8_t)(x & 0xff);
	a[3] = (uint8_t)((x >> 8) & 3);
	a[4] = (uint8_t)(y & 0xff);
	a[5] = (uint8_t)((y >> 8) & 3);
	a[6] = (uint8_t)((z & 3) << 2); // 8x8, no flip
	a[7] = 0;
	for (int b = 0; b < 8; b++) {
		video_space_write(ADDR_SPRDATA + (uint32_t)slot * 8 + b, a[b]);
	}
}

// An 8x8 4bpp sprite is eight rows of four bytes.
static void
fill_sprite_gfx(uint32_t addr, uint8_t value)
{
	for (uint32_t i = 0; i < 8 * 4; i++) {
		video_space_write(addr + i, value);
	}
}

static void
setup(void)
{
	video_reset();
	// VGA out, sprites enabled.
	video_write(DC_VIDEO, 0x41);
	fill_sprite_gfx(SPRITE_GFX_OPAQUE, 0x11); // both nibbles a visible colour
	fill_sprite_gfx(SPRITE_GFX_CLEAR, 0x00);  // wholly transparent
	for (int slot = 0; slot < 128; slot++) {
		write_sprite(slot, SPRITE_GFX_OPAQUE, 0, 0, 0); // z=0: disabled
	}
}

// One slot, two sprites, a raster rewrite between them: the whole point of the
// exercise, measured through the real renderer.
static void
test_a_slot_rewritten_mid_frame_yields_two_effective_sprites(void)
{
	setup();
	check(sprite_trace_set_enabled(true), "tracing enables");

	write_sprite(0, SPRITE_GFX_OPAQUE, 100, 10, 1);

	// Let a frame boundary pass so the trace is recording a clean frame.
	check(step_one_frame(), "the beam reaches a frame boundary");

	// Now the frame under test: the sprite shows at y=10..17, then the handler
	// hands the slot to a second sprite that shows at y=200..207.
	check(step_to_line(50), "the beam reaches the split");
	write_sprite(0, SPRITE_GFX_OPAQUE, 140, 200, 1);
	check(step_one_frame(), "the frame under test completes");

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}

	check_eq(f->summary.effective_sprites, 2u, "one slot showed two sprites");
	check_eq(f->summary.slots_used, 1u, "using one hardware slot");
	check_eq(f->summary.slots_multiplexed, 1u, "the slot is reported as multiplexed");
	check(f->summary.multiplexing, "multiplexing is detected");
	check_eq(f->summary.phantom_writes, 0u, "neither value was wasted");
	check_eq(f->summary.late_writes, 0u, "neither write was late");

	// The residency map has to show the handover at the right place.
	const uint16_t at12  = f->line_gen[(size_t)12 * SPRITE_TRACE_SLOTS + 0];
	const uint16_t at202 = f->line_gen[(size_t)202 * SPRITE_TRACE_SLOTS + 0];
	check(at12 != at202, "the slot holds different values above and below the split");
	check(f->line_flags[(size_t)12 * SPRITE_TRACE_SLOTS + 0] & SPRITE_TRACE_DREW,
	      "the first sprite drew on its own lines");
	check(f->line_flags[(size_t)202 * SPRITE_TRACE_SLOTS + 0] & SPRITE_TRACE_DREW,
	      "the second sprite drew on its own lines");
	check(!(f->line_flags[(size_t)100 * SPRITE_TRACE_SLOTS + 0] & SPRITE_TRACE_DREW),
	      "and neither drew on a line between them");

	if (at202 < f->gen_count) {
		check_eq(f->gens[at202].y, 200u, "the second generation carries its own Y");
		check_eq(f->gens[at202].lines_drawn, 8u, "it drew on all eight of its rows");
	}
}

// The trace reports what reached the screen, not what the attribute table says
// should have. A fully transparent sprite is live, enabled and on screen, and
// still contributes nothing.
static void
test_a_transparent_sprite_is_not_an_effective_sprite(void)
{
	setup();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");

	write_sprite(1, SPRITE_GFX_OPAQUE, 50, 20, 1); // draws
	write_sprite(2, SPRITE_GFX_CLEAR, 60, 20, 1);  // enabled, on screen, invisible

	check(step_one_frame(), "the beam reaches a frame boundary");
	check(step_one_frame(), "a full frame is recorded");

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}

	check_eq(f->summary.effective_sprites, 1u, "only the sprite with visible pixels counts");
	check_eq(f->summary.slots_used, 1u, "one slot reached the screen");

	const size_t clear_at20 = (size_t)20 * SPRITE_TRACE_SLOTS + 2;
	check(f->line_flags[clear_at20] & SPRITE_TRACE_ONSCREEN,
	      "the transparent sprite is still recorded as on screen");
	check(!(f->line_flags[clear_at20] & SPRITE_TRACE_DREW),
	      "but it is not recorded as having drawn");
}

// The per-scanline VERA budget has to be charged from the renderer's own
// accounting, and only on the lines that actually did sprite work.
static void
test_the_budget_is_charged_on_the_lines_that_did_the_work(void)
{
	setup();
	sprite_trace_set_enabled(false);
	check(sprite_trace_set_enabled(true), "tracing re-enables");

	write_sprite(0, SPRITE_GFX_OPAQUE, 100, 30, 1);

	check(step_one_frame(), "the beam reaches a frame boundary");
	check(step_one_frame(), "a full frame is recorded");

	const sprite_trace_frame_t *f = sprite_trace_last_frame();
	check(f != NULL, "the frame is published");
	if (!f) {
		return;
	}

	// Every line pays one cycle per slot looked at; a line carrying the sprite
	// pays for its fetches and pixels on top.
	const uint16_t idle = f->lines[10].budget_used;
	const uint16_t busy = f->lines[32].budget_used;
	check(busy > idle, "a line with a sprite on it costs more than one without");
	check(idle >= 128, "every line still pays to look at all 128 slots");
	check_eq(f->lines[32].drawn, 1u, "one sprite drew on that line");
	check_eq(f->summary.lines_exhausted, 0u, "a single small sprite is nowhere near the budget");
	check(f->summary.peak_budget < SPRITE_TRACE_LINE_BUDGET,
	      "peak demand stays under the hardware ceiling");
}

int
main(void)
{
	test_a_slot_rewritten_mid_frame_yields_two_effective_sprites();
	test_a_transparent_sprite_is_not_an_effective_sprite();
	test_the_budget_is_charged_on_the_lines_that_did_the_work();
	sprite_trace_set_enabled(false);
	return x16_test_summary("sprite_multiplex");
}
