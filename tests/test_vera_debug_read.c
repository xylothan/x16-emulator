// The value half of the debugOn contract, swept across VERA's register page.
//
// real_read6502() carries a debugOn flag to every device behind $9F00-$9FFF.
// tests/test_debugon_contract.c pins one half of what that flag means: a debug
// read must move no device state and spend no cycles. This file pins the other
// half, which nothing has been checking:
//
//     a debug read must return the same value a real read would.
//
// Both halves are needed and neither implies the other. A debug read that is
// perfectly free of side effects but reports a different byte is still a
// debugger lying about the machine, and it lies most convincingly in a memory
// view, where the number looks authoritative and there is nothing to compare it
// against.
//
// The existing contract test cannot see this. It drives recording fakes, which
// return synthetic values by construction, so value equality there would be a
// statement about the fake. This file links the real video.c instead, which is
// why it lives separately -- see tests/support/video_fixture.c for the seam.
//
// ORACLE: none, and none is needed. This is an internal consistency property
// between two paths through the same emulator, not a claim about hardware. The
// RTL has no notion of a debug read at all -- which is the point: whatever the
// hardware does, both paths must report it identically.

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

// The registers that currently disagree: $9F29-$9F2C in DCSEL 2, 3, 5 and 6,
// wherever the register in that bank is write-only or unmapped. Stated as a
// number so the sweep fails if the set changes in either direction.
#define KNOWN_MISMATCHES 13

// Registers whose value legitimately changes between two consecutive reads, so
// that comparing a debug read against a following real read would be measuring
// the change rather than the contract.
// $9F23 and $9F24 are the data ports: a real read advances the address, so the
// second read of a pair sees the next byte by design. That is covered in
// tests/test_vera_video.c, and separately below with the address pinned.
static bool
reads_are_self_advancing(uint8_t reg)
{
	return reg == VERA_DATA0 || reg == VERA_DATA1;
}

// Put something distinguishable everywhere a register might source a value
// from, so a mismatch is visible rather than 0 == 0.
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
			// $9F2B and $9F2C in bank 5 are the FX fill length, which is
			// read-only; writing them only produces a warning.
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

// The sweep. For each register in each bank: read it as the debugger, then as
// the machine, and require the same answer.
//
// The debug read goes first deliberately. It is the one claiming to have no
// side effects, so if that half of the contract holds, both reads observe
// identical state and any difference is in the reporting.
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

	// Recorded exactly, not as "some". A bare divergence marker would let a
	// fourteenth mismatch appear without anything noticing, which is how a
	// divergence register turns into a dumping ground. This fails on growth
	// and on unrecorded improvement alike, so either direction has to be a
	// deliberate edit -- the same rule the CPU baselines follow.
	check_eq(mismatches, KNOWN_MISMATCHES,
	         "the set of registers that disagree is unchanged");

	check_divergent(mismatches == 0,
	                "every VERA register reads the same to the debugger as to the machine",
	                "13 registers differ, all $9F29-$9F2C in banks where the register is "
	                "write-only or unmapped: video.c:2690 sends a debug read to the stored "
	                "composer byte, while a real read returns the version string");
}

// The data ports are excluded from the sweep because a real read advances the
// address. With the address pinned either side, the same rule applies to them:
// the byte reported must be the byte the machine would read.
static void
test_data_ports_report_the_same_byte(void)
{
	for (uint8_t port = VERA_DATA0; port <= VERA_DATA1; port++) {
		prime();

		// Point the selected register at a known byte with no increment, so
		// the real read cannot move it.
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

// The other half of the contract, restated here for the registers this file
// touches: having read as the debugger, the machine must still see what it
// would have seen. Without this, a passing sweep could be achieved by making
// the debug read perform the real read's side effects.
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

int
main(void)
{
	test_debug_and_real_reads_agree();
	test_data_ports_report_the_same_byte();
	test_a_debug_read_still_moves_nothing();
	return x16_test_summary("vera_debug_read");
}
