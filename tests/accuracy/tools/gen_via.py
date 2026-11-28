#!/usr/bin/env python3
"""Generate VIA bus vectors from a 6522 model run under tb_via.vhd.

    python gen_via.py TB_VIA OUTDIR --commit SHA

TB_VIA is the elaborated testbench (ghdl -e tb_via); it reads a script on
stdin and prints R and I lines. Each vector file is the script's accesses with
the model's answers:

    W reg value T       write at cycle T
    R reg value T       read at cycle T returns value
    I level T           IRQ pin (1 = asserted) as cycle T's data is taken
    P pin value T       input pin driven to value from cycle T on
    O ca2 cb1 cb2 pa paoe pb pboe T
                        outputs as cycle T's data is taken: a pin 0, 1 or Z,
                        a port its value and the bits that drive
"""
import argparse
import os
import subprocess

ORB, ORA, DDRB, DDRA = 0, 1, 2, 3
T1CL, T1CH, T1LL, T1LH, T2CL, T2CH, ACR, PCR, IFR, IER = 4, 5, 6, 7, 8, 9, 11, 12, 13, 14
IRA = 15


def w(r, v):
    return "w %X %02X" % (r, v)


def r(reg):
    return "r %X" % reg


def idle(k):
    return "n %d" % k


def pin(name, v):
    return "p %s %X" % (name, v)


OUTS = "o"


def every_cycle(reg, k):
    out = []
    for _ in range(k):
        out += [r(reg), "i"]
    return out


def outs(k):
    """The outputs in each of the next k cycles."""
    out = []
    for _ in range(k):
        out += [OUTS, idle(1)]
    return out


def edge(pcr, pin, active, ier):
    """A control pin's edge each way, with the flag and IRQ read each cycle."""
    return ([w(PCR, pcr), w(IER, 0x80 | ier), pin_(pin, 1 - active), idle(2), pin_(pin, active)]
            + every_cycle(IFR, 3) + [pin_(pin, 1 - active)] + every_cycle(IFR, 2)
            + [w(IFR, ier), pin_(pin, active)] + every_cycle(IFR, 2))


def pin_(name, v):
    return pin(name, v)


def t1_setup(acr, n):
    return [w(ACR, acr), w(IER, 0xC0), w(T1LL, n & 0xFF), w(T1CH, n >> 8)]


def t2_setup(n):
    return [w(ACR, 0), w(IER, 0xA0), w(T2CL, n & 0xFF), w(T2CH, n >> 8)]


SR = 10


def watch(k, pins=None):
    """IFR, IRQ and the outputs in each of the next k cycles; pins[c] lists
    (pin, level) driven from the c-th of them on."""
    out = []
    for c in range(k):
        for p, v in (pins or {}).get(c, []):
            out.append(pin(p, v))
        out += [r(IFR), "i", OUTS]
    return out


def ext_clock(bits, flip=None):
    """CB1 pulsed from outside, three cycles low and three high, with CB2 at
    each bit while CB1 is low; flip moves CB2 to the other level that many
    cycles after CB1 rises. Returns the cycles taken and the pins."""
    pins, c = {}, 0
    for b in bits:
        pins.setdefault(c, []).extend([("CB2", b), ("CB1", 0)])
        pins.setdefault(c + 3, []).append(("CB1", 1))
        if flip is not None:
            pins.setdefault(c + 3 + flip, []).append(("CB2", b ^ 1))
        c += 6
    return c, pins


def sr_t2_start(k):
    """Mode 101, N=2, the SR written k cycles after T2 starts. T2L is FF when
    k is 3, left out: see README."""
    return ([w(SR, 0x00), w(ACR, 0x14), w(IER, 0x84), w(T2CL, 2), w(T2CH, 0)] + ([idle(k)] if k else [])
            + [w(SR, 0xA5)] + watch(12))


def sr_in_ext(flip):
    c, pins = ext_clock([1, 0, 1, 1, 0, 0, 1, 0, 1], flip)
    return [w(SR, 0x00), w(ACR, 0x0C), w(IER, 0x84), r(SR)] + watch(c + 2, pins) + [r(SR)]


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
    # Control pins: Table 2-5.
    "ca1_negative": edge(0x00, "CA1", 0, 0x02),
    "ca1_positive": edge(0x01, "CA1", 1, 0x02),
    "cb1_negative": edge(0x00, "CB1", 0, 0x10),
    "cb1_positive": edge(0x10, "CB1", 1, 0x10),
    "ca2_negative": edge(0x00, "CA2", 0, 0x01),
    "ca2_positive": edge(0x04, "CA2", 1, 0x01),
    "cb2_negative": edge(0x00, "CB2", 0, 0x08),
    "cb2_positive": edge(0x40, "CB2", 1, 0x08),
    # Flags cleared by port accesses: CA2 and CB2 only when not independent;
    # register 15 clears none.
    "ca_flags_cleared": [w(PCR, 0x00), pin("CA1", 0), pin("CA2", 0), idle(1), r(IFR), r(IRA), r(IFR), r(ORA), r(IFR),
                         pin("CA1", 1), pin("CA2", 1), w(PCR, 0x03), idle(1), pin("CA2", 0), pin("CA1", 0), idle(1),
                         pin("CA1", 1), idle(1), r(IFR), w(ORA, 0), r(IFR), w(IFR, 0x7F), w(PCR, 0x02), pin("CA2", 1),
                         idle(1), pin("CA2", 0), idle(1), r(IFR), r(ORA), r(IFR)],
    "cb_flags_cleared": [w(PCR, 0x00), pin("CB1", 0), pin("CB2", 0), idle(1), r(IFR), r(ORB), r(IFR),
                         w(PCR, 0x20), pin("CB2", 1), pin("CB1", 1), idle(1), pin("CB2", 0), pin("CB1", 0), idle(1),
                         r(IFR), w(ORB, 0), r(IFR)],
    # Input latching, Figure 4-7: the port at the active transition.
    "latch_a": [w(ACR, 0x01), pin("PA", 0x12), idle(1), pin("CA1", 0), idle(1), pin("PA", 0x34), idle(1),
                r(IRA), r(ORA), pin("CA1", 1), idle(1), pin("PA", 0x56), pin("CA1", 0), idle(1), pin("PA", 0x78),
                idle(1), r(IRA),
                w(DDRA, 0xF0), w(ORA, 0xA5), r(ORA), w(ACR, 0x00), r(ORA)],
    "latch_b": [w(ACR, 0x02), pin("PB", 0x12), idle(1), pin("CB1", 0), idle(1), pin("PB", 0x34), idle(1),
                r(ORB), w(DDRB, 0xF0), w(ORB, 0xA5), r(ORB), w(ACR, 0x00), r(ORB)],
    "latch_starts": [pin("PA", 0x12), pin("PB", 0x34), idle(1), w(ACR, 0x03), pin("PA", 0x56), pin("PB", 0x78),
                     idle(1), r(IRA), r(ORB)],
    # The testbench drives every port pin, so it drives an output pin to
    # the VIA's own level.
    "ports_read": [pin("PA", 0x35), pin("PB", 0xC3), w(DDRA, 0x0F), w(ORA, 0x55), w(DDRB, 0xF0), w(ORB, 0xAA),
                   r(IRA), r(ORA), r(ORB)],
    # CA2 and CB2 outputs, Figures 4-3 to 4-6.
    "ca2_read_pulse": [w(PCR, 0x0A), idle(2), r(ORA)] + outs(3) + [r(IRA)] + outs(2),
    "ca2_read_handshake": [w(PCR, 0x08), idle(2), r(ORA)] + outs(3) + [pin("CA1", 0)] + outs(2)
                          + [pin("CA1", 1), idle(1)] + outs(1),
    "ca2_write_pulse": [w(PCR, 0x0A), idle(2), w(ORA, 0x11)] + outs(3) + [w(IRA, 0x22)] + outs(2),
    "ca2_write_handshake": [w(PCR, 0x09), idle(2), w(ORA, 0x11)] + outs(2) + [pin("CA1", 0)] + outs(2)
                           + [pin("CA1", 1)] + outs(2),
    "cb2_write_pulse": [w(PCR, 0xA0), idle(2), w(ORB, 0x11)] + outs(3) + [r(ORB)] + outs(2),
    "cb2_write_handshake": [w(PCR, 0x80), idle(2), w(ORB, 0x11)] + outs(2) + [pin("CB1", 0)] + outs(2),
    "c2_manual": [w(PCR, 0xCC)] + outs(2) + [w(PCR, 0xEE)] + outs(2) + [w(PCR, 0x00)] + outs(2),
    # CA2 and CB2 as outputs set no flag.
    "c2_output_no_flag": [w(PCR, 0xEE), pin("CA2", 0), pin("CB2", 0), idle(1), pin("CA2", 1), pin("CB2", 1), idle(1),
                          r(IFR), w(PCR, 0xCC), pin("CA2", 0), pin("CB2", 0), idle(1), pin("CA2", 1), pin("CB2", 1),
                          idle(1), r(IFR)],
    # From the load on: before it, see README.
    "pb7_output": [w(DDRB, 0x8F), w(ORB, 0x0A)] + t1_setup(0xC0, 3) + [idle(1)] + outs(12),
    # Shift register, 2.11 to 2.13. Written in mode 000 first: the model's
    # reset clears the SR, the emulator's does not. Mode 011 and 111 inputs
    # change only while the pin is low.
    "sr_out_phi2": [w(SR, 0x00), w(IER, 0x84), w(ACR, 0x18), w(SR, 0x5A)] + watch(20)
                   + [r(SR), r(IFR), "i"],
    "sr_in_phi2": [w(SR, 0x00), w(ACR, 0x08), w(IER, 0x84), r(SR)]
                  + watch(20, {0: [("CB2", 1)], 1: [("CB2", 0)], 2: [("CB2", 1)], 4: [("CB2", 0)],
                               9: [("CB2", 1)], 12: [("CB2", 0)], 13: [("CB2", 1)]})
                  + [r(SR), r(IFR)],
    # Figure 2-11: the next SR access starts another eight.
    "sr_out_phi2_again": [w(SR, 0x00), w(ACR, 0x18), w(SR, 0x81), idle(20), r(IFR), w(SR, 0x7E)] + watch(20),
    "sr_out_t2": [w(SR, 0x00), w(IER, 0x84), w(T2CL, 1), w(T2CH, 0), w(ACR, 0x14), w(SR, 0xA6)] + watch(54)
                 + [r(T2CL), r(SR), r(IFR)],
    "sr_t2_start_0": sr_t2_start(0),
    "sr_t2_start_1": sr_t2_start(1),
    "sr_t2_start_2": sr_t2_start(2),
    "sr_t2_start_4": sr_t2_start(4),
    # T2H counts on; IFR5 as T2 passes 0000.
    "sr_t2_counter": [w(ACR, 0x14), w(IER, 0xA0), w(T2CL, 2), w(T2CH, 1), w(SR, 0xA5)]
                     + every_cycle(T2CL, 12) + every_cycle(T2CH, 1) + every_cycle(IFR, 2),
    # T2L reloads only while a T2 mode is selected.
    "sr_t2_mode_late": [w(T2CL, 2), w(T2CH, 0), idle(4), w(ACR, 0x14), w(SR, 0xA5)] + every_cycle(T2CL, 6),
    "sr_free_run": [w(SR, 0x81), w(IER, 0x84), w(T2CL, 1), w(T2CH, 0), w(ACR, 0x10)] + watch(56)
                   + [w(SR, 0x3C)] + watch(54),
    "sr_in_t2": [w(SR, 0x00), w(T2CL, 1), w(T2CH, 0), w(ACR, 0x04), w(IER, 0x84), r(SR)]
                + watch(52, {0: [("CB2", 1)], 6: [("CB2", 0)], 9: [("CB2", 1)], 14: [("CB2", 0)],
                             19: [("CB2", 1)], 26: [("CB2", 0)], 31: [("CB2", 1)], 38: [("CB2", 0)],
                             41: [("CB2", 1)]})
                + [r(SR), r(IFR)],
    # Figure 2-8: CB2 is taken in the first cycle CB1 is high.
    "sr_in_ext": sr_in_ext(None),
    "sr_in_ext_flip_0": sr_in_ext(0),
    "sr_in_ext_flip_1": sr_in_ext(1),
    "sr_out_ext": [w(SR, 0x00), w(ACR, 0x1C), w(IER, 0x84), w(SR, 0xA5)]
                  + watch(56, {c: [("CB1", 0 if c % 6 == 0 else 1)] for c in range(0, 54, 3)})
                  + [r(SR), r(IFR)],
    # While the SR drives CB1, CB1 sets no flag.
    "sr_cb1_driven": [w(PCR, 0x00), w(ACR, 0x18), w(SR, 0x00), idle(20)]
                     + watch(4, {0: [("CB1", 0)], 2: [("CB1", 1)]}) + [w(PCR, 0x10)]
                     + watch(4, {0: [("CB1", 0)], 2: [("CB1", 1)]}),
    # 2.12.1: an access in mode 000 starts nothing.
    "sr_mode_entry": [w(SR, 0x5A), w(ACR, 0x18)] + watch(6),
    # Left low by mode 100, the clock rests high in mode 110 until an access.
    "sr_free_run_stop": [w(T2CL, 3), w(T2CH, 0), w(ACR, 0x10), idle(5), w(ACR, 0x18)] + watch(3)
                        + [w(SR, 0x0F)] + watch(6),
    "sr_mode_off":[w(SR, 0x00), w(ACR, 0x18), w(SR, 0x55), idle(3), w(ACR, 0x00)] + watch(4),
    # IFR and IER.
    "ifr_ier": [w(IER, 0x7F), r(IER), w(IER, 0xC0), r(IER), "i", w(IER, 0x40), r(IER)]
              + t1_setup(0x00, 1) + every_cycle(IFR, 4) + [w(IFR, 0x7F), r(IFR), "i"],
}


def run(tb, script):
    p = subprocess.run([tb], input="\n".join(script) + "\n", capture_output=True, text=True, check=True)
    answers = [l.split() for l in p.stdout.splitlines() if l[:2] in ("R ", "I ", "O ")]
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
        elif f[0] == "p":
            out.append("P %s %s %d" % (f[1], f[2], t))
        elif f[0] == "o":
            a = answers[ai]; ai += 1
            assert a[0] == "O" and int(a[8]) == t - 1, (a, t)
            out.append(" ".join(a[:8]) + " %d" % (t - 1))
    assert ai == len(answers)
    return out


def sr_unchecked(lines):
    """What the datasheet leaves open around the SR, left out of a vector:
    CB2 from an SR or ACR write until CB1 next falls, hatched as undefined in
    Figures 2-10 to 2-12 (the model's level there depends on the testbench's
    previous cycle); and IFR7 and IRQ in the cycle IFR2 sets, where the model
    reads IFR2 set with IRQ still off, which IFR7's definition in 2.14 rules
    out."""
    out, ier, hold, lag, cb1 = [], 0, False, set(), "1"
    for l in lines:
        f = l.split()
        if f[0] == "W" and f[1] == "0E":
            v = int(f[2], 16)
            ier = ier | (v & 0x7F) if v & 0x80 else ier & ~v
        if f[0] == "W" and f[1] in ("0A", "0B"):
            hold = True
        if f[0] == "P" and f[1] == "CB1":
            cb1 = f[2]
        if f[0] == "R" and f[1] == "0D":
            v = int(f[2], 16)
            if v & ier & 0x7F and not v & 0x80:
                lag.add(f[3])
                l += " 7F"
        if f[0] == "I" and f[2] in lag:
            continue
        if f[0] == "O" and hold:
            if f[3] != "Z":
                f[3] = "X"
            hold = (cb1 if f[2] == "Z" else f[2]) != "0"
            l = " ".join(f)
        out.append(l)
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
        if name.startswith("sr_"):
            lines = sr_unchecked(lines)
        with open(os.path.join(a.outdir, "via_%s.vec" % name), "w", newline="\n") as f:
            f.write("# oracle m6522.vhd %s (hoglet67/BeebFpga)\n" % a.commit)
            f.write("\n".join(lines) + "\n")
        names.append("via_%s.vec" % name)
    with open(os.path.join(a.outdir, "MANIFEST"), "w", newline="\n") as f:
        f.write("\n".join(sorted(names)) + "\n")
    print("%d vectors" % len(names))


if __name__ == "__main__":
    main()
