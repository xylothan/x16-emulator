#!/usr/bin/env python3
"""Fetch Klaus Dormann's 6502/65C02 functional test binaries.

They are not committed: they are GPL-licensed and 64K each, and this repository
is BSD. tests/test_klaus.c reports them skipped when they are absent, so this is
optional for a local build and a single step in CI.

    python tests/fetch_klaus.py

The listings that come with them (bin_files/*.lst) map an address to the check
that stopped there, which is how you read a failure. They are large, so they are
not fetched here; look them up in the upstream repository when you need one.
"""

import sys
import urllib.request
from pathlib import Path

DEST = Path(__file__).resolve().parent / "klaus"
BASE = ("https://raw.githubusercontent.com/Klaus2m5/"
        "6502_65C02_functional_tests/master/bin_files")

BINARIES = (
    "6502_functional_test.bin",
    "65C02_extended_opcodes_test.bin",
)

# The listings map an address back to the check that stopped there, which is
# what turns "trapped at $0ECE" into a line of source. Large, but the difference
# between a usable failure and a scavenger hunt.
LISTINGS = (
    "6502_functional_test.lst",
    "65C02_extended_opcodes_test.lst",
)

EXPECTED_SIZE = 65536


def get(name, path, expect_size=None):
    if path.exists() and (expect_size is None or path.stat().st_size == expect_size):
        print(f"  have {name}")
        return True
    print(f"  get  {name}")
    try:
        with urllib.request.urlopen(f"{BASE}/{name}", timeout=180) as r:
            data = r.read()
    except Exception as exc:
        print(f"  FAIL {name}: {exc}")
        return False
    if expect_size is not None and len(data) != expect_size:
        print(f"  FAIL {name}: got {len(data)} bytes, expected {expect_size}")
        return False
    path.write_bytes(data)
    return True


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    ok = True

    for name in BINARIES:
        ok &= get(name, DEST / name, EXPECTED_SIZE)
    for name in LISTINGS:
        ok &= get(name, DEST / name)

    print(f"\ntest images in {DEST}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
