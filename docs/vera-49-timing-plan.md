# Plan: bring VERA emulation up to 49.0.6

## Where things stand

* `src/video.c` reports **49.0.6**, but only because of an uncommitted edit.
  `HEAD` still reports 47.0.2.
* Every RTL citation in the emulator and tests is pinned to **vera-module
  v47.0.2** (`45cc1f05`, `tests/fetch_vera_rtl.py`). That covers the sprite
  budget, the bandwidth model and the VERA conformance tests.
  `tests/check_vera_version.py` therefore **fails** on the working copy:
  `emulator reports VERA 49.0.6, tests are written against 47.0.2`.
* **There is no public 49.x RTL.** `X16Community/vera-module` tops out at
  v48.0.1: its `main` is the v48.0.1 commit `6e8bea68`. No tag, branch, PR or
  known fork has a 49 version define. The only 49.0.6 artifact we have is
  `vera_rc49.0.6.bin`.
* The bitstream header reads:
  `VERC R49.0.6 / Lattice Diamond NG / Part: iCE40UP5K-SG48 / Date: Oct 21 2026 09:20:36`.
  It is the same part as the production VERA. The design name is spelled
  **VERC**, not VERA, and should be checked with whoever built it.

## Finding: 47.0.2 → 48.0.1 changes no timing

The whole diff touches two files:

| Change | File | Emulator impact |
| --- | --- | --- |
| Saw/triangle XOR `~pulsewidth` | `audio/psg.v` | Already emulated (`vera_psg.c:122-123`, backported in #290) |
| `VERSION_MAJOR/MINOR/BUILD` defines | `top.v` | Version register only |
| Write address latched on `posedge bus_write` instead of via a hold register | `top.v` | Sub-CPU-cycle bus latching. The emulator does not model it, and it needs no change |

So nothing between 47.0.2 and 48.0.1 explains a timing difference. Any change
that matters for 49.0.6 lies in RTL that has not been published.

## Emulator timing surfaces

Each diff hunk from 49.0.6 should land on one of these:

| Surface | Emulator | RTL (v47.0.2 names) |
| --- | --- | --- |
| Scan geometry: 800×525 at 25 MHz, NTSC 794 per half line | `video.c:79-88` | `video/video_vga.v:27-37`, `video_composite.v` |
| Scan advance and line render trigger | `video_step`, `video.c:1977-2050` | `graphics/composer.v` (`line_render_start`) |
| VSYNC, line IRQ and collision latch | `update_isr_and_coll` at `video.c:1960`; IRQLINE writes at `video.c:3522-3529`; readback at `video.c:3288` | `top.v`, `composer.v` |
| Next-IRQ prediction (debugger) | `video_next_irq`, `video.c:2703` | derived from the two rows above |
| Sprite render budget | `SPRITE_RENDER_TIME 798` at `video.c:77`, loop at `video.c:1123-1296` | `graphics/sprite_renderer.v:44`, `:171`, `:349` |
| VRAM bus model: 800 clocks per line, 2 clocks per access, priority if0 > L0 > L1 > sprites | `vera_bandwidth.h`, `vera_bandwidth.c` | `vram_if.v:142-157`, `layer_renderer.v:250-271`, `main_ram.v`, `top.v:680-713` |
| Layer fetch counts | `vera_bandwidth.c` (`pixels_per_word`, `words_per_tile_line`) | `layer_renderer.v:42-77`, `:488-501` |
| Mid-line register effects | `video_step(MHZ, 0, true)` at `video.c:3432`, `3536`, `3668`, `3680` | `composer.v` (when a register write takes effect within a line) |
| Data port and FX | applied instantly | `addr_data.v` |
| Oracle and version check | `tests/fetch_vera_rtl.py`, `tests/check_vera_version.py`, citations in `tests/test_vera_*.c` | — |

## Plan

### Phase 0: get the source (blocking for everything 49-specific)

Ask whoever supplied `vera_rc49.0.6.bin` for the RTL commit, or at least the
change list. It was built on 2026-10-21, so it is very likely on someone's
unpushed branch. One diff of the RTL turns everything below into
straightforward work. Without it, Phase 4 is the only route.

### Phase 1: re-pin the oracle to v48.0.1 (no behaviour change, can ship now)

`check_vera_version.py` already names this step as the next one.

1. Point `COMMIT` in `fetch_vera_rtl.py` at `6e8bea68` and drop the separate
   `COMMIT_PSG` pin.
2. Re-verify every cited line number. In `top.v`, lines move +3 after the
   new defines and back by 2 after the write-latch block. So `top.v:191`
   becomes `:194`, and `top.v:680-713` becomes `:681-714`.
3. Update `EXPECTED_*` in `check_vera_version.py`. The docstring can drop the
   "RTL reports 47.0.0" caveat, because v48.0.1 is self-consistent.
4. Keep the PCM caveats (the AUDIO_CTRL restart/loop behaviour, and the
   AUDIO_RATE fold). They match no revision.

### Phase 2: diff 48.0.1 → 49.0.6 and classify (once the source exists)

1. Add the 49.0.6 commit as a pin and diff every file in
   `fetch_vera_rtl.py`'s list, plus `video_composite.v` and `video_modulator.v`.
2. Sort each hunk into one of three buckets:
   * **timing**: it lands on a row of the table above;
   * **behaviour**: registers, FX or rendering;
   * **invisible**: below what the emulator resolves, like the 48.0.1 bus
     latch.
3. For each timing hunk, write down the emulator value before and after. The
   usual suspects are H_TOTAL and V_TOTAL, the `render_time_r` limit, the
   access cost (FETCH and WAIT_FETCH), the arbiter order, the
   `line_render_start` offset, and the IRQ assertion point.

### Phase 3: implement, smallest blast radius first

1. **Constants first:** `SPRITE_RENDER_TIME`, the `VERA_BW_*` constants, and
   the scan widths. Each is one number, with a citation to update.
2. **Structure second:** arbiter order, prefetch offset, IRQ timing. These
   touch `video_step` and the sprite loop.
3. **Tests with every change:** `test_vera_bandwidth.c`, `test_sprite_trace.c`,
   `test_vera_irq.c` and `test_vera_video.c` hold the current numbers, so their
   expectations move with the RTL citation.
4. **Version bump last:** move `check_vera_version.py` to (49, 0, 6) in the
   same PR as the timing work. Then the version claim and the behaviour land
   together. The `video.c` bump should **not** be committed on its own.
5. Push and let CI judge. Per `AGENTS.md`, 11 VERA tests never compile on
   MSVC, so a green Windows build says nothing about them.

### Phase 4 (fallback, no source): measure the bitstream as a black box

This is only worth doing if Phase 0 fails. It is a project in its own right,
not a step in this plan.

* **Tools:** OSS CAD Suite (yosys, Project IceStorm, iverilog, verilator).
  `iceunpack` turns the `.bin` into `.asc`. `icebox_vlog` turns that into
  structural Verilog.
* **Check first:** does `icebox_vlog` emit usable models for the UP5K hard
  cells VERA depends on? Those are SPRAM (all 128 KB of VRAM), the DSP blocks
  (FX multiplier and video modulator) and the PLL/HFOSC. If it does not, the
  netlist will not simulate, and this phase stops here. The bitstream also
  came from Lattice's own toolchain rather than nextpnr. iceunpack should not
  care, but that is unverified.
* **What it can measure:** drive `clk25` and the 6502 bus pins, mapping them
  with v48.0.1's `.pcf`. Then read the pins for the HSYNC period, lines per
  VSYNC, the IRQ assertion line, and register readback. Comparing sprite
  output under load against the 798-clock cutoff gives the sprite budget
  indirectly.
* **What it cannot do:** recover names or intent, or localize a change by
  diffing against a 48.0.1 bitstream (place-and-route reshuffles everything).
  Bus-arbitration detail is visible only through its effect on output pixels.
* **Cheap regardless:** BRAM initial contents, such as the default palette,
  can be pulled from the `.asc` and checked with
  `tests/check_vera_palette.py`.
