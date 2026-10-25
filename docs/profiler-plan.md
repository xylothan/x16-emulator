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

### Phase 0: what was built

- **`src/host_prof.c`** splits host time per emulated frame into exclusive,
  nesting sections: `emu` (whatever no other section claims), `events`,
  `present`, `ui_build`, `ui_draw`, `sleep` and `dap`. It also times each panel
  separately. It takes its clock as a function pointer, and
  `tests/test_host_prof.c` drives it with a fake clock.
- **Menu-bar readout:** "host N.N ms" is the smoothed busy time per frame.
  Hovering it shows each section and each panel.
- **`x16/hostPerf` over DAP**, with an optional `reset`.
- **`-bench <frames> [<warmup>]`** prints the report and exits. It refuses
  `-headless`, which never closes a frame.
- **`-uipanels <list|all|none>`** opens exactly those panels. Combined with
  `-bench`, it also stops the run from reading or writing `imgui.ini`.
- **`-uiiotrace`** forces I/O capture on for VIA, YM, I2C, joystick, SD and
  files. It takes effect only with both `-bench` and `-uipanels`.
- Without `-uipanels`, a benchmark measures **the user's own layout** from the
  `imgui.ini` in its working directory.

Each configuration below is one run of:

```
x16emu -rom latest_rom\rom.bin -prg <dir>\<program>.prg -run -fsroot <dir>
       -ram 2048 -dbgfile <dir>\<program>.dbg -srcpath <src>
       <config flags> -bench <frames> <warmup> [-warp]
```

The config flags are:

| Config | Flags |
| --- | --- |
| no `-imgui` | none |
| `-debugport` only | `-debugport 9772` |
| `-imgui`, no panels | `-imgui -uipanels none` |
| all panels | `-imgui -uipanels all` |
| Performance only | `-imgui -uipanels Performance` |
| I/O only, capture on | `-imgui -uipanels I/O -uiiotrace` |
| user layout | `-imgui`, run from a folder holding a copy of the user's `imgui.ini` |

### Phase 0: baseline (2026-10-23)

> **Correction (2026-10-24): the workload was a wait loop.** The test program
> stops at a start menu and waits for a button in a `WAI` loop. The runs below,
> and every run up to the Phase 2 work, never pressed it. Most of them
> therefore measured that wait loop: about 1 cycle per emulator step, against
> about 2.9 for the program's running code.
>
> - **Comparisons still hold.** Before and after were always measured on the
>   same workload, so they remain valid as comparisons.
> - **Per-step costs were overstated.** That includes the debugger check: a
>   wait loop runs about three times as many emulator steps per frame as
>   running code does.
>
> Runs now pass `-autostart` (tap START until the program is running), and the
> benchmark output names the guest PC at the end, so a run stuck in a wait
> loop is caught. The fast-path result was re-measured on running code;
> see Phase 1.

Setup:

- **Machine and build:** Windows, this developer's machine, Release build of
  branch `profiler-phase0`, which is committed `main` (`e4cb328`) plus this
  phase. It does **not** include the uncommitted Performance-panel and
  sprite-tab edits, or the VERA 49 version bump, from the main checkout. The
  Performance-panel numbers therefore describe the committed panel.
- **Workload:** a banked assembly program (`-ram 2048`, files served from
  `-fsroot`, with `-dbgfile`), left running with no input.
- **ROM:** `latest_rom\rom.bin`.
- **Native runs:** 1,200 warm-up frames, then 1,800 measured.
- **Warp runs:** 3,600 warm-up frames, then 3,600 measured.
- **"user":** a copy of the developer's own `imgui.ini`, which has I/O capture
  off.

**Native speed** (ms per emulated frame; one frame is 16.8 ms):

| Config | busy p50 | p95 | max | emu | present | ui_build | ui_draw | Top panel |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| no `-imgui` | 3.49 | 3.84 | 4.77 | 2.93 | 0.59 | – | – | – |
| no `-imgui`, second run | 3.37 | 4.32 | **27.56** | 2.93 | 0.49 | – | – | – |
| `-debugport` only | 4.40 | 4.86 | 16.84 | 3.85 | 0.45 | – | – | (dap 0.16) |
| `-imgui`, no panels | 4.62 | 5.02 | 6.19 | 3.77 | 0.58 | 0.05 | 0.23 | – |
| all panels | 5.88 | 6.30 | **17.09** | 4.21 | 0.58 | 0.64 | 0.33 | Performance 0.53 |
| Performance only | 5.76 | 6.47 | 8.51 | 4.28 | 0.52 | 0.56 | 0.32 | Performance 0.54 |
| I/O only, capture on | 6.25 | 6.74 | 8.03 | 3.83 | 0.52 | 1.56 | 0.27 | I/O 1.64 |
| user layout | 5.97 | 6.67 | **23.96** | 4.30 | 0.54 | 0.65 | 0.33 | Performance 0.56 |

**Warp** (emulated MHz; a higher number is better):

| no `-imgui` | `-imgui`, no panels | all panels | Performance only | I/O, capture on | user layout |
| --- | --- | --- | --- | --- | --- |
| 49.3 | 36.6 (−26%) | 30.2 (−39%) | 28.5 (−42%) | 30.3 (−39%) | 28.9 (−41%) |

What this says, against the hypotheses:

1. **The largest single cost is the debugger being attached at all, before any
   panel draws.** Under `-imgui`, emulation goes from 2.93 to 3.77 ms per frame
   (+29%), and warp throughput drops 26%.
   - **It is per-instruction work, not drawing.** The emulation cost is about
     the same per emulated frame at native speed (+0.84 ms) and in warp
     (+0.88 ms), even though warp draws the UI about 4.5 times less often per
     frame (0.92 down to 0.20 UI frames per emulated frame).
   - **It is the status check, not `code_map`.** (Corrected. This first
     argued from `-debugport` costing the same as `-imgui`, but `-debugport`
     also sets `debugger_enabled`, so it runs `code_map` too and that
     comparison proved nothing.) Measured instead with two throwaway builds on
     `main` at `4735808`, `-imgui` with no panels, emulation per frame:

     | Build | `-debugport` | `-imgui` |
     | --- | --- | --- |
     | as is | 3.88 ms | 3.86 ms |
     | `code_map_record_current` removed | 3.77 ms | 3.69 ms |
     | status check removed (call and gate) | 2.94 ms | 2.86 ms |

     The same run without a debugger was 2.97 ms. So the status check costs
     about 0.9 ms per frame and `code_map` about 0.1–0.17 ms, near noise.
     Removing the status check alone reaches the no-debugger speed
     (hypothesis 4: right place, wrong function).

   Phase 1 should look at this first.
2. **The Performance panel costs about 1 ms per frame in two parts.**
   - About 0.55 ms is the panel's own render. It grows to 0.74–0.80 ms once the
     3,600-frame history is full (warp), which fits the percentile-sort
     hypothesis (1). The sort and the drawing have not yet been timed
     separately.
   - About 0.5 ms more is emulation time from the recorders it arms,
     `perf_budget` and `vera_bandwidth` (hypothesis 3, confirmed).
3. **The I/O Activity log, with capture on and a full ring, is the most
   expensive panel: 1.6–1.8 ms per render** (hypothesis 2, confirmed). Capture
   itself is cheap (+0.06 ms of emulation). With capture off, which is the
   user's saved setting, the I/O panel costs about 1 µs. So the I/O slowdown
   seen in practice depends on capture being on.
4. **Spikes are not explained yet.** On this machine, p95 busy time stays under
   about 6.7 ms against a 16.8 ms frame in every configuration, so the
   averages leave plenty of slack. Single worst frames of 17–28 ms (a missed
   frame) showed up in several runs, **including one with no debugger at
   all**. Each of those is one frame from one run, so it is an observation,
   not yet the cause of the slowdown the user sees. Phase 0 does not record
   which section a worst frame came from; that comes next.
5. **Fixed costs:**
   - Presenting the main window takes about 0.5 ms per frame.
   - Drawing the debugger window takes 0.23–0.33 ms per frame.
   - With DAP enabled (`-debugport`), polling the socket takes about
     0.16–0.2 ms per frame. That includes this profiler's own clock reads
     around each poll, and was measured mostly with no client attached.

Caveats:

- **One machine, one workload.** On a slower host, or in a heavier scene, the
  emulation share grows and the same UI cost eats the remaining slack sooner.
- **A panel is measured only while its window is visible.** In the first warp
  I/O run the debugger window was hidden, and the I/O panel read 2 µs. That
  run was discarded; the rerun, used in the table, read 1,755 µs. Keep the
  debugger window on screen while benchmarking.
- **Only the front tab of each dock was drawn.** In "all panels" and "user
  layout", panels such as VERA, Memory and Source were hidden tabs, so their
  ~1 µs means "not drawn", not "cheap". The Phase 1 order below ranks only
  what was actually drawn. Running each panel alone (`-uipanels <name>`, a few
  hundred frames each) would fill that gap.
- **Busy-time percentiles cover the last 1,024 frames**, not the whole run.

Phase 1 order, by measured cost:

1. The per-instruction debugger status check (`DEBUGGetCurrentStatus`), which
   runs under `-imgui` and `-debugport`.
2. The I/O Activity table: clipping, and filtering only on change.
3. The Performance panel: cached statistics, and recorders armed by intent
   rather than by visibility.
4. Recording which section caused each spike.

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
- **The per-instruction status check:** measured in Phase 0 as the largest
  cost, at about 0.85 ms per frame (~22 ns per instruction). `code_map`
  measured close to free, so leave it.

  With no breakpoints set, `debug_bp_on_arrival` is an empty loop. The cost
  is overhead paid on every instruction while running:
  - the out-of-line call to `DEBUGGetCurrentStatusEx`;
  - `cpu_irq_take_entered_flag`, `hitBreakpoint` on the step target, and
    `video_debug_ui_available`;
  - **`SDL_GetKeyboardState`, a call into the SDL DLL, made only to check the
    Break key.**

  The fix is a fast path the loops test inline before calling in:
  - **One flag says "nothing can stop here".** It is true while running, with
    no step target, no pending watchpoint stop, no "break on interrupt", and
    no interrupt-entry flag waiting to be consumed.
  - **One bit says "a breakpoint may sit at this PC"**: a 64K-bit map (8 KB)
    over the CPU address, kept in step with every add and remove. Bank,
    condition, hit count and ignore budget are still decided by the slow path,
    so their semantics do not change.
  - **The Break key and the DAP socket are checked on the existing
    1-in-1000-instruction throttle**, not on every instruction. That is about
    0.1 ms of latency.
  - Both `emulator_loop` and `emulator_step_during_move` use it.

  Risk: breakpoint semantics. The resume guard, the WAI guard, hit and ignore
  counts, dump breakpoints and interrupt following all depend on exactly when
  the slow path runs. The DAP testbench (126 checks) and the `debug_core` unit
  tests must stay green, with new tests for "a breakpoint added while running
  fires on its next arrival".

  Target: emulation under `-imgui` with no panels within 5% of the no-debugger
  2.93 ms per frame.

  **Re-measured on running code (2026-10-24).** The table below this one was
  taken on the start menu's wait loop (see the correction under Phase 0). This
  is the same interleaved method, three runs each, with `-autostart`. All 18
  runs ended in running code:

  | Build | no debugger | `-debugport` | `-imgui`, no panels |
  | --- | --- | --- | --- |
  | before | 3.26 ms | 3.93 ms | 3.89 ms |
  | fast path | 3.21 ms | 3.37 ms | 3.35 ms |

  `-imgui` is 4.4% over no debugger, within the 5% target. The saving is
  0.54 ms per frame, not 0.96. Running code averages about 2.9 cycles per
  instruction, so it takes about a third as many emulator steps per frame as
  the wait loop, and a per-step cost shrinks accordingly.

  **Done (`67ff70f`, `307712f`).** Measured interleaved: the same source with
  the fast path switched off ("before": the inline check always takes the slow
  path and the gate makes its old out-of-line calls) alternating with it on,
  three runs each, at native speed on the Phase 0 workload, with `-hidden`.
  Emulation per frame, median of three; the runs agreed within ±0.05 ms:

  | Build | no debugger | `-debugport` | `-imgui`, no panels |
  | --- | --- | --- | --- |
  | before | 3.04 ms | 3.92 ms | 3.91 ms |
  | fast path | 2.96 ms | 3.10 ms | 2.95 ms |

  - **`-imgui` now costs nothing measurable:** 2.95 ms against 2.96 with no
    debugger, where it was 32% over. The target is met with room to spare.
  - **`-debugport` is 5% over,** which is `code_map`, still recorded on every
    instruction in that mode.
  - **The no-debugger figure improved too (−0.08 ms),** because the old gate
    called `debug_server_is_enabled()` on every instruction in every DAP-capable
    build, debugger or not. It is now read once per frame.

  An earlier single-session comparison against `main` gave the same picture
  but sat within the ~5% drift seen between sessions, which is why this was
  re-measured interleaved.

  DAP testbench: 129/129, including three new checks for state changed while
  the machine runs. Its pause check fails above 0.2 s, which a counter that
  polled once per million instructions (~0.4 s) would exceed.
- **Attribute spikes:** keep a per-frame ring of section times in `host_prof`,
  and have `-bench` and `x16/hostPerf` report the worst frames by section. That
  answers whether the 17–28 ms frames come from the emulator, the UI, present,
  or the OS.

  **Done.** `host_prof` keeps every recent frame's section breakdown in its
  ring, and the 8 worst frames since reset outside it. A frame whose busy time
  exceeds its own real-time budget (guest cycles ÷ MHz, 16.8 ms for VGA at
  8 MHz) counts as a spike, and each spike is attributed to the section that
  dominated it. `-bench` prints both, and `x16/hostPerf` returns `spikes` and
  `worstFrames`.

  First capture: the Phase 0 workload, 3,600 frames, `-hidden`:

  - **No spikes.** No frame exceeded 16.8 ms, either with no debugger or with
    the user's layout.
  - **The worst frames are present stalls.** Busy time was 8.7–11.7 ms, with
    6–8 ms of it spent presenting the main window (normally 0.3–0.5 ms).
  - **Next come emulation peaks:** up to 6.1 ms against a typical 3–4 ms,
    which is the program's own work.
  - **The debugger UI was never the worst part**, at about 1.5 ms at most.

  Caveat: with the windows hidden, nothing is composited on screen, so present
  can behave differently from a visible run. The 17–28 ms frames seen earlier
  came from visible runs. Capturing them visibly is the open question.

  **Visible capture**, same workload and the same build (`profiler-spikes`,
  which does not include #94's panel fixes):

  | Config | Spikes over 16.8 ms | Worst frames |
  | --- | --- | --- |
  | no debugger | 15 of 3,600: emu 12, events 2, present 1 | 20–44 ms, nearly all `emu` |
  | user layout | 0 of 3,600 | ≤ 11.3 ms; `present`, then `emu` |

  - **The spikes are on the emulator thread, outside every UI section.** The
    long frames are almost entirely `emu`, which is time no other section
    claimed. They cluster in frames 145–189, about one second of the run,
    plus one at frame 1821.
  - **Working hypothesis: host file I/O.** The program loads files through
    the host filesystem in that stretch, and that I/O runs on the emulator
    thread, so it is charged to `emu`. The no-debugger run went first, and the
    layout run that followed, with any OS file cache warm, had none. That
    fits, but does not prove it.
  - **One frame stalled for 17 ms in the SDL event pump** (`events`), with the
    window visible.
  - **Next step:** add a host-filesystem section around the hostfs/IEEE file
    calls, so this shows up by name instead of inside `emu`.

  **Hypothesis tested and ruled out.** Two new sections: `hostfs` (KERNAL file
  calls served from `-fsroot`) and `dbginfo` (`dbg_load_poll`). The program's
  files were copied with `robocopy /J` (unbuffered, so not left in the file cache),
  then run visibly twice, cold then warm, with no debugger:

  | Run | Spikes over 16.8 ms | Long frames |
  | --- | --- | --- |
  | cold | 4 of 3,610, plus 6 just under | all `events`: 15–25 ms each, ten consecutive frames (1873–1882) |
  | warm | 0 of 3,600 | worst 7.7 ms |

  - **File I/O is not the cause.** `hostfs` averaged 0.2 µs per frame and
    `dbginfo` zero, and neither appears in any long frame.
  - **The stalls move around.** This run caught them in the SDL event pump
    (`events`); the earlier one caught them inside `emu`. The same cold files
    a minute later produced none.
  - **Reading:** these are intermittent host-side stalls of the emulator
    thread, landing in whatever section the thread happened to be in. The
    debugger plays no part: every spike so far was in a run with no debugger
    attached.
  - **Remaining candidates:**
    - the OS descheduling the thread;
    - the Windows message pump blocking inside `SDL_PollEvent` (window or
      compositor messages);
    - SDL's audio device lock, which sits inside `emu` because audio is
      rendered there.

    An `audio` section, and timestamps on each spike for matching against
    system events, would separate them.
- **Optional:** a "UI refresh rate" setting (60 / 30 / 10 Hz) for panels that
  show live data.

Targets for the panels:
- I/O Activity with a full ring: under 0.1 ms per render (from 1.6).
- Performance panel: under 0.15 ms per render (from 0.55). Its two recorders
  cost 0.5 ms of emulation between them; measure `perf_budget` and
  `vera_bandwidth` separately once they can be armed apart.

**Panels done.** Measured before and after on one session, with `-hidden`,
render time per panel:

| Panel | before | after |
| --- | --- | --- |
| I/O, Activity tab, capture on, full 4096-event ring | 1,703 µs | 36 µs |
| Performance | 561 µs | 80 µs |
| user layout, UI build time per frame | 0.66 ms | 0.21 ms |

- **I/O:** only the visible rows are drawn (`ImGuiListClipper`), and only
  events that arrived since the last frame are filtered. Capture settings are
  now applied every debugger frame, so changing them in System > Settings
  works with the panel closed.
- **Performance:** the Statistics and zone percentiles are cached, and
  refreshed at most every 0.25 s, only when a frame completed or the budget or
  zones changed.
- **The Performance panel's recorders now run only while it is drawn.**
  With the panel visible, emulation is about 0.3–0.45 ms per frame above the
  no-recorder figure; that is `perf_budget` plus `vera_bandwidth`. They used to
  be armed whenever the panel was open, which by default meant every `-imgui`
  session, even with Performance behind another tab, and they stayed armed
  after the panel was closed with its [x]. They are now armed while the panel
  is drawn, or while "Keep profiling when hidden" is on, and disarmed half a
  second after it stops being drawn. Disarming discards the history.

Done when the Phase 0 benchmark set, rerun, meets these targets, and CI
(including `wasm`) is green.

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

- **Recorder overhead:** the same capture, run windowed without `-imgui`,
  compared with no capture. (Not headless: headless never runs `video_step`, so
  no frame closes, and it skips audio, so it does not compare.) This is what the recorders cost, and the flat histogram
  writes into arrays of about 2.6M keys on every instruction. The bound is set
  from Phase 0.
- **UI overhead:** a capture under `-imgui` compared with the same capture
  windowed without it. The target is about zero; this is what "the UI goes quiet"
  promises.

### Phase 2: what was built (2026-10-24)

- **`profile_capture.c`**, the state machine:
  - states idle → armed → running → done;
  - ends on a frame limit, a time limit, a requested stop, or the guest's
    `$9FBC` markers (`$F8` starts, `$F9` stops);
  - a marker only starts a capture the user armed;
  - recorders and the clock are callbacks, and the module is unit-tested.
- **`profile_report.c`**, the report:
  - `perf_hist`'s per-(bank, address) cycles rolled up to routines, by the
    nearest routine label in the same RAM bank;
  - time no label covers is grouped by 256-byte page, so the rows always sum
    to the histogram's totals, main-line and IRQ separately;
  - a "cycles per instruction" column, where a figure near 1 means `WAI`
    (waiting);
  - unit-tested against the real `perf_hist`.
- **`dbg_info`** now reads two things it ignored:
  - ld65's `bank=` on segments, so banked code is named correctly without
    watching the LOADs;
  - scope records, so local labels (`loop`, `next`) roll up into their
    routine.

  On a banked test program this took the report from 3,017 of 3,291 rows
  "bank not established" to 0 of 325, all named by routine.
- **`perf_budget`'s marker hook** runs ahead of its marker-mode latch, so
  starting a capture from the guest does not change how work and waiting are
  classified.
- **The UI goes quiet while capturing:** only the menu bar (status and Stop),
  at most 4 times a second. The new **Profile** panel holds the controls and
  the report table.
- **Commands:** DAP `x16/capture`, and on the command line
  `-profile <frames> [<warmup>]`. (`-capture` already means "grab the mouse".)
- **`-autostart <frames>`** taps START so scripted runs get past a start menu.
  `-bench` now prints the guest PC at the end. (Since generalised: see
  "Scripted input" below.)

`perf_hist` costs 0.24 ms per frame on a `WAI` wait loop. On running code,
which takes fewer emulator steps per frame, the cost is within noise.

**Exit criteria, met** (the Phase 0 workload past its start menu, `-hidden`,
1,800 frames, two runs each; all six ended in running code):

| Config | Emulation | Busy p50 | UI build |
| --- | --- | --- | --- |
| no debugger, no capture | 3.37 / 3.16 ms | 3.50 / 3.45 ms | – |
| no debugger, capture | 3.31 / 3.22 ms | 3.55 / 3.49 ms | – |
| `-imgui`, all panels, capture | 3.48 / 3.44 ms | 3.77 / 3.73 ms | 4 µs |

- **Recorder overhead:** within noise.
- **UI overhead:** drawing is effectively zero during a capture, 4 µs per
  frame against 640 µs normally with every panel open. The remaining ~0.2 ms
  under `-imgui` is having a debugger attached at all: the fast-path check,
  plus `code_map` recording every instruction. Pausing `code_map` during a
  capture would remove most of it, at the cost of the Disassembly views having
  a gap for that stretch. That is still the open question below.
- **Determinism:** the guest's cycle count in its top routine matched to
  within 6 cycles across all four captures, with and without the debugger.

### Scripted input

A benchmark or capture is only repeatable if the guest does the same thing
each run, so input is scripted by emulated frame, like a TAS input file
(`src/input_script.c`, unit-tested in `tests/test_input_script.c`):

```
# <frame> <command>          frame 1 is the first; +N is N after the previous line
60   joy1 start              # pad 1 holds START from frame 60
+6   joy1 none               # ...and lets go 6 frames later
+30  key Return              # press and release a key (SDL key names)
+2   keydown Left Shift
+1   key A
+1   keyup Left Shift
900  joy1 right+a            # held until changed
1800 joy1 off                # hand the pad back to the host
```

- **`-inputscript <file>`**, or **`-input "<cmd>; <cmd>"`** inline.
- **`-autostart <frames>`** is now shorthand for a script of START taps.
- Sources combine; each is its own timeline, and events merge by frame.
- A bad line stops the emulator at startup, naming the line.
- Pads go through `joystick_set_virtual`, keys through `handle_keyboard`:
  the same paths as DAP `x16/joystick` and `x16/sendKey`.

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

### Phase 3: what was built (2026-10-24)

- **`src/profile_cct.c`**, the tree builder. It is pure and is unit-tested in
  `tests/test_profile_cct.c`, which covers:
  - nesting, repeated calls, worst call and worst frame;
  - PLA/PLA early exits, RTS jump tables and TXS resets;
  - an IRQ landing mid-routine, a call inside a handler, and BRK;
  - recursion, a return with nothing open, and a call that never returns;
  - the pool filling up, so new paths fold into `[other]`.
- **CPU hooks:** `cpu_flow_hook` lives in `cpu/irq_ctx.c`, which every build of
  the core already links. It fires on JSR, JSL, RTS, RTL, interrupt entry and
  RTI, and is NULL when off: one load and branch per call or return, nothing on
  other instructions.
- **Cycle ordering matches `perf_hist` exactly.**
  - Calls and returns are queued, then applied after the instruction's cycles
    are counted in `perf_hist_step_`. A JSR's cycles go to the caller and an
    RTS's to the callee.
  - Interrupt entry and RTI apply at once, so BRK's cycles land in the handler
    and RTI's in the code it returns to.
  - The result: tree self time equals the histogram's main and IRQ totals to
    the cycle, and every report checks this ("self totals match the
    histogram").
- **Pairing:** calls and returns are matched by stack pointer on one shadow
  stack. Before each event, frames the stack has risen past are popped. A
  return below the innermost frame is a computed jump and pops nothing.
- **Trampolines, handled generally.** A JSR whose target is a `JMP abs` or
  `JMP (abs)` stub is keyed by where the JMP goes, read at call time, which is
  exact. This covers:
  - jump tables;
  - the KERNAL's `jmpfr` (`$02DF`), through which `jsrfar` from every ROM bank
    reaches its target;
  - a program's own far-call stubs.

  Matching only on `$FF6E` would have missed most far calls: the ROM banks
  carry their own `jsrfar` copies, and BASIC's is at bank 4 `$EA08`.
- **Report additions:**
  - per routine: inclusive cycles, calls, worst call and worst frame. A node
    inside an outer call of the same routine is not counted twice;
  - the tree itself, printed top-down by `-profile`;
  - a Call tree tab in the Profile panel;
  - `tree` and `treeConsistent` fields in `x16/capture`.
- **Recorders:** `tree` is a capture recorder, on by default. Use
  `-profilerec` to choose recorders from the command line.

**Cost** (Phase 0 workload past its start menu, `-hidden`, 1,800 frames,
interleaved, six runs each, medians):

| Recorders | Emulation per frame | Busy p50 |
| --- | --- | --- |
| histogram | 3.37 ms | 3.65 ms |
| histogram + tree | 3.42 ms | 3.69 ms |

- **The tree costs about 0.05 ms per frame (~1.5%),** with 1,084 nodes in
  that run's tree.
- **Guest-side determinism.** The test is a scripted BASIC loop (600 frames).
  Two histogram-only runs and two histogram-plus-tree runs gave identical
  main and IRQ cycles in every reported row, and identical instruction counts.
  - Recording the tree does not change the guest. The flow hook only reads
    memory, with side-effect-free debug reads.
  - The Phase 0 workload cannot settle this either way: it varies by a few
    hundred instructions between runs even with the histogram alone.

Tested:
- the DAP testbench, 138/138, including a live capture whose tree totals must
  equal the histogram's and whose `[IRQ]` root must hold the KERNAL handler;
- a BASIC loop, which exercises `jsrfar` across ROM banks;
- the Profile panel's Routines and Call tree tabs, drawn in a Debug build
  (ImGui asserts on) for two reports in a row.

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

### Phase 4: what was built (2026-10-24)

- **ROM symbols (`src/rom_syms.c`).** The x16-rom release ships one VICE
  label file per bank beside `rom.bin`. They load at startup from that folder,
  or from `-romsyms <dir>`.
  - **Bank mapping:** the same as the ROM's own build (its `rom_labels.h`
    rule): kernal 0, keymap 1, dos 2, fat32 3, basic 4, monitor 5, charset 6,
    diag 7, graph 8, demo 9, audio 10, util 11, bannex 12. It was checked
    against the R49 listing (`rom_lst.h`).
  - **What is kept:** labels in each bank's `$C000-$FFFF`, plus the KERNAL's
    RAM-resident code at `$0200-$03FF` (the IRQ entry `__irq`, `jsrfar`'s RAM
    half). The RAM labels name that range only where the program's debug info
    does not cover it.
  - **What is dropped:** ld65 segment symbols (`__X_RUN__` and similar) and
    data addresses.
  - Ties at one address break by name, so naming does not depend on load
    order.
- **The effect,** on a BASIC `PRINT` loop: rows like `ROM 0:$CCxx` and
  `ROM 4:$EA08` now read `screen_copy_line` and `bjsrfar`. The tree shows
  `gone3 → outdo → bsout → bjsrfar → nbsout → nxln → scro0 →
  screen_copy_line`: 82% of that loop is screen scrolling.
- **Exports:**
  - `.folded`: collapsed stacks with self cycles, `[main];a;b 1234`, for
    speedscope or `flamegraph.pl`. The counts sum to every captured cycle.
  - `.json`: totals, every row and the whole tree.
  - **How to write them:** `-profileout <prefix>` with `-profile`;
    `export: {folded, json}` on `x16/capture` (the emulator writes the files);
    or the Profile panel's Save buttons.
- **Summary:** when the capture ran the `budget` recorder, it reports work
  against waiting from `perf_budget`'s classifier, the worst frame's work, and
  frames over budget. This appears in the printed report, the panel and the
  JSON.
- **Panel:** the Routines table sorts by any column.
- **`x16dbg profile <frames> [top n] [folded path] [json path] [rec list]`**
  starts a capture, waits for it, and prints the top routines and heaviest call
  paths. It also works in interactive mode.
- **Not done here:** the plan suggested extending `x16/perfHist` with capture
  sub-commands. `x16/capture` already covers that, so `x16/perfHist` stays the
  raw histogram dump.

Tested:
- unit tests: `rom_syms`, and ROM and KERNAL-RAM naming in `profile_report`;
- the DAP testbench, 143/143, including the export checks;
- `-profileout` and `x16dbg profile` against a live BASIC loop;
- the panel, drawn in a Debug build for two reports in a row.

**Next, with the ROM source** (`x16-rom`): its Makefile links each bank with
`ld65 -Ln` only. Adding `--dbgfile` would give `.dbg` files with source lines
for ROM code, which the Phase 5 source heat map could use for the KERNAL and
BASIC as well as the program.

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

### Phase 5 split

Phase 5 lands in three PRs:

- **5a:** the recorder, aggregates and Perfetto export (below);
- **5b:** the ImGui waterfall view, with hover and jump-to-source;
- **5c:** the Source panel heat map.

### Phase 5a: what was built (2026-10-24)

- **`src/waterfall.c`**, pure and unit-tested in `tests/test_waterfall.c`:
  - 12-byte records;
  - a ring of the last 30 frames, plus the 8 worst kept after they leave it.
    Frames are ranked by work cycles when the `budget` recorder runs, else by
    event count;
  - frames that overflow their 32k-event buffer are flagged truncated;
  - whole-capture aggregates per (instruction key, lane);
  - the begin/end walk the export uses.
- **Lanes:** CPU, VERA registers, PSG, palette, sprites, layer 0 and 1 maps
  and tiles, other VRAM, PCM, SPI/SD, YM2151 (by selected register), VIA1,
  VIA2, and the rest of the I/O page.
  - VRAM writes are classed by where they land, using the layer registers as
    they stand at the moment of the write.
  - VERA FX cache writes are flagged as four bytes.
- **Hooks:** both are NULL when off.
  - `io_write_tap` lives in `io_trace.c`, which every link of `memory.c`
    already includes, so no test stubs were needed.
  - `video_vram_write_tap` lives in `video.c`, at the data-port write.
  - Data-port bytes come only from the VRAM tap, so nothing is counted twice.
- **Timing:**
  - Writes are queued during the instruction. `perf_hist_step_` stamps them
    with the instruction's own key and start time, then counts its cycles, then
    applies its calls and returns.
  - The CPU lane is the call tree's innermost node, reported once per instant
    through `cct_switch_hook`. It inherits Phase 3's stack-pointer pairing
    rather than keeping a second call stack.
- **Report:** writes rolled up by (routine, lane) appear in `-profile` output
  and in `x16/capture` as `deviceWrites`.
- **Export:** Chrome trace-event JSON for ui.perfetto.dev and
  `chrome://tracing`:
  - routine spans on CPU and IRQ tracks, nested and balanced;
  - one track per device lane, each write an instant event with value,
    routine, `bank:PC` and source line;
  - a Frames track marking which frames are among the worst.

  Write it with `-profileout` (as `<prefix>.trace.json`), `x16/capture`
  `export.trace`, or the panel's Save trace button.
- **Recorder:** `waterfall`, off by default: `-profilerec tree,waterfall`, or
  the panel checkbox.

**On the Phase 0 workload** (600 frames):
- the top writers were pad scanning on VIA1 (38 per frame), a VRAM copy, map
  writes through VERA's address registers, FX multiplier setup, and palette
  pushes;
- 99% of the exported writes carried a source line.

**Cost:** none measurable. Three interleaved pairs of 1,800 frames gave a
median of 3.47 ms per frame with the waterfall and 3.52 ms with the tree
alone, at about 170 device writes per frame.

**Determinism:** on the scripted BASIC loop, histogram-only runs and
tree-plus-waterfall runs matched in every row and in instruction count.

**Not yet:**
- the PCM FIFO fill-level line;
- reads (joystick polling, SPI reads); only writes are recorded;
- frames marked by hotkey, guest marker or breakpoint;
- the ImGui view (5b) and the Source heat map (5c).

Tested:
- unit tests;
- the DAP testbench, 150/150, including a waterfall capture whose exported
  trace must nest and balance on every track and carry device and IRQ events;
- Linux GCC (WSL, `-O2 -Wall -Werror`) on every touched file.

### Phase 5b: what was built (2026-10-24)

- **A Waterfall tab in the Profile panel** (`src/debug_ui/panels/waterfall_view.cpp`).
  It appears when the capture recorded the waterfall. It is never drawn while
  a capture runs.
  - **Frame picker** over the detailed frames: the recent ring and the worst
    list. It opens on the worst frame and flags truncated frames.
  - **Rows:** nested routine spans on CPU rows (one per call depth) and IRQ
    rows, then one row per device lane that was written, with a tick per
    write, merged per pixel column. The roots `[main]` and `[IRQ]` are not
    drawn.
  - **Axis:** scanlines, with tick spacing that adapts to the zoom, and a
    marker at the end of the 480 visible lines.
  - **Hover** a span or a tick for its routine, `bank:PC`, source line,
    cycles and scanline. Writes also show register or VRAM address and value.
  - **Click** to open the routine or the writing instruction, through
    `debug_ui_request_goto`. The Source panel follows it when there is debug
    info, the Disassembly panel otherwise.
  - **Navigation:** mouse wheel zooms about the cursor, right-drag pans, and
    Fit resets.
  - **Drag** to select a range. A table then lists routines by cycles
    innermost in that range, and the writes per lane. It can zoom to the range.
- **`wf_frame_range()`** in `waterfall.c` does the range arithmetic: cycles per
  innermost node in `[c0, c1)` and the writes there. It is pure, and
  unit-tested with stretches cut at both ends of a range.
- **Session helpers** for the UI: the detailed frame list, plus a key's name,
  `bank:PC` and source line.

Tested:
- unit tests;
- the DAP testbench, 150/150;
- the tab drawn in a Debug build (ImGui asserts on) for two waterfall
  captures, with a selection range forced on so the range table draws too;
- GCC on every touched file.

Hover and click were not exercised by a script: a hidden window takes no
mouse input.

### Phase 5d: every frame, and a way to pick one (2026-10-25)

The 5a recorder kept detail for 38 frames: the last 30 and the 8 worst.
Anything else in a capture could not be looked at, and "worst" was by event
count unless the `budget` recorder happened to be on.

- **Every frame has a summary**: its CPU work, device writes, length and
  output mode. Picks are computed from these, so they cover the whole capture.
- **Detail for every frame, within a budget** (128 MB; 32 MB in the browser
  build). Each frame's events are stored at their own size. Past the budget,
  detail is thinned evenly: every 2nd frame, then every 4th, and so on.
  The 8 highest and 8 lowest frames by each measure are never thinned.
  A frame without detail still has its summary, and the view shows the
  nearest frame that kept its detail, saying so.
- **CPU work is always measured** with the waterfall. The capture arms the
  budget recorder itself when it was not asked for, and disarms it after.
  That costs about 0.12 ms of host time per frame: three interleaved pairs of
  1,800 frames gave medians of 3.71 ms against 3.59 ms for the tree alone.
  Histogram-only and tree-plus-waterfall runs of the scripted BASIC loop still
  match in every row and in instruction count.
- **The Waterfall tab:**
  - *Show* All frames, Worst, Best, Typical (median), 90th percentile or
    Average, *by* CPU work or device writes. `<` and `>` step through the
    frames shown;
  - a strip of every frame in the capture, one bar per pixel column (the
    heaviest frame in it). It marks the frames shown and the frames without
    detail, and a click opens a frame;
  - device rows are fixed for the whole capture, in lane order. A row with no
    writes in this frame is dimmed and stays in place; *Hide empty lanes*
    leaves those rows out.
- **Export:** a trace carries up to 64 frames: the extremes first, then
  frames spread evenly through the capture. Each frame's work and writes are
  in its args.
- **DAP:** `waterfallFrames` counts every frame; `waterfallDetailFrames` counts
  the frames that kept detail.

Tested:
- unit tests, including thinning under a small budget (the extremes keep
  their detail, and the rest share one stride across the whole capture);
- the tab drawn in a Debug build for every Show and Rank-by choice, with the
  budget forced small enough that the summary-only fallback draws;
- GCC on every touched file.

## Phase 6: seeing who calls what

Every view here is a different summary of the call tree that Phase 3 already
records, so none of them records anything new or costs the running machine
anything. In order:

1. **Flame graph (done, 2026-10-25).** A Flame graph tab in the Profile panel
   (`src/debug_ui/panels/flame_view.cpp`).
   - **Layout:** each routine is a box as wide as its total time, and the
     routines it called sit beneath it, widest first. The gap at a box's right
     is its own time.
   - **Navigation:** click to zoom into a box, with a breadcrumb trail back to
     "Everything". Double-click opens the source.
   - **Colour** follows the routine, so it reads the same under every caller.
   - **Search** lights up matching boxes, dims the rest, and reports the
     matches' share of the recording, each counted once even when nested.
   - **Tooltips:** total and own time as a percentage and in ms per frame,
     calls and the slowest call, `bank:PC`, source line, and the call path.
   - **Cost:** only boxes at least a pixel wide are drawn, so a 1,000-node tree
     costs what is visible.
   - This replaces speedscope for everyday use; the `.folded` export stays for
     other tools.
2. **Callers and callees of one routine (done, 2026-10-25).** A "Callers &
   callees" tab (`src/debug_ui/panels/callers_view.cpp`).
   - **Layout:** the chosen routine's callers on the left, each with the share
     of its total time it caused. On the right, "its own code" and its
     callees, each with the share they cost it. Both sides add up to its total.
   - **Navigation:** click a neighbour to move to it, with back and forward.
     Double-click opens the source.
   - **Opening it:** click a routine in the Routines tab to select it, or
     right-click in Routines, Call tree or Flame graph and choose "Callers &
     callees". A picker with a filter lists every routine that was called.
   - **The arithmetic** is `prof_report_routine_view()` in `profile_report.c`.
     It merges every outermost call of the routine across all paths, and folds
     recursion in, so a routine calling itself is neither caller nor callee and
     its time counts once.
   - **Tested:** unit tests check both invariants (callers sum to the total;
     own plus callees sum to the total) on a tree with mutual recursion,
     direct recursion and an interrupt caller.
3. **Neighbourhood call graph.** Boxes and arrows two levels each way around the
   selected routine. Box size is own time; arrow width is time or calls along
   that edge. Click a box to re-centre on it.
4. **Source annotations (Phase 5c).** A gutter with each line's share of time,
   and a "called from" list on each routine's first line.

Steps 2–4 share one piece of data: edges merged by routine (caller routine →
callee routine, with calls and total time), built once per report.

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
- Do we want per-frame call-tree snapshots, for questions like "why was frame
  812 slow"? Or are session totals plus worst call and worst frame enough?
- Is 65C816 native-mode code (JSL/RTL, 24-bit PCs) in scope now, or only
  6502/65C02 X16 programs?
