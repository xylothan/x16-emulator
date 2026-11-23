#!/usr/bin/env python3
"""compare.py OPS RTL EMU: first divergence per sequence, bucketed."""
import collections
import sys

FIELDS = "rd a0 n0 a1 n1 d0 d1 px py cache accum poke word".split()
REG = {0: "ADDR_L", 1: "ADDR_M", 2: "ADDR_H", 3: "DATA0", 4: "DATA1", 5: "CTRL",
       9: "DC9", 0xA: "DCA", 0xB: "DCB", 0xC: "DCC"}
SKIP_AFTER_RESET = 10


def rd_comparable(reg, dc):
    if reg in (0, 1, 2, 3, 4, 5):
        return True
    if reg == 9:
        return dc == 2
    if reg in (0xB, 0xC):
        return dc == 5
    return False


def main():
    ops = open(sys.argv[1]).read().split("\n")
    rtl = open(sys.argv[2]).read().split("\n")
    emu = open(sys.argv[3]).read().split("\n")
    n = min(len(ops), len(rtl), len(emu))
    if len(rtl) != len(emu):
        print(f"line counts differ: rtl {len(rtl)} emu {len(emu)}")

    buckets = collections.Counter()
    examples = {}
    seqs = clean = total_ops = 0
    i = 0
    while i < n and ops[i]:
        assert ops[i].startswith("2 ")
        start = i
        i += 1
        seqs += 1
        dc = sel = mode = four = 0
        hit = False
        since = 0
        while i < n and ops[i] and not ops[i].startswith("2 "):
            k, reg, val = ops[i].split()
            k, reg, val = int(k), int(reg, 16), int(val, 16)
            total_ops += 1
            since += 1
            if not hit and since > SKIP_AFTER_RESET and rtl[i] != emu[i]:
                a, b = rtl[i].split(), emu[i].split()
                diff = [f for f, x, y in zip(FIELDS, a, b) if x != y
                        and (f != "rd" or (k == 0 and rd_comparable(reg, dc)))]
                if diff:
                    hit = True
                    key = (",".join(diff), ("R " if k == 0 else "W ") + REG.get(reg, hex(reg)),
                           f"dc{dc}" if reg >= 9 else "", f"mode{mode}", "4bit" if four else "8bit")
                    buckets[key] += 1
                    if key not in examples:
                        ctx = [ops[j] for j in range(max(start, i - 5), i + 1)]
                        vals = {f: (a[FIELDS.index(f)], b[FIELDS.index(f)]) for f in diff}
                        examples[key] = (start, i, vals, ctx)
            if k == 1 and reg == 5:
                dc, sel = (val >> 1) & 0x3f, val & 1
            if k == 1 and reg == 9 and dc == 2:
                mode, four = val & 3, (val >> 2) & 1
            i += 1
        clean += not hit

    print(f"sequences {seqs}  ops {total_ops}  clean {clean}  diverged {seqs - clean}")
    for key, c in buckets.most_common(int(sys.argv[4]) if len(sys.argv) > 4 else 40):
        s, at, vals, ctx = examples[key]
        v = " ".join(f"{f}: rtl={x} emu={y}" for f, (x, y) in vals.items())
        print(f"{c:5d}  {' '.join(p for p in key[1:] if p):28} [{key[0]}]  {v}  (line {at + 1})")
    return 0 if clean == seqs else 1


if __name__ == "__main__":
    sys.exit(main())
