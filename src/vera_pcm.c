// Commander X16 Emulator
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#include "vera_pcm.h"
#include <stdio.h>
#include <string.h>

static uint8_t  fifo[PCM_FIFO_SIZE];
static unsigned fifo_wridx;
static unsigned fifo_rdidx;
static unsigned fifo_cnt;

static uint8_t ctrl;
static uint8_t rate;
static uint8_t loop;

static uint8_t volume_lut[16] = {0, 1, 2, 3, 4, 5, 6, 8, 11, 14, 18, 23, 30, 38, 49, 64};

static int16_t cur_l, cur_r;
static uint8_t phase;

// Debugger oscilloscope capture. `scope_armed` counts down in pcm_render() and
// is refreshed by pcm_debug_scope_read(), so capture stops by itself once the
// debugger stops looking (see vera_pcm.h).
#define PCM_SCOPE_ARM_SAMPLES 24576 // ~0.5 s at 48828 Hz

static int16_t  scope_buf[PCM_SCOPE_STREAMS][PCM_SCOPE_SAMPLES];
static unsigned scope_wridx;
static unsigned scope_armed;

static void
fifo_reset(void)
{
	fifo_wridx = 0;
	fifo_rdidx = 0;
	fifo_cnt   = 0;
}

static void
fifo_restart(void)
{
	fifo_rdidx = 0;
	fifo_cnt = fifo_wridx;
}

void
pcm_reset(void)
{
	fifo_reset();
	ctrl  = 0;
	rate  = 0;
	cur_l = 0;
	cur_r = 0;
	phase = 0;
	memset(scope_buf, 0, sizeof(scope_buf));
	scope_wridx = 0;
}

void
pcm_write_ctrl(uint8_t val)
{
	if ((val & 0xc0) == 0xc0) {
		loop = true;
	} else {
		loop = false;
		if (val & 0x80) {
			fifo_reset();
		}
	}
	if (val & 0x40) {
		fifo_restart();
	}
	ctrl = val & 0x3F;
}

uint8_t
pcm_read_ctrl(void)
{
	uint8_t result = ctrl;
	if (fifo_cnt == sizeof(fifo) - 1) {
		result |= 0x80;
	}
	if (fifo_cnt == 0) {
		result |= 0x40;
	}
	return result;
}

void
pcm_write_rate(uint8_t val)
{
	rate = (val > 128) ? (256 - val) : val;
}

uint8_t
pcm_read_rate(void)
{
	return rate;
}

void
pcm_write_fifo(uint8_t val)
{
	if (fifo_cnt < sizeof(fifo) - 1) {
		fifo[fifo_wridx++] = val;
		if (fifo_wridx == sizeof(fifo)) {
			fifo_wridx = 0;
		}
		fifo_cnt++;
	}
}

static uint8_t
read_fifo()
{
	static uint8_t result = 0;
	if (fifo_cnt == 0) {
		return 0;
	}
	result = fifo[fifo_rdidx++];
	if (fifo_rdidx == sizeof(fifo)) {
		fifo_rdidx = 0;
	}
	fifo_cnt--;
	return result;
}

bool
pcm_is_fifo_almost_empty(void)
{
	return fifo_cnt < 1024;
}

void
pcm_render(int16_t *buf, unsigned num_samples)
{
	while (num_samples--) {
		uint8_t old_phase = phase;
		phase += rate;
		if ((old_phase & 0x80) != (phase & 0x80)) {
			if (fifo_cnt == 0) {
				cur_l = 0;
				cur_r = 0;
			} else {
				switch ((ctrl >> 4) & 3) {
					case 0: { // mono 8-bit
						cur_l = (int16_t)read_fifo() << 8;
						cur_r = cur_l;
						break;
					}
					case 1: { // stereo 8-bit
						if (fifo_cnt < 2) {
							fifo_cnt = 0;
							fifo_rdidx = fifo_wridx;
						} else {
							cur_l = read_fifo() << 8;
							cur_r = read_fifo() << 8;
						}
						break;
					}
					case 2: { // mono 16-bit
						if (fifo_cnt < 2) {
							fifo_cnt = 0;
							fifo_rdidx = fifo_wridx;
						} else {
							cur_l = read_fifo();
							cur_l |= read_fifo() << 8;
							cur_r = cur_l;
						}
						break;
					}
					case 3: { // stereo 16-bit
						if (fifo_cnt < 4) {
							fifo_cnt = 0;
							fifo_rdidx = fifo_wridx;
						} else {
							cur_l = read_fifo();
							cur_l |= read_fifo() << 8;
							cur_r = read_fifo();
							cur_r |= read_fifo() << 8;
						}
						break;
					}
				}
				if (loop && fifo_cnt == 0) {
					fifo_restart();
				}
			}
		}
		*(buf++) = (int16_t)((int32_t)cur_l * volume_lut[ctrl & 0xF] / 64);
		*(buf++) = (int16_t)((int32_t)cur_r * volume_lut[ctrl & 0xF] / 64);

		if (scope_armed > 0) {
			scope_buf[PCM_SCOPE_L][scope_wridx] = buf[-2];
			scope_buf[PCM_SCOPE_R][scope_wridx] = buf[-1];
			scope_wridx = (scope_wridx + 1) & (PCM_SCOPE_SAMPLES - 1);
			scope_armed--;
		}
	}
}

// ─── Debugger accessors ─────────────────────────────────────────────────────

void
pcm_debug_get_state(struct pcm_debug_state *out)
{
	if (out == NULL) {
		return;
	}
	out->ctrl         = ctrl;
	out->rate         = rate;
	out->loop         = loop != 0;
	out->fifo_cnt     = fifo_cnt;
	out->fifo_rdidx   = fifo_rdidx;
	out->fifo_wridx   = fifo_wridx;
	out->fifo_size    = (unsigned)sizeof(fifo);
	out->cur_l        = cur_l;
	out->cur_r        = cur_r;
	out->phase        = phase;
	out->almost_empty = pcm_is_fifo_almost_empty();
}

unsigned
pcm_debug_peek_fifo(uint8_t *dest, unsigned offset, unsigned len)
{
	if (dest == NULL || offset >= fifo_cnt) {
		return 0;
	}
	unsigned avail = fifo_cnt - offset;
	if (len > avail) {
		len = avail;
	}
	for (unsigned i = 0; i < len; i++) {
		dest[i] = fifo[(fifo_rdidx + offset + i) % sizeof(fifo)];
	}
	return len;
}

unsigned
pcm_debug_scope_read(int side, int16_t *dest, unsigned max_samples)
{
	scope_armed = PCM_SCOPE_ARM_SAMPLES; // (re)arm: the debugger is watching

	if (side < 0 || side >= PCM_SCOPE_STREAMS || dest == NULL || max_samples == 0) {
		return 0;
	}

	unsigned n = (max_samples > PCM_SCOPE_SAMPLES) ? PCM_SCOPE_SAMPLES : max_samples;
	unsigned start = (scope_wridx + PCM_SCOPE_SAMPLES - n) & (PCM_SCOPE_SAMPLES - 1);
	for (unsigned i = 0; i < n; i++) {
		dest[i] = scope_buf[side][(start + i) & (PCM_SCOPE_SAMPLES - 1)];
	}
	return n;
}

unsigned
pcm_debug_scope_predict(int16_t *dest_l, int16_t *dest_r, unsigned num_samples)
{
	if (dest_l == NULL || dest_r == NULL || num_samples == 0) {
		return 0;
	}
	unsigned n = (num_samples > PCM_SCOPE_SAMPLES) ? PCM_SCOPE_SAMPLES : num_samples;

	// Everything pcm_render() mutates, saved so the projection leaves no trace.
	// ctrl/rate/loop are only touched by register writes, so they stay put.
	uint8_t  saved_fifo[PCM_FIFO_SIZE];
	memcpy(saved_fifo, fifo, sizeof(fifo));
	const unsigned saved_wridx = fifo_wridx;
	const unsigned saved_rdidx = fifo_rdidx;
	const unsigned saved_cnt   = fifo_cnt;
	const int16_t  saved_l     = cur_l;
	const int16_t  saved_r     = cur_r;
	const uint8_t  saved_phase = phase;
	const unsigned saved_armed = scope_armed;
	scope_armed = 0; // keep the projection out of the live capture ring

	// Render through the real path in chunks, so the waveform, the FIFO drain
	// order and the loop-restart behaviour all match live playback exactly.
	int16_t chunk[128 * 2];
	unsigned done = 0;
	while (done < n) {
		unsigned want = n - done;
		if (want > 128) {
			want = 128;
		}
		pcm_render(chunk, want);
		for (unsigned i = 0; i < want; i++) {
			dest_l[done + i] = chunk[i * 2];
			dest_r[done + i] = chunk[i * 2 + 1];
		}
		done += want;
	}

	memcpy(fifo, saved_fifo, sizeof(fifo));
	fifo_wridx  = saved_wridx;
	fifo_rdidx  = saved_rdidx;
	fifo_cnt    = saved_cnt;
	cur_l       = saved_l;
	cur_r       = saved_r;
	phase       = saved_phase;
	scope_armed = saved_armed;
	return n;
}
