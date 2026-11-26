#!/usr/bin/env python3
"""Generate VIA bus vectors from a 6522 model run under tb_via.vhd.

    python gen_via.py TB_VIA OUTDIR --commit SHA

TB_VIA is the elaborated testbench (ghdl -e tb_via); it reads a script on
stdin and prints R and I lines. Each vector file is the script's accesses with
the model's answers:

    W reg value T       write at cycle T
    R reg value T       read at cycle T returns value
    I level T           IRQ pin (1 = asserted) as cycle T's data is taken
"""
import argparse
import os
import subprocess

T1CL, T1CH, T1LL, T1LH, T2CL, T2CH, ACR, IFR, IER = 4, 5, 6, 7, 8, 9, 11, 13, 14


def w(r, v):
    return "w %X %02X" % (r, v)


def r(reg):
    return "r %X" % reg


def idle(k):
    return "n %d" % k


def every_cycle(reg, k):
    out = []
    for _ in range(k):
        out += [r(reg), "i"]
    return out


def t1_setup(acr, n):
    return [w(ACR, acr), w(IER, 0xC0), w(T1LL, n & 0xFF), w(T1CH, n >> 8)]


def t2_setup(n):
    return [w(ACR, 0), w(IER, 0xA0), w(T2CL, n & 0xFF), w(T2CH, n >> 8)]


SCRIPTS = {
    # Counter read every cycle across the time-out: Figure 2-3.
    "t1_oneshot_count": t1_setup(0x00, 5) + every_cycle(T1CH, 1) + every_cycle(T1CL, 14),
    "t1_oneshot_ifr": t1_setup(0x00, 5) + every_cycle(IFR, 16),
    # Free-run: the flag again N+2 cycles later, Figure 2-4.
    "t1_freerun_ifr": t1_setup(0x40, 5) + every_cycle(IFR, 8) + [w(IFR, 0x40)] + every_cycle(IFR, 9),
    "t1_freerun_count": t1_setup(0x40, 3) + every_cycle(T1CL, 14),
    "t1_freerun_n0": t1_setup(0x40, 0) + every_cycle(IFR, 3) + [w(IFR, 0x40)] + every_cycle(IFR, 3),
    "t1_freerun_n1": t1_setup(0x40, 1) + every_cycle(T1CL, 10),
    # Free-run with the flag left set, across many periods without an access.
    "t1_freerun_idle": t1_setup(0x40, 3) + [idle(41)] + every_cycle(T1CL, 6)
                       + [idle(30)] + every_cycle(T1CL, 6),
    "t1_oneshot_idle": t1_setup(0x00, 2) + [idle(37)] + every_cycle(T1CL, 6) + every_cycle(IFR, 1),
    # Reading T1C-L clears the flag; in the cycle the flag sets, and after.
    "t1_read_clears_at_timeout": t1_setup(0x00, 4) + [idle(5)] + every_cycle(T1CL, 1) + every_cycle(IFR, 3),
    "t1_read_clears_after": t1_setup(0x00, 4) + [idle(6)] + every_cycle(T1CL, 1) + every_cycle(IFR, 3),
    "t1_read_clears_before": t1_setup(0x00, 4) + [idle(4)] + every_cycle(T1CL, 1) + every_cycle(IFR, 3),
    # Writing T1C-H on or around the time-out.
    "t1_reload_at_timeout": t1_setup(0x00, 4) + [idle(5), w(T1CH, 0)] + every_cycle(IFR, 8),
    "t1_reload_early": t1_setup(0x00, 4) + [idle(3), w(T1CH, 0)] + every_cycle(IFR, 8),
    "t1_reload_after_timeout": t1_setup(0x00, 4) + [idle(6), w(T1CH, 0)] + every_cycle(IFR, 8),
    # T1L-H write clears the flag; latches take effect at the next reload.
    "t1_latch_high_clears": t1_setup(0x00, 2) + [idle(5), r(IFR), w(T1LH, 0), r(IFR), "i"],
    "t1_latch_next_period": t1_setup(0x40, 4) + [w(T1LL, 2)] + every_cycle(T1CL, 14),
    # One-shot: no second flag, the counter still reloads and runs.
    "t1_oneshot_once": t1_setup(0x00, 3) + [idle(6), w(IFR, 0x40)] + every_cycle(IFR, 2) + [idle(6)] + every_cycle(IFR, 2) + every_cycle(T1CL, 6),
    # T2: Figure 2-3, then counts on past FFFF without reloading.
    "t2_count": t2_setup(5) + every_cycle(T2CH, 1) + every_cycle(T2CL, 12),
    "t2_ifr": t2_setup(5) + every_cycle(IFR, 10),
    "t2_read_clears": t2_setup(3) + [idle(4)] + every_cycle(T2CL, 1) + every_cycle(IFR, 3),
    "t2_reload_at_timeout": t2_setup(4) + [idle(5), w(T2CH, 0)] + every_cycle(IFR, 8),
    "t2_once": t2_setup(2) + [idle(5), w(IFR, 0x20)] + [idle(300)] + every_cycle(IFR, 2),
    # IFR and IER.
    "ifr_ier": [w(IER, 0x7F), r(IER), w(IER, 0xC0), r(IER), "i", w(IER, 0x40), r(IER)]
              + t1_setup(0x00, 1) + every_cycle(IFR, 4) + [w(IFR, 0x7F), r(IFR), "i"],
}


def run(tb, script):
    p = subprocess.run([tb], input="\n".join(script) + "\n", capture_output=True, text=True, check=True)
    answers = [l.split() for l in p.stdout.splitlines() if l[:2] in ("R ", "I ")]
    return answers


def vector(script, answers):
    out, t, ai = [], 0, 0
    for line in script:
        f = line.split()
        if f[0] == "w":
            out.append("W %s %s %d" % (f[1].upper().zfill(2), f[2].upper(), t))
            t += 1
        elif f[0] == "r":
            a = answers[ai]; ai += 1
            assert a[0] == "R" and int(a[3]) == t, (a, t)
            out.append("R %s %s %d" % (a[1], a[2], t))
            t += 1
        elif f[0] == "n":
            t += int(f[1])
        elif f[0] == "i":
            a = answers[ai]; ai += 1
            assert a[0] == "I" and int(a[2]) == t - 1, (a, t)
            out.append("I %s %d" % (a[1], t - 1))
    assert ai == len(answers)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tb")
    ap.add_argument("outdir")
    ap.add_argument("--commit", required=True)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    names = []
    for name, script in SCRIPTS.items():
        lines = vector(script, run(a.tb, script))
        with open(os.path.join(a.outdir, "via_%s.vec" % name), "w", newline="\n") as f:
            f.write("# oracle m6522.vhd %s (hoglet67/BeebFpga)\n" % a.commit)
            f.write("\n".join(lines) + "\n")
        names.append("via_%s.vec" % name)
    with open(os.path.join(a.outdir, "MANIFEST"), "w", newline="\n") as f:
        f.write("\n".join(sorted(names)) + "\n")
    print("%d vectors" % len(names))


if __name__ == "__main__":
    main()
