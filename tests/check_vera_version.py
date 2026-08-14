#!/usr/bin/env python3
"""Check that the VERA tests cite the RTL the emulator claims to implement.

The VERA conformance tests quote Verilog by file and line. Those citations are
only meaningful against one revision, and the revision that matters is the one
the emulator reports itself to be: guest software reads VERA_VERSION_MAJOR,
MINOR and PATCH back from registers 0x00-0x02 and adapts to them. Whatever
firmware any particular physical board carries, an emulator claiming 47.0.2 has
to behave like 47.0.2.

So this fails when src/video.c and tests/fetch_vera_rtl.py stop agreeing.
Bumping the emulator's version is legitimate, but it invalidates every cited
line number, and that has to be a deliberate step rather than a silent one.

It does not need the RTL to be present -- it compares two declarations, not the
Verilog itself.

    python tests/check_vera_version.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VIDEO_C = ROOT / "src" / "video.c"
FETCH = ROOT / "tests" / "fetch_vera_rtl.py"

# The vera-module tag whose RTL the VERA tests cite, and the emulator version it
# corresponds to. Both halves are stated so a mismatch names which side moved.
EXPECTED_VERSION = (47, 0, 2)
EXPECTED_TAG = "v47.0.2"
EXPECTED_COMMIT = "45cc1f053376dae12173ea63612820e4d289c0da"


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

    if version != EXPECTED_VERSION:
        got = ".".join(str(n) for n in version)
        want = ".".join(str(n) for n in EXPECTED_VERSION)
        print(f"FAIL: emulator reports VERA {got}, tests are written against {want}")
        print("      The cited RTL line numbers no longer describe what this")
        print("      emulator claims to be. Re-point tests/fetch_vera_rtl.py at")
        print(f"      the matching vera-module tag, re-verify every citation in")
        print("      tests/test_vera_pcm.c, then update EXPECTED_* here.")
        ok = False

    if commit != EXPECTED_COMMIT:
        print(f"FAIL: fetch_vera_rtl.py pins {commit[:8]}, expected {EXPECTED_COMMIT[:8]} ({EXPECTED_TAG})")
        print("      Re-verify the cited line numbers against the new revision")
        print("      before updating EXPECTED_COMMIT.")
        ok = False

    if ok:
        v = ".".join(str(n) for n in version)
        print(f"ok  : emulator reports VERA {v}; oracle pinned to {EXPECTED_TAG}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
