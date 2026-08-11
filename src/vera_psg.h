// Commander X16 Emulator
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void psg_reset(void);
void psg_writereg(uint8_t reg, uint8_t val);
void psg_render(int16_t *buf, unsigned num_samples);

// ─── Debugger accessors ─────────────────────────────────────────────────────
// Read-only views of the PSG for the ImGui debugger's PSG panel. All of these
// are side-effect free (apart from arming/clearing the debug-only capture state
// documented below) and are called from the emulator main thread — the same
// thread that runs psg_render() via audio_render() — so no locking is needed.

#define PSG_NUM_CHANNELS 16
#define PSG_NUM_REGS     (PSG_NUM_CHANNELS * 4)

// Waveform codes, register +3 bits 7-6.
enum psg_waveform {
	PSG_WF_PULSE = 0,
	PSG_WF_SAWTOOTH,
	PSG_WF_TRIANGLE,
	PSG_WF_NOISE,
};

struct psg_debug_channel {
	uint16_t freq;        // 16-bit frequency word
	uint8_t  vol_raw;     // volume as written (0-63)
	uint16_t vol_lut;     // volume after the 64-entry LUT (0-511)
	bool     left, right;
	uint8_t  pw;          // pulse width (Pulse) / wave-shaping mask (Saw, Tri)
	uint8_t  waveform;    // enum psg_waveform
	uint32_t phase;       // live 17-bit phase accumulator
	uint16_t noiseval;    // last latched noise sample (0-63)
	int16_t  last_sample; // last rendered sample, before L/R panning
	uint16_t peak;        // peak |sample| since the previous call (then reset)
};

// Snapshot one voice. `peak` reports the loudest sample since the previous call
// and is reset by it, which is exactly what a per-frame level meter wants.
// Returns false (and leaves *out untouched) for an out-of-range index.
bool psg_debug_get_channel(int ch, struct psg_debug_channel *out);

// Raw register byte as last written. VERA also mirrors these into video_ram at
// $1F9C0+reg, but that mirror survives psg_reset(), so prefer this shadow.
uint8_t psg_debug_get_reg(uint8_t reg);

// The shared 16-bit noise LFSR state.
uint16_t psg_debug_get_noise_state(void);

// ─── Scope capture ──────────────────────────────────────────────────────────
// Self-arming per-voice sample capture backing the debugger's oscilloscope.
// psg_debug_scope_read() (re)arms capture for roughly half a second; if nothing
// reads for that long, capture switches itself off again. psg_render() therefore
// costs nothing extra while the Scope tab is hidden, and the panels need no
// explicit enable/disable lifecycle.

#define PSG_SCOPE_SAMPLES  2048             // ring depth, ~42 ms at 48828 Hz
#define PSG_SCOPE_MIX_L    PSG_NUM_CHANNELS // summed PSG output, left
#define PSG_SCOPE_MIX_R    (PSG_NUM_CHANNELS + 1)
#define PSG_SCOPE_STREAMS  (PSG_NUM_CHANNELS + 2)

// Copy the most recent `max_samples` samples of stream `ch` (a voice index, or
// PSG_SCOPE_MIX_L/R) into `dest`, oldest first. All streams share one write
// index, so reads of different streams are time-aligned. Returns the sample
// count written; always (re)arms capture, even for an invalid stream.
unsigned psg_debug_scope_read(int ch, int16_t *dest, unsigned max_samples);

// Predict what the next `num_samples` would look like, from the current voice
// state, WITHOUT advancing the PSG. Fills every stream (indexed as above) of
// `dest` and returns the sample count written to each.
//
// This exists for the debugger: scope capture is self-arming and nothing renders
// audio while the machine is paused, so a scope opened at a breakpoint would
// otherwise have nothing to draw. The registers fully determine the waveform, so
// we can run the real renderer forward on the voice state and show what it is
// about to emit. It is a projection of the current registers, not captured
// output, so callers must label it as such.
//
// Uses the same render path as live audio, so the traces cannot drift from it.
//
// Implementation note for reviewers: this runs the live voice state forward and
// then rewinds it, rather than working on a copy. That is safe here because
//   * every field render() touches is saved and restored (the state is just
//     `channels` and the noise LFSR - there is no hidden or derived state), and
//   * psg_render() is only ever called from audio_render() on the main thread,
//     the same thread as the debugger, and not at all while paused.
// Nothing in the audio path observes the intermediate state.
unsigned psg_debug_scope_predict(int16_t (*dest)[PSG_SCOPE_SAMPLES], unsigned num_samples);

#ifdef __cplusplus
}
#endif
