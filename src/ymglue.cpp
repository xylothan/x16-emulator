#include "ymglue.h"
#include "ymfm_opm.h"
#include "savestate.h"
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

// ymfm's own save_restore() covers the engine except three fields of its
// bookkeeping -- which channels are active, which were modified, and how far
// along the periodic re-prepare sweep is -- and it ends by marking every
// channel modified. That makes saving a write: the next sample re-prepares
// every channel, which a machine that was never saved would not do. These
// pointers reach the three fields so a save can put them back and a load can
// restore them exactly. Never instantiated: deriving only grants the access
// needed to name the protected members.
using ym_engine = ymfm::fm_engine_base<ymfm::opm_registers>;
struct ym_engine_fields : ym_engine {
	using field = uint32_t ym_engine::*;
	static field active()   { return &ym_engine_fields::m_active_channels; }
	static field modified() { return &ym_engine_fields::m_modified_channels; }
	static field prepare()  { return &ym_engine_fields::m_prepare_count; }
};

// It also leaves out one table. The OPM's fourth LFO waveform is noise, and
// ymfm builds that waveform as the chip runs: every clock writes the noise
// generator's latest bits into the entry just ahead of the LFO. Without it, a
// load would leave the table as the frames played since the save left it, and
// any channel with noise LFO pitch or volume modulation would drift from the
// saved machine. The other three waveforms never change.
struct ym_register_fields : ymfm::opm_registers {
	// ymfm keeps its own length private; were it ever to change, the pointer
	// below would no longer convert and this would stop compiling.
	static constexpr uint32_t LFO_LENGTH = 256;
	static constexpr uint32_t NOISE_WAVEFORM = 3;
	using table = int16_t (ymfm::opm_registers::*)[4][LFO_LENGTH];
	static table lfo_waveform() { return &ym_register_fields::m_lfo_waveform; }
};

// Debugger mute: one bit per channel, applied only to what goes to the host.
// File scope rather than per instance so the scratch chip used for scope
// projections hears the same mix as the live one.
static uint8_t s_mute_mask;                     // the debugger UI's own selection
static uint8_t s_owner_mute[VOICE_MUTE_OWNERS]; // each debug client's
static uint8_t s_effective_mute;                // all of them ORed

static void (*s_lost_handler)(uint8_t reg, uint8_t val);

static inline int16_t clamp_sample(int32_t v) {
	return (int16_t)std::min(32767, std::max(-32768, v));
}

// The chip's CPU side, clocked once per cycle: two phiM clocks, 32 per sample.
// Both ports write one bus latch, which the chip reads two cycles later. An
// address of $20 or above holds its data until the slot scan reaches that
// register (every 8 cycles for $20-$3F, every 32 above), and a new address
// before then loses it. Busy does not gate writes. Expected behaviour comes
// from Nuked-OPM, which is derived from the die (tests/accuracy/ym2151).
struct opm_bus {
	uint8_t  cycle = 0;
	uint8_t  bus = 0;
	bool     wa = false, wd = false;        // strobes since the last cycle
	bool     wa_en = false, wd_en = false;  // strobes the chip is taking
	uint8_t  mode_addr = 0;                 // address, for $00-$1F
	uint8_t  addr = 0;                      // address, for $20-$FF
	bool     addr_ok = false;
	uint8_t  data = 0;
	bool     data_ok = false;               // waiting for, or past, its slot
	bool     data_done = false;             // handed to the engine
	bool     busy = false;
	uint8_t  busy_cnt = 0;

	uint16_t a_reg = 0, a_val = 0;
	bool     a_load_reg = false, a_load = false, a_inc = false, a_do_load = false;
	bool     a_do_reset = true, a_temp = true, a_of = false;
	bool     a_irqen = false, a_reset = false, a_flag = false;

	uint8_t  b_reg = 0, b_val = 0, b_sub = 0;
	bool     b_load_reg = false, b_inc = false, b_do_load = false;
	bool     b_do_reset = true, b_temp = true, b_of = false, b_sub_of = false;
	bool     b_irqen = false, b_reset = false, b_flag = false;

	bool     csm = false, csm_lock = false;
	bool     irq = false;

	static unsigned bit(bool v, unsigned n) {
		return (v ? 1u : 0u) << n;
	}

	uint8_t status() const {
		return (uint8_t)(bit(busy, 7) | bit(b_flag, 1) | bit(a_flag, 0));
	}

	// True when the strobe overwrites a byte the chip has not read yet.
	bool write(uint8_t port, uint8_t value) {
		const bool clobbers = wa || wd || wa_en || wd_en;
		bus = value;
		if (port & 1) {
			wd = true;
		} else {
			wa = true;
		}
		return clobbers;
	}

	// Nothing but the cycle and timer B's prescaler can change.
	bool quiet() const {
		return !wa && !wd && !wa_en && !wd_en && !busy && (!data_ok || data_done)
		    && !a_load_reg && !a_load && !a_inc && !a_do_load && !a_of && a_temp && a_do_reset && !a_reset
		    && !b_load_reg && !b_inc && !b_do_load && !b_of && b_temp && b_do_reset && !b_reset
		    && !csm_lock && irq == (a_flag || b_flag) && a_val == 0 && b_val == 0;
	}

	// Nothing pending on the bus, and both timers between counts with LOAD
	// taken. Only cycle 0 (timer B's prescaler, timer A's count) then acts, so
	// from cycle 1 to the end of a sample a settled chip changes nothing.
	bool settled() const {
		return !wa && !wd && !wa_en && !wd_en && !busy && (!data_ok || data_done)
		    && a_load == a_load_reg
		    && !a_inc && !a_do_load && !a_of && a_temp == !a_load && a_do_reset == a_temp && !a_reset
		    && (!a_do_reset || a_val == 0)
		    && !b_inc && !b_do_load && !b_of && !b_sub_of && b_temp == !b_load_reg && b_do_reset == b_temp && !b_reset
		    && (!b_do_reset || b_val == 0)
		    && !csm_lock && irq == (a_flag || b_flag);
	}

	static bool scans(uint8_t a, uint8_t cycle) {
		return a < 0x40 ? (a & 7) == (cycle & 7) : (a & 0x1f) == cycle;
	}

	void mode_write(uint8_t a, uint8_t v) {
		switch (a) {
			case 0x10: a_reg = (uint16_t)((a_reg & 3) | v << 2); break;
			case 0x11: a_reg = (uint16_t)((a_reg & 0x3fc) | (v & 3)); break;
			case 0x12: b_reg = v; break;
			case 0x14:
				csm        = v & 0x80;
				b_reset    = v & 0x20;
				a_reset    = v & 0x10;
				b_irqen    = v & 0x08;
				a_irqen    = v & 0x04;
				b_load_reg = v & 0x02;
				a_load_reg = v & 0x01;
				break;
		}
	}

	// `apply(a, v)` receives each register write as the chip takes it,
	// `lost(a, v)` each one it never will, and `csm_keyon()` each CSM key-on.
	template <class Apply, class Lost, class Csm>
	void clock(Apply &&apply, Lost &&lost, Csm &&csm_keyon) {
		irq = a_flag || b_flag;

		uint16_t va = (uint16_t)(a_val + a_inc);
		a_of = (va >> 10) & 1;
		if (a_do_reset) va = 0;
		if (a_do_load) va = a_reg;
		a_val = va & 1023;

		uint16_t vb = (uint16_t)(b_val + b_inc);
		b_of = (vb >> 8) & 1;
		if (b_do_reset) vb = 0;
		if (b_do_load) vb = b_reg;
		b_val = (uint8_t)vb;
		if (cycle == 0) b_sub++;
		b_sub_of = (b_sub >> 4) & 1;
		b_sub &= 15;

		if (data_ok && !data_done && scans(addr, cycle)) {
			apply(addr, data);
			data_done = true;
		}
		if (wd_en && mode_addr < 0x20) {
			mode_write(mode_addr, bus);
			apply(mode_addr, bus);
		}
		if (wa_en && data_ok && !data_done) {
			lost(addr, data);
		}
		data_ok = data_ok && !wa_en;
		if (addr_ok && wd_en) {
			if (data_ok && !data_done) {
				lost(addr, data);
			}
			data = bus;
			data_ok = true;
			data_done = false;
		}
		addr_ok = addr_ok && !wa_en;
		if (wa_en && (bus & 0xe0)) {
			addr = bus;
			addr_ok = true;
		}
		if (wa_en) {
			mode_addr = bus;
		}

		busy_cnt = (uint8_t)(busy_cnt + busy);
		busy = (!(busy_cnt >> 5) && busy) || wd_en;
		busy_cnt &= 31;
		wa_en = wa;
		wd_en = wd;
		wa = wd = false;

		if (cycle == 1) a_load = a_load_reg;
		a_inc = a_load && cycle == 0;
		a_do_load = a_of || (a_load && a_temp);
		a_do_reset = a_temp;
		a_temp = !a_load;
		a_flag = !a_reset && (a_flag || (a_irqen && a_of));
		a_reset = false;

		b_inc = b_load_reg && b_sub_of;
		b_do_load = b_of || (b_load_reg && b_temp);
		b_do_reset = b_temp;
		b_temp = !b_load_reg;
		b_flag = !b_reset && (b_flag || (b_irqen && b_of));
		b_reset = false;

		if (cycle == 1) {
			csm_lock = a_do_load && csm;
			if (csm_lock) csm_keyon();
		}
		cycle = (cycle + 1) & 31;
	}

	// Runs n cycles, skipping ahead while nothing is pending.
	template <class Apply, class Lost, class Csm>
	void run(uint32_t n, Apply &&apply, Lost &&lost, Csm &&csm_keyon) {
		while (n > 0) {
			if (cycle >= 1 && settled()) {
				const uint32_t step = std::min<uint32_t>(n, 32u - cycle);
				cycle = (uint8_t)((cycle + step) & 31);
				n -= step;
			} else if (!quiet()) {
				clock(apply, lost, csm_keyon);
				n--;
			} else if (cycle == 0) {
				b_sub++;
				b_sub_of = (b_sub >> 4) & 1;
				b_sub &= 15;
				cycle = 1;
				n--;
			} else {
				const uint32_t step = std::min<uint32_t>(n, 32u - cycle);
				b_sub_of = false;
				cycle = (uint8_t)((cycle + step) & 31);
				n -= step;
			}
		}
	}

	static constexpr size_t IMAGE_SIZE = 22;

	void to_image(uint8_t img[IMAGE_SIZE]) const {
		img[0]  = cycle;
		img[1]  = bus;
		img[2]  = mode_addr;
		img[3]  = addr;
		img[4]  = data;
		img[5]  = busy_cnt;
		img[6]  = (uint8_t)(bit(wa, 0) | bit(wd, 1) | bit(wa_en, 2) | bit(wd_en, 3) | bit(addr_ok, 4) | bit(data_ok, 5) | bit(data_done, 6) | bit(busy, 7));
		img[7]  = (uint8_t)(a_reg & 0xff);
		img[8]  = (uint8_t)(a_reg >> 8);
		img[9]  = (uint8_t)(a_val & 0xff);
		img[10] = (uint8_t)(a_val >> 8);
		img[11] = (uint8_t)(bit(a_load_reg, 0) | bit(a_load, 1) | bit(a_inc, 2) | bit(a_do_load, 3) | bit(a_do_reset, 4) | bit(a_temp, 5) | bit(a_of, 6));
		img[12] = (uint8_t)(bit(a_irqen, 0) | bit(a_reset, 1) | bit(a_flag, 2));
		img[13] = b_reg;
		img[14] = b_val;
		img[15] = b_sub;
		img[16] = (uint8_t)(bit(b_load_reg, 0) | bit(b_inc, 1) | bit(b_do_load, 2) | bit(b_do_reset, 3) | bit(b_temp, 4) | bit(b_of, 5) | bit(b_sub_of, 6));
		img[17] = (uint8_t)(bit(b_irqen, 0) | bit(b_reset, 1) | bit(b_flag, 2));
		img[18] = (uint8_t)(bit(csm, 0) | bit(csm_lock, 1) | bit(irq, 2));
		img[19] = img[20] = img[21] = 0; // reserved
	}

	void from_image(const uint8_t img[IMAGE_SIZE]) {
		cycle = img[0] & 31; bus = img[1]; mode_addr = img[2]; addr = img[3]; data = img[4]; busy_cnt = img[5] & 31;
		wa = img[6] & 1; wd = img[6] & 2; wa_en = img[6] & 4; wd_en = img[6] & 8;
		addr_ok = img[6] & 16; data_ok = img[6] & 32; data_done = img[6] & 64; busy = img[6] & 128;
		a_reg = (uint16_t)((img[7] | img[8] << 8) & 1023);
		a_val = (uint16_t)((img[9] | img[10] << 8) & 1023);
		a_load_reg = img[11] & 1; a_load = img[11] & 2; a_inc = img[11] & 4; a_do_load = img[11] & 8;
		a_do_reset = img[11] & 16; a_temp = img[11] & 32; a_of = img[11] & 64;
		a_irqen = img[12] & 1; a_reset = img[12] & 2; a_flag = img[12] & 4;
		b_reg = img[13]; b_val = img[14]; b_sub = img[15] & 15;
		b_load_reg = img[16] & 1; b_inc = img[16] & 2; b_do_load = img[16] & 4; b_do_reset = img[16] & 8;
		b_temp = img[16] & 16; b_of = img[16] & 32; b_sub_of = img[16] & 64;
		b_irqen = img[17] & 1; b_reset = img[17] & 2; b_flag = img[17] & 4;
		csm = img[18] & 1; csm_lock = img[18] & 2; irq = img[18] & 4;
	}
};
// A new field must reach to_image() and from_image().
static_assert(sizeof(opm_bus) == 44, "opm_bus changed: update its image");

class ym2151_interface : public ymfm::ymfm_interface {
	public:
		ym2151_interface():
			m_chip(*this),
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

		// Busy, the timers, their flags and IRQ are opm_bus's. The engine's
		// own copies are never read.
		virtual void ymfm_set_timer(uint32_t, int32_t) override { }
		virtual void ymfm_set_busy_end(uint32_t) override { }
		virtual bool ymfm_is_busy() override { return false; }
		virtual void ymfm_update_irq(bool) override { }

		void reset() {
			m_chip.reset();
			m_bus = opm_bus{};
			std::fill(std::begin(m_regs), std::end(m_regs), 0);
			std::fill(std::begin(m_keyon), std::end(m_keyon), 0);
		}

		// Runs the interface n cycles, handing the engine each register write
		// as the chip takes it.
		void run(uint32_t n) {
			m_bus.run(n,
				[this](uint8_t a, uint8_t v) {
					m_chip.engine().regs().set_write_cycle(m_bus.cycle);
					m_chip.write_address(a);
					m_chip.write_data(v);
					m_regs[a] = v;
					if (a == 0x08) {
						latch_keyon(v);
					}
				},
				[this](uint8_t a, uint8_t v) {
					if (m_report_lost && s_lost_handler != nullptr) {
						s_lost_handler(a, v);
					}
				},
				[this]() { m_engine->engine_timer_expired(0); });
		}

		// To `cycle` of the sample not yet generated. Time only moves forward.
		void sync(uint8_t cycle) {
			if (cycle > m_bus.cycle) {
				run(cycle - m_bus.cycle);
			}
		}

		// Returns true when it overwrites a byte the chip had not read yet.
		bool write(uint8_t port, uint8_t value) {
			return m_bus.write(port, value);
		}

		// `output` is the machine's mix of every channel; `host_output` the
		// same without the muted ones. Each sample follows the rest of its
		// 32 interface cycles.
		void generate(int16_t* output, int16_t* host_output, uint32_t numsamples) {
			int s = 0;
			for (uint32_t i = 0; i < numsamples; i++) {
				run(32u - m_bus.cycle);
				m_chip.generate(&opm_out);
				const int16_t ls = clamp_sample(opm_out.data[0]);
				const int16_t rs = clamp_sample(opm_out.data[1]);
				int16_t hl = ls, hr = rs;
				if (s_effective_mute != 0) {
					ymfm::ym2151::output_data host = opm_out;
					apply_mute(host);
					hl = clamp_sample(host.data[0]);
					hr = clamp_sample(host.data[1]);
				}
				host_output[s] = hl;
				host_output[s + 1] = hr;
				output[s++] = ls;
				output[s++] = rs;
				// Re-test per sample rather than hoisting: capture_sample()
				// decrements the countdown, and numsamples is arbitrary, so a
				// hoisted test would keep decrementing past zero and wrap the
				// unsigned counter — leaving capture armed forever.
				if (m_scope_armed > 0) {
					capture_sample(hl, hr);
				}
			}
		}

		uint8_t read_status() const {
			return m_bus.status();
		}

		bool irq() const {
			return m_bus.irq;
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

		bool debug_busy() const {
			return m_bus.busy;
		}

		// $19 is one write address for two depths, split on bit 7, so only
		// the chip knows both.
		void debug_lfo_depths(uint8_t *am, uint8_t *pm) {
			const ymfm::opm_registers &regs = m_chip.engine().regs();
			*am = (uint8_t)regs.lfo_am_depth();
			*pm = (uint8_t)regs.lfo_pm_depth();
		}

		// ─── Save states ────────────────────────────────────────────────

		int16_t (&noise_waveform())[ym_register_fields::LFO_LENGTH] {
			ymfm::opm_registers &regs = m_chip.engine().regs();
			return (regs.*ym_register_fields::lfo_waveform())[ym_register_fields::NOISE_WAVEFORM];
		}

		// The LFO's counters and latches, which ymfm's own state leaves out.
		// A new field must reach lfo_to_image() and lfo_from_image().
		static constexpr size_t LFO_IMAGE_SIZE = 12;
		static_assert(sizeof(ymfm::opm_registers::lfo_state) == 12, "lfo_state changed: update its image");

		void lfo_to_image(uint8_t img[LFO_IMAGE_SIZE]) {
			const ymfm::opm_registers::lfo_state &lfo = m_chip.engine().regs().lfo();
			img[0]  = (uint8_t)(lfo.count & 0xff);
			img[1]  = (uint8_t)(lfo.count >> 8);
			img[2]  = (uint8_t)(lfo.ticks & 0xff);
			img[3]  = (uint8_t)(lfo.ticks >> 8);
			img[4]  = lfo.rate_steps;
			img[5]  = lfo.window;
			img[6]  = lfo.pm;
			img[7]  = lfo.pm_late;
			img[8]  = lfo.am_late;
			img[9]  = lfo.steps;
			img[10] = lfo.reload ? 1 : 0;
			img[11] = 0; // reserved
		}

		void lfo_from_image(const uint8_t img[LFO_IMAGE_SIZE]) {
			ymfm::opm_registers::lfo_state &lfo = m_chip.engine().regs().lfo();
			lfo.count      = (uint16_t)(img[0] | img[1] << 8);
			lfo.ticks      = (uint16_t)(img[2] | img[3] << 8);
			lfo.rate_steps = img[4];
			lfo.window     = img[5] & 7;
			lfo.pm         = img[6];
			lfo.pm_late    = img[7];
			lfo.am_late    = img[8];
			lfo.steps      = img[9];
			lfo.reload     = img[10] & 1;
			// A count of 0 would never reach its next step.
			if (lfo.ticks == 0) {
				lfo.ticks = 1;
			}
		}

		void state_save(ss_writer *w) {
			ym_engine &eng = m_chip.engine();
			const uint32_t active   = (eng.*ym_engine_fields::active());
			const uint32_t modified = (eng.*ym_engine_fields::modified());
			const uint32_t prepare  = (eng.*ym_engine_fields::prepare());

			std::vector<uint8_t> blob;
			{
				ymfm::ymfm_saved_state saver(blob, true);
				m_chip.save_restore(saver);
			}
			// Undo the invalidation save_restore() did, so saving only reads.
			(eng.*ym_engine_fields::modified()) = modified;

			ss_put_u32(w, (uint32_t)blob.size());
			ss_put(w, blob.data(), blob.size());
			ss_put_u32(w, active);
			ss_put_u32(w, modified);
			ss_put_u32(w, prepare);
			ss_put(w, m_regs, sizeof m_regs);
			ss_put(w, m_keyon, sizeof m_keyon);
			for (int16_t v : noise_waveform()) {
				ss_put_u16(w, (uint16_t)v);
			}
			uint8_t img[opm_bus::IMAGE_SIZE];
			m_bus.to_image(img);
			ss_put(w, img, sizeof img);
			uint8_t lfo[LFO_IMAGE_SIZE];
			lfo_to_image(lfo);
			ss_put(w, lfo, sizeof lfo);
		}

		void state_load(ss_reader *r, int version) {
			std::vector<uint8_t> blob(ss_get_u32(r));
			ss_get(r, blob.data(), blob.size());
			const uint32_t active   = ss_get_u32(r);
			const uint32_t modified = ss_get_u32(r);
			const uint32_t prepare  = ss_get_u32(r);
			// Version 2 counted phiM clocks to each timer's expiry and to the
			// end of busy, and kept the IRQ line.
			uint32_t v2_timer[2] = { 0, 0 };
			if (version < 3) {
				v2_timer[0] = ss_get_u32(r);
				v2_timer[1] = ss_get_u32(r);
				(void)ss_get_u32(r);
				(void)ss_get_bool(r);
			}
			ss_get(r, m_regs, sizeof m_regs);
			ss_get(r, m_keyon, sizeof m_keyon);
			int16_t noise[ym_register_fields::LFO_LENGTH];
			for (int16_t &v : noise) {
				v = (int16_t)ss_get_u16(r);
			}
			uint8_t img[opm_bus::IMAGE_SIZE] = {};
			if (version >= 3) {
				ss_get(r, img, sizeof img);
			}
			// Version 3 had no LFO image: the LFO starts over.
			uint8_t lfo[LFO_IMAGE_SIZE] = {};
			if (version >= 4) {
				ss_get(r, lfo, sizeof lfo);
			}
			if (r->failed) {
				return;
			}
			std::copy(std::begin(noise), std::end(noise), std::begin(noise_waveform()));

			{
				ymfm::ymfm_saved_state restorer(blob, false);
				m_chip.save_restore(restorer);
			}
			ym_engine &eng = m_chip.engine();
			(eng.*ym_engine_fields::active())   = active;
			(eng.*ym_engine_fields::modified()) = modified;
			(eng.*ym_engine_fields::prepare())  = prepare;

			if (version >= 3) {
				m_bus.from_image(img);
			} else {
				bus_from_v2(v2_timer);
			}
			if (version >= 4) {
				lfo_from_image(lfo);
			} else {
				m_chip.engine().regs().lfo() = ymfm::opm_registers::lfo_state{};
				m_chip.engine().regs().lfo().ticks = (uint16_t)(1u << (15 - (m_regs[0x18] >> 4)));
			}

			// Each operator caches values derived from the registers (phase
			// step, envelope rates, waveform), rebuilt only when its channel is
			// prepared. A channel the saved chip did not have marked modified
			// will not be prepared soon, so rebuild every cache now, from the
			// registers just restored -- which is exactly what the saved chip's
			// caches were built from.
			for (uint32_t op = 0; op < YM_NUM_OPERATORS; op++) {
				auto *fmop = eng.debug_operator(op);
				if (fmop != nullptr) {
					fmop->regs().cache_operator_data(fmop->choffs(), fmop->opoffs(), fmop->debug_cache());
				}
			}
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
			scratch.m_bus         = m_bus;
			scratch.m_chip.engine().regs().lfo() = m_chip.engine().regs().lfo();
			scratch.m_report_lost = false;
			scratch.m_scope_armed = 0;

			ymfm::ym2151::output_data out;
			for (unsigned i = 0; i < n; i++) {
				// Mirror generate()'s per-sample work, including the interface
				// cycles, so pending writes and CSM key-ons show up.
				scratch.run(32u - scratch.m_bus.cycle);
				scratch.m_chip.generate(&out);
				scratch.apply_mute(out);
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

		// Version 2 kept phiM clocks left to each expiry. The interface's
		// registers are rebuilt from the register shadow and its flags from
		// the engine's status; each count resumes at the sample it implies.
		void bus_from_v2(const uint32_t left[2]) {
			opm_bus b;
			b.mode_write(0x10, m_regs[0x10]);
			b.mode_write(0x11, m_regs[0x11]);
			b.mode_write(0x12, m_regs[0x12]);
			b.mode_write(0x14, (uint8_t)(m_regs[0x14] & 0x8f));
			const uint8_t st = m_chip.read_status();
			b.a_flag = st & 1;
			b.b_flag = st & 2;
			b.irq = b.a_flag || b.b_flag;
			b.a_load = b.a_load_reg;
			b.a_temp = b.a_do_reset = !b.a_load;
			b.b_temp = b.b_do_reset = !b.b_load_reg;
			if (b.a_load) {
				const uint32_t samples = std::min<uint32_t>((left[0] + 63) / 64, 1024);
				b.a_val = (uint16_t)(1024 - std::max<uint32_t>(samples, 1));
			}
			if (b.b_load_reg) {
				const uint32_t samples = std::min<uint32_t>((left[1] + 63) / 64, 4096);
				const uint32_t ticks = std::max<uint32_t>((samples + 15) / 16, 1);
				b.b_val = (uint8_t)(256 - std::min<uint32_t>(ticks, 256));
				b.b_sub = (uint8_t)((16 - samples % 16) % 16);
			}
			m_bus = b;
		}

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

		// Replace `out` (a copy of what generate() produced) with the mix minus
		// the muted channels. generate() itself always runs every channel, and
		// must: output() also computes each channel's operator-1 feedback input,
		// which the next clock() consumes, so leaving a channel out of generate()
		// would change how it sounds afterwards and what a save state holds.
		// This second pass recomputes those same values from unchanged inputs
		// (the argument sample_streams() below relies on too), so it changes
		// nothing but the samples handed to the host.
		void apply_mute(ymfm::ym2151::output_data &out) {
			if (s_effective_mute == 0) {
				return;
			}
			m_chip.engine().output(out.clear(), 0, 32767, 0xffu & ~(uint32_t)s_effective_mute);
			out.roundtrip_fp();
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
		opm_bus m_bus;
		bool m_report_lost = true;

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

	void YM_stream_update(uint16_t* output, uint16_t* host_output, uint32_t numsamples) {
		if (initialized) opm_iface.generate((int16_t*)output, (int16_t*)host_output, numsamples);
	}

	void YM_sync(uint8_t cycle) {
		if (initialized) opm_iface.sync(cycle);
	}

	bool YM_write(uint8_t port, uint8_t val) {
		return initialized ? opm_iface.write(port, val) : false;
	}

	void YM_reset(void) {
		opm_iface.reset();
	}

	void YM_set_lost_write_handler(void (*handler)(uint8_t reg, uint8_t val)) {
		s_lost_handler = handler;
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

	void YM_state_save(ss_writer *w) {
		ss_begin(w, "YM  ", 4);
		ss_put_bool(w, initialized);
		if (initialized) opm_iface.state_save(w);
		ss_end(w);
	}

	void YM_state_load(ss_reader *r) {
		const int version = ss_open(r, "YM  ");
		if (ss_get_bool(r)) opm_iface.state_load(r, version);
		ss_close(r);
	}

	static void update_effective_mute() {
		s_effective_mute = s_mute_mask | YM_debug_get_client_mute_mask();
	}

	void YM_debug_set_mute_mask(uint8_t mask) {
		s_mute_mask = mask;
		update_effective_mute();
	}

	uint8_t YM_debug_get_mute_mask() {
		return s_mute_mask;
	}

	void YM_debug_set_owner_mute_mask(voice_mute_owner_t owner, uint8_t mask) {
		if ((int)owner < 0 || owner >= VOICE_MUTE_OWNERS) return;
		s_owner_mute[owner] = mask;
		update_effective_mute();
	}

	uint8_t YM_debug_get_owner_mute_mask(voice_mute_owner_t owner) {
		return ((int)owner < 0 || owner >= VOICE_MUTE_OWNERS) ? 0 : s_owner_mute[owner];
	}

	uint8_t YM_debug_get_client_mute_mask() {
		uint8_t mask = 0;
		for (uint8_t m : s_owner_mute) mask |= m;
		return mask;
	}

	uint8_t YM_debug_get_effective_mute_mask() {
		return s_effective_mute;
	}

	void YM_debug_get_lfo_depths(uint8_t *am, uint8_t *pm) {
		opm_iface.debug_lfo_depths(am, pm);
	}

	uint32_t YM_debug_phase_step(const struct ym_debug_phase *in) {
		static ymfm::opm_registers regs;
		uint32_t chan, opmask;
		regs.reset();
		regs.write(0x19, 0xff, chan, opmask);
		regs.write(0x28, in->kc, chan, opmask);
		regs.write(0x30, (uint8_t)(in->kf << 2), chan, opmask);
		regs.write(0x38, (uint8_t)(in->pms << 4), chan, opmask);
		regs.write(0x40, (uint8_t)(in->dt1 << 4 | in->mul), chan, opmask);
		regs.write(0xc0, (uint8_t)(in->dt2 << 6), chan, opmask);
		ymfm::opdata_cache cache;
		regs.cache_operator_data(0, 0, cache);
		return regs.compute_phase_step(0, 0, cache, in->pm);
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
