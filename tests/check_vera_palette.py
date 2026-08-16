#!/usr/bin/env python3
"""Check the emulator's default palette against the one in VERA's bitstream.

VERA's palette RAM is initialised at configuration time from a Lattice memory
file, fpga/source/palette_ram.mem, baked into the FPGA bitstream. That makes
the default palette a property of the hardware rather than a convention the
ROM establishes, so src/video.c's default_palette[] is checkable against it
entry for entry.

    python tests/check_vera_palette.py

Skips when the RTL has not been fetched. Nothing in the suite requires the
reference to be present -- run tests/fetch_vera_rtl.py to get it.

WHY THIS IS A SCRIPT AND NOT A C TEST
-------------------------------------
The palette the renderer uses lives in a static array with no accessor, and
the only path to it is rendered pixels, which need the framebuffer. Comparing
the two declarations needs neither. It also catches an entry being edited by
hand, which is the way a 256-entry table actually goes wrong.

WHAT THIS DOES NOT CHECK
------------------------
How a 4-bit channel becomes an 8-bit one. video.c:792-794 replicates the
nibble, so 0xF becomes 0xFF. The RTL cannot settle that: video_vga.v declares
vga_r, vga_g and vga_b as [3:0] and assigns palette_rgb_data[11:8] etc.
straight to them, so the hardware terminates at four bits per channel and the
expansion is the emulator's own presentation choice. Asserting it against the
Verilog would be inventing an oracle.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VIDEO_C = ROOT / "src" / "video.c"
MEM = ROOT / "tests" / "vera-rtl" / "palette_ram.mem"

ENTRIES = 256


def emulator_palette():
    src = VIDEO_C.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"default_palette\[\]\s*=\s*\{(.*?)\}\s*;", src, re.S)
    if not m:
        print(f"FAIL: default_palette[] not found in {VIDEO_C}")
        return None
    return [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", m.group(1))]


def rtl_palette():
    out = []
    for lineno, line in enumerate(MEM.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            out.append(int(line, 16))
        except ValueError:
            print(f"FAIL: {MEM.name}:{lineno} is not a hex word: {line!r}")
            return None
    return out


def main():
    if not MEM.exists():
        print(f"skip: {MEM.name} not present (run tests/fetch_vera_rtl.py)")
        return 0

    emu = emulator_palette()
    rtl = rtl_palette()
    if emu is None or rtl is None:
        return 1

    ok = True

    # Counted separately from the comparison. Two tables of different lengths
    # would otherwise compare equal over the shorter one and say nothing about
    # the rest.
    for name, got in (("video.c", len(emu)), (MEM.name, len(rtl))):
        if got != ENTRIES:
            print(f"FAIL: {name} holds {got} entries, expected {ENTRIES}")
            ok = False
    if not ok:
        return 1

    mismatches = 0
    for i, (a, b) in enumerate(zip(emu, rtl)):
        if a != b:
            mismatches += 1
            if mismatches <= 8:
                print(f"FAIL: entry {i} ($1FA{i * 2:02X}): video.c 0x{a:03x}, RTL 0x{b:03x}")
    if mismatches > 8:
        print(f"      and {mismatches - 8} more")

    if mismatches:
        print(f"FAIL: {mismatches} of {ENTRIES} palette entries differ")
        return 1

    print(f"ok  : all {ENTRIES} default palette entries match palette_ram.mem")
    return 0


if __name__ == "__main__":
    sys.exit(main())
