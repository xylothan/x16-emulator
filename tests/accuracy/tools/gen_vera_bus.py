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


def line_irq_window(lines, frames=1):
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0)
    for _ in range(frames):
        s.v()
    for line in lines:
        s.at(at_pos(line - 1, 100))
        set_irqline(s, line, 0x02); s.w(7, 0x07)
        for x in range(200, 2 * LINE + 51, 50):
            s.at(at_pos(line - 1, x) - 1); s.pin()
    return s


def timed_scanline_steady():
    s = S()
    s.w(5, 0); s.w(9, 0x01); s.w(6, 0)
    s.v(); s.v()
    for y in range(492, 525):
        read_at(s, y, 400, 8); read_at(s, y, 413, 6)
        read_at(s, y, 794, 8); read_at(s, y, 807, 6)
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


# Sprite pixels: 0x0000-0x00FF opaque, 0x0100-0x01FF transparent, and from
# 0x0200 16-byte rows opaque only in their first 8 bytes.
OPAQUE, CLEAR, HALF = 0x0000, 0x0100, 0x0200


def spr(i, x, y, z, mask, addr=OPAQUE, bpp=0, wz=0, hz=0, flip=0):
    return (i, (addr >> 5) & 0xFF, (bpp << 7) | (addr >> 13), x & 0xFF, (x >> 8) & 3, y & 0xFF, (y >> 8) & 3,
            (mask << 4) | (z << 2) | flip, (hz << 6) | (wz << 4))


def sprite_scene(s, sprites, dcsel1=None, scale=None):
    s.w(5, 0); s.w(0, 0); s.w(1, 0); s.w(2, 0x10)
    for k in range(0x300):
        s.w(3, 0xFF if k < 0x100 or (k >= 0x200 and k % 16 < 8) else 0)
    for i, *attr in sprites:
        s.w(0, (i * 8) & 0xFF); s.w(1, 0xFC | (i >> 5)); s.w(2, 0x11)
        for v in attr:
            s.w(3, v)
    if dcsel1:
        s.w(5, 2)
        for r, v in zip((9, 10, 11, 12), dcsel1):
            s.w(r, v)
        s.w(5, 0)
    if scale:
        s.w(10, scale); s.w(11, scale)
    s.w(9, 0x41)


def collide(sprites, dcsel1=None, scale=None):
    """ISR after two frames: bit 2 latches, bits 7:4 are the last frame's mask (top.v:179, 450)."""
    def script():
        s = S()
        sprite_scene(s, sprites, dcsel1, scale)
        s.f(); s.f()
        s.r(7, 0xF7)
        s.w(7, 0xFF); s.r(7, 0xF7)
        return s
    return script


def collide_settled(sprites, dcsel1=None):
    """collide(), counting only the third frame: the first runs while the scene is still being written."""
    def script():
        s = S()
        sprite_scene(s, sprites, dcsel1)
        s.f(); s.f()
        s.w(7, 0xFF)
        s.f()
        s.r(7, 0xF7)
        s.w(7, 0xFF); s.r(7, 0xF7)
        return s
    return script


def on_row(r):
    """Two sprites that share only row r."""
    return [spr(0, 100, r - 7, 1, 0x1), spr(1, 100, r, 2, 0x1)]


def last_clock(x9):
    """Nine 64-pixel 4bpp sprites take 83 clocks each, so sprite 9's pixel 38 lands on
    render_time_r 798 (sprite_renderer.v:44) and only it can reach sprite 8 at x=338."""
    return collide([spr(i, 0, 100, 1, 0, wz=3) for i in range(8)]
                   + [spr(8, 338, 100, 1, 0x1, wz=3), spr(9, x9, 100, 2, 0x1, wz=3)])


def timed_collision():
    s = S()
    sprite_scene(s, [spr(0, 100, 100, 3, 0x9), spr(1, 104, 104, 1, 0x3)])
    s.w(6, 0)
    s.v(); s.w(7, 0x07); s.w(6, 0x04)
    pins_around(s, 480, 3, 4)
    read_at(s, 480, 8, 7, 0xF7)
    s.w(7, 0x04); s.pin(); s.pin()
    s.v(); s.w(6, 0); s.w(7, 0x07)
    for x in range(-3, 4):
        read_at(s, 480, x, 7, 0xF7)
    s.v(); s.w(7, 0x07)
    s.w(0, 0x0E); s.w(1, 0xFC); s.w(2, 0x01); s.w(3, 0)
    for x in (-1, 0, 1, 2):
        read_at(s, 480, x, 7, 0xF7)
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
    "timed_scanline_steady": timed_scanline_steady,
    "timed_line_irq_steady_even": lambda: line_irq_window(range(494, 511, 2), 2),
    "timed_line_irq_steady_odd": lambda: line_irq_window(range(493, 512, 2), 2),
    # sprite_renderer.v:319, 326-328: a pixel adds its mask to the line buffer only if drawn.
    "coll_hidden_behind": collide([spr(0, 100, 100, 3, 0x1), spr(1, 100, 100, 1, 0x2), spr(2, 100, 100, 1, 0x2)]),
    "coll_hidden_equal_z": collide([spr(0, 100, 100, 2, 0x1), spr(1, 100, 100, 2, 0x2), spr(2, 100, 100, 2, 0x2)]),
    "coll_behind_counts": collide([spr(0, 100, 100, 3, 0x3), spr(1, 100, 100, 1, 0x2)]),
    "coll_drawn_part": collide([spr(0, 100, 100, 3, 0x1), spr(1, 104, 100, 1, 0x2), spr(2, 108, 100, 1, 0x2)]),
    "coll_hidden_part": collide([spr(0, 100, 100, 3, 0x1), spr(1, 104, 100, 1, 0x2), spr(2, 100, 100, 1, 0x2)]),
    "coll_transparent": collide([spr(0, 100, 100, 3, 0x1), spr(1, 100, 100, 1, 0x1, addr=CLEAR),
                                 spr(2, 100, 100, 1, 0x1, addr=CLEAR)]),
    "coll_mask_zero": collide([spr(0, 100, 100, 3, 0x0), spr(1, 100, 100, 1, 0xF), spr(2, 100, 100, 2, 0x0)]),
    "coll_8bpp_hflip": collide([spr(0, 200, 50, 1, 0x4, HALF, bpp=1, wz=1, flip=1),
                                spr(1, 208, 50, 2, 0xC, HALF, bpp=1, wz=1)]),
    "coll_8bpp_noflip": collide([spr(0, 200, 50, 1, 0x4, HALF, bpp=1, wz=1),
                                 spr(1, 208, 50, 2, 0xC, HALF, bpp=1, wz=1)]),
    # composer.v:157-161 compares the scaled line before it steps.
    "coll_line_480": collide([spr(0, 100, 473, 1, 0x1), spr(1, 100, 480, 2, 0x1)]),
    "coll_line_479": collide([spr(0, 100, 472, 1, 0x1), spr(1, 100, 479, 2, 0x1)]),
    # sprite_renderer.v:326-327: none at line buffer index 640 or past it.
    "coll_at_640": collide([spr(0, 636, 100, 1, 0x1), spr(1, 640, 100, 2, 0x1)]),
    "coll_before_640": collide([spr(0, 636, 100, 1, 0x1), spr(1, 638, 100, 2, 0x2), spr(2, 639, 100, 3, 0x2)]),
    "coll_left_edge": collide([spr(0, -4, 100, 1, 0x1), spr(1, 0, 100, 2, 0x1)]),
    "coll_off_left": collide([spr(0, -8, 100, 1, 0x1), spr(1, -8, 100, 2, 0x1), spr(2, 1016, 200, 3, 0x2),
                              spr(3, -8, 200, 2, 0x2)]),
    # composer.v:147-162: only lines given render_start are drawn, so only they collide.
    "coll_below_vstop": collide([spr(0, 100, 400, 1, 0x1), spr(1, 100, 400, 2, 0x1)], dcsel1=(0, 0xA0, 0, 0x80)),
    "coll_above_vstop": collide([spr(0, 100, 250, 1, 0x1), spr(1, 100, 250, 2, 0x1)], dcsel1=(0, 0xA0, 0, 0x80)),
    "coll_window_low": collide([spr(0, 100, 220, 1, 0x1), spr(1, 100, 220, 2, 0x1)], dcsel1=(0, 0xA0, 0x80, 0xF0)),
    "coll_window_out": collide([spr(0, 100, 240, 1, 0x1), spr(1, 100, 240, 2, 0x1)], dcsel1=(0, 0xA0, 0x80, 0xF0)),
    "coll_empty_window_l0": collide([spr(0, 100, 0, 1, 0x1), spr(1, 100, 0, 2, 0x1)], dcsel1=(0, 0xA0, 0x80, 0x80)),
    "coll_empty_window_l1": collide([spr(0, 100, 1, 1, 0x1), spr(1, 100, 1, 2, 0x1)], dcsel1=(0, 0xA0, 0x80, 0x80)),
    # composer.v:150-161: VSTART 496, VSTOP 510 draws line indexes 0-14 in blanking.
    "coll_vstart_blank_r0": collide_settled(on_row(0), dcsel1=(0, 0xA0, 0xF8, 0xFF)),
    "coll_vstart_blank_r14": collide_settled(on_row(14), dcsel1=(0, 0xA0, 0xF8, 0xFF)),
    "coll_vstart_blank_r15": collide_settled(on_row(15), dcsel1=(0, 0xA0, 0xF8, 0xFF)),
    # composer.v:134, 157: the line at VSTOP is still drawn, though not shown.
    "coll_vstop_r256": collide_settled(on_row(256), dcsel1=(0, 0xA0, 0, 0x80)),
    "coll_vstop_r257": collide_settled(on_row(257), dcsel1=(0, 0xA0, 0, 0x80)),
    "coll_vscale": collide([spr(0, 100, 250, 1, 0x1), spr(1, 100, 250, 2, 0x1)], scale=0x40),
    "coll_vscale_in": collide([spr(0, 100, 230, 1, 0x1), spr(1, 100, 230, 2, 0x1)], scale=0x40),
    "coll_clock_797": last_clock(301),
    "coll_clock_798": last_clock(300),
    "coll_clock_799": last_clock(299),
    "timed_collision": timed_collision,
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
