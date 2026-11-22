// VERA PCM FIFO and registers, checked against the RTL.
//
//   repo:  https://github.com/X16Community/vera-module
//   tag:   v48.0.1 (6e8bea68)
//   files: fpga/source/audio/audio_fifo.v
//          fpga/source/audio/pcm.v
//          fpga/source/top.v

#include "support/harness.h"

#include "vera_pcm.h"

#include <string.h>

// audio_fifo.v:20   reg [7:0] mem_r [4095:0];                 // 4096 cells
// audio_fifo.v:22   wire [11:0] wridx_next = wridx_r + 12'd1;
// audio_fifo.v:26   assign empty = (wridx_r == rdidx_r);
// audio_fifo.v:27   assign full  = (wridx_next == rdidx_r);
//
// 4096 cells, 4095 usable: wridx == rdidx means empty.
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
// top.v:541-545        audio_pcm_sample_rate_r <= 0; ... audio_pcm_volume_r <= 0;
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
// A write to a full FIFO is dropped.
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

// top.v:230   5'h1B: rddata = {audio_fifo_full, audio_fifo_empty,
//                              audio_mode_16bit_r, audio_mode_stereo_r,
//                              audio_pcm_volume_r};
//
// Read bits 7 and 6 are live FIFO flags.
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
// 1023 asserts, 1024 does not.
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

// top.v:474-478   if (do_write && access_addr == 5'h1B) begin
//                     audio_fifo_reset_next  = write_data[7];
//                     audio_mode_16bit_next  = write_data[5];
//                     audio_mode_stereo_next = write_data[4];
//                     audio_pcm_volume_next  = write_data[3:0];
//
// Bits 5:0 are the stored state.
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

// top.v:475   audio_fifo_reset_next = write_data[7];
// top.v:321   audio_fifo_reset_next = 0;          // default, every cycle
// pcm.v:31    wire audio_fifo_reset = rst || fifo_reset;
//
// One-shot: resets both pointers and is not stored.
static void
test_ctrl_bit7_resets_the_fifo(void)
{
	pcm_reset();
	fill(50, 0);
	pcm_write_ctrl(0x80);
	check_eq(state().fifo_cnt, 0u, "bit 7 empties the FIFO");
	check_eq(state().ctrl, 0u, "bit 7 itself is not stored");
}

// top.v:139       reg [7:0] audio_pcm_sample_rate_r, audio_pcm_sample_rate_next;
// top.v:480-481   if (do_write && access_addr == 5'h1C) begin
//                     audio_pcm_sample_rate_next = write_data;
// top.v:231       5'h1C: rddata = audio_pcm_sample_rate_r;
//
// Stored and read back unchanged.
static void
test_rate_register_stores_the_raw_value(void)
{
	pcm_reset();

	static const uint8_t rates[] = {0, 64, 128, 129, 200, 255};
	for (size_t i = 0; i < sizeof rates; i++) {
		pcm_write_rate(rates[i]);
		check_eq(pcm_read_rate(), rates[i], "the rate register reads back as written");
	}
}

static unsigned
bytes_played(uint8_t rate, unsigned samples)
{
	int16_t buf[512];
	pcm_reset();
	fill(USABLE, 0);
	pcm_write_rate(rate);
	pcm_render(buf, samples);
	return (unsigned)USABLE - state().fifo_cnt;
}

// pcm.v:53   reg [7:0] sr_accum_r;
// pcm.v:67   sr_accum_r <= sr_accum_r + sample_rate;
// pcm.v:72   wire new_sample = next_sample_r && (sr_accum7_r != sr_accum_r[7]);
//
// 8-bit accumulator: r and 256 - r fire equally often. 128 is fastest.
static void
test_rates_above_128_alias(void)
{
	unsigned matching = 0;
	for (unsigned r = 129; r <= 255; r++) {
		matching += bytes_played((uint8_t)r, 256) == bytes_played((uint8_t)(256 - r), 256);
	}
	check_eq(matching, 127u, "every rate above 128 plays at the speed of 256 - rate");
	check_eq(bytes_played(128, 256), 256u, "rate 128 takes a byte every sample");
	check_eq(bytes_played(200, 256), 112u, "rate 200 plays at the speed of 56");
	check_eq(bytes_played(255, 256), 2u, "rate 255 plays at the speed of 1");
}

// top.v:474-478   decodes bits 7, 5, 4 and 3:0 of 0x1B; bit 6 is ignored.
// audio_fifo.v:33,44   the only assignments to rdidx_r.
static void
test_ctrl_bit6_is_ignored(void)
{
	// Advance the read pointer so a rewind would be visible.
	pcm_reset();
	fill(100, 0x40);
	int16_t buf[128];
	pcm_write_rate(128);
	pcm_render(buf, 20);

	unsigned rd_before = state().fifo_rdidx;
	unsigned cnt_before = state().fifo_cnt;
	check(rd_before > 0, "the read pointer has advanced");

	pcm_write_ctrl(0x40);
	check_eq(state().fifo_rdidx, rd_before, "bit 6 leaves the read pointer alone");
	check_eq(state().fifo_cnt, cnt_before, "bit 6 leaves the queued count alone");
}

// top.v:475   audio_fifo_reset_next = write_data[7];
//
// Bit 7 resets regardless of bit 6. A drained FIFO stays empty.
static void
test_no_loop_mode(void)
{
	pcm_reset();
	fill(50, 0);
	pcm_write_ctrl(0xC0);
	check_eq(state().fifo_cnt, 0u, "bit 7 resets the FIFO even with bit 6 set");

	fill(10, 0);
	int16_t buf[64];
	pcm_write_rate(128);
	pcm_render(buf, 32);
	check_eq(state().fifo_cnt, 0u, "a drained FIFO does not restart");
}

// audio_fifo.v:17-18   reg [11:0] wridx_r = 0;  reg [11:0] rdidx_r = 0;
// audio_fifo.v:22-23   wridx_next = wridx_r + 12'd1;  rdidx_next = rdidx_r + 12'd1;
//
// 12-bit pointers wrap mod 4096.
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
// Reads are gated on !empty. The debug peek changes no FIFO state.
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
// With nothing queued, the outputs hold their reset value of zero.
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
	test_rates_above_128_alias();
	test_ctrl_bit6_is_ignored();
	test_no_loop_mode();
	test_pointers_wrap_within_the_ring();
	test_debug_peek_consumes_nothing();
	test_empty_fifo_renders_silence();
	return x16_test_summary("vera_pcm");
}
