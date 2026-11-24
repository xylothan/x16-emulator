#!/usr/bin/env python3
"""Generate VERA scene vectors from a gate-level netlist of a released bitstream.

    gen_vera_scenes.py VVP OUT_DIR --version 49.0.6 --sha256 HEX [--jobs N] [--only NAME...]

VVP is tb_scene.v compiled with the netlist. The bitstream and netlist are not
part of this repository.
"""

import argparse
import concurrent.futures as cf
import os
import random
import struct
import subprocess
import tempfile
import zlib

WIDTH, HEIGHT = 640, 480
VRAM_SIZE = 0x20000
SEED_BASE = 0x49000000


def xorshift_vram(seed):
    x = seed or 1
    out = bytearray(VRAM_SIZE)
    for i in range(0, VRAM_SIZE, 4):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        struct.pack_into("<I", out, i, x)
    return out


def build_ops(seed, c):
    r = random.Random(seed)
    ops = []
    w = lambda a, v: ops.append((a, v & 255))

    w(5, 0); w(0, 0); w(1, 0xFA); w(2, 0x11)
    for _ in range(256):
        v = r.getrandbits(12)
        w(3, v); w(3, v >> 8)

    spr = c.get("sprites", 0)
    fixed = c.get("sprite_list")
    armed = []
    w(5, 0); w(0, 0); w(1, 0xFC); w(2, 0x11)
    for i in range(128):
        if fixed is not None and i < len(fixed):
            for v in fixed[i]:
                w(3, v)
        elif i < spr:
            a = r.getrandbits(12)
            bpp = c.get("bpp", r.getrandbits(1))
            x = r.randrange(-64, 640) & 0x3FF
            y = r.randrange(-64, 480) & 0x3FF
            z = r.choice(c.get("z", [1, 2, 3]))
            fl = c.get("flipv", r.getrandbits(2)) if c.get("flip", True) else 0
            size = c.get("size")
            wz = c.get("wz", r.randrange(4) if size is None else size)
            hz = c.get("hz", r.randrange(4) if size is None else size)
            mask = r.getrandbits(4) if c.get("collide", True) else 0
            b6 = (mask << 4) | (z << 2) | fl
            if c.get("armed"):
                armed.append((i, b6))
                b6 &= ~0x0C
            for v in (a, (a >> 8) | (bpp << 7), x, x >> 8, y, y >> 8,
                      b6, (hz << 6) | (wz << 4) | r.getrandbits(4)):
                w(3, v)
        else:
            for _ in range(8):
                w(3, 0)

    def layer(base, l):
        depth = l["depth"]
        bm = l.get("bitmap", 0)
        mw = l.get("mw", r.randrange(4)) if not bm else 0
        mh = l.get("mh", r.randrange(4)) if not bm else 0
        w(base, (mh << 6) | (mw << 4) | (l.get("t256", 0) << 3) | (bm << 2) | depth)
        w(base + 1, r.getrandbits(8))
        if bm:
            w(base + 2, (r.randrange(64) << 2) | l.get("wide", 0))
        else:
            w(base + 2, (r.randrange(64) << 2) | (l.get("th", r.getrandbits(1)) << 1)
              | l.get("tw", r.getrandbits(1)))
        hs = r.getrandbits(12) if l.get("scroll") else 0
        vs = r.getrandbits(12) if l.get("scroll") else 0
        if bm:
            hs = r.getrandbits(4) << 8
        w(base + 3, hs); w(base + 4, hs >> 8); w(base + 5, vs); w(base + 6, vs >> 8)

    en = 0x01
    if "l0" in c:
        layer(0x0D, c["l0"]); en |= 0x10
    if "l1" in c:
        layer(0x14, c["l1"]); en |= 0x20
    if spr or fixed:
        en |= 0x40
    w(5, 2)
    h0, h1, v0, v1 = c.get("win", (0, 0xA0, 0, 0xF0))
    w(9, h0); w(10, h1); w(11, v0); w(12, v1)
    w(5, 0)
    w(9, 0x01)
    w(10, c.get("hs", 0x80)); w(11, c.get("vs", 0x80)); w(12, c.get("border", r.getrandbits(8)))
    # Sprite buffer entries 640 and 641 keep their first write, so armed sprites
    # get their Z in blanking, before any line has rendered them.
    start = len(ops)
    if armed:
        w(0, 0); w(1, 0xFC); w(2, 0x01)
        for i, b6 in armed:
            w(0, i * 8 + 6); w(3, b6)
    w(9, en)
    return ops, len(ops) - start


def scenes():
    s = {}
    for d in range(4):
        for wide in (0, 1):
            s[f"bitmap_d{d}_w{wide}"] = dict(l0=dict(bitmap=1, depth=d, wide=wide))
            s[f"bitmap_l1_d{d}_w{wide}"] = dict(l1=dict(bitmap=1, depth=d, wide=wide))
    for d in range(4):
        for tw in (0, 1):
            for th in (0, 1):
                s[f"tile_d{d}_{8 << tw}x{8 << th}"] = dict(l0=dict(depth=d, tw=tw, th=th))
                s[f"tile_d{d}_{8 << tw}x{8 << th}_scroll"] = dict(l0=dict(depth=d, tw=tw, th=th, scroll=1))
    for mw in range(4):
        for mh in range(4):
            s[f"tile_map_{32 << mw}x{32 << mh}"] = dict(l0=dict(depth=2, mw=mw, mh=mh, scroll=1))
    s["tile_t256"] = dict(l0=dict(depth=0, t256=1, tw=0, th=0))
    s["tile_t256_scroll"] = dict(l0=dict(depth=0, t256=1, scroll=1))
    for h in (0x10, 0x20, 0x40, 0x55, 0x67, 0x81, 0x9A, 0xC0, 0xE7, 0xFF):
        s[f"scale_{h:02x}"] = dict(l0=dict(depth=2, scroll=1), hs=h, vs=h)
        s[f"scale_bm_{h:02x}"] = dict(l0=dict(bitmap=1, depth=3), hs=h, vs=h)
    for h in (0x40, 0xC0, 0xFF):
        s[f"hscale_{h:02x}"] = dict(l0=dict(depth=3, scroll=1), hs=h)
        s[f"vscale_{h:02x}"] = dict(l0=dict(depth=3, scroll=1), vs=h)
    s["window"] = dict(l0=dict(depth=3, scroll=1), win=(0x10, 0x88, 0x18, 0xD0), border=0x2A)
    s["window_scaled"] = dict(l0=dict(depth=2, scroll=1), win=(0x21, 0x77, 0x11, 0xC3), hs=0x40, vs=0x40)
    s["window_past_480"] = dict(l0=dict(depth=2, scroll=1), win=(0x08, 0x90, 0x11, 0xE5), hs=0xC0, vs=0xC0, border=0x2A)
    s["window_bm_past_480"] = dict(l0=dict(bitmap=1, depth=3), win=(0x08, 0x90, 0x20, 0xF0), vs=0xD3, border=0x55)
    s["border_only"] = dict(border=0x7E)
    s["layers"] = dict(l0=dict(depth=2, scroll=1), l1=dict(depth=1, scroll=1))
    s["layers_mix"] = dict(l0=dict(bitmap=1, depth=3), l1=dict(depth=0, t256=1, scroll=1))
    s["layers_past_480"] = dict(l0=dict(depth=2, scroll=1), l1=dict(bitmap=1, depth=2), vs=0xB7)
    for wz in range(4):
        for hz in range(4):
            s[f"sprites_{8 << wz}x{8 << hz}"] = dict(sprites=10, wz=wz, hz=hz, z=[3])
    for fl in range(4):
        s[f"sprites_flip{fl}"] = dict(sprites=16, flipv=fl, z=[3])
    for bpp in (0, 1):
        s[f"sprites_bpp{bpp}"] = dict(sprites=24, bpp=bpp, z=[3])
    for z in (1, 2, 3):
        s[f"sprites_z{z}"] = dict(l0=dict(depth=2, scroll=1), l1=dict(depth=1, scroll=1), sprites=12, z=[z])
    s["sprites_many"] = dict(l0=dict(depth=2), sprites=128)
    s["sprites_scaled"] = dict(l0=dict(depth=2), sprites=20, hs=0x40, vs=0x40)
    s["sprites_past_640_a"] = dict(l0=dict(depth=2, scroll=1), sprites=24, vs=0xC0, hs=0xC0, armed=1)
    s["sprites_past_640_b"] = dict(l0=dict(depth=2, scroll=1), sprites=24, vs=0xC0, hs=0xC0, armed=1, frame=2)
    s["sprites_past_640_c"] = dict(l0=dict(depth=2, scroll=1), sprites=24, vs=0xC0, hs=0xC0, armed=1, frame=3)
    return s


def line_crcs(pixels):
    out = []
    for y in range(HEIGHT):
        line = bytearray()
        for p in pixels[y * WIDTH:(y + 1) * WIDTH]:
            line += struct.pack("<H", int(p, 16))
        out.append(zlib.crc32(bytes(line)))
    return out


def run(item, vvp, out_dir, header):
    index, (name, cfg) = item
    seed = SEED_BASE + index
    ops, final_ops = build_ops(seed, cfg)
    frame = cfg.get("frame", 1)
    with tempfile.TemporaryDirectory() as tmp:
        vram, opsf, frame_out = (os.path.join(tmp, n) for n in ("vram", "ops", "frame"))
        with open(vram, "wb") as f:
            f.write(xorshift_vram(seed))
        with open(opsf, "w", newline="\n") as f:
            f.writelines(f"{a:x} {v:x}\n" for a, v in ops)
        subprocess.run(["vvp", vvp, f"+vram={vram}", f"+ops={opsf}", f"+out={frame_out}",
                        f"+frame={frame}", f"+final={final_ops}"], check=True, stdout=subprocess.DEVNULL, timeout=7200)
        with open(frame_out) as f:
            pixels = f.read().split()
    if len(pixels) != WIDTH * HEIGHT:
        raise RuntimeError(f"{name}: captured {len(pixels)} pixels")
    with open(os.path.join(out_dir, f"{name}.vec"), "w", newline="\n") as f:
        f.write(header)
        f.write(f"scene {name}\nseed {seed:08x}\nfinal {final_ops}\nframe {frame}\n")
        f.write("ops " + "".join(f"{a:02x}{v:02x}" for a, v in ops) + "\n")
        f.write("crc " + " ".join(f"{c:08x}" for c in line_crcs(pixels)) + "\n")
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vvp")
    ap.add_argument("out_dir")
    ap.add_argument("--version", required=True)
    ap.add_argument("--sha256", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    header = f"oracle vera {a.version} bitstream-sha256 {a.sha256}\n"
    all_scenes = list(scenes().items())
    items = [(i, s) for i, s in enumerate(all_scenes) if not a.only or s[0] in a.only]
    print("scenes", len(items), "jobs", a.jobs, flush=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for name in ex.map(lambda it: run(it, a.vvp, a.out_dir, header), items):
            print("done", name, flush=True)


if __name__ == "__main__":
    main()
