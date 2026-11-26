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
- `O N S C` is samples N to N+63 of DAC side S (L or R). Sample N is the one
  the emulator generates Nth; Nuked-OPM's DAC puts it out 5 samples later.
- `F KC C` holds the phase steps of one operator at key code KC, for every KF,
  DT1 and DT2, and MUL 0, 1, 7 and 15. `M PMS KC C` holds them under every PM
  value at sensitivity PMS.
- C is a CRC-32 of the values in order, 4 bytes each, LSB first.
- Output blocks start 8 samples after the last key-on and end 4 samples before
  the next key-on or write that changes the sound. `$18` writes are the
  exception: the LFO vectors check them inside blocks.

```
cc -O2 -I<nuked-opm> -o tb_opm tools/tb_opm.c <nuked-opm>/opm.c
python tools/gen_ym2151.py tb_opm ym2151/nuked-opm --commit <sha>
```

Run with `ym_accuracy ym2151/nuked-opm`.

## VIA

`via/m6522/` holds bus vectors from m6522.vhd (hoglet67/BeebFpga), a 6522
model that passes VICE's VIA tests. Each file names its commit. The model
matches W65C22S datasheet Figures 2-3 and 2-4 for both timers; where the two
disagree, the datasheet wins and the case is left out (see below).

- T counts PHI2 cycles from the first access.
- `W R V T` writes V to register R in cycle T. `R R V T` reads V.
- `I L T` is the IRQ pin as the CPU takes data in cycle T, before that cycle's
  access has any effect.
- The runner steps one cycle at a time, then again in instructions of up to
  seven cycles whose accesses catch the VIA up to their own cycle.

```
ghdl -a --std=93c -fexplicit --ieee=synopsys <m6522.vhd> tools/tb_via.vhd
ghdl -e --std=93c -fexplicit --ieee=synopsys tb_via
python tools/gen_via.py <script running ghdl -r tb_via> via/m6522 --commit <sha>
```

Run with `via_accuracy via/m6522`.

## CPU bus cycles

`cpu_dummy_cycles` counts the accesses a device sees for each instruction form.
Expected counts come from the SingleStepTests bus traces in `tests/pt/` and the
Commander X16 Rev 1.1 schematic, whose I/O decode uses the address alone, so
65C816 internal cycles reach I/O.

`cpu_access_cycles` checks the cycle within the instruction at which each VIA
access lands against the same traces.

## I2C, SMC and RTC

`i2c_targets` drives SDA and SCL a bit at a time, as the KERNAL's `i2c.s`
does, and checks:

- the bus against MCP7940N datasheet DS20005010J Section 4.1.1;
- the RTC against Table 5-1, Registers 5-1 to 5-19 and Sections 5.3, 5.4
  and 6.1;
- the SMC against x16-smc r48.0.0 (0a2684d): `x16-smc.ino` and `smc_wire.cpp`.

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
- YM2151 CSM key-on, the test register ($01) and reads of the address port
- YM2151 output when a register write or key-on lands: Nuked-OPM shows a
  write's effect up to one sample sooner than the emulator, two for TL, and differs
  on the first sample after key-on
- YM2151 envelopes, noise, the noise LFO wave and output past the clamp
- YM2151 left output: Nuked-OPM mixes slots 15 to 31 from one sample earlier
  on the left (slot 31 on the right), so the vectors use the right side
- YM2151 PM and AM depth writes while the LFO's value is being multiplied out
- YM2151 and VERA accesses within an instruction: those devices see each
  access at the instruction's first cycle
- A T1C-H write in the cycle T1 reads 0: m6522.vhd sets no flag for the new
  count, where the datasheet has "reloading the counter will always
  reinitialize the time out period"
- VIA shift register, CA1/CA2/CB1/CB2, port latching and PB6 pulse counting
- When the CPU takes the VIA's IRQ: the vectors check the pin, not the CPU
- CPU dummy cycles at the program counter or stack: implied, stack, branch and JSR
- SMC clock stretching, and the delay from STOP to the SMC's NMI or reset:
  both need measurements of the hardware
- SMC power-off, flash and fuse commands ($0A-$0D, $8F-$93) and keyboard
  command timing: the modelled keyboard acknowledges at once
- RTC: OSCRUN rising 32 oscillator cycles after ST, the MFP pin, the square
  wave, trimming, power-fail time-stamps, and invalid BCD values
