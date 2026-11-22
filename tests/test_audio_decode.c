// Register decoding shared by the audio panels and debug servers.
//
// PSG: VERA Programmer's Reference, "Programmable Sound Generator".
// YM2151: datasheet register map; KC $4A is A4 (440 Hz).
// PCM: VERA Programmer's Reference, AUDIO_CTRL and AUDIO_RATE.

#include "support/harness.h"

#include "audio_decode.h"

#include <math.h>
#include <string.h>

static bool
near(double got, double want)
{
	return fabs(got - want) < 0.001;
}

static void
test_psg(void)
{
	check(!strcmp(audio_psg_wave_name(0), "Pulse"), "waveform 0 is Pulse");
	check(!strcmp(audio_psg_wave_name(1), "Saw"), "waveform 1 is Saw");
	check(!strcmp(audio_psg_wave_name(2), "Tri"), "waveform 2 is Tri");
	check(!strcmp(audio_psg_wave_name(3), "Noise"), "waveform 3 is Noise");
	check(near(audio_psg_hz(0), 0.0), "frequency word 0 is 0 Hz");
	check(near(audio_psg_hz(0x0400), 381.4697265625), "frequency word $0400 in Hz");
}

static void
test_note_from_hz(void)
{
	char s[32];
	audio_note_from_hz(440.0, s, sizeof s);
	check(!strcmp(s, "A4    +0 c"), "440 Hz is A4");
	audio_note_from_hz(0.0, s, sizeof s);
	check(!strcmp(s, "--"), "0 Hz has no note");
}

static void
test_ym_note(void)
{
	struct audio_ym_note n;
	audio_ym_note_decode(0x4A, 0x00, &n);
	check(n.valid && near(n.hz, 440.0), "KC $4A is 440 Hz");
	check(!strcmp(n.text, "A4 +0 c  440.00 Hz"), "KC $4A text");
	audio_ym_note_decode(0x4A, 0x80, &n);
	check(near(n.hz, 440.0 * pow(2.0, 0.5 / 12.0)), "KF is in bits 7-2, 64ths of a semitone");
	audio_ym_note_decode(0x43, 0x00, &n);
	check(!n.valid && n.hz == 0.0, "note code 3 is unused");
}

static void
test_ym_timers(void)
{
	check(near(audio_ym_timer_a_ms(0), 1024.0 * 64.0 / 3579545.0 * 1000.0), "timer A period");
	check(near(audio_ym_timer_a_ms(1023), 64.0 / 3579545.0 * 1000.0), "timer A at its shortest");
	check(near(audio_ym_timer_b_ms(0), 256.0 * 16.0 * 64.0 / 3579545.0 * 1000.0), "timer B period");
}

static void
test_pcm(void)
{
	struct audio_pcm_format f = audio_pcm_format_decode(0x00);
	check(!strcmp(f.name, "mono 8-bit") && f.bytes_per_frame == 1, "format 0");
	f = audio_pcm_format_decode(0x1F);
	check(!strcmp(f.name, "stereo 8-bit") && f.bytes_per_frame == 2 && f.stereo, "format 1");
	f = audio_pcm_format_decode(0x20);
	check(!strcmp(f.name, "mono 16-bit") && f.bytes_per_frame == 2 && f.bits16, "format 2");
	f = audio_pcm_format_decode(0x3F);
	check(!strcmp(f.name, "stereo 16-bit") && f.bytes_per_frame == 4, "format 3");
	check_eq(audio_pcm_volume_lut(0x00), 0, "volume 0");
	check_eq(audio_pcm_volume_lut(0x37), 8, "volume 7, other bits ignored");
	check_eq(audio_pcm_volume_lut(0x0F), 64, "volume 15");
	check_eq(audio_pcm_rate_step(128), 128, "rate 128 is the fastest");
	check_eq(audio_pcm_rate_step(200), 56, "rate 200 plays as 56");
	check(near(audio_pcm_rate_hz(128), 25000000.0 / 512.0), "rate 128 in Hz");
	check(near(audio_pcm_rate_hz(0), 0.0), "rate 0 is stopped");
}

int
main(void)
{
	test_psg();
	test_note_from_hz();
	test_ym_note();
	test_ym_timers();
	test_pcm();
	return x16_test_summary("audio_decode");
}
