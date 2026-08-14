#!/usr/bin/env python3
"""Convert SingleStepTests JSON into a binary fixture. Format 2.

Avoids a JSON parser in C. Run once; the C runner walks the output with fread().

One format covers both suites. The 65C02 has no bank registers and no emulation
bit, so those fields carry the values a 65816 in emulation mode would hold, and
the cpu field says which core to reset into.

Layout, all little-endian:

    magic 'X16P', u32 version = 2, u32 case count
    per case:
        u8  opcode
        u8  cpu           0 = 65C02, 1 = 65816
        initial state, then u16 ram count and [u32 addr, u8 value] each
        final state,   then u16 ram count and [u32 addr, u8 value] each
        u16 cycle count, then [u32 addr, u8 value, u8 flags] each

    state: u16 pc, u16 s, u16 a, u16 x, u16 y, u8 p, u8 e, u8 dbr, u8 pbr, u16 d

Cycle flags, from the 65816 suite's eight-character signal string:

    0x01 VDA   0x02 VPA   0x04 VPB   0x08 write
    0x10 E     0x20 M     0x40 X     0x80 MLB

The 65C02 suite reports only read or write, so its cycles carry VDA and VPA set
and the rest from the read/write word alone.
"""

import struct

MAGIC = b"X16P"
VERSION = 2

CPU_65C02 = 0
CPU_65816 = 1

F_VDA, F_VPA, F_VPB, F_WRITE = 0x01, 0x02, 0x04, 0x08
F_E, F_M, F_X, F_MLB = 0x10, 0x20, 0x40, 0x80


def pack_state(st, emulation_defaults):
    e = st.get("e", 1 if emulation_defaults else 0)
    return struct.pack(
        "<HHHHHBBBBH",
        st["pc"] & 0xFFFF,
        st["s"] & 0xFFFF,
        st["a"] & 0xFFFF,
        st["x"] & 0xFFFF,
        st["y"] & 0xFFFF,
        st["p"] & 0xFF,
        e & 0xFF,
        st.get("dbr", 0) & 0xFF,
        st.get("pbr", 0) & 0xFF,
        st.get("d", 0) & 0xFFFF,
    )


def pack_ram(ram):
    out = struct.pack("<H", len(ram))
    for addr, val in ram:
        out += struct.pack("<IB", (addr or 0) & 0xFFFFFF, (val or 0) & 0xFF)
    return out


def pack_cycle_flags(word):
    """Third element of a cycles entry, either 'read'/'write' or eight signals."""
    if word in ("read", "write"):
        flags = F_VDA | F_VPA
        if word == "write":
            flags |= F_WRITE
        return flags
    # 'dp-remx-': VDA, VPA, VPB, R/W, E, M, X, MLB, each its letter or '-'.
    bits = (F_VDA, F_VPA, F_VPB, 0, F_E, F_M, F_X, F_MLB)
    flags = 0
    for i, bit in enumerate(bits):
        if word[i] != "-":
            flags |= bit
    if word[3] == "w":
        flags |= F_WRITE
    return flags


def pack_cycles(cycles):
    out = struct.pack("<H", len(cycles))
    for entry in cycles:
        addr, val, word = entry[0], entry[1], entry[2]
        # Both can be null: an internal cycle drives nothing meaningful onto
        # the bus. VDA and VPA in the flags are what says the address is real,
        # so a placeholder zero here is not mistaken for address zero.
        out += struct.pack("<IBB", (addr or 0) & 0xFFFFFF, (val or 0) & 0xFF,
                           pack_cycle_flags(word))
    return out


def pack_case(opcode, cpu, case):
    emulation_defaults = (cpu == CPU_65C02)
    return (
        struct.pack("<BB", opcode, cpu)
        + pack_state(case["initial"], emulation_defaults)
        + pack_ram(case["initial"]["ram"])
        + pack_state(case["final"], emulation_defaults)
        + pack_ram(case["final"]["ram"])
        + pack_cycles(case.get("cycles", []))
    )


def write_fixture(path, entries):
    """Write (opcode, cpu, case) triples. Order is preserved."""
    blobs = [pack_case(op, cpu, c) for op, cpu, c in entries]
    with open(path, "wb") as fh:
        fh.write(MAGIC + struct.pack("<II", VERSION, len(blobs)))
        for b in blobs:
            fh.write(b)
    return len(blobs)
