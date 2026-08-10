#include "ymglue.h"
#include "ymfm_opm.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

// ymfm keeps the FM engine `protected` inside ym2151, but the engine is exactly
// what a debugger needs: it exposes debug_operator()/debug_channel() and the
// per-channel output() used by the oscilloscope. Publish it via a subclass
// rather than patching the vendored source.
class ym2151_debuggable : public ymfm::ym2151 {
	public:
		using ymfm::ym2151::ym2151;
		fm_engine &engine() { return m_fm; }
};

class ym2151_interface : public ymfm::ymfm_interface {
	public:
		ym2151_interface():
			m_chip(*this),
			m_timers{0, 0},
			m_busy_timer{ 0 },
			m_irq_status{ false },
			m_regs{},
			m_keyon{},
			m_scope{},
			m_scope_wridx{ 0 },
			m_scope_armed{ 0 }
		{ }
		~ym2151_interface() { }

		virtual void ymfm_sync_mode_write(uint8_t data) override {
			m_engine->engine_mode_write(data);
		}

		virtual void ymfm_sync_check_interrupts() override {
			m_engine->engine_check_interrupts();
		}

		virtual void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
			if (tnum >= 2) return;
			m_timers[tnum] = duration_in_clocks;
		}

		virtual void ymfm_set_busy_end(uint32_t clocks) override {
			m_busy_timer = clocks;
		}

		virtual bool ymfm_is_busy() override {
			return m_busy_timer > 0;
		}

		virtual void ymfm_update_irq(bool asserted) override {
			m_irq_status = asserted;
		}

		void update_clocks(int cycles) {
			m_busy_timer = std::max(0, m_busy_timer - (64 * cycles));
			for (int i = 0; i < 2; ++i) {
				if (m_timers[i] > 0) {
					m_timers[i] = std::max(0, m_timers[i] - (64 * cycles));
					if (m_timers[i] <= 0) {
						m_engine->engine_timer_expired(i);
					}
				}
			}	
		}

		void write(uint8_t addr, uint8_t value) {
			if (!ymfm_is_busy()) {
				m_chip.write_address(addr);
				m_chip.write_data(value);
				// Shadow only accepted writes, so the debugger's register view
				// matches what the chip actually holds.
				m_regs[addr] = value;
				if (addr == 0x08) {
					latch_keyon(value);
				}
			} else {
				printf("YM2151 write received while busy.\n");
			}
		}

		void generate(int16_t* output, uint32_t numsamples) {
			int s = 0;
			int ls, rs;
			update_clocks(numsamples);
			for (uint32_t i = 0; i < numsamples; i++) {
				m_chip.generate(&opm_out);
				ls = opm_out.data[0];
				rs = opm_out.data[1];
				if (ls < -32768) ls = -32768;
				if (ls > 32767) ls = 32767;
				if (rs < -32768) rs = -32768;
				if (rs > 32767) rs = 32767;
				output[s++] = ls;
				output[s++] = rs;
				// Re-test per sample rather than hoisting: capture_sample()
				// decrements the countdown, and numsamples is arbitrary, so a
				// hoisted test would keep decrementing past zero and wrap the
				// unsigned counter — leaving capture armed forever.
				if (m_scope_armed > 0) {
					capture_sample((int16_t)ls, (int16_t)rs);
				}
			}
		}

		uint8_t read_status() {
			return m_chip.read_status();
		}

		bool irq() {
			return m_irq_status;
		}

		// ─── Debugger accessors ─────────────────────────────────────────────

		uint8_t debug_reg(uint8_t reg) const {
			return m_regs[reg];
		}

		uint8_t debug_keyon(int ch) const {
			return (ch >= 0 && ch < YM_NUM_CHANNELS) ? m_keyon[ch] : 0;
		}

		bool debug_operator(int op, struct ym_debug_operator *out) {
			if (op < 0 || op >= YM_NUM_OPERATORS || out == nullptr) {
				return false;
			}
			auto *fmop = m_chip.engine().debug_operator((uint32_t)op);
			if (fmop == nullptr) {
				return false;
			}
			out->eg_state       = (uint8_t)fmop->debug_eg_state();
			out->eg_attenuation = fmop->debug_eg_attenuation();
			return true;
		}

		bool debug_busy() {
			return ymfm_is_busy();
		}

		unsigned debug_scope_read(int ch, int16_t* dest, unsigned max_samples) {
			// Always (re)arm: an out-of-range stream still means the debugger
			// is watching, and streams may be probed in any order.
			m_scope_armed = SCOPE_ARM_SAMPLES;

			if (ch < 0 || ch >= YM_SCOPE_STREAMS || dest == nullptr || max_samples == 0) {
				return 0;
			}
			unsigned n = (max_samples > YM_SCOPE_SAMPLES) ? YM_SCOPE_SAMPLES : max_samples;
			unsigned start = (m_scope_wridx + YM_SCOPE_SAMPLES - n) & (YM_SCOPE_SAMPLES - 1);
			for (unsigned i = 0; i < n; i++) {
				dest[i] = m_scope[ch][(start + i) & (YM_SCOPE_SAMPLES - 1)];
			}
			return n;
		}

		// Project the next `num_samples` of output WITHOUT touching the live
		// chip in any way.
		//
		// The live chip's state is snapshotted and loaded into a scratch chip,
		// and only the scratch chip is clocked. Envelopes, LFO and operator
		// phase therefore continue from exactly where the live chip is, so this
		// is a true continuation rather than a re-triggered note.
		//
		// Restoring back into the live chip would work too - the round trip is
		// lossless - but its state is an opaque object graph rather than a
		// handful of plain fields, so "everything was put back" is not something
		// a reviewer can check by eye the way it is for the PSG and PCM cores.
		// ymfm hands us both a spare instance and a save-state, so use them.
		unsigned debug_scope_predict(int16_t (*dest)[YM_SCOPE_SAMPLES], unsigned num_samples) {
			if (dest == nullptr || num_samples == 0) {
				return 0;
			}
			unsigned n = (num_samples > YM_SCOPE_SAMPLES) ? YM_SCOPE_SAMPLES : num_samples;

			// Saving is a pure read of the live chip.
			std::vector<uint8_t> chip_state;
			{
				ymfm::ymfm_saved_state saver(chip_state, true);
				m_chip.save_restore(saver);
			}

			ym2151_interface &scratch = scratch_chip();
			{
				ymfm::ymfm_saved_state restorer(chip_state, false);
				scratch.m_chip.save_restore(restorer);
			}
			scratch.m_timers[0]   = m_timers[0];
			scratch.m_timers[1]   = m_timers[1];
			scratch.m_busy_timer  = m_busy_timer;
			scratch.m_irq_status  = m_irq_status;
			scratch.m_scope_armed = 0;

			ymfm::ym2151::output_data out;
			for (unsigned i = 0; i < n; i++) {
				// Mirror generate()'s per-sample work, including the clock
				// bookkeeping, so timer-driven envelope changes show up.
				scratch.update_clocks(1);
				scratch.m_chip.generate(&out);
				int32_t ls = out.data[0], rs = out.data[1];
				if (ls < -32768) ls = -32768;
				if (ls > 32767) ls = 32767;
				if (rs < -32768) rs = -32768;
				if (rs > 32767) rs = 32767;
				int16_t streams[YM_SCOPE_STREAMS];
				scratch.sample_streams((int16_t)ls, (int16_t)rs, streams);
				for (int s = 0; s < YM_SCOPE_STREAMS; s++) {
					dest[s][i] = streams[s];
				}
			}
			return n;
		}

	private:
		static constexpr unsigned SCOPE_ARM_SAMPLES = 28000; // ~0.5 s at 55930 Hz

		// Spare chip used only to render debugger projections, so nothing the
		// debug UI does can perturb the chip that is producing audio. Built on
		// first use: most sessions never open the FM scope while paused.
		static ym2151_interface &scratch_chip() {
			static ym2151_interface *s_scratch = new ym2151_interface();
			return *s_scratch;
		}

		// $08 bits 3-6 select operators in the OPM's key-on order M1, C1, M2, C2,
		// which is NOT the order the $40-$FF register slots use (M1, M2, C1, C2).
		// Re-pack into slot order so callers index key-on state the same way they
		// index operators and registers.
		void latch_keyon(uint8_t value) {
			const uint8_t kon   = (value >> 3) & 0x0F;
			uint8_t       slots = 0;
			if (kon & 0x01) slots |= 1u << 0; // M1 -> slot 0
			if (kon & 0x02) slots |= 1u << 2; // C1 -> slot 2
			if (kon & 0x04) slots |= 1u << 1; // M2 -> slot 1
			if (kon & 0x08) slots |= 1u << 3; // C2 -> slot 3
			m_keyon[value & 7] = slots;
		}

		// Isolate each channel by re-running the engine's output stage with a
		// single-channel mask. This does NOT disturb the audio: output() only
		// reads operator phase/envelope state (compute_volume is const and pure)
		// and writes the mutable m_feedback_in, which clock() consumes on the
		// next sample — recomputing it from unchanged inputs yields the same
		// value.
		void sample_streams(int16_t mix_l, int16_t mix_r, int16_t out[YM_SCOPE_STREAMS]) {
			ymfm::ym2151::output_data one;
			for (int c = 0; c < YM_NUM_CHANNELS; c++) {
				m_chip.engine().output(one.clear(), 0, 32767, 1u << c);
				int32_t v = one.data[0] + one.data[1];
				if (v < -32768) v = -32768;
				if (v > 32767) v = 32767;
				out[c] = (int16_t)v;
			}
			out[YM_SCOPE_MIX_L] = mix_l;
			out[YM_SCOPE_MIX_R] = mix_r;
		}

		// Only reached while capture is armed.
		void capture_sample(int16_t mix_l, int16_t mix_r) {
			int16_t streams[YM_SCOPE_STREAMS];
			sample_streams(mix_l, mix_r, streams);
			for (int i = 0; i < YM_SCOPE_STREAMS; i++) {
				m_scope[i][m_scope_wridx] = streams[i];
			}
			m_scope_wridx = (m_scope_wridx + 1) & (YM_SCOPE_SAMPLES - 1);
			m_scope_armed--;
		}

		ym2151_debuggable m_chip;
		int32_t m_timers[2];
		int32_t m_busy_timer;
		bool m_irq_status;

		uint8_t m_regs[YM_NUM_REGS];
		uint8_t m_keyon[YM_NUM_CHANNELS];

		int16_t  m_scope[YM_SCOPE_STREAMS][YM_SCOPE_SAMPLES];
		unsigned m_scope_wridx;
		unsigned m_scope_armed;

		ymfm::ym2151::output_data opm_out;
};

namespace {
	ym2151_interface opm_iface;
	bool initialized = false;
}

extern "C" {
	void YM_Create(int clock) {
		// clock is fixed at 3.579545MHz
	}

	void YM_init(int sample_rate, int frame_rate) {
		// args are ignored
		initialized = true;
	}

	void YM_stream_update(uint16_t* output, uint32_t numsamples) {
		if (initialized) opm_iface.generate((int16_t*)output, numsamples);
	}

	void YM_write_reg(uint8_t reg, uint8_t val) {
		if (initialized) opm_iface.write(reg, val);
	}

	uint8_t YM_read_status() {
		if (initialized)
			return opm_iface.read_status();
		else
			return 0x00; // prevent programs that wait for the busy flag to clear from locking up (emulator compromise)
	}

	bool YM_irq() {
		if (initialized)
			return opm_iface.irq();
		else
			return false;
	}

	// ─── Debugger accessors ─────────────────────────────────────────────────

	uint8_t YM_debug_get_reg(uint8_t reg) {
		return opm_iface.debug_reg(reg);
	}

	uint8_t YM_debug_get_keyon(int ch) {
		return opm_iface.debug_keyon(ch);
	}

	bool YM_debug_get_operator(int op, struct ym_debug_operator *out) {
		if (!initialized) return false;
		return opm_iface.debug_operator(op, out);
	}

	uint8_t YM_debug_get_status() {
		// read_status() has no side effects on the OPM (it just ORs the busy
		// flag into the engine's status byte), so it is safe to poll.
		return initialized ? opm_iface.read_status() : 0x00;
	}

	bool YM_debug_is_busy() {
		return initialized ? opm_iface.debug_busy() : false;
	}

	unsigned YM_debug_scope_read(int ch, int16_t* dest, unsigned max_samples) {
		return opm_iface.debug_scope_read(ch, dest, max_samples);
	}

	unsigned YM_debug_scope_predict(int16_t (*dest)[YM_SCOPE_SAMPLES], unsigned num_samples) {
		return initialized ? opm_iface.debug_scope_predict(dest, num_samples) : 0;
	}
}
