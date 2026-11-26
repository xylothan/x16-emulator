#ifndef YMGLUE_H
#define YMGLUE_H

#ifdef __cplusplus
extern "C" {
#endif
	#include <stdint.h>
	#include <stdbool.h>
	#include "voice_mute.h"

	uint8_t YM_read_status(void);
	void YM_Create(int clock);
	void YM_init(int sample_rate, int frame_rate);
	// `output` receives the machine's mix of every channel, `host_output` the
	// same without the muted ones. Each sample is the last of 32 interface
	// cycles (two phiM clocks each).
	void YM_stream_update(uint16_t* output, uint16_t* host_output, uint32_t numsamples);
	// Runs the interface to `cycle` (0-31) of the sample not yet generated,
	// so that accesses land between the cycles where they fall.
	void YM_sync(uint8_t cycle);
	// Writes `val` to port 0 (address) or 1 (data). The chip takes no write
	// while busy is checked: none is ever refused. Returns true when the byte
	// overwrites one the chip had not read yet.
	bool YM_write(uint8_t port, uint8_t val);
	bool YM_irq(void);
	// Power-on state, for tests.
	void YM_reset(void);
	// Called with each data write the chip loses: a new address, or new data,
	// came before the slot scan reached its register.
	void YM_set_lost_write_handler(void (*handler)(uint8_t reg, uint8_t val));

	// ─── Debugger accessors ─────────────────────────────────────────────────
	// Read-only views of the YM2151 for the ImGui debugger's FM panel. Called
	// from the emulator main thread — the same thread that runs
	// YM_stream_update() via audio_render() — so no locking is needed.

	#define YM_NUM_CHANNELS  8
	#define YM_NUM_OPERATORS (YM_NUM_CHANNELS * 4)
	#define YM_NUM_REGS      0x100

	// ymfm envelope_state values (ymfm.h). Only these four occur on the OPM.
	enum ym_eg_state {
		YM_EG_ATTACK  = 1,
		YM_EG_DECAY   = 2,
		YM_EG_SUSTAIN = 3,
		YM_EG_RELEASE = 4,
	};

	struct ym_debug_operator {
		uint8_t  eg_state;       // enum ym_eg_state
		uint16_t eg_attenuation; // 0 (loudest) .. 0x3FF (silent)
	};

	// Register byte as last *accepted* by the chip. YM_write_reg drops writes
	// that arrive while the chip is busy, and those are not shadowed either, so
	// this reflects what the chip actually holds.
	uint8_t YM_debug_get_reg(uint8_t reg);

	// Live key-on state for a channel, as a mask of operator SLOTS:
	// bit 0 = M1, 1 = M2, 2 = C1, 3 = C2 — the same order used to address the
	// $40-$FF per-operator registers (base + slot*8 + channel).
	// NOTE: this is deliberately NOT the bit order of the $08 key-on register,
	// whose bits 3-6 select operators as M1, C1, M2, C2. The glue normalizes
	// that quirk so callers can index key-on the same way they index registers.
	uint8_t YM_debug_get_keyon(int ch);

	// Live envelope state of one of the 32 operators. `op` is the ymfm operator
	// index: channel + 8 * slot, matching the $40-$FF register offsets. Returns
	// false (leaving *out untouched) for an out-of-range index.
	bool YM_debug_get_operator(int op, struct ym_debug_operator *out);

	// Status byte without the read side effects of the CPU-visible port, and
	// the busy state tracked by the glue's busy timer.
	uint8_t YM_debug_get_status(void);
	bool    YM_debug_is_busy(void);

	// The LFO's AM and PM depths as the chip holds them. Both are written
	// through $19, so the register shadow keeps only the later of the two.
	void    YM_debug_get_lfo_depths(uint8_t *am, uint8_t *pm);

	// Host-output mute, one bit per channel (bit n = channel n). The chip runs
	// every channel exactly as it would otherwise and they all stay in the mix
	// the machine holds; muted ones are only left out of the mix sent to the
	// host (and so out of the scope's mix streams and any WAV recording).
	// Per-channel scope traces still show them. A debugger setting, not machine
	// state: save states neither store nor restore it.
	//
	// set/get_mute_mask are the debugger UI's own selection. Each debug client
	// has a separate mask; what is muted is the OR of them all.
	void    YM_debug_set_mute_mask(uint8_t mask);
	uint8_t YM_debug_get_mute_mask(void);
	void    YM_debug_set_owner_mute_mask(voice_mute_owner_t owner, uint8_t mask);
	uint8_t YM_debug_get_owner_mute_mask(voice_mute_owner_t owner);
	uint8_t YM_debug_get_client_mute_mask(void);    // every client's, ORed
	uint8_t YM_debug_get_effective_mute_mask(void); // UI and clients, ORed

	// ─── Scope capture ──────────────────────────────────────────────────────
	// Self-arming per-channel capture, same contract as the PSG's (see
	// vera_psg.h): reading (re)arms it for roughly half a second. This one
	// matters most — while armed, each sample costs one extra ymfm output()
	// call per channel — so it really does need to switch itself off.

	#define YM_SCOPE_SAMPLES 2048
	#define YM_SCOPE_MIX_L   YM_NUM_CHANNELS // summed YM output, left
	#define YM_SCOPE_MIX_R   (YM_NUM_CHANNELS + 1)
	#define YM_SCOPE_STREAMS (YM_NUM_CHANNELS + 2)

	// Copy the most recent `max_samples` samples of stream `ch` (a channel
	// index, or YM_SCOPE_MIX_L/R) into `dest`, oldest first. All streams share
	// one write index, so reads are time-aligned. Returns the sample count
	// written; always (re)arms capture, even for an invalid stream.
	unsigned YM_debug_scope_read(int ch, int16_t* dest, unsigned max_samples);

	// Predict the next `num_samples` from the current chip state WITHOUT
	// advancing it. Fills every stream of `dest`; returns the count written.
	//
	// For the debugger: capture is self-arming and nothing renders audio while
	// paused, so a scope opened at a breakpoint has an empty ring. Envelopes,
	// LFO and operator phase are all live chip state, so we can run the chip
	// forward over a ymfm save-state round-trip and show what it is about to
	// emit, then put it back exactly as it was.
	//
	// This is a projection, not captured output; label it as such.
	unsigned YM_debug_scope_predict(int16_t (*dest)[YM_SCOPE_SAMPLES], unsigned num_samples);

#ifdef __cplusplus
}
#endif

#endif
