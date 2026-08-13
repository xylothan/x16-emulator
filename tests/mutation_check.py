#!/usr/bin/env python3
"""Check that the tests actually fail when the emulator is wrong.

Every test here is written to pass against correct behaviour, which says
nothing about whether it would notice incorrect behaviour. A test that asserts
the wrong thing, or reads a value it never actually exercises, passes exactly
as convincingly as one that works.

So: break the emulator on purpose, one small change at a time, and check the
test that should care goes red. A mutation that survives means either the test
is not covering what it claims, or the code it changed does not matter.

    python tests/mutation_check.py --build-dir build

Every mutation is reverted afterwards, including on failure or interrupt. The
source is restored from a copy taken before each edit, so an aborted run leaves
the tree as it found it.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CMAKE = os.environ.get("CMAKE", "cmake")
CTEST = os.environ.get("CTEST", "ctest")

# Each entry breaks one specific behaviour and names the tests that should
# notice. `count` is how many places the pattern is expected to match, and a
# mismatch is reported rather than applied -- a pattern that has quietly stopped
# matching would otherwise look exactly like a mutation the tests caught.
MUTATIONS = [
    {
        "name": "direct-page cycle penalty is never cleared",
        "file": "src/cpu/fake6502.c",
        # step6502()'s copy, four-space indented. exec6502()'s is eight-space
        # and is dead code -- mutating that proves nothing, which is exactly
        # what an earlier version of this list accidentally did.
        "find": "\n    penaltyd = 0;\n",
        "into": "\n",
        "count": 1,
        "caught_by": ["cpu_smoke"],
    },
    {
        "name": "page-cross penalty is dropped",
        "file": "src/cpu/fake6502.c",
        "find": "if (penaltyop && penaltyaddr) clockticks6502++;",
        "into": "if (0) clockticks6502++;",
        "count": 2,  # exec6502() and step6502() both have it
        # Not klaus: its functional tests check results, not cycle counts, so
        # they are blind to timing. Worth knowing.
        "caught_by": ["cpu_loadstore"],
    },
    {
        "name": "decimal mode costs no extra cycle",
        "file": "src/cpu/instructions.h",
        "find": "clockticks6502 += (uint32_t)(!regs.is65c816);",
        "into": "clockticks6502 += (uint32_t)(0);",
        "count": 2,  # adc() and sbc()
        "caught_by": ["cpu_decimal"],
    },
    {
        "name": "VIA is not told a read is for the debugger",
        "file": "src/memory.c",
        "find": "return via1_read(address & 0xf, debugOn);",
        "into": "return via1_read(address & 0xf, false);",
        "count": 1,
        "caught_by": ["debugon_contract"],
    },
    {
        "name": "a debug read of slow I/O costs cycles",
        "file": "src/memory.c",
        "find": "if (!debugOn && address >= 0x9fa0) {",
        "into": "if (address >= 0x9fa0) {",
        "count": 1,
        "caught_by": ["debugon_contract"],
    },
    {
        "name": "a watchpoint swallows the store",
        "file": "src/memory.c",
        "find": "\t\tDEBUGBreakOnWatchpoint();\n\t}",
        "into": "\t\tDEBUGBreakOnWatchpoint();\n\t\treturn;\n\t}",
        "count": 1,
        "caught_by": ["watchpoint_purity"],
    },
    {
        "name": "the overflow flag is never set",
        "file": "src/cpu/support.h",
        # Masking with 0x00 rather than replacing the condition with 0: the
        # macro parameters stay referenced, so this compiles under -Werror
        # where an `if (0)` leaves them unused and GCC refuses.
        "find": "if (((n) ^ (m)) & ((n) ^ (o)) & 0x80) setoverflow();",
        "into": "if (((n) ^ (m)) & ((n) ^ (o)) & 0x00) setoverflow();",
        "count": 1,
        "caught_by": ["cpu_adc_sbc", "klaus"],
    },
    {
        "name": "zero page indexing does not wrap inside page zero",
        "file": "src/cpu/support.h",
        "find": "((uint16_t) ((uint8_t) (regs.dp & 0x00FF)) + (offset & 0xFF))",
        "into": "((uint16_t) ((uint8_t) (regs.dp & 0x00FF)) + offset)",
        "count": 1,
        "caught_by": ["cpu_loadstore", "klaus"],
    },
]


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def build(build_dir):
    r = run([CMAKE, "--build", build_dir, "--target", "unit_tests"])
    return r.returncode == 0, r.stdout + r.stderr


def tests_fail(build_dir, names):
    """True if every named test reports failure."""
    for name in names:
        r = run([CTEST, "--test-dir", build_dir, "-R", f"^{name}$"])
        if r.returncode == 0:
            return False, name
    return True, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--only", help="run just the mutation whose name contains this")
    args = ap.parse_args()

    ok, out = build(args.build_dir)
    if not ok:
        print("the tree does not build before any mutation:\n" + out[-2000:])
        return 2

    survived = []
    for m in MUTATIONS:
        if args.only and args.only not in m["name"]:
            continue

        path = ROOT / m["file"]
        # Universal newlines, so a pattern written with \n matches a file
        # checked out with CRLF. The original bytes are restored afterwards.
        original = path.read_text(encoding="utf-8")

        want = m.get("count", 1)
        count = original.count(m["find"])
        if count != want:
            print(f"SKIP  {m['name']}\n      pattern matches {count} times in "
                  f"{m['file']}, expected {want} -- the source has moved on")
            survived.append(m["name"] + " (pattern stale)")
            continue

        backup = Path(tempfile.gettempdir()) / (path.name + ".mutation_backup")
        shutil.copy2(path, backup)
        try:
            path.write_text(original.replace(m["find"], m["into"]),
                            encoding="utf-8")
            built, out = build(args.build_dir)
            if not built:
                # A mutation that will not compile proves nothing either way,
                # and is usually a mutation that needs rewriting rather than a
                # problem with the tests. Show enough to fix it.
                first = next((l for l in out.splitlines()
                              if "error" in l.lower()), "")
                print(f"SKIP  {m['name']}\n      does not compile: {first.strip()[:120]}")
                survived.append(m["name"] + " (did not compile)")
                continue

            caught, missed = tests_fail(args.build_dir, m["caught_by"])
            if caught:
                print(f"caught  {m['name']}")
            else:
                print(f"SURVIVED  {m['name']}\n"
                      f"          {missed} still passed with this broken")
                survived.append(m["name"])
        finally:
            shutil.copy2(backup, path)
            backup.unlink(missing_ok=True)

    # Leave the tree built from clean source, so a later run is not confused by
    # objects compiled from a mutation.
    build(args.build_dir)

    total = len([m for m in MUTATIONS
                 if not args.only or args.only in m["name"]])
    print(f"\n{total - len(survived)}/{total} mutations caught")
    if survived:
        print("survived:")
        for s in survived:
            print(f"  {s}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
