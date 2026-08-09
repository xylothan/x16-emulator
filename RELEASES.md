
<p align="center">
  <img src="./.gh/logo.png" />
</p>

## Releases

**X16Emu ADD** is the Advanced Disassembler and Debugger, a fork of the
[official Commander X16 emulator](https://github.com/X16Community/x16-emulator).

Releases are named `R<major>.<nnn>`:

* **`R<major>`** is the official Commander X16 release the build tracks. That half of the number
  is upstream's, and so are its release notes — they are not reproduced here. Read them at
  [X16Community/x16-emulator/releases](https://github.com/X16Community/x16-emulator/releases),
  e.g. [r49 ("Pyrite")](https://github.com/X16Community/x16-emulator/releases/tag/r49).
* **`nnn`** counts the ADD builds on top of that release. Those are the changes listed below.

An `R49.nnn` build is the upstream R49 emulator plus everything in the entries below, and it
wants an R49 `rom.bin` — which ships in the release package.

### R49.001

The first ADD release. Everything here is new relative to the official R49 emulator.

**Graphical debugger (`-imgui`)**

* New Dear ImGui debug window with a dockable, rearrangeable layout that is saved between runs
* Disassembly panel with effective-address tooltips, run-to-here and breakpoint toggling
* CPU panel: registers with 65C816-aware widths, decoded flags, the stack, the X16 virtual
  registers R0–R15, and user-defined address watches
* Memory panel: hex editor over CPU space, banked RAM and VRAM, with range selection, hex/ASCII
  search, changed-byte highlighting, and break-on-write straight from a selection
* Source panel: original cc65 source in tabs, current line highlighted and centred on each stop,
  breakpoints settable before the PC ever reaches the code
* VERA panel: palette, tile, sprite, bitmap and tilemap viewers, plus all 32 registers decoded
  per scanline so raster splits are visible rather than flattened
* PSG, YM2151 and PCM panels with scope traces that keep drawing while the machine is paused
* Symbols, Call Stack and Breakpoints panels; the call stack names frames by nearest label, so
  code without debug info still reads sensibly
* Toolbar with run/step controls, an absolute clock-speed readout (red above native speed) and a
  warp toggle; status line showing run state, IRQ nesting depth, 24-bit PC, cycles and instructions

**Remote debugging (`-debugport`)**

* New Debug Adapter Protocol server over TCP, default port 9009, for VS Code, Visual Studio or
  any other DAP-compliant client
* Breakpoints, stepping, stack traces, scopes, variables, memory read/write and disassembly
* Conditional breakpoints on registers and memory (`A == $05`, `byte[$1234] != 0`), bank-pinned
  conditions (`bank == 2`), and hit counts
* Expression evaluation covering registers, memory, cc65 labels and equates, VRAM, and VERA
  registers — including `vera_line`, which reports the registers that actually rendered a given
  scanline
* Scriptable keyboard, text and joystick input, making automated regression runs practical
* `tools/x16dbg`, a .NET command-line DAP client with one-shot and interactive modes

**Source-level debugging (`-dbgfile`, `-srcpath`)**

* cc65 `.dbg` parsing: addresses map back to source files, lines, labels and equates
* `.dbg` files auto-load alongside the program they belong to, including for modules the running
  program `LOAD`s at runtime, so debugging follows execution into overlays
* Breakpoints are invalidated when a module unloads and re-resolved when one loads back in
* Banked code in `$A000`–`$BFFF` is attributed to the correct RAM bank at runtime, so source
  mapping and breakpoints stay correct across bank switches

**Debugger capacity**

* Memory write watchpoints raised from 16 to 64
* 65C816 / GS support throughout: 24-bit bank:address browsing and mode-correct register widths

**Windows**

* Statically linked MSVC builds for x64, x86 and ARM64 — a single self-contained `x16emu.exe`
  with no SDL2, zlib or Visual C++ redistributable to install
* Separate MIDI builds shipping FluidSynth, for those who want it
* The emulator keeps running and painting while its window is dragged or resized, instead of
  freezing for the duration

**Build and release**

* Every push is built for all platforms; pushing an `R<major>.<nnn>` tag publishes the release
  automatically, and the default branch keeps a rolling `dev` prerelease
* ROM images are fetched with a fallback to the latest release when the upstream build artifact
  has expired

**Known limitations**

* The debugger and DAP server are experimental — expect keys, layouts and DAP details to change
* The WebAssembly build includes neither the ImGui debugger nor the DAP server
* MSVC builds have no `-trace`; use a `-mingw` package if you need it
* Upstream's original `-debug` debugger is still present but is not extended by ADD
