// The value half of the debugOn contract, swept across VERA's register page.
//
// Pins the value: a debug read returns what a real read would.
// Links the real video.c; see tests/support/video_fixture.c for the seam.
//
// ORACLE: none. An internal consistency property between two paths; the RTL has no debug read.

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <stdio.h>
#include <string.h>

#define VERA_ADDR_L 0x00
#define VERA_ADDR_M 0x01
#define VERA_ADDR_H 0x02
#define VERA_DATA0  0x03
#define VERA_DATA1  0x04
#define VERA_CTRL   0x05

// Registers whose value changes between consecutive reads.
// $9F23 and $9F24 are data ports: a real read advances the address.
static bool
reads_are_self_advancing(uint8_t reg)
{
	return reg == VERA_DATA0 || reg == VERA_DATA1;
}

// Distinguishable values wherever a register may source one, so a mismatch is not 0 == 0.
static void
prime(void)
{
	video_reset();

	// A run of VRAM under the data ports.
	video_write(VERA_CTRL, 0);
	video_write(VERA_ADDR_L, 0x00);
	video_write(VERA_ADDR_M, 0x10);
	video_write(VERA_ADDR_H, 0x10);       // increment 1
	for (unsigned i = 0; i < 64; i++) {
		video_write(VERA_DATA0, (uint8_t)(0x80 + i));
	}

	// Composer, layer and sprite registers across the banks that have them.
	for (unsigned dcsel = 0; dcsel < 8; dcsel++) {
		video_write(VERA_CTRL, (uint8_t)(dcsel << 1));
		for (uint8_t reg = 0x09; reg <= 0x0C; reg++) {
			// $9F2B and $9F2C in bank 5 are the read-only FX fill length.
			if (dcsel == 5 && reg >= 0x0B) {
				continue;
			}
			video_write(reg, (uint8_t)(0x30 + dcsel * 4 + (reg - 0x09)));
		}
	}
	video_write(VERA_CTRL, 0);
	for (uint8_t reg = 0x0D; reg <= 0x1A; reg++) {
		video_write(reg, (uint8_t)(0x40 + reg));
	}
}

// Each register in each bank: debug read, then real read, same answer.
static void
test_debug_and_real_reads_agree(void)
{
	static const unsigned banks[] = {0, 1, 2, 3, 5, 6, 63};

	unsigned mismatches = 0;
	char first[160];
	first[0] = '\0';

	for (unsigned b = 0; b < sizeof banks / sizeof banks[0]; b++) {
		for (uint8_t reg = 0x00; reg < 0x20; reg++) {
			if (reads_are_self_advancing(reg)) {
				continue;
			}

			prime();
			video_write(VERA_CTRL, (uint8_t)(banks[b] << 1));

			uint8_t as_debugger = video_read(reg, true);
			uint8_t as_machine  = video_read(reg, false);

			if (as_debugger != as_machine) {
				mismatches++;
				if (first[0] == '\0') {
					snprintf(first, sizeof first,
					         "$9F%02X with DCSEL %u: debugger sees 0x%02X, machine sees 0x%02X",
					         reg + 0x20, banks[b], as_debugger, as_machine);
				}
				printf("      mismatch: $9F%02X DCSEL %u  debug=0x%02X real=0x%02X\n",
				       reg + 0x20, banks[b], as_debugger, as_machine);
			}
		}
	}

	if (mismatches != 0) {
		printf("      first: %s\n", first);
	}

	check_eq(mismatches, 0, "every VERA register reads the same to the debugger as to the machine");
}

// Data ports are outside the sweep; with the address pinned the rule holds.
static void
test_data_ports_report_the_same_byte(void)
{
	for (uint8_t port = VERA_DATA0; port <= VERA_DATA1; port++) {
		prime();

		// Point the selected register at a known byte, no increment.
		uint8_t sel = (uint8_t)(port - VERA_DATA0);
		video_write(VERA_CTRL, sel);
		video_write(VERA_ADDR_L, 0x08);
		video_write(VERA_ADDR_M, 0x10);
		video_write(VERA_ADDR_H, 0x00);      // increment code 0: no movement

		uint8_t as_debugger = video_read(port, true);
		uint8_t as_machine  = video_read(port, false);

		check_eq(as_debugger, as_machine, "the data port reports the same byte to both");
		check_eq(as_machine, 0x88u, "and it is the byte that was written there");
	}
}

// Having read as the debugger, the machine must still see what it would have seen.
static void
test_a_debug_read_still_moves_nothing(void)
{
	prime();
	video_write(VERA_CTRL, 0);
	video_write(VERA_ADDR_L, 0x20);
	video_write(VERA_ADDR_M, 0x10);
	video_write(VERA_ADDR_H, 0x10);          // increment 1

	uint32_t before = video_get_address(0);
	video_read(VERA_DATA0, true);
	check_eq(video_get_address(0), before, "a debug read of the data port does not advance it");

	video_read(VERA_DATA0, false);
	check_eq(video_get_address(0), before + 1, "while a real read does");
}

// DCSEL=6 $9F29: a real read clears the FX accumulator, a debug read does not.
static void
test_a_debug_read_keeps_the_fx_accumulator(void)
{
	video_reset();
	video_write(VERA_CTRL, 6 << 1);
	for (uint8_t reg = 0x09; reg <= 0x0C; reg++) {
		video_write(reg, (uint8_t)(0x11 * (reg - 0x08)));
	}
	video_write(VERA_CTRL, 2 << 1);
	video_write(0x0C, 0x40);                 // accumulate
	const uint32_t accumulated = video_get_fx_accum();
	check(accumulated != 0, "the accumulator holds a value");

	video_write(VERA_CTRL, 6 << 1);
	video_read(0x09, true);
	check_eq(video_get_fx_accum(), accumulated, "a debug read leaves it");

	video_read(0x09, false);
	check_eq(video_get_fx_accum(), 0u, "a real read clears it");
}

int
main(void)
{
	test_debug_and_real_reads_agree();
	test_data_ports_report_the_same_byte();
	test_a_debug_read_still_moves_nothing();
	test_a_debug_read_keeps_the_fx_accumulator();
	return x16_test_summary("vera_debug_read");
}
