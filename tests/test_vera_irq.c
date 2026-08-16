// VERA's interrupt latch and mask: IEN ($9F26) and ISR ($9F27).
//
// Four sources reach ISR -- VSYNC, LINE, sprite collision and PCM AFLOW -- but
// only three of them are latches, and only three of ISR's eight bits can be
// cleared by writing to it. Which bits a write can store and which it can
// clear is the whole of what this file checks.
//
// Raster IRQ position is not covered. The plan rules it out as needing
// co-simulation, so events are used as events: nothing here asserts which
// scanline one arrives on.
//
// ORACLE: B -- X16Community/vera-module Verilog, tag v47.0.2
// (45cc1f053376dae12173ea63612820e4d289c0da). The cited lines are quoted
// inline; `python tests/fetch_vera_rtl.py` retrieves the sources to check them
// against. The IRQ logic is identical at v48.0.1, three lines further down
// top.v.

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <stdint.h>

#define VERA_CTRL       0x05
#define VERA_IEN        0x06
#define VERA_ISR        0x07
#define VERA_IRQLINE    0x08 // write: IRQLINE[7:0]. read: SCANLINE[7:0]
#define VERA_AUDIO_CTRL 0x1B
#define VERA_AUDIO_DATA 0x1D

#define IRQ_VSYNC  0x01
#define IRQ_LINE   0x02
#define IRQ_SPRCOL 0x04
#define IRQ_AFLOW  0x08

#define SPRITE_ATTR 0x1FC00
#define SPRITE_GFX  0x01000

// video_step() advances the scan position by PIXEL_FREQ * steps / mhz and
// subtracts one scan width per call, so any step wider than VGA_SCAN_WIDTH
// (800) advances exactly one line.
static void
step_line(void)
{
	video_step(25.0f, 801.0f, false);
}

// SCAN_HEIGHT lines: every scanline value is passed exactly once and the
// position ends where it began.
static void
step_frame(void)
{
	for (unsigned i = 0; i < 525; i++) {
		step_line();
	}
}

static uint8_t
read_isr(void)
{
	return video_read(VERA_ISR, false);
}

static uint8_t
read_ien(void)
{
	return video_read(VERA_IEN, false);
}

// top.v:529-536
//     irq_enable_audio_fifo_low_r   <= 0;
//     irq_enable_vsync_r            <= 0;
//     irq_enable_line_r             <= 0;
//     irq_enable_sprite_collision_r <= 0;
//     irq_status_vsync_r            <= 0;
//     irq_status_line_r             <= 0;
//     irq_status_sprite_collision_r <= 0;
//     irq_line_r                    <= 0;
static void
test_reset_clears_the_enables_the_latches_and_irqline(void)
{
	video_reset();
	video_write(VERA_IEN, 0x0F);
	video_write(VERA_IRQLINE, 100);
	step_frame();
	check((read_isr() & (IRQ_VSYNC | IRQ_LINE)) != 0, "the frame left something to clear");

	video_reset();
	check_eq(read_ien() & 0x0F, 0, "reset clears the four enables");
	check_eq(read_isr() & 0x07, 0, "reset clears the three status latches");

	uint16_t line = 0xFFFF;
	video_get_irq_state(NULL, NULL, &line);
	check_eq(line, 0, "reset clears IRQLINE");
}

// top.v:334-335
//     if (do_write && access_addr == 5'h05) begin
//         fpga_reconfigure_next = write_data[7];
// Reconfiguring reloads the bitstream, so CTRL bit 7 reaches the reset block
// above rather than being a separate clear.
static void
test_ctrl_bit_7_resets_the_irq_registers(void)
{
	video_reset();
	video_write(VERA_IEN, 0x0F);
	step_frame();
	check((read_isr() & IRQ_VSYNC) != 0, "VSYNC was latched before the reset");

	video_write(VERA_CTRL, 0x80);
	check_eq(read_ien() & 0x0F, 0, "CTRL bit 7 clears the enables");
	check_eq(read_isr() & 0x07, 0, "CTRL bit 7 clears the status latches");
}

// top.v:431-437
//     if (do_write && access_addr == 5'h06) begin
//         irq_line_next[8]                 = write_data[7];
//         irq_enable_audio_fifo_low_next   = write_data[3];
//         irq_enable_sprite_collision_next = write_data[2];
//         irq_enable_line_next             = write_data[1];
//         irq_enable_vsync_next            = write_data[0];
//     end
//
// top.v:175
//     5'h06: rddata = {irq_line_r[8], scanline[8], 2'b0, irq_enable_audio_fifo_low_r, irq_enable_sprite_collision_r, irq_enable_line_r, irq_enable_vsync_r};
static void
test_ien_stores_four_enables_and_nothing_else(void)
{
	video_reset();

	for (unsigned bit = 0; bit < 4; bit++) {
		video_write(VERA_IEN, (uint8_t)(1u << bit));
		check_eq(read_ien() & 0x0F, 1u << bit, "each enable bit round-trips on its own");
	}

	// Bits 5 and 4 are 2'b0 in the read and have no target in the write.
	video_write(VERA_IEN, 0x30);
	check_eq(read_ien() & 0x3F, 0, "IEN bits 5 and 4 store nothing");

	// Through the register alone that is indistinguishable from a fifth bit
	// stored and then masked out on the way back, so check the byte itself:
	// top.v:433-436 assign four flops and there is nowhere else to put one.
	uint8_t stored = 0xFF;
	video_get_irq_state(&stored, NULL, NULL);
	check_eq(stored, 0, "and reach no enable register behind it");

	video_write(VERA_IEN, 0xFF);
	check_eq(read_ien() & 0x3F, 0x0F, "and read zero even when written as ones");

	video_get_irq_state(&stored, NULL, NULL);
	check_eq(stored, 0x0Fu, "with only the four enables stored");
}

// Bit 6 of the read is scanline[8], live, with no write target. Nothing is
// written to it here, so a set bit can only have come from the counter.
static void
test_ien_bit_6_reports_the_scanline(void)
{
	video_reset();
	video_write(VERA_IEN, 0x00);

	check_eq(read_ien() & 0x40, 0, "bit 6 is clear while the scanline MSB is");
	check_eq(video_read(VERA_IRQLINE, false), 0, "with $9F28 reporting the low byte");

	for (unsigned i = 0; i < 256; i++) {
		step_line();
	}

	check_eq(read_ien() & 0x40, 0x40, "bit 6 follows the scanline MSB");
	check_eq(video_read(VERA_IRQLINE, false), 0, "as the low byte wraps to zero");
}

// top.v:444-446
//     if (do_write && access_addr == 5'h08) begin
//         irq_line_next[7:0] = write_data;
//     end
// with bit 8 arriving from IEN at top.v:432, so the compare value is split
// across two registers and one of them also carries the enables.
static void
test_irqline_spans_ien_bit_7_and_9f28(void)
{
	uint16_t line;

	video_reset();
	video_write(VERA_IRQLINE, 0x55);
	video_get_irq_state(NULL, NULL, &line);
	check_eq(line, 0x055, "$9F28 writes the low eight bits");

	video_write(VERA_IEN, 0x80);
	video_get_irq_state(NULL, NULL, &line);
	check_eq(line, 0x155, "IEN bit 7 writes bit 8 and leaves the low byte");
	check_eq(read_ien() & 0x80, 0x80, "and reads back from IRQLINE, not from an enable");

	video_write(VERA_IEN, 0x0F);
	video_get_irq_state(NULL, NULL, &line);
	check_eq(line, 0x055, "so setting the enables clears bit 8 with them");
}

// top.v:438-443
//     if (do_write && access_addr == 5'h07) begin
//         // Clear status bits
//         irq_status_sprite_collision_next = irq_status_sprite_collision_r & !write_data[2];
//         irq_status_line_next             = irq_status_line_r             & !write_data[1];
//         irq_status_vsync_next            = irq_status_vsync_r            & !write_data[0];
//     end
static void
test_isr_latches_are_cleared_by_writing_one(void)
{
	video_reset();
	video_write(VERA_IRQLINE, 100);
	step_frame();

	// Exact rather than masked: a frame with no sprites must latch VSYNC and
	// LINE and nothing else, over AFLOW live from the empty FIFO.
	check_eq(read_isr(), IRQ_VSYNC | IRQ_LINE | IRQ_AFLOW, "a frame latches VSYNC and LINE");

	video_write(VERA_ISR, 0x00);
	check_eq(read_isr(), IRQ_VSYNC | IRQ_LINE | IRQ_AFLOW, "writing zero clears nothing");

	video_write(VERA_ISR, IRQ_LINE);
	check_eq(read_isr(), IRQ_VSYNC | IRQ_AFLOW, "writing one bit clears only that bit");

	// The latch holds until it is written, not until the next line.
	step_line();
	step_line();
	check_eq(read_isr(), IRQ_VSYNC | IRQ_AFLOW, "the remaining latch survives more scanlines");

	video_write(VERA_ISR, IRQ_VSYNC);
	check_eq(read_isr(), IRQ_AFLOW, "and clears when its own bit is written");
}

// top.v:176 takes bit 3 from a wire, and the write above has no clause for it.
//     5'h07: rddata = {sprite_collisions,   audio_fifo_low,              irq_status_sprite_collision_r, irq_status_line_r, irq_status_vsync_r};
//
// audio_fifo.v:24,28
//     wire [11:0] fifo_count = wridx_r - rdidx_r;
//     assign almost_empty = fifo_count < 12'd1024;
static void
test_aflow_is_live_and_cannot_be_written_away(void)
{
	video_reset();
	check_eq(read_isr() & IRQ_AFLOW, IRQ_AFLOW, "an empty FIFO reads as AFLOW");

	video_write(VERA_ISR, 0xFF);
	check_eq(read_isr() & IRQ_AFLOW, IRQ_AFLOW, "which a write of $FF does not clear");

	for (unsigned i = 0; i < 1023; i++) {
		video_write(VERA_AUDIO_DATA, 0x40);
	}
	check_eq(read_isr() & IRQ_AFLOW, IRQ_AFLOW, "1023 bytes is still below the mark");

	video_write(VERA_AUDIO_DATA, 0x40);
	check_eq(read_isr() & IRQ_AFLOW, 0, "1024 clears it with no write to ISR at all");

	// $9F3B bit 7 empties the FIFO, so the bit returns without ISR being
	// touched in either direction.
	video_write(VERA_AUDIO_CTRL, 0x80);
	check_eq(read_isr() & IRQ_AFLOW, IRQ_AFLOW, "and emptying the FIFO brings it back");
}

// top.v:1197-1200
//     wire [3:0] irq_enable = {irq_enable_audio_fifo_low_r, irq_enable_sprite_collision_r, irq_enable_line_r, irq_enable_vsync_r};
//     wire [3:0] irq_status = {audio_fifo_low,              irq_status_sprite_collision_r, irq_status_line_r, irq_status_vsync_r};
//
//     assign extbus_irq_n = (irq_status & irq_enable) == 0;
static void
test_the_irq_line_is_status_and_enable(void)
{
	video_reset();

	// AFLOW is set throughout, so a released line is a statement about IEN
	// rather than about there being nothing to report.
	check_eq(read_isr() & IRQ_AFLOW, IRQ_AFLOW, "AFLOW is set");
	check(!video_get_irq_out(), "a status bit with no enable does not assert");

	video_write(VERA_IEN, IRQ_AFLOW);
	check(video_get_irq_out(), "enabling that source asserts it");

	video_write(VERA_IEN, IRQ_VSYNC);
	check(!video_get_irq_out(), "a different enable does not");

	step_frame();
	check(video_get_irq_out(), "a latch against its own enable asserts it");

	video_write(VERA_ISR, IRQ_VSYNC);
	check(!video_get_irq_out(), "and clearing that latch releases it");
}

// Nothing in the read at top.v:175-176 has a side effect; only the write at
// top.v:438-443 clears. A debugger polling ISR must therefore not cost the
// program an interrupt.
static void
test_reading_the_status_does_not_clear_it(void)
{
	const uint8_t want = IRQ_VSYNC | IRQ_LINE | IRQ_AFLOW;

	video_reset();
	video_write(VERA_IEN, 0x0D);
	step_frame();
	check_eq(read_isr(), want, "the frame latched VSYNC and LINE");

	check_eq(video_read(VERA_ISR, false), want, "a second real read reports the same");
	check_eq(video_read(VERA_ISR, true), want, "and so does a debug read");
	check_eq(read_isr(), want, "with the latches still set after both");

	uint8_t dbg_ien = 0;
	uint8_t dbg_isr = 0;
	video_get_irq_state(&dbg_ien, &dbg_isr, NULL);
	check_eq(dbg_isr, want, "the debugger's own accessor agrees");
	check_eq(dbg_ien, 0x0Du, "as does its view of the enables");
	check_eq(read_isr(), want, "and reading through it clears nothing either");
}

// ─── Sprite collision, ISR bits 7:4 ─────────────────────────────────────────
//
// Unlike the three status latches, the collision nibble is not part of the
// interrupt register at all. It belongs to the sprite renderer, and the ISR
// write path has no way to reach it.
//
// sprite_renderer.v:37
//     assign collisions = frame_collision_mask_r;
//
// sprite_renderer.v:400-403
//     if (frame_done) begin
//         sprcol_irq                = (cur_collision_mask_r != 4'b0);
//         frame_collision_mask_next = cur_collision_mask_r;
//         cur_collision_mask_next   = 4'b0;
//     end

// Sixteen lines of 4bpp, eight bytes each, every pixel colour index 1.
// Collisions are recorded for non-transparent pixels only, so index 0 would
// never collide however the sprites are placed.
static void
fill_sprite_graphics(void)
{
	for (unsigned i = 0; i < 16 * 8; i++) {
		video_space_write(SPRITE_GFX + i, 0x11);
	}
}

// Eight attribute bytes per sprite. Byte 6 holds the collision mask in bits
// 7:4 and the z-depth in bits 3:2; a z-depth of zero is not drawn.
static void
place_sprite(unsigned n, uint8_t mask, uint8_t zdepth)
{
	const uint32_t attr = SPRITE_ATTR + n * 8;

	video_space_write(attr + 0, SPRITE_GFX >> 5);                  // address 12:5
	video_space_write(attr + 1, 0x00);                             // 4bpp, address 16:13
	video_space_write(attr + 2, 0x00);                             // X
	video_space_write(attr + 3, 0x00);
	video_space_write(attr + 4, 0x00);                             // Y
	video_space_write(attr + 5, 0x00);
	video_space_write(attr + 6, (uint8_t)(mask | (zdepth << 2)));
	video_space_write(attr + 7, 0x50);                             // 16x16, palette offset 0
}

// Two sprites on the same pixels. The second one drawn sees the first one's
// mask already in the line buffer, so the reported collision is the AND of
// the two masks.
static void
arm_two_sprites(uint8_t mask_a, uint8_t mask_b, uint8_t zdepth_b)
{
	video_reset();
	fill_sprite_graphics();
	place_sprite(0, mask_a, 1);
	place_sprite(1, mask_b, zdepth_b);

	video_write(VERA_CTRL, 0x00);
	video_write(0x09, 0x41); // DCSEL 0, $9F29: sprites on, VGA output
}

static void
test_a_collision_latches_the_status_and_the_nibble(void)
{
	arm_two_sprites(0x70, 0xD0, 1);
	step_frame();

	check_eq(read_isr() & 0xF0, 0x50, "the nibble reports the masks the sprites share");
	check_eq(read_isr() & IRQ_SPRCOL, IRQ_SPRCOL, "and the collision status latches with it");
}

// frame_collision_mask_next is assigned unconditionally at frame_done, so the
// nibble describes the frame just ended and nothing earlier. The status bit
// beside it is a latch and does not follow.
static void
test_the_nibble_is_replaced_each_frame_while_the_latch_holds(void)
{
	arm_two_sprites(0x70, 0xD0, 1);
	step_frame();
	check_eq(read_isr() & 0xF0, 0x50, "a collision in the first frame");

	// Take the second sprite out of the picture, so the next frame collides
	// with nothing.
	place_sprite(1, 0xD0, 0);
	step_frame();

	check_eq(read_isr() & 0xF0, 0x00, "a frame without one replaces the nibble with zero");
	check_eq(read_isr() & IRQ_SPRCOL, IRQ_SPRCOL, "while the status latch holds until written");
}

// top.v:440 clears the status latch and nothing beside it:
//     irq_status_sprite_collision_next = irq_status_sprite_collision_r & !write_data[2];
static void
test_clearing_the_status_leaves_the_nibble(void)
{
	arm_two_sprites(0x70, 0xD0, 1);
	step_frame();

	video_write(VERA_ISR, IRQ_SPRCOL);
	check_eq(read_isr() & IRQ_SPRCOL, 0, "the collision status clears when written");
	check_eq(read_isr() & 0xF0, 0x50, "leaving the nibble it was reported with");

	// The nibble has no clause in the ISR write at all, so the byte a program
	// reaches for to acknowledge everything at once cannot touch it either.
	arm_two_sprites(0x70, 0xD0, 1);
	step_frame();
	video_write(VERA_ISR, 0xFF);
	check_divergent((read_isr() & 0xF0) == 0x50,
	                "a write of $FF acknowledges the latches without disturbing the nibble",
	                "video.c:2910 clears all eight bits, but ISR bits 7:4 are "
	                "sprite_renderer.v's frame_collision_mask_r, which the write at "
	                "top.v:438-443 cannot reach; on the machine the nibble holds until "
	                "the next vblank replaces it");
}

// sprite_renderer.v:326-328
//     wire [3:0] collision =
//         linebuf_idx_r < 'd640 &&
//         (!pixel_is_transparent && sprite_collision_mask_r != 4'b0) ? (linebuf_rddata[15:12] & sprite_collision_mask_r) : 4'b0;
//
// The mask is used directly. It was inverted until 4f46cb32 (2023-01-26, "Fix
// sprite collision and improve write timings (again)"), which is the only
// change to this expression in the RTL's history -- so masks that share no bit
// distinguish the two forms rather than merely exercising one.
static void
test_masks_collide_only_where_they_overlap(void)
{
	arm_two_sprites(0x30, 0xC0, 1);
	step_frame();

	check_eq(read_isr() & 0xF0, 0x00, "disjoint masks do not collide");
	check_eq(read_isr() & IRQ_SPRCOL, 0, "and raise no collision status");
}

// top.v:1198 builds irq_status from four bits, and the nibble is not among
// them:
//     wire [3:0] irq_status = {audio_fifo_low, irq_status_sprite_collision_r, irq_status_line_r, irq_status_vsync_r};
static void
test_the_nibble_cannot_raise_the_interrupt_line(void)
{
	arm_two_sprites(0x70, 0xD0, 1);
	step_frame();

	video_write(VERA_ISR, IRQ_VSYNC | IRQ_LINE | IRQ_SPRCOL);
	for (unsigned i = 0; i < 1024; i++) {
		video_write(VERA_AUDIO_DATA, 0x40);
	}
	check_eq(read_isr(), 0x50, "with every latch cleared only the nibble is left");

	// Every enable there is: IEN has no bit for the nibble, which is the whole
	// of why it cannot interrupt.
	video_write(VERA_IEN, 0xFF);
	check(!video_get_irq_out(), "which no enable can turn into an interrupt");
}

int
main(void)
{
	test_reset_clears_the_enables_the_latches_and_irqline();
	test_ctrl_bit_7_resets_the_irq_registers();
	test_ien_stores_four_enables_and_nothing_else();
	test_ien_bit_6_reports_the_scanline();
	test_irqline_spans_ien_bit_7_and_9f28();
	test_isr_latches_are_cleared_by_writing_one();
	test_aflow_is_live_and_cannot_be_written_away();
	test_the_irq_line_is_status_and_enable();
	test_reading_the_status_does_not_clear_it();
	test_a_collision_latches_the_status_and_the_nibble();
	test_the_nibble_is_replaced_each_frame_while_the_latch_holds();
	test_clearing_the_status_leaves_the_nibble();
	test_masks_collide_only_where_they_overlap();
	test_the_nibble_cannot_raise_the_interrupt_line();
	return x16_test_summary("vera_irq");
}
