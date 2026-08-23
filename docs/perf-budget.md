# Performance budgets: telling work apart from waiting

Two budgets, in one panel and one DAP response: the **CPU's cycles**, and
**VERA's VRAM bandwidth**. They answer the same question — "what is eating my
frame?" — from the two ends that can independently run out, and the frames worth
looking at are the ones where they disagree.

The cycle model comes first; [VERA's bandwidth](#veras-bandwidth) is below.

## The problem

"How much of my frame am I using?" has no direct answer on a 6502, because the
CPU never idles. At 8 MHz a VGA frame is exactly

```
cycles/frame = MHZ * 1e6 * (VGA_SCAN_WIDTH * SCAN_HEIGHT) / PIXEL_FREQ
             = 8e6 * (800 * 525) / 25e6
             = 134,400 cycles          (256 per scanline, over 525 lines)
```

and the machine burns every one of them whether your program is doing work or
spinning on a vsync flag. Raw cycles-per-frame is therefore always 100% and says
nothing at all.

The number you actually want is **work versus waiting**. Producing it means
classifying cycles, and classification is a judgement call — so this document
says exactly what judgement is made, and where it can be wrong.

## The budget

`targetFps` asks **how many vsyncs your frame work is allowed to span**. The
frame period is fixed by VERA and cannot be changed by wanting it to be, so
asking for 30 fps does not make frames longer: it means the work may take two of
them, and the budget becomes two frames' worth of cycles.

| Target | Budget period | Budget at 8 MHz |
| --- | --- | --- |
| 60 fps | 1 vsync | 134,400 cycles |
| 30 fps | 2 vsyncs | 268,800 cycles |
| 20 fps | 3 vsyncs | 403,200 cycles |
| 120 fps | 1 vsync | ~66,600 cycles |

A target *faster* than the machine scans is a different question — "must this
routine fit in half a frame?" — and gets a fractional budget instead. Between
the two, a target within 5% of a whole number of vsyncs snaps onto it: "60" on a
59.52 Hz machine plainly means "every frame", not "0.8% faster than the hardware
can scan", and reporting the latter would put every saturated program
permanently over budget for no reason anyone would accept.

The budget always follows the machine's **native** clock, never
`timing_get_speed_khz()`. The speed control exists so a routine can be watched at
25 kHz; a budget that shrank to match would report every frame as a catastrophic
overrun for a machine that is running perfectly well.

## How cycles are classified

Three layers, highest precedence first. Each exists because the one below it has
a limit you can hit in practice.

### 1. Guest markers (`$9FBC`) — exact, opt-in

The program itself says where its frame work begins and ends. This is the only
layer that involves no guessing at all, and the only one that can be right about
code the other two cannot see.

| Write | Meaning |
| --- | --- |
| `$01`–`$3F` | enter region *n* (nests) |
| `$80 \| n` | leave region *n* |
| `$00` | leave the innermost region |
| `$F0` | frame work begins |
| `$F1` | frame work ends — the rest of the frame is waiting |
| `$FE` | reset all statistics |

Reading `$9FBC` returns a protocol version, so guest code can feature-detect.

Any marker write puts the module in marker mode, which suppresses the heuristic
entirely: a program that says where its work is should not also be guessed at.
Marker mode is cleared by a reset, so a program that used markers once does not
lose automatic detection for the rest of the session.

A region id first seen here claims a zone slot named `region <n>`, so marker
regions and address zones appear in one breakdown rather than two.

Unbalanced writes are tolerated rather than rejected — leaving a region that is
not open is ignored, and a region still open when the frame ends is closed by the
frame boundary. A guest that forgets a leave mis-attributes one frame, not every
frame after it.

### 2. Zones — exact, no guest changes

Address ranges named in the debugger or over DAP. Cycles executed inside a zone
are attributed to it, giving the per-routine breakdown.

A zone flagged **idle** declares "cycles here are waiting, not work". That is the
manual override for a wait loop the heuristic below cannot see.

Zones are looked up through a flat 64K table (`PC -> zone`), rebuilt only when
the zone list changes, so attribution costs one array index per instruction
rather than a scan of range checks. Later zones win where two overlap, which
makes "draw a tighter zone inside a looser one" behave as expected.

### 3. Automatic detection — zero setup, heuristic

Two signals:

* **`WAI`** — unambiguous, and free to detect.
* **A store-free tight loop.** A loop is identified by the *backward jump* that
  closes it. Once the same backward jump has closed twice with **no memory
  stores** and a body spanning no more than 32 bytes, its cycles are credited as
  waiting.

That catches the two idioms which dominate X16 code:

```asm
    lda $9F27       ; poll VERA's ISR for the vsync bit
    and #1
    beq -

    lda vsync_flag  ; poll a flag an IRQ handler sets
    beq -
```

#### Why the backward jump, and not PC drift

An earlier version anchored on the PC drifting outside a window. That meant
whatever address the walk happened to start at became the anchor — so a compact
routine whose whole body fits inside the window was treated as one enormous
"loop" that stores, and the tight, store-free poll nested inside it was never
seen at all. Anchoring on the branch target instead means each loop is recognised
as itself, however tightly the code around it is packed. Pinned by a test.

#### What automatic detection cannot see

**A wait loop that stores, or that calls a subroutine, reads as work.**
`jsr GETIN / cmp #0 / beq -` is the common example. "Performs no stores" is the
only cheap signal that separates a spin from a computation, and a loop that
writes memory has done something.

This is not a bug to be fixed by a cleverer heuristic — it is precisely why
layers 1 and 2 exist. Mark such a range as an idle zone, or bracket the real work
with markers, and it is counted correctly.

The detector also re-anchors whenever an interrupt is taken, so a wait loop
interrupted by its own vsync handler loses up to two passes before it is
confirmed again. Two passes of a four-instruction loop is on the order of twenty
cycles against a budget of 134,400, so no state is carried to avoid it.

## Interrupts

Cycles taken with an interrupt handler open are attributed to `irq` and are never
idle. The split between handler and main-line work usually answers "what is
eating my frame?" on its own, and needs no configuration: `cpu_irq_depth()`
already tracks it.

## Overruns, and why a saturated program is not automatically one

A frame boundary is detected *after* the instruction that crossed it, so a
frame's accounted cycles can overshoot the nominal period by up to one
instruction. Without a tolerance, a program that simply uses the whole machine
reads as "over budget by 3 cycles" on the frames where the boundary happens to
land late — measurement granularity reported as a defect. The overrun test
therefore allows one maximum-length instruction (8 cycles) per frame in the
period.

Note that with a whole-frame budget, work can never exceed the cycles that
actually elapsed, so utilisation approaches but does not meaningfully pass 100%.
Sub-frame budgets (a target faster than the machine scans) are where an overrun
figure carries real information: there, work genuinely can exceed what you
allowed it.

## Cost

Off: one load and one predictable branch per instruction, from an inline guard.
Nothing is allocated until profiling is switched on.

On: an add, one array index for the zone lookup, and a handful of branches — the
same order as the `code_map_record_current()` call the debugger already makes on
every instruction. The history ring is 3,600 frames (sixty seconds at 60 Hz) at
about 56 bytes each.

Percentiles are computed on demand by copying the window and sorting it, so a
window nobody asks for costs nothing.

Profiling runs while **any** owner wants it — the ImGui panel or a DAP client —
mirroring the ownership rule the core already applies to breakpoints. Closing the
panel does not strand a client with a dead feed, and a client disconnecting does
not switch off a panel somebody is reading. The bandwidth accounting below
follows the same switch and the same ownership rule, so "profile this machine" is
one decision rather than two.

The bandwidth side costs a few arithmetic ops per scanline rather than per
instruction — 480 lines against 134,400 cycles — and its buffers are two fixed
6 KB arrays, so there is nothing to allocate and nothing that can fail to.

## VERA's bandwidth

Everything above accounts for **CPU cycles**. VERA has its own budget, and a
program can have cycles to spare and still tear, drop sprites or run out of room
to push pixels — so `src/vera_bandwidth.c` measures the VRAM bus alongside.

### What the bus is

Quoted from the RTL, which for an FPGA is not a description of the hardware but
*is* the hardware. Pinned to `X16Community/vera-module` tag **v47.0.2**, the same
commit `tests/fetch_vera_rtl.py` fetches, so every line below can be checked.

| Fact | Source |
| --- | --- |
| A scanline is 800 clocks (640 + 16 + 96 + 48), 525 lines a frame | `video_vga.v:27-37` |
| VRAM is 32 bits wide, so **an access is four bytes** | `main_ram.v:7-10` |
| An access costs **two clocks**: strobe, then ack | `layer_renderer.v:250-271` |
| One bus, **fixed priority: CPU > layer 0 > layer 1 > sprites** | `vram_if.v:142-157`, wired at `top.v:680-713` |
| A tile map word holds **two** 16-bit entries, so a map fetch every two tiles | `layer_renderer.v:113`, `:305` |
| The layer renderer always fills **640 pixels**, then stops | `layer_renderer.v:488` |

### Why the headline is a scanline

VERA's peak is 25 MHz × 4 bytes = 100 MB/s, and a real program uses perhaps a
fifth of it. "Percent of total bandwidth" would therefore sit near 20% forever
and say nothing — the same trap as raw cycles-per-frame always being 100%.

The **800 clocks of one scanline** is the window that genuinely runs out, and at
full load it exactly does:

```
layer 0, 8bpp bitmap    160 fetches x 2 = 320 clocks
layer 1, 8bpp bitmap    160 fetches x 2 = 320 clocks
sprites, 4bpp, full line                = 160 clocks
                                          ----
                                           800   = one scanline
```

So the panel reports peak, p95 and mean **clocks per scanline**, and names the
line the peak fell on.

### Fetches per scanline

| Tile mode | 8px tiles | 16px tiles |
| --- | --- | --- |
| 1 bpp | 120 | 60 |
| 2 bpp | 120 | 60 |
| 4 bpp | 120 | 100 |
| 8 bpp | 200 | 180 |

| Bitmap mode | 1 bpp | 2 bpp | 4 bpp | 8 bpp |
| --- | --- | --- | --- | --- |
| Fetches | 20 | 40 | 80 | 160 |

Two things in that table surprise people:

* **A 320-wide bitmap costs the same as a 640-wide one.** `layer_renderer` has
  no scale input at all; `DC_HSCALE` is applied by the composer when it *reads*
  the line buffer. The renderer just fills to 640 pixels and runs off the end of
  its own line. Halving the width buys VRAM, not bandwidth.
* **Scrolling is not free.** The renderer starts at `0 - subtile_hscroll`
  (`layer_renderer.v:499-501`), so a layer scrolled off a tile boundary draws one
  tile more than the screen needs — three extra fetches at 8 bpp.

### The data port, and VERA FX

`$9F23`/`$9F24` is the one piece of VERA bandwidth a program controls directly,
so it is reported on its own: bytes read, bytes written, and what reached VRAM.
An 8 MHz CPU can issue roughly 64 accesses a scanline — a 4-cycle `sta` is 12.5
VERA clocks — which is about **33 KB a frame** and a ceiling no program can pass.

FX is where those two numbers come apart. A cache write is one store that moves
**four** bytes, and an affine read costs a second fetch to translate the map. The
panel reports the ratio, because "bytes into VRAM per byte the CPU pushed" is the
whole reason to use FX.

The debugger's own reads and writes are never charged. `writeMemory` with a
`vram:` reference reaches VRAM through `video_space_write()`, around the port
entirely; a write to the CPU address `$9F23` raises `video_set_debug_write()` so
the accounting declines it; and the read path is already guarded by `debugOn`.
If opening a memory view moved the number, the number would be worthless.

### Where the model is wrong

Stated plainly, because a bandwidth figure nobody can calibrate is worse than
none.

* **Fetches are modelled from the layer registers, not observed.** The
  emulator's renderer is a *pixel* loop — 640 columns reading a byte at a time,
  re-reading the same byte in 320-wide modes — and never fetches 32 bits at once
  the way the hardware does. Counting those calls would measure the emulator's
  inner loop rather than VERA's bus. The cost is that a bug in the emulator's
  renderer will not show up here.
* **Clocks are a lower bound; accesses are exact.** Two clocks per access is the
  uncontended cost. Under contention a lower-priority client holds its strobe and
  waits, burning more. Trust the access counts; read the clock totals as "at
  least this bad".
* **The scroll tile is an assumption.** Which tile is "every other" for the map
  fetch depends on the scrolled tile counter's parity, so the count can be one
  out at the margin.
* **A line is fetched during the line before it.** The layer line buffer is
  double-buffered (`top.v` `active_line_buf_r`, `composer.v`). Fetches are
  reported against the line they *paint*, which is where a developer looks.
* **FX is covered where the emulator implements it** — the four-byte cache write
  and the affine prefetch — not as a whole model of `addr_data.v`.

### Sprites, and the contention this makes visible

Sprite render time is modelled separately and reported in the VERA panel's
**Multiplex → Render time** view. The Bandwidth section links to it rather than
drawing it twice.

The two now meet at a real number. `SPRITE_TRACE_LINE_BUDGET` (798) is
**wall clock**, not bus accesses: `render_time_r` increments every clock
(`sprite_renderer.v:43-56`), including the `STATE_RENDER` clocks when the
renderer is painting and holding no strobe at all. So it is the envelope sprite
work must finish inside, not a share of bandwidth.

Sprites are also *last* on the bus. What layers and the CPU leave is what they
get, so the per-line tooltip shows the headroom left after both — and says so
when a line's sprite demand exceeds it. That is the known gap made expressible:
the sprite model charges `STATE_WAIT_FETCH` one clock, the uncontended best case,
so on a line the layers have filled it under-charges. The panel can now show
*which* lines those are; making the sprite model itself contention-aware would
mean simulating the arbiter cycle by cycle, and is not done.

## Where this is implemented

| File | Role |
| --- | --- |
| `src/perf_budget.{c,h}` | All the cycle accounting. Reaches for no emulator state — everything arrives as a function argument — so it is unit-tested with no CPU, no memory, no video and no SDL. |
| `src/vera_bandwidth.{c,h}` | The VRAM bus model, dependency-free for the same reason. |
| `src/main.c` | Per-instruction and frame-boundary hooks. |
| `src/memory.c` | The store signal the spin detector needs, and the `$9FBC` register. |
| `src/video.c` | Per-scanline layer modes, data-port traffic, and the VERA frame boundary. |
| `src/debug_ui/panels/perf_panel.cpp` | The Performance panel, CPU and bandwidth in one place. |
| `src/debug_server.c` | `x16/perfStats`, `x16/perfConfig`, and the `x16/perfBudgetOverrun` event. |
| `tests/test_perf_budget.c` | The classification and statistics, driven directly. |
| `tests/test_vera_bandwidth.c` | The fetch model, checked against the RTL's decode tables. |
| `testbench/test_dap.py` | Both DAP surfaces against a real running machine. |

