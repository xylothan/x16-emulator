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

EXPECTED_SIZE = 65536


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    failed = False

    for name in BINARIES:
        path = DEST / name
        if path.exists() and path.stat().st_size == EXPECTED_SIZE:
            print(f"  have {name}")
            continue
        print(f"  get  {name}")
        try:
            with urllib.request.urlopen(f"{BASE}/{name}", timeout=120) as r:
                data = r.read()
        except Exception as exc:
            print(f"  FAIL {name}: {exc}")
            failed = True
            continue
        if len(data) != EXPECTED_SIZE:
            print(f"  FAIL {name}: got {len(data)} bytes, expected {EXPECTED_SIZE}")
            failed = True
            continue
        path.write_bytes(data)

    print(f"\ntest images in {DEST}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
