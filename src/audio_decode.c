// Commander X16 Emulator
// All rights reserved. License: 2-clause BSD

#include "audio_decode.h"

#include <math.h>
#include <stdio.h>

#define PSG_PHASE_STEPS 131072.0

const char *
audio_psg_wave_name(uint8_t waveform)
{
	static const char *const names[] = { "Pulse", "Saw", "Tri", "Noise" };
	return names[waveform & 3];
}

double
audio_psg_hz(uint16_t freq)
{
	return (double)freq * AUDIO_VERA_SAMPLE_HZ / PSG_PHASE_STEPS;
}

void
audio_note_from_hz(double hz, char *out, size_t outsz)
{
	if (hz <= 0.0) {
		snprintf(out, outsz, "--");
		return;
	}

	static const char *const names[] = {
		"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
	};
	const double semis = 69.0 + 12.0 * log(hz / 440.0) / log(2.0);
	const int    note  = (int)floor(semis + 0.5);
	const int    idx   = ((note % 12) + 12) % 12;
	const int    oct   = note / 12 - 1;
	int cents = (int)floor((semis - (double)note) * 100.0 + ((semis >= (double)note) ? 0.5 : -0.5));
	// Pad name+octave to a constant width so the cents value stays put.
	char nb[16];
	snprintf(nb, sizeof nb, "%s%d", names[idx], oct);
	snprintf(out, outsz, "%-4s %+3d c", nb, cents);
}

void
audio_ym_note_decode(uint8_t kc, uint8_t kf, struct audio_ym_note *out)
{
	static const char *const names[12] = { "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "C" };
	const int oct  = kc >> 4;
	const int low  = kc & 0x0f;
	const int semi = (kc & 3) + 3 * ((kc >> 2) & 3);
	out->valid = (low != 3 && low != 7 && low != 11 && low != 15);
	if (!out->valid) {
		out->hz = 0.0;
		snprintf(out->text, sizeof(out->text), "invalid KC %02X", kc);
		return;
	}
	const double fine_semi = (double)(kf >> 2) / 64.0;
	const double midi = 12.0 * (double)(oct + 1) + 1.0 + (double)semi + fine_semi;
	out->hz = 440.0 * pow(2.0, (midi - 69.0) / 12.0);
	snprintf(out->text, sizeof(out->text), "%s%d %+d c  %.2f Hz", names[semi], oct,
	         (int)(fine_semi * 100.0 + 0.5), out->hz);
}

double
audio_ym_timer_a_ms(uint16_t ta)
{
	return (1024.0 - (double)ta) / AUDIO_YM_SAMPLE_HZ * 1000.0;
}

double
audio_ym_timer_b_ms(uint8_t tb)
{
	return (256.0 - (double)tb) * 16.0 / AUDIO_YM_SAMPLE_HZ * 1000.0;
}

struct audio_pcm_format
audio_pcm_format_decode(uint8_t ctrl)
{
	static const struct audio_pcm_format formats[4] = {
		{ "mono 8-bit", 1, false, false },
		{ "stereo 8-bit", 2, true, false },
		{ "mono 16-bit", 2, false, true },
		{ "stereo 16-bit", 4, true, true },
	};
	return formats[(ctrl >> 4) & 3];
}

uint8_t
audio_pcm_volume_lut(uint8_t ctrl)
{
	static const uint8_t lut[16] = { 0, 1, 2, 3, 4, 5, 6, 8, 11, 14, 18, 23, 30, 38, 49, 64 };
	return lut[ctrl & 0x0f];
}

unsigned
audio_pcm_rate_step(uint8_t rate)
{
	return (rate > 128) ? 256u - rate : rate;
}

double
audio_pcm_rate_hz(uint8_t rate)
{
	return AUDIO_VERA_SAMPLE_HZ * (double)audio_pcm_rate_step(rate) / 128.0;
}
