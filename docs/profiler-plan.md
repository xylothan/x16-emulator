# Plan: a capture profiler that does not slow the emulator

Status: proposal. Nothing here is implemented yet.

**Baseline.** Written against local `main` at `e4cb328`, which is 44 commits
ahead of `origin/main`. `perf_hist.c` and the DAP commands `x16/perfHist` and
`x16Dump` exist only there (`931efad`, `0a02218`, `e4cb328`) and are **not on
`origin/main`**. The other modules named below (`perf_budget`,
`vera_bandwidth`, `io_trace`, `sprite_trace`, `code_map`) are on origin. The
Performance panel was read with uncommitted local edits in place.

## What we want

1. **Know the cost.** We need to measure how much each debugger panel and each
   recorder costs the host. Right now "the IO tab feels slow" is a feeling, not
   a number.
2. **Profiling should not slow the game.** Opening the Performance or I/O panel
   noticeably slows the game today. A profiler that changes the thing it
   measures, or that makes the game unplayable while you capture, is not
   useful.
3. **Capture, then analyse.** Start a capture and play the game at full speed
   with nothing redrawing. When the capture ends, get a breakdown: which
   routines the time went to, self and inclusive, with call counts and worst
   cases, by name.

## One thing to settle first: what "impact" means

Guest cycle counts are **already immune to the UI**. The emulator is
cycle-stepped and single-threaded. A slow ImGui frame delays the host, but
does not add a single 6502 cycle. Every number `perf_budget` and `perf_hist`
report in cycles is correct no matter how many panels are open.

What the panels do hurt is **host time**. On the one thread that steps the
CPU, 60 Hz of UI rendering competes with emulation, so the machine drops below
100% speed, audio stutters, and a human playing the game reacts to a game that
is not running at real speed. That makes captures of real play unrepresentative
even though each cycle in them is counted correctly.

So the goal is: **host overhead low enough that the game stays at 100% speed
during a capture**, and **guest cycle totals identical with capture on or
off**. The plan keeps those two apart throughout.

## What exists today

| Piece | What it does | Cost model |
| --- | --- | --- |
| `perf_budget.c` | Work vs. idle per frame, zones, `$9FBC` guest markers, 3600-frame ring, percentiles | Inline guard per instruction; armed by the Performance panel or DAP (owner model) |
| `perf_hist.c` (local `main` only) | Cycles per effective `(bank, PC)` key (main / IRQ / instruction count), plus one level of "tasks" (JSRs out of a caller range); dump over DAP `x16/perfHist` | Inline guard per instruction; plus a `debug_read6502` of the opcode when the PC is in the caller range |
| `vera_bandwidth.c` | VRAM bandwidth per scanline | Armed with the Performance panel |
| `io_trace.c` | Ring of I/O register accesses and decoded device events | Per `$9Fxx` access while `io_trace_enabled` |
| `code_map.c` | Marks instruction starts for the disassembly views | **Every instruction whenever `-imgui` is on**, panel open or not |
| `sprite_trace.c` | Sprite multiplexing trace | Armed by its tab |

The breakdown we want is mostly there: `perf_hist` already gives **self time
per routine** (the histogram, mapped through debug info by the host). Three
things are missing:

- **Inclusive time and a call tree.** Tasks go only one level deep, from a
  user-named caller range to its JSR targets.
- **Names inside the emulator.** The dump is raw keys, and symbolisation
  happens in an external tool.
- **A capture workflow in the UI** that turns the UI off while it records.

## Where the host time probably goes (hypotheses, to confirm in Phase 0)

Read from the code, not measured yet:

1. **The Performance panel re-sorts its history every UI frame.** The
   Statistics table calls `perf_budget_window()` once per window row (5 rows).
   Each call `qsort`s five arrays of up to 3600 frames. That is about 25 sorts
   and roughly 90k elements, 60 times a second, on the emulation thread.
2. **The I/O Activity tab draws every event in the ring**, as a table row with
   tooltips and no `ImGuiListClipper`, every UI frame. ImGui cost grows with the
   ring size, not with what fits on screen.
3. **Recorders are tied to panel visibility, not to intent.**
   - Opening the Performance panel arms `perf_budget` *and*
     `vera_bandwidth`.
   - The I/O panel writes `io_trace_enabled` only while it is visible, so the
     setting sticks at its last value after the panel closes. Tracing can keep
     running with nobody looking.
4. **`code_map_record_current()` runs on every instruction under `-imgui`**,
   even with every panel closed.
5. **The UI is paced at wall-clock 60 Hz on the emulation thread**
   (`video.c`, `debug_ui_render`), so every one of the costs above is paid in
   emulation time.

## Phase 0: measure first

Make the overhead a number we can watch.

- **Split host time per emulated frame** into emulation, `debug_ui_render`, and
  present. `perf_budget_close_frame` already measures `host_us` for the whole
  frame, UI included; add two `SDL_GetPerformanceCounter` brackets. Show the
  numbers in a small, cheap status line and expose them over DAP.
- **Time each panel's render function** inside `debug_ui_render`: one counter
  pair per registered panel, kept as a running average. This is the
  "we need to be able to know" piece: a "UI cost" readout that names the
  expensive panel.
- **A benchmark.** Run in warp mode, where nothing throttles, and report
  emulated MHz over a fixed number of frames. Run the same boot or game with:
  no `-imgui`; `-imgui` with all panels closed; Performance panel open; I/O
  panel open (Activity tab). Record the results in this file as the baseline.
  Every later phase is judged against it.

Exit: the hypotheses above are confirmed or replaced by measured numbers.

## Phase 1: cheap UI fixes (independent of the profiler)

Each is small, and each is checked against the Phase 0 numbers.

- **I/O Activity:** use `ImGuiListClipper`. Pre-filter into an index vector
  only when the ring's `io_trace_total()` changes, so the filter does not run
  every frame.
- **Performance Statistics:** cache `perf_budget_window()` results. Recompute
  only when `perf_budget_total_frames()` has moved, at most about 4 Hz, and only
  for sections that are expanded.
- **Recorder lifetime:**
  - Each recorder runs because someone asked for it (a checkbox, a capture, a
    DAP owner), not because a panel happens to be drawn.
  - Move the `io_trace_enabled` write out of the visible-only path, so closing
    the panel does what the setting says.
  - Arm VERA bandwidth separately from cycle accounting.
- **`code_map`:** measure it. If it shows up, record only while the machine is
  paused, or while a disassembly view is open, and fill in lazily. The views
  must still decode correctly the first time they open after a run.
- **Optional:** a "UI refresh rate" setting (60 / 30 / 10 Hz) for panels that
  show live data.

These do not depend on anything below and could ship first.

## Phase 2: capture mode

A state machine in one small module (`profile_capture.c`):
**idle, armed, capturing, analysing, report ready.**

- **Start and stop triggers:**
  - manual: a button, a hotkey, or DAP;
  - after N frames or N seconds;
  - on guest `$9FBC` markers: add two marker values, "capture start" and
    "capture stop", next to the existing `$F0`/`$F1`;
  - on a breakpoint hit, reusing the `x16Dump` style of breakpoint that acts
    and keeps running.
- **While capturing, the UI goes quiet.** `debug_ui_render` skips every panel
  and draws one status line: "Capturing: 1,234 frames, 20.6 s, [Stop]". It
  repaints at a low rate (around 4 Hz). Hiding a panel is not enough: the
  recorders that panels arm also have to follow the capture's needs, not the
  panels'.
- **What runs during a capture** is chosen at capture start. The default is
  the histogram and call recorder below. Per-frame budget, VRAM bandwidth,
  I/O trace, and `code_map` are opt-in.
- **Ownership:** use `perf_budget`'s owner model (UI, DAP), so a DAP client
  and the UI cannot stop each other's capture.

Exit: report two separate numbers, because they have different causes and
different targets.

- **Recorder overhead:** the same capture, run headless or over DAP, compared
  with no capture. This is what the recorders cost, and the flat histogram
  writes into arrays of about 2.6M keys on every instruction. The bound is set
  from Phase 0.
- **UI overhead:** a capture under `-imgui` compared with the same capture
  headless. The target is about zero; this is what "the UI goes quiet"
  promises.

## Phase 3: inclusive time and the call tree

Extend `perf_hist`; do not add a third system. The flat histogram already gives
self time. What is added is a **calling-context tree (CCT)**, updated live on
call and return events.

**Why not a raw event log.** A few thousand calls per frame, two events per
call, and 60 frames per second comes to about 360k events per second. Each
record also needs more than 16 bytes: the key alone is 22 bits, and a 32-bit
cycle stamp wraps after about 9 minutes at 8 MHz. That is roughly 6 MB per
second, or about 350 MB per minute. A CCT has one node per unique call path, so
its size depends on the program's structure, not on how long the capture runs.

**Recording.** Hook the control-flow points in the CPU core, the same way
`irq_ctx` hooks interrupt entry and RTI:

- JSR, JSL, RTS, RTL, interrupt entry, and RTI;
- nothing on ordinary instructions, so the per-instruction cost does not grow;
- this replaces the current opcode read (`debug_read6502` in the caller range)
  with a hook that knows the opcode for free.

Each event passes kind, the target's effective `(bank, PC)` key, SP, and a
64-bit cycle count to the tree builder:

- A call finds or creates the child node of the current node: one hash lookup
  in a node pool allocated at capture start.
- A return adds the elapsed cycles to that node's inclusive total, then pops.

At about 180k calls per second, that host cost is small. No symbol lookup
happens in the hot path. If the node pool fills, new paths fold into an
"[other]" node under their parent rather than stopping the capture.

**The tree builder** is a pure module (`profile_cct.c`) with no emulator
globals, so it is unit-testable like `perf_budget.c`. Tests feed it synthetic
event streams; the capture feeds it live. An optional raw event trace, for
debugging the builder itself, can be added later.

- Keep a shadow call stack. Pair each return with its call by
  SP, as `irq_ctx` does, so the usual 6502 stack tricks heal themselves:
  - a return address dropped with PLA/PLA;
  - RTS used as a computed jump (push address minus 1, then RTS);
  - TXS resets;
  - a longjmp back to the main loop.

  A return whose SP matches no open frame unwinds to the nearest frame at or
  above it. A call that never returns is closed at capture end.
- **IRQ time gets its own root** ("[IRQ] handler"), so it never inflates the
  routine it interrupted. This matches how `perf_hist` already splits main and
  IRQ cycles.
- **Known trampolines.** The KERNAL `jsrfar` call is a JSR to a fixed address
  followed by inline target bytes, so the JSR operand is not the routine that
  runs. Attribute through it by recognising the trampoline's address and
  reading the inline operand at record time; that is one extra read on that
  one path. Document any other trampolines the same way as we find them.
- **Per-node stats** accumulate as the capture runs: self cycles, inclusive
  cycles, calls, worst single call, and worst frame. The report then merges
  nodes by routine for a flat view, and keeps the tree for caller→callee
  edges.

## Phase 4: the report

- **Names:** symbolise keys with `dbg_info_addr_to_label_nearest` and the
  banked lookups (`dbg_info_addr_to_source_banked_ex`). A key with no symbol
  shows as `bank:addr`. ROM keys need KERNAL/BASIC symbols, so we need to load
  the ROM's `.sym` or `.dbg` files; there is no ROM symbol table in the tree
  today. Group by the nearest preceding label, so self time from the histogram rolls
  up to routines.
- **Profile panel (new)**, filled only when a report is ready, so it costs
  nothing during a capture:
  - a flat table sortable by self, inclusive, calls, worst call, and percent
    of frame;
  - a call-tree view with expandable callers and callees;
  - a summary with frames captured, total cycles, IRQ share, and idle share
    (from `perf_budget`'s classifier, if it was on);
  - double-click a row to jump to the Disassembly or Source view.
- **Export:**
  - collapsed-stack text (`main;update;draw_sprites 12345`), so speedscope or
    `flamegraph.pl` work on day one without our own flame graph;
  - the existing `PHST` dump, unchanged;
  - JSON for scripts.
- **DAP:** extend `x16/perfHist` with `capture` / `stop` / `report`
  sub-commands instead of adding a parallel command. `tools/x16dbg` gets a
  `profile` verb that starts a capture, waits, and prints the top routines.

## Phase 5: the frame waterfall, linked to source

A timeline of one frame, like a GPU frame capture: time runs left to right
across the frame, and each row is a resource. Every mark on it can be traced
back to the source line that caused it.

```
cycle →   0            33,600          67,200         100,800        134,400
scanline  0              131             262             393             525
CPU       [main_loop][update_actors.......][draw_hud][wait vsync~~~~~~~~~~~]
IRQ       [irq]                                                   [irq]
VERA      ||||| sprite attrs      |||||||||| tilemap        |      palette
PSG          |  |  |
FM        ||  |
PCM       ▮      ▮       ▮       ▮       ▮      (FIFO fill level as a line)
I/O       joy                      sd ||||||||
```

### Rows

- **CPU:** routine spans from the Phase 3 call events, with nesting shown as
  stacked bars, and idle (wait loops, WAI) shaded, using `perf_budget`'s
  classifier.
- **IRQ:** handler spans, from the `irq_ctx` entry and RTI hooks.
- **VERA writes:** split by **what in VRAM was written**, not by register.
  `video_space_write` already knows the target address, so each write can be
  sorted into a class: PSG (`$1F9C0`), palette (`$1FA00`), sprite attributes
  (`$1FC00`), layer maps and tiles (from the live layer config), bitmap, or
  other. Register writes (scroll, config, line IRQ) form their own lane.
- **PSG:** voice register writes, labelled with the voice number.
- **FM (YM2151):** register writes from `memory.c`'s `YM_write_reg` path,
  labelled by register.
- **PCM:** FIFO writes, plus the FIFO fill level as a line. That shows
  underrun risk directly.
- **I/O:** VIA, joystick, SD/SPI, I2C/SMC, serial, and MIDI. This is the same
  source as `io_trace`.
- **Markers:** vsync, the line IRQ, `$9FBC` guest markers, and scanline ticks
  along the top axis. At 256 cycles per line, the x axis doubles as beam
  position, so "this palette write landed mid-screen" is visible.

### The record

The existing `io_event_t` is about 96 bytes because it carries 72 bytes of
decoded text. That is fine for an I/O log, but too large at VERA data-port
rates. The waterfall uses a compact record instead, about 12 bytes:

```
u32 cycle-in-frame | u16 pc | u8 bank (RAM or ROM, as perf_hist keys it)
u8 lane/class      | u16 target (register or VRAM offset) | u8 value | u8 flags
```

Decoded text, such as SD command names, is produced when the event is
*displayed*, not when it is recorded.

### Event volume: two layers

A busy game can do tens of thousands of VERA writes per frame. Recording every
event for a whole session runs into the same volume problem as the raw call
log (about 20k events × 12 bytes × 60 fps ≈ 15 MB/s). So there are two layers:

1. **Aggregates for the whole capture, always on.** Keep counters keyed by
   `(source key, lane/class)`, where the source key is the same effective
   `(bank, PC)` key the histogram uses. Each counter records writes, plus
   cycles for the CPU lane. The cost is one increment per event, and the memory
   size depends on the code, not on time. This answers "which lines write the
   most to the palette over the whole run".
2. **Full-detail waterfall frames, for a bounded set of frames:**
   - a rolling ring of the last N frames (default 120);
   - the **worst K frames**, by work cycles or by any lane's count, kept even
     after they leave the ring. Slow frames are what you came for, and you
     should not have to stop the capture on the right frame to see one;
   - frames you mark: a hotkey, a `$9FBC` marker, or a breakpoint, using the
     Phase 2 triggers.

   Each frame is a separate buffer, and a frame that overflows its buffer is
   flagged as truncated rather than quietly cut short.

### Linking to source

`dbg_info_addr_to_source_banked_ex(addr, ram_bank, …)` already resolves a
`(bank, PC)` pair to a file and line. It also says when a banked address might
match more than one segment, so the UI can show "maybe" instead of a confident
wrong line. All lookups happen after the capture, never while recording.

- **Waterfall:**
  - hover any mark to see the routine, the file and line, and the
    instruction;
  - click to open the Source panel (or Disassembly when there are no debug
    lines) at that line;
  - drag across a range of the frame to get "what code ran here", a table of
    lines and routines ranked by cycles in that range.
- **Source heat map:** the Source panel gets a gutter showing cycles per line
  for the capture (from the histogram), plus per-lane counts ("312 VRAM / 4
  PSG writes"). Lines roll up into routines and files. This is the "what code
  is taking the time" view, and it works without ever opening the waterfall.
- **Assembly vs. high-level source:** `.dbg` files from cc65/ca65 can carry
  both. With `dbg_info_has_high_level()`, the heat map can show C lines and
  expand each into its assembly lines.
- **No debug info:** fall back to labels from `dbg_info_addr_to_label_nearest`,
  then to raw `bank:addr`. The ROM needs its own `.dbg` or `.sym` files to get
  KERNAL names, as noted in Phase 4.

### Export

Write the detailed frames as **Chrome trace-event JSON**: one track per lane,
CPU spans as nested slices, and writes as instant events with source file and
line in their arguments. Perfetto (ui.perfetto.dev) and `chrome://tracing` open
that format directly. That gives a zoomable waterfall on day one, before our
own ImGui view exists. The ImGui view follows; it is the one that can jump to
source in the emulator.

### Cost

- Recording a write is the inline guard plus one compact record and one
  aggregate increment. That is on the order of the existing `io_trace` access
  hook, and it is paid only on device accesses, not on every instruction.
- The CPU lane reuses Phase 3's call hooks, so it adds nothing per instruction.
- As in Phase 2, nothing is drawn while a capture is running.

### Dependencies

- The CPU and IRQ lanes need Phase 3.
- The device lanes, the aggregates, the source heat map, and the Perfetto
  export need only Phase 2 and the compact record. They can be built first,
  and they already answer "which code is hammering VERA or the YM".

## Constraints every phase has to respect

- **Two per-instruction loops.** `emulator_loop` and
  `emulator_step_during_move` (the Windows drag loop) each step the CPU. Any
  per-instruction hook goes in both, or captures silently lose time whenever
  the window is dragged. CPU-core hooks (Phase 3) avoid this by design, which
  is one reason to put them there.
- **Test-linkable.** New recorder and analysis code takes machine state as
  arguments, as `perf_budget.c` and `perf_hist.c` do. Anything `memory.c` or
  `video.c` newly calls needs a stub in `tests/support/fake_devices.c`. Per
  AGENTS.md, only Linux CI catches a missing stub.
- **Cost when off stays as it is today:** one load and one predictable branch
  per instruction for each recorder.
- **Guest-side determinism:** capture on or off must not change a single guest
  cycle. A test runs a fixed program both ways and compares cycle totals and
  final RAM.

## Acceptance

- Phase 0 numbers are recorded in this file.
- With a capture running under `-imgui`, warp throughput is within the agreed
  bound of the no-UI baseline, and a real-time game holds 100% speed.
- Guest cycle totals are identical with capture on and off.
- On a test program with known nesting (including a PLA/PLA early exit, an RTS
  jump table, and an IRQ landing mid-routine), the tree builder's inclusive and
  self times match hand-computed values in unit tests.
- On a real game, the Profile panel names the top routines, and its self-time
  totals agree with the flat `perf_hist` histogram.

## Suggested order

1. Phase 0, measurement (small; everything else is judged by it).
2. Phase 1, UI fixes (ships on its own and helps immediately).
3. Phase 2, capture mode with the existing flat histogram only. Combined with
   symbol roll-up from Phase 4, this already answers "where does the time go,
   by routine" (self time).
4. Phase 3, the calling-context tree (adds inclusive time and the tree).
5. Phase 4, the rest of the report UI and DAP/x16dbg.
6. Phase 5, the waterfall. The device lanes, the source heat map and the
   Perfetto export can be built right after Phase 2, in parallel with Phase 3.
   The CPU lane arrives with Phase 3.

## Open questions

- Should capture mode pause `code_map` by default? It breaks nothing while
  running, but the Disassembly view would have to tolerate gaps afterwards.
- Phase 3 depends on `perf_hist`, which is not on `origin/main` yet. Should it
  land as its own PR first?
- Do we want per-frame call-tree snapshots, for questions like "why was frame
  812 slow"? Or are session totals plus worst call and worst frame enough?
- Is 65C816 native-mode code (JSL/RTL, 24-bit PCs) in scope now, or only
  6502/65C02 X16 programs?
