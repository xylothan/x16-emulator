#!/usr/bin/env python3
"""Generate VERA bus vectors from a gate-level netlist of a released bitstream.

    gen_vera_bus.py VVP OUT_DIR --version 49.0.6 --sha256 HEX [--jobs N]

VVP is tb_bus.v compiled with the netlist.
"""

import argparse
import concurrent.futures as cf
import os
import subprocess
import tempfile

W, R, F, AT, PIN = 0, 1, 2, 3, 4
IEN_MASK = 0xBF
# tb_bus.v o[4], package pin 32: falls when IEN enables a pending VSYNC, rises on its ack.
IRQ_PIN = 4
# VSYNC status shows at 480:0 (top.v:454), line status at L:1, SCANLINE steps at 0 (composer.v:79-85).
LINE, LINES, ANCHOR_Y, ANCHOR_X = 800, 525, 492, 4


def at_pos(y, x):
    """T of VGA line y, dot x, counted from the VSYNC rising edge."""
    return ((y - ANCHOR_Y) % LINES) * LINE + x - ANCHOR_X


class S:
    def __init__(self):
        self.ops = []
        self.timed = False

    def w(self, r, v):
        self.ops.append((W, r, v & 255, 0xFF))

    def r(self, r, mask=0xFF):
        self.ops.append((R, r, 0, mask))

    def f(self):
        self.ops.append((F, 0, 0, 0xFF))

    def v(self):
        self.timed = True
        self.f()

    def at(self, t):
        self.ops.append((AT, 0, t, 0xFF))

    def pin(self):
        self.ops.append((PIN, 0, 0, 0xFF))


def dcsel_sweep():
    s = S()
    s.w(5, 0); s.w(9, 0x21); s.w(10, 0x55); s.w(11, 0x66); s.w(12, 0x77)
    s.w(5, 2); s.w(9, 0x11); s.w(10, 0x88); s.w(11, 0x22); s.w(12, 0xE0)
    for d in range(64):
        s.w(5, d << 1)
        for r in range(9, 13):
            s.r(r)
    return s


def ctrl_roundtrip():
    s = S()
    for v in (0x00, 0x01, 0x02, 0x03, 0x7E, 0x7F, 0x55 & 0x7F, 0x2A):
        s.w(5, v); s.r(5)
    s.w(5, 0)
    return s


def layer_roundtrip():
    s = S()
    s.w(5, 0)
    for r in range(0x0D, 0x1B):
        for v in (0x00, 0xFF, 0x5A, 0xA5, 0x0F, 0xF0):
            s.w(r, v); s.r(r)
    return s


def ien_irqline():
    s = S()
    s.w(5, 0)
    for v in (0x00, 0x0F, 0x05, 0x0A, 0x80, 0x8F, 0xFF):
        s.w(6, v); s.r(6, IEN_MASK)
    s.w(6, 0)
    return s


def data_port_increments():
    s = S()
    s.w(5, 0)
    for port in (0, 1):
        s.w(5, port)
        for inc in range(16):
            for dec in (0, 1):
                hi = (inc << 4) | (dec << 3)
                s.w(0, 0x00); s.w(1, 0x80); s.w(2, hi)
                for k in range(4):
                    s.w(3 + port, 0x10 + k)
                    s.r(0); s.r(1); s.r(2)
                s.w(0, 0x00); s.w(1, 0x80); s.w(2, hi)
                for _ in range(4):
                    s.r(3 + port)
                    s.r(0); s.r(1); s.r(2)
    s.w(5, 0)
    return s


def data_port_wrap():
    s = S()
    s.w(5, 0)
    for inc in (1, 2, 8, 15):
        for dec in (0, 1):
            for lo, mid, top in ((0xFF, 0xFF, 1), (0x00, 0x00, 0), (0xFE, 0xFF, 0)):
                s.w(0, lo); s.w(1, mid); s.w(2, (inc << 4) | (dec << 3) | top)
                for _ in range(3):
                    s.w(3, 0x77); s.r(0); s.r(1); s.r(2)
    return s


def two_ports_share_vram():
    s = S()
    s.w(5, 0); s.w(0, 0x40); s.w(1, 0x12); s.w(2, 0x10)
    for k in range(8):
        s.w(3, 0xC0 + k)
    s.w(5, 1); s.w(0, 0x40); s.w(1, 0x12); s.w(2, 0x10)
    for _ in range(8):
        s.r(4)
    s.w(5, 0); s.w(0, 0x40); s.w(1, 0x12); s.w(2, 0x00)
    s.r(3); s.r(3); s.w(3, 0x3C); s.r(3)
    return s


def isr_sprite_collision():
    s = S()
    s.w(5, 0); s.w(0, 0); s.w(1, 0x00); s.w(2, 0x10)
    for _ in range(64):
        s.w(3, 0xFF)
    s.w(0, 0); s.w(1, 0xFC); s.w(2, 0x11)
    for mask in (0x7, 0xD):
        for v in (0x00, 0x80, 100, 0, 100, 0, (mask << 4) | (3 << 2), 0):
            s.w(3, v)
    s.w(9, 0x41)
    s.f(); s.f()
    s.r(7, 0xF7)
    s.w(7, 0xFF); s.r(7, 0xF7)
    s.w(7, 0x04); s.r(7, 0xF7)
    s.w(0, 0x06); s.w(1, 0xFC); s.w(2, 0x01); s.w(3, 0)
    s.w(0, 0x0E); s.w(1, 0xFC); s.w(2, 0x01); s.w(3, 0)
    s.f(); s.f()
    s.r(7, 0xF7)
    return s


def isr_sprites_disabled():
    s = S()
    s.w(5, 0); s.w(0, 0); s.w(1, 0x00); s.w(2, 0x10)
    for _ in range(64):
        s.w(3, 0xFF)
    s.w(0, 0); s.w(1, 0xFC); s.w(2, 0x11)
    for mask in (0x7, 0xD):
        for v in (0x00, 0x80, 100, 0, 100, 0, (mask << 4) | (3 << 2), 0):
            s.w(3, v)
    s.w(9, 0x01)
    s.f(); s.f()
    s.r(7, 0xF7)
    return s


def isr_latches():
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(8, 0x10); s.w(6, 0x00)
    s.f(); s.f()
    s.r(7, 0x07)
    s.w(7, 0x01); s.r(7, 0x07)
    s.w(7, 0x02); s.r(7, 0x07)
    s.w(7, 0xF8); s.r(7, 0x07)
    return s


READ_LEAD = 5


def read_at(s, y, x, r, mask=0xFF):
    s.at(at_pos(y, x) - READ_LEAD); s.r(r, mask)


def pins_around(s, y, before, after):
    s.at(at_pos(y, -before) - 1)
    for _ in range(before + after):
        s.pin()


def timed_scanline():
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0)
    s.v(); s.w(7, 0x07)
    for b in (512, 524, 0):
        for i, x in enumerate(range(-26, 40, 13)):
            read_at(s, b, x, 8 if i % 2 == 0 else 6)
    for j in range(40):
        read_at(s, 1 + j, -20 + j, 8)
    for b in (256, 480, 491):
        for i, x in enumerate(range(-26, 40, 13)):
            read_at(s, b, x, 8 if i % 2 == 0 else 6)
    return s


def set_irqline(s, line, ien):
    s.w(8, line); s.w(6, ((line >> 1) & 0x80) | ien)


def timed_line_irq():
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0)
    s.v()
    for line in (500, 510, 511, 0, 1, 100, 255, 256, 300, 479, 480, 490):
        s.at(at_pos(line - 1, 300))
        set_irqline(s, line, 0x02); s.w(7, 0x07)
        pins_around(s, line, 4, 10)
        read_at(s, line, 60, 7, 0x07); read_at(s, line, 73, 8)
        s.w(7, 0x02)
        for _ in range(8):
            s.pin()
    return s


def timed_irqline_race():
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0x02); s.w(8, 0)
    s.v(); s.w(7, 0x07)
    for i, line in enumerate(range(102, 122)):
        s.at(at_pos(line - 1, 785 + i) - 5)
        s.w(8, line)
        read_at(s, line, 40, 7, 0x07)
        s.w(7, 0x02)
    for i, line in enumerate(range(130, 140)):
        s.at(at_pos(line, 100 + 40 * i))
        s.w(8, line)
        read_at(s, line, 700, 7, 0x07)
        s.w(7, 0x02)
    return s


def timed_scanline_blank():
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0)
    s.v()
    for y in list(range(492, 525)) + list(range(0, 3)) + list(range(484, 492)):
        read_at(s, y, 400, 8); read_at(s, y, 413, 6)
        read_at(s, y, 794, 8); read_at(s, y, 807, 8)
    return s


def line_irq_window(lines):
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0)
    s.v()
    for line in lines:
        s.at(at_pos(line - 1, 100))
        set_irqline(s, line, 0x02); s.w(7, 0x07)
        for x in range(200, 2 * LINE + 51, 50):
            s.at(at_pos(line - 1, x) - 1); s.pin()
    return s


def timed_vsync_irq():
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0)
    s.v(); s.w(7, 0x07)
    s.at(at_pos(479, 600)); s.w(6, 0x01)
    pins_around(s, 480, 4, 10)
    read_at(s, 480, 60, 7, 0x07)
    s.w(7, 0x01)
    for _ in range(8):
        s.pin()
    s.at(at_pos(483, 0)); s.w(6, 0); set_irqline(s, 486, 0); s.w(7, 0x07)
    s.at(at_pos(487, 100)); s.w(6, 0x02)
    for _ in range(8):
        s.pin()
    s.w(6, 0)
    for _ in range(8):
        s.pin()
    s.w(6, 0x02); s.w(7, 0x02)
    for _ in range(8):
        s.pin()
    read_at(s, 488, 0, 7, 0x07)
    return s


def timed_irq_vstart():
    s = S()
    s.w(5, 2); s.w(9, 0x00); s.w(10, 0xA0); s.w(11, 0x0A); s.w(12, 0x64)
    s.w(5, 0); s.w(9, 0x01); s.w(10, 0x40); s.w(11, 0x40); s.w(6, 0)
    s.v()
    for line in (0, 19, 20, 21, 100, 199, 200, 201, 479):
        s.at(at_pos(line - 1, 300))
        set_irqline(s, line, 0x02); s.w(7, 0x07)
        pins_around(s, line, 2, 6)
        read_at(s, line, 60, 7, 0x07); read_at(s, line, 73, 8)
        s.w(7, 0x02)
    return s


SCRIPTS = {
    "dcsel_sweep": dcsel_sweep,
    "ctrl_roundtrip": ctrl_roundtrip,
    "layer_roundtrip": layer_roundtrip,
    "ien_irqline": ien_irqline,
    "data_port_increments": data_port_increments,
    "data_port_wrap": data_port_wrap,
    "two_ports_share_vram": two_ports_share_vram,
    "isr_sprite_collision": isr_sprite_collision,
    "isr_sprites_disabled": isr_sprites_disabled,
    "isr_latches": isr_latches,
    "timed_scanline": timed_scanline,
    "timed_line_irq": timed_line_irq,
    "timed_irqline_race": timed_irqline_race,
    "timed_vsync_irq": timed_vsync_irq,
    "timed_irq_vstart": timed_irq_vstart,
    "timed_scanline_blank": timed_scanline_blank,
    "timed_line_irq_blank_even": lambda: line_irq_window(range(494, 511, 2)),
    "timed_line_irq_blank_odd": lambda: line_irq_window(range(493, 512, 2)),
}


def capture(name, s, vvp, cache):
    script = "".join(f"{k} {r:x} {v:x}\n" for k, r, v, _ in s.ops)
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


def run(name, vvp, out_dir, header, cache=None):
    s = SCRIPTS[name]()
    got = [ln.split() for ln in capture(name, s, vvp, cache).splitlines() if ln.strip()]
    want = sum(1 for o in s.ops if o[0] in (W, R, PIN))
    if len(got) != want or any("x" in t or "z" in t for ln in got for t in ln):
        raise RuntimeError(f"{name}: bad capture")
    if IRQ_PIN is None and any(o[0] == PIN for o in s.ops):
        return f"{name} (captured; IRQ_PIN unset)"
    it = iter(got)
    timed = False
    with open(os.path.join(out_dir, f"bus_{name}.vec"), "w", newline="\n") as f:
        f.write(header)
        f.write(f"bus {name}\n")
        for k, r, v, m in s.ops:
            if k == AT:
                continue
            if k == F:
                timed |= s.timed
                f.write("v\n" if timed else "f\n")
                continue
            ln = next(it)
            if k == PIN:
                f.write(f"I {ln[1][7 - IRQ_PIN]} {ln[2]}\n")
            elif k == W:
                f.write(f"W {r:02x} {v:02x} {ln[1]}\n" if timed else f"w {r:02x} {v:02x}\n")
            elif timed:
                f.write(f"R {r:02x} {ln[0]} {m:02x} {ln[1]}\n")
            elif m == 0xFF:
                f.write(f"r {r:02x} {ln[0]}\n")
            else:
                f.write(f"r {r:02x} {ln[0]} {m:02x}\n")
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
