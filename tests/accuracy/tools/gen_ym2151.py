#!/usr/bin/env python3
"""Generate YM2151 bus vectors from Nuked-OPM.

    gen_ym2151.py TB_OPM OUT_DIR --commit SHA

TB_OPM is tb_opm.c built against Nuked-OPM at commit SHA.
"""

import argparse
import os
import subprocess

SAMPLE = 32          # OPM_Clock calls per sample
SETTLE = 2           # samples each block starts after the one a script asks for
START = 320


class S:
    def __init__(self):
        self.ops = []

    def _op(self, t, text):
        self.ops.append((t, len(self.ops), text))

    def w(self, p, v, t):
        self._op(t, "w %d %02x %d" % (p, v & 255, t))

    def reg(self, a, v, t):
        """Address, then data 4 cycles later. Returns the time busy has cleared."""
        self.w(0, a, t)
        self.w(1, v, t + 4)
        return t + 4 + 40

    def r(self, t):
        self._op(t, "r %d" % t)

    def g(self, a, t):
        self._op(t, "g %02x %d" % (a, t))

    def x(self, t0, t1):
        self._op(t0, "x %d %d" % (t0, t1))

    def o(self, n0, n, side):
        self._op(SAMPLE * n0, "o %d %d %s" % (n0 + SETTLE, n, side))

    def text(self):
        return "\n".join(o[2] for o in sorted(self.ops)) + "\ne\n"


def busy_phase():
    s = S()
    for k in range(32):
        t = START + 101 * k
        s.w(0, 0x30, t - 10)
        s.x(t - 2, t + 40)
        s.w(1, 0x10, t)
    return s


def address_no_busy():
    s = S()
    s.x(START, START + 64 * 13)
    for k in range(12):
        s.w(0, 0x20 + k, START + 4 + 67 * k)
    return s


def spacing():
    """Address then data g cycles apart. Data values name registers, to show where they land."""
    s = S()
    t = START
    for g in range(4):
        for ph in range(0, 32, 5):
            c = (g * 8 + ph) & 7
            v = 0x40 | ((g * 8 + ph) & 0x1f)
            t0 = t + ph
            s.w(0, 0x28 + c, t0)
            s.w(1, v, t0 + g)
            for a in (0x28 + c, v):
                s.g(a, t0 + g + 40)
            t += 100
        t0 = t + 3
        s.w(0, 0x12, t0)
        s.w(1, 0x21 + g, t0 + g)
        s.g(0x12, t0 + 40)
        s.g(0x21 + g, t0 + 40)
        t += 100
    return s


def pending(base, span, delays):
    """Data to `base`, then a new address d cycles later, over every phase."""
    s = S()
    t = START
    n = 0
    for d in delays:
        for ph in range(32):
            n += 1
            a = base(n)
            v = n & 0x7f
            t0 = t + ph
            s.w(0, a, t0)
            s.w(1, v, t0 + 3)
            s.w(0, 0x20, t0 + 3 + d)
            s.g(a, t0 + 3 + d + 40)
            s.g(0x20, t0 + 3 + d + 40)
            t += span
    return s


def pending_slot():
    return pending(lambda n: 0x60 | (n % 32), 80, (1, 2, 5, 9, 16, 24, 31, 32, 33))


def pending_channel():
    return pending(lambda n: 0x28 | (n % 8), 60, (1, 2, 3, 5, 7, 8, 9))


def data_repeat():
    """Data written again without an address: the last value stays."""
    s = S()
    t = START
    for g in (1, 2, 3, 4, 8, 40):
        for ph in (0, 7, 13, 30):
            t0 = t + ph
            s.w(0, 0x6a, t0)
            s.w(1, 0x11, t0 + 3)
            s.w(1, 0x22 + g, t0 + 3 + g)
            s.g(0x6a, t0 + 3 + g + 1)
            s.g(0x6a, t0 + 3 + g + 40)
            t += 100
    # An address alone does not carry the last data to its register.
    t = s.reg(0x66, 0x33, t)
    s.w(0, 0x67, t)
    s.g(0x67, t + 40)
    s.g(0x66, t + 40)
    return s


def mode_writes():
    s = S()
    t = START
    for a, v in ((0x0f, 0x9b), (0x18, 0xa5), (0x1b, 0x43), (0x10, 0x5c), (0x11, 0x02), (0x12, 0x7e)):
        t = s.reg(a, v, t)
        s.g(a, t)
    for g in (1, 2, 3):
        t0 = t + g
        s.w(0, 0x12, t0)
        s.w(1, 0x11, t0 + 3)
        s.w(0, 0x10, t0 + 3 + g)
        s.w(1, 0x22, t0 + 3 + 2 * g)
        s.g(0x12, t0 + 60)
        s.g(0x10, t0 + 60)
        t += 100
    return s


def timer(kind, n, irq=True, resets=4, phase=0):
    """Load a timer and watch it expire. Each expiry is cleared halfway to the next."""
    s = S()
    t = START + phase
    if kind == "a":
        period = SAMPLE * (1024 - n)
        t = s.reg(0x10, n >> 2, t)
        t = s.reg(0x11, n & 3, t)
        load, reset = (0x05 if irq else 0x01), 0x10
    else:
        period = SAMPLE * 16 * (256 - n)
        t = s.reg(0x12, n, t)
        load, reset = (0x0a if irq else 0x02), 0x20
    s.x(t, t + period * resets + 2 * period + 200)
    s.reg(0x14, load, t)
    for k in range(resets):
        s.reg(0x14, load | reset, t + period * k + period // 2 + 100)
    return s


def timer_irq_enable():
    """Without IRQEN no flag is set; enabling it later flags the next expiry."""
    s = S()
    t = s.reg(0x10, 0xfe, START)
    t = s.reg(0x11, 0x00, t)
    t = s.reg(0x12, 0xff, t)
    s.x(t, t + 3000)
    s.reg(0x14, 0x03, t)
    s.reg(0x14, 0x0f, t + 1200)
    s.reg(0x14, 0x33, t + 2000)
    return s


def timer_reload():
    """A new period, written while a timer runs, starts at the next expiry."""
    s = S()
    t = s.reg(0x10, 0xfd, START)
    t = s.reg(0x11, 0x00, t)
    s.x(t, t + 2400)
    s.reg(0x14, 0x05, t)
    s.reg(0x10, 0xf8, t + 150)
    for k in range(1, 6):
        s.reg(0x14, 0x15, t + 300 * k)
    return s


def timer_stop():
    """Clearing LOAD stops a timer; setting it again starts a full period."""
    s = S()
    t = s.reg(0x10, 0xfc, START)
    t = s.reg(0x11, 0x00, t)
    s.x(t, t + 2600)
    s.reg(0x14, 0x05, t)
    s.reg(0x14, 0x14, t + 70)
    for k, ph in enumerate((0, 9, 21)):
        t1 = t + 300 + 700 * k + ph
        s.reg(0x14, 0x05, t1)
        s.reg(0x14, 0x15, t1 + 400)
        s.reg(0x14, 0x14, t1 + 500)
    return s


def irq_both():
    """IRQ stays asserted while either flag is set."""
    s = S()
    t = s.reg(0x10, 0xff, START)
    t = s.reg(0x11, 0x01, t)
    t = s.reg(0x12, 0xff, t)
    s.x(t, t + 1600)
    s.reg(0x14, 0x0f, t)
    s.reg(0x14, 0x1f, t + 700)
    s.reg(0x14, 0x2f, t + 900)
    s.reg(0x14, 0x3f, t + 1200)
    s.reg(0x14, 0x00, t + 1300)
    return s


class V(S):
    """Register writes in sequence, 80 cycles apart, from START."""

    def __init__(self):
        super().__init__()
        self.t = START

    def set(self, a, v):
        self.reg(a, v, self.t)
        self.t += 80

    def wait(self, n):
        self.t += SAMPLE * n

    def out(self, n, side="R"):
        """Output over the next n samples, which go on under later writes."""
        n0 = -(-self.t // SAMPLE)
        self.o(n0, n, side)
        return n0

    def until(self, n):
        self.t = max(self.t, SAMPLE * n)

    def channel(self, ch, kc, kf=0, con=7, fb=0, pms=0, ams=0, rl=3):
        self.set(0x20 + ch, rl << 6 | fb << 3 | con)
        self.set(0x28 + ch, kc)
        self.set(0x30 + ch, kf << 2)
        self.set(0x38 + ch, pms << 4 | ams)

    def op(self, sl, dt1=0, mul=1, tl=0, am=0, dt2=0):
        """An operator at full attack that holds its level until key-off."""
        for a, v in ((0x40, dt1 << 4 | mul), (0x60, tl), (0x80, 0x1f),
                     (0xa0, am << 7), (0xc0, dt2 << 6), (0xe0, 0x0f)):
            self.set(a + sl, v)

    def key(self, ch, ops):
        """ops: bits for M1, C1, M2, C2. Output starts 8 samples on."""
        self.set(0x08, ops << 3 | ch)
        self.wait(8)

    def rekey(self, ch, ops):
        """Key-off and on again, which restarts the phase."""
        self.set(0x08, ch)
        self.wait(16)
        self.key(ch, ops)

    def lfo(self, rate, wave, amd=0, pmd=0):
        self.set(0x1b, wave)
        self.set(0x19, amd)
        self.set(0x19, 0x80 | pmd)
        self.set(0x18, rate)


def phase_static():
    """Phase steps without PM, at every key code."""
    s = S()
    for kc in range(128):
        s._op(kc, "f %02x" % kc)
    return s


def phase_pm():
    """Phase steps under every PM value: gap notes, the top and bottom of the range.
    Then the same latches under a PM depth of 0."""
    s = S()
    n = 0
    for pms in range(8):
        for kc in (0x00, 0x03, 0x1e, 0x3c, 0x3f, 0x4a, 0x7b, 0x7e, 0x7f):
            s._op(n, "m %d %02x" % (pms, kc))
            n += 1
    for pms in (1, 5, 6, 7):
        for kc in (0x03, 0x4a, 0x7f):
            s._op(n, "z %d %02x" % (pms, kc))
            n += 1
    return s


def lfo_voice(wave, rate, amd, pmd, pms, ams, kc=0x4a, kf=0x21, dt1=3, dt2=1):
    s = V()
    s.lfo(rate, wave, amd, pmd)
    s.channel(0, kc, kf, pms=pms, ams=ams)
    s.op(0, dt1=dt1, dt2=dt2, am=1)
    s.key(0, 1)
    s.out(4096)
    return s


def lfo_rates():
    """Rate changes over the low nibble's extra steps, on a saw PM."""
    s = V()
    s.lfo(0xf0, 0, 0, 0x7f)
    s.channel(0, 0x4a, 0x21, pms=7)
    s.op(0)
    s.key(0, 1)
    n0 = s.out(512 * 8)
    for k, rate in enumerate((0xf1, 0xf5, 0xfa, 0xff, 0xe7, 0xd9, 0xfe, 0x83)):
        s.until(n0 + 512 * k + 100)
        s.set(0x18, rate)
    return s


def lfo_reload():
    """$18 rewritten at every cycle around the LFO's tick, at a period of 4 windows."""
    s = V()
    s.lfo(0xd0, 0, 0, 0x7f)
    s.channel(0, 0x4a, 0x21, pms=7)
    s.op(0)
    s.key(0, 1)
    n0 = s.out(64 * 48)
    t = SAMPLE * (n0 + 16)
    for o in (0, 5, 31, 224, 240, 250, 251, 252, 253, 254, 255) * 4:
        t += 2000
        t = t - t % 256 + o
        s.w(0, 0x18, t - 6)
        s.w(1, 0xd0, t)
    return s


def lfo_pm_split():
    """PM on the last slot to take it early (6's C1) and the first to take it late (7's C1)."""
    s = V()
    s.lfo(0xfc, 0, 0, 0x7f)
    s.channel(6, 0x4a, 0x10, pms=7)
    s.channel(7, 0x5d, 0x2b, pms=6)
    s.op(22)
    s.op(23)
    s.key(6, 2)
    s.key(7, 2)
    s.out(4096)
    return s


def lfo_am_split():
    """AM on the last slot to take it late (7's M2) and the first to take it early (0's C1)."""
    s = V()
    s.lfo(0xfc, 2, 0x7f, 0)
    s.channel(7, 0x4a, ams=3)
    s.channel(0, 0x5d, ams=3)
    s.op(15, am=1)
    s.op(16, am=1)
    s.key(7, 4)
    s.key(0, 2)
    s.out(4096)
    return s


def pm_gap_note():
    """A gap note under negative PM and DT2, and under PM of depth 0."""
    s = V()
    s.lfo(0xe8, 0, 0, 0x7f)
    s.channel(0, 0x4b, 0x3f, pms=7)
    s.op(0, dt1=6, dt2=2)
    s.key(0, 1)
    s.out(2048)
    s.wait(2048 + 4)
    s.set(0x19, 0x80)
    s.rekey(0, 1)
    s.out(1024)
    return s


def detune_voices():
    """DT1, DT2, KF and MUL across the key codes, with no LFO."""
    s = V()
    s.channel(0, 0x00)
    s.op(0)
    for kc, kf, dt1, dt2, mul in (
            (0x00, 0x00, 7, 0, 1), (0x0e, 0x3f, 3, 3, 15), (0x23, 0x15, 5, 2, 0), (0x37, 0x2a, 1, 1, 7),
            (0x4a, 0x21, 6, 3, 3), (0x5b, 0x00, 2, 2, 12), (0x6e, 0x31, 4, 1, 1), (0x7b, 0x3f, 7, 3, 1),
            (0x7e, 0x3f, 3, 3, 15), (0x7f, 0x3f, 2, 2, 9), (0x13, 0x08, 0, 3, 2), (0x47, 0x20, 1, 0, 1)):
        s.set(0x28, kc)
        s.set(0x30, kf << 2)
        s.set(0x40, dt1 << 4 | mul)
        s.set(0xc0, dt2 << 6)
        s.rekey(0, 1)
        s.out(256)
        s.wait(256 + 4)
        s.set(0x08, 0)
    return s


def slot_framing():
    """Every slot of channels 6 and 7 at its own MUL, on both sides: 14 and 15 either
    side of the left output's split, 31 alone late on the right. Then channel 3 as two
    pairs, with feedback."""
    s = V()
    for ch in (6, 7):
        s.channel(ch, 0x3a + ch, 0x11)
        for k, sl in enumerate((ch, ch + 8, ch + 16, ch + 24)):
            s.op(sl, mul=1 + k + 4 * (ch - 6), tl=6)
    s.key(6, 15)
    s.key(7, 15)
    s.out(512, "L")
    s.out(512, "R")
    s.wait(512 + 4)
    s.set(0x08, 6)
    s.set(0x08, 7)
    s.wait(256)
    s.channel(3, 0x4a, 0x21, con=4, fb=5)
    for k, sl in enumerate((3, 11, 19, 27)):
        s.op(sl, mul=k + 1, tl=18 if k < 3 else 0)
    s.key(3, 15)
    s.out(512, "L")
    s.out(512, "R")
    return s


def noise_voice(s, tl=0, ams=0):
    """Channel 7's C2, slot 31, the noise slot."""
    s.channel(7, 0x4a, 0x00, ams=ams)
    s.op(31, tl=tl, am=1 if ams else 0)


def settle(s, n, sides="R"):
    """Output over n samples from 8 samples on, past the latency of the write before."""
    s.wait(8)
    for side in sides:
        s.out(n, side)
    s.wait(n + 4)


def noise_rates():
    """The noise generator at a range of frequencies, each written as it runs."""
    s = V()
    s.set(0x0f, 0x80)
    noise_voice(s)
    s.key(7, 8)
    settle(s, 448, "LR")
    for nf in (0x1f, 0x1e, 0x10, 0x0a, 0x01, 0x00):
        s.set(0x0f, 0x80 | nf)
        settle(s, 448, "LR")
    return s


def noise_level():
    """Noise with nothing keyed, then its level over TL."""
    s = V()
    s.set(0x0f, 0x9c)
    noise_voice(s)
    settle(s, 128)
    s.key(7, 8)
    settle(s, 64)
    for tl in (0x01, 0x02, 0x07, 0x10, 0x20, 0x33, 0x40, 0x55, 0x60, 0x6a, 0x70, 0x78, 0x7c, 0x7e, 0x7f, 0x3f, 0x00):
        s.set(0x60 + 31, tl)
        settle(s, 64)
    return s


def noise_writes():
    """$0F rewritten at every cycle of the sample, between two frequencies."""
    s = V()
    s.set(0x0f, 0x9e)
    noise_voice(s)
    s.key(7, 8)
    t = s.t
    for o in range(32):
        t += 80 * SAMPLE
        t = t - t % SAMPLE + o
        s.w(0, 0x0f, t - 6)
        s.w(1, 0x9e if o & 1 else 0x93, t)
        s.o(t // SAMPLE + 8, 64, "R")
    return s


def noise_am():
    """A saw AM on the noise slot, at TL 0 and 7F."""
    s = V()
    s.lfo(0xe3, 0, 0x7f, 0)
    s.set(0x0f, 0x9a)
    noise_voice(s, ams=3)
    s.key(7, 8)
    settle(s, 1024)
    s.set(0x60 + 31, 0x7f)
    settle(s, 1024)
    return s


def lfo_noise_am(rate):
    """The noise waveform's AM on a sine, at a rate with no second steps. Its PM, and
    rates with them, are not covered (tests/accuracy/README.md)."""
    s = V()
    s.set(0x0f, 0x1b)
    s.lfo(rate, 3, 0x7f, 0)
    s.channel(0, 0x4a, 0x21, ams=3)
    s.op(0, am=1)
    s.key(0, 1)
    s.out(4096)
    return s


def algorithms():
    """Every algorithm on channel 2 without feedback and channel 5 with it, both
    sides, each slot at its own MUL and level. Feedback stays low enough that the
    first samples after key-on (tests/accuracy/README.md) die away."""
    s = V()
    for con in range(8):
        for ch, fb in ((2, 0), (5, 1 + con % 3)):
            s.channel(ch, 0x3c + 3 * ch, 0x15, con=con, fb=fb)
            for k, sl in enumerate((ch, ch + 8, ch + 16, ch + 24)):
                s.op(sl, mul=(1, 2, 3, 5)[(k + ch) % 4], tl=(0, 10, 20, 4)[k])
        s.key(2, 15)
        s.key(5, 15)
        s.out(256, "L")
        s.out(256, "R")
        s.wait(256 + 4)
        s.set(0x08, 2)
        s.set(0x08, 5)
        s.wait(256)
    return s


def output_clamp():
    """Every channel at full level with four carriers: the mix past both ends of
    the DAC's range."""
    s = V()
    for ch in range(8):
        s.channel(ch, 0x30 + 5 * ch, 7 * ch)
        for k in range(4):
            s.op(ch + 8 * k, mul=1 + (k + ch) % 3)
    for ch in range(8):
        s.key(ch, 15)
    s.out(512, "L")
    s.out(512, "R")
    return s


SCRIPTS = {
    "busy_phase": busy_phase,
    "address_no_busy": address_no_busy,
    "spacing": spacing,
    "pending_slot": pending_slot,
    "pending_channel": pending_channel,
    "data_repeat": data_repeat,
    "mode_writes": mode_writes,
    "timer_a_1023": lambda: timer("a", 1023, resets=6),
    "timer_a_1022": lambda: timer("a", 1022, phase=5),
    "timer_a_1000": lambda: timer("a", 1000, phase=17),
    "timer_a_768": lambda: timer("a", 768, resets=2, phase=29),
    "timer_b_255": lambda: timer("b", 255),
    "timer_b_254": lambda: timer("b", 254, phase=11),
    "timer_b_240": lambda: timer("b", 240, resets=2, phase=23),
    "timer_irq_enable": timer_irq_enable,
    "timer_reload": timer_reload,
    "timer_stop": timer_stop,
    "irq_both": irq_both,
    "phase_static": phase_static,
    "phase_pm": phase_pm,
    "lfo_saw_pm": lambda: lfo_voice(0, 0xc4, 0, 0x7f, 7, 0),
    "lfo_square_pm_am": lambda: lfo_voice(1, 0xf7, 0x40, 0x55, 7, 1),
    "lfo_triangle_am": lambda: lfo_voice(2, 0xd3, 0x7f, 0, 0, 3),
    "lfo_triangle_pm": lambda: lfo_voice(2, 0xe1, 0, 0x6c, 6, 0, kc=0x3e, kf=0x3f, dt2=3),
    "lfo_rates": lfo_rates,
    "lfo_reload": lfo_reload,
    "lfo_pm_split": lfo_pm_split,
    "lfo_am_split": lfo_am_split,
    "pm_gap_note": pm_gap_note,
    "detune_voices": detune_voices,
    "slot_framing": slot_framing,
    "noise_rates": noise_rates,
    "noise_level": noise_level,
    "noise_writes": noise_writes,
    "noise_am": noise_am,
    "lfo_noise_am_d0": lambda: lfo_noise_am(0xd0),
    "lfo_noise_am_f0": lambda: lfo_noise_am(0xf0),
    "algorithms": algorithms,
    "output_clamp": output_clamp,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tb")
    ap.add_argument("out")
    ap.add_argument("--commit", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name, make in SCRIPTS.items():
        out = subprocess.run([a.tb], input=make().text(), capture_output=True, text=True, check=True).stdout
        with open(os.path.join(a.out, "ym_%s.vec" % name), "w", newline="\n") as f:
            f.write("oracle nuked-opm %s\n" % a.commit)
            f.write(out)
    with open(os.path.join(a.out, "MANIFEST"), "w", newline="\n") as f:
        f.write("".join("ym_%s.vec\n" % n for n in SCRIPTS))


if __name__ == "__main__":
    main()
