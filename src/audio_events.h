// Commander X16 Emulator — what the program told the sound chips to play, for
// x16/step's "audio" option (debug_server.c).
//
// While a step that asked for it runs, every YM2151 key-on (a write to
// register $08 with an operator bit set), optionally every key-off, and every
// time the VERA PCM FIFO goes from empty to holding data is noted with the
// frame of the step it happened in. Only reads: the chips, the machine and its
// state hash are the same with it on or off.
//
// memory.c and video.c call in only while audio_events_on is set, so the cost
// when nobody asked is one flag test per YM data write or PCM FIFO write.

#ifndef AUDIO_EVENTS_H
#define AUDIO_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_EVENTS_MAX 512 // per step; more are counted, not kept

typedef struct {
	uint32_t frame;   // within the step, from 0
	uint32_t voice;   // hash of the channel's sound-defining registers at key-on
	uint8_t  channel; // 0-7
	uint8_t  ops;     // key-on operator mask, register $08 bits 3-6 shifted down: M1 C1 M2 C2
	uint8_t  kc, kf;  // register $28+ch, and $30+ch shifted down to the 6-bit KF
	bool     off;     // a key-off: ops is 0, and kc, kf and voice are not filled in
} audio_ym_event;

extern bool audio_events_on;

// Start recording, from frame 0, dropping what the last step recorded.
void audio_events_begin(bool key_offs);
void audio_events_end(void);
void audio_events_frame_end(void);

// From the chips' write paths, only while audio_events_on.
void audio_events_ym_write(uint8_t reg, uint8_t val); // a write the YM2151 accepted
void audio_events_pcm_start(void);                    // the FIFO was empty and is written

int                   audio_events_ym_count(void);
const audio_ym_event *audio_events_ym(int i);
uint32_t              audio_events_ym_dropped(void);
int                   audio_events_pcm_count(void);
uint32_t              audio_events_pcm_frame(int i);

// The voice hash of a channel as the chip's registers now stand: FNV-1a over
// $20+ch, then for each operator M1 M2 C1 C2 the bytes at $40 $60 $80 $A0 $C0
// $E0 + op*8 + ch.
uint32_t audio_events_voice(int channel);

#endif
