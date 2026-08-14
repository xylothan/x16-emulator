#!/usr/bin/env python3
"""Fetch the VERA Verilog that tests/test_vera_pcm.c cites.

VERA is an FPGA, so the RTL is not a description of the hardware -- it IS the
hardware, and it is the oracle for every VERA conformance test here. The tests
quote the lines they rest on so a reader need not fetch anything, but the
quotes are only trustworthy if they can be checked against the source.

    python tests/fetch_vera_rtl.py

Nothing here is committed: it is someone else's repository under its own
licence, and it is reference material rather than part of the build. No test
depends on it being present.

Pinned to a commit rather than tracking a branch. A citation of "top.v:481" is
meaningless against a moving target, and the whole point is that a reviewer can
check the quoted line. When the pin moves, the quoted lines must be re-checked.

Use X16Community/vera-module and not fvdhoef/vera-module. The latter is the
parent repository, but the two diverged at the v0.7 release in 2019 and are now
different chips: the parent has no audio RTL at all -- no audio directory, no
PCM, no FIFO, and only eight bus registers against this one's thirty-two. Audio
was added for production hardware and exists solely in the fork.
"""

import sys
import urllib.request
from pathlib import Path

DEST = Path(__file__).resolve().parent / "vera-rtl"

# VERSION_MAJOR = 8'd48 in top.v -- the "R48" hardware revision.
COMMIT = "6e8bea68a5a04687149e27b1b7b3726fb01405f4"
BASE = f"https://raw.githubusercontent.com/X16Community/vera-module/{COMMIT}"

FILES = (
    "fpga/source/top.v",                    # register decode, incl. 0x1B-0x1D
    "fpga/source/audio/audio_fifo.v",       # the PCM FIFO itself
    "fpga/source/audio/pcm.v",              # rate accumulator, sample fetch, volume
    "fpga/source/audio/audio.v",            # PCM and PSG into the DAC
    "fpga/source/audio/psg.v",              # for the PSG tests
)


def get(url, path):
    if path.exists():
        print(f"  have {path.name}")
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(url) as r:
            data = r.read()
    except Exception as e:
        print(f"  FAILED {path.name}: {e}", file=sys.stderr)
        return False
    path.write_bytes(data)
    print(f"  got  {path.name} ({len(data)} bytes)")
    return True


def main():
    print(f"VERA RTL -> {DEST}")
    print(f"  commit {COMMIT}")
    ok = True
    for f in FILES:
        # Flattened: the citations name the file, not its path in the repo.
        ok &= get(f"{BASE}/{f}", DEST / Path(f).name)
    if not ok:
        return 1
    (DEST / "COMMIT").write_text(COMMIT + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
