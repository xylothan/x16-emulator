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
  the emulator generates Nth; Nuked-OPM's DAC puts it out 3 samples later.
- `F KC C` holds the phase steps of one operator at key code KC, for every KF,
  DT1 and DT2, and MUL 0, 1, 7 and 15. `M PMS KC C` holds them under every PM
  value at sensitivity PMS. `Z PMS KC C` is the same under a PM depth of 0.
- C is a CRC-32 of the values in order, 4 bytes each, LSB first.
- Output blocks start 8 samples after the last key-on and end 4 samples before
  the next key-on or write that changes the sound. The exceptions are `$18`
  and `$19` writes in the LFO vectors, key-on, key-off and operator writes in
  `envelopes`, `key_timing`, `key_latch` and `op_writes`, and channel writes in
  `channel_writes`, which are checked inside blocks.

```
cc -O2 -I<nuked-opm> -o tb_opm tools/tb_opm.c <nuked-opm>/opm.c
python tools/gen_ym2151.py tb_opm ym2151/nuked-opm --commit <sha>
```

Run with `ym_accuracy ym2151/nuked-opm`.

## VIA

`via/m6522/` holds bus vectors from m6522.vhd (hoglet67/BeebFpga), a 6522
model that passes VICE's VIA tests. Each file names its commit. The model
matches W65C22S datasheet Figures 2-3 and 2-4 for both timers, and the W65C22
datasheet (WDC) Table 2-5 and Figures 4-3 to 4-7 for the control pins, input
latching and handshakes, and 2.11 to 2.13 with Figures 2-6 to 2-12 for the
shift register; where they disagree, the datasheet wins and the case is left
out (see below).

- T counts PHI2 cycles from the first access.
- `W R V T` writes V to register R in cycle T. `R R V T` reads V.
- `I L T` is the IRQ pin as the CPU takes data in cycle T, before that cycle's
  access has any effect.
- `P PIN V T` drives input PIN (`CA1`, `CA2`, `CB1`, `CB2`, `PA`, `PB`) to V
  from cycle T on. A pin's last level in a cycle is the one its clock sees.
- `O CA2 CB1 CB2 PA PAOE PB PBOE T` is the outputs as the CPU takes data in
  cycle T, as `I`: a pin 0, 1 or Z when it does not drive, X when it drives
  but is not checked, a port its output register and the bits that drive.
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

`cpu_access_cycles` checks the cycle within the instruction at which each VIA,
VERA and YM2151 access lands against the same traces; a YM2151 access also
waits three cycles for each slow I/O access up to it, the emulator's own
model of the 2 MHz slow I/O range.

`processor_tests` compares each opcode's whole bus trace, address and
direction per cycle, with the same traces; `tests/pt_baseline_*.txt` record
which opcodes match.

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
- YM2151 test register ($01) bits 1, 6 and 7, the LFO steps bits 2 and 3 add,
  and reads of the address port
- YM2151 test register bit 4 taken at cycle c of 1-6: the chip takes it in
  time for slots c + 25 to 31 of the sample before, the emulator from the
  sample it is written in
- YM2151 test register bit 5 written in a sample's last cycle: the chip takes
  it in time for slot 31 of that sample, the emulator from the next
- YM2151 phases of operators not keyed since reset, heard only under test
  register bit 5: Nuked-OPM's reset leaves them part way on, the emulator at 0
- YM2151 RL ($20-$27 bits 6 and 7) latched at the channel's M1 cycle: the
  chip's C2 output takes it a sample before the emulator's
- YM2151 PMS ($38-$3F bits 4-6) lowered from 5 or 7 under a sounding note: the
  emulator's phase then parts from the chip's (from 1 or 3 it does not)
- A T1C-H write in the cycle T1 reads 0: m6522.vhd sets no flag for the new
  count, where the datasheet has "reloading the counter will always
  reinitialize the time out period"
- VIA PB6 pulse counting
- VIA CB2 from an SR or ACR write until CB1 next falls, hatched as undefined in
  Figures 2-10 to 2-12: m6522.vhd's level there depends on the testbench's
  previous cycle
- VIA IRQ in the cycle IFR2 sets: m6522.vhd reads IFR2 set with IFR7 clear,
  which 2.14's IFR7 equation rules out
- A VIA SR access in the cycle T2L reads FF in modes 001, 100 and 101:
  m6522.vhd's answer depends on the testbench's previous cycle
- VIA SR modes 001, 100 and 101 with T2's latch low byte 0: m6522.vhd never
  clocks, where 2.12.2 has N+2 cycles
- VIA SR mode 000 with CB1 pulsed: m6522.vhd shifts, where 2.12.1 has the SR
  disabled
- A VIA SR access while a count of eight runs: m6522.vhd carries on with the
  count, where 2.12.4 and 2.13.4 have the access start another
- VIA register 15 reads of PA's output bits: m6522.vhd gives the input
  register, the datasheet the same as register 1
- VIA PB7 before T1's first load: m6522.vhd has it low, where the datasheet
  has it go low at the load
- VIA PB7 as T1's output while DDRB bit 7 is 0: m6522.vhd leaves it an input
- A VIA port pin driven both by the VIA and from outside
- When the CPU takes the VIA's IRQ: the vectors check the pin, not the CPU
- Where in a slow I/O access's four cycles the YM2151 takes it: the emulator
  takes it at the end
- 65C02 bus cycles of $5C, and the address of the extra cycle decimal ADC and
  SBC immediate make
- Bus cycles of WAI, STP, MVN and MVP, and of IRQ and NMI entry
- SMC clock stretching, and the delay from STOP to the SMC's NMI or reset:
  both need measurements of the hardware
- SMC power-off, flash and fuse commands ($0A-$0D, $8F-$93) and keyboard
  command timing: the modelled keyboard acknowledges at once
- RTC: OSCRUN rising 32 oscillator cycles after ST, the MFP pin, the square
  wave, trimming, power-fail time-stamps, and invalid BCD values
