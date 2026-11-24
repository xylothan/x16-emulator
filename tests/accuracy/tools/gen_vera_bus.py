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

W, R, F = 0, 1, 2
IEN_MASK = 0xBF


class S:
    def __init__(self):
        self.ops = []

    def w(self, r, v):
        self.ops.append((W, r, v & 255, 0xFF))

    def r(self, r, mask=0xFF):
        self.ops.append((R, r, 0, mask))

    def f(self):
        self.ops.append((F, 0, 0, 0xFF))


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
}


def run(name, vvp, out_dir, header):
    s = SCRIPTS[name]()
    with tempfile.TemporaryDirectory() as tmp:
        inp, out = os.path.join(tmp, "in"), os.path.join(tmp, "out")
        with open(inp, "w", newline="\n") as f:
            f.writelines(f"{k} {r:x} {v:x}\n" for k, r, v, _ in s.ops)
        subprocess.run(["vvp", vvp, f"+in={inp}", f"+out={out}"], check=True,
                       stdout=subprocess.DEVNULL, timeout=7200)
        with open(out) as f:
            reads = f.read().split()
    want = sum(1 for o in s.ops if o[0] == R)
    if len(reads) != want or any("x" in v or "z" in v for v in reads):
        raise RuntimeError(f"{name}: bad capture")
    it = iter(reads)
    with open(os.path.join(out_dir, f"bus_{name}.vec"), "w", newline="\n") as f:
        f.write(header)
        f.write(f"bus {name}\n")
        for k, r, v, m in s.ops:
            if k == W:
                f.write(f"w {r:02x} {v:02x}\n")
            elif k == F:
                f.write("f\n")
            elif m == 0xFF:
                f.write(f"r {r:02x} {next(it)}\n")
            else:
                f.write(f"r {r:02x} {next(it)} {m:02x}\n")
        f.write("e\n")
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vvp")
    ap.add_argument("out_dir")
    ap.add_argument("--version", required=True)
    ap.add_argument("--sha256", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    header = f"oracle vera {a.version} bitstream-sha256 {a.sha256}\n"
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name in ex.map(lambda n: run(n, a.vvp, a.out_dir, header), SCRIPTS):
            print("done", name, flush=True)


if __name__ == "__main__":
    main()
