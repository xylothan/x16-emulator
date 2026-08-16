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

Pinned to the tag the emulator reports itself to be, not to a branch. A citation of "top.v:480" is
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
RAW = "https://raw.githubusercontent.com/X16Community/vera-module"

# The tag matching VERA_VERSION_MAJOR/MINOR/PATCH in src/video.c. Held in step
# by tests/check_vera_version.py: if the emulator's declared version changes,
# this pin and every cited line number must be re-checked.
COMMIT = "45cc1f053376dae12173ea63612820e4d289c0da"  # tag v47.0.2

# psg.v is pinned to R48 instead, because that is the revision the emulator
# implements. vera_psg.c:122-123 XOR the saw and triangle waveforms with the
# inverted pulse width, which arrived in the RTL as a8135f32 (2024-10-21) and
# shipped in v48.0.1; R47 has the unmodulated forms. The emulator backported it
# in #290 on 2024-08-11, two months before the RTL change landed.
#
# psg.v is the only audio file that differs between the two tags -- audio_fifo.v,
# pcm.v and audio.v are byte-identical -- so this is the whole of the split.
# See the header of tests/check_vera_version.py for why the emulator is not any
# single revision.
COMMIT_PSG = "6e8bea68a5a04687149e27b1b7b3726fb01405f4"  # tag v48.0.1

FILES = (
    ("fpga/source/top.v", COMMIT),                  # register decode, incl. 0x1B-0x1D
    ("fpga/source/graphics/layer_renderer.v", COMMIT),   # map and tile address arithmetic
    ("fpga/source/graphics/sprite_renderer.v", COMMIT),  # sprite collision mask and sprcol_irq
    ("fpga/source/audio/audio_fifo.v", COMMIT),     # the PCM FIFO itself
    ("fpga/source/audio/pcm.v", COMMIT),            # rate accumulator, sample fetch, volume
    ("fpga/source/audio/audio.v", COMMIT),          # PCM and PSG into the DAC
    ("fpga/source/audio/psg.v", COMMIT_PSG),        # R48: see above
)


def manifest():
    return "".join(f"{Path(p).name} {ref}\n" for p, ref in FILES)


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
    for p, ref in FILES:
        print(f"  {Path(p).name:16} {ref[:8]}")

    # Re-fetch when any pin has moved. Skipping on "file exists" alone would
    # quietly serve the previous revision's RTL under the new commit's name,
    # and the line numbers cited in the tests would silently stop matching --
    # which is exactly the failure this whole oracle exists to prevent.
    stamp = DEST / "COMMIT"
    want = manifest()
    have = stamp.read_text() if stamp.exists() else None
    if have is not None and have != want:
        print("  pins moved: discarding the fetched copy")
        for p, _ in FILES:
            (DEST / Path(p).name).unlink(missing_ok=True)
        stamp.unlink(missing_ok=True)

    ok = True
    for p, ref in FILES:
        # Flattened: the citations name the file, not its path in the repo.
        ok &= get(f"{RAW}/{ref}/{p}", DEST / Path(p).name)
    if not ok:
        return 1
    stamp.write_text(want)
    return 0


if __name__ == "__main__":
    sys.exit(main())
