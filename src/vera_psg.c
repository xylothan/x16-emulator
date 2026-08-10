// Commander X16 Emulator
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#include "vera_psg.h"

#include <stdbool.h>
#include <string.h>

enum waveform {
	WF_PULSE = 0,
	WF_SAWTOOTH,
	WF_TRIANGLE,
	WF_NOISE,
};

struct channel {
	uint16_t freq;
	uint16_t volume;
	bool     left, right;
	uint8_t  pw;
	uint8_t  waveform;

	uint16_t noiseval;
	uint32_t phase;

	// Debug-only mirrors, maintained by render(). Cheap enough (two compares
	// and a store per voice per sample) to keep unconditional, which lets the
	// debugger show live levels without arming the scope capture.
	int16_t  last_sample;
	uint16_t peak;
};

static struct channel channels[16];

// Raw register bytes exactly as written, for the debugger's register view.
static uint8_t reg_shadow[PSG_NUM_REGS];

static uint16_t volume_lut[64] = {
	  0,                                           4,   8,  12,
	 16,  17,  18,  20,  21,  22,  23,  25,  26,  28,  30,  31,
	 33,  35,  37,  40,  42,  45,  47,  50,  53,  56,  60,  63,
	 67,  71,  75,  80,  85,  90,  95, 101, 107, 113, 120, 127,
	135, 143, 151, 160, 170, 180, 191, 202, 214, 227, 241, 255,
	270, 286, 303, 321, 341, 361, 382, 405, 429, 455, 482, 511
};

static uint16_t noise_state;

// Debugger oscilloscope capture. One shared write index keeps every stream
// time-aligned. `scope_armed` counts down in render() and is refreshed by
// psg_debug_scope_read(), so capture stops by itself once the debugger stops
// looking (see vera_psg.h).
#define PSG_SCOPE_ARM_SAMPLES 24576 // ~0.5 s at 48828 Hz

static int16_t  scope_buf[PSG_SCOPE_STREAMS][PSG_SCOPE_SAMPLES];
static unsigned scope_wridx;
static unsigned scope_armed;

void
psg_reset(void)
{
	memset(channels, 0, sizeof(channels));
	memset(reg_shadow, 0, sizeof(reg_shadow));
	memset(scope_buf, 0, sizeof(scope_buf));
	scope_wridx = 0;
	noise_state = 1;
}

void
psg_writereg(uint8_t reg, uint8_t val)
{
	reg &= 0x3f;

	reg_shadow[reg] = val;

	int ch  = reg / 4;
	int idx = reg & 3;

	switch (idx) {
		case 0: channels[ch].freq = (channels[ch].freq & 0xFF00) | val; break;
		case 1: channels[ch].freq = (channels[ch].freq & 0x00FF) | (val << 8); break;
		case 2: {
			channels[ch].right  = (val & 0x80) != 0;
			channels[ch].left   = (val & 0x40) != 0;
			channels[ch].volume = volume_lut[val & 0x3F];
			break;
		}
		case 3: {
			channels[ch].pw       = val & 0x3F;
			channels[ch].waveform = val >> 6;
			break;
		}
	}
}

static void
render(int16_t *left, int16_t *right)
{
	int16_t l = 0;
	int16_t r = 0;

	const bool capture = scope_armed > 0;

	for (int i = 0; i < 16; i++) {
		// In FPGA implementation, noise values are generated every system clock and
		// the channel update is run sequentially. So, even if both two channels are
		// fetching a noise value in the same sample, they should have different values
		noise_state = (noise_state << 1) | (((noise_state >> 1) ^ (noise_state >> 2) ^ (noise_state >> 4) ^ (noise_state >> 15)) & 1);

		struct channel *ch = &channels[i];

		uint32_t new_phase = (ch->left || ch->right) ? ((ch->phase + ch->freq) & 0x1FFFF) : 0;
		if ((ch->phase & 0x10000) && !(new_phase & 0x10000)) {
			ch->noiseval = (noise_state >> 1) & 0x3F;
		}
		ch->phase = new_phase;

		uint32_t v = 0;
		switch (ch->waveform) {
			case WF_PULSE: v = ((ch->phase >> 10) > ch->pw) ? 0 : 0x3F; break;
    		case WF_SAWTOOTH: v = (ch->phase >> 11) ^ ((ch->pw ^ 0x3f) & 0x3f); break;
			case WF_TRIANGLE: v = ((ch->phase & 0x10000) ? (~(ch->phase >> 10) & 0x3F) : ((ch->phase >> 10) & 0x3F)) ^ ((ch->pw ^ 0x3f) & 0x3f); break;		
			case WF_NOISE: v = ch->noiseval; break;
		}
		int16_t sv = (v ^ 0x20);
		if (sv & 0x20) {
			sv |= 0xFFC0;
		}

		int16_t val = sv * ch->volume;

		ch->last_sample = val;
		uint16_t mag = (uint16_t)(val < 0 ? -val : val);
		if (mag > ch->peak) {
			ch->peak = mag;
		}
		if (capture) {
			scope_buf[i][scope_wridx] = val;
		}

		if (ch->left) {
			l += val >> 3;
		}
		if (ch->right) {
			r += val >> 3;
		}
	}

	*left  = l;
	*right = r;

	if (capture) {
		scope_buf[PSG_SCOPE_MIX_L][scope_wridx] = l;
		scope_buf[PSG_SCOPE_MIX_R][scope_wridx] = r;
		scope_wridx = (scope_wridx + 1) & (PSG_SCOPE_SAMPLES - 1);
		scope_armed--;
	}
}

void
psg_render(int16_t *buf, unsigned num_samples)
{
	while (num_samples--) {
		render(&buf[0], &buf[1]);
		buf += 2;
	}
}

// ─── Debugger accessors ─────────────────────────────────────────────────────

bool
psg_debug_get_channel(int ch, struct psg_debug_channel *out)
{
	if (ch < 0 || ch >= PSG_NUM_CHANNELS || out == NULL) {
		return false;
	}
	struct channel *c = &channels[ch];

	out->freq        = c->freq;
	out->vol_raw     = reg_shadow[ch * 4 + 2] & 0x3F;
	out->vol_lut     = c->volume;
	out->left        = c->left;
	out->right       = c->right;
	out->pw          = c->pw;
	out->waveform    = c->waveform;
	out->phase       = c->phase;
	out->noiseval    = c->noiseval;
	out->last_sample = c->last_sample;
	out->peak        = c->peak;

	c->peak = 0; // peak-since-last-read, so a level meter decays naturally
	return true;
}

uint8_t
psg_debug_get_reg(uint8_t reg)
{
	return reg_shadow[reg & 0x3F];
}

uint16_t
psg_debug_get_noise_state(void)
{
	return noise_state;
}

unsigned
psg_debug_scope_read(int ch, int16_t *dest, unsigned max_samples)
{
	// Always (re)arm first: an out-of-range stream still means "the debugger is
	// watching", and the caller may legitimately probe streams in any order.
	scope_armed = PSG_SCOPE_ARM_SAMPLES;

	if (ch < 0 || ch >= PSG_SCOPE_STREAMS || dest == NULL || max_samples == 0) {
		return 0;
	}

	unsigned n = (max_samples > PSG_SCOPE_SAMPLES) ? PSG_SCOPE_SAMPLES : max_samples;
	unsigned start = (scope_wridx + PSG_SCOPE_SAMPLES - n) & (PSG_SCOPE_SAMPLES - 1);
	for (unsigned i = 0; i < n; i++) {
		dest[i] = scope_buf[ch][(start + i) & (PSG_SCOPE_SAMPLES - 1)];
	}
	return n;
}

unsigned
psg_debug_scope_predict(int16_t (*dest)[PSG_SCOPE_SAMPLES], unsigned num_samples)
{
	if (dest == NULL || num_samples == 0) {
		return 0;
	}
	unsigned n = (num_samples > PSG_SCOPE_SAMPLES) ? PSG_SCOPE_SAMPLES : num_samples;

	// Drive the real renderer over a saved-and-restored copy of the state. Going
	// through render() rather than reimplementing the waveform maths is the whole
	// point: the projection can never disagree with what the voices would emit.
	//
	// render() writes last_sample for every voice on every sample, so the
	// per-voice traces come out of that and the mix comes out of its return
	// values. Capture is forced off so this never lands in the live scope ring.
	struct channel saved_channels[16];
	memcpy(saved_channels, channels, sizeof(channels));
	const uint16_t saved_noise = noise_state;
	const unsigned saved_armed = scope_armed;
	scope_armed = 0;

	for (unsigned i = 0; i < n; i++) {
		int16_t l = 0, r = 0;
		render(&l, &r);
		for (int c = 0; c < PSG_NUM_CHANNELS; c++) {
			dest[c][i] = channels[c].last_sample;
		}
		dest[PSG_SCOPE_MIX_L][i] = l;
		dest[PSG_SCOPE_MIX_R][i] = r;
	}

	memcpy(channels, saved_channels, sizeof(channels));
	noise_state = saved_noise;
	scope_armed = saved_armed;
	return n;
}
