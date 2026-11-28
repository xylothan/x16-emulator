// The sound chips stepped in two parts within an instruction: up to an access
// by audio_catch_up(), the rest by audio_step_rest(). The parts add up to the
// whole, and the YM is synced to the access's own cycle.

#include "../src/audio.c"

#include "support/harness.h"

uint8_t MHZ = 8;
bool    input_log_seeking = false;

static int     syncs;
static uint8_t last_sync;

void YM_Create(int clock) { (void)clock; }
void YM_init(int sample_rate, int frame_rate) { (void)sample_rate; (void)frame_rate; }
void YM_stream_update(uint16_t *output, uint16_t *host_output, uint32_t numsamples)
{
	(void)output; (void)host_output; (void)numsamples;
}
void YM_sync(uint8_t cycle) { last_sync = cycle; syncs++; }
uint8_t YM_debug_get_effective_mute_mask(void) { return 0; }
void psg_render_split(int16_t *buf, int16_t *host_buf, unsigned num_samples)
{
	(void)buf; (void)host_buf; (void)num_samples;
}
uint16_t psg_debug_get_effective_mute_mask(void) { return 0; }
void pcm_render(int16_t *buf, unsigned num_samples) { (void)buf; (void)num_samples; }
void midi_synth_render(int16_t *buf, int len) { (void)buf; (void)len; }
void wav_recorder_process(const int16_t *samples, const int num_samples) { (void)samples; (void)num_samples; }

static void
start(void)
{
	audio_set_deterministic(true);
	audio_init("none", 8);
	step_consumed = 0;
}

static void
test_parts_add_up(void)
{
	start();
	audio_step(9);
	const uint32_t whole = ym_samp_pos_hd;

	start();
	audio_catch_up(4);
	audio_catch_up(6);
	audio_step_rest(9);
	check_eq(ym_samp_pos_hd, whole, "catch-ups to 4 and 6, then the rest of 9");
}

static void
test_rest_never_steps_back(void)
{
	start();
	audio_catch_up(7);
	const uint32_t at = ym_samp_pos_hd;
	audio_catch_up(2);
	audio_step_rest(7);
	check_eq(ym_samp_pos_hd, at, "a catch-up to an earlier cycle and a rest of 7 after 7");
}

static void
test_rest_restarts_the_count(void)
{
	start();
	audio_catch_up(5);
	audio_step_rest(5);
	const uint32_t at = ym_samp_pos_hd;
	audio_catch_up(5);
	check(ym_samp_pos_hd != at, "the next instruction's catch-up counts from its own start");
}

// 64 CPU cycles at 8 MHz are 0.45 of a YM sample.
static void
test_ym_synced_to_the_access(void)
{
	start();
	audio_step(64);
	audio_render();
	const uint8_t want = last_sync;

	start();
	audio_catch_up(64);
	syncs = 0;
	audio_render();
	check_eq(syncs, 1, "a render syncs the YM once");
	check_eq(last_sync, want, "the YM is synced to the access's cycle");
	check(want != 0, "64 cycles in is part way through a sample");
}

int
main(void)
{
	test_parts_add_up();
	test_rest_never_steps_back();
	test_rest_restarts_the_count();
	test_ym_synced_to_the_access();
	audio_close();
	return x16_test_summary("audio_catch_up");
}
