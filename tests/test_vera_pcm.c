// VERA's PCM channel: the FIFO the CPU feeds and the two registers that
// control it.
//
// This is the first test of anything in VERA, and it starts here because
// vera_pcm.c reaches outside itself for nothing at all -- its only includes are
// its own header and libc. So the test links the real thing and needs no
// fixture, no SDL and no stubs.
//
// What is worth pinning is the arithmetic at the edges. The FIFO holds one byte
// fewer than its array, the rate register folds rather than clamps, and the
// interrupt that says "nearly out of audio" fires on a count comparison. Each
// of those is a place an off-by-one changes behaviour without changing anything
// visible in ordinary use.

#include "support/harness.h"

#include "vera_pcm.h"

#include <string.h>

// The FIFO stores one byte fewer than the array: a full ring cannot be told
// from an empty one when the indices meet, so the implementation stops one
// short.
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

static void
test_reset_is_deterministic(void)
{
	pcm_reset();
	fill(100, 0x10);
	pcm_write_ctrl(0x2F);
	pcm_write_rate(64);

	pcm_reset();
	struct pcm_debug_state s = state();
	check_eq(s.fifo_cnt, 0u, "reset empties the FIFO");
	check_eq(s.fifo_rdidx, 0u, "reset rewinds the read index");
	check_eq(s.fifo_wridx, 0u, "reset rewinds the write index");
	check_eq(s.ctrl, 0u, "reset clears the control register");
	check_eq(s.rate, 0u, "reset clears the rate");
	check_eq(s.phase, 0u, "reset clears the playback phase");
}

static void
test_fifo_holds_one_less_than_its_array(void)
{
	pcm_reset();
	fill(USABLE, 0);
	check_eq(state().fifo_cnt, (unsigned)USABLE,
	         "the FIFO accepts one byte fewer than its array");

	// The write that would make the indices meet is dropped rather than
	// wrapping the ring onto itself.
	pcm_write_fifo(0xAA);
	check_eq(state().fifo_cnt, (unsigned)USABLE,
	         "a write to a full FIFO is discarded");

	check(pcm_read_ctrl() & 0x80, "a full FIFO reports full");
	check(!(pcm_read_ctrl() & 0x40), "a full FIFO does not report empty");
}

static void
test_empty_and_full_are_distinguishable(void)
{
	pcm_reset();
	check(pcm_read_ctrl() & 0x40, "an empty FIFO reports empty");
	check(!(pcm_read_ctrl() & 0x80), "an empty FIFO does not report full");

	pcm_write_fifo(0x01);
	check(!(pcm_read_ctrl() & 0x40), "one byte is not empty");
	check(!(pcm_read_ctrl() & 0x80), "one byte is not full");
}

static void
test_almost_empty_boundary(void)
{
	// The AFLOW interrupt is what tells a program to send more audio. It is a
	// count comparison, so the exact boundary matters: at 1024 bytes queued
	// the program is not yet asked for more.
	pcm_reset();
	check(pcm_is_fifo_almost_empty(), "an empty FIFO is almost empty");

	fill(1023, 0);
	check(pcm_is_fifo_almost_empty(), "1023 bytes is still almost empty");
	check(state().almost_empty, "and the debug view agrees");

	pcm_write_fifo(0);
	check_eq(state().fifo_cnt, 1024u, "1024 bytes queued");
	check(!pcm_is_fifo_almost_empty(), "1024 bytes is not almost empty");
	check(!state().almost_empty, "and the debug view agrees");
}

static void
test_rate_folds_rather_than_clamps(void)
{
	// Values above 128 are not clipped to 128, they are folded: 256 - val.
	// Writing 200 and reading back 56 looks like a bug until you know that.
	//
	// 128 is the fold's fixed point (256 - 128 == 128), so the threshold
	// itself is not observable from outside -- testing either side of it
	// proves nothing. The subtrahend is what these cases pin.
	pcm_reset();
	pcm_write_rate(0);
	check_eq(pcm_read_rate(), 0u, "rate 0 reads back as 0");

	pcm_write_rate(128);
	check_eq(pcm_read_rate(), 128u, "128 is the largest value kept as written");

	pcm_write_rate(129);
	check_eq(pcm_read_rate(), 127u, "129 folds to 127");

	pcm_write_rate(200);
	check_eq(pcm_read_rate(), 56u, "200 folds to 56");

	pcm_write_rate(255);
	check_eq(pcm_read_rate(), 1u, "255 folds to 1");
}

static void
test_ctrl_keeps_only_the_low_bits(void)
{
	pcm_reset();
	pcm_write_ctrl(0x3F);
	check_eq(state().ctrl, 0x3Fu, "the format and volume bits are kept");

	// Bits 7 and 6 are commands rather than stored settings, so they never
	// appear in the stored value -- only in the status bits of a read.
	pcm_reset();
	pcm_write_ctrl(0xC0 | 0x15);
	check_eq(state().ctrl, 0x15u, "the command bits are not stored");
}

static void
test_reset_bit_empties_the_fifo(void)
{
	pcm_reset();
	fill(50, 0);
	pcm_write_ctrl(0x80);
	check_eq(state().fifo_cnt, 0u, "bit 7 alone empties the FIFO");
	check(!state().loop, "bit 7 alone does not select looping");
}

static void
test_both_high_bits_select_looping(void)
{
	pcm_reset();
	fill(50, 0);
	pcm_write_ctrl(0xC0);
	check(state().loop, "bits 7 and 6 together select looping");
	// Looping keeps the data: it is the point of the mode.
	check_eq(state().fifo_cnt, 50u, "selecting looping does not empty the FIFO");
}

static void
test_restart_rewinds_without_losing_data(void)
{
	pcm_reset();
	fill(50, 0x40);
	// Bit 6 alone restarts: the read pointer goes back to the beginning and
	// everything written so far is queued again.
	pcm_write_ctrl(0x40);
	struct pcm_debug_state s = state();
	check_eq(s.fifo_rdidx, 0u, "restart rewinds the read index");
	check_eq(s.fifo_cnt, 50u, "restart requeues what was written");
	check(!s.loop, "bit 6 alone does not select looping");
}

static void
test_ring_wraps_and_keeps_order(void)
{
	// Fill, drain most of it, then write past the end of the array so the
	// write index wraps while data is still queued behind it.
	pcm_reset();
	fill(USABLE, 0);

	int16_t buf[4096];
	pcm_write_rate(128);
	pcm_render(buf, 2000);   // consume some of the queue

	unsigned before = state().fifo_cnt;
	check(before < (unsigned)USABLE, "rendering consumed part of the queue");

	fill(100, 0x80);
	struct pcm_debug_state s = state();
	check_eq(s.fifo_cnt, before + 100, "the writes after the wrap are queued");
	check(s.fifo_wridx < PCM_FIFO_SIZE, "the write index stays inside the ring");
	check(s.fifo_rdidx < PCM_FIFO_SIZE, "the read index stays inside the ring");
}

static void
test_peek_does_not_consume(void)
{
	pcm_reset();
	fill(10, 0x30);

	uint8_t seen[10];
	unsigned got = pcm_debug_peek_fifo(seen, 0, sizeof seen);
	check_eq(got, 10u, "peeking returns what was queued");
	check_eq(seen[0], 0x30u, "peeking starts at the read pointer");
	check_eq(seen[9], 0x39u, "and reads forward in order");
	check_eq(state().fifo_cnt, 10u, "peeking consumes nothing");

	got = pcm_debug_peek_fifo(seen, 8, sizeof seen);
	check_eq(got, 2u, "peeking past the end stops at the queued data");
	check_eq(seen[0], 0x38u, "the offset is applied from the read pointer");

	check_eq(pcm_debug_peek_fifo(seen, 10, 4), 0u,
	         "an offset at the end of the queue returns nothing");
}

static void
test_render_on_an_empty_fifo_is_silent(void)
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
	check(silent, "rendering an empty FIFO writes silence, not stale data");
}

int
main(void)
{
	test_reset_is_deterministic();
	test_fifo_holds_one_less_than_its_array();
	test_empty_and_full_are_distinguishable();
	test_almost_empty_boundary();
	test_rate_folds_rather_than_clamps();
	test_ctrl_keeps_only_the_low_bits();
	test_reset_bit_empties_the_fifo();
	test_both_high_bits_select_looping();
	test_restart_rewinds_without_losing_data();
	test_ring_wraps_and_keeps_order();
	test_peek_does_not_consume();
	test_render_on_an_empty_fifo_is_silent();
	return x16_test_summary("vera_pcm");
}
