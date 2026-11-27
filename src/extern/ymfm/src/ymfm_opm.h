// BSD 3-Clause License
//
// Copyright (c) 2021, Aaron Giles
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef YMFM_OPM_H
#define YMFM_OPM_H

#pragma once

#include "ymfm.h"
#include "ymfm_fm.h"

namespace ymfm
{

//*********************************************************
//  REGISTER CLASSES
//*********************************************************

// ======================> opm_registers

//
// OPM register map:
//
//      System-wide registers:
//           01 xxxxxx-x Test register
//              ------x- LFO reset
//           08 -x------ Key on/off operator 4
//              --x----- Key on/off operator 3
//              ---x---- Key on/off operator 2
//              ----x--- Key on/off operator 1
//              -----xxx Channel select
//           0F x------- Noise enable
//              ---xxxxx Noise frequency
//           10 xxxxxxxx Timer A value (upper 8 bits)
//           11 ------xx Timer A value (lower 2 bits)
//           12 xxxxxxxx Timer B value
//           14 x------- CSM mode
//              --x----- Reset timer B
//              ---x---- Reset timer A
//              ----x--- Enable timer B
//              -----x-- Enable timer A
//              ------x- Load timer B
//              -------x Load timer A
//           18 xxxxxxxx LFO frequency
//           19 0xxxxxxx AM LFO depth
//              1xxxxxxx PM LFO depth
//           1B xx------ CT (2 output data lines)
//              ------xx LFO waveform
//
//     Per-channel registers (channel in address bits 0-2)
//        20-27 x------- Pan right
//              -x------ Pan left
//              --xxx--- Feedback level for operator 1 (0-7)
//              -----xxx Operator connection algorithm (0-7)
//        28-2F -xxxxxxx Key code
//        30-37 xxxxxx-- Key fraction
//        38-3F -xxx---- LFO PM sensitivity
//              ------xx LFO AM shift
//
//     Per-operator registers (channel in address bits 0-2, operator in bits 3-4)
//        40-5F -xxx---- Detune value (0-7)
//              ----xxxx Multiple value (0-15)
//        60-7F -xxxxxxx Total level (0-127)
//        80-9F xx------ Key scale rate (0-3)
//              ---xxxxx Attack rate (0-31)
//        A0-BF x------- LFO AM enable
//              ---xxxxx Decay rate (0-31)
//        C0-DF xx------ Detune 2 value (0-3)
//              ---xxxxx Sustain rate (0-31)
//        E0-FF xxxx---- Sustain level (0-15)
//              ----xxxx Release rate (0-15)
//
//     Internal (fake) registers:
//           1A -xxxxxxx PM depth
//

class opm_registers : public fm_registers_base
{
	// LFO waveforms are 256 entries long
	static constexpr uint32_t LFO_WAVEFORM_LENGTH = 256;

public:
	// constants
	static constexpr uint32_t OUTPUTS = 2;
	static constexpr uint32_t CHANNELS = 8;
	static constexpr uint32_t ALL_CHANNELS = (1 << CHANNELS) - 1;
	static constexpr uint32_t OPERATORS = CHANNELS * 4;
	static constexpr uint32_t WAVEFORMS = 1;
	static constexpr uint32_t REGISTERS = 0x100;
	static constexpr uint32_t DEFAULT_PRESCALE = 2;
	static constexpr uint32_t EG_CLOCK_DIVIDER = 3;
	static constexpr uint32_t CSM_TRIGGER_MASK = ALL_CHANNELS;
	static constexpr bool LFO_AM_SPLIT = true;
	static constexpr uint32_t REG_MODE = 0x14;
	static constexpr uint8_t STATUS_TIMERA = 0x01;
	static constexpr uint8_t STATUS_TIMERB = 0x02;
	static constexpr uint8_t STATUS_BUSY = 0x80;
	static constexpr uint8_t STATUS_IRQ = 0;

	// constructor
	opm_registers();

	// reset to initial state
	void reset();

	// save/restore
	void save_restore(ymfm_saved_state &state);

	// map channel number to register offset
	static constexpr uint32_t channel_offset(uint32_t chnum)
	{
		assert(chnum < CHANNELS);
		return chnum;
	}

	// map operator number to register offset
	static constexpr uint32_t operator_offset(uint32_t opnum)
	{
		assert(opnum < OPERATORS);
		return opnum;
	}

	// each envelope steps every sample from the state the sample began with,
	// and the output reads it as it was before the step (checked against
	// Nuked-OPM)
	static constexpr bool envelope_by_sample() { return true; }

	// operator 3 (M2) runs before operator 2 (C1) in the sample, so it takes
	// C1's output from the sample before
	static constexpr bool op3_takes_last_op2() { return true; }

	// the mixer takes slots 15-31 into the left output, and slot 31 into the
	// right, a sample after the others
	static constexpr bool output_late(uint32_t opoffs, uint32_t output)
	{
		return (output == 0) ? opoffs >= 15 : opoffs == 31;
	}

	// return an array of operator indices for each channel
	struct operator_mapping { uint32_t chan[CHANNELS]; };
	void operator_map(operator_mapping &dest) const;

	// handle writes to the register array
	bool write(uint16_t index, uint8_t data, uint32_t &chan, uint32_t &opmask);

	// clock the noise and LFO, if present, returning LFO PM value
	int32_t clock_noise_and_lfo();

	// clock the noise generator by one sample
	void clock_noise();

	// a noise generator: LFSR (17 bits) and timer (bits 0-4) with its last
	// two matches (bits 5-6)
	struct noise_gen
	{
		uint32_t lfsr;
		uint8_t counter;
	};
	static uint32_t step_noise(noise_gen &gen, uint32_t before, uint32_t after, uint32_t change, uint32_t &bits);

	// the noise state save_restore() leaves out, for a host's own image
	noise_gen &noise_ahead() { return m_noise_ahead; }
	uint64_t &noise_history() { return m_noise_history; }
	uint32_t &noise_old_target() { return m_noise_old_target; }
	uint32_t &noise_write_cycle() { return m_noise_write_cycle; }

	// start the noise generator over, as at reset
	void reset_noise();

	// return the AM offset from LFO for the given channel
	uint32_t lfo_am_offset(uint32_t choffs) const;

	// the LFO as the chip runs it (checked against the Nuked-OPM vectors): a counter
	// stepped by a reload counter and a rate multiplier, and AM and PM values
	// latched once every 8 samples; the later operators of a sample take each
	// new value a sample before the earlier ones
	struct lfo_state
	{
		uint16_t count;      // waveform counter
		uint16_t ticks;      // reload ticks left to the next step (1-32768)
		uint8_t rate_steps;  // rate multiplier counter (4 bits)
		uint8_t window;      // sample within the 8-sample window, from reset
		uint8_t pm;          // PM latch for operators 0-22: sign (bit 7), magnitude
		uint8_t pm_late;     // PM latch for operators 23-31
		uint8_t am_late;     // AM latch for operators 16-31
		uint8_t steps;       // steps due at the next window start
		bool reload;         // a $18 write is due after this window's tick
	};
	lfo_state &lfo() { return m_lfo; }
	lfo_state const &lfo() const { return m_lfo; }

	// the cycle (0-31) within the coming sample at which the following
	// register writes reach the chip
	void set_write_cycle(uint32_t cycle) { m_write_cycle = cycle; }

	// return the current noise state, gated by the noise clock
	uint32_t noise_state() const { return m_noise_state; }

	// LFO values from the counter
	uint8_t lfo_am_value(uint32_t waveform, uint32_t count) const;
	uint8_t lfo_pm_value(uint32_t waveform, uint32_t count) const;
	static constexpr uint32_t LFO_RELOAD_CYCLE = 27;
	static constexpr uint32_t NOISE_AHEAD = 3;

	// caching helpers
	void cache_operator_data(uint32_t choffs, uint32_t opoffs, opdata_cache &cache);

	// compute the phase step, given a PM value
	uint32_t compute_phase_step(uint32_t choffs, uint32_t opoffs, opdata_cache const &cache, int32_t lfo_raw_pm);

	// log a key-on event
	std::string log_keyon(uint32_t choffs, uint32_t opoffs);

	// system-wide registers
	uint32_t test() const                            { return byte(0x01, 0, 8); }
	uint32_t lfo_reset() const                       { return byte(0x01, 1, 1); }
	uint32_t noise_frequency() const                 { return byte(0x0f, 0, 5) ^ 0x1f; }
	uint32_t noise_enable() const                    { return byte(0x0f, 7, 1); }
	uint32_t timer_a_value() const                   { return word(0x10, 0, 8, 0x11, 0, 2); }
	uint32_t timer_b_value() const                   { return byte(0x12, 0, 8); }
	uint32_t csm() const                             { return byte(0x14, 7, 1); }
	uint32_t reset_timer_b() const                   { return byte(0x14, 5, 1); }
	uint32_t reset_timer_a() const                   { return byte(0x14, 4, 1); }
	uint32_t enable_timer_b() const                  { return byte(0x14, 3, 1); }
	uint32_t enable_timer_a() const                  { return byte(0x14, 2, 1); }
	uint32_t load_timer_b() const                    { return byte(0x14, 1, 1); }
	uint32_t load_timer_a() const                    { return byte(0x14, 0, 1); }
	uint32_t lfo_rate() const                        { return byte(0x18, 0, 8); }
	uint32_t lfo_am_depth() const                    { return byte(0x19, 0, 7); }
	uint32_t lfo_pm_depth() const                    { return byte(0x1a, 0, 7); }
	uint32_t output_bits() const                     { return byte(0x1b, 6, 2); }
	uint32_t lfo_waveform() const                    { return byte(0x1b, 0, 2); }

	// per-channel registers
	uint32_t ch_output_any(uint32_t choffs) const    { return byte(0x20, 6, 2, choffs); }
	uint32_t ch_output_0(uint32_t choffs) const      { return byte(0x20, 6, 1, choffs); }
	uint32_t ch_output_1(uint32_t choffs) const      { return byte(0x20, 7, 1, choffs); }
	uint32_t ch_output_2(uint32_t choffs) const      { return 0; }
	uint32_t ch_output_3(uint32_t choffs) const      { return 0; }
	uint32_t ch_feedback(uint32_t choffs) const      { return byte(0x20, 3, 3, choffs); }
	uint32_t ch_algorithm(uint32_t choffs) const     { return byte(0x20, 0, 3, choffs); }
	uint32_t ch_block_freq(uint32_t choffs) const    { return word(0x28, 0, 7, 0x30, 2, 6, choffs); }
	uint32_t ch_lfo_pm_sens(uint32_t choffs) const   { return byte(0x38, 4, 3, choffs); }
	uint32_t ch_lfo_am_sens(uint32_t choffs) const   { return byte(0x38, 0, 2, choffs); }

	// per-operator registers
	uint32_t op_detune(uint32_t opoffs) const        { return byte(0x40, 4, 3, opoffs); }
	uint32_t op_multiple(uint32_t opoffs) const      { return byte(0x40, 0, 4, opoffs); }
	uint32_t op_total_level(uint32_t opoffs) const   { return byte(0x60, 0, 7, opoffs); }
	uint32_t op_ksr(uint32_t opoffs) const           { return byte(0x80, 6, 2, opoffs); }
	uint32_t op_attack_rate(uint32_t opoffs) const   { return byte(0x80, 0, 5, opoffs); }
	uint32_t op_lfo_am_enable(uint32_t opoffs) const { return byte(0xa0, 7, 1, opoffs); }
	uint32_t op_decay_rate(uint32_t opoffs) const    { return byte(0xa0, 0, 5, opoffs); }
	uint32_t op_detune2(uint32_t opoffs) const       { return byte(0xc0, 6, 2, opoffs); }
	uint32_t op_sustain_rate(uint32_t opoffs) const  { return byte(0xc0, 0, 5, opoffs); }
	uint32_t op_sustain_level(uint32_t opoffs) const { return byte(0xe0, 4, 4, opoffs); }
	uint32_t op_release_rate(uint32_t opoffs) const  { return byte(0xe0, 0, 4, opoffs); }

protected:
	// return a bitfield extracted from a byte
	uint32_t byte(uint32_t offset, uint32_t start, uint32_t count, uint32_t extra_offset = 0) const
	{
		return bitfield(m_regdata[offset + extra_offset], start, count);
	}

	// return a bitfield extracted from a pair of bytes, MSBs listed first
	uint32_t word(uint32_t offset1, uint32_t start1, uint32_t count1, uint32_t offset2, uint32_t start2, uint32_t count2, uint32_t extra_offset = 0) const
	{
		return (byte(offset1, start1, count1, extra_offset) << count2) | byte(offset2, start2, count2, extra_offset);
	}

	// internal state
	uint32_t m_lfo_counter;               // LFO counter
	uint32_t m_noise_lfsr;                // noise LFSR state
	uint8_t m_noise_counter;              // noise counter
	uint8_t m_noise_state;                // latched noise state
	uint64_t m_noise_history;             // in-time low bit per cycle, last two samples
	noise_gen m_noise_ahead;              // the generator NOISE_AHEAD samples on
	uint32_t m_noise_old_target;          // the target before a pending $0F write
	uint32_t m_noise_write_cycle;         // cycle of a pending $0F write, 32 if none
	uint8_t m_lfo_am;                     // AM latch for operators 0-15
	lfo_state m_lfo;                      // LFO counters and PM latch
	uint32_t m_write_cycle;               // cycle of the writes being made
	uint8_t m_regdata[REGISTERS];         // register data
	int16_t m_lfo_waveform[4][LFO_WAVEFORM_LENGTH]; // LFO waveforms; AM in low 8, PM in upper 8
	uint16_t m_waveform[WAVEFORMS][WAVEFORM_LENGTH]; // waveforms
};



//*********************************************************
//  OPM IMPLEMENTATION CLASSES
//*********************************************************

// ======================> ym2151

class ym2151
{
public:
	using fm_engine = fm_engine_base<opm_registers>;
	using output_data = fm_engine::output_data;
	static constexpr uint32_t OUTPUTS = fm_engine::OUTPUTS;

	// constructor
	ym2151(ymfm_interface &intf) : ym2151(intf, VARIANT_YM2151) { }

	// reset
	void reset();

	// save/restore
	void save_restore(ymfm_saved_state &state);

	// pass-through helpers
	uint32_t sample_rate(uint32_t input_clock) const { return m_fm.sample_rate(input_clock); }
	void invalidate_caches() { m_fm.invalidate_caches(); }

	// read access
	uint8_t read_status();
	uint8_t read(uint32_t offset);

	// write access
	void write_address(uint8_t data);
	void write_data(uint8_t data);
	void write(uint32_t offset, uint8_t data);

	// generate one sample of sound
	void generate(output_data *output, uint32_t numsamples = 1);

protected:
	// variants
	enum opm_variant
	{
		VARIANT_YM2151,
		VARIANT_YM2164
	};

	// internal constructor
	ym2151(ymfm_interface &intf, opm_variant variant);

	// internal state
	opm_variant m_variant;           // chip variant
	uint8_t m_address;               // address register
	fm_engine m_fm;                  // core FM engine
};



//*********************************************************
//  OPP IMPLEMENTATION CLASSES
//*********************************************************

// ======================> ym2164

// the YM2164 is almost 100% functionally identical to the YM2151, except
// it apparently has some mystery registers in the 00-07 range, and timer
// B's frequency is half that of the 2151
class ym2164 : public ym2151
{
public:
	// constructor
	ym2164(ymfm_interface &intf) : ym2151(intf, VARIANT_YM2164) { }
};

}


#endif // YMFM_OPM_H
