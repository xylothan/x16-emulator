# Commander X16 Emulator - Windows packaging triplet.
#
# Link everything statically, including the CRT, so a release is a single
# self-contained x16emu.exe with no redistributable to install.
#
# FluidSynth is the one exception, and has to be. The emulator never links
# against it: midi.c loads libfluidsynth-3.dll at runtime with LoadLibrary and
# resolves each entry point with GetProcAddress, so MIDI support is whatever
# DLL happens to sit beside the executable. A static build would produce a .lib
# the emulator can never use, so build it as a DLL and ship that alongside.
#
# This also suits the licence: FluidSynth is LGPL-2.1-or-later, and loading it
# at runtime leaves users free to drop in their own build of it.
set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "fluidsynth")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
