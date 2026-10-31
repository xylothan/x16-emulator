# DAP coverage of the ImGui debugger

Goal: anything a person can see or do in the ImGui debugger can also be read or
done over DAP, so scripted tooling (`tools/x16dbg`, `testbench/test_dap.py`,
agents) sees what a person sees.

This page is an audit and a plan. Nothing here is implemented yet.

Audited against `origin/main` at 72c147b: every panel in
`src/debug_ui/panels/`, the menus and pages in `src/debug_ui/`, the DAP server
in `src/debug_server.c`, and the client in `tools/x16dbg`.

## Legend

The **Server** and **x16dbg** columns are separate because the server already
answers many requests that x16dbg never sends.

| Mark | Meaning |
|---|---|
| ✓ | Reachable. The cell names the request. |
| ◐ | Partial. Raw bytes only with no decoding, or some fields missing. |
| ✗ | Not reachable. |
| — | Nothing needed. |

Presentation-only state is noted once under each panel and gets no rows:
zoom, layout, column widths, follow-PC, colours, tooltips and explanatory text.

## What the server exposes today

Standard DAP requests:

* `initialize`, `launch`, `attach`, `configurationDone`, `disconnect`, `terminate`, `restart`
* `threads`, `stackTrace`, `scopes`, `variables`, `setVariable`
* `continue`, `next`, `stepIn`, `stepOut`, `pause`, `goto`, `gotoTargets`, `stepInTargets`
* `setBreakpoints`, `setFunctionBreakpoints`, `setInstructionBreakpoints`, `breakpointLocations`
* `dataBreakpointInfo`, `setDataBreakpoints`
* `readMemory`, `writeMemory`, `disassemble`, `loadedSources`, `source`, `evaluate`

`scopes` returns four groups:

* Registers. Includes P, RAM bank and ROM bank.
* Virtual Regs (R0–R15).
* Zero Page.
* Stack.

The `memoryReference` of `readMemory` and `writeMemory` takes one of two forms:

* A CPU address, 24-bit on gen2. Banked windows follow the *current* mapping.
* `vram:` followed by a 17-bit VERA address.

`evaluate` understands a small console language:

* `$addr`, plus register names and `regs_all`.
* `bp_add`, `bp_remove`, `bp_list`, `bp_clear`.
* `watch_add`, `watch_remove`, `watch_list`, `watch_clear`.
* `vram ADDR [N]`, `vera_reg` and `reset`.

Custom `x16/*` requests:

* `x16/registers`: CPU state, banks, KERNAL r0–r15 and decoded VERA registers.
* `x16/sendKey`, `x16/type`, `x16/joystick`.
* `x16/perfStats`, `x16/perfConfig`, `x16/perfHist`, `x16/hostPerf`.
* `x16/capture`.
* `x16/listBreakpoints`. Lists every owner's breakpoints and watchpoints with hit counts and conditions.
* `x16/spriteMuxStats`, `x16/spriteMuxConfig`.

**x16dbg sends only a subset of these.** It uses:

* `evaluate`, `readMemory`/`writeMemory`, `scopes`/`variables` and `setVariable`.
* The stepping requests.
* `x16/joystick` and `x16/capture`.

It has commands for `step`, `continue`, `break`, `status`, `reset`, `regs`,
`setreg`, `mem`, `setmem`, `vram`, `setvram`, `joy`, `bp` and `profile`.

## Per-panel audit

### CPU (`cpu_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Registers: A, X, Y, SP, PC, D, DBR, PBR, E | `regs` | ✓ `variables` (Registers), `x16/registers` | ✓ `regs`, `setreg` | — |
| Status flags | `regs.status` | ✓ P in Registers, `x16/registers.cpu.status` | ◐ `regs` shows the byte | No per-flag decoding in x16dbg |
| Virtual regs R0–R15 | ZP $02–$21 | ✓ `variables` (Virtual Regs), `x16/registers.kernal` | ✗ | Add to x16dbg `regs` |
| Stack view | `debug_ui_read6502` | ✓ `variables` (Stack), `readMemory 0x0100` | ◐ `mem` | — |
| Interrupt vectors (native and emulation) | vectors read from ROM | ◐ `readMemory 0xFFE4` and up, raw only | ◐ `mem` | No decoded vector table |
| VERA IRQ state and the next IRQ ("VSYNC/LINE in N cyc") | `video_get_irq_state`, `video_next_irq` | ◐ `x16/registers.vera` has IEN, ISR and IRQ line, but not the next IRQ | ✗ | Next-IRQ prediction is missing |
| Watch list (address, length, hex edit) | debug_ui state, `debug_ui_write6502` | ✓ `readMemory`/`writeMemory` reach the values | ✓ `mem`/`setmem` | The list itself is UI state. — |
| "Widen now (REP #$30)" action | writes P | ✓ `setVariable` on P | ✓ `setreg` | — |

### Disassembly (`disasm_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Disassembly around PC or an address | `code_map_disasm_*` | ✓ `disassemble` | ✗ | No x16dbg command |
| Labels in the listing | `dbg_info_addr_to_label` | ◐ `disassemble` carries source location but no label | ✗ | Add `symbol` to each instruction (DAP field) |
| Goto symbol | `debug_ui_label_to_addr` | ✗ no symbol lookup | ✗ | See P5 |
| Toggle breakpoint, Run to here | `debug_bp_*`, goto | ✓ `setInstructionBreakpoints`, `goto` | ◐ `bp add` | — |

UI only: Follow PC, the cursor.

### Source (`source_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Source files from the .dbg | `dbg_info_file_*` | ✓ `loadedSources`, `source` | ✗ | — |
| Current line | `dbg_info_addr_to_source_banked` | ✓ `stackTrace` frame source and line | ◐ `status` | — |
| Breakpoint on a line, Run to here | `debug_ui_source_to_addr` | ✓ `setBreakpoints`, `breakpointLocations`, `goto` | ✗ | — |
| Step by line | debugger step mode | ◐ `next` and `stepIn` use the debugger's own auto mode and ignore the DAP `granularity` argument | ◐ | Honour `granularity` |
| Last stop reason | `debug_server_last_stop_reason` | ✓ `stopped` event | ✓ | — |
| Load a .dbg file, Reload, the "Use this" candidates | `dbg_info_load`, `dbg_info_scan_dbg_files` | ◐ only as a `launch`/`attach` argument | ✗ | No runtime load. See P5 |
| Source folders | `source_folders.cpp` | ✗ | ✗ | A UI setting, low value |

UI only: tabs, Follow PC, goto line.

### Symbols (`symbols_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Symbol table (name, address, current value) | `debug_ui_symbol_at`, `dbg_info_symbol_count` | ✗ | ✗ | See P5 |
| Go to, Toggle breakpoint, Run to here | | ✓ through `setFunctionBreakpoints` by name and `goto` | ◐ | — |

### Call stack (`callstack_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Frames with label and source | stack unwind, `dbg_info_*` | ✓ `stackTrace` | ◐ `status` shows the top frame | x16dbg has no full backtrace command |

### Breakpoints (`breakpoints_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Every breakpoint, any owner (address, bank, enabled, hits, condition, ignore count) | `debug_bp_at` | ✓ `x16/listBreakpoints` | ◐ `bp list` uses `evaluate bp_list`, which gives the address and "(off)" only | Switch x16dbg to `x16/listBreakpoints` |
| Add and remove with a bank pin | `debug_bp_*` | ✓ `evaluate bp_add/bp_remove bb:aaaa`, `setInstructionBreakpoints` | ✓ `bp add/remove` | — |
| Condition and hit count | | ✓ `setBreakpoints` condition and hitCondition | ✗ | — |
| Enable/disable one breakpoint, Enable All, Disable All, Reset hits | `enabled`, hit counters | ✗ DAP owns only its own breakpoints and cannot toggle a breakpoint set with F9 or `-bp` | ✗ | See P6 |
| Watchpoints (address, length, value filter, active) | `debug_wp_*` | ✓ `setDataBreakpoints` with a value condition, `evaluate watch_*`, listed by `x16/listBreakpoints` | ✗ | Enable/disable is missing. See P6 |
| Clear All | | ✓ `evaluate bp_clear`, `watch_clear` | ✓ `bp clear` | — |

### Memory (`memory_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| CPU tab: view and edit | `debug_ui_read6502`/`write6502` | ✓ `readMemory`/`writeMemory`, 24-bit on gen2 | ✓ `mem`/`setmem` | — |
| Banked tab: **any** RAM bank at $A000 or ROM bank at $C000, whatever is mapped | banked `debug_read6502(addr, bank, x16Bank)` | ✗ `readMemory` always passes `USE_CURRENT_X16_BANK` | ✗ | **The largest gap.** Today another bank can be read only by writing $00/$01 first, which changes machine state. See P2 |
| VRAM tab | `video_space_read`/`write` | ✓ `vram:` reference | ✓ `vram`/`setvram` | — |
| Search: Find Next/Prev, all banks | panel search | ✗ | ✗ | See P9 |
| Watch from selection | `debug_ui_request_watch` | ✓ `setDataBreakpoints` | ✗ | — |

UI only: highlights, selection, the Sync K and Sync banks buttons.

### VERA (`vera_panel.cpp`, `sprite_multiplex_tab.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Registers tab: raw and decoded | `video_read(…, true)`, `vera_reg_name` | ✓ `x16/registers.vera` decoded, `evaluate vera_reg` raw | ✗ | x16dbg has no command |
| Current scanline, layer line state, active layer size | `video_get_scanline_count`, `video_get_layer_line_state`, `video_get_active_layer_size` | ✗ | ✗ | See P3 |
| Palette | VRAM $1FA00 | ◐ `readMemory vram:1FA00`, raw only | ◐ `vram` | Decoded RGB is missing |
| Sprites table (128 attributes, decoded) | VRAM $1FC00 | ◐ `vram:1FC00`, raw only | ◐ | Decoding is missing |
| Tiles, Tilemap, Bitmap viewers | VRAM, decoded through the layer config | ◐ raw VRAM plus `x16/registers` layer config | ◐ | Rendered images are out of scope. A decoded tilemap cell (tile, palette offset, flip) would be useful |
| Multiplex tab: per-frame summary | `sprite_trace` | ✓ `x16/spriteMuxStats` and `Config` | ✗ | — |
| Multiplex: Effective sprites list, raster ribbon, frame ghost, render time | `sprite_trace` per-sprite and per-line data | ◐ summaries only | ✗ | See P3 |

UI only: zoom, the tile and page stepping buttons, Follow raster, Hide disabled.

### I/O (`io_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| VIA1 and VIA2: registers, IRQ flags and enables, port pins | `via_debug_get_state`, `via_read(…, true)` | ◐ `readMemory 0x9F00`/`0x9F10`. The debug read has no side effects, but returns raw bytes only | ◐ `mem` | No decoded state (IFR/IER, pins, timers). See P4 |
| I2C bus | `i2c_debug_get_state` | ✗ | ✗ | See P4 |
| SMC | `smc_debug_get_state` | ✗ | ✗ | See P4 |
| RTC | `rtc_debug_get_state` | ✗ | ✗ | See P4 |
| Joysticks: decoded per-port state | `joystick_debug_get_state` | ◐ `x16/joystick` reports the held set for a slot it drives | ✓ `joy` | No read-only view of every port, including undriven ones. See P4 |
| IEC serial lines (ATN, CLK, DATA in and out) | `serial_port` | ✗ | ✗ | See P4 |
| Files > Host filesystem: open channels and history | `ieee_debug_get_state` | ✗ | ✗ | See P4 |
| Activity: I/O trace log, capture switch, device filter, dropped count | `io_trace_*` | ✗ | ✗ | See P7 |
| **SD Card status, SD access history, Files > SD image** | | in progress | in progress | **In progress on branch `io-sd-views`.** Not covered here |

UI only: auto-scroll, explanatory text.

### Audio: PSG, FM and PCM (`psg_panel.cpp`, `fm_panel.cpp`, `pcm_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| PSG registers | VRAM $1F9C0–$1F9FF | ◐ `vram:1F9C0`, raw only | ◐ `vram` | — |
| PSG voices decoded (frequency, waveform, volume, pan, noise state) | `psg_debug_get_channel`, `psg_debug_get_noise_state` | ✗ | ✗ | See P8 |
| YM2151 registers ($00–$FF) | `ym_reg` (write-only on hardware) | ✗ | ✗ | See P8 |
| YM channels and operators decoded, algorithm | `ym_reg` | ✗ | ✗ | See P8. The client can decode the registers |
| PCM status (rate, format, volume, FIFO level) | `pcm_debug_get_state` | ◐ `x16/registers.vera.audioCtrl/audioRate` | ✗ | See P8 |
| PCM FIFO bytes | `pcm_debug_peek_fifo` | ✗ | ✗ | See P8 |
| Scopes (waveforms) | `*_debug_scope_read` | ✗ | ✗ | Out of scope. They are pictures |

### Performance (`perf_panel.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Statistics, history, breakdown | `perf_budget_*` | ✓ `x16/perfStats` | ✗ | x16dbg has no command. See P1 |
| Zones: add, remove, clear, from symbol | `perf_budget_zone_*` | ✓ `x16/perfConfig.zones` | ✗ | "From symbol" needs P5 |
| Settings: target fps, idle detection, history size, Reset | `perf_budget_set_*` | ✓ `x16/perfConfig` | ✗ | — |
| Bandwidth: per-frame totals | `vera_bandwidth_last_frame` | ✓ `x16/perfStats.bandwidth` | ✗ | — |
| Bandwidth: per-line clocks | `vera_bandwidth_last_lines` | ✗ | ✗ | See P3 |
| Host time | `host_prof` | ✓ `x16/hostPerf` | ✗ | — |

### Profile (`profile_panel.cpp`, `flame_view.cpp`, `callers_view.cpp`, `waterfall_view.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| Start recording: frames, wait for marker, recorders | `profile_run`, `profile_capture` | ✓ `x16/capture.start` | ✓ `profile rec` | — |
| Routines table | `profile_session_report` | ✓ `x16/capture` `top` rows | ✓ `profile top` | — |
| Call tree, Callers & callees | `profile_cct`, `profile_session_*` | ◐ the folded and JSON exports carry the data | ◐ `profile folded` and `json` | No direct callers or callees query for one routine. See P10 |
| Flame graph | `profile_flame_draw` | ◐ the folded export | ◐ | — |
| Waterfall (per-frame timeline) | `waterfall` | ✗ | ✗ | See P10 |
| Frame budget summary | `profile_session_budget_summary` | ◐ `x16/perfStats` while live | ✗ | — |

### Menus and pages (`debug_ui.cpp`, `debug_info_page.cpp`, `export_page.cpp`, `profile_files.cpp`, `debug_ui_settings.cpp`)

| Item | ImGui source | Server | x16dbg | Gap |
|---|---|---|---|---|
| File > Open profile run, Recent runs | `profile_files` | ✗ | ✗ | See P10 |
| File > Save profile run, Save as | `profile_run` | ✗ | ✗ | See P10 |
| File > Export profile (folded, JSON, trace.json) | `profile_report` | ◐ folded and JSON through `x16/capture` | ◐ | trace.json is missing |
| File > Load debug info | `dbg_info_load` | ◐ only at `launch`/`attach` | ✗ | See P5 |
| File > Export folders | `export_paths` | ✗ | ✗ | A UI setting. — |
| System > Reset | `machine_reset` | ✓ `evaluate reset`, `restart` | ✓ `reset` | — |
| System > Trigger IRQ, Trigger NMI | | ✗ | ✗ | See P6 |
| System > Settings (I/O trace capture, idle detection, UI scale and so on) | `debug_ui_settings` | ◐ idle detection through `x16/perfConfig` | ✗ | I/O trace capture: see P7. The rest is UI only |
| View, Layout, Help | | — | — | UI only |

## Proposed additions, in priority order

The proposals follow three rules:

* **Naming.** Each new request is named `x16/<noun>`. It returns the existing
  `*_debug_get_state` struct as JSON, so the request adds no new emulator logic.
* **No side effects.** Every new reader goes through those accessors, or through
  `debugOn=true` reads. Raw device reads have side effects: a VIA register read
  clears flags, and the PCM path can run `audio_render`. The DAP view must be as
  passive as the ImGui view.
* **Arming.** A feature that has to be armed follows the existing
  `perfStats`/`perfConfig` pattern: polling arms it, and a `…Config` request sets
  it up.

### P1. Expose existing server requests in x16dbg (client only)

No server work. The commands:

| x16dbg command | Request |
|---|---|
| `x16dbg bps` | `x16/listBreakpoints`. Replaces `evaluate bp_list` in `bp list` |
| `x16dbg dis [addr] [n]` | `disassemble` |
| `x16dbg bt` | `stackTrace` |
| `x16dbg vera` | `x16/registers.vera` |
| `x16dbg perf [--frames N]`, `perf zone add/rm`, `perf config …` | `x16/perfStats`, `x16/perfConfig`, `x16/hostPerf` |
| `x16dbg mux` | `x16/spriteMuxStats` |
| `x16dbg key …`, `x16dbg type "…"` | `x16/sendKey`, `x16/type` |

Also extend `regs` to print R0–R15 and the decoded flags.

### P2. Banked memory references

Add two `memoryReference` prefixes beside `vram:`, accepted by both
`readMemory` and `writeMemory`:

```
rambank:05:A000      RAM bank $05, address $A000-$BFFF
rombank:03:C000      ROM bank $03, address $C000-$FFFF
```

The read goes through `debug_read6502(addr, 0, bank)`, the same call the Banked
tab makes. It must not touch $00 or $01.

The prefix is explicit because `05A000` or `05:A000` would be confused with a
gen2 24-bit address. The response echoes the reference in the same form, as
`vram:` does, so a client can page through.

x16dbg: `mem --bank 5 A000 100` and `mem --rom 3 C000 40`.

### P3. `x16/vera`: decoded video state

```
request  { "sprites": bool, "palette": bool, "lines": bool }
response { "scanline", "irq": { "ien", "isr", "line", "next": { "source": "vsync|line", "cycles" } },
           "layers": [ { "w", "h", "lineState": {…} } ],
           "palette": [ "#rrggbb", ×256 ],
           "sprites": [ { "index", "addr", "mode", "x", "y", "z", "w", "h", "hflip", "vflip", "palOffset" } ],
           "bandwidthLines": [ clocks per line ] }
```

* Sources: `video_get_*`, `video_next_irq`, `vera_bandwidth_last_lines`.
* The optional parts are off by default, to keep the poll small.
* Per-sprite multiplex data (effective sprites, the ribbon) goes in
  `x16/spriteMuxStats` with `{ "detail": true }`.

x16dbg: `vera`, `vera sprites`, `vera palette`.

### P4. `x16/io`: device state

```
request  { "devices": ["via1","via2","i2c","smc","rtc","joystick","iec","hostfs"] }   // default: all
response { "via1": { "regs":[16], "ifr", "ier", "pa", "pb", "ddra", "ddrb", "t1", "t2", … },
           "i2c": {…}, "smc": {…}, "rtc": {…},
           "joystick": [ { "port", "connected", "driven", "buttons" } ],
           "iec": { "in": { "atn","clk","data" }, "out": { "clk","data" } },
           "hostfs": { "channels": [ … ], "history": [ … ] } }
```

* Each block is the matching `*_debug_get_state` struct, serialised.
* Leave room for the `sd` key that branch `io-sd-views` is adding, so SD fits
  in with no reshaping.
* The joystick block reports undriven ports as well as driven ones. That makes
  the "reads high forever" rule in `AGENTS.md` testable.

x16dbg: `io [device…]`.

### P5. `x16/debugInfo`: load .dbg files and look up symbols

```
request  { "load": "path.dbg" }                 → { "loaded": bool, "files", "symbols" }
request  { "scan": true }                       → { "candidates": [ paths ] }
request  { "symbols": "filter", "limit": n }    → { "symbols": [ { "name", "addr", "bank" } ] }
request  { "lookup": "name" | "addr": n }       → { "name", "addr", "bank", "source", "line" }
```

This unblocks the disassembly "Goto symbol", the Symbols panel and perf
"Zone from symbol".

x16dbg: `dbg load`, `sym [filter]`, `sym ADDR`.

### P6. Machine actions and breakpoint control for every owner

```
x16/interrupt   { "line": "irq" | "nmi" }
x16/breakpoint  { "pc", "bank", "x16Bank", "enabled"?: bool, "resetHits"?: true }
                { "all": true, "enabled": bool }                // Enable All / Disable All
x16/watchpoint  { "addr", "len", "active"?: bool }
```

A person's F9 breakpoint can be switched on or off but not deleted, which keeps
the existing per-owner rules intact. `bp_remove` is unchanged.

x16dbg: `irq`, `nmi`, `bp on|off ADDR`, `bp reset-hits`.

### P7. `x16/ioTrace` and `x16/ioTraceConfig`: the Activity log

```
x16/ioTraceConfig { "capture": bool, "devices": [ … ], "clear": true }
x16/ioTrace       { "since": seq, "limit": n }
                  → { "entries": [ { "seq", "cycle", "frame", "pc", "device", "reg", "rw", "value" } ],
                      "dropped", "capacity", "next": seq }
```

* A `since` cursor lets a client tail the log with no duplicates.
* Coordinate the entry fields with `io-sd-views`, which is changing the
  Activity columns.

x16dbg: `iotrace on|off [devices]`, `iotrace [--follow]`.

### P8. `x16/audio`: PSG, YM and PCM state

```
request  { "psg": bool, "ym": bool, "pcm": bool, "fifo": n }
response { "psg": { "voices": [ { "freq", "hz", "wave", "pw", "vol", "pan" } ×16 ], "noise": {…} },
           "ym":  { "regs": [256] },
           "pcm": { "rate", "format", "volume", "fifoLevel", "fifo": [ first n bytes ] } }
```

* The YM block ships raw registers. Decoding channels and operators is left to
  the client, as the FM panel does locally.
* Sources: `psg_debug_*`, `ym_reg`, `pcm_debug_get_state`, `pcm_debug_peek_fifo`.
  Use these only; never `audio_render`.

x16dbg: `audio [psg|ym|pcm]`.

### P9. `x16/findMemory`: memory search

```
request  { "pattern": "A9 00" | "text", "from": ref, "backward": bool, "allBanks": bool }
response { "found": ref | null }
```

The reference uses the P2 forms. x16dbg: `find PATTERN [--all-banks]`.

### P10. Profile runs and per-routine queries

```
x16/profileRun   { "open": path } | { "save": path } | { "export": "folded|json|trace", "path": … }
x16/profileQuery { "routine": name|key, "callers": bool, "callees": bool }
                 { "waterfall": { "frame": n } }
```

This covers File > Open, Save and Export, and the Callers & callees and
Waterfall tabs.

x16dbg: `profile open|save|export`, `profile callers NAME`, `profile frame N`.

### Smaller items

* Make `next` and `stepIn` honour the DAP `granularity` argument (`instruction`
  or `line`), so a client can choose between Step and Step by line.
* `x16/registers`: add decoded interrupt vectors, as shown in the CPU panel's
  Interrupts section.

## Out of scope

* Rendered images: the tile, tilemap and bitmap viewers, scopes, and the
  flame graph as a picture. The data behind each of these is covered above.
* Pure UI state: layout, zoom, colours, source folders, export folders and UI
  scale.
* SD card status, SD access history and the SD image file index, which branch
  `io-sd-views` is adding.
