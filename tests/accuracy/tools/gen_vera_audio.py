#!/usr/bin/env python3
"""Generate VERA audio vectors from a gate-level netlist of a released bitstream.

    gen_vera_audio.py VVP OUT_DIR --version 49.0.6 --sha256 HEX [--jobs N]

VVP is tb_audio.v compiled with the netlist.
"""

import argparse
import concurrent.futures as cf
import os
import subprocess
import tempfile

W, R, AT = 0, 1, 3
# Render r reads voice 0 at clock 512 * (r + 1) + 1 and voice c 6c clocks later
# (psg.v:203-233); PCM fetches in the same window (pcm.v:117-131).
TICK, FIRST, SCAN = 512, 513, 104
# A write lands 3 clocks after its strobe rises; tb_audio.v strobes 5 clocks after an op starts.
WRITE_DELAY, STROBE, OP = 3, 5, 13
# The DAC latches the mix at each render, before that render's scan, and a frame starting
# on LRCK's falling edge at E carries the latest one: r = (E - LATCH) // 512 - 1.
LATCH, MARGIN = 3, 8
START = 3000
# ISR: only AFLOW (top.v:179) belongs to audio.
READ_MASK = {0x07: 0x08}


def safe(e):
    """No access within MARGIN clocks before a render, or during its scan."""
    off = (e - FIRST) % TICK
    return SCAN < off < TICK - MARGIN


class A:
    def __init__(self):
        self.ops = []

    def w(self, r, v):
        self.ops.append((W, r, v & 255))

    def r(self, r):
        self.ops.append((R, r, 0))

    def wait(self, renders):
        self.ops.append((AT, 0, renders))

    def vram(self, addr, data, inc=1):
        self.w(0, addr & 255); self.w(1, (addr >> 8) & 255); self.w(2, (addr >> 16) | (inc << 4))
        for b in data:
            self.w(3, b)

    def voice(self, ch, freq=0, ctl=0, wav=0):
        self.vram(0x1F9C0 + 4 * ch, [freq & 255, freq >> 8, ctl, wav])

    def vreg(self, ch, i, v):
        self.vram(0x1F9C0 + 4 * ch + i, [v])

    def fifo(self, data):
        for b in data:
            self.w(0x1D, b)


def s16(*xs):
    out = []
    for x in xs:
        out += [x & 255, (x >> 8) & 255]
    return out


def schedule(a, tail):
    """tb script with every access placed outside a scan; returns (script, end clock)."""
    lines, t = [], START
    for k, r, v in a.ops:
        if k == AT:
            t = FIRST + ((t - FIRST) // TICK + v) * TICK + SCAN + 1
            continue
        while not safe(t + STROBE + (WRITE_DELAY if k == W else 0)):
            t += 1
        lines.append(f"3 0 {t:x}\n{k} {r:x} {v:x}\n")
        t += OP
    end = FIRST + ((t - FIRST) // TICK + tail) * TICK
    lines.append(f"3 0 {end:x}\n")
    return "".join(lines), end


def psg_saw():
    a = A(); a.voice(0, 0x1000, 0xFF, 0x40); a.wait(80); return a


def psg_saw_width():
    a = A(); a.voice(0, 0x0C35, 0xFF, 0x40 | 0x15); a.wait(80); return a


def psg_triangle():
    a = A(); a.voice(2, 0x0A00, 0xFF, 0x80); a.wait(60); a.vreg(2, 3, 0x80 | 0x2A); a.wait(60); return a


def psg_pulse_widths():
    a = A(); a.voice(1, 0x0800, 0xFF, 0x00)
    for pw in (0x00, 0x01, 0x10, 0x1F, 0x20, 0x3E, 0x3F):
        a.vreg(1, 3, pw); a.wait(40)
    return a


def psg_volumes():
    a = A(); a.voice(3, 0, 0xC0, 0x3F)
    for vol in range(64):
        a.vreg(3, 2, 0xC0 | vol); a.wait(2)
    return a


def psg_pan():
    a = A(); a.voice(4, 0x0400, 0x7F, 0x40); a.voice(5, 0x0600, 0xBF, 0x80); a.wait(30)
    a.vreg(4, 2, 0xFF); a.wait(30); a.vreg(5, 2, 0x3F); a.wait(30); a.vreg(5, 2, 0xBF); a.wait(30)
    return a


def psg_phase_reset():
    a = A(); a.voice(6, 0x0900, 0xFF, 0x40); a.wait(25); a.vreg(6, 2, 0x3F); a.wait(5)
    a.vreg(6, 2, 0xFF); a.wait(25); return a


def psg_freq_change():
    a = A(); a.voice(7, 0x0100, 0xFF, 0x40); a.wait(20)
    for f in (0x3456, 0xFFFF, 0x8000, 0x0001):
        a.vram(0x1F9C0 + 28, [f & 255, f >> 8]); a.wait(30)
    return a


def psg_all_voices():
    a = A()
    for ch in range(16):
        a.voice(ch, 0x0123 * (ch + 1) & 0xFFFF, (0x40, 0x80, 0xC0)[ch % 3] | (0x3F - 2 * ch), (ch % 3) << 6 | (ch * 5 & 0x3F))
    a.wait(150); return a


def psg_noise():
    a = A(); a.voice(0, 0xFFFF, 0x7F, 0xC0); a.voice(15, 0x4000, 0xBF, 0xC0); a.voice(9, 0x1234, 0xFF, 0xC0)
    a.wait(300); return a


def psg_noise_late():
    a = A(); a.wait(1500); a.voice(5, 0xC000, 0xFF, 0xC0); a.wait(200); return a


def pcm_mono8():
    a = A(); a.w(0x1B, 0x0F); a.fifo([0x7F, 0x80, 0x00, 0xFF, 0x01, 0x40, 0xC0, 0x55, 0xAA]); a.w(0x1C, 0x80)
    a.wait(14); return a


def pcm_stereo8():
    a = A(); a.w(0x1B, 0x1F); a.fifo([0x7F, 0x80, 0x01, 0xFF, 0x40, 0xC0, 0x12, 0x34, 0x56]); a.w(0x1C, 0x80)
    a.wait(10); return a


def pcm_mono16():
    a = A(); a.w(0x1B, 0x2F); a.fifo(s16(0x7FFF, 0x8000, 0xFFFF, 0x0001, 0x1234, 0xFEDC, 0x0040)); a.w(0x1C, 0x80)
    a.wait(12); return a


def pcm_stereo16():
    a = A(); a.w(0x1B, 0x3F); a.fifo(s16(0x1111, 0xEEEE, 0x7FFF, 0x8000, 0x0102, 0xFDFE, 0x3333)); a.w(0x1C, 0x80)
    a.wait(10); return a


def pcm_volumes():
    a = A(); a.w(0x1B, 0x20); a.fifo(s16(*([0xFF81] * 20 + [0x7F7F] * 20))); a.w(0x1C, 0x80)
    for vol in range(16):
        a.w(0x1B, 0x20 | vol); a.wait(1)
    for vol in range(16):
        a.w(0x1B, 0x20 | vol); a.wait(1)
    a.wait(10); return a


def pcm_rates():
    a = A(); a.w(0x1B, 0x0F)
    for rate in (0x01, 0x40, 0x55, 0x7F, 0x81, 0xC0, 0xFF):
        a.fifo(list(range(0x10, 0x28))); a.w(0x1C, rate); a.wait(40); a.w(0x1B, 0x8F)
    return a


def pcm_partial():
    a = A(); a.w(0x1B, 0x3F); a.fifo(s16(0x1111, 0x2222, 0x3333) + [0x44]); a.w(0x1C, 0x80); a.wait(6)
    a.w(0x1B, 0x2F); a.fifo([0x10, 0x20, 0x30]); a.wait(6)
    a.w(0x1B, 0x1F); a.fifo([0x10, 0x20, 0x30]); a.wait(6)
    return a


def pcm_hold_reset():
    a = A(); a.w(0x1B, 0x0F); a.fifo([0x10, 0x20, 0x30, 0x40, 0x50, 0x60]); a.w(0x1C, 0x80); a.wait(2)
    a.w(0x1C, 0x00); a.wait(4); a.w(0x1B, 0x0A); a.wait(4); a.w(0x1B, 0x8F); a.wait(4)
    return a


def pcm_mode_change():
    a = A(); a.w(0x1B, 0x0F); a.fifo(list(range(0x21, 0x41))); a.w(0x1C, 0x80); a.wait(3)
    for mode in (0x1F, 0x2F, 0x3F, 0x0F):
        a.w(0x1B, mode); a.wait(3)
    a.wait(8); return a


def pcm_flags():
    a = A(); a.w(0x1B, 0x0F); a.r(0x1B); a.r(0x07)
    a.fifo([0x40] * 1023); a.r(0x07); a.fifo([0x41]); a.r(0x07); a.fifo([0x42] * 4); a.r(0x07); a.r(0x1B)
    a.w(0x1C, 0x80)
    for _ in range(8):
        a.wait(1); a.r(0x07); a.r(0x1B)
    a.w(0x1C, 0x00); a.w(0x1B, 0x8F); a.r(0x1B); a.r(0x07)
    return a


def pcm_full():
    a = A(); a.w(0x1B, 0x0F); a.fifo([0x11] * 4094); a.r(0x1B); a.fifo([0x22]); a.r(0x1B); a.fifo([0x33]); a.r(0x1B)
    a.w(0x1B, 0x8F); a.r(0x1B); a.fifo([0x44, 0x55]); a.w(0x1C, 0x80); a.wait(4)
    return a


def mix():
    # Under 16 FIFO bytes: see README, not covered.
    a = A(); a.voice(0, 0x0800, 0xFF, 0x40); a.voice(1, 0x0300, 0xFF, 0x80); a.w(0x1B, 0x3C)
    a.fifo(s16(0x8000, 0x8123, 0x7FFF, 0x0456, 0xC000, 0x3FFF, 0x0001)); a.w(0x1C, 0x80); a.wait(8)
    a.w(0x1B, 0x07); a.fifo([0x90, 0x70, 0xFF, 0x01, 0x80, 0x7F, 0x00, 0x40]); a.wait(12)
    return a


SCRIPTS = {
    "psg_saw": psg_saw,
    "psg_saw_width": psg_saw_width,
    "psg_triangle": psg_triangle,
    "psg_pulse_widths": psg_pulse_widths,
    "psg_volumes": psg_volumes,
    "psg_pan": psg_pan,
    "psg_phase_reset": psg_phase_reset,
    "psg_freq_change": psg_freq_change,
    "psg_all_voices": psg_all_voices,
    "psg_noise": psg_noise,
    "psg_noise_late": psg_noise_late,
    "pcm_mono8": pcm_mono8,
    "pcm_stereo8": pcm_stereo8,
    "pcm_mono16": pcm_mono16,
    "pcm_stereo16": pcm_stereo16,
    "pcm_volumes": pcm_volumes,
    "pcm_rates": pcm_rates,
    "pcm_partial": pcm_partial,
    "pcm_hold_reset": pcm_hold_reset,
    "pcm_mode_change": pcm_mode_change,
    "pcm_flags": pcm_flags,
    "pcm_full": pcm_full,
    "mix": mix,
}


def capture(name, script, vvp, cache):
    if cache:
        os.makedirs(cache, exist_ok=True)
        cin, cout = os.path.join(cache, f"{name}.in"), os.path.join(cache, f"{name}.out")
        if os.path.exists(cout) and os.path.exists(cin) and open(cin).read() == script:
            return open(cout).read()
    with tempfile.TemporaryDirectory() as tmp:
        inp, out = os.path.join(tmp, "in"), os.path.join(tmp, "out")
        with open(inp, "w", newline="\n") as f:
            f.write(script)
        subprocess.run(["vvp", vvp, f"+in={inp}", f"+out={out}"], check=True,
                       stdout=subprocess.DEVNULL, timeout=14400)
        text = open(out).read()
    if cache:
        with open(cout, "w", newline="\n") as f:
            f.write(text)
        with open(cin, "w", newline="\n") as f:
            f.write(script)
    return text


def sample(word):
    """The 17-bit mix in a 32-bit half-frame: one delay bit, then {mix, 7'b0} (audio.v:94-104)."""
    x = (word >> 7) & 0xFFFFFF
    return (x - (1 << 24) if x & 0x800000 else x) >> 7


def run(name, vvp, out_dir, header, cache=None):
    a = SCRIPTS[name]()
    script, end = schedule(a, 4)
    text = capture(name, script, vvp, cache)
    acc, frames, start, left = [], [], None, None
    for ln in text.splitlines():
        f = ln.split()
        if not f:
            continue
        if f[0] in "wr":
            if "x" in ln or "z" in ln:
                raise RuntimeError(f"{name}: bad capture")
            acc.append(f)
        elif f[0] == "F":
            t, level, word = int(f[1]), int(f[2]), int(f[3], 16)
            if "x" in f[3] or "z" in f[3]:
                continue
            if level == 1:
                left, start_l = sample(word), start
            elif left is not None and start_l is not None:
                frames.append((start_l, left, sample(word)))
                left = None
            if level == 0:
                start = t
    want = sum(1 for k, _, _ in a.ops if k in (W, R))
    if len(acc) != want:
        raise RuntimeError(f"{name}: {len(acc)} accesses captured, {want} expected")
    it = iter(acc)
    seen = {}
    for e, l, r in frames:
        off = (e - LATCH) % TICK
        if off < MARGIN or off >= TICK - MARGIN:
            continue
        rr = (e - LATCH) // TICK - 1
        if rr >= 0 and rr not in seen:
            seen[rr] = (l, r)
    with open(os.path.join(out_dir, f"audio_{name}.vec"), "w", newline="\n") as f:
        f.write(header)
        f.write(f"audio {name}\n")
        events = []
        for k, r, v in a.ops:
            if k == AT:
                continue
            ln = next(it)
            t = int(ln[-1])
            if k == W:
                if not safe(t + WRITE_DELAY):
                    raise RuntimeError(f"{name}: write at {t} lands in a scan")
                events.append((t, f"W {r:02x} {v:02x} {t}\n"))
            else:
                if not safe(t):
                    raise RuntimeError(f"{name}: read at {t} lands in a scan")
                events.append((t, f"R {r:02x} {ln[1]} {READ_MASK.get(r, 0xFF):02x} {t}\n"))
        for rr, (l, r) in seen.items():
            events.append((FIRST + rr * TICK, f"S {rr} {l} {r}\n"))
        for _, s in sorted(events, key=lambda x: x[0]):
            f.write(s)
        f.write("e\n")
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vvp")
    ap.add_argument("out_dir")
    ap.add_argument("--version", required=True)
    ap.add_argument("--sha256", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--cache", help="keep raw captures here and reuse them")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    header = f"oracle vera {a.version} bitstream-sha256 {a.sha256}\n"
    names = [n for n in SCRIPTS if not a.only or n in a.only]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name in ex.map(lambda n: run(n, a.vvp, a.out_dir, header, a.cache), names):
            print("done", name, flush=True)


if __name__ == "__main__":
    main()
