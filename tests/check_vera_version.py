#!/usr/bin/env python3
"""Check that the VERA tests cite the RTL the emulator claims to implement.

The VERA conformance tests quote Verilog by file and line. Those citations are
only meaningful against one revision, and the revision that matters is the one
the emulator reports itself to be: guest software reads VERA_VERSION_MAJOR,
MINOR and PATCH back from registers 0x00-0x02 and adapts to them. Whatever
firmware any particular physical board carries, an emulator claiming a version
has to behave like it.

So this fails when src/video.c or tests/fetch_vera_rtl.py moves without the
other being re-checked. Bumping the emulator's version is legitimate, but it
invalidates every cited line number, and that has to be a deliberate step
rather than a silent one.

THE CLAIM AND THE ORACLE ARE APART, ON PURPOSE
----------------------------------------------
The emulator reports 49.0.6, the version of the R49.0.6 release-candidate
bitstream. There is no public RTL for it: X16Community/vera-module tops out at
v48.0.1. The citations therefore stay pinned to v47.0.2, the revision they were
checked against, and both halves are recorded below so that moving either one
still fails here.

What that costs: 47.0.2 -> 48.0.1 changes no timing (psg.v, already emulated,
and the version defines in top.v), so the cited behaviour holds through 48.0.1.
What 49.x changes is unknown until its RTL is published; when it is, re-pin
fetch_vera_rtl.py to it, re-verify the citations, and set ORACLE_* to match
CLAIMED_VERSION.

It does not need the RTL to be present -- it compares two declarations, not the
Verilog itself.

    python tests/check_vera_version.py

WHAT THIS DOES NOT CHECK, AND WHY IT MATTERS
--------------------------------------------
The version register is a claim, and the rest of the emulator does not honour
it. This is a composite of several revisions rather than any one of them:

    version register    R49.0.6         video.c (no public RTL; see above)
    PSG saw/triangle    R48             XOR with ~pulsewidth, vera_psg.c:122-123,
                                        backported by #290 on 2024-08-11 -- two
                                        months BEFORE the RTL change (a8135f32,
                                        2024-10-21) that shipped in R48
    PCM AUDIO_CTRL      none            bit 6 restart and bits 7+6 loop match
                                        70a03e00, which lived on trunk for five
                                        weeks in 2023 and was reverted as an
                                        accidental merge
    PCM AUDIO_RATE      none            the 256-val fold appears in no revision

The pattern is that this emulator tracks vera-module trunk and proposals rather
than releases, so it can hold behaviour that no released bitstream ever had.

That makes "base the tests on version X" impossible to satisfy by picking a tag:
the emulator would diverge from any single one. Each subsystem records the
revision it is actually checked against instead. For PCM the choice is moot --
audio_fifo.v, pcm.v and audio.v are byte-identical across v47.0.2 and v48.0.1 --
but for PSG it decides the expected waveform, and R48 is what the code
implements.

The tidy end state is to bump this emulator to 48.0.1: PSG already matches it,
PCM is unchanged between the two, and the only obstacles are the two PCM
behaviours above, which match no revision and are bugs independent of version.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VIDEO_C = ROOT / "src" / "video.c"
FETCH = ROOT / "tests" / "fetch_vera_rtl.py"

# The version the emulator claims, and the vera-module tag whose RTL the VERA
# tests cite. They differ on purpose; see THE CLAIM AND THE ORACLE above. Both
# are stated so a mismatch names which side moved.
CLAIMED_VERSION = (49, 0, 6)

# The oracle's own version. Kept for the record and for the day the two agree
# again; the claim, not this, is what video.c is checked against.
#
# One wrinkle, recorded rather than smoothed over: the RTL inside tag v47.0.2
# reports 47.0.0 from its own version registers (top.v:191, 199, 207), not
# 47.0.2 -- the tag name and the registers disagree in the patch field. v48.0.1
# introduced VERSION_MAJOR/MINOR/BUILD defines and is self-consistent. Nothing
# in the audio path turns on this, since audio_fifo.v, pcm.v and audio.v are
# byte-identical across the two tags, but it does mean that RTL reporting
# exactly 47.0.2 does not exist. v47.0.2 is the closest thing to what the
# emulator claims.
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
