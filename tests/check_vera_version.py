#!/usr/bin/env python3
"""Check that the VERA tests cite the RTL the emulator claims to implement.

Fails when src/video.c or tests/fetch_vera_rtl.py moves without the other.
Compares the two declarations; the RTL need not be present.

    python tests/check_vera_version.py

Claim and oracle differ:

    claimed    49.0.6     video.c; no public RTL
    oracle     v48.0.1    VERA test citations
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VIDEO_C = ROOT / "src" / "video.c"
FETCH = ROOT / "tests" / "fetch_vera_rtl.py"

# The version the emulator claims, and the tag the VERA tests cite.
CLAIMED_VERSION = (49, 0, 6)

# Oracle version. The RTL at this tag reports 48.0.1 (top.v:194, 202, 210).
EXPECTED_VERSION = (48, 0, 1)
EXPECTED_TAG = "v48.0.1"
EXPECTED_COMMIT = "6e8bea68a5a04687149e27b1b7b3726fb01405f4"


def emulator_version():
    src = VIDEO_C.read_text(encoding="utf-8", errors="replace")
    out = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"^#define\s+VERA_VERSION_{part}\s+(\d+)", src, re.M)
        if not m:
            print(f"FAIL: VERA_VERSION_{part} not found in {VIDEO_C}")
            return None
        out.append(int(m.group(1)))
    return tuple(out)


def pinned_commit():
    src = FETCH.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'^COMMIT\s*=\s*"([0-9a-f]{40})"', src, re.M)
    if not m:
        print(f"FAIL: no COMMIT pin found in {FETCH}")
        return None
    return m.group(1)


def main():
    version = emulator_version()
    commit = pinned_commit()
    if version is None or commit is None:
        return 1

    ok = True

    if version != CLAIMED_VERSION:
        got = ".".join(str(n) for n in version)
        want = ".".join(str(n) for n in CLAIMED_VERSION)
        print(f"FAIL: emulator reports VERA {got}, this check records {want}")
        print("      A version bump is a deliberate step: if there is RTL for")
        print("      the new version, re-point tests/fetch_vera_rtl.py at it and")
        print("      re-verify every citation; either way, update")
        print("      CLAIMED_VERSION here and say why in the docstring.")
        ok = False

    if commit != EXPECTED_COMMIT:
        print(f"FAIL: fetch_vera_rtl.py pins {commit[:8]}, expected {EXPECTED_COMMIT[:8]} ({EXPECTED_TAG})")
        print("      Re-verify the cited line numbers against the new revision")
        print("      before updating EXPECTED_COMMIT.")
        ok = False

    if ok:
        v = ".".join(str(n) for n in version)
        note = "" if version == EXPECTED_VERSION else " (claim and oracle differ; see docstring)"
        print(f"ok  : emulator reports VERA {v}; oracle pinned to {EXPECTED_TAG}{note}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
