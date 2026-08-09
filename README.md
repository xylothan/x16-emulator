<p align="center">
  <img src="./.gh/logo.png" />
</p>

# Commander X16 Emulator with Advanced Debugging

[![Build Status](https://github.com/xylothan/x16-emulator/actions/workflows/build.yml/badge.svg)](https://github.com/xylothan/x16-emulator/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/xylothan/x16-emulator)](https://github.com/xylothan/x16-emulator/releases)
[![License: BSD-Clause](https://img.shields.io/github/license/xylothan/x16-emulator)](./LICENSE)
[![Contributors](https://img.shields.io/github/contributors/xylothan/x16-emulator.svg)](https://github.com/xylothan/x16-emulator/graphs/contributors)

This is a fork of the [official Commander X16 emulator][upstream] built for people who write
software for the X16 rather than just run it. It is the upstream emulator with a real debugger
bolted on: a dockable graphical debug UI, source-level debugging from cc65 `.dbg` files, and a
Debug Adapter Protocol server so you can drive the machine from VS Code, Visual Studio, or any
other DAP-speaking editor.

Everything the official emulator does, this does too. It tracks upstream and merges its releases.

> ### ⚠️ This is experimental software
>
> The emulator core is upstream's and is solid, but **the debugger, the DAP server and the
> Windows-specific changes in this fork are experimental**. Expect rough edges, and expect
> details to change between releases.
>
> That said, it is not a toy — it has been used successfully to carry several real
> Commodore 64 to X16 porting projects through to completion, which is exactly the work it was
> built for. If you are debugging assembly on the X16 today, it will very likely save you time.
>
> Please report anything you find on [this fork's issue tracker][issues]. Bug reports about
> *this fork's* features belong here, not upstream.

Why you might want this fork
----------------------------

| | |
|---|---|
| **Graphical debugger** | A dockable Dear ImGui debug window with disassembly, CPU, memory, source, call stack, symbols, breakpoints and VERA/PSG/FM/PCM inspectors. |
| **Source-level debugging** | Point it at a cc65 `.dbg` file and step through your original `.s`/`.c` source, with labels, equates and live values. `.dbg` files auto-load, including for overlays the program `LOAD`s at runtime. |
| **Debug from your editor** | A built-in DAP server means breakpoints, stepping, watches, memory and disassembly in VS Code, Visual Studio, or any DAP-speaking client. |
| **Conditional breakpoints** | Break on `A == $05`, `byte[$1234] != 0`, a specific RAM bank, or the Nth hit. |
| **Watchpoints** | Break on writes to an address or a whole range, with up to 64 of them. |
| **Scriptable input** | Inject keystrokes, typed text and joystick state over DAP, so you can automate regression runs of your game. |
| **VERA inspectors** | Live palette, tile, sprite, bitmap and tilemap viewers plus fully decoded VERA registers — decoded per scanline, so raster splits are visible. |
| **65C816 / GS aware** | 24-bit bank:address memory browsing, mode-correct register widths, and the X16 virtual registers R0–R15 broken out. |
| **Real Windows builds** | Self-contained statically linked `x16emu.exe` for x64, x86 and ARM64. No SDL2, zlib or Visual C++ redistributable to install. |
| **The window doesn't freeze** | On Windows the emulator keeps running and painting while you drag or resize its window. |
| **Automated releases** | Every push is built for all platforms; tagged builds are published automatically. |

The debugger is documented in full under [Advanced Debugging](#advanced-debugging) below.

Relationship to the official emulator
-------------------------------------

This fork exists to add debugging tooling, not to diverge from the X16 platform. It follows
upstream closely and merges each official release.

**Everything about the Commander X16 itself — the KERNAL, BASIC, VERA, the hardware, the file
formats — is documented officially, and those docs apply here unchanged:**

* [Official X16 documentation][x16docs] — the reference for the machine, its KERNAL and BASIC
* [x16-rom][x16rom] — the KERNAL/BASIC ROM sources
* [Official emulator][upstream] — upstream, whose release notes are reproduced in [RELEASES.md](RELEASES.md)
* [commanderx16.com][website] and the [community forum][forum]

Use the official docs for the machine. Use this README for what this fork adds on top.

### Version and release numbering

Releases here are named **`R49.nnn`**:

* `R49` is the official Commander X16 release the build tracks, so `R49.007` is compatible with
  everything an official R49 build is compatible with — including the R49 `rom.bin`.
* `nnn` counts our builds on top of it, and goes up as this fork's own features land.

When upstream ships R50, our releases become `R50.001` and so on. Release branches and tags use
the same `R49.nnn` name, so a tag maps unambiguously to the upstream release it is based on.

> **Match your ROM to your emulator.** An `R49.nnn` build wants an R49 `rom.bin`, which is
> included in each release package. Older ROMs may not work with newer emulators, and vice versa.

Features
--------

Inherited from the official emulator:

* CPU: 65C02 and 65C816 instruction sets, selected by command line switch
* VERA
	* Mostly cycle exact emulation
	* Supports almost all features:
		* composer
		* two layers
		* sprites
		* VSYNC, raster, sprite IRQ
* Sound
	* PCM
	* PSG
	* YM2151
	* MIDI via FluidSynth
* Real-Time-Clock
* NVRAM
* System Management Controller
* SD card: reading and writing (image file)
* VIA
	* ROM/RAM banking
	* keyboard
	* mouse
	* gamepads

Added by this fork:

* Dear ImGui graphical debugger (`-imgui`)
* Debug Adapter Protocol server for editor-based debugging (`-debugport`)
* cc65 source-level debugging (`-dbgfile`, `-srcpath`)
* Conditional breakpoints, hit counts and memory watchpoints
* Statically linked MSVC builds for Windows x64/x86/ARM64
* Non-blocking window drag and resize on Windows

Binaries & Compiling
--------------------

Binary releases for macOS, Windows and Linux are available on [this fork's releases page][releases].
Looking for the official builds instead? They are [over here][upstream-releases].

### Which Windows download?

Windows ships in three flavours. Unless you have a reason to pick otherwise, take `x16emu_win64`.

| Package | What it is |
|---|---|
| `x16emu_win64`, `x16emu_win32`, `x16emu_win-arm64` | A single self-contained `x16emu.exe`, statically linked. Nothing to install — no SDL2, zlib or Visual C++ redistributable. Built without FluidSynth, so it offers no MIDI options at all. |
| `…-midi` | A separate build with FluidSynth compiled in, shipped with `libfluidsynth-3.dll` for the MIDI synth (`-midicard` / `-sf2`). Take this if you want MIDI. |
| `x16emu_win64-mingw`, `x16emu_win32-mingw` | The older MinGW build, kept as a fallback. Ships MIDI support and the `-trace` option, at the cost of around 25 DLLs alongside the executable. |

All of them include the debugger and the DAP server.

Two differences worth knowing about the default builds:

- The non-`-midi` packages are built without FluidSynth, so they do not offer the MIDI options
  at all — they will not appear in `-h`, and dropping the DLL next to one will not enable them.
- The MSVC builds have no `-trace` option. The generated ROM listing contains a string literal
  larger than MSVC's 16 KB limit, so a trace-enabled build cannot compile. Use the `-mingw`
  package if you need `-trace`.

The emulator itself is dependent only on SDL2. However, to run the emulated system you will also need a compatible `rom.bin` ROM image. This will be
loaded from the directory containing the emulator binary, or you can use the `-rom .../path/to/rom.bin` option.

> __WARNING:__ Older versions of the ROM might not work in newer versions of the emulator, and vice versa.

You can build a ROM image yourself using the [build instructions][x16rom-build] in the [x16-rom] repo. The `rom.bin` included in the [_latest_ release][releases] of the emulator may also work with the HEAD of this repo, but this is not guaranteed.

### Building from source

The emulator depends on SDL2. The debugger and DAP server add two more requirements:

* a **C++17** compiler, because Dear ImGui and the debug UI are C++ (Dear ImGui itself is
  vendored in `src/extern/imgui/`, so there is nothing to install for it), and
* **cJSON**, used by the DAP server — `libcjson-dev` on Debian/Ubuntu, `cjson` on Homebrew,
  or supplied by vcpkg on Windows.

Both the ImGui debugger and the DAP server are always compiled in; `-imgui` and `-debugport`
are runtime switches, so there is nothing to enable at build time.

### macOS Build

Install SDL2 using `brew install sdl2`, and cJSON using `brew install cjson`.

### Linux Build

The SDL2 development package is available as a distribution package with most major versions of Linux:
- Red Hat: `yum install SDL2-devel cjson-devel`
- Debian: `apt-get install libsdl2-dev libcjson-dev`

Type `make` to build the source. The output will be `x16emu` in the current directory. Remember you will also need a `rom.bin` as described above.

### WebAssembly Build

Steps for compiling WebAssembly/HTML5 can be found [here][webassembly]. The WebAssembly build does
not include the ImGui debugger or the DAP server.

### Windows Build

The Windows releases are built with **MSVC and CMake**, using vcpkg for dependencies and custom
triplets in `vcpkg-triplets/` that link everything statically. That is what produces the
self-contained `x16emu.exe` in the release packages:

```
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_INSTALLATION_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_OVERLAY_TRIPLETS=vcpkg-triplets ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-x16emu ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DENABLE_TRACE=OFF
cmake --build build --config Release
```

Use `-A Win32` with `x86-windows-x16emu`, or `-A ARM64` with `arm64-windows-x16emu`, for the other
architectures. Add `-DENABLE_FLUIDSYNTH=ON -DVCPKG_MANIFEST_FEATURES=midi` for a MIDI build.
`ENABLE_TRACE` must stay off under MSVC: the generated ROM listing contains a string literal
larger than MSVC's 16 KB limit.

The output is `build\Release\x16emu.exe` and `build\Release\makecart.exe`. Remember you will also
need a `rom.bin` as described above.

Cross-compiling from macOS/Linux/MSYS2 with mingw-w64 also still works. Install the mingw-w64
toolchain and the mingw32 version of SDL, then:
```
CROSS_COMPILE_WINDOWS=1 MINGW32=/usr/x86_64-w64-mingw32 WIN_SDL2=/usr/x86_64-w64-mingw32 make
```
Paths to those libraries can be changed to your installation directory if they aren't located there.

The output will be `x16emu.exe` in the current directory. You will also need `SDL2.dll` from SDL2's binary folder.


Starting
--------

You can start `x16emu`/`x16emu.exe` either by double-clicking it, or from the command line. The latter allows you to specify additional arguments.
When starting `x16emu` without arguments, it will pick up the system ROM (`rom.bin`) from the executable's directory.

* `-prg <app.prg>[,<load_addr>]` lets you specify a `.prg` file that gets loaded after start. It is fetched from the host filesystem, even if an SD card is attached. The override load address is hex without a prefix.
* `-bas <app.txt>` lets you specify a BASIC program in ASCII format that automatically typed in (and tokenized).
* `-run` executes the application specified through `-prg` or `-bas` using `RUN`.
* `-scale {1|2|3|4}` scales video output to an integer multiple of 640x480
* `-quality {nearest|linear|best}` change image scaling algorithm quality
    * `nearest`: nearest pixel sampling
    * `linear`: linear filtering
    * `best`: (default) anisotropic filtering
* `-widescreen` Stretch output to 16:9 resolution to mimic display of a widescreen monitor.
* `-fullscreen` Start up in fullscreen mode instead of in a window.
* `-opacity (0.0,...,1.0)` Set the opacity value (0.0 for transparent, 1.0 for opaque) of the window. (default: 1.0)
* `-rtc` causes the real-time-clock set to the system's time and date.
* `-echo [{iso|raw}]` causes all KERNAL/BASIC output to be printed to the host's terminal. Enable this and use the BASIC command "LIST" to convert a BASIC program to ASCII (detokenize).
* `-rom <rom.bin>` Override KERNAL/BASIC/* ROM file.
* `-ram <ramsize>` specifies banked RAM size in KB (8, 16, 32, ..., 2048). The default is 512.
* `-cart <crtfile.crt>` loads a cartridge file. This requires a specially formatted cartridge file, as specified in the documentation.
* `-cartbin <romfile.bin>` loads a raw cartridge file. This will be loaded starting at ROM bank 32. All cart banks will be flagged as RAM.
* `-joy1`, `-joy2`, `-joy3`, `-joy4` enables binding a gamepad to that SNES controller port
* `-nvram` lets you specify a 64 byte file for the system's non-volatile RAM. If it does not exist, it will be created once the NVRAM is modified.
* `-keymap` tells the KERNAL to switch to a specific keyboard layout. Use it without an argument to view the supported layouts.
* `-noemucmdkeys`  Disable emulator command keys. `Ctrl+M`/`⇧⌘M` will always be intercepted by the emulator.
* `-capture` starts the emulator with the mouse/keyboard captured
* `-nokeyboardcapture` prevents the emulator from fully capturing the keyboard in capture mode, which allows OS-level keystrokes like Alt+Tab to work while in capture mode.
* `-sdcard` lets you specify an SD card image (partition table + FAT32) which will be presented as device 8 at boot.
* `-hostfsdev <unit>` specifies the device number to use for the HostFS device. If this argument is not used, and `-sdcard` is specified, HostFS is disabled. If `-sdcard` is not specified, the default is 8. If both `-sdcard` and `-hostfsdev 8` are specified, HostFS will take precedence, but both will be active. In this circumstance, if the HostFS device is changed away from unit 8 via a channel 15 command (e.g. `"S-9"`), the SD card device will then become visible on unit 8.
* `-fsroot <dir>` specifies a file system root for the HostFS interface. This lets you save and load files without an SD card image. (As of R42, this is the preferred method.) Default is the current working directory.
* `-startin <dir>` specify the host filesystem directory path that the emulated filesystem starts in. Default is the current working directory if it lies within the hierarchy of fsroot, otherwise it defaults to fsroot itself.
* `-serial` makes accesses to the host filesystem go through the Serial Bus [experimental].
* `-nohostieee` or `-nohostfs` disables IEEE API interception to access the host fs. IEEE API HostFS is normally enabled unless `-sdcard` or `-serial` is specified.
* `-warp` causes the emulator to run as fast as possible, possibly faster than a real X16.
* `-pastewarp` causes the emulator to enter warp mode during pasting (`Ctrl+V` or `⌘V`) and during loading via `-bas`.
* `-gif <filename>[,wait]` to record the screen into a GIF. See below for more info.
* `-wav <filename>[{,wait|,auto}]` to record audio into a WAV. See below for more info.
* `-log` enables one or more types of logging (e.g. `-log KS`):
	* `K`: keyboard (key-up and key-down events)
	* `S`: speed (CPU load, frame misses)
	* `V`: video I/O reads and writes
* `-debug [<address>]` enables the debugger. Optionally, set a breakpoint
* `-imgui` opens the graphical Dear ImGui debugger in its own window, with dockable panels for the CPU, memory, disassembly, source, breakpoints, symbols, the call stack, VERA graphics and the three audio sources (PSG, YM2151 and PCM). Additive: independent of, and combinable with, `-debug`. See [Advanced Debugging](#advanced-debugging).
* `-bp <address>` sets a breakpoint at `<address>` (hex). Can be repeated. Implies `-debug`.
* `-debugport [<port>]` starts the Debug Adapter Protocol server so an IDE can attach (default port 9009). See [Remote debugging with DAP](#remote-debugging-with-dap).
* `-dbgfile <path>` loads a cc65 `.dbg` file, so addresses can be mapped back to source files and line numbers.
* `-srcpath <dir>` adds a directory to search for the source files a `.dbg` file names. Can be repeated. Sources are looked for at the path recorded in the `.dbg` first, then the directory of the `.dbg`, then each `-srcpath` directory (most recently added first), then the directory of the loaded program, and finally the current directory.
* `-dump` configure system dump (e.g. `-dump CB`):
	* `C`: CPU registers (7 B: A,X,Y,SP,STATUS,PC)
	* `R`: RAM (40 KiB)
	* `B`: Banked RAM (2 MiB)
	* `V`: Video RAM and registers (128 KiB VRAM, 32 B composer registers, 512 B palette, 16 B layer0 registers, 16 B layer1 registers, 16 B sprite registers, 2 KiB sprite attributes)
* `-memorystats <filename.txt>` Saves memory read and write access statistics to the given file when emulator exits.
* `-testbench` Headless mode for unit testing with an external test runner
* `-sound <device>` can be used to specify the output sound device. If 'none', no audio is generated.
* `-abufs` can be used to specify the number of audio buffers (defaults to 8 when using the SD card, 32 when using HostFS). If you're experiencing stuttering in the audio, try increasing this number. This will result in additional audio latency though.
* `-via2` installs the second VIA chip expansion at $9F10.
* `-midline-effects` enables mid-scanline raster effects at the cost of vastly increased host CPU usage.
* `-mhz <integer>` sets the emulated CPU's speed. Range is from 1-40. This option is mainly for testing and benchmarking.
* `-enable-ym2151-irq` connects the YM2151's IRQ pin to the system's IRQ line with a modest increase in host CPU usage.
* `-wuninit` enables warnings on the console for reads of uninitialized memory.
* `-zeroram` fills RAM at startup with zeroes instead of the default of random data.
* `-version` prints additional version information of the emulator and ROM.
* `-c02` selects the 65C02 CPU (default).
* `-c816` selects the 65C816 CPU (experimental).
* `-rockwell` when used while running with the 65C02 CPU, suppresses the console warning emitted on the first occurence when executing a Rockwell instruction. These are the SMBx, RMBx, BBRx, and BBSx instructions. Since these instructions are not supported on the 65C816 processor, such a program using them would not run properly on the 65C816.
* `-longpwron` Simulate a long press of the power button at system power-on.
* When compiled with `#define TRACE`, `-trace` will enable an instruction trace on stdout.

Run `x16emu -h` to see all command line options.

Keyboard Layout
---------------

The X16 uses a PS/2 keyboard, and the ROM currently supports several different layouts. The following table shows their names, and what keys produce different characters than expected:

|Name  |Description 	       |Differences|
|------|------------------------|-------|
|en-us |US		       |[`] ⇒ [←], [~] ⇒ [π], [&#92;] ⇒ [£]|
|en-gb |United Kingdom	       |[`] ⇒ [←], [~] ⇒ [π]|
|de    |German		       |[§] ⇒ [£], [´] ⇒ [^], [^] ⇒ [←], [°] ⇒ [π]|
|nordic|Nordic                 |key left of [1] ⇒ [←],[π]|
|it    |Italian		       |[&#92;] ⇒ [←], [&vert;] ⇒ [π]|
|pl    |Polish (Programmers)   |[`] ⇒ [←], [~] ⇒ [π], [&#92;] ⇒ [£]|
|hu    |Hungarian	       |[&#92;] ⇒ [←], [&vert;] ⇒ [π], [§] ⇒ [£]|
|es    |Spanish		       |[&vert;] ⇒ π, &#92; ⇒ [←], Alt + [<] ⇒ [£]|
|fr    |French		       |[²] ⇒ [←], [§] ⇒ [£]|
|de-ch |Swiss German	       |[^] ⇒ [←], [°] ⇒ [π]|
|fr-be |Belgian French	       |[²] ⇒ [←], [³] ⇒ [π]|
|fi    |Finnish		       |[§] ⇒ [←], [½] ⇒ [π]|
|pt-br |Portuguese (Brazil ABNT)|[&#92;] ⇒ [←], [&vert;] ⇒ [π]|

Keys that produce international characters (like [ä] or [ç]) will not produce any character.

Since the host computer tells the Commander X16 via the emulator the *position* of keys that are pressed, you need to configure the layout for the X16 independently of the keyboard layout you have configured on the host.

**Use the `MENU` command to select a layout, or set the keyboard layout at startup using the `-keymap` command line argument.**

The following keys can be used for controlling games:

|Keyboard Key  | SNES Equivalent |
|--------------|-----------------|
|X or Ctrl     | A               |
|Z or Alt      | B               |
|S 	           | X               |
|A 	           | Y               |
|D 	           | L               |
|C 	           | R               |
|Shift         | SELECT          |
|Enter         | START           |
|Cursor Up     | UP              |
|Cursor Down   | DOWN            |
|Cursor Left   | LEFT            |
|Cursor Right  | RIGHT           |


Options for the WebEmulator
--------

The following options are available for the WebEmulator. With the exception of manifest and model, they all work exactly the same as in the normal emulator.
* `manifest`
* `ram`
* `cpu`
* `mhz`
* `keymap`
* `model`
* `longpwron`
* `widescreen`
* `capture`
* `midlineeffects`
* `joyX` (where X is a digit, 1 through 4. I.E. joy1, joy2, joy3, joy4)

#### The Address Line
`manifest` tells the emulator what should be loaded as a startup program. If the file is a .bas or .prg, the emulator will load it and try to execute it. If the file is a .zip, the WebEmulator will get access to all the files inside that zip-file. When using a zip-file you may add a manifest file to provide additional information - See section below for more information on `manifest.json`

On the Commander X16 forums, a link to the webemulator could look something like this:
https://cx16forum.org/webemu/x16emu.html?manifest=/forum/download/file.php?id=1218&ram=2048&cpu=c816&mhz=10&keymap=da&widescreen&capture  
This will load the forum file with id 1218 into the emulator.  
Give the emulator 2MB of RAM  
Set the CPU type to 65C816  
Set the CPU speed at 10 MHz  
Set the keyboard layout to Danish  
Show the emulator in widescreen mode  
Capture the mouse and keyboard input 
The options `longpwron`, `widescreen`, `capture`, `midlineeffects` and the `joyX` options do not have any values, it is enough to have them on the address line to enable the feature. The `model` option is only used if set to `gs`, then the `-gs` option will be passed to the emulator.

#### The manifest.json File
If an application requires more than a single file to function, for example graphics or audio assets, it is necessary to package the needed files in a zip file. If there are more than one start file (BAS or PRG) the `manifest.json` file can be used to specify the default start file with `start_prg` or `start_bas` otherwise the WebEmulator will start the .prg or .bas it finds in the zip file.  

Here is an example of the optional `manifest.json` file  
	
	{
		"manifest_version": "1.0.0",
		"name": "My Program",
		"author": "John Smith",
		"app_version": "1.0.0",
		"license": "GPL 3",
		"ram": "2048",
		"cpu": "c816",
		"mhz": "10",
		"keymap": "da",
		"widescreen": true,
		"capture": true,
		"longpwron": false,
		"midlineeffects": false,
		"start_prg": "MYPROG.PRG",
		"resources": [
			"MYPROG.PRG",
			"FILE1.BIN",
			"FILE2.BIN"
		]
	}

If the resources section is present, only files specified will be made available to the WebEmulator.  
Options set in `manifest.json` will override options on the address line.

Functions while running
-----------------------

#### Windows and Linux
* `Ctrl` + `F` and `Ctrl` + `Return` will toggle full screen mode.
* `Ctrl` + `M` will toggle mouse capture mode.
* `Ctrl` + `P` will write a screenshot in PNG format to disk.
* `Ctrl` + `R` will reset the computer.
* `Ctrl` + `Backspace` will send an NMI to the computer (like RESTORE key).
* `Ctrl` + `S` will save a system dump (configurable with `-dump`) to disk.
* `Ctrl` + `V` will paste the clipboard by injecting key presses.
* `Ctrl` + `=` and `Ctrl` + `+` will toggle warp mode.

#### Mac OS
* `⌘F` and `⌘Return` will toggle full screen mode.
* `⇧⌘M` will toggle mouse capture mode.
* `⌘P` will write a screenshot in PNG format to disk.
* `⌘R` will reset the computer.
* `⌘Delete` aka `⌘Backspace` will send an NMI to the computer (like RESTORE key).
* `⌘S` will save a system dump (configurable with `-dump`) to disk.
* `⌘V` will paste the clipboard by injecting key presses.
* `⌘=` and `⇧⌘+` will toggle warp mode.


GIF Recording
-------------

With the argument `-gif`, followed by a filename, a screen recording will be saved into the given GIF file. Please exit the emulator before reading the GIF file.

If the option `,wait` is specified after the filename, it will start recording on `POKE $9FB5,2`. It will capture a single frame on `POKE $9FB5,1` and pause recording on `POKE $9FB5,0`. `PEEK($9FB5)` returns a 128 if recording is enabled but not active.


WAV Recording
-------------

With the argument `-wav`, followed by a filename, an audio recording will be saved into the given WAV file. Please exit the emulator before reading the WAV file.

If the option `,wait` is specified after the filename, it will start recording on `POKE $9FB6,1`. If the option `,auto` is specified after the filename, it will start recording on the first non-zero audio signal. It will pause recording on `POKE $9FB6,0`. `PEEK($9FB6)` returns a 1 if recording is enabled but not active.


Emulator I/O registers
-------------------
x16-emulator exposes registers in the range of, from `$9FB0`-`$9FBF`, which allows one to control or toggle various emulator features from within emulated code.

When writing machine code that uses these registers, good practice is to read `$9FBE` and `$9FBF` and check for their return values. If the emulator is present, those memory locations will return the ASCII/PETSCII characters "1" and "6" respectively (`$31` and `$36` hex).  After verifying that the code is running under the emulator, you can confidently use the features provided by these registers.

Several of the following registers are particularly useful for debugging. In particular, writing data to `$9FB9`, `$9FBA`, or `$9FBB` will output debug information to the console, terminal, or command prompt window from which you ran x16emu.


| Register | Read Behavior | Write Behavior |
|-|-|-|
| \$9FB0 | Returns debugger enabled flag | `0` disables, `1` enables the debugger, overriding the absence or presence of the `-debug` command line argument. |
| \$9FB1 | Returns video logging flag | `0` disables, `1` enables logging of VRAM accesses to the console |
| \$9FB2 | Returns keyboard logging flag | `0` disables, `1` enables logging of keyboard events to the console |
| \$9FB3 | Returns echo mode | `0` disables, `1` enables raw echo, `2` enables cooked (`\Xnn` for non-ASCII), and `3` enables ISO (w/ conversion to UTF-8). When on, characters sent via the `BSOUT` KERNAL call will also appear on the console. |
| \$9FB4 | Returns save-on-exit flag | `0` disables, `1` enables save-on-exit. When this option is set and the program counter reaches \$FFFF, the emulator outputs a dump of emulator state to `dump.bin` before exiting. |
| \$9FB5 | Returns GIF recorder state | `0` pauses, `1` captures a single frame, and `2` activates/resumes GIF recording. The path to the GIF file must have been passed to the `-gif` command line option in advance. |
| \$9FB6 | Returns WAV recorder state | `0` pauses, `1` enables WAV recording, and `2` sets up autostart. The path to the WAV file must have been passed to the `-wav` command line option in advance. |
| \$9FB7 | Returns emu command key flag | `0` allows, and `1` inhibits most emulator command keys. Setting this flag prevents the emulator from intercepting keystrokes such as Ctrl+V/⌘V or Ctrl+R/⌘R, allowing the Commander X16 application running inside to make use of them. |
| \$9FB8 | Latches the cpu clock counter and returns bits 0-7 | Resets the cpu clock counter to 0 |
| \$9FB9 | Returns bits 8-15 from the latched cpu clock counter value | Outputs `"User debug 1: $xx"` to the console with xx replaced by the value written. |
| \$9FBA | Returns bits 16-23 from the latched cpu clock counter value | Outputs `"User debug 2: $xx"` to the console with xx replaced by the value written. |
| \$9FBB | Returns bits 24-31 from the latched cpu clock counter value | Outputs the given character to the console. This is basically a STDOUT port for programs running in the emulator. Only printable characters are allowed. Non-printables are replaced with &#xfffd;.
| \$9FBC | - | - |
| \$9FBD | Returns the keymap index, based on the argument to the `-keymap` command line option | - |
| \$9FBE | Returns the value `$31`/ASCII "1", useful for emulator presence detection | - |
| \$9FBF | Returns the value `$36`/ASCII "6", useful for emulator presence detection | - |


BASIC and the Screen Editor
---------------------------

On startup, the X16 presents direct mode of BASIC V2. You can enter BASIC statements, or line numbers with BASIC statements and `RUN` the program, just like on Commodore computers.

* To stop execution of a BASIC program, hit the `RUN/STOP` key (`Pause`), or `Ctrl+C`.
* To insert characters, first insert spaces by pressing `Shift+Backspace` or `Insert`, then type over those spaces.
* To clear the screen, press `Shift+Home`.
* To send NMI, similar to `STOP+RESTORE` on the C64, use Ctrl+Backspace/⌘Delete. On real hardware this is done with `Ctrl+Alt+RESTORE` (`Ctrl+Alt+PrtScr`) or by pressing the NMI button.


SD Card Images
--------------

The command line argument `-sdcard` lets you attach an image file for the emulated SD card. Using an emulated SD card makes filesystem operations go through the X16's DOS implementation, so it supports all filesystem operations (including directory listing though `DOS"$` command channel commands using the `DOS` statement) and guarantees full compatibility with the real device.

Images must be greater than 32 MB in size and contain an MBR partition table and a FAT32 filesystem. The file `sdcard.img.zip` in this repository is an empty 100 MB image in this format.

On macOS, you can just double-click an image to mount it, or use the command line:

	# hdiutil attach sdcard.img
	/dev/disk2              FDisk_partition_scheme
	/dev/disk2s1            Windows_FAT_32                  /Volumes/X16 DISK
	# [do something with the filesystem]
	# hdiutil detach /dev/disk[n] # [n] = number of device as printed above

On Linux, you can use the command line:

	# sudo losetup -P /dev/loop21 disk.img
	# sudo mount /dev/loop21p1 /mnt # pick a location to mount it to, like /mnt
	# [do something with the filesystem]
	# sudo umount /mnt
	# sudo losetup -d /dev/loop21

On Windows, you can use the [OSFMount](https://www.osforensics.com/tools/mount-disk-images.html) tool. Windows VHD files can also be created using the built-in Disk Manager. Careful attention should be paid to the settings when creating and formatting the VHD:

 * The file must be at least 32MB and must be fixed size. Expanding VHDs are not supported.
 * Use an MBR partition tables. The Commander X16 does not recognize GPT partition tables.
 * You must format the VHD with FAT32. Other file formats are not supported.

 This is a trick, since Fixed-size VHD files contain the data first, with the metadata in a footer at the end. Since the emulator does not read or edit that medatada, it will only work with fixed-size files that are fully populated.


Host Filesystem Interface
-------------------------

If the system ROM contains any version of the KERNAL, and there is no SD card image attached, all accesses to the ("IEEE") Commodore Bus are intercepted by the emulator for device 8 (the default). So the BASIC statements will target the host computer's local filesystem:

      DOS"$"
      LOAD"FOO.PRG"
      LOAD"IMAGE.PRG",8,1
      SAVE"BAR.PRG"
      OPEN2,8,2,"FOO,S,R"

The emulator will interpret filenames relative to the directory it was started in. On macOS, when double-clicking the executable, this is the home directory. To specify a different path as the emulated root, you can use the `-fsroot` command line option.

To avoid compatibility problems between the PETSCII and ASCII encodings, you can

* use uppercase filenames on the host side, and unshifted filenames on the X16 side.
* use `Ctrl+O` to switch to the X16 to ISO mode for ASCII compatibility.
* use `Ctrl+N` to switch to the upper/lower character set for a workaround.

As of R42, the Host Filesystem interface (or HostFS) is the preferred method of accessing files. It does not require creating or managing an SDcard image, and it supports all of the CMDR-DOS commands. However, it is not cycle-accurate, since the emulator traps calls to DOS and performs the same actions in the host environment. If performance and hardware accuracy is required, you will want to perform final testing using an SD card image.

Dealing with BASIC Programs
---------------------------

BASIC programs are encoded in a tokenized form when saved. They are not simply ASCII files. If you want to edit BASIC programs on the host's text editor, you need to convert it to tokenized BASIC encoding from ASCII encoding before calling `LOAD` in the emulator.

* To convert the basic file from ASCII to tokenized BASIC encoding, reboot the machine and paste the ASCII text using `Ctrl + V` (Mac: `Cmd + V`) into the terminal. You can now run the program with `RUN`, or use the `SAVE` BASIC command to write the tokenized version to the host disk.  Below is an example.
  1. Copy ASCII text from host basic file "PRG.BAS"
  2. Paste into new terminal session
  3. `SAVE"ENCODED.BAS`
  4. Now you can restart the emulator and load the encoded basic file with `LOAD"ENCODED.BAS"`
  5. Run with `RUN"ENCODED.BAS"`

* To convert BASIC to ASCII, start x16emu with the `-echo` argument, `LOAD` the BASIC file, and type `LIST`. Now copy the ASCII version from the terminal.


Using the KERNAL/BASIC environment
----------------------------------

Please see the official [KERNAL/BASIC documentation][x16docs].


Advanced Debugging
------------------

This is the part of the emulator that this fork exists for. There are three ways to debug, and
they are additive — you can turn on any combination of them:

| You want | Use | Start with |
|---|---|---|
| A graphical debugger with panels, source view and VERA inspectors | **ImGui debugger** | `-imgui` |
| To debug from VS Code / Visual Studio / a script | **DAP server** | `-debugport` |
| The small, keyboard-driven debugger inherited from upstream | **Classic debugger** | `-debug` |

A good default for day-to-day assembly work:

```
x16emu -imgui -prg myprog.prg -run
```

A good default when your source is built with cc65:

```
x16emu -imgui -prg myprog.prg -run -dbgfile myprog.dbg -srcpath ./src
```

> ⚠️ The ImGui debugger and the DAP server are **experimental**. They have been used to complete
> several real C64→X16 porting projects, but you should expect rough edges and occasional
> changes to keys, layouts and DAP details between releases.

### The ImGui debugger

`-imgui` opens a second, resizable OS window titled **"Commander X16 - ImGui Debugger"**,
960×720, placed next to the emulator window. It is available on Windows, Linux and macOS. (It is
not available in the WebAssembly build.)

It does not require `-debug`, and it does not replace the emulator window — the machine keeps
running in its own window while you inspect it.

#### Panels

Every panel is dockable, closable and reopenable from the **View** menu.

| Panel | What it gives you |
|---|---|
| **Disassembly** | Live disassembly around the PC. Hovering an operand shows the effective address and the value there. Right-click for *Run to here* and *Toggle breakpoint*. |
| **CPU** | Registers with 65C816-aware widths, decoded status flags, and the stack with the most recent push on top. A collapsible **Virtual Regs (R0–R15)** section shows the X16 pseudo-registers at `$02`–`$21`. A collapsible **Watch** section holds your own address watches — bank-qualified, up to 16 bytes each, editable, with hex/decimal/binary tooltips. |
| **Memory** | A hex editor with three tabs: **CPU** (the CPU map), **Banked** (browse any RAM bank at `$A000`–`$BFFF`) and **VRAM** (VERA's full 17-bit address space). Drag to select a range, search by hex bytes or ASCII with Find Next/Prev, jump to an address, and watch changed bytes flash. Right-click to *Add to watch*, *Add range to watch*, *Copy address*, *Break on write* or *Clear selection*. Edits go through the normal write path, so I/O side effects happen and watchpoints fire. |
| **Source** | Your original `.s`/`.c` source in tabs, with the current line highlighted and centred on each stop. Right-click for *Run to here* and *Toggle breakpoint*. **Open…** pre-loads a file so you can set breakpoints before the PC ever gets there. Hovering a label or number resolves it to an address and its live value. |
| **VERA** | Six tabs: **Registers** (all 32 registers `$9F20`–`$9F3F`, fields decoded), **Palette**, **Tiles**, **Sprites**, **Bitmap** and **Tilemap**. Each view decodes using the registers that actually rendered each scanline, so raster splits show up correctly rather than being flattened to the end-of-frame state. |
| **Breakpoints** | Every breakpoint, with its condition and hit count. Enable, disable or delete individually. |
| **Symbols** | A filterable list of every label from your `.dbg`, with live values. Right-click to *Go to*, *Toggle breakpoint* or *Run to here*. |
| **Call Stack** | A heuristic 65xx stack unwind. Frames are named after the nearest enclosing label, so even code without debug info gets a useful name. Click a frame to jump there in both source and disassembly. |
| **PSG** | VERA PSG voices with live register values, plus scope traces. |
| **YM2151** | FM channel state and scope traces. |
| **PCM** | VERA PCM state and scope traces. |

While the machine is paused, the audio panels keep drawing their scope traces by projecting from
the current register state, so you can see what a voice *would* be doing at the moment you stopped.

#### Toolbar

Along the top, after the menus: **Continue**, **Pause**, **Step Into**, **Step Over**,
**Step Out** (each greyed out when it does not apply and each showing its shortcut in the
tooltip), then the speed control, then a status readout.

The speed control shows the emulated clock as a real number — `1.75MHz`, `800kHz`, or `warp` —
rather than a percentage, coloured blue below native speed, **red above it**, and orange in warp
mode. `-` and `+` step the speed, `1x` returns to the machine's own clock, and `Warp` removes the
limit entirely.

The status readout shows `RUNNING`/`PAUSED`, an `IRQ` marker (with nesting depth) while the PC is
inside an interrupt handler, the 24-bit PC as `KK:PPPP`, and the elapsed cycle and instruction counts.

#### Menus

* **View** — show or hide any panel.
* **Layout** — *Reset to Default Layout* reopens everything and rebuilds the default docking arrangement.
* **System** — *Reset* (`Ctrl+Shift+F5`, keeps your breakpoints), *Trigger IRQ*, *Trigger NMI*, and *Settings…*.
* **Help** — the keyboard shortcut table, so you never need this README for it.

**Settings…** covers interrupt following, break-on-interrupt, whether a break auto-switches you to
the Disassembly or Source panel, memory change highlighting and its duration, holding audio while
paused, and the global interface scale. Settings and your window layout are saved to `imgui.ini`
and restored next run.

#### Keyboard shortcuts

Active when the debugger window has focus and you are not typing into a search or goto box.

| Key | Action |
|---|---|
| `F5` | Continue (while paused) |
| `Break` / `Pause` | Pause (while running) |
| `F11` | Step into |
| `Shift`+`F11` | Step out |
| `F10` | Step over |
| `Ctrl`+`F10` | Run to cursor |
| `F9` | Toggle a breakpoint at the PC |
| `Ctrl`+`Shift`+`F5` | Reset the machine, keeping breakpoints |
| `Ctrl`+`Mouse wheel` | Zoom the font of the panel under the cursor |

On macOS these are the physical `Control` and `Shift` keys, not `Command`.

These work in the emulator window and are handy while debugging graphics:

| Key | Action |
|---|---|
| `Ctrl`+`F3` | Toggle VERA layer 0 |
| `Alt`+`F3` | Toggle VERA layer 1 |
| `Shift`+`F3` | Toggle sprites |
| `Shift`+`F8` | Toggle KERNAL skip |

#### Breakpoints and watchpoints

Set breakpoints by clicking the gutter in the Disassembly or Source panel, pressing `F9`, using a
right-click menu, or passing `-bp <address>` on the command line. Watchpoints break on writes to
an address or a selected range — select bytes in the Memory panel and choose *Break on write*.
Up to 64 watchpoints can be active at once.

Conditional breakpoints and hit counts are set through a DAP client; the syntax is documented
under [Remote debugging with DAP](#remote-debugging-with-dap), and the resulting conditions and
hit counts are visible in the Breakpoints panel.

### Source-level debugging with cc65

Build with debug info, and the emulator will show you your own source instead of raw disassembly.

Build:

```
ca65 --debug-info -o myprog.o myprog.s
ld65 -C myprog.cfg --dbgfile myprog.dbg -o myprog.prg myprog.o
```

Run:

```
x16emu -imgui -prg myprog.prg -run -dbgfile myprog.dbg -srcpath ./src
```

There are three ways a `.dbg` gets loaded:

* `-dbgfile <file.dbg>` loads one explicitly at start-up.
* A `.dbg` next to a program is auto-loaded when that program is loaded, whether via `-prg` or a
  KERNAL `LOAD`. The rule is simply to swap the extension: `myprog.prg` → `myprog.dbg`.
* **Any file the running program `LOAD`s at runtime** gets its matching `.dbg` merged
  automatically. Debug info for the address range being replaced is dropped first, so the Source
  panel follows execution into overlays and dynamically loaded modules and switches to the right
  file as the PC crosses module boundaries. Breakpoints in an unloaded range are invalidated and
  re-resolved when a module loads back into it. This works with or without a DAP client attached.

Because a `.dbg` only records source *file names*, the emulator locates the actual `.s`/`.c` files
by searching, in order: the path stored in the `.dbg`, the directory of the `.dbg`, each
`-srcpath <dir>` you passed (most recent first), the directory of the loaded program, and finally
the current directory.

**Banked code is handled.** A `.dbg` does not record which RAM bank a `$A000`–`$BFFF` segment
belongs to, so the emulator works it out at runtime by watching the actual RAM bank register, and
filters source mapping and breakpoints by the current bank. You do not have to do anything at
build time. In a DAP breakpoint condition you can pin a breakpoint to a bank explicitly with
`bank == N`.

### Remote debugging with DAP

`-debugport [<port>]` starts a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
server over TCP, on port **9009** by default, so an IDE such as VS Code or Visual Studio can
attach for source-level debugging. It speaks standard `Content-Length`-framed JSON, so any
compliant DAP client works.

```
x16emu -debugport -prg myprog.prg -run -dbgfile myprog.dbg -srcpath ./src
x16emu -debugport 4711 ...           # pick your own port
x16emu -debugport -debug ...         # also open the classic window, so F12 can pause manually
```

The emulator runs immediately on launch; it does not auto-pause waiting for a client.

A minimal VS Code attach configuration looks like this — note that the `type` value depends on
which DAP extension you wire it up to, as no editor extension is bundled with the emulator:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Attach to X16 Emulator",
      "request": "attach",
      "type": "x16",
      "host": "localhost",
      "port": 9009
    }
  ]
}
```

#### What the server supports

Standard requests: `initialize`, `launch`, `attach`, `configurationDone`, `threads`, `stackTrace`,
`scopes`, `variables`, `setVariable`, `setBreakpoints`, `setFunctionBreakpoints`,
`setInstructionBreakpoints`, `setDataBreakpoints`, `setExceptionBreakpoints`, `breakpointLocations`,
`continue`, `next`, `stepIn`, `stepOut`, `pause`, `goto`, `gotoTargets`, `stepInTargets`,
`evaluate`, `readMemory`, `writeMemory`, `disassemble`, `loadedSources`, `source`, `restart`,
`disconnect` and `terminate`.

Four scopes are exposed on every stack frame: **Registers** (including the 65C816 extras and the
current RAM/ROM bank), **Virtual Registers** (R0–R15), **Zero Page** and **Stack**. Memory reads
and writes use 24-bit addresses, so the high byte selects the 65C816 program bank on a GS machine.

#### Conditional breakpoints

Put a condition string on a breakpoint in `setBreakpoints`. Terms are joined with `&&`.

* **Pin to a bank:** `bank == 2`, `bank 2`, `rombank == 3`. Decimal or `$XX`/`0xXX`.
* **Test a value:** operands `A`, `X`, `Y`, `SP`, `P`, `byte[$ADDR]`, `word[$ADDR]`, or a bare
  `$ADDR` (which means `byte[$ADDR]`). Operators `==`, `!=`, `<`, `<=`, `>`, `>=`.

```
A == $05
byte[$1234] != 0
word[$0002] >= $0100
bank == 0 && byte[$1234] != 0
```

A `hitCondition` of `"5"` stops on the fifth hit.

#### Expressions you can evaluate

These work in a watch or REPL window, in addition to plain registers and addresses:

| Expression | Result |
|---|---|
| `$C000`, `$01C000`, `0xC000` | The byte at that CPU address (24-bit; high byte is the 65C816 bank) |
| `A`, `X`, `Y`, `SP`, `PC`, `P` | Register values, sized for the current CPU mode |
| `regs_all` | A one-line summary of every register |
| `vram ADDR [COUNT]` | VERA VRAM bytes, from a 17-bit hex address |
| `vera_reg` | All 32 VERA registers |
| `vera_line LINE` | The layer registers that actually rendered that display scanline — the way to catch a raster split |
| `bp_add ADDR`, `bp_remove ADDR`, `bp_list`, `bp_clear` | Manage breakpoints |
| `watch_add ADDR [LEN]`, `watch_remove ADDR`, `watch_list`, `watch_clear` | Manage write watchpoints |
| `reset` | Reset the machine |

cc65 labels and equates from a loaded `.dbg` resolve too.

#### Scripting input for automated tests

Three custom requests let a script drive the machine, which is what makes automated regression
runs of a game practical:

| Command | Arguments | Effect |
|---------|-----------|--------|
| `x16/sendKey` | `key` (SDL key name like `"Return"`/`"A"`/`"Left"`, or a numeric scancode), `action` (`"press"` (default), `"down"`, `"up"`), optional `modifiers` (array of key names held around the key) | Injects a key event through the same path as a physical key press. |
| `x16/type` | `text` (string) | Types a string into the KERNAL keyboard buffer, like a clipboard paste. Ideal for BASIC lines and filenames; supports the `\Xnn` hex escape. |
| `x16/joystick` | `index` (slot 0–3), `buttons` (array of `up`, `down`, `left`, `right`, `a`, `b`, `x`, `y`, `start`, `select`, `l`, `r`), optional `mask` (raw 16-bit active-low), `enabled` (set `false` to release the slot back to a physical controller) | Holds or releases a virtual SNES controller. Buttons not listed are released. |

There is also `x16/registers`, which returns the full CPU, KERNAL and VERA state as JSON in one call.

#### x16dbg, the bundled command-line client

`tools/x16dbg` is a small .NET 8 console DAP client, useful for poking at a running emulator
without an IDE and for scripting.

```
dotnet run --project tools/x16dbg                       # interactive
dotnet run --project tools/x16dbg -- mem 1030-1040      # one-shot
dotnet run --project tools/x16dbg -- -host 127.0.0.1 -port 9009
```

Options are `-host <ip>` (default `127.0.0.1`), `-port <port>` (default `9009`), `-i` to force
interactive mode, and `-h` for help. With no command it drops into an interactive prompt.

Commands, in both modes: `step`/`s`, `continue`/`c`, `break`/`b`, `status`/`t`, `reset`,
`regs`/`r`, `setreg <reg> <hexval>`, `mem <addr>-<end>` or `mem <addr> <len>`,
`setmem <addr> <hexdata>`, and `bp add|remove|list|clear <addr>`. Addresses are hex.

```
> b
! STOPPED (pause)
  PC=$C1C6  A=$02 (  2)  X=$FF (255)  Y=$0A ( 10)
  SP=$F0  P=$24 [--B-DI--]  RAM Bank=$00  ROM Bank=$00
> bp add 080D
> mem C000-C00F
  C000: A9 00 85 00 A9 01 85 01 A9 00 8D 00 02 A9 00 8D  |................|
> c
```

### The classic debugger

The original upstream debugger is still here, unchanged, and is a good choice when you want
something small and keyboard-driven.

The classic debugger requires `-debug`. To start it, press the F12 key. Without `-debug`, the debugger is disabled and won't start.  If you wish to set an initial breakpoint you can also include the memory address, in hexadecimal, of the breakpoint after the `-debug` switch. For example `-debug 080d`.

The debugger runs in its own separate window ("X16 Debugger"). Function/debugger
keys are routed by keyboard focus: when the **debugger** window is focused its
keys (F1, F5, F9, F12, navigation, etc.) drive the debugger; when the **emulator**
window is focused all keys — function keys included — go to the emulated machine.
So to break into a running program with F12, click the debugger window first.

There are 2 panels you can control. The code panel, the top left half, and the data panel, the bottom half of the screen. You can also edit the contents of the registers PC, A, B, C, D, K, DB, X, Y, and SP.

Greyed out numbers in the register display indicate values that are fixed at their given value due to the current processor state. This applies to
- the high bytes of X and Y if the index flags is set
- the flags M (memory) and I (index) if emulation mode is active
- the high byte of SP if emulation mode is active.

The debugger uses its own command line with the following syntax:

|Statement|Description|
|---------|----------------------------------------------------------------------------------------------------|
|d %x|Change the code panel to view disassembly starting from the address %x.|
|m %x|Change the data panel to view memory starting from the address %x.|
|v %x|Display VERA RAM (VRAM) starting from address %x.|
|f&#160;%x&#160;%v&#160;[%n]|Fill memory address %x with value %v, optionally for %n number of bytes. In banked memory, the bank currently being displayed in the data panel is used, otherwise bank 0
|b %s %d|Changes the current memory bank for disassembly and data. The %s param can be either 'ram' or 'rom', the %d is the memory bank to display (but see NOTE below!).|
|r %s %x|Changes the value in the specified register. Valid registers in the %s param are 'pc', 'a', 'b', 'c', 'd', 'k', 'dbr', 'x', 'y', and 'sp'. %x is the value to store in that register.|

NOTE. To disassemble or dump memory locations in banked RAM or ROM, prepend the bank number to the address; for example, "m 4a300" displays memory contents of BANK 4, starting at address $a300.  This also works for the 'd' command.

The debugger keys are similar to the Microsoft Debugger shortcut keys, and work as follows

| Key               | Description                                                                             |
|-------------------|-----------------------------------------------------------------------------------------|
| F1                | resets the disassembly position to the current PC                                       |
| F2                | resets the emulated CPU but not any of the hardware.                                    |
| F5                | close debugger window and return to Run mode, the emulator should run as normal.        |
| F9                | sets the breakpoint to the currently code position.                                     |
| F10               | steps 'over' routines - if the next instruction is JSR it will break on return.         |
| F11               | steps 'into' routines.                                                                  |
| F12               | breaks back into the debugger (when the debugger window is focused). This does not happen if you do not have -debug |
| PAGE UP           | scrolls memory up by page.                                                              |
| PAGE DOWN         | scrolls memory down by page.                                                            |
| Shift + PAGE UP   | scrolls disassembly up by 16 bytes.                                                     |
| Shift + PAGE DOWN | scrolls disassembly down by 16 bytes.                                                   |
| UP                | scrolls memory up by row.                                                               |
| DOWN              | scrolls memory down by row.                                                             |
| Shift + UP        | scrolls disassembly up by one byte.                                                     |
| Shift + DOWN      | scrolls disassembly down by one byte.                                                   |
| TAB               | when stopped, or single stepping, hides the debug panel while pressed.                  |

When `-debug` is selected the STP instruction (opcode $DB) will break into the debugger automatically.

Keyboard routines only work when the emulator is running normally. Single stepping through keyboard code will not work at present.

#### The classic Source window

With `-debug`, a second window, "X16 Source", opens alongside the classic debugger. It shows the
source file the running code was compiled from and highlights the line at the current program
counter, updating as you step. It uses the same `.dbg` loading and source search rules described
in [Source-level debugging with cc65](#source-level-debugging-with-cc65).

Give the Source window keyboard focus to interact with it:

| Key / action        | Description                                              |
|---------------------|----------------------------------------------------------|
| Up / Down           | scroll one line                                          |
| Page Up / Page Down | scroll one screen                                        |
| Home / End          | jump to the top / bottom of the file                    |
| Mouse wheel         | scroll                                                   |
| Left-click a line   | toggle a breakpoint on that source line                 |
| F9                  | toggle a breakpoint on the current (highlighted) line   |

Execution keys (F5/F10/F11/F12) still work while the Source window is focused.

CRT File Format
---------------

The Commander X16 will support cartridge ROMs, including auto-booting game cartridges. On the Gen-1 Developer board, the first slot will be used for cartridges. On the Gen-2 console machine, there is only one slot. ROM carts should work on both systems.

This CRT format is intended for the emulator, and it is not required or used by the hardware. You can, however, use the MakeCart tool to convert between a single CRT file and BIN files that can be used to program a ROM burner. Also, note that this is different from the CRT format used the VICE emualtor, so files are not interchangable.

Commander X16 cartridges will occupy the same address space as the Commander's KERNAL and BASIC ROMs. You can control the active bank by writing to address $0001 on the computer. Banks 0-31 are the built-in ROM banks, and banks 32-255 will select the cartridge ROMs.

### Header Layout

This is the cartridge header. The first 256 bytes are ASCII data and Human readable. The second 256 bytes are bank data; these are byte integers. Text fields are set to 16 or 32-byte boundaries for ease of formatting.

| Location | Length | Description                                                                                        |
|----------|--------|----------------------------------------------------------------------------------------------------|
| 00-15    | 16     | ASCII text: CX16 CARTRIDGE\r\n                                                                     |
| 16-31    | 16     | CRT format version. ASCII digits in format 01.02, space padded.                                    |
| 32-63    | 32     | Name. ASCII text.                                                                                  |
| 64-95    | 32     | Programmer/Developer. ASCII text.                                                                  |
| 96-127   | 32     | Copyright information. ASCII text.                                                                 |
| 128-191  | 32     | Program version. ASCII text.                                                                       |
| 192-255  | 64     | Empty.                                                                                             |
| 256-287  | 32     | Fill with zeros.                                                                                   |
| 288-511  | 224    | Bank Flags.                                                                                        |
|          |        | 00: Not Present. No data is present in the emulator or in the file.                                |
|          |        | 01: ROM: 16KB of ROM data. Data is write protected in emulator.                                    |
|          |        | 02: RAM: No data in file. Bank is read/write in emulator.                                          |
|          |        | 03: RAM: Data present: data is loaded from the file and discarded on shutdown. Useful for testing. |
|          |        | 04: NVRAM: No Data in file. Memory is writeable. Emulator saves data to NVRAM file.                |
|          |        | 05: NVRAM: Data present. Memory is writeable. Emulator saves data to NVRAM file.                   |
| 512-end  |        | Payload data.                                                                                      |
|          |        | 16384 bytes per bank for types 1, 3, and 5.                                                        |
|          |        | 0 bytes for types 0,2, and 4.                                                                      |


For NVRAM banks: on shutdown, the emulator will write out an NVRAM file that contains the data of all of the NVRAM banks. The next time this cartridge is started, the NVRAM file will be loaded into any NVRAM bank. This overwrites any data present in NVRAM banks in the CRT file.

For types 00, 02, and 04: The file does *not* contain data for these bank types. Instead, the file skips straight to the next bank with initialized data (01, 03, or 05).

For all "No Data" banks, the data in RAM is *undefined*. While the emulator currently initializes RAM to 0 bytes, the hardware will have random values. In addition, unpopulated addresses will be "open collector" and will have unpredicatable results.

### Vectors

X16 hardware, and thus the emulator, will only read 6502 vectors out of bank 0. This is done via the CPU's VPB pin being connected to the ROM bank latch reset pin. In the past specific vectors were recommended in cartridge ROMs, but this is no longer true. In cartridges, the addresses `$FFFA`-`$FFFF` are free to use for data.


<!-- For PDF formatting -->
<div class="page-break"></div>

## MakeCart Conversion Tool

A conversion tool to pack cartridge data into a CRT file, `makecart`, is included in this release.

`-cfg <filename.cfg>`
Use this file to pack the cartridge data. Config file is simply the command line switches, one per line.

`-desc "Name/Description"`
Set the description field of the cartridge file. Up to 32 bytes of ASCII text.

`-author "Author Information"`
Set the author information field of the cartridge file. Up to 32 bytes of ASCII text.

`-copyright "Copyright Information"`
Set the copyright information field of the cartridge file. Up to 32 bytes of ASCII text.

`-version "version"`
Set the version information field of the cartridge file. Up to 32 bytes of ASCII text.

`-fill <value>`
Set the fill value to use with any partially-filled banks of cartridge memory. Value can be defined in decimal, or in hexadecimal with a '$' or '0x' prefix. 8-bit values will be repeated every byte, 16-bit values every two bytes, and 32-bit values every 4 bytes.

`-rom_file <start_bank> [<filename.bin> [<filename.bin>] ... ]`

Define rom banks from the specified list of files. File data is tightly packed -- if a file does not end on a 16KB interval, the next file will be inserted immediately after it within the same bank. If the last file does not end on a 16KB interval, the remainder of the rom will be filled with the value set by '-fill'.

Valid bank numbers are 32 - 255.

`-ram <start_bank> [<end bank>]`
Define one or more banks of RAM. RAM banks are not included in the payload.

`-ram_file <start_bank> [<filename.bin> [<filename.bin>] ... ]`
Define one or more banks of initialized RAM. Note that Initialized RAM banks are not saved to the NVRAM file at shutdown.

`-nvram <start_bank> [<end_bank>]`
Define one or more uninitalized nvram banks.

`-nvram_value <start_bank> <end_bank>`
Define pre-initialized nvram banks with the value set by '-fill'. Repeated payload bytes will be written to the file.

`-nvram_file <start_bank> [<filename.bin> [<filename.bin>] ... ]`

Define pre-initialized nvram banks from the specified list of files. File data is tightly packed like with -rom. If the last file does not end on a 16KB interval, the remainder of the rom will be filled with the value set by '-fill'.

`-none <start_bank> [<end_bank>]`
Define one or more unpopulated banks of the cartridge. By default, all banks are unpopulated unless specified by a previous command-line option. These banks are not present in the payload and only popualte the bank header in the CRT file.

`-o <output.crt>`
Set the filename of the output cartridge file.

All options can be specified multiple times, and are applied  in-order from left to right. For -desc and -o, it is legal to specify them multiple times but only the right-most instances of each will have effect.

`-unpack <input.crt> [<rom_size>]`
Unpacks the binary data from the cartridge file into `<rom_size>` slices. (for use with an EPROM programmer.) The ouptut files will be the same filename as the input file, with _### appended. This will also create a .cfg file that can be used to re-pack the files into a new CRT if needed.

The config file is just a series of command-line switches, with one item per line. This example assumes ladder.bin uses 3 banks, for a total of 48K, and that each level map is 4KB in size.

```
-o ladder.crt
-name "Ladder"
-author "Yahoo Software"
-copyright "(c) 1982, 1983 Yahoo Software"
-version "1.30TP"
-rom_file 32 ladder.bin
-rom_file 35 level_01.bin level_02.bin level_03.bin level_04.bin
-nvram 37
-fill 0
```

This would create file with

* 512 byte header
* 5 ROM banks
  * 3 for the 48K ladder.bin
  * 1 for the four 4KB level files.
* 1 empty NVRAM bank

Since the NVRAM bank is not initialized, it is not included in the file. This makes the file a total of 66,048 bytes long. (512 bytes, plus four 16KB banks.)


Web Site
--------

Commander X16: [https://commanderx16.com](https://commanderx16.com)

This fork: [https://github.com/xylothan/x16-emulator](https://github.com/xylothan/x16-emulator)

Forum
-----

[https://cx16forum.com/forum](https://cx16forum.com/forum/)


Contributing
------------

Issues and pull requests for **this fork's** features — the ImGui debugger, the DAP server,
source-level debugging, and the Windows builds — belong on [this fork's tracker][issues].

Anything that is really an upstream emulator or X16 platform issue is better reported
[upstream][upstream-issues], so the whole community benefits from the fix. We merge upstream
releases, so fixes there land here too.


License
-------

Copyright (c) 2019-2023 Michael Steil &lt;mist64@mac.com&gt;, [www.pagetable.com](https://www.pagetable.com/), et al.
All rights reserved. License: 2-clause BSD

Fork additions are released under the same 2-clause BSD license. Dear ImGui is vendored in
`src/extern/imgui/` under its own MIT license.


Release Notes
-------------
Fork releases are listed on the [releases page][releases]. The upstream release notes are in
[RELEASES](RELEASES.md#releases).


<!-------------------------------------------------------------------->
[releases]: https://github.com/xylothan/x16-emulator/releases
[issues]: https://github.com/xylothan/x16-emulator/issues
[upstream]: https://github.com/X16Community/x16-emulator
[upstream-releases]: https://github.com/X16Community/x16-emulator/releases
[upstream-issues]: https://github.com/X16Community/x16-emulator/issues
[webassembly]: https://github.com/xylothan/x16-emulator/blob/main/webassembly/WebAssembly.md
[x16docs]: https://github.com/X16Community/x16-docs
[x16rom-build]: https://github.com/X16Community/x16-rom#releases-and-building
[x16rom]: https://github.com/X16Community/x16-rom
[website]: https://commanderx16.com
[forum]: https://cx16forum.com/forum/

<!-- For PDF formatting -->
<div class="page-break"></div>
