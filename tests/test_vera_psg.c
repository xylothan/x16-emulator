// VERA PSG: the sixteen-voice sound generator behind $9F40-$9F7F.
//
// ORACLE (strength B): the VERA Verilog, fpga/source/audio/psg.v.
//
//   repo:   https://github.com/X16Community/vera-module
//   tag:    v48.0.1   (commit 6e8bea68)
//
// This file cites v48.0.1; the PCM tests cite v47.0.2. vera_psg.c:122-123 XOR saw and
// triangle with the inverted pulse width (RTL a8135f32, in v48.0.1).
//
// tests/fetch_vera_rtl.py retrieves the sources.
//
// Links vera_psg.c directly: no fixture, no SDL.

#include "support/harness.h"

#include "vera_psg.h"

#include <string.h>

// Registers are four bytes per voice: two of frequency, one of volume and
// panning, one of pulse width and waveform.
#define R_FREQL(ch) ((ch) * 4 + 0)
#define R_FREQH(ch) ((ch) * 4 + 1)
#define R_VOL(ch)   ((ch) * 4 + 2)
#define R_WAVE(ch)  ((ch) * 4 + 3)

static struct psg_debug_channel
chan(int ch)
{
	struct psg_debug_channel c;
	memset(&c, 0, sizeof c);
	psg_debug_get_channel(ch, &c);
	return c;
}

// Drive one voice with everything needed to make sound.
static void
voice(int ch, uint16_t freq, uint8_t vol, uint8_t waveform, uint8_t pw)
{
	psg_writereg(R_FREQL(ch), freq & 0xFF);
	psg_writereg(R_FREQH(ch), freq >> 8);
	psg_writereg(R_VOL(ch), 0xC0 | (vol & 0x3F));   // left and right enabled
	psg_writereg(R_WAVE(ch), (uint8_t)(waveform << 6) | (pw & 0x3F));
}

// psg.v:126   lfsr_r <= 16'd1;                         // on reset
static void
test_reset_seeds_the_noise_lfsr(void)
{
	psg_writereg(R_FREQL(0), 0x34);
	psg_writereg(R_VOL(0), 0x3F);
	psg_reset();

	check_eq(psg_debug_get_noise_state(), 1u,
	         "reset seeds the noise LFSR to 1");
	check_eq(chan(0).freq, 0u, "reset clears the frequency");
	check_eq(chan(0).vol_raw, 0u, "reset clears the volume");
	check_eq(chan(0).phase, 0u, "reset clears the phase accumulator");
	check_eq(psg_debug_get_reg(R_FREQL(0)), 0u, "reset clears the register shadow");
}

// psg.v:39-44   cur_freq       = cur_channel_attr_r[15:0];
//               cur_volume     = cur_channel_attr_r[21:16];
//               cur_left_en    = cur_channel_attr_r[22];
//               cur_right_en   = cur_channel_attr_r[23];
//               cur_pulsewidth = cur_channel_attr_r[29:24];
//               cur_waveform   = cur_channel_attr_r[31:30];
//
// Byte 2: volume 5:0, left 6, right 7. Byte 3: pulse width 5:0, waveform 7:6.
static void
test_register_decode(void)
{
	psg_reset();

	psg_writereg(R_FREQL(3), 0x34);
	psg_writereg(R_FREQH(3), 0x12);
	check_eq(chan(3).freq, 0x1234u, "the two frequency bytes form a 16-bit word");

	psg_writereg(R_VOL(3), 0x80 | 0x2A);
	check_eq(chan(3).vol_raw, 0x2Au, "volume is bits 5:0");
	check(chan(3).right, "bit 7 enables right");
	check(!chan(3).left, "bit 6 alone controls left");

	psg_writereg(R_VOL(3), 0x40 | 0x2A);
	check(chan(3).left, "bit 6 enables left");
	check(!chan(3).right, "and bit 7 alone controls right");

	psg_writereg(R_WAVE(3), 0x80 | 0x15);
	check_eq(chan(3).pw, 0x15u, "pulse width is bits 5:0");
	check_eq(chan(3).waveform, (uint32_t)PSG_WF_TRIANGLE, "waveform is bits 7:6");
}

// psg.v:26-28   dpram #(.ADDR_WIDTH(6) ...) audio_attr_ram(
//                   .rd_addr({cur_channel_r, cur_channel_byte_r[1:0]}),
//
// 6-bit address: 16 voices of 4 bytes; a write past the end wraps.
static void
test_register_index_wraps_at_64(void)
{
	psg_reset();
	psg_writereg(0, 0x11);
	psg_writereg(64, 0x22);   // wraps onto register 0
	check_eq(chan(0).freq & 0xFF, 0x22u, "register 64 wraps onto register 0");

	psg_writereg(65, 0x33);
	check_eq(chan(0).freq >> 8, 0x33u, "register 65 wraps onto register 1");

	psg_writereg(255, 0x44);  // 255 & 0x3f == 63
	check_eq(chan(15).pw, 0x04u, "register 255 wraps onto register 63");
	check_eq(chan(15).waveform, (uint32_t)PSG_WF_SAWTOOTH, "and decodes there normally");
}

// psg.v:47-48   // cur_volume_log = cur_volume * 4                     ; cur_volume < 4
//               //                = 511 * exp2((cur_volume - 63) / 12) ; cur_volume >= 4
//
// Linear below 4, then 0.5 dB per step.
static void
test_volume_curve(void)
{
	psg_reset();

	const struct { uint8_t raw; uint16_t want; } cases[] = {
		{0, 0}, {1, 4}, {2, 8}, {3, 12},   // linear regime
		{4, 16},                            // first logarithmic step
		{63, 511},                          // full scale
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		psg_writereg(R_VOL(0), 0xC0 | cases[i].raw);
		check_eq(chan(0).vol_lut, cases[i].want, "volume maps through the curve");
	}
}

// psg.v:140   new_phase = (cur_left_en | cur_right_en) ? (cur_phase + cur_freq) : 17'd0;
//
// A silenced voice is held at phase zero, so unmuting restarts the waveform.
static void
test_phase_is_held_at_zero_while_silent(void)
{
	psg_reset();
	int16_t buf[256];

	// 0x0700 over 64 samples does not wrap to zero; 0x0800 would.
	voice(0, 0x0700, 63, PSG_WF_SAWTOOTH, 0x20);
	psg_render(buf, 64);
	check(chan(0).phase != 0, "an audible voice advances its phase");

	// Clear both pan bits, keeping frequency and volume.
	psg_writereg(R_VOL(0), 0x3F);
	psg_render(buf, 64);
	check_eq(chan(0).phase, 0u, "a voice with neither side enabled resets to phase zero");

	psg_writereg(R_VOL(0), 0xC0 | 0x3F);
	psg_render(buf, 64);
	check(chan(0).phase != 0, "and runs again once a side is re-enabled");
}

// psg.v:124-130   always @(posedge clk ...) lfsr_r <=
//                     {lfsr_r[14:0], lfsr_r[1] ^ lfsr_r[2] ^ lfsr_r[4] ^ lfsr_r[15]};
//
// A 16-bit Fibonacci LFSR, taps 1, 2, 4 and 15, shifting left, seeded to 1.
//
// Hardware steps it every system clock (psg.v:180-188).
// Emulated once per voice, 16 per sample (vera_psg.c:105-109).
static void
test_noise_lfsr_sequence(void)
{
	psg_reset();
	check_eq(psg_debug_get_noise_state(), 1u, "seeded to 1");

	int16_t buf[8];
	uint16_t expect = psg_debug_get_noise_state();
	for (int sample = 0; sample < 8; sample++) {
		for (int voice_step = 0; voice_step < PSG_NUM_CHANNELS; voice_step++) {
			unsigned bit = ((expect >> 1) ^ (expect >> 2) ^ (expect >> 4) ^ (expect >> 15)) & 1;
			expect = (uint16_t)((expect << 1) | bit);
		}
		psg_render(buf, 1);
		check_eq(psg_debug_get_noise_state(), expect,
		         "the LFSR follows taps 1, 2, 4 and 15, once per voice");
	}
}

// psg.v:142-143   do_noise_sample = cur_phase[16] && !new_phase[16];
//                 new_noise = do_noise_sample ? noise_value_r : cur_noise;
// psg.v:123       noise_value_r = lfsr_r[6:1];
//
// A noise voice latches six LFSR bits only when the phase top bit falls.
static void
test_noise_is_latched_on_phase_wrap(void)
{
	psg_reset();
	int16_t buf[8];

	// Frequency 0x0800: bit 16 falls at sample 64, one latch in the first 64.
	voice(0, 0x0800, 63, PSG_WF_NOISE, 0);

	unsigned changes = 0;
	int changed_at = -1;
	uint16_t prev = chan(0).noiseval;

	for (int sample = 1; sample <= 64; sample++) {
		uint16_t lfsr_before = psg_debug_get_noise_state();
		psg_render(buf, 1);
		check(psg_debug_get_noise_state() != lfsr_before,
		      "the LFSR advances on every sample");

		uint16_t now = chan(0).noiseval;
		if (now != prev) {
			changes++;
			changed_at = sample;
			prev = now;
		}
	}

	check_eq(changes, 1u, "the voice latches noise once per phase wrap");
	check_eq((uint32_t)changed_at, 64u, "and does so when the accumulator wraps");
	check(chan(0).noiseval <= 0x3F, "the latched value is six bits");
}

// psg.v:168-173   2'b00: signal = signal_pw;
//                 2'b01: signal = signal_saw;
//                 2'b10: signal = signal_triangle;
//                 2'b11: signal = signal_noise;
static void
test_waveform_codes(void)
{
	psg_reset();
	const uint8_t codes[] = {PSG_WF_PULSE, PSG_WF_SAWTOOTH, PSG_WF_TRIANGLE, PSG_WF_NOISE};
	for (unsigned i = 0; i < 4; i++) {
		psg_writereg(R_WAVE(1), (uint8_t)(codes[i] << 6));
		check_eq(chan(1).waveform, codes[i], "the waveform code round-trips");
	}
}

// Render a voice from reset and report whether anything came out differently.
static bool
renders_same(uint8_t waveform, uint8_t pw_a, uint8_t pw_b)
{
	int16_t a[256], b[256];

	psg_reset();
	voice(0, 0x0400, 63, waveform, pw_a);
	psg_render(a, 128);

	psg_reset();
	voice(0, 0x0400, 63, waveform, pw_b);
	psg_render(b, 128);

	return memcmp(a, b, sizeof a) == 0;
}

// psg.v:162-164   signal_pw       = (cur_phase[16:10] > {1'b0, cur_pulsewidth}) ? 6'd0 : 6'd63;
//                 signal_saw      = cur_phase[16:11] ^ ~cur_pulsewidth[5:0];
//                 signal_triangle = (cur_phase[16] ? ~cur_phase[15:10] : cur_phase[15:10])
//                                   ^ ~cur_pulsewidth[5:0];
//
// R47 saw and triangle ignore pulse width; R48 XORs both with its complement.
static void
test_pulse_width_shapes_saw_and_triangle(void)
{
	check(!renders_same(PSG_WF_PULSE, 0x10, 0x30),
	      "pulse width changes the pulse waveform");

	check(!renders_same(PSG_WF_SAWTOOTH, 0x00, 0x3F),
	      "pulse width changes the sawtooth (R48; R47 ignores it)");

	check(!renders_same(PSG_WF_TRIANGLE, 0x00, 0x3F),
	      "pulse width changes the triangle (R48; R47 ignores it)");

	// A full-scale pulse width XORs in zero and leaves the plain waveform.
	check(renders_same(PSG_WF_SAWTOOTH, 0x3F, 0x3F),
	      "the same pulse width gives the same sawtooth");
}

// psg.v:175   signed_signal = signal ^ 6'h20;
//
// Flipping the top bit puts silence mid-range; volume zero must produce exactly nothing.
static void
test_silence_at_zero_volume(void)
{
	psg_reset();
	int16_t buf[128];
	memset(buf, 0x7F, sizeof buf);

	voice(0, 0x0400, 0, PSG_WF_SAWTOOTH, 0x20);
	psg_render(buf, 64);

	bool silent = true;
	for (unsigned i = 0; i < 128; i++) {
		if (buf[i] != 0) {
			silent = false;
		}
	}
	check(silent, "volume zero renders exact silence");
}

static void
two_voices(void)
{
	psg_reset();
	voice(0, 0x0400, 63, PSG_WF_PULSE, 0x20);
	voice(1, 0x0733, 40, PSG_WF_SAWTOOTH, 0x10);
}

static void
test_mute_changes_only_the_host_mix(void)
{
	int16_t plain[256], machine[256], host[256], solo[256];

	two_voices();
	psg_render(plain, 128);

	two_voices();
	psg_debug_set_mute_mask(1u << 1);
	psg_render_split(machine, host, 128);
	psg_debug_set_mute_mask(0);

	psg_reset();
	voice(0, 0x0400, 63, PSG_WF_PULSE, 0x20);
	psg_render(solo, 128);

	check(!memcmp(machine, plain, sizeof plain), "a muted voice stays in the machine mix");
	check(!memcmp(host, solo, sizeof solo), "the host mix leaves it out");
	check(memcmp(host, plain, sizeof plain) != 0, "the two mixes differ");

	two_voices();
	psg_debug_set_owner_mute_mask(VOICE_MUTE_MCP, 1u << 1);
	psg_render_split(machine, host, 128);
	psg_debug_set_owner_mute_mask(VOICE_MUTE_MCP, 0);
	check(!memcmp(machine, plain, sizeof plain), "a client's mute stays out of the machine mix");
	check(!memcmp(host, solo, sizeof solo), "and out of the host mix");
}

static void
test_mute_owners_are_separate(void)
{
	psg_debug_set_mute_mask(0x0001);
	psg_debug_set_owner_mute_mask(VOICE_MUTE_DAP, 0x0002);
	psg_debug_set_owner_mute_mask(VOICE_MUTE_MCP, 0x0005);
	check_eq(psg_debug_get_mute_mask(), 0x0001, "a client leaves the UI selection alone");
	check_eq(psg_debug_get_owner_mute_mask(VOICE_MUTE_DAP), 0x0002, "DAP mask");
	check_eq(psg_debug_get_owner_mute_mask(VOICE_MUTE_MCP), 0x0005, "MCP mask");
	check_eq(psg_debug_get_client_mute_mask(), 0x0007, "client masks OR");
	check_eq(psg_debug_get_effective_mute_mask(), 0x0007, "effective mask ORs all");

	psg_debug_set_owner_mute_mask(VOICE_MUTE_MCP, 0);
	check_eq(psg_debug_get_effective_mute_mask(), 0x0003, "releasing one keeps the others");
	psg_debug_set_mute_mask(0);
	check_eq(psg_debug_get_effective_mute_mask(), 0x0002, "the UI clearing keeps a client's");
	psg_debug_set_owner_mute_mask(VOICE_MUTE_DAP, 0);
	check_eq(psg_debug_get_effective_mute_mask(), 0, "all released");
}

static void
test_peek_keeps_the_peak(void)
{
	int16_t buf[128];
	struct psg_debug_channel a, b;

	psg_reset();
	voice(0, 0x0400, 63, PSG_WF_PULSE, 0x20);
	psg_render(buf, 64);

	check(psg_debug_peek_channel(0, &a) && a.peak > 0, "peek reports the peak");
	check(psg_debug_peek_channel(0, &b) && b.peak == a.peak, "peek does not reset it");
	check(chan(0).peak == a.peak, "the meter's read still sees it");
	check(psg_debug_peek_channel(0, &b) && b.peak == 0, "the meter's read resets it");
	check(!psg_debug_peek_channel(16, &b), "peek refuses an out-of-range voice");
}

int
main(void)
{
	test_reset_seeds_the_noise_lfsr();
	test_register_decode();
	test_register_index_wraps_at_64();
	test_volume_curve();
	test_phase_is_held_at_zero_while_silent();
	test_noise_lfsr_sequence();
	test_noise_is_latched_on_phase_wrap();
	test_waveform_codes();
	test_pulse_width_shapes_saw_and_triangle();
	test_silence_at_zero_volume();
	test_mute_changes_only_the_host_mix();
	test_mute_owners_are_separate();
	test_peek_keeps_the_peak();
	return x16_test_summary("vera_psg");
}
