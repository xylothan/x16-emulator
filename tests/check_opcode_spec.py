#!/usr/bin/env python3
"""Check the generated opcode tables against the published 65C02 spec.

ProcessorTests measures what the hardware does, across tens of thousands of
randomised cases. This checks something different and much smaller: that the
sizes and cycle counts in src/cpu/tables.h still agree with the documentation.

The two have disagreed once already, over $5C, where both WDC datasheets and
the 6502.org list say eight cycles and the test suite says four. Divergences
like that are listed below rather than hidden, so changing one is a deliberate
act.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Bytes consumed by each addressing mode, including the opcode.
MODE_BYTES = {
    "imp": 1, "acc": 1, "imp8": 1,
    "imm8": 2, "immm": 2, "immx": 2,
    "zp": 2, "zpx": 2, "zpy": 2, "rel": 2,
    "ind0": 2, "indx": 2, "indy": 2,
    "abso": 3, "absx": 3, "absy": 3, "ind": 3, "ainx": 3, "zprel": 3,
}

# Where this emulator knowingly differs from the published 65C02 spec. The
# value is what the emulator is expected to be, so a drift away from the
# recorded difference fails rather than being waved through.
KNOWN = {
    0xDB: ((1, 1), "the X16 maps $DB to a debugger trap rather than STP"),
}

# The spec file is data, and data can be quietly emptied. A truncated file would
# otherwise report zero failures out of zero opcodes and pass.
MIN_ROWS = 120


def read_table(path, decl):
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(decl) + len(decl)
    body = text[start:text.index("};", start)]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [x.strip() for x in body.replace("\n", " ").split(",") if x.strip()]


def read_spec():
    rows = {}
    path = ROOT / "tests" / "spec_65c02.txt"
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        op, size, cycles, note = line.split()
        rows[int(op, 16)] = (int(size), int(cycles), note)
    return rows


def main() -> int:
    modes = read_table("src/cpu/tables.h", "addrtable_c02[256])() = {")
    ticks = [int(x) for x in read_table("src/cpu/tables.h", "ticktable_c02[256] = {")]
    spec = read_spec()

    if len(modes) != 256 or len(ticks) != 256:
        print("FAIL: parsed %d modes and %d cycle counts, expected 256 of each"
              % (len(modes), len(ticks)))
        return 1

    failures = 0
    if len(spec) < MIN_ROWS:
        print("FAIL: spec_65c02.txt holds %d opcodes, expected at least %d"
              % (len(spec), MIN_ROWS))
        failures += 1

    for op in sorted(spec):
        want_size, want_cycles, _note = spec[op]
        mode = modes[op]
        if mode not in MODE_BYTES:
            print("FAIL: $%02X uses addressing mode %s, which has no size here"
                  % (op, mode))
            failures += 1
            continue
        got = (MODE_BYTES[mode], ticks[op])
        if got == (want_size, want_cycles):
            if op in KNOWN:
                print("FAIL: $%02X now matches the spec; drop it from KNOWN (%s)"
                      % (op, KNOWN[op][1]))
                failures += 1
            continue
        if op in KNOWN:
            expected, why = KNOWN[op]
            if got == expected:
                print("ok   : $%02X is %d bytes and %d cycles rather than %d and "
                      "%d -- %s" % (op, got[0], got[1], want_size, want_cycles, why))
            else:
                print("FAIL: $%02X is %d bytes and %d cycles; the spec says %d and "
                      "%d and the recorded difference is %d and %d"
                      % (op, got[0], got[1], want_size, want_cycles,
                         expected[0], expected[1]))
                failures += 1
            continue
        print("FAIL: $%02X is %d bytes and %d cycles, spec says %d and %d"
              % (op, got[0], got[1], want_size, want_cycles))
        failures += 1

    print("%s: %d opcodes checked against the published spec, %d failed"
          % ("opcode_spec", len(spec), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
