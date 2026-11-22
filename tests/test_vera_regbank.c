// VERA register banking: CTRL at $9F25, and the four registers it banks.
//
// ORACLE (strength B): the VERA Verilog.
//
//   repo:   https://github.com/X16Community/vera-module
//   tag:    v48.0.1   (commit 6e8bea68)
//   file:   fpga/source/top.v
//
// $9F29-$9F2C mean different things depending on DCSEL.
//
// See tests/support/video_fixture.c for the link seam.

#include "support/video_fixture.h"
#include "support/harness.h"

#include "video.h"

#include <string.h>

#define VERA_CTRL    0x05
#define VERA_DC_0    0x09
#define VERA_DC_1    0x0A
#define VERA_DC_2    0x0B
#define VERA_DC_3    0x0C

// top.v:335-338   if (do_write && access_addr == 5'h05) begin
//                     fpga_reconfigure_next = write_data[7];
//                     dc_select_next        = write_data[6:1];
//                     vram_addr_select_next = write_data[0];
static uint8_t
ctrl(unsigned dcsel, unsigned addrsel)
{
	return (uint8_t)(((dcsel & 0x3F) << 1) | (addrsel & 1));
}

static void
select_dcsel(unsigned dcsel)
{
	video_write(VERA_CTRL, ctrl(dcsel, 0));
}

// top.v:89    reg [5:0] dc_select_r, dc_select_next;
// top.v:337   dc_select_next = write_data[6:1];
//
// Six bits: 64 banks.
static void
test_dcsel_is_six_bits(void)
{
	video_reset();

	select_dcsel(0);
	check_eq(video_read(VERA_CTRL, true) >> 1 & 0x3F, 0u, "DCSEL 0 reads back");

	select_dcsel(63);
	check_eq(video_read(VERA_CTRL, true) >> 1 & 0x3F, 63u,
	         "DCSEL 63 reads back, so the field is six bits wide");

	select_dcsel(0x2A);
	check_eq(video_read(VERA_CTRL, true) >> 1 & 0x3F, 0x2Au, "and an arbitrary value survives");
}

// top.v:176   5'h05: rddata = {1'b0, dc_select_r, vram_addr_select_r};
//
// Bit 7 reads zero: on a write it is fpga_reconfigure (top.v:336), a command, not stored state.
static void
test_ctrl_bit7_reads_as_zero(void)
{
	video_reset();

	video_write(VERA_CTRL, 0x80);
	check_eq(video_read(VERA_CTRL, true) & 0x80, 0u,
	         "bit 7 reads back as zero after being written high");

	video_write(VERA_CTRL, 0xFF);
	check_eq(video_read(VERA_CTRL, true) & 0x80, 0u, "and still zero with every bit set");
	check_eq(video_read(VERA_CTRL, true) & 0x7F, 0x7Fu,
	         "while DCSEL and ADDRSEL keep what was written");
}

// top.v:338   vram_addr_select_next = write_data[0];
//
// ADDRSEL shares the register with DCSEL; changing one must not disturb the other.
static void
test_addrsel_and_dcsel_are_independent(void)
{
	video_reset();

	video_write(VERA_CTRL, ctrl(5, 1));
	uint8_t got = video_read(VERA_CTRL, true);
	check_eq(got & 1, 1u, "ADDRSEL is bit 0");
	check_eq((got >> 1) & 0x3F, 5u, "and DCSEL is unaffected by it");

	video_write(VERA_CTRL, ctrl(5, 0));
	got = video_read(VERA_CTRL, true);
	check_eq(got & 1, 0u, "ADDRSEL clears");
	check_eq((got >> 1) & 0x3F, 5u, "and DCSEL still holds");
}

// top.v:182-211   5'h09..5'h0C each switch on dc_select_r
//
// The four addresses differ per bank; a value survives a trip through another.
static void
test_banked_registers_do_not_alias(void)
{
	video_reset();

	// DCSEL 0: $9F2A is DC_HSCALE, $9F2C is DC_BORDER.
	select_dcsel(0);
	video_write(VERA_DC_1, 0x40);
	video_write(VERA_DC_3, 0x11);

	// DCSEL 1: the same addresses are the active-area window.
	select_dcsel(1);
	video_write(VERA_DC_1, 0x99);
	video_write(VERA_DC_3, 0x77);

	select_dcsel(0);
	check_eq(video_read(VERA_DC_1, true), 0x40u, "bank 0 kept its own value");
	check_eq(video_read(VERA_DC_3, true), 0x11u, "and so did its other register");

	select_dcsel(1);
	check_eq(video_read(VERA_DC_1, true), 0x99u, "bank 1 kept its own value");
	check_eq(video_read(VERA_DC_3, true), 0x77u, "and so did its other register");
}

// top.v:187   default: rddata = "V";
// top.v:194   default: rddata = `VERSION_MAJOR;   // 8'd48
// top.v:202   default: rddata = `VERSION_MINOR;   // 8'd0
// top.v:210   default: rddata = `VERSION_BUILD;   // 8'd1
//
// Unmapped banks return the version string; DCSEL 3 maps none of the four.
// Claims 49.0.6 (VERA_VERSION_* in video.c); the v48.0.1 registers report 48.0.1.
// See tests/check_vera_version.py.
static void
test_unmapped_banks_return_the_version(void)
{
	video_reset();
	select_dcsel(3);

	// Real reads, not debug reads: video.c:2690 diverts a debug read to the stored composer array.
	check_eq(video_read(VERA_DC_0, false), (uint32_t)'V',
	         "an unmapped bank reports 'V' at the first address");
	check_divergent(video_read(VERA_DC_1, false) == 48,
	                "the major version at the second reads 48",
	                "emulator claims 49 (VERA_VERSION_MAJOR, R49.0.6); no public RTL "
	                "for 49 exists, so the oracle is still v48.0.1");
	check_eq(video_read(VERA_DC_2, false), 0u, "the minor version at the third");

	check_divergent(video_read(VERA_DC_3, false) == 1,
	                "the patch version at the fourth reads 1",
	                "emulator claims 6 (VERA_VERSION_PATCH, R49.0.6); the RTL in tag "
	                "v48.0.1 reports 48.0.1");
}

// video.c:2690   if (debugOn) return video_get_dc_value(i);
//
// Unmapped bank: CPU sees the version string, the debugger the composer bytes.
static void
test_debug_and_real_reads_disagree_on_unmapped_banks(void)
{
	video_reset();
	select_dcsel(3);

	check(video_read(VERA_DC_0, true) != video_read(VERA_DC_0, false),
	      "a debug read of an unmapped bank does not match what the CPU sees");
	check_eq(video_read(VERA_DC_0, true), 0u, "the debugger sees the stored composer byte");
	check_eq(video_read(VERA_DC_0, false), (uint32_t)'V', "while the CPU sees the version");
}

// Reading the version needs no bank selected and leaves the selection alone.
static void
test_reading_the_version_leaves_the_bank_alone(void)
{
	video_reset();
	select_dcsel(3);

	video_read(VERA_DC_0, true);
	video_read(VERA_DC_1, true);

	check_eq(video_read(VERA_CTRL, true) >> 1 & 0x3F, 3u,
	         "probing the version leaves DCSEL where it was");
}

int
main(void)
{
	test_dcsel_is_six_bits();
	test_ctrl_bit7_reads_as_zero();
	test_addrsel_and_dcsel_are_independent();
	test_banked_registers_do_not_alias();
	test_unmapped_banks_return_the_version();
	test_debug_and_real_reads_disagree_on_unmapped_banks();
	test_reading_the_version_leaves_the_bank_alone();
	return x16_test_summary("vera_regbank");
}
