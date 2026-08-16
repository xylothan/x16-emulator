// VERA address registers and the data ports at $9F20-$9F24.
//
// ORACLE (strength B): the VERA Verilog.
//
//   repo:   https://github.com/X16Community/vera-module
//   tag:    v47.0.2   (commit 45cc1f05)
//   files:  fpga/source/addr_data.v, fpga/source/top.v
//
// This is the first test to link the real video.c. That file is the VERA
// implementation and the emulator's SDL host in one, and only the first half is
// under test here -- see tests/support/video_fixture.c for the seam. No test
// calls video_init(), video_update() or video_end(); video_reset() is
// deterministic and is called between cases.
//
// The data ports are where nearly every VERA access goes: a program sets an
// address and an auto-increment once, then reads or writes $9F23 in a loop. The
// increment arithmetic is therefore load-bearing for all of it, and it is also
// the part a debugger can disturb, because reading the data port MOVES the
// address on real hardware.

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <string.h>

#define VERA_ADDR_L  0x00
#define VERA_ADDR_M  0x01
#define VERA_ADDR_H  0x02
#define VERA_DATA0   0x03
#define VERA_DATA1   0x04
#define VERA_CTRL    0x05

// video.c:2792-2796   case 0x02:
//                         io_addr[io_addrsel] = ... | ((value & 0x1) << 16);
//                         fx_nibble_bit[io_addrsel]  = (value >> 1) & 0x1;
//                         fx_nibble_incr[io_addrsel] = (value >> 2) & 0x1;
//                         io_inc[io_addrsel]         = value >> 3;
//
// So ADDRx_H carries address bit 16 in bit 0, two nibble-mode controls in bits
// 1 and 2, the decrement flag in bit 3 and the increment code in bits 7:4.
static uint8_t
addr_h(unsigned incr_code, bool decrement, unsigned addr_bit16)
{
	return (uint8_t)((incr_code << 4) | ((decrement ? 1u : 0u) << 3) | (addr_bit16 & 1u));
}

static void
set_address(uint8_t sel, uint32_t addr, unsigned incr_code, bool decrement)
{
	video_write(VERA_CTRL, sel & 1);
	video_write(VERA_ADDR_L, addr & 0xFF);
	video_write(VERA_ADDR_M, (addr >> 8) & 0xFF);
	video_write(VERA_ADDR_H, addr_h(incr_code, decrement, addr >> 16));
}

// addr_data.v:182-213, the whole increment decode. Indexed there as
// {decr, incr}; the emulator packs the same two fields as (incr << 1) | decr
// and interleaves the signs in its table, which comes to the same thing.
//
// The four values that are not powers of two -- 40, 80, 160, 320, 640 -- are
// the ones worth having in a test: they exist so a program can step whole tile
// rows and character cells, and they are exactly what a "tidied" implementation
// would get wrong by assuming the series is 1 << n.
static const int expected_increment[16] = {
	0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 40, 80, 160, 320, 640
};

static void
test_reset_clears_the_address_registers(void)
{
	video_reset();
	check_eq(video_get_address(0), 0u, "reset clears the first address register");
	check_eq(video_get_address(1), 0u, "reset clears the second");
}

// addr_data.v:183-197 for the increments, :199-213 for the decrements.
static void
test_every_increment_code(void)
{
	for (unsigned code = 0; code < 16; code++) {
		video_reset();
		set_address(0, 0x1000, code, false);
		video_write(VERA_DATA0, 0xAA);
		check_eq(video_get_address(0), 0x1000u + expected_increment[code],
		         "the increment code advances the address by its documented step");
	}

	for (unsigned code = 0; code < 16; code++) {
		video_reset();
		set_address(0, 0x1000, code, true);
		video_write(VERA_DATA0, 0xAA);
		check_eq(video_get_address(0), 0x1000u - expected_increment[code],
		         "and the decrement flag reverses it");
	}
}

// top.v:169-170 carries vram_addr_0_r[16] through the register interface, so
// the address is 17 bits and wraps at 128K rather than saturating.
static void
test_address_wraps_at_128k(void)
{
	video_reset();
	set_address(0, 0x1FFFF, 1, false);   // one byte short of the end
	video_write(VERA_DATA0, 0x55);
	check_eq(video_get_address(0), 0u, "incrementing past the end wraps to zero");

	video_reset();
	set_address(0, 0x00000, 1, true);
	video_write(VERA_DATA0, 0x55);
	check_eq(video_get_address(0), 0x1FFFFu, "and decrementing below zero wraps to the top");
}

// addr_data.v:75-78 exposes two independent address registers with their own
// increments, and top.v:169-170 selects between them. A program relies on that
// to stream from one place while writing to another.
static void
test_the_two_address_registers_are_independent(void)
{
	video_reset();
	set_address(0, 0x0100, 1, false);    // step by 1
	set_address(1, 0x0200, 2, false);    // step by 2

	video_write(VERA_CTRL, 0);
	video_write(VERA_DATA0, 0x11);
	check_eq(video_get_address(0), 0x0101u, "the first register uses its own increment");
	check_eq(video_get_address(1), 0x0200u, "and leaves the second alone");

	video_write(VERA_CTRL, 1);
	video_write(VERA_DATA1, 0x22);
	check_eq(video_get_address(1), 0x0202u, "the second uses its own");
	check_eq(video_get_address(0), 0x0101u, "and leaves the first alone");
}

// The data port is not idempotent: a read advances the address exactly as a
// write does. This is the behaviour a debugger most easily breaks, by peeking
// VRAM through the same path the machine uses -- so it is pinned here, and the
// debug-read path is pinned separately in the debugOn contract tests.
static void
test_reading_the_data_port_advances_the_address(void)
{
	video_reset();
	set_address(0, 0x0040, 1, false);

	video_read(VERA_DATA0, false);
	check_eq(video_get_address(0), 0x0041u, "a real read advances the address");

	video_read(VERA_DATA0, false);
	check_eq(video_get_address(0), 0x0042u, "and again on the next read");
}

// Round-trip through VRAM: write a run of bytes with auto-increment, then read
// them back the same way. Proves the address arithmetic and the memory it
// indexes agree, which the increment tests alone do not.
static void
test_data_port_round_trip(void)
{
	video_reset();

	set_address(0, 0x0400, 1, false);
	for (unsigned i = 0; i < 16; i++) {
		video_write(VERA_DATA0, (uint8_t)(0xF0 + i));
	}
	check_eq(video_get_address(0), 0x0410u, "sixteen writes advanced sixteen bytes");

	set_address(0, 0x0400, 1, false);
	bool ok = true;
	for (unsigned i = 0; i < 16; i++) {
		if (video_read(VERA_DATA0, false) != (uint8_t)(0xF0 + i)) {
			ok = false;
		}
	}
	check(ok, "the bytes read back in the order they were written");
}

int
main(void)
{
	test_reset_clears_the_address_registers();
	test_every_increment_code();
	test_address_wraps_at_128k();
	test_the_two_address_registers_are_independent();
	test_reading_the_data_port_advances_the_address();
	test_data_port_round_trip();
	return x16_test_summary("vera_video");
}
