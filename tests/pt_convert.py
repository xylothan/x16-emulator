#!/usr/bin/env python3
"""Convert SingleStepTests/ProcessorTests JSON into a binary fixture.

Avoids a JSON parser in C. Run once; the C runner walks the output with fread().

    python tests/pt_convert.py <json-dir> <out.bin> [--cases N] [--opcodes 0e,7d]

Layout, all little-endian:

    magic 'X16P', u32 version, u32 case count
    per case:
        u8  opcode
        initial: u16 pc, u8 s, a, x, y, p
        u16 ram count, then [u16 addr, u8 value] each
        final:   u16 pc, u8 s, a, x, y, p
        u16 ram count, then [u16 addr, u8 value] each
        u16 cycle count, then [u16 addr, u8 value, u8 is_write] each
"""

import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = b"X16P"
VERSION = 1


def pack_state(st):
    return struct.pack("<HBBBBB", st["pc"] & 0xFFFF, st["s"] & 0xFF,
                       st["a"] & 0xFF, st["x"] & 0xFF, st["y"] & 0xFF,
                       st["p"] & 0xFF)


def pack_ram(ram):
    out = struct.pack("<H", len(ram))
    for addr, val in ram:
        out += struct.pack("<HB", addr & 0xFFFF, val & 0xFF)
    return out


def pack_cycles(cycles):
    out = struct.pack("<H", len(cycles))
    for entry in cycles:
        addr, val, kind = entry[0], entry[1], entry[2]
        # A few cases carry a null value for a cycle with no meaningful data.
        out += struct.pack("<HBB", addr & 0xFFFF, (val or 0) & 0xFF,
                           1 if "write" in kind else 0)
    return out


def pack_case(opcode, c):
    return (
        struct.pack("<B", opcode)
        + pack_state(c["initial"])
        + pack_ram(c["initial"]["ram"])
        + pack_state(c["final"])
        + pack_ram(c["final"]["ram"])
        + pack_cycles(c.get("cycles", []))
    )


def write_fixture(path, entries):
    """Write (opcode, case) pairs to a fixture file. Order is preserved."""
    blobs = [pack_case(op, c) for op, c in entries]
    with open(path, "wb") as fh:
        fh.write(MAGIC + struct.pack("<II", VERSION, len(blobs)))
        for b in blobs:
            fh.write(b)
    return len(blobs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json_dir")
    ap.add_argument("out")
    ap.add_argument("--cases", type=int, default=0,
                    help="cases per opcode, 0 for all")
    ap.add_argument("--opcodes", default="",
                    help="comma-separated hex opcodes, default all present")
    args = ap.parse_args()

    src = Path(args.json_dir)
    wanted = [o.strip().lower() for o in args.opcodes.split(",") if o.strip()]
    files = sorted(src.glob("*.json"))
    if wanted:
        files = [f for f in files if f.stem.lower() in wanted]
    if not files:
        print(f"no .json files in {src}")
        return 1

    entries = []
    for f in files:
        opcode = int(f.stem, 16)
        cases = json.loads(f.read_text())
        if args.cases:
            cases = cases[:args.cases]
        entries.extend((opcode, c) for c in cases)
        print(f"  {f.name}: {len(cases)} cases")

    total = write_fixture(args.out, entries)
    size = Path(args.out).stat().st_size
    print(f"\n{total} cases -> {args.out} ({size/1024/1024:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
