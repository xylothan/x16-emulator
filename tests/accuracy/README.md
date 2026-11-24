# Accuracy vectors

Generic accuracy tests. Every expected value comes from a named hardware release
or a published specification, never from the emulator.

## VERA

`vera/<version>/` holds one directory per release.

- `scene` vectors render a VGA frame and store one CRC-32 per line. `mid` lines
  are writes made while the frame is drawn.
- `bus` vectors replay register writes and check every read. After a `v` line,
  writes, reads and IRQ pin samples (`W`, `R`, `I`) carry T, the clock count
  since the VSYNC pin last rose (dot 4 of line 492). A write takes effect 3
  clocks after its strobe rises.
- Each file names its oracle: VERA version and bitstream SHA-256.

Vectors are captured through the package pins of a gate-level netlist decoded
from the released bitstream. The bitstream and netlist are not in this repository.

```
iverilog -g2012 -o scene.vvp tools/tb_scene.v <netlist.v> <cell-models.v>
iverilog -g2012 -o bus.vvp tools/tb_bus.v <netlist.v> <cell-models.v>
python tools/gen_vera_scenes.py scene.vvp vera/<version> --version <version> --sha256 <hash>
python tools/gen_vera_bus.py bus.vvp vera/<version> --version <version> --sha256 <hash>
```

Run with `vera_accuracy vera/<version>`.

`MANIFEST` lists vectors that must match. `KNOWN_GAPS` lists vectors captured from
hardware that the emulator does not match yet. They report `DIVERGE`, and a gap
that starts matching fails the run until it moves to `MANIFEST`.

## CPU bus cycles

`cpu_dummy_cycles` counts the accesses a device sees for each instruction form.
Expected counts come from the SingleStepTests bus traces in `tests/pt/` and the
Commander X16 Rev 1.1 schematic, whose I/O decode uses the address alone, so
65C816 internal cycles reach I/O.

## Not covered

- NTSC, RGB, interlaced and 240p output
- Mid-frame layer, scale, border and palette writes (`mid_*` in `KNOWN_GAPS`).
  Hardware fetches line y+1 during line y with live registers and looks up the
  palette at scan-out, so a write inside a line splits a line.
- Sprite collision IRQ timing
- Sprite fetch limits under VRAM contention (`sprites_many` in `KNOWN_GAPS`)
- Sprite buffer entries 640 and 641 when sprites are armed outside vertical blank
- PSG, PCM and SPI
- FPGA reconfiguration
- CPU dummy cycles at the program counter or stack: implied, stack, branch and JSR
