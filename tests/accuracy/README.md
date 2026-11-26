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
- `audio` vectors replay register writes and check the I2S output and reads.
  T is the clock count from power-on. Render r reads its registers at
  512 × (r + 1) + 1, and `S r L R` is the 17-bit mix the DAC latched then.
  Accesses avoid the 104 clocks from that point, while the voices are read.
- Each file names its oracle: VERA version and bitstream SHA-256.

Vectors are captured through the package pins of a gate-level netlist decoded
from the released bitstream. The bitstream and netlist are not in this repository.

```
iverilog -g2012 -o scene.vvp tools/tb_scene.v <netlist.v> <cell-models.v>
iverilog -g2012 -o bus.vvp tools/tb_bus.v <netlist.v> <cell-models.v>
iverilog -g2012 -o audio.vvp tools/tb_audio.v <netlist.v> <cell-models.v>
python tools/gen_vera_scenes.py scene.vvp vera/<version> --version <version> --sha256 <hash>
python tools/gen_vera_bus.py bus.vvp vera/<version> --version <version> --sha256 <hash>
python tools/gen_vera_audio.py audio.vvp vera/<version> --version <version> --sha256 <hash>
```

Run with `vera_accuracy vera/<version>`.

`MANIFEST` lists vectors that must match. `KNOWN_GAPS` lists vectors captured from
hardware that the emulator does not match yet. They report `DIVERGE`, and a gap
that starts matching fails the run until it moves to `MANIFEST`.

## YM2151

`ym2151/nuked-opm/` holds bus vectors from Nuked-OPM, a model of the YM2151
derived from its die. Each file names the Nuked-OPM commit. Nuked-OPM is LGPL
and is not in this repository.

- T counts interface cycles since reset: two φM clocks, 32 per sample.
- `W P V T` writes V to port P (0 address, 1 data) before cycle T.
- `R V T` is the status byte after T cycles.
- `G A V M T` is register A as the chip holds it, under mask M. The emulator
  compares the register shadow the debugger shows.
- `X T0 T1` watches status and IRQ over T0 to T1. `C S I T` lines give each
  change. The runner checks every cycle, then replays the file in batches of up
  to 64 samples.

```
cc -O2 -I<nuked-opm> -o tb_opm tools/tb_opm.c <nuked-opm>/opm.c
python tools/gen_ym2151.py tb_opm ym2151/nuked-opm --commit <sha>
```

Run with `ym_accuracy ym2151/nuked-opm`.

## CPU bus cycles

`cpu_dummy_cycles` counts the accesses a device sees for each instruction form.
Expected counts come from the SingleStepTests bus traces in `tests/pt/` and the
Commander X16 Rev 1.1 schematic, whose I/O decode uses the address alone, so
65C816 internal cycles reach I/O.

## Not covered

- NTSC, RGB, interlaced and 240p output
- Sprite fetches delayed by CPU data port accesses
- Sprite buffer entries 640 and 641 when sprites are armed outside vertical blank
- Sprite buffer entries 640 and 641 under the pixel on the last render clock
- Writes while the PSG voices or the PCM FIFO are being read
- PSG voices while the PCM FIFO holds 16 bytes or more. The two RAMs' write
  addresses share upper bits in the decoded netlist.
- The DAC's 48 kHz output rate: 6 frames per 3125 clocks, against one render per 512
- SPI
- FPGA reconfiguration
- YM2151 sound output, CSM key-on, the test register ($01) and reads of the
  address port
- YM2151 accesses within an instruction: devices see each access at the
  instruction's first cycle
- CPU dummy cycles at the program counter or stack: implied, stack, branch and JSR
