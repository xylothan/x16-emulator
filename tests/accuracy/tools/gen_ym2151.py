#!/usr/bin/env python3
"""Generate YM2151 bus vectors from Nuked-OPM.

    gen_ym2151.py TB_OPM OUT_DIR --commit SHA

TB_OPM is tb_opm.c built against Nuked-OPM at commit SHA.
"""

import argparse
import os
import subprocess

SAMPLE = 32          # OPM_Clock calls per sample
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
