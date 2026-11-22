#!/usr/bin/env python3
"""Fetch the VERA Verilog that tests/test_vera_pcm.c cites.

    python tests/fetch_vera_rtl.py

Not committed; no test depends on it. Source is X16Community/vera-module, not
fvdhoef/vera-module, which has no audio RTL. Re-check quoted lines when a pin
moves.
"""

import sys
import urllib.request
from pathlib import Path

DEST = Path(__file__).resolve().parent / "vera-rtl"
RAW = "https://raw.githubusercontent.com/X16Community/vera-module"

# Tag the VERA citations are checked against. See tests/check_vera_version.py.
COMMIT = "45cc1f053376dae12173ea63612820e4d289c0da"  # tag v47.0.2

# psg.v only: vera_psg.c:122-123 implements the v48.0.1 waveforms.
COMMIT_PSG = "6e8bea68a5a04687149e27b1b7b3726fb01405f4"  # tag v48.0.1

FILES = (
    ("fpga/source/top.v", COMMIT),                  # register decode, incl. 0x1B-0x1D
    ("fpga/source/graphics/layer_renderer.v", COMMIT),   # map and tile address arithmetic
    ("fpga/source/graphics/sprite_renderer.v", COMMIT),  # sprite collision mask and sprcol_irq
    ("fpga/source/palette_ram.mem", COMMIT),        # the default palette, baked into the bitstream
    ("fpga/source/audio/audio_fifo.v", COMMIT),     # the PCM FIFO itself
    ("fpga/source/audio/pcm.v", COMMIT),            # rate accumulator, sample fetch, volume
    ("fpga/source/audio/audio.v", COMMIT),          # PCM and PSG into the DAC
    ("fpga/source/audio/psg.v", COMMIT_PSG),        # R48: see above
    # VRAM bandwidth model, src/vera_bandwidth.c:
    ("fpga/source/vram_if.v", COMMIT),              # the one VRAM bus: 4-way fixed priority, one access per clock
    ("fpga/source/main_ram.v", COMMIT),             # 32k x 32 BRAM array, which is why an access is 4 bytes
    ("fpga/source/video/video_vga.v", COMMIT),      # H_TOTAL 800, V_TOTAL 525: the scanline clock window
    ("fpga/source/graphics/composer.v", COMMIT),    # line_render_start, so a line is fetched during the one before it
    ("fpga/source/graphics/layer_line_buffer.v", COMMIT),  # the double buffer that makes that prefetch possible
    ("fpga/source/addr_data.v", COMMIT),            # the $9F23/$9F24 data port and the FX engine behind it
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

    # Re-fetch when any pin has moved.
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
