// VERA PCM channel: the FIFO the CPU feeds and the two registers that drive it.
//
// ORACLE (strength B): the VERA Verilog. VERA is an FPGA, so the RTL is not a
// description of the hardware -- it IS the hardware. Documentation and the
// emulator's own C are both downstream of it and can drift from it.
//
//   repo:   https://github.com/X16Community/vera-module
//   tag:    v47.0.2   (commit 45cc1f05)
//   files:  fpga/source/audio/audio_fifo.v
//           fpga/source/audio/pcm.v
//           fpga/source/top.v                          (register decode)
//
// v47.0.2 because that is what this emulator reports itself to be, from
// VERA_VERSION_MAJOR/MINOR/PATCH in video.c. Guest software reads those bytes
// back and adapts to them, so the version the emulator claims is the version it
// has to behave like, whatever revision any particular physical board carries.
// tests/check_vera_version.py fails if the two are changed apart.
//
// The audio RTL happens to be byte-identical in v48.0.1, so nothing here turns
// on the choice; the two differ only in the version registers and a bus
// write-address register. Line numbers in top.v do differ between them, and
// these are v47.0.2's.
//
// Use this repo and no other. X16Community/vera-module is a fork of
// fvdhoef/vera-module, but the two diverged at the v0.7 release in 2019 and are
// now different chips: the parent has no audio RTL of any kind -- no audio
// directory, no PCM, no FIFO, and only eight bus registers. Audio was added for
// production hardware and exists solely in the fork.
//
// Every expectation below quotes the RTL it rests on, so a reader can check the
// claim here rather than take it on trust or re-derive it from the repo. An
// assertion with no quoted RTL does not belong in this file.
//
// Three behaviours diverge from the RTL and are marked with check_divergent().
// Those assert what the HARDWARE does: they print loudly and do not fail the
// build. If one starts passing, the emulator has been fixed and the marker must
// be deleted rather than left lying.
//
// Pin a commit, but check the history before calling something a divergence.
// Two of the three below look like emulator inventions against the pinned RTL
// and are not: the feature was real, sat on the RTL trunk for five weeks in
// 2023, and was reverted as an accidental merge before any release. A single
// snapshot would have recorded that as "no hardware counterpart", which is
// wrong and would have justified deleting working code. VERA firmware is
// field-flashable, so what matters is what shipped, not what main holds today.
//
// vera_pcm.c includes only its own header and libc, so this links it directly
// with no fixture and no SDL.

#include "support/harness.h"

#include "vera_pcm.h"

#include <string.h>

// audio_fifo.v:20   reg [7:0] mem_r [4095:0];                 // 4096 cells
// audio_fifo.v:22   wire [11:0] wridx_next = wridx_r + 12'd1;
// audio_fifo.v:26   assign empty = (wridx_r == rdidx_r);
// audio_fifo.v:27   assign full  = (wridx_next == rdidx_r);
//
// 4096 cells exist, but `full` asserts one short of them: the write pointer is
// never allowed to catch the read pointer, because wridx == rdidx already means
// empty. So the usable depth is 4095.
#define USABLE (PCM_FIFO_SIZE - 1)

static struct pcm_debug_state
state(void)
{
	struct pcm_debug_state s;
	pcm_debug_get_state(&s);
	return s;
}

static void
fill(unsigned count, uint8_t first)
{
	for (unsigned i = 0; i < count; i++) {
		pcm_write_fifo((uint8_t)(first + i));
	}
}

// audio_fifo.v:31-34   if (rst) begin wridx_r <= 0; rdidx_r <= 0; rddata <= 0; end
// top.v:540-544        audio_pcm_sample_rate_r <= 0; ... audio_pcm_volume_r <= 0;
static void
test_reset_clears_pointers_and_registers(void)
{
	pcm_reset();
	fill(100, 0x10);
	pcm_write_ctrl(0x2F);
	pcm_write_rate(64);

	pcm_reset();
	struct pcm_debug_state s = state();
	check_eq(s.fifo_cnt, 0u, "reset empties the FIFO");
	check_eq(s.fifo_rdidx, 0u, "reset clears the read pointer");
	check_eq(s.fifo_wridx, 0u, "reset clears the write pointer");
	check_eq(s.ctrl, 0u, "reset clears the control register");
	check_eq(s.rate, 0u, "reset clears the rate register");
	check_eq(s.phase, 0u, "reset clears the rate accumulator");
}

// audio_fifo.v:37-40   if (wr_en && !full) begin
//                          mem_r[wridx_r] <= wrdata;
//                          wridx_r <= wridx_next;
//                      end
//
// The write is gated on !full with no else branch, so a byte arriving at a full
// FIFO is dropped: nothing wraps, nothing is overwritten, no error is raised.
static void
test_fifo_depth_is_one_less_than_its_memory(void)
{
	pcm_reset();
	fill(USABLE, 0);
	check_eq(state().fifo_cnt, (unsigned)USABLE,
	         "the FIFO fills to 4095, one short of its 4096 cells");

	pcm_write_fifo(0xAA);
	check_eq(state().fifo_cnt, (unsigned)USABLE,
	         "a write to a full FIFO is dropped");
}

// top.v:227   5'h1B: rddata = {audio_fifo_full, audio_fifo_empty,
//                              audio_mode_16bit_r, audio_mode_stereo_r,
//                              audio_pcm_volume_r};
//
// Bits 7 and 6 of a read are the live FIFO flags from audio_fifo.v:26-27, not
// stored register bits.
static void
test_full_and_empty_status_bits(void)
{
	pcm_reset();
	check(pcm_read_ctrl() & 0x40, "an empty FIFO reports empty");
	check(!(pcm_read_ctrl() & 0x80), "an empty FIFO does not report full");

	pcm_write_fifo(0x01);
	check(!(pcm_read_ctrl() & 0x40), "one byte is not empty");
	check(!(pcm_read_ctrl() & 0x80), "one byte is not full");

	fill(USABLE - 1, 0);
	check(pcm_read_ctrl() & 0x80, "4095 bytes reports full");
	check(!(pcm_read_ctrl() & 0x40), "a full FIFO does not report empty");
}

// audio_fifo.v:24   wire [11:0] fifo_count = wridx_r - rdidx_r;
// audio_fifo.v:28   assign almost_empty = fifo_count < 12'd1024;
//
// A strict less-than against a literal 1024, on a real subtraction-based count
// rather than a pointer-MSB approximation. 1023 asserts and 1024 does not, and
// that edge is what drives the AFLOW interrupt.
static void
test_almost_empty_is_strictly_below_1024(void)
{
	pcm_reset();
	check(pcm_is_fifo_almost_empty(), "an empty FIFO is almost empty");

	fill(1023, 0);
	check(pcm_is_fifo_almost_empty(), "1023 bytes is almost empty");
	check(state().almost_empty, "and the debug view agrees");

	pcm_write_fifo(0);
	check_eq(state().fifo_cnt, 1024u, "1024 bytes queued");
	check(!pcm_is_fifo_almost_empty(), "1024 bytes is not almost empty");
	check(!state().almost_empty, "and the debug view agrees");
}

// top.v:473-477   if (do_write && access_addr == 5'h1B) begin
//                     audio_fifo_reset_next  = write_data[7];
//                     audio_mode_16bit_next  = write_data[5];
//                     audio_mode_stereo_next = write_data[4];
//                     audio_pcm_volume_next  = write_data[3:0];
//
// Bit 5 is the 16-bit flag, bit 4 selects stereo, bits 3:0 are volume. Those
// six bits are the whole of the stored state, and a read returns them under the
// two status bits.
static void
test_ctrl_stores_mode_and_volume_only(void)
{
	pcm_reset();
	pcm_write_ctrl(0x3F);
	check_eq(state().ctrl, 0x3Fu, "16-bit, stereo and full volume are stored");

	pcm_reset();
	pcm_write_ctrl(0x15);
	check_eq(state().ctrl, 0x15u, "stereo and volume 5 are stored");
}

// top.v:474   audio_fifo_reset_next = write_data[7];
// top.v:320   audio_fifo_reset_next = 0;          // default, every cycle
// pcm.v:31    wire audio_fifo_reset = rst || fifo_reset;
//
// Bit 7 is a one-shot command rather than a stored setting: the default
// assignment clears it every cycle, so it pulses for the write cycle only and
// resets both FIFO pointers via audio_fifo.v:32-33.
static void
test_ctrl_bit7_resets_the_fifo(void)
{
	pcm_reset();
	fill(50, 0);
	pcm_write_ctrl(0x80);
	check_eq(state().fifo_cnt, 0u, "bit 7 empties the FIFO");
	check_eq(state().ctrl, 0u, "bit 7 itself is not stored");
}

// DIVERGENCE 1 -- AUDIO_RATE is stored raw on hardware; the emulator folds it.
//
// top.v:139       reg [7:0] audio_pcm_sample_rate_r, audio_pcm_sample_rate_next;
// top.v:479-480   if (do_write && access_addr == 5'h1C) begin
//                     audio_pcm_sample_rate_next = write_data;
// top.v:228       5'h1C: rddata = audio_pcm_sample_rate_r;
// pcm.v:67        sr_accum_r <= sr_accum_r + sample_rate;
// pcm.v:72        wire new_sample = next_sample_r && (sr_accum7_r != sr_accum_r[7]);
//
// All 8 bits are stored unchanged -- no fold, no mask, no clamp -- read back
// unchanged, and added whole to the rate accumulator. A new sample fires when
// bit 7 of that accumulator flips, so a LARGER value plays FASTER, all the way
// to 255.
//
// This is a leftover from the loop feature, not a reading of the hardware. No
// RTL revision has ever transformed this register:
//
//   2020-03-08  1b402ea2 (original PCM)  sr_accum_r <= sr_accum_r + sample_rate;
//   2023-08-12  70a03e00 (loop era)      5'h1C: audio_pcm_sample_rate_next = write_data;
//   v47.0.2     45cc1f05 (this oracle)   same, top.v:480
//
// The original emulator code by VERA's designer was `rate = val`, matching all
// three. Upstream #116 then added a loop feature triggered by writing
// AUDIO_RATE > 128, and masked to recover the rate: `loop = (val > 128);
// rate = loop ? (val & 0x7f) : val`. When #159 moved that trigger onto
// AUDIO_CTRL bits 6 and 7 -- correctly, to match the RTL of the day -- it left
// a transform behind on the rate register and changed it to `256 - val`.
//
// loop is now assigned only in pcm_write_ctrl, so nothing reads loop state out
// of this register. The fold survives with no purpose beyond corrupting rates
// above 128: 200 becomes 56, playing at roughly a quarter of the hardware speed
// and reading back as 56. Values up to and including 128 are unaffected, and
// 128 is the fold's fixed point, so the boundary itself is invisible -- which
// is why this has gone unnoticed.
static void
test_rate_register_stores_the_raw_value(void)
{
	pcm_reset();

	pcm_write_rate(0);
	check_eq(pcm_read_rate(), 0u, "rate 0 is stored as 0");
	pcm_write_rate(64);
	check_eq(pcm_read_rate(), 64u, "rate 64 is stored as 64");
	pcm_write_rate(128);
	check_eq(pcm_read_rate(), 128u, "rate 128 is stored as 128");

	pcm_write_rate(129);
	check_divergent(pcm_read_rate() == 129, "rate 129 is stored as 129",
	                "emulator folds to 127; top.v:480 stores write_data unchanged");

	pcm_write_rate(200);
	check_divergent(pcm_read_rate() == 200, "rate 200 is stored as 200",
	                "emulator folds to 56; on hardware 200 plays faster than 128, not slower");

	pcm_write_rate(255);
	check_divergent(pcm_read_rate() == 255, "rate 255 is stored as 255",
	                "emulator folds to 1, near-silence, where hardware runs near maximum");
}

// DIVERGENCE 2 -- AUDIO_CTRL bit 6 does nothing on any released hardware.
//
// top.v:473-477 decodes only bits 7, 5, 4 and 3:0 of a write to 0x1B; bit 6 is
// never read. Searching top.v for write_data[6] finds one use, at top.v:340,
// for sprites_enabled on an unrelated register. audio_fifo.v assigns rdidx_r in
// exactly two places, :33 (reset, together with wridx_r) and :44 (increment on
// read), so nothing rewinds the read pointer alone.
//
// The emulator treats bit 6 as a "restart" that rewinds the read pointer while
// keeping the queued data. That was NOT invented: the RTL briefly carried the
// same feature, and the emulator implemented it faithfully.
//
//   2023-08-09  VERA v0.3.1 released -- no loop or restart
//   2023-08-12  vera-module 70a03e00 adds fifo_restart and fifo_loop, decoding
//                 audio_fifo_reset_next   = write_data[7:6] == 2'b10;
//                 audio_fifo_restart_next = write_data[6];
//                 audio_fifo_loop_next    = write_data[7:6] == 2'b11;
//               and an rd_rst input on the FIFO that clears rdidx_r alone
//   2023-08-13  emulator #159 matches that decode exactly, one day later
//   2023-09-16  vera-module b1ada295 reverts it: "Was accidentally merged in"
//   2023-11-20  v0.3.2, then v47.0.2 and v48.0.1 -- none contain fifo_loop or
//               fifo_restart
//
// So the feature existed on the RTL trunk for five weeks, by mistake, and is in
// no released bitstream. The emulator tracked the addition and not the revert.
// Recorded rather than removed, since software written in the meantime may use
// it -- but no real machine has ever had it.
static void
test_ctrl_bit6_is_ignored(void)
{
	// The read pointer has to be somewhere other than zero for this to mean
	// anything: rewinding to zero from zero is indistinguishable from doing
	// nothing, so render first to advance it.
	pcm_reset();
	fill(100, 0x40);
	int16_t buf[128];
	pcm_write_rate(128);
	pcm_render(buf, 20);

	unsigned rd_before = state().fifo_rdidx;
	unsigned cnt_before = state().fifo_cnt;
	check(rd_before > 0, "the read pointer has advanced");

	pcm_write_ctrl(0x40);
	check_divergent(state().fifo_rdidx == rd_before,
	                "bit 6 leaves the read pointer alone",
	                "emulator rewinds it to 0; bit 6 of 0x1B is not decoded in top.v");
	check_divergent(state().fifo_cnt == cnt_before,
	                "bit 6 leaves the queued count alone",
	                "emulator requeues everything written; there is no restart in the RTL");
}

// DIVERGENCE 3 -- there is no loop mode on any released hardware.
//
// The string "loop" does not occur anywhere in top.v, audio.v, pcm.v or
// audio_fifo.v at the pinned commit. There is no loop register, no loop input
// on the pcm module (pcm.v:8-20), and no re-read path in the FIFO.
//
// The emulator reads bits 7 and 6 set together as loop mode and, in that case,
// deliberately does not reset the FIFO. On the pinned RTL bit 7 resets it
// regardless of bit 6, because bit 6 is not decoded at all.
//
// Same history as divergence 2: the reverted RTL had
//     if (fifo_empty && fifo_loop) fifo_restart_next = 1;
// which is precisely the emulator's `if (loop && fifo_cnt == 0) fifo_restart()`.
// The emulator is a faithful implementation of a design that was accidentally
// merged and then reverted before any release.
static void
test_no_loop_mode(void)
{
	pcm_reset();
	fill(50, 0);
	pcm_write_ctrl(0xC0);
	check_divergent(state().fifo_cnt == 0,
	                "bit 7 resets the FIFO even with bit 6 set",
	                "emulator reads bits 7+6 as loop mode and keeps the data; no loop in the RTL");
}

// audio_fifo.v:17-18   reg [11:0] wridx_r = 0;  reg [11:0] rdidx_r = 0;
// audio_fifo.v:22-23   wridx_next = wridx_r + 12'd1;  rdidx_next = rdidx_r + 12'd1;
//
// The pointers are 12 bits and wrap mod 4096 by natural overflow, so the RTL
// needs no explicit bounds check and neither should the emulator.
static void
test_pointers_wrap_within_the_ring(void)
{
	pcm_reset();
	fill(USABLE, 0);

	int16_t buf[4096];
	pcm_write_rate(128);
	pcm_render(buf, 2000);

	unsigned before = state().fifo_cnt;
	check(before < (unsigned)USABLE, "rendering consumed part of the queue");

	fill(100, 0x80);
	struct pcm_debug_state s = state();
	check_eq(s.fifo_cnt, before + 100, "writes after the wrap are queued");
	check(s.fifo_wridx < PCM_FIFO_SIZE, "the write pointer stays in the ring");
	check(s.fifo_rdidx < PCM_FIFO_SIZE, "the read pointer stays in the ring");
}

// audio_fifo.v:42   if (rd_en && !empty) begin
//
// Reads are gated on !empty, so an empty FIFO advances nothing. The debug peek
// has no RTL counterpart -- it exists only for the debugger -- so what is
// pinned here is that it disturbs none of the state the RTL does define.
static void
test_debug_peek_consumes_nothing(void)
{
	pcm_reset();
	fill(10, 0x30);

	uint8_t seen[10];
	unsigned got = pcm_debug_peek_fifo(seen, 0, sizeof seen);
	check_eq(got, 10u, "peeking returns what was queued");
	check_eq(seen[0], 0x30u, "peeking starts at the read pointer");
	check_eq(seen[9], 0x39u, "and reads forward in order");
	check_eq(state().fifo_cnt, 10u, "peeking consumes nothing");
	check_eq(state().fifo_rdidx, 0u, "and does not move the read pointer");

	got = pcm_debug_peek_fifo(seen, 8, sizeof seen);
	check_eq(got, 2u, "peeking past the end stops at the queued data");
	check_eq(seen[0], 0x38u, "the offset is applied from the read pointer");

	check_eq(pcm_debug_peek_fifo(seen, 10, 4), 0u,
	         "an offset at the end of the queue returns nothing");
}

// pcm.v:60    sr_accum_r <= 0;                       // and the sample registers
// pcm.v:160   right_output_next = mode_stereo ? right_sample : left_sample;
//
// With nothing queued the sample state machine never leaves IDLE and the output
// registers hold. From reset those registers are zero, so rendering an empty
// FIFO produces silence rather than stale buffer contents.
static void
test_empty_fifo_renders_silence(void)
{
	pcm_reset();
	int16_t buf[64];
	memset(buf, 0x7F, sizeof buf);
	pcm_render(buf, 32);
	bool silent = true;
	for (int i = 0; i < 64; i++) {
		if (buf[i] != 0) {
			silent = false;
		}
	}
	check(silent, "rendering an empty FIFO writes silence");
}

int
main(void)
{
	test_reset_clears_pointers_and_registers();
	test_fifo_depth_is_one_less_than_its_memory();
	test_full_and_empty_status_bits();
	test_almost_empty_is_strictly_below_1024();
	test_ctrl_stores_mode_and_volume_only();
	test_ctrl_bit7_resets_the_fifo();
	test_rate_register_stores_the_raw_value();
	test_ctrl_bit6_is_ignored();
	test_no_loop_mode();
	test_pointers_wrap_within_the_ring();
	test_debug_peek_consumes_nothing();
	test_empty_fifo_renders_silence();
	return x16_test_summary("vera_pcm");
}
