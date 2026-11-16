# Working in this repository

Notes for anyone — human or agent — making changes here. Everything below was
learned by hitting it, so it is worth reading before the first build.

## Branching

**Never push to `main`.** Open a pull request.

`main` carries a ruleset that requires changes to arrive through a PR. Some
accounts can bypass that rule, so a direct push may appear to succeed — do not
rely on it. A second ruleset blocks force-pushes and deletion, and *that* one has
no bypass, so a direct push to `main` cannot be undone afterwards.

Work on a branch, push the branch, open a PR.

## Building on Windows

Use `build.ps1` at the repo root:

```powershell
.\build.ps1                      # Debug build of the emulator
.\build.ps1 -Config Release
.\build.ps1 -Tests               # build and run the unit tests
.\build.ps1 -Run                 # build, then launch
.\build.ps1 -DapTest -FetchRom   # download a ROM, boot, run the DAP testbench
.\build.ps1 -Dbg                 # build tools/x16dbg, the .NET DAP client
```

It finds Visual Studio, CMake, vcpkg and the right preset itself. Two failures it
exists to prevent, both of which report something misleading:

* The `cmake` on `PATH` is often older than the installed Visual Studio and does
  not know its generator. `cmake --preset vs2026` then prints two screens of
  generator names without mentioning that the one asked for is missing. Visual
  Studio ships its own CMake, which always knows; `build.ps1` prefers it.
* A running emulator holds its own `.exe` open, so the link step fails with a
  bare `LNK1104` naming the output file and nothing else. `build.ps1` detects
  this and says so; `-Force` stops the emulator first.

**Run games on the Release build.** Debug is for debugging the emulator's own
code. It does not keep up with real time under load, `-midline-effects` above
all, so sound crackles and frame timings and profiles come out wrong. The ImGui
debugger works the same in Release.

## Tests

`.\build.ps1 -Tests` runs the unit suite.

**11 tests do not compile under MSVC, and that is pre-existing** —
`test_vera_*`, `test_memory_banking`, `test_debugon_contract`,
`test_debug_write_path` and `test_watchpoint_purity`. SDL's headers `#define main
SDL_main`, which collides with their `int main(void)` and raises C4026, fatal
under `/WX`. They report as *Not Run* rather than failing. The other 25 pass and
are the ones to watch; do not go hunting for a regression you did not cause.

**A green local build is necessary, never sufficient.** Because those 11 never
compile on Windows, their *link step never runs here either* — and four of them
(`memory_banking`, `debugon_contract`, `debug_write_path`, `watchpoint_purity`)
link the real `src/memory.c` against `tests/support/fake_devices.c`. So anything
`memory.c` newly calls must also exist in `fake_devices.c`, and on Windows only
CI can tell you it does not.

Two Linux-only link failures have already shipped this way: `lrintf`/`fabsf` out
of `perf_budget.c` (MSVC intrinsics, glibc libm symbols), and a missing
`video_set_debug_write` stub after `memory.c` started calling it. Both times the
code was correct on Windows, both times the failure was one push away from being
visible, and both times **completion was claimed before the push**. That is the
actual lesson, and it is broader than the MSVC gap:

> Push and let CI answer before claiming a result.

Broader still, because CI is only the commonest case: **check what is true where
the artifact will live, not where you are standing.** A branch is judged by CI,
not by your worktree. A file that ships to `origin/main` is read against
`origin/main`, not against whatever your local `main` happens to have merged.

The paragraph below is the third instance of that same error, kept rather than
quietly corrected: it was first drafted citing `perf_budget.c` and
`vera_bandwidth.c` as though they existed, because they existed *where the author
was standing* — an unmerged local `main` — while on `origin/main` they did not.

One live tripwire of exactly this shape, once the performance work lands (PRs #79
and #80, which is where `perf_budget.c` and `vera_bandwidth.c` come from):
`src/vera_bandwidth.c` is deliberately *not* in those four link sets, and does
not need to be while `memory.c` reaches VERA only through `video_write()` and
`video_set_debug_write()`. The day anything in `memory.c` calls
`vera_bandwidth_*` directly, all four break together.

The DAP testbench needs a running emulator:

```powershell
.\build.ps1 -DapTest
```

Doing it by hand needs two things `build.ps1` handles: give the machine ~25s to
reach BASIC before asserting (the DAP port opens long before the ROM finishes
booting, and early assertions otherwise race it), and pass a port —
`python testbench/test_dap.py <port>` — because anything else on the machine that
speaks DAP will connect to a server on the default 9009 and take the session down
mid-suite.

## Save states: layout and machine versions

A save state names two versions from `src/savestate.c`, and a build treats
them differently. Training runs keep hundreds of states across emulator
updates on the strength of this, so get them right:

* **`SAVESTATE_LAYOUT_VERSION`: what the bytes mean.** Another layout is
  refused. Bump it when a chunk gains, loses or reorders a field, or a value
  changes meaning, and then empty `legacy_builds`. Prefer a new *chunk* version
  whose loader still reads the old one, filling new fields with defaults: then
  old states keep loading and the layout version stays.
* **`SAVESTATE_MACHINE_VERSION`: how the machine runs.** Bump it in any change
  that alters what the emulated machine does (CPU, memory, VERA, audio chips,
  VIAs, SMC, timing of any of them). A state from another machine version is
  refused by `x16/loadState` unless the caller passes `allowBehaviourChange`,
  because it will not run on as it did when saved; the error body still carries
  `machine` so the caller can see why.
* **Leave both** for changes to the debugger, DAP, the window, keys, or the
  sound sent to the host, which is not machine state.
* **Never save a struct whole.** Its bytes would depend on the compiler. Write a
  byte image with explicit offsets, as `regs_to_image()` (`cpu/fake6502.c`) and
  `layer_image()` (`video.c`) do, with a `_Static_assert` on the struct's size,
  so that a new field cannot slip past its image.
* The toolchain is in the ID as well, since floating-point code can run
  differently under another compiler. It is reported (`toolchainChanged`), never
  refused.
* `testbench/test_machine_version.py` replays fixed workloads from power-on and
  compares each state's chunk list and its hashes with
  `testbench/machine_golden.json`. CI runs it on Linux (the `machine-version`
  job):
  * chunks that change under the same layout version mean a layout bump was
    missed;
  * hashes that change under the same machine version mean a machine bump was
    missed.

  It also loads one state under edited IDs to check what each version decides.
  The goldens depend on the ROM, so the job pins one: x16-rom release `r49`,
  checked by SHA-256. To run it or regenerate:

  ```powershell
  python testbench/test_machine_version.py <x16emu> <r49 rom.bin>            # check
  python testbench/test_machine_version.py <x16emu> <r49 rom.bin> --update   # rewrite the goldens
  ```

  When it fails, the job's log ends with the JSON this build gives, ready to
  commit once you have bumped the version.

## Poking at a running emulator

`tools/x16dbg` is a .NET DAP client:

```powershell
dotnet run --project tools/x16dbg -- vram 1B000 20      # VERA video RAM
dotnet run --project tools/x16dbg -- joy 1 right a      # hold buttons
dotnet run --project tools/x16dbg                       # interactive
```

Each **one-shot** command is its own debug session, and the emulator deliberately
resumes when a session that paused it disconnects — so a headless machine cannot
be left halted with nobody able to restart it. Anything that depends on staying
stopped (`break`, `setreg`, stepping) therefore belongs in interactive mode, where
one session stays open. Memory, VRAM and joystick state persist either way.

## Two things that are not where you would guess

* **VRAM is not in the CPU map.** The CPU reaches it only through VERA's data
  port, a byte at a time. Over DAP, use a `vram:` prefix on `memoryReference`;
  `readMemory 0x1B000` and `readMemory vram:1B000` are different memories at the
  same number.
* **A controller port that nothing drives reads high forever**, while a driven one
  runs its 16 bits out and then reads 0. That difference is how the KERNAL tells a
  connected pad from an empty socket, so `JOY(1)` is `$FF` when empty and `$00`
  when connected with nothing held. Code touching `joystick.c` must preserve it.
