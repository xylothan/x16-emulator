// VERA stepped in two parts within an instruction: up to an access by
// video_catch_up(), the rest by video_step_rest(). The parts add up to the
// whole.

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <stdint.h>

#define VERA_IRQLINE 0x08 // read: SCANLINE[7:0]

extern uint8_t  MHZ;
extern float    vga_scan_pos_x;
extern uint16_t vga_scan_pos_y;

// 800 dots at 25 MHz: 256 cycles at 8 MHz.
#define LINE_CYCLES 256u

// One cycle a call, as the main loop's instructions are short: a single call
// ends at most one line.
static void
step_cycles(unsigned n)
{
	while (n--) {
		video_step(MHZ, 1, false);
	}
}

static void
test_parts_add_up(void)
{
	video_reset();
	video_step(MHZ, 100, false);
	const float x_whole = vga_scan_pos_x;
	const uint16_t y_whole = vga_scan_pos_y;

	video_reset();
	video_catch_up(30);
	video_catch_up(70);
	video_step_rest(MHZ, 100);
	check(vga_scan_pos_x == x_whole && vga_scan_pos_y == y_whole, "catch-ups to 30 and 70, then the rest of 100");
}

static void
test_rest_never_steps_back(void)
{
	video_reset();
	video_catch_up(12);
	const float x = vga_scan_pos_x;
	video_catch_up(5);
	video_step_rest(MHZ, 12);
	check(vga_scan_pos_x == x, "a catch-up to an earlier cycle and a rest of 12 after 12");
}

static void
test_rest_restarts_the_count(void)
{
	video_reset();
	video_catch_up(40);
	video_step_rest(MHZ, 40);
	const float x = vga_scan_pos_x;
	video_catch_up(40);
	check(vga_scan_pos_x > x, "the next instruction's catch-up counts from its own start");
}

static void
test_read_sees_the_access_cycle(void)
{
	// The first line is a cycle longer, and reads as line 0 twice.
	video_reset();
	step_cycles(2 * LINE_CYCLES + 2);
	const uint8_t want = video_read(VERA_IRQLINE, false);

	video_reset();
	step_cycles(2 * LINE_CYCLES);
	const uint8_t before = video_read(VERA_IRQLINE, false);
	video_catch_up(2);
	const uint8_t at = video_read(VERA_IRQLINE, false);
	check(before != want, "SCANLINE a cycle before the line ends");
	check_eq(at, want, "SCANLINE read 2 cycles into an instruction that crosses the line");
}

static void
test_frame_end_carried_to_the_rest(void)
{
	video_reset();
	while (!video_step(MHZ, 1, false)) {
	}
	// One cycle short of the next frame.
	step_cycles(525 * LINE_CYCLES - 1);
	video_catch_up(4);
	check(video_step_rest(MHZ, 6), "a frame ended by a catch-up is reported after the instruction");
	check(!video_step_rest(MHZ, 6), "and only once");
}

static void
test_reset_clears_the_count(void)
{
	video_reset();
	video_catch_up(50);
	video_reset();
	video_step_rest(MHZ, 50);
	const float x = vga_scan_pos_x;

	video_reset();
	video_step(MHZ, 50, false);
	check(vga_scan_pos_x == x, "cycles caught up before a reset are not taken off the next rest");
}

int
main(void)
{
	test_parts_add_up();
	test_rest_never_steps_back();
	test_rest_restarts_the_count();
	test_read_sees_the_access_cycle();
	test_frame_end_carried_to_the_rest();
	test_reset_clears_the_count();
	return x16_test_summary("vera_catch_up");
}
