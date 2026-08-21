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

## Tests

`.\build.ps1 -Tests` runs the unit suite.

**11 tests do not compile under MSVC, and that is pre-existing** —
`test_vera_*`, `test_memory_banking`, `test_debugon_contract`,
`test_debug_write_path` and `test_watchpoint_purity`. SDL's headers `#define main
SDL_main`, which collides with their `int main(void)` and raises C4026, fatal
under `/WX`. They report as *Not Run* rather than failing. The other 25 pass and
are the ones to watch; do not go hunting for a regression you did not cause.

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
