#!/usr/bin/env python3
"""gen_ops.py SEED SEQUENCES OPS_PER_SEQ OUT [--special]

Randomized VERA register traffic. Each sequence starts from reset. --special
also aims the ports at the PSG/palette/sprite region.
"""
import random
import sys


def sequence(r, nops, special):
    o = [(2, 0, 0)]
    state = {"sel": 0}

    def W(a, v):
        o.append((1, a, v & 0xff))

    def R(a):
        o.append((0, a, 0))

    def ctrl(dc, sel=None):
        if sel is None:
            sel = state["sel"]
        state["sel"] = sel
        W(5, (dc << 1) | sel)

    def rand_addr():
        if special and r.random() < 0.3:
            return r.randint(0x1f9a0, 0x1ffff)
        return r.randint(0, 0x10000)

    def set_addr(sel):
        ctrl(0, sel)
        a = rand_addr()
        W(0, a)
        W(1, a >> 8)
        inc = r.choice([0, 0, 1, 1, 1, 2, 3, 4, 9, 14, r.randint(0, 15)])
        W(2, ((a >> 16) & 1) | (r.getrandbits(1) << 1) | (r.getrandbits(1) << 2) |
          (r.getrandbits(1) << 3) | (inc << 4))

    def set_incr():
        ctrl(3)
        for reg in (9, 0xA, 0xB, 0xC):
            if reg in (0xA, 0xC):
                W(reg, r.choice([0, 1, 2, 0x7f, 0x40, 0x80, 0x81, r.getrandbits(8)]))
            else:
                W(reg, r.getrandbits(8))

    def set_pos():
        ctrl(4)
        W(9, r.getrandbits(8))
        W(0xA, r.choice([0, 0, 1, 2, 7, 0x80, r.getrandbits(8)]))
        W(0xB, r.getrandbits(8))
        W(0xC, r.choice([0, 0, 1, 2, 7, 0x80, r.getrandbits(8)]))
        if r.random() < 0.5:
            ctrl(5)
            W(9, r.getrandbits(8))
            W(0xA, r.getrandbits(8))

    def set_cache():
        ctrl(6)
        for reg in (9, 0xA, 0xB, 0xC):
            W(reg, r.choice([0, 0xff, 1, 0x80, r.getrandbits(8), r.getrandbits(8)]))

    def fx_setup():
        ctrl(2)
        base = r.getrandbits(5) << 3
        W(0xA, base | r.getrandbits(2))
        W(0xB, (r.getrandbits(5) << 3) | r.getrandbits(2))
        W(0xC, r.getrandbits(6))
        mode = r.choice([0, 1, 1, 2, 2, 3, 3])
        bits = 0
        for bit, p in ((2, .5), (3, .25), (4, .3), (5, .35), (6, .35), (7, .3)):
            if r.random() < p:
                bits |= 1 << bit
        W(9, bits | mode)

    set_addr(0)
    set_addr(1)
    while len(o) < nops:
        steps = [set_cache, set_incr, set_pos]
        r.shuffle(steps)
        if r.random() < 0.5:
            fx_setup()
            for s in steps:
                s()
        else:
            for s in steps:
                s()
            fx_setup()
        set_addr(0) if r.random() < 0.3 else None
        for _ in range(r.randint(8, 70)):
            x = r.random()
            if x < .20:
                R(3)
            elif x < .42:
                R(4)
            elif x < .54:
                W(3, r.getrandbits(8))
            elif x < .72:
                W(4, r.choice([0, r.getrandbits(8), r.getrandbits(8)]))
            elif x < .77:
                ctrl(5, r.getrandbits(1))
                R(0xB)
                R(0xC)
            elif x < .83:
                ctrl(6)
                R(r.choice((9, 0xA, 0xA)))
            elif x < .88:
                ctrl(2)
                W(0xC, r.getrandbits(8) & 0x7f if r.random() < .8 else r.getrandbits(8))
            elif x < .91:
                ctrl(0, r.getrandbits(1))
                R(r.choice((0, 1, 2)))
            elif x < .94:
                ctrl(0, 1)
                W(0, r.getrandbits(8))
            elif x < .96:
                set_pos()
            elif x < .98:
                set_incr()
            else:
                ctrl(2)
                R(9)
    return o


def plain_sequence(r, nops):
    """ADDR/DATA traffic with every increment, never touching an FX register."""
    o = [(2, 0, 0)]

    def W(a, v):
        o.append((1, a, v & 0xff))

    def set_addr(sel):
        W(5, sel)
        a = r.choice([r.randint(0, 0x1ffff), r.randint(0, 0x30), r.randint(0x1ffd0, 0x1ffff),
                      r.randint(0x1f9a0, 0x1ffff)])
        W(0, a)
        W(1, a >> 8)
        W(2, ((a >> 16) & 1) | (r.getrandbits(1) << 3) | (r.randint(0, 15) << 4))

    set_addr(0)
    set_addr(1)
    while len(o) < nops:
        x = r.random()
        if x < .28:
            o.append((0, 3, 0))
        elif x < .56:
            o.append((0, 4, 0))
        elif x < .70:
            W(3, r.getrandbits(8))
        elif x < .84:
            W(4, r.getrandbits(8))
        elif x < .92:
            W(5, r.getrandbits(1))
            o.append((0, r.choice((0, 1, 2, 5)), 0))
        else:
            set_addr(r.getrandbits(1))
    return o


def main():
    seed, seqs, nops, path = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    special = "--special" in sys.argv
    plain = "--no-fx" in sys.argv
    r = random.Random(seed)
    with open(path, "w", newline="\n") as f:
        for _ in range(seqs):
            ops = plain_sequence(r, nops) if plain else sequence(r, nops, special)
            for k, a, v in ops:
                f.write(f"{k} {a:x} {v:x}\n")


if __name__ == "__main__":
    main()
