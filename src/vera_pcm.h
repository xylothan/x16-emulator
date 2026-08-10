// Commander X16 Emulator
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void    pcm_reset(void);
void    pcm_write_ctrl(uint8_t val);
uint8_t pcm_read_ctrl(void);
void    pcm_write_rate(uint8_t val);
uint8_t pcm_read_rate(void);
void    pcm_write_fifo(uint8_t val);
void    pcm_render(int16_t *buf, unsigned num_samples);
bool    pcm_is_fifo_almost_empty(void);

// ─── Debugger accessors ─────────────────────────────────────────────────────
// Read-only views of the VERA PCM channel for the ImGui debugger's PCM panel.
// Called from the emulator main thread — the same thread that runs pcm_render()
// via audio_render() — so no locking is needed.

#define PCM_FIFO_SIZE 4096

struct pcm_debug_state {
	uint8_t  ctrl;         // AUDIO_CTRL as stored (bits 5-0; format + volume)
	uint8_t  rate;         // effective rate divider after the >128 clamp
	bool     loop;
	unsigned fifo_cnt;     // bytes queued
	unsigned fifo_rdidx;
	unsigned fifo_wridx;
	unsigned fifo_size;    // PCM_FIFO_SIZE (usable fill is fifo_size - 1)
	int16_t  cur_l, cur_r; // last sample fetched from the FIFO
	uint8_t  phase;        // 8-bit playback phase accumulator
	bool     almost_empty; // the AFLOW IRQ condition (fifo_cnt < 1024)
};

void pcm_debug_get_state(struct pcm_debug_state *out);

// Copy up to `len` queued FIFO bytes into `dest`, starting `offset` bytes after
// the read pointer, without consuming anything. Handles the ring wrap and stops
// at the end of the queued data. Returns the number of bytes written.
unsigned pcm_debug_peek_fifo(uint8_t *dest, unsigned offset, unsigned len);

// ─── Scope capture ──────────────────────────────────────────────────────────
// Self-arming output capture, same contract as the PSG's (see vera_psg.h):
// reading (re)arms it for roughly half a second, so pcm_render() costs nothing
// extra while the debugger's Scope tab is hidden.

#define PCM_SCOPE_SAMPLES 2048
#define PCM_SCOPE_L       0
#define PCM_SCOPE_R       1
#define PCM_SCOPE_STREAMS 2

unsigned pcm_debug_scope_read(int side, int16_t *dest, unsigned max_samples);

// Predict the next `num_samples` of playback from the queued FIFO contents,
// WITHOUT consuming them. Returns the sample count written to each side.
//
// For the debugger: scope capture is self-arming and nothing renders audio while
// paused, so a scope opened at a breakpoint has an empty ring. The FIFO already
// holds the samples that are about to play, so we can show them. Runs the real
// pcm_render() over saved-and-restored state, so the rate stepping, FIFO drain
// order and loop restart all behave exactly as they will on resume.
//
// This is a projection of queued data, not captured output; label it as such.
//
// Implementation note for reviewers: this runs the live FIFO state forward and
// then rewinds it, rather than working on a copy. That is safe here because
//   * every field pcm_render() touches is saved and restored (the FIFO, its
//     three indices, the held sample pair and the rate phase - there is no
//     hidden or derived state), and
//   * pcm_render() is only ever called from audio_render() on the main thread,
//     the same thread as the debugger, and not at all while paused.
// Nothing in the audio path observes the intermediate state.
unsigned pcm_debug_scope_predict(int16_t *dest_l, int16_t *dest_r, unsigned num_samples);

#ifdef __cplusplus
}
#endif
