// Commander X16 Emulator
// All rights reserved. License: 2-clause BSD

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the sound chips' registers mean, shared by the debugger panels and the
// debug servers so that both report the same numbers. Pure functions.

#define AUDIO_VERA_SAMPLE_HZ (25000000.0 / 512.0)
#define AUDIO_YM_CLOCK_HZ    3579545.0
#define AUDIO_YM_SAMPLE_HZ   (AUDIO_YM_CLOCK_HZ / 64.0)

// PSG: waveform name (Pulse, Saw, Tri, Noise) and a frequency word in Hz.
const char *audio_psg_wave_name(uint8_t waveform);
double      audio_psg_hz(uint16_t freq);

// Nearest note and its error in cents, such as "A4   +0 c"; "--" for 0 Hz.
void audio_note_from_hz(double hz, char *out, size_t outsz);

// YM2151: the note a key code and key fraction play. `kf` is register $30+ch as
// stored, so its value is in bits 7-2. An unused note code gives valid = false.
struct audio_ym_note {
	bool   valid;
	char   text[48];
	double hz;
};
void audio_ym_note_decode(uint8_t kc, uint8_t kf, struct audio_ym_note *out);

// Timer periods in milliseconds, from the 10-bit timer A and 8-bit timer B.
double audio_ym_timer_a_ms(uint16_t ta);
double audio_ym_timer_b_ms(uint8_t tb);

// PCM: the format in AUDIO_CTRL bits 5-4.
struct audio_pcm_format {
	const char *name;
	int         bytes_per_frame;
	bool        stereo;
	bool        bits16;
};
struct audio_pcm_format audio_pcm_format_decode(uint8_t ctrl);

// AUDIO_CTRL bits 3-0 through the volume table, in 64ths.
uint8_t audio_pcm_volume_lut(uint8_t ctrl);

// The rate accumulator is 8 bits and a sample fires when its bit 7 flips, so
// rate r and 256 - r fire equally often. 128 is the fastest.
unsigned audio_pcm_rate_step(uint8_t rate);
double   audio_pcm_rate_hz(uint8_t rate);

#ifdef __cplusplus
}
#endif
