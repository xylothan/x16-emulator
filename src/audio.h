// Commander X16 Emulator
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#pragma once

#include <SDL.h>
#include <stdbool.h>

#define AUDIO_SAMPLERATE (25000000 / 512)

void audio_init(const char *dev_name, int num_audio_buffers);
void audio_close(void);
// Clock the sound chips at a fixed rate whether or not a device opens (and with
// -sound none). Call before audio_init().
void audio_set_deterministic(bool on);
void audio_step(int cpu_clocks);
void audio_render();

void audio_usage(void);
