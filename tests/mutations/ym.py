"""Mutations for the YM2151 and its interface."""

MUTATIONS = [
    # YM2151 interface. Expected values: Nuked-OPM, tests/accuracy/ym2151.
    {
        "name": "YM2151 busy rises with the strobe",
        "file": "src/ymglue.cpp",
        "find": "busy = (!(busy_cnt >> 5) && busy) || wd_en;",
        "into": "busy = (!(busy_cnt >> 5) && busy) || wd_en || wd;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 busy lasts 16 cycles",
        "file": "src/ymglue.cpp",
        "find": "busy = (!(busy_cnt >> 5) && busy) || wd_en;",
        "into": "busy = (!(busy_cnt >> 4) && busy) || wd_en;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 refuses writes while busy",
        "file": "src/ymglue.cpp",
        "find": "\t\tconst bool clobbers = wa || wd || wa_en || wd_en;\n",
        "into": "\t\tif (busy) return false;\n\t\tconst bool clobbers = wa || wd || wa_en || wd_en;\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 a write reaches the chip a cycle early",
        "file": "src/ymglue.cpp",
        "find": "\t\t\twa = true;\n",
        "into": "\t\t\twa_en = true;\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 data waits for no slot",
        "file": "src/ymglue.cpp",
        "find": "if (data_ok && !data_done && scans(addr, cycle)) {",
        "into": "if (data_ok && !data_done) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 channel registers wait for a 32-cycle slot",
        "file": "src/ymglue.cpp",
        "find": "return a < 0x40 ? (a & 7) == (cycle & 7)",
        "into": "return a < 0x40 ? (a & 7) == cycle",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 an address write keeps pending data",
        "file": "src/ymglue.cpp",
        "find": "data_ok = data_ok && !wa_en;",
        "into": "data_ok = data_ok && true;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer registers wait for the slot scan",
        "file": "src/ymglue.cpp",
        "find": "if (wd_en && mode_addr < 0x20) {",
        "into": "if (wd_en && mode_addr < 0x10) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer A counts twice a sample",
        "file": "src/ymglue.cpp",
        "find": "a_inc = test_timers || (a_load && cycle == 0);",
        "into": "a_inc = test_timers || (a_load && (cycle & 15) == 0);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer B prescaler divides by 8",
        "file": "src/ymglue.cpp",
        "find": "if (cycle == 0) b_sub++;\n\t\tb_sub_of = (b_sub >> 4) & 1;",
        "into": "if (cycle == 0) b_sub++;\n\t\tb_sub_of = (b_sub >> 3) & 1;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer B prescaler stops while idle",
        "file": "src/ymglue.cpp",
        "find": "\t\t\t\tb_sub++;\n",
        "into": "",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LOAD is taken on any cycle",
        "file": "src/ymglue.cpp",
        "find": "if (cycle == 1) a_load = a_load_reg;",
        "into": "a_load = a_load_reg;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 timer flag set without IRQEN",
        "file": "src/ymglue.cpp",
        "find": "a_flag = !a_reset && (a_flag || (a_irqen && a_of));",
        "into": "a_flag = !a_reset && (a_flag || a_of);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 RESET leaves timer B's flag",
        "file": "src/ymglue.cpp",
        "find": "b_flag = !b_reset && (",
        "into": "b_flag = (",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 a settled sample is skipped whole",
        "file": "src/ymglue.cpp",
        "find": "if (cycle >= 1 && settled()) {",
        "into": "if (settled()) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 IRQ follows the flags in the same cycle",
        "file": "src/ymglue.cpp",
        "find": "\t\tcycle = (cycle + 1) & 31;\n\t}",
        "into": "\t\tirq = a_flag || b_flag;\n\t\tcycle = (cycle + 1) & 31;\n\t}",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    # YM2151 phase generator and LFO. Expected values: Nuked-OPM, tests/accuracy/ym2151.
    {
        "name": "YM2151 phase: PM's positive zero skips no gap note",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_phase_pm.vec
        "find": "if (sum < 0x2000 && (bitfield(sum, 6, 2) == 3 || carry))",
        "into": "if (sum < 0x2000 && carry)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase: PMS 6 does not skip the gap notes",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_phase_pm.vec
        "find": "amount = (mag + (mag >= 0x60 ? 0x20 : 0)) << 1;",
        "into": "amount = mag << 1;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase: PMS 7 does not skip the gap notes",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_phase_pm.vec
        "find": "s_spread[8] = { 0, 1, 2, 4, 5, 6, 8, 9 };",
        "into": "s_spread[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase: DT2 lands on gap notes",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_phase_static.vec
        "find": "s_step[3][4] = { { 0, 0, 0, 0 }, { 1, 1, 2, 2 }, { 2, 3, 3, 3 } };",
        "into": "s_step[3][4] = { { 0, 0, 0, 0 }, { 1, 1, 1, 1 }, { 2, 2, 2, 2 } };",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase: DT1 from the key code before PM and DT2",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_phase_static.vec
        "find": "std::min<uint32_t>(code >> 8, 0x1c)",
        "into": "std::min<uint32_t>(cache.block_freq >> 8, 0x1c)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase: the step is not cut to 17 bits",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_phase_static.vec
        "find": "\tphase_step &= 0x1ffff;\n",
        "into": "\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase: PM depth 0 still applies the latch",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_phase_pm.vec
        "find": "(m_lfo_depth[(m_lfo.window - 1) & 7].pm[1] != 0) ? bitfield(lfo_raw_pm, (opoffs >= 23) ? 8 : 0, 8) : 0;",
        "into": "bitfield(lfo_raw_pm, (opoffs >= 23) ? 8 : 0, 8);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO: PM splits at operator 24",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_pm_split.vec
        "find": "(opoffs >= 23) ? 8 : 0",
        "into": "(opoffs >= 24) ? 8 : 0",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO: AM splits at operator 17",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_lfo_am_split.vec
        "find": "(m_opoffs >= 16) ? 16 : 0",
        "into": "(m_opoffs >= 17) ? 16 : 0",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO: square PM is 127 deep",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_square_pm_am.vec
        "find": "\t\t\tvalue = 128;\n",
        "into": "\t\t\tvalue = 127;\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO: triangle steps by 1",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_triangle_am.vec
        "find": "lfo.count += (waveform == 2) ? 2 : 1;",
        "into": "lfo.count += 1;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO: rate multiplier takes the low bits LSB first",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_rates.vec
        "find": "bitfield(lfo_rate(), 3 - bit)",
        "into": "bitfield(lfo_rate(), bit)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO: a rate write in the tick's cycle reloads at once",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_reload.vec
        "find": "m_lfo.window == 7 && m_write_cycle > LFO_RELOAD_CYCLE",
        "into": "m_lfo.window == 7 && m_write_cycle > LFO_RELOAD_CYCLE + 1",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO: PM latched after the tick",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_saw_pm.vec
        "find": "uint32_t const pm_value = lfo_pm_value(waveform, lfo.count);",
        "into": "uint32_t const pm_value = lfo_pm_value(waveform, lfo.count + 1);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    # YM2151 output framing and noise. Expected values: Nuked-OPM, tests/accuracy/ym2151.
    {
        "name": "YM2151 output: left takes slot 15 in time",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_slot_framing.vec
        "find": "(output == 0) ? opoffs >= 15 : opoffs == 31",
        "into": "(output == 0) ? opoffs >= 16 : opoffs == 31",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 output: right takes slot 30 late",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_slot_framing.vec
        "find": "(output == 0) ? opoffs >= 15 : opoffs == 31",
        "into": "(output == 0) ? opoffs >= 15 : opoffs >= 30",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 output: the late part is dropped",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_slot_framing.vec
        "find": "m_late_prev[index] = m_late[index];",
        "into": "m_late_prev[index] = 0;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: LFSR taps bit 3",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_noise_rates.vec
        "find": "bitfield(ring, 2) ^ extra",
        "into": "bitfield(ring, 3) ^ extra",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: output taken at cycle 13",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_noise_rates.vec
        "find": "if (cycle == 12)",
        "into": "if (cycle == 13)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: timer counts at cycles 14 and 30",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_noise_rates.vec
        "find": "if ((cycle & 15) == 15)",
        "into": "if ((cycle & 15) == 14)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: a $0F write takes the whole sample",
        "file": "src/ymglue.cpp",
        # ym_noise_writes.vec, ym_noise_frequency_writes.vec
        "find": "noise_frequency_write(e.value, e.cycle);",
        "into": "noise_frequency_write(e.value, 0);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: a $0F frequency waits a sample",
        "file": "src/ymglue.cpp",
        # ym_noise_frequency_writes.vec
        "find": "queue(0, a, v, LATE_NOISE, m_bus.cycle);",
        "into": "queue(1, a, v, LATE_NOISE, m_bus.cycle);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: enable taken a cycle later",
        "file": "src/ymglue.cpp",
        # ym_noise_enable_writes.vec
        "find": "queue(m_bus.cycle >= NOISE_ENABLE_CYCLE ? 1 : 0, a, v, LATE_WRITE);",
        "into": "queue(m_bus.cycle > NOISE_ENABLE_CYCLE ? 1 : 0, a, v, LATE_WRITE);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: enable waits a sample",
        "file": "src/ymglue.cpp",
        # ym_noise_enable_writes.vec
        "find": "queue(m_bus.cycle >= NOISE_ENABLE_CYCLE ? 1 : 0, a, v, LATE_WRITE);",
        "into": "queue(m_bus.cycle >= NOISE_ENABLE_CYCLE ? 2 : 1, a, v, LATE_WRITE);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: channel 7 left out while the noise is off",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_noise_enable_unkeyed.vec
        "find": "((has_output_late<RegisterType>::value || m_regs.noise_enable()) ? 0x80 : 0)",
        "into": "((m_regs.noise_enable()) ? 0x80 : 0)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 5 ignored",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_full_level.vec
        "find": "{ return test_bit(int32_t(opoffs) + 1, 5); }",
        "into": "{ return 0 * opoffs; }",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 5 waits a sample",
        "file": "src/ymglue.cpp",
        # ym_test_full_level.vec
        "find": "TEST_FAST_WAIT = 0;",
        "into": "TEST_FAST_WAIT = 1;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: every slot takes a write in its sample",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_full_level.vec
        "find": "&& at >= m_test_cycle[index])",
        "into": "&& at + 32 >= m_test_cycle[index])",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: slots take a write a cycle late",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_full_level.vec
        "find": "test_bit(int32_t(opoffs) + 1, 5)",
        "into": "test_bit(int32_t(opoffs), 5)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 5 leaves a silent envelope silent",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_test_full_level.vec
        "find": "if (env_level() <= EG_QUIET || m_regs.test_full_level(m_opoffs))",
        "into": "if (env_level() <= EG_QUIET)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 5 leaves idle channels out",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_test_full_level.vec
        "find": "uint32_t const computed = m_regs.test_all_channels() ? 0xffffffff :",
        "into": "uint32_t const computed = false ? 0xffffffff :",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 4 ignored",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_output_bit.vec
        "find": "{ return test_bit(int32_t(opoffs) + 7, 4); }",
        "into": "{ return 0 * opoffs; }",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 4 waits for the engine",
        "file": "src/ymglue.cpp",
        # ym_test_output_bit.vec
        "find": "TEST_FAST = 0x39;",
        "into": "TEST_FAST = 0x29;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 4 read at the envelope",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_output_bit.vec
        "find": "test_bit(int32_t(opoffs) + 7, 4)",
        "into": "test_bit(int32_t(opoffs) + 1, 4)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 4 not sign-extended",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_test_output_bit.vec
        "find": "result = int16_t(uint16_t(result ^ 0x2000) << 2) >> 2;",
        "into": "result ^= 0x2000;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 4 leaves idle channels out",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_output_bit.vec
        "find": "return seen & 0x30;",
        "into": "return seen & 0x20;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: a second write in a sample hides the first",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_write_pairs.vec
        "find": "if (m_test_writes == 0)",
        "into": "if (true)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 3 ignored",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_test_phase_reset.vec
        "find": "if (m_regs.test_phase_reset(m_opoffs))",
        "into": "if (false)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 3 read in the sample it is written",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_phase_reset.vec
        "find": "test_bit(int32_t(opoffs) + 7 - 32, 3)",
        "into": "test_bit(int32_t(opoffs) + 7, 3)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 3 read a cycle late",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_phase_reset.vec
        "find": "test_bit(int32_t(opoffs) + 7 - 32, 3)",
        "into": "test_bit(int32_t(opoffs) + 6 - 32, 3)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 3 not carried to the sample after",
        "file": "src/ymglue.cpp",
        # ym_test_phase_reset.vec
        "find": "queue(TEST_FAST_WAIT + 1, 0, v, LATE_CARRY, m_bus.cycle);",
        "into": "",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 3 carried without the value before",
        "file": "src/ymglue.cpp",
        # ym_test_phase_reset.vec
        "find": "m_late[j].addr = test;",
        "into": "m_late[j].addr = e.value;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 0 leaves the operators' envelopes alone",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_eg_clock.vec
        "find": "{ return test_bit(int32_t(opoffs), 0); }",
        "into": "{ return 0 * opoffs; }",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 0 read a cycle early by the envelopes",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_eg_clock.vec
        "find": "test_bit(int32_t(opoffs), 0)",
        "into": "test_bit(int32_t(opoffs) + 1, 0)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 0 leaves the envelope timer alone",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_eg_clock.vec
        "find": "{ return test_bit(0, 0); }",
        "into": "{ return 0; }",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 0 reaches the envelope timer in its sample",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_test_eg_clock.vec
        "find": "{ return test_bit(0, 0); }",
        "into": "{ return test_bit(1, 0); }",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 0 steps the timer on the divider's own cycle too",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_test_eg_clock.vec
        "find": "else if (m_regs.test_eg_timer())",
        "into": "if (m_regs.test_eg_timer())",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 2 ignored by the timers",
        "file": "src/ymglue.cpp",
        # ym_timer_test.vec
        "find": "case 0x01: test_timers = v & 0x04; break;",
        "into": "case 0x01: break;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 2 leaves timer A alone",
        "file": "src/ymglue.cpp",
        # ym_timer_test.vec
        "find": "a_inc = test_timers || (a_load && cycle == 0);",
        "into": "a_inc = a_load && cycle == 0;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 test register: bit 2 leaves timer B alone",
        "file": "src/ymglue.cpp",
        # ym_timer_test.vec
        "find": "b_inc = test_timers || (b_load_reg && b_sub_of);",
        "into": "b_inc = b_load_reg && b_sub_of;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 a channel left out drops what it held",
        "file": "src/extern/ymfm/src/ymfm_fm.h",
        # ym_test_full_level.vec
        "find": "m_feedback_in = 0; m_op2_in = 0; mix_late(output); }",
        "into": "m_feedback_in = 0; m_op2_in = 0; (void)output; }",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 reset leaves the channels' outputs on",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_reset_outputs.vec
        "find": "std::fill_n(&m_regdata[0], REGISTERS, 0);",
        "into": "std::fill_n(&m_regdata[0], REGISTERS, 0); m_regdata[0x27] = 0xc0;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: level keeps its low bit",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_noise_level.vec
        "find": "return int32_t((level & ~1u) << 2);",
        "into": "return int32_t(level << 2);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: low bits set at level 0",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_noise_level.vec
        "find": "if (level != 0)",
        "into": "if (level != 0 || true)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: silent with nothing keyed",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_noise_level.vec
        "find": "(m_active_channels | ((has_output_late<RegisterType>::value || m_regs.noise_enable()) ? 0x80 : 0));",
        "into": "(m_active_channels);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 envelope steps per cycle",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_envelopes.vec, ym_key_timing.vec
        "find": "static constexpr bool envelope_by_sample() { return true; }",
        "into": "static constexpr bool envelope_by_sample() { return false; }",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 envelope output reads the level after its step",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_envelopes.vec
        "find": "return envelope_by_sample<RegisterType>::value ? m_env_out : m_env_attenuation;",
        "into": "return m_env_attenuation;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 envelope high rates step in ymfm's order",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_envelopes.vec
        "find": "(0u - (env_counter >> 2)) & 3",
        "into": "(env_counter >> 2) & 3",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 reset keeps the envelope counter",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_envelopes.vec, run after a reset
        "find": "\tm_env_counter = 0;",
        "into": "",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase does not restart at key-on",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_envelopes.vec
        "find": "if (m_phase_restart)",
        "into": "if (false && m_phase_restart)",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 phase steps on this sample's cache",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_op_writes.vec
        "find": "clock_phase(lfo_raw_pm, m_phase_cache);",
        "into": "clock_phase(lfo_raw_pm, m_cache);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 key latch copied at another cycle",
        "file": "src/ymglue.cpp",
        # ym_key_timing.vec
        "find": "const uint8_t at = (uint8_t)(24 + ch);",
        "into": "const uint8_t at = (uint8_t)(16 + ch);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 key-on reaches the engine as written",
        "file": "src/ymglue.cpp",
        # ym_key_latch.vec
        "find": "m_kon_latch = v & 0x7f;",
        "into": "m_kon_latch = v & 0x7f; to_engine(0x08, m_kon_latch, m_bus.cycle);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 operator writes wait for the engine",
        "file": "src/ymglue.cpp",
        # ym_op_writes.vec
        "find": "} else if (a >= 0x20) {",
        "into": "} else if (a >= 0xff) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 CSM key-on holds until the next write",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_csm_c2.vec, ym_csm_m1.vec
        "find": "if (bitfield(chanmask, chnum) && m_channel[chnum]->key_pending())",
        "into": "if (false && m_channel[chnum]->key_pending())",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 CSM key-on reaches the engine a sample late",
        "file": "src/ymglue.cpp",
        # ym_csm_c2.vec, ym_csm_phase.vec
        "find": "queue(0, CSM_FIRST, 0, LATE_CSM);",
        "into": "queue(1, CSM_FIRST, 0, LATE_CSM);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 CSM key-on: slots 0-4 take it with the rest",
        "file": "src/ymglue.cpp",
        # ym_csm_m1.vec
        "find": "queue(1, CSM_LATER, 0, LATE_CSM);",
        "into": "queue(0, CSM_LATER, 0, LATE_CSM);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 CSM key-on: slots 0-1 take it early",
        "file": "src/ymglue.cpp",
        # ym_csm_m1.vec
        "find": "const bool later = ch <= 4;",
        "into": "const bool later = ch >= 2 && ch <= 4;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 envelope: high rates double on the table's cycles",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_env_high_rates.vec
        "find": "{ 0x0, 0x8, 0xa, 0xb };",
        "into": "{ 0x0, 0x8, 0xa, 0xe };",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 envelope: high rates step past 8",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_env_high_rates.vec
        "find": "return 1u << std::min<uint32_t>(shift, 3);",
        "into": "return 1u << std::min<uint32_t>(shift, 4);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO depth writes wait a sample",
        "file": "src/ymglue.cpp",
        # ym_lfo_am_depth_late.vec
        "find": "queue(DEPTH_WAIT, a, v, LATE_DIRECT, m_bus.cycle);",
        "into": "queue(DEPTH_WAIT + 1, a, v, LATE_DIRECT, m_bus.cycle);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 direct writes wait a sample too long",
        "file": "src/ymglue.cpp",
        # ym_key_timing.vec, ym_op_writes.vec
        "find": "queue(DIRECT_WAIT, a, v, LATE_DIRECT, cycle);",
        "into": "queue(DIRECT_WAIT + 1, a, v, LATE_DIRECT, cycle);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    # YM2151 LFO multiply. Expected values: Nuked-OPM, tests/accuracy/ym2151.
    {
        "name": "YM2151 LFO depth writes wait for the engine",
        "file": "src/ymglue.cpp",
        # ym_lfo_am_depth_writes.vec, ym_lfo_pm_depth_writes.vec
        "find": "} else if (a == 0x19) {",
        "into": "} else if (false) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO multiply reads the depth at its end",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_am_depth_writes.vec, ym_lfo_pm_depth_writes.vec
        "find": "int32_t const cycle = start + bit + 7 - step;",
        "into": "int32_t const cycle = end + 0 * start;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO multiply takes depth bits LSB first",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_am_depth_writes.vec
        "find": "depth.am[half], 6 - step))",
        "into": "depth.am[half], step))",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 LFO multiply ends a step early",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        # ym_lfo_am_depth_writes.vec, ym_lfo_pm_depth_writes.vec
        "find": "static constexpr int32_t LFO_AM_END = 110;",
        "into": "static constexpr int32_t LFO_AM_END = 94;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise PM: last step reads the value at the tick",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_noise_pm_d0.vec
        "find": "(waveform == 3 && lfo.ticks == 1) ? (pm_sign ? 0xff : 0) : pm_mag;",
        "into": "pm_mag;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise LFO: a second step takes the first step's bits",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_noise_am_d3.vec, ym_lfo_noise_am_f7.vec
        "find": "(lfo.steps == 2) ? 47 : 31",
        "into": "(lfo.steps == 2) ? 31 : 31",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 noise: LFO takes bits from cycle 30",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        # ym_lfo_noise_am_d0.vec
        "find": "(lfo.steps == 2) ? 47 : 31",
        "into": "(lfo.steps == 2) ? 47 : 30",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    # YM2151 operator modulation and DAC. Expected values: Nuked-OPM, tests/accuracy/ym2151.
    {
        "name": "YM2151 algorithms: M2 takes C1 in time",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_algorithms.vec
        "find": "\t\tlast[2] = m_op2_last;\n",
        "into": "\t\tlast[2] = opout[2];\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 algorithms: M2 takes M1 in time",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_algorithms.vec
        "find": "\t\tlast[1] = m_feedback[1];\n",
        "into": "\t\tlast[1] = opout[1];\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 algorithms: C2 takes C1 in time",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        # ym_algorithms.vec
        "find": "opmod = int16_t(m_op2_last + opout[3]) >> 1;",
        "into": "opmod = opout[7] >> 1;",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 DAC: overflow gives 32767",
        "file": "src/extern/ymfm/src/ymfm.h",
        # ym_output_clamp.vec
        "find": "\tif (value > 32767)\n\t\tvalue = 32767;\n",
        "into": "\tif (value > 32767)\n\t\treturn 32767;\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "the sound chips step the whole instruction after catching up",
        "file": "src/audio.c",
        "find": "audio_step(clocks > step_consumed ? (int)(clocks - step_consumed) : 0);",
        "into": "audio_step((int)clocks);",
        "count": 1,
        "caught_by": ["audio_catch_up"],
    },
    {
        "name": "the sound chips' catch-up count outlives the instruction",
        "file": "src/audio.c",
        "find": "(int)(clocks - step_consumed) : 0);\n\tstep_consumed = 0;",
        "into": "(int)(clocks - step_consumed) : 0);",
        "count": 1,
        "caught_by": ["audio_catch_up"],
    },
    {
        "name": "YM2151 an operator reads a channel register latched at its own cycle",
        "file": "src/extern/ymfm/src/ymfm_opm.h",
        "find": "choffs + 8 * scan < cycle",
        "into": "choffs + 8 * scan <= cycle",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 channel registers latched in a sample wait for the next",
        "file": "src/ymglue.cpp",
        "find": "\t\t\t\t\tm_chip.engine().regs().ch_latch_early(e.addr, e.value, e.cycle);\n",
        "into": "",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 early frequency and LFO sensitivity skip the operator cache",
        "file": "src/ymglue.cpp",
        "find": "if (e.addr >= 0x28) {",
        "into": "if (false) {",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 early channel registers outlast their sample",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        "find": "\t\tset = 0;\n",
        "into": "\t\tset = set;\n",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 an operator's output stage reads RL at its own slot",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        "find": "uint32_t const at = ops[term]->opoffs() + 16;",
        "into": "uint32_t const at = ops[term]->opoffs();",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 an operator's output stage reads CON at its own slot",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        "find": "alg_out[k] = m_regs.ch_algorithm_at(m_choffs, slot + 16);",
        "into": "alg_out[k] = m_regs.ch_algorithm_at(m_choffs, slot);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 operator inputs read CON at the output stage",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        "find": "alg_in[k] = m_regs.ch_algorithm_at(m_choffs, slot);",
        "into": "alg_in[k] = m_regs.ch_algorithm_at(m_choffs, slot + 16);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 AMS is read once per channel",
        "file": "src/extern/ymfm/src/ymfm_fm.ipp",
        "find": "am[k] = m_regs.lfo_am_offset_at(m_choffs, slot);",
        "into": "am[k] = m_regs.lfo_am_offset_at(m_choffs, 0);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
    {
        "name": "YM2151 KC and KF are read once per channel",
        "file": "src/extern/ymfm/src/ymfm_opm.cpp",
        "find": "cache.block_freq = ch_block_freq_at(choffs, opoffs);",
        "into": "cache.block_freq = ch_block_freq(choffs);",
        "count": 1,
        "caught_by": ["ym_accuracy"],
    },
]
