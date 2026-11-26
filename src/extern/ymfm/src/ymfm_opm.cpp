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

#include "ymfm_opm.h"
#include "ymfm_fm.ipp"

namespace ymfm
{

//*********************************************************
//  OPM REGISTERS
//*********************************************************

//-------------------------------------------------
//  opm_registers - constructor
//-------------------------------------------------

opm_registers::opm_registers() :
	m_lfo_counter(0),
	m_noise_lfsr(1),
	m_noise_counter(0),
	m_noise_state(0),
	m_noise_history(0),
	m_noise_ahead{ 0, 0 },
	m_noise_old_target(0),
	m_noise_write_cycle(32),
	m_lfo_am(0),
	m_lfo{ 0, 32768, 0, 0, 0, 0, 0, 0, false },
	m_write_cycle(0)
{
	// create the waveforms
	for (uint32_t index = 0; index < WAVEFORM_LENGTH; index++)
		m_waveform[0][index] = abs_sin_attenuation(index) | (bitfield(index, 9) << 15);

	// create the LFO waveforms; AM in the low 8 bits, PM in the upper 8
	// waveforms are adjusted to match the pictures in the application manual
	for (uint32_t index = 0; index < LFO_WAVEFORM_LENGTH; index++)
	{
		// waveform 0 is a sawtooth
		uint8_t am = index ^ 0xff;
		uint8_t pm = index;
		m_lfo_waveform[0][index] = am | (pm << 8);

		// waveform 1 is a square wave
		am = bitfield(index, 7) ? 0 : 0xff;
		pm = am ^ 0x80;
		m_lfo_waveform[1][index] = am | (pm << 8);

		// waveform 2 is a triangle wave
		am = bitfield(index, 7) ? (index << 1) : ((index ^ 0xff) << 1);
		pm = bitfield(index, 6) ? am : ~am;
		m_lfo_waveform[2][index] = am | (pm << 8);

		// waveform 3 is noise; it is filled in dynamically
		m_lfo_waveform[3][index] = 0;
	}
}


//-------------------------------------------------
//  reset - reset to initial state
//-------------------------------------------------

void opm_registers::reset()
{
	std::fill_n(&m_regdata[0], REGISTERS, 0);

	// enable output on both channels by default
	m_regdata[0x20] = m_regdata[0x21] = m_regdata[0x22] = m_regdata[0x23] = 0xc0;
	m_regdata[0x24] = m_regdata[0x25] = m_regdata[0x26] = m_regdata[0x27] = 0xc0;

	m_lfo = lfo_state{ 0, 32768, 0, 0, 0, 0, 0, 0, false };
	m_lfo_am = 0;
	reset_noise();
}


//-------------------------------------------------
//  reset_noise - start the noise generator over
//-------------------------------------------------

void opm_registers::reset_noise()
{
	m_noise_lfsr = 0;
	m_noise_counter = 0;
	m_noise_history = 0;
	m_noise_ahead = noise_gen{ 0, 0 };
	m_noise_old_target = 0;
	m_noise_write_cycle = 32;
	uint32_t bits;
	for (uint32_t sample = 0; sample < NOISE_AHEAD; sample++)
		m_noise_state = uint8_t(step_noise(m_noise_ahead, noise_frequency(), noise_frequency(), 0, bits));
}


//-------------------------------------------------
//  save_restore - save or restore the data
//-------------------------------------------------

void opm_registers::save_restore(ymfm_saved_state &state)
{
	state.save_restore(m_lfo_counter);
	state.save_restore(m_lfo_am);
	state.save_restore(m_noise_lfsr);
	state.save_restore(m_noise_counter);
	state.save_restore(m_noise_state);
	// the byte the noise LFO value once took, kept for the layout
	uint8_t unused = 0;
	state.save_restore(unused);
	state.save_restore(m_regdata);
}


//-------------------------------------------------
//  operator_map - return an array of operator
//  indices for each channel; for OPM this is fixed
//-------------------------------------------------

void opm_registers::operator_map(operator_mapping &dest) const
{
	// Note that the channel index order is 0,2,1,3, so we bitswap the index.
	//
	// This is because the order in the map is:
	//    carrier 1, carrier 2, modulator 1, modulator 2
	//
	// But when wiring up the connections, the more natural order is:
	//    carrier 1, modulator 1, carrier 2, modulator 2
	static const operator_mapping s_fixed_map =
	{ {
		operator_list(  0, 16,  8, 24 ),  // Channel 0 operators
		operator_list(  1, 17,  9, 25 ),  // Channel 1 operators
		operator_list(  2, 18, 10, 26 ),  // Channel 2 operators
		operator_list(  3, 19, 11, 27 ),  // Channel 3 operators
		operator_list(  4, 20, 12, 28 ),  // Channel 4 operators
		operator_list(  5, 21, 13, 29 ),  // Channel 5 operators
		operator_list(  6, 22, 14, 30 ),  // Channel 6 operators
		operator_list(  7, 23, 15, 31 ),  // Channel 7 operators
	} };
	dest = s_fixed_map;
}


//-------------------------------------------------
//  write - handle writes to the register array
//-------------------------------------------------

bool opm_registers::write(uint16_t index, uint8_t data, uint32_t &channel, uint32_t &opmask)
{
	assert(index < REGISTERS);

	// the noise generator takes a new frequency from this cycle
	if (index == 0x0f && m_noise_write_cycle == 32)
	{
		m_noise_old_target = noise_frequency();
		m_noise_write_cycle = m_write_cycle;
	}

	// LFO AM/PM depth are written to the same register (0x19);
	// redirect the PM depth to an unused neighbor (0x1a)
	if (index == 0x19)
		m_regdata[index + bitfield(data, 7)] = data;
	else if (index != 0x1a)
		m_regdata[index] = data;

	// a frequency write reloads the counter; the window's reload tick is at
	// cycle 28 of its last sample, and a write after cycle 27 reloads after it
	if (index == 0x18)
	{
		if (m_lfo.window == 7 && m_write_cycle > LFO_RELOAD_CYCLE)
			m_lfo.reload = true;
		else
			m_lfo.ticks = 1 << (15 - bitfield(data, 4, 4));
	}

	// handle writes to the key on index
	if (index == 0x08)
	{
		channel = bitfield(data, 0, 3);
		opmask = bitfield(data, 3, 4);
		return true;
	}
	return false;
}


//-------------------------------------------------
//  lfo_am_value / lfo_pm_value - the LFO's AM
//  value, and its PM value as sign (bit 7) and
//  magnitude, from the counter and the depth
//-------------------------------------------------

uint8_t opm_registers::lfo_am_value(uint32_t waveform, uint32_t count) const
{
	uint32_t value;
	switch (waveform)
	{
		case 1:  value = bitfield(count, 7) ? 0 : 255;                         break;
		case 2:  value = bitfield(count, 8) ? (count & 255) : (count & 255) ^ 255;  break;
		default: value = (count & 255) ^ 255;                                  break;
	}
	return (value * lfo_am_depth()) >> 7;
}

uint8_t opm_registers::lfo_pm_value(uint32_t waveform, uint32_t count) const
{
	bool negative;
	uint32_t value;
	switch (waveform)
	{
		// full scale: the latch takes the depth itself
		case 1:
			negative = bitfield(count, 7);
			value = 128;
			break;

		// up and down over each half of 512 counts
		case 2:
			negative = bitfield(count, 8);
			value = bitfield(count, 7) ? (count & 127) ^ 127 : (count & 127);
			break;

		// the counter's low byte, in ones' complement
		default:
			negative = bitfield(count, 7);
			value = negative ? (count & 127) ^ 127 : (count & 127);
			break;
	}
	return (negative ? 0x80 : 0) | ((value * lfo_pm_depth()) >> 7);
}


//-------------------------------------------------
//  step_noise - run a noise generator through one
//  sample, its target changing at cycle `change`;
//  returns the output bit and fills `bits` with
//  the low bit seen at each cycle
//-------------------------------------------------

uint32_t opm_registers::step_noise(noise_gen &gen, uint32_t before, uint32_t after, uint32_t change, uint32_t &bits)
{
	// a 17-bit LFSR (x^17 + x^3 + 1, fed a 1 while it is all zero), held as
	// a 16-bit ring that turns once a cycle plus one more bit. A 5-bit timer
	// counts at cycles 15 and 31; once it has reached the target it starts
	// again from 0, and the LFSR steps once a cycle for as long as it
	// matches, from two cycles after. The output takes the ring's low bit
	// at cycle 12.
	uint32_t ring = bitfield(gen.lfsr, 0, 16);
	uint32_t extra = bitfield(gen.lfsr, 16);
	uint32_t timer = bitfield(gen.counter, 0, 5);
	uint32_t matched = bitfield(gen.counter, 5, 2);   // bit 0 one cycle ago, bit 1 two
	uint32_t out = 0;
	bits = 0;
	for (uint32_t cycle = 0; cycle < 32; cycle++)
	{
		bits |= (ring & 1) << cycle;
		uint32_t in = ring & 1;
		if (bitfield(matched, 1))
		{
			in = (ring == 0 && extra == 0) ? 1 : bitfield(ring, 2) ^ extra;
			extra = ring & 1;
		}
		ring = (ring >> 1) | (in << 15);
		if (cycle == 12)
			out = ring & 1;
		uint32_t const match = (timer == ((cycle < change) ? before : after)) ? 1 : 0;
		if ((cycle & 15) == 15)
			timer = bitfield(matched, 0) ? 0 : (timer + 1) & 31;
		matched = ((matched << 1) | match) & 3;
	}
	gen.lfsr = ring | (extra << 16);
	gen.counter = uint8_t(timer | (matched << 5));
	return out;
}


//-------------------------------------------------
//  clock_noise - clock the noise generator
//-------------------------------------------------

void opm_registers::clock_noise()
{
	// Nuked-OPM's noise reaches the mixer NOISE_AHEAD samples sooner than an
	// operator's output: the output takes the generator that many samples
	// on. m_noise_lfsr and m_noise_counter run in time and take each $0F
	// write at its cycle; m_noise_ahead is them run NOISE_AHEAD samples
	// further, again from them after a write.
	uint32_t const target = noise_frequency();
	uint32_t bits;
	noise_gen gen = { m_noise_lfsr, m_noise_counter };
	step_noise(gen, m_noise_write_cycle < 32 ? m_noise_old_target : target, target, m_noise_write_cycle, bits);
	m_noise_history = (m_noise_history >> 32) | (uint64_t(bits) << 32);
	m_noise_lfsr = gen.lfsr;
	m_noise_counter = gen.counter;
	if (m_noise_write_cycle < 32)
	{
		m_noise_ahead = gen;
		for (uint32_t sample = 1; sample < NOISE_AHEAD; sample++)
			step_noise(m_noise_ahead, target, target, 0, bits);
		m_noise_write_cycle = 32;
	}
	m_noise_state = uint8_t(step_noise(m_noise_ahead, target, target, 0, bits));
}


//-------------------------------------------------
//  clock_noise_and_lfo - clock the noise and LFO,
//  handling clock division, depth, and waveform
//  computations
//-------------------------------------------------

int32_t opm_registers::clock_noise_and_lfo()
{
	clock_noise();

	uint32_t const waveform = lfo_waveform();
	lfo_state &lfo = m_lfo;
	switch (lfo.window)
	{
		// the steps the last window's tick asked for
		case 0:
			for (; lfo.steps != 0; lfo.steps--)
			{
				// noise takes 8 of the in-time generator's bits, the low one
				// from the last cycle of the sample before
				if (waveform == 3)
					lfo.count = uint16_t(bitfield(uint32_t(m_noise_history >> 31), 0, 8));
				else
					lfo.count += (waveform == 2) ? 2 : 1;
			}
			break;

		// AM from the stepped counter
		case 1:
			lfo.am_late = lfo_am_value(waveform, lfo.count);
			break;
		case 2:
			m_lfo_am = lfo_am_value(waveform, lfo.count);
			break;

		// PM from the counter before the next steps
		case 6:
			lfo.pm_late = lfo_pm_value(waveform, lfo.count);
			break;

		// the reload tick: a step every 2^(15-hi) ticks, and a second one
		// in lo of every 16 steps, from the lowest clear bit of a 4-bit
		// counter, MSB of lo first
		case 7:
			lfo.pm = lfo_pm_value(waveform, lfo.count);
			if (--lfo.ticks == 0)
			{
				lfo.ticks = 1 << (15 - bitfield(lfo_rate(), 4, 4));
				lfo.steps = 1;
				uint32_t bit = 0;
				while (bit < 4 && bitfield(lfo.rate_steps, bit))
					bit++;
				if (bit < 4 && bitfield(lfo_rate(), 3 - bit))
					lfo.steps = 2;
				lfo.rate_steps = (lfo.rate_steps + 1) & 15;
			}
			if (lfo.reload)
				lfo.ticks = 1 << (15 - bitfield(lfo_rate(), 4, 4));
			lfo.reload = false;
			break;
	}
	lfo.window = (lfo.window + 1) & 7;

	// bit 1 of the test register holds the counter at zero
	if (lfo_reset())
		lfo.count = 0;

	return lfo.pm | (lfo.pm_late << 8);
}


//-------------------------------------------------
//  lfo_am_offset - return the AM offset from LFO
//  for the given channel
//-------------------------------------------------

uint32_t opm_registers::lfo_am_offset(uint32_t choffs) const
{
	// OPM maps AM quite differently from OPN

	// shift value for AM sensitivity is [*, 0, 1, 2],
	// mapping to values of [0, 23.9, 47.8, and 95.6dB]
	uint32_t am_sensitivity = ch_lfo_am_sens(choffs);
	if (am_sensitivity == 0)
		return 0;

	// QUESTION: see OPN note below for the dB range mapping; it applies
	// here as well

	// raw LFO AM value on OPM is 0-FF, which is already a factor of 2
	// larger than the OPN below, putting our staring point at 2x theirs;
	// this works out since our minimum is 2x their maximum
	return (m_lfo_am << (am_sensitivity - 1)) | ((m_lfo.am_late << (am_sensitivity - 1)) << 16);
}


//-------------------------------------------------
//  cache_operator_data - fill the operator cache
//  with prefetched data
//-------------------------------------------------

void opm_registers::cache_operator_data(uint32_t choffs, uint32_t opoffs, opdata_cache &cache)
{
	// set up the easy stuff
	cache.waveform = &m_waveform[0][0];

	// get frequency from the channel
	uint32_t block_freq = cache.block_freq = ch_block_freq(choffs);

	// compute the keycode: block_freq is:
	//
	//     BBBCCCCFFFFFF
	//     ^^^^^
	//
	// the 5-bit keycode is just the top 5 bits (block + top 2 bits
	// of the key code)
	uint32_t keycode = bitfield(block_freq, 8, 5);

	// detune adjustment
	cache.detune = detune_adjustment(op_detune(opoffs), keycode);

	// multiple value, as an x.1 value (0 means 0.5)
	cache.multiple = op_multiple(opoffs) * 2;
	if (cache.multiple == 0)
		cache.multiple = 1;

	// phase step, or PHASE_STEP_DYNAMIC if PM is active; this depends on
	// block_freq, detune, and multiple, so compute it after we've done those
	if (ch_lfo_pm_sens(choffs) == 0)
		cache.phase_step = compute_phase_step(choffs, opoffs, cache, 0);
	else
		cache.phase_step = opdata_cache::PHASE_STEP_DYNAMIC;

	// total level, scaled by 8
	cache.total_level = op_total_level(opoffs) << 3;

	// 4-bit sustain level, but 15 means 31 so effectively 5 bits
	cache.eg_sustain = op_sustain_level(opoffs);
	cache.eg_sustain |= (cache.eg_sustain + 1) & 0x10;
	cache.eg_sustain <<= 5;

	// determine KSR adjustment for enevlope rates
	uint32_t ksrval = keycode >> (op_ksr(opoffs) ^ 3);
	cache.eg_rate[EG_ATTACK] = effective_rate(op_attack_rate(opoffs) * 2, ksrval);
	cache.eg_rate[EG_DECAY] = effective_rate(op_decay_rate(opoffs) * 2, ksrval);
	cache.eg_rate[EG_SUSTAIN] = effective_rate(op_sustain_rate(opoffs) * 2, ksrval);
	cache.eg_rate[EG_RELEASE] = effective_rate(op_release_rate(opoffs) * 4 + 2, ksrval);
}


//-------------------------------------------------
//  opm_apply_pm - move a 13-bit key code by the
//  LFO's PM value at the given sensitivity
//-------------------------------------------------

static uint32_t opm_apply_pm(uint32_t code, int32_t lfo_raw_pm, uint32_t pms)
{
	// with no sensitivity the adder still runs, adding a positive zero
	uint32_t mag = (pms != 0) ? (lfo_raw_pm & 0x7f) : 0;
	bool negative = pms != 0 && (lfo_raw_pm & 0x80) != 0;

	// sensitivities 1-5 take the top bits of the magnitude; 6 and 7 first
	// spread its top three bits over the note encoding, skipping the
	// gaps, then shift it up
	uint32_t amount;
	if (pms <= 5)
		amount = (pms != 0) ? mag >> (6 - pms) : 0;
	else if (pms == 6)
		amount = (mag + (mag >= 0x60 ? 0x20 : 0)) << 1;
	else
	{
		static const uint8_t s_spread[8] = { 0, 1, 2, 4, 5, 6, 8, 9 };
		amount = ((s_spread[mag >> 4] << 4) | (mag & 15)) << 2;
	}

	// the sum steps over the missing fourth note of each group: up past
	// it when the sum lands on it or carries out of a group, down past it
	// when the difference borrows out of a group whose amount has room
	if (!negative)
	{
		uint32_t sum = code + amount;
		bool carry = (code & 0xff) + (amount & 0xff) >= 0x100;
		if (sum < 0x2000 && (bitfield(sum, 6, 2) == 3 || carry))
			sum += 0x40;
		return (sum < 0x2000) ? sum : 0x1fbf;
	}
	if (amount > code)
		return 0;
	uint32_t diff = code - amount;
	bool borrow = (code & 0xff) < (amount & 0xff);
	if (borrow && bitfield(amount, 6, 2) != 3)
		return (diff >= 0x40) ? diff - 0x40 : 0;
	return diff;
}


//-------------------------------------------------
//  opm_apply_dt2 - add DT2 to a 13-bit key code:
//  whole notes, then a fraction whose carry is a
//  further note, stepping over the missing fourth
//  note of each group
//-------------------------------------------------

static uint32_t opm_apply_dt2(uint32_t code, uint32_t dt2)
{
	static const uint8_t s_groups[4] = { 0, 2, 2, 3 };
	static const uint8_t s_notes[4] = { 0, 0, 1, 0 };
	static const uint8_t s_fraction[4] = { 0, 0, 52, 32 };
	if (dt2 == 0)
		return code;

	uint32_t fraction = bitfield(code, 0, 6) + s_fraction[dt2];
	uint32_t notes = s_notes[dt2] + (fraction >> 6);
	uint32_t kc = (code >> 6) + 4 * s_groups[dt2];

	// raw steps for one and two notes from each place in the group
	static const uint8_t s_step[3][4] = { { 0, 0, 0, 0 }, { 1, 1, 2, 2 }, { 2, 3, 3, 3 } };
	kc += s_step[notes][kc & 3];
	if (kc >= 0x80)
		return 0x1fbf;
	return (kc << 6) | (fraction & 0x3f);
}


//-------------------------------------------------
//  opm_code_to_phase_step - phase step of a 13-bit
//  key code; the missing notes give the chip's
//  F-number table nearly nothing
//-------------------------------------------------

static uint32_t opm_code_to_phase_step(uint32_t code)
{
	if (bitfield(code, 6, 2) != 3)
		return opm_key_code_to_phase_step(code, 0);
	static const uint8_t s_gap_fnum[16] = { 0, 2, 4, 6, 4, 6, 8, 10, 9, 11, 13, 15, 15, 17, 19, 21 };
	return (s_gap_fnum[bitfield(code, 0, 4)] << bitfield(code, 10, 3)) >> 2;
}


//-------------------------------------------------
//  compute_phase_step - compute the phase step
//-------------------------------------------------

uint32_t opm_registers::compute_phase_step(uint32_t choffs, uint32_t opoffs, opdata_cache const &cache, int32_t lfo_raw_pm)
{
	// PM, then DT2, move the key code itself, in the chip's own encoding:
	// block, a 4-bit note whose values 3, 7, 11 and 15 are not notes, and a
	// 6-bit fraction. lfo_raw_pm holds the PM latches for operators 0-22
	// (bits 0-7) and 23-31 (bits 8-15), each a sign (bit 7) and magnitude.
	// Behaviour follows the Nuked-OPM vectors.
	uint32_t code = cache.block_freq;
	// a depth of 0 holds PM at a positive zero as soon as it is written,
	// whatever the latch still holds
	uint32_t pm = (lfo_pm_depth() != 0) ? bitfield(lfo_raw_pm, (opoffs >= 23) ? 8 : 0, 8) : 0;
	code = opm_apply_pm(code, pm, ch_lfo_pm_sens(choffs));
	code = opm_apply_dt2(code, op_detune2(opoffs));

	// DT1 is chosen by the block and note group the code has reached
	uint32_t phase_step = opm_code_to_phase_step(code);
	phase_step += detune_adjustment(op_detune(opoffs), std::min<uint32_t>(code >> 8, 0x1c));
	phase_step &= 0x1ffff;

	// apply frequency multiplier (which is cached as an x.1 value)
	return (phase_step * cache.multiple) >> 1;
}


//-------------------------------------------------
//  log_keyon - log a key-on event
//-------------------------------------------------

std::string opm_registers::log_keyon(uint32_t choffs, uint32_t opoffs)
{
	uint32_t chnum = choffs;
	uint32_t opnum = opoffs;

	char buffer[256];
	int end = 0;

	end += snprintf(&buffer[end], sizeof(buffer) - end, "%u.%02u freq=%04X dt2=%u dt=%u fb=%u alg=%X mul=%X tl=%02X ksr=%u adsr=%02X/%02X/%02X/%X sl=%X out=%c%c",
		chnum, opnum,
		ch_block_freq(choffs),
		op_detune2(opoffs),
		op_detune(opoffs),
		ch_feedback(choffs),
		ch_algorithm(choffs),
		op_multiple(opoffs),
		op_total_level(opoffs),
		op_ksr(opoffs),
		op_attack_rate(opoffs),
		op_decay_rate(opoffs),
		op_sustain_rate(opoffs),
		op_release_rate(opoffs),
		op_sustain_level(opoffs),
		ch_output_0(choffs) ? 'L' : '-',
		ch_output_1(choffs) ? 'R' : '-');

	bool am = (lfo_am_depth() != 0 && ch_lfo_am_sens(choffs) != 0 && op_lfo_am_enable(opoffs) != 0);
	if (am)
		end += snprintf(&buffer[end], sizeof(buffer) - end, " am=%u/%02X", ch_lfo_am_sens(choffs), lfo_am_depth());
	bool pm = (lfo_pm_depth() != 0 && ch_lfo_pm_sens(choffs) != 0);
	if (pm)
		end += snprintf(&buffer[end], sizeof(buffer) - end, " pm=%u/%02X", ch_lfo_pm_sens(choffs), lfo_pm_depth());
	if (am || pm)
		end += snprintf(&buffer[end], sizeof(buffer) - end, " lfo=%02X/%c", lfo_rate(), "WQTN"[lfo_waveform()]);
	if (noise_enable() && opoffs == 31)
		end += snprintf(&buffer[end], sizeof(buffer) - end, " noise=1");

	return buffer;
}



//*********************************************************
//  YM2151
//*********************************************************

//-------------------------------------------------
//  ym2151 - constructor
//-------------------------------------------------

ym2151::ym2151(ymfm_interface &intf, opm_variant variant) :
	m_variant(variant),
	m_address(0),
	m_fm(intf)
{
}


//-------------------------------------------------
//  reset - reset the system
//-------------------------------------------------

void ym2151::reset()
{
	// reset the engines
	m_fm.reset();
}


//-------------------------------------------------
//  save_restore - save or restore the data
//-------------------------------------------------

void ym2151::save_restore(ymfm_saved_state &state)
{
	m_fm.save_restore(state);
	state.save_restore(m_address);
}


//-------------------------------------------------
//  read_status - read the status register
//-------------------------------------------------

uint8_t ym2151::read_status()
{
	uint8_t result = m_fm.status();
	if (m_fm.intf().ymfm_is_busy())
		result |= fm_engine::STATUS_BUSY;
	return result;
}


//-------------------------------------------------
//  read - handle a read from the device
//-------------------------------------------------

uint8_t ym2151::read(uint32_t offset)
{
	uint8_t result = 0xff;
	switch (offset & 1)
	{
		case 0: // data port (unused)
			debug::log_unexpected_read_write("Unexpected read from YM2151 offset %d\n", offset & 3);
			break;

		case 1: // status port, YM2203 compatible
			result = read_status();
			break;
	}
	return result;
}


//-------------------------------------------------
//  write_address - handle a write to the address
//  register
//-------------------------------------------------

void ym2151::write_address(uint8_t data)
{
	// just set the address
	m_address = data;
}


//-------------------------------------------------
//  write - handle a write to the register
//  interface
//-------------------------------------------------

void ym2151::write_data(uint8_t data)
{
	// write the FM register
	m_fm.write(m_address, data);

	// special cases
	if (m_address == 0x1b)
	{
		// writes to register 0x1B send the upper 2 bits to the output lines
		m_fm.intf().ymfm_external_write(ACCESS_IO, 0, data >> 6);
	}

	// mark busy for a bit
	m_fm.intf().ymfm_set_busy_end(32 * m_fm.clock_prescale());
}


//-------------------------------------------------
//  write - handle a write to the register
//  interface
//-------------------------------------------------

void ym2151::write(uint32_t offset, uint8_t data)
{
	switch (offset & 1)
	{
		case 0: // address port
			write_address(data);
			break;

		case 1: // data port
			write_data(data);
			break;
	}
}


//-------------------------------------------------
//  generate - generate one sample of sound
//-------------------------------------------------

void ym2151::generate(output_data *output, uint32_t numsamples)
{
	for (uint32_t samp = 0; samp < numsamples; samp++, output++)
	{
		// clock the system
		m_fm.clock(fm_engine::ALL_CHANNELS);

		// update the FM content; OPM is full 14-bit with no intermediate clipping
		m_fm.output(output->clear(), 0, 32767, fm_engine::ALL_CHANNELS);

		// YM2151 uses an external DAC (YM3012) with mantissa/exponent format
		// convert to 10.3 floating point value and back to simulate truncation
		output->roundtrip_fp();
	}
}


//*********************************************************
//  EXPLICIT INSTANTIATION
//*********************************************************

template class fm_engine_base<opm_registers>;

}
