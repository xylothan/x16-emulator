// What a VERA reset leaves behind.
//
// video.c keeps three copies of the sprite attributes: the VRAM mirror the
// guest reads, a sprite_data[] shadow the renderer's raw source, and a
// sprite_properties[] array of values derived from that shadow. The hardware
// keeps one. Every extra copy is a chance for reset to clear some of them and
// leave the machine describing a state that no longer exists -- which has
// already happened once for layers, and is why video_reset_layer_pipeline()
// exists (video.c:255-260).
//
// This file checks that reset leaves nothing behind that still describes the
// pre-reset machine. Sprites are the subject because they are the only derived
// state with an observation channel that needs no framebuffer: a collision
// reaches ISR bits 7:4.
//
// ORACLE: B -- X16Community/vera-module Verilog, tag v47.0.2
// (45cc1f053376dae12173ea63612820e4d289c0da), for the claim that the hardware
// has nothing to go stale. sprite_renderer.v:73-85 decodes every attribute
// from sprite_attr, read from the attribute RAM as the sprite is rendered, and
// sprite_renderer.v:408-417 resets the renderer's own registers. There is no
// derived copy to refresh.
//
// One assertion needs no oracle at all and is marked as such below.

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

// video_reset() fills VRAM with rand(), so the sprite bitmap has to be laid
// down after every reset or a collision would depend on the random contents
// rather than on what this file is measuring.
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

// Two sprites on the same pixels with masks that share bits 6 and 4, so a
// collision reports 0x50 in ISR bits 7:4.
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

// The control. Everything below rests on a collision being visible when the
// sprites are there and absent when they are not, so prove the channel before
// using it to measure a reset.
static void
test_the_collision_channel_follows_the_attributes(void)
{
	video_reset();
	arm_colliding_sprites();
	check_eq(collision_after_a_frame(), 0x50, "two armed sprites collide");

	// A guest taking a sprite out of the picture is an ordinary attribute
	// write, and the renderer follows it.
	place_sprite(1, 0xD0, 0);
	check_eq(collision_after_a_frame(), 0x00, "and stop when one is given no z-depth");
}

// The renderer holds no attribute state of its own -- sprite_renderer.v:73-85
// decodes each one out of the attribute RAM as it goes -- so clearing the
// attributes is enough to stop a sprite, and a reset that clears them must
// stop it too.
static void
test_reset_stops_sprites_configured_before_it(void)
{
	video_reset();
	arm_colliding_sprites();
	check_eq(collision_after_a_frame(), 0x50, "sprites collide before the reset");

	video_reset();
	opaque_graphics();
	enable_sprites();

	check_divergent(collision_after_a_frame() == 0x00,
	                "a reset stops sprites that were running before it",
	                "video.c:339 clears sprite_data but never re-derives "
	                "sprite_properties, whose only writers are the two "
	                "video_space_write() paths at video.c:2396 and video.c:2425, so "
	                "the renderer keeps drawing the pre-reset sprites");
}

// ORACLE: none, and none is needed. Storing a byte that is already there is a
// no-op by definition, whatever the hardware does with it. This is the same
// statement as above with the reset taken out of it, so it holds even if the
// reset value is ever argued about.
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

	// sprite_data[] reads back as zero for both sprites after the reset, so
	// this stores the value already held.
	video_space_write(SPRITE_ATTR + 0 * 8 + ATTR_ZDEPTH, 0x00);
	video_space_write(SPRITE_ATTR + 1 * 8 + ATTR_ZDEPTH, 0x00);

	check_divergent(collision_after_a_frame() == before,
	                "rewriting an attribute byte with its own value does not move the machine",
	                "the write re-derives sprite_properties, which the reset did not, so "
	                "storing a zero over a zero is what actually stops the sprites");
}

// The frame accumulator is cleared (video.c:355), matching
// sprite_renderer.v:416. Recorded so the two halves of the reset are not
// confused: this one is right, the derived attributes are not.
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

	// Reading ISR here would say nothing -- reset clears ISR itself, so the
	// nibble is zero whatever the accumulator holds. The accumulator is only
	// visible once a vblank latches it, so run a frame. The reset also turned
	// sprites off, so nothing new can accumulate during it and what latches is
	// exactly what the reset left behind.
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
