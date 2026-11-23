// What a VERA reset leaves behind.
//
// ORACLE: B -- X16Community/vera-module Verilog, tag v48.0.1
// (6e8bea68a5a04687149e27b1b7b3726fb01405f4)

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <stdint.h>

#define VERA_CTRL 0x05
#define VERA_ISR  0x07

#define SPRITE_ATTR 0x1FC00
#define SPRITE_GFX  0x01000

#define ATTR_ZDEPTH 6

static void
step_frame(void)
{
	for (unsigned i = 0; i < 525; i++) {
		video_step(25.0f, 801.0f, false);
	}
}

// video_reset() randomises VRAM, so the sprite bitmap is rewritten each time.
static void
opaque_graphics(void)
{
	for (unsigned i = 0; i < 16 * 8; i++) {
		video_space_write(SPRITE_GFX + i, 0x11);
	}
}

static void
place_sprite(unsigned n, uint8_t mask, uint8_t zdepth)
{
	const uint32_t attr = SPRITE_ATTR + n * 8;

	video_space_write(attr + 0, SPRITE_GFX >> 5);
	video_space_write(attr + 1, 0x00);
	video_space_write(attr + 2, 0x00);
	video_space_write(attr + 3, 0x00);
	video_space_write(attr + 4, 0x00);
	video_space_write(attr + 5, 0x00);
	video_space_write(attr + ATTR_ZDEPTH, (uint8_t)(mask | (zdepth << 2)));
	video_space_write(attr + 7, 0x50);
}

static void
enable_sprites(void)
{
	video_write(VERA_CTRL, 0x00);
	video_write(0x09, 0x41); // DCSEL 0, $9F29: sprites on, VGA output
}

// Masks share bits 6 and 4: a collision reports 0x50 in ISR bits 7:4.
static void
arm_colliding_sprites(void)
{
	opaque_graphics();
	place_sprite(0, 0x70, 1);
	place_sprite(1, 0xD0, 1);
	enable_sprites();
}

static uint8_t
collision_after_a_frame(void)
{
	step_frame();
	return video_read(VERA_ISR, true) & 0xF0;
}

// Control: a collision is visible with the sprites, absent without.
static void
test_the_collision_channel_follows_the_attributes(void)
{
	video_reset();
	arm_colliding_sprites();
	check_eq(collision_after_a_frame(), 0x50, "two armed sprites collide");

	// Zero z-depth stops a sprite.
	place_sprite(1, 0xD0, 0);
	check_eq(collision_after_a_frame(), 0x00, "and stop when one is given no z-depth");
}

// sprite_renderer.v:73-85 decodes attributes from RAM, so reset stops sprites.
static void
test_reset_stops_sprites_configured_before_it(void)
{
	video_reset();
	arm_colliding_sprites();
	check_eq(collision_after_a_frame(), 0x50, "sprites collide before the reset");

	video_reset();
	opaque_graphics();
	enable_sprites();

	check_eq(collision_after_a_frame(), 0x00, "a reset stops sprites that were running before it");
}

// ORACLE: none. Storing the byte already held is a no-op.
static void
test_writing_a_byte_that_is_already_there_changes_nothing(void)
{
	video_reset();
	arm_colliding_sprites();
	step_frame();

	video_reset();
	opaque_graphics();
	enable_sprites();

	const uint8_t before = collision_after_a_frame();

	// sprite_data[] holds zero for both sprites after the reset.
	video_space_write(SPRITE_ATTR + 0 * 8 + ATTR_ZDEPTH, 0x00);
	video_space_write(SPRITE_ATTR + 1 * 8 + ATTR_ZDEPTH, 0x00);

	check_eq(collision_after_a_frame(), before,
	         "rewriting an attribute byte with its own value does not move the machine");
}

// video.c:355 clears the accumulator, matching sprite_renderer.v:416.
static void
test_reset_clears_the_collision_accumulator(void)
{
	video_reset();
	arm_colliding_sprites();

	// Part of a frame, so collisions are accumulated but not yet latched.
	for (unsigned i = 0; i < 32; i++) {
		video_step(25.0f, 801.0f, false);
	}

	video_reset();

	// Reset clears ISR; a vblank latches what the accumulator kept.
	check_eq(collision_after_a_frame(), 0x00, "reset clears the pending collisions");
}

int
main(void)
{
	test_the_collision_channel_follows_the_attributes();
	test_reset_stops_sprites_configured_before_it();
	test_writing_a_byte_that_is_already_there_changes_nothing();
	test_reset_clears_the_collision_accumulator();
	return x16_test_summary("vera_reset");
}
