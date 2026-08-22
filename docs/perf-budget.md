# Performance budgets: telling work apart from waiting

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
not switch off a panel somebody is reading.

## Where this is implemented

| File | Role |
| --- | --- |
| `src/perf_budget.{c,h}` | All the accounting. Reaches for no emulator state — everything arrives as a function argument — so it is unit-tested with no CPU, no memory, no video and no SDL. |
| `src/main.c` | Per-instruction and frame-boundary hooks. |
| `src/memory.c` | The store signal the spin detector needs, and the `$9FBC` register. |
| `src/debug_ui/panels/perf_panel.cpp` | The Performance panel. |
| `src/debug_server.c` | `x16/perfStats`, `x16/perfConfig`, and the `x16/perfBudgetOverrun` event. |
| `tests/test_perf_budget.c` | The classification and statistics, driven directly. |
| `testbench/test_dap.py` | The DAP surface against a real running machine. |

## Not covered here

This module accounts for **CPU cycles**. VERA's own bandwidth is a separate
question and is not measured here:

* **Layer fetches** — layer 0/1 tile and map reads in `render_layer_line_tile()`,
  `render_layer_line_text()` and `render_layer_line_bitmap()`, which vary with
  tile mode, bitmap mode and colour depth.
* **The CPU's data-port traffic** through `$9F23`/`$9F24`, which is the one piece
  of VERA bandwidth a program controls directly.
* **VERA FX** writes.

Sprite render time is the exception: it is modelled, but by the VERA sprite
renderer rather than by anything here, and it is reported in the VERA panel's
**Multiplex → Render time** view rather than in this budget. It is a per-scanline
time ceiling that really does drop sprites, which is a different shape of
question from "how many cycles did my code use", so the two are deliberately
kept apart rather than blended into one number that would mean neither.
