cmake_minimum_required(VERSION 3.15)

string(ASCII 27 Esc)
set(ColorReset  "${Esc}[m")
set(ColorYellow "${Esc}[33m")
set(ColorGreen  "${Esc}[32m")
set(ColorRed    "${Esc}[31m")

project(x16emu LANGUAGES C CXX)

# --- Options (Replacements for Env Vars) ---
option(ENABLE_LTO "Enable Link Time Optimization" ON)

# Turning this off builds only the unit tests, which link no emulator core and so
# need neither SDL nor a ROM. Chiefly for CI and for working on the tests.
option(BUILD_EMULATOR "Build the x16emu executable and makecart" ON)

# --- Configuration & Standards ---
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(STATUS "[x16] LTO not supported: ${lto_error}")
    endif()
endif()

# Common compiler warnings/flags
if(MSVC)
    # /W3 = production-level warnings, /WX = warnings as errors
    # Suppress common harmless warnings from the codebase:
    #   C4244: conversion with possible loss of data (e.g., int to uint8_t)
    #   C4267: conversion from size_t to smaller type
    #   C4018: signed/unsigned mismatch in comparison
    #   C4996: deprecated POSIX function names (unlink, etc.)
    #   C4334: result of 32-bit shift implicitly converted to 64 bits
    #   C4098: void function returning a value
    #   C4305: truncation from double to float
    add_compile_options(/W3 /WX /wd4244 /wd4267 /wd4018 /wd4996 /wd4334 /wd4098 /wd4305)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Werror)
endif()

set(GIT_REV_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GIT_REV_HEADER "${GIT_REV_HEADER_DIR}/git_rev.h")
file(MAKE_DIRECTORY ${GIT_REV_HEADER_DIR})

find_package(Git)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/GetGitRevision.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/GetGitRevision.cmake"
    @ONLY
)

# Create a custom target that runs EVERY build
add_custom_target(
    update_git_rev
    ALL # Ensures this runs by default
    COMMAND ${CMAKE_COMMAND}
        -DCMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_FILE=${GIT_REV_HEADER}
        -P ${CMAKE_CURRENT_BINARY_DIR}/GetGitRevision.cmake
    BYPRODUCTS ${GIT_REV_HEADER}
    COMMENT "Checking Git revision..."
)

# --- Dependencies ---
# The unit tests link no emulator core, so a tests-only build (-DBUILD_EMULATOR=OFF)
# has no use for SDL or zlib and should not be blocked by their absence -- that is
# what lets the tests run on a bare machine and keeps them honestly independent of
# any one display backend. The exceptions are the tests whose sources reach SDL's
# headers through glue.h (code_map and timing_speed); those are skipped when SDL
# is absent rather than failing the build.
if(EMSCRIPTEN)
    # Emscripten: SDL2 is a built-in port, not a package we search for.
    # We create a "Dummy" target that passes the compiler flags (-s USE_SDL=2)
    # to any target that links against it.
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    target_compile_options(SDL2::SDL2 INTERFACE "SHELL:-s USE_SDL=2")
    target_link_options(SDL2::SDL2 INTERFACE "SHELL:-s USE_SDL=2")

    set(SDL2_FOUND TRUE)

    # Emscripten ZLIB (Built-in port)
    add_library(ZLIB::ZLIB INTERFACE IMPORTED)
    target_compile_options(ZLIB::ZLIB INTERFACE "SHELL:-s USE_ZLIB=1")
    target_link_options(ZLIB::ZLIB INTERFACE "SHELL:-s USE_ZLIB=1")

    set(ZLIB_FOUND TRUE)
else()
    if(BUILD_EMULATOR)
        find_package(SDL2 REQUIRED)
        find_package(ZLIB REQUIRED)
    else()
        find_package(SDL2 QUIET)
        find_package(ZLIB QUIET)
    endif()
    find_package(cJSON CONFIG QUIET)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(PkgConfig QUIET)

# Dear ImGui debugger
#
# An in-process debugger UI in its own window: memory, disassembly, CPU, VERA,
# source, breakpoints, symbols and call stack. Opt in at runtime with -imgui.
# AUTO builds it when the vendored sources are present, which they are in-tree;
# OFF drops the third-party C++ from the build for anyone who does not want it.

set(ENABLE_IMGUI "AUTO" CACHE STRING "Build the Dear ImGui debugger UI (ON/OFF/AUTO)")
set_property(CACHE ENABLE_IMGUI PROPERTY STRINGS ON OFF AUTO)

set(USE_IMGUI FALSE)
# imgui_impl_sdlrenderer2 needs SDL_RenderGeometry(), added in SDL 2.0.17. Some
# distributions still ship older SDL2 (Raspbian bullseye, Debian oldstable), so
# AUTO has to check rather than assume. find_package sets SDL2_VERSION in config
# mode; fall back to pkg-config, and treat "unknown" as too old rather than
# assume the best -- an unbuildable configuration is worse than a missing panel.
set(IMGUI_MIN_SDL_VERSION 2.0.17)
set(IMGUI_SDL_VERSION "${SDL2_VERSION}")
if(NOT IMGUI_SDL_VERSION AND PKG_CONFIG_FOUND)
    pkg_check_modules(IMGUI_PC_SDL2 QUIET sdl2)
    set(IMGUI_SDL_VERSION "${IMGUI_PC_SDL2_VERSION}")
endif()

if(ENABLE_IMGUI STREQUAL "OFF")
    message(STATUS "[x16] ImGui debugger explicitly disabled.")
elseif(EMSCRIPTEN)
    # SDL2's emscripten backend is single-window -- it renders to the one HTML
    # canvas -- so the second SDL_CreateWindow the debugger needs cannot
    # succeed. Forced off rather than left to fail at runtime, which also keeps
    # ~70k lines of dead C++ out of the .wasm download.
    if(ENABLE_IMGUI STREQUAL "ON")
        message(FATAL_ERROR "[x16] ENABLE_IMGUI=ON is not supported under Emscripten (single-window backend).")
    endif()
    message(STATUS "[x16] ImGui debugger disabled: unsupported under Emscripten.")
elseif(NOT IMGUI_SDL_VERSION OR IMGUI_SDL_VERSION VERSION_LESS IMGUI_MIN_SDL_VERSION)
    if(ENABLE_IMGUI STREQUAL "ON")
        message(FATAL_ERROR "[x16] ENABLE_IMGUI=ON needs SDL ${IMGUI_MIN_SDL_VERSION} or newer, found '${IMGUI_SDL_VERSION}'.")
    endif()
    message(STATUS "[x16] ImGui debugger disabled: SDL '${IMGUI_SDL_VERSION}' is older than ${IMGUI_MIN_SDL_VERSION} (SDL_RenderGeometry).")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/extern/imgui/imgui.cpp")
    set(USE_IMGUI TRUE)
    message(STATUS "[x16] ImGui debugger enabled (SDL ${IMGUI_SDL_VERSION}).")
elseif(ENABLE_IMGUI STREQUAL "ON")
    message(FATAL_ERROR "[x16] ENABLE_IMGUI=ON but src/extern/imgui is missing.")
else()
    message(STATUS "[x16] Vendored ImGui not found; building without the debugger UI.")
endif()

# Debug Adapter Protocol server
#
# Speaks DAP over a TCP socket so an editor can drive the emulator's debugger.
# Needs cJSON. AUTO builds it when cJSON is present and quietly leaves it out
# when it is not, so a checkout without the dependency still builds an emulator
# -- just one that rejects -dap. ON makes a missing cJSON an error instead.

set(ENABLE_DAP "AUTO" CACHE STRING "Enable the Debug Adapter Protocol server (ON/OFF/AUTO)")
set_property(CACHE ENABLE_DAP PROPERTY STRINGS ON OFF AUTO)

set(USE_DAP FALSE)
if(ENABLE_DAP STREQUAL "OFF")
    message(STATUS "[x16] DAP server explicitly disabled.")
elseif(TARGET cjson OR TARGET cjson::cjson OR cJSON_FOUND)
    set(USE_DAP TRUE)
elseif(ENABLE_DAP STREQUAL "ON")
    message(FATAL_ERROR "[x16] ENABLE_DAP=ON but cJSON was not found.")
else()
    message(STATUS "[x16] cJSON not found; building without the DAP server.")
endif()

# Fluidsynth

set(ENABLE_FLUIDSYNTH "AUTO" CACHE STRING "Enable FluidSynth MIDI support (ON/OFF/AUTO)")
set_property(CACHE ENABLE_FLUIDSYNTH PROPERTY STRINGS ON OFF AUTO)

set(USE_FLUIDSYNTH FALSE)

if(ENABLE_FLUIDSYNTH STREQUAL "OFF")
    message(STATUS "[x16] FluidSynth support explicitly disabled.")
elseif(EMSCRIPTEN)
    if (ENABLE_FLUIDSYNTH STREQUAL "ON")
        message(FATAL_ERROR "[x16] FluidSynth cannot be enabled for wasm builds.")
    elseif (ENABLE_FLUIDSYNTH STREQUAL "AUTO")
        message(STATUS "[x16] FluidSynth disabled for wasm builds.")
    endif()
else()
    # Try pkg-config first, then fall back to find_package (for vcpkg/MSVC)
    if(PKG_CONFIG_FOUND)
        if(ENABLE_FLUIDSYNTH STREQUAL "ON")
            pkg_check_modules(FLUIDSYNTH REQUIRED IMPORTED_TARGET fluidsynth>=2.0)
            set(USE_FLUIDSYNTH TRUE)
            message(STATUS "[x16] FluidSynth found and enabled.")
        else() 
            pkg_check_modules(FLUIDSYNTH IMPORTED_TARGET fluidsynth>=2.0)
            if(FLUIDSYNTH_FOUND)
                set(USE_FLUIDSYNTH TRUE)
                message(STATUS "[x16] FluidSynth found and enabled (auto).")
            else()
                message(WARNING "[x16] FluidSynth not found. MIDI support disabled (auto).")
            endif()
        endif()
    else()
        # Fallback: try find_package (works with vcpkg and system installs)
        find_package(FluidSynth CONFIG QUIET)
        if(FluidSynth_FOUND OR FLUIDSYNTH_FOUND)
            set(USE_FLUIDSYNTH TRUE)
            message(STATUS "[x16] FluidSynth found via find_package and enabled.")
        elseif(ENABLE_FLUIDSYNTH STREQUAL "ON")
            message(FATAL_ERROR "FluidSynth requested (ON), but neither pkg-config nor find_package found it.")
        else()
            message(WARNING "[x16] pkg-config missing and find_package didn't find FluidSynth. MIDI support disabled.")
        endif()
    endif()
endif()

# Trace Logging

set(ENABLE_TRACE "AUTO" CACHE STRING "Enable trace logging support (ON/OFF/AUTO)")
set_property(CACHE ENABLE_TRACE PROPERTY STRINGS ON OFF AUTO)

set(USE_TRACE FALSE)

if(ENABLE_TRACE STREQUAL "OFF")
    message(STATUS "[x16] Trace support explicitly disabled.")
elseif(EMSCRIPTEN)
    if (ENABLE_TRACE STREQUAL "ON")
        message(FATAL_ERROR "[x16] Trace cannot be enabled for wasm builds.")
    elseif (ENABLE_TRACE STREQUAL "AUTO")
        message(STATUS "[x16] Trace disabled for wasm builds.")
    endif()
else()
    if(ENABLE_TRACE STREQUAL "ON")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/rom_lst.h" AND
        EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/rom_labels.h")
            set(USE_TRACE ON)
            message(STATUS "[x16] Trace enabled and headers found (rom_lst.h, rom_labels.h).")
        else()
            message(FATAL_ERROR "[x16] Trace enabled but headers (rom_lst.h, rom_labels.h) not found.")
        endif()
    else()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/rom_lst.h" AND
        EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/rom_labels.h")
            set(USE_TRACE ON)
            message(STATUS "[x16] Trace enabled (auto) and headers found (rom_lst.h, rom_labels.h).")
        else()
            message(STATUS "[x16] Trace disabled (auto). Headers (rom_lst.h, rom_labels.h) not found.")
        endif()
    endif()
endif()

# --- Code Generation (Python Script) ---
# This replaces the 'cpu/tables.h' rule in the Makefile
set(TABLE_GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/buildtables.py")
set(TABLE_OUTPUT_H "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/tables.h")
set(MNEMONIC_OUTPUT_H "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/mnemonics.h")

add_custom_command(
    OUTPUT ${TABLE_OUTPUT_H} ${MNEMONIC_OUTPUT_H}
    COMMAND "${Python3_EXECUTABLE}" "${TABLE_GEN_SCRIPT}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu"
    DEPENDS "${TABLE_GEN_SCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/6502.opcodes"
    DEPENDS "${TABLE_GEN_SCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/65c02.opcodes"
    DEPENDS "${TABLE_GEN_SCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/65c816.opcodes"
    COMMENT "Generating CPU tables..."
)
# Create a target so the main executable can depend on it
add_custom_target(gen_cpu_tables DEPENDS ${TABLE_OUTPUT_H})

# --- Sources ---
set(X16_SOURCES
    src/cpu/fake6502.c
    src/memory.c
    src/io_trace.c
    src/disasm.c
    src/video.c
    src/i2c.c
    src/smc.c
    src/rtc.c
    src/via.c
    src/serial.c
    src/ieee.c
    src/vera_spi.c
    src/audio.c
    src/vera_pcm.c
    src/vera_psg.c
    src/sdcard.c
    src/sdcard_fat.c
    src/main.c
    src/debugger.c
    src/dbg_info.c
    src/dbg_load.c
    src/source_view.c
    src/insn_info.c
    src/code_map.c
    src/debug_core.c
    src/cpu/irq_ctx.c
    src/javascript_interface.c
    src/joystick.c
    src/rendertext.c
    src/keyboard.c
    src/icon.c
    src/timing.c
    src/wav_recorder.c
    src/testbench.c
    src/files.c
    src/cartridge.c
    src/iso_8859_15.c
    src/ymglue.cpp
    src/midi.c
    src/extern/ymfm/src/ymfm_opm.cpp
)

if(USE_IMGUI)
    # Vendored Dear ImGui (docking branch) + the SDL2 / SDLRenderer2 backends,
    # kept in their own list so the project's strict warnings can be relaxed
    # for just these files: ImGui does not build clean under /W3 /WX or
    # -Wall -Werror, and it is not ours to fix. The debug_ui sources are
    # deliberately NOT in this list -- see the warning settings below.
    set(IMGUI_SOURCES
        src/extern/imgui/imgui.cpp
        src/extern/imgui/imgui_draw.cpp
        src/extern/imgui/imgui_tables.cpp
        src/extern/imgui/imgui_widgets.cpp
        src/extern/imgui/imgui_demo.cpp
        src/extern/imgui/backends/imgui_impl_sdl2.cpp
        src/extern/imgui/backends/imgui_impl_sdlrenderer2.cpp
    )

    # The debug_ui module: a C-callable core plus self-registering panels.
    # Adding a panel means dropping a file under src/debug_ui/panels/ and
    # listing it here.
    set(DEBUG_UI_SOURCES
        src/debug_ui/debug_ui.cpp
        src/debug_ui/debug_ui_panels.cpp
        src/debug_ui/debug_ui_settings.cpp
        src/debug_ui/io_reg_info.cpp
        src/debug_ui/panels/memory_panel.cpp
        src/debug_ui/panels/disasm_panel.cpp
        src/debug_ui/panels/cpu_panel.cpp
        src/debug_ui/panels/vera_panel.cpp
        src/debug_ui/panels/source_panel.cpp
        src/debug_ui/panels/breakpoints_panel.cpp
        src/debug_ui/panels/symbols_panel.cpp
        src/debug_ui/panels/callstack_panel.cpp
        src/debug_ui/panels/psg_panel.cpp
        src/debug_ui/panels/fm_panel.cpp
        src/debug_ui/panels/pcm_panel.cpp
        src/debug_ui/panels/io_panel.cpp
    )

    list(APPEND X16_SOURCES ${IMGUI_SOURCES} ${DEBUG_UI_SOURCES})
endif()

if(WIN32)
    list(APPEND X16_SOURCES src/video_win32.c)
endif()

if(USE_DAP)
    list(APPEND X16_SOURCES src/debug_server.c)
endif()

# --- Main Executable Target ---
# Guarded so a tests-only build skips the emulator and its SDL dependency. The
# body is left un-indented deliberately: CMake ignores whitespace, and
# re-indenting ~150 lines would bury the one-line change in noise.
if(BUILD_EMULATOR)

add_executable(x16emu ${X16_SOURCES})

# Ensure the Python script runs before compiling x16emu
add_dependencies(x16emu gen_cpu_tables)

# Add git revision header
add_dependencies(x16emu update_git_rev)

# Includes
target_include_directories(x16emu PRIVATE
    src
    src/extern/include
    src/extern/ymfm/src
    ${GIT_REV_HEADER_DIR}
)

if(USE_IMGUI)
    target_include_directories(x16emu PRIVATE
        src/extern/imgui
        src/extern/imgui/backends
        src/extern/imgui_club
        src/debug_ui
    )
    target_compile_definitions(x16emu PRIVATE HAS_IMGUI)
    # Vendored ImGui does not build clean under this project's warning level,
    # and it is not ours to fix -- so warnings are silenced for those files
    # only. The debug_ui sources ARE ours and are held to the same standard as
    # the rest of the tree; suppressing them too would have hidden real defects
    # in the code most likely to contain them.
    if(MSVC)
        set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
    else()
        set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()

if(USE_TRACE)
    target_compile_definitions(x16emu PRIVATE TRACE)
endif()

# Linking
target_link_libraries(x16emu PRIVATE
    SDL2::SDL2
    ZLIB::ZLIB
)

if(TARGET SDL2::SDL2main)
    target_link_libraries(x16emu PRIVATE SDL2::SDL2main)
endif()

if(USE_DAP)
    target_compile_definitions(x16emu PRIVATE HAS_DAP)
    if(TARGET cjson::cjson)
        target_link_libraries(x16emu PRIVATE cjson::cjson)
    else()
        target_link_libraries(x16emu PRIVATE cjson)
    endif()
    if(WIN32)
        target_link_libraries(x16emu PRIVATE ws2_32)
    endif()
endif()

if(USE_FLUIDSYNTH)
    add_compile_definitions(HAS_FLUIDSYNTH)
    if(TARGET PkgConfig::FLUIDSYNTH)
        target_link_libraries(x16emu PRIVATE PkgConfig::FLUIDSYNTH)
    elseif(TARGET FluidSynth::libfluidsynth)
        target_link_libraries(x16emu PRIVATE FluidSynth::libfluidsynth)
    elseif(FLUIDSYNTH_LIBRARIES)
        target_link_libraries(x16emu PRIVATE ${FLUIDSYNTH_LIBRARIES})
        target_include_directories(x16emu PRIVATE ${FLUIDSYNTH_INCLUDE_DIRS})
    endif()
endif()

# Windows Specifics
if(WIN32)
    # MinGW Linker flags
    if(MINGW)
        target_link_options(x16emu PRIVATE
            -static-libstdc++
            -static-libgcc
            -Wl,--subsystem,console
        )
    endif()
    if(MSVC)
        target_link_options(x16emu PRIVATE /SUBSYSTEM:CONSOLE)
    endif()
    target_link_libraries(x16emu PRIVATE dwmapi)
endif()

# macOS Specifics
if(APPLE)

    find_library(COREAUDIO CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX AudioToolbox REQUIRED)
    find_library(FORCEFEEDBACK ForceFeedback REQUIRED)
    find_library(COREVIDEO CoreVideo REQUIRED)
    find_library(COCOA Cocoa REQUIRED)
    find_library(CARBON Carbon REQUIRED)
    find_library(IOKIT IOKit REQUIRED)
    find_library(ICONV iconv REQUIRED)
    find_library(QUARTZCORE QuartzCore REQUIRED)
    find_library(METAL Metal REQUIRED)
    find_library(COREHAPTICS CoreHaptics REQUIRED)
    find_library(GAMECONTROLLER GameController REQUIRED)

    target_link_libraries(x16emu PRIVATE
        ${COREAUDIO} ${AUDIOTOOLBOX} ${FORCEFEEDBACK}
        ${COREVIDEO} ${COCOA} ${CARBON} ${IOKIT} ${ICONV}
        ${QUARTZCORE} ${METAL} ${COREHAPTICS} ${GAMECONTROLLER}
    )
endif()

# Linux/Default Specifics
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(x16emu PRIVATE dl pthread m)
endif()

# Emscripten
if(EMSCRIPTEN)
    set_target_properties(x16emu PROPERTIES SUFFIX ".html")
    target_link_options(x16emu PRIVATE
        --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/webassembly/x16emu-template.html
        --preload-file ${CMAKE_CURRENT_SOURCE_DIR}/build/rom.bin
        "SHELL:-s TOTAL_MEMORY=32MB"
        "SHELL:-s ASSERTIONS=1"
        "SHELL:-s DISABLE_DEPRECATED_FIND_EVENT_TARGET_BEHAVIOR=1"
        "SHELL:-s EXPORTED_FUNCTIONS=['_j2c_reset','_j2c_paste','_j2c_start_audio','_main']"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
        "SHELL:-s USE_ZLIB=1"
        "SHELL:-s EXIT_RUNTIME=1"
    )
endif()

# --- Secondary Target: Makecart ---
add_executable(makecart
    src/makecart.c
    src/files.c
    src/cartridge.c
    src/makecart_javascript_interface.c
)

add_dependencies(makecart update_git_rev)

# Makecart dependencies
target_include_directories(makecart PRIVATE
    src
    src/extern/include
    ${GIT_REV_HEADER_DIR}
)
target_link_libraries(makecart PRIVATE
    ZLIB::ZLIB
    SDL2::SDL2
)

if(TARGET SDL2::SDL2main)
    target_link_libraries(makecart PRIVATE SDL2::SDL2main)
endif()

if(EMSCRIPTEN)
    set_target_properties(makecart PROPERTIES SUFFIX ".html")
    target_link_options(makecart PRIVATE "SHELL:-s USE_ZLIB=1")
endif()

endif() # BUILD_EMULATOR

# --- Optional: unit tests ---
# Every test is off by default so none of them affect the normal `x16emu` build.
# -DBUILD_TESTS=ON turns the lot on at once, which is what CI does; the per-test
# options remain for building a single one while working on it. Each test stubs
# out whatever it needs from the emulator core, so none of them link x16emu, and
# none of them need a ROM.
#
# Emscripten is excluded throughout: the output there is .js/.wasm, which ctest
# cannot execute as a plain binary.
option(BUILD_TESTS "Build every unit test" OFF)

# add_test() only writes a CTestTestfile when testing is enabled, so this has to
# come before the x16_add_test() calls below.
if(NOT EMSCRIPTEN)
    enable_testing()
endif()

# Everything x16_add_test() registers hangs off this, so CI can build the whole
# suite by name without naming each test -- and without a bare --build, which
# would drag in the emulator and duplicate what the platform jobs already report.
# Without it, "adding a test is one line" would be false: the workflow would be
# a second place to remember, and forgetting it leaves ctest running executables
# that were never compiled.
add_custom_target(unit_tests)

# x16_add_test(<name> SOURCES <src>... [NEEDS_SDL] [NEEDS_CPU_TABLES])
#
# Declares the per-test BUILD_<NAME>_TEST option, builds tests/<name>_test from
# SOURCES, and registers it with ctest. Adding a test is therefore one line in
# one place, which matters when many tests land in parallel: a shared block per
# test would make this file a standing merge conflict.
#
# NEEDS_SDL marks a test whose sources reach SDL's headers (via glue.h). Those
# are skipped rather than failing the build when SDL is absent, so the tests that
# genuinely stand alone still run on a machine without it.
function(x16_add_test name)
    cmake_parse_arguments(T "NEEDS_SDL;NEEDS_CPU_TABLES" "" "SOURCES" ${ARGN})

    string(TOUPPER "${name}" name_uc)
    option(BUILD_${name_uc}_TEST "Build the ${name} unit test" OFF)

    if(EMSCRIPTEN)
        return()
    endif()
    if(NOT BUILD_TESTS AND NOT BUILD_${name_uc}_TEST)
        return()
    endif()
    if(T_NEEDS_SDL AND NOT SDL2_FOUND)
        message(STATUS "SDL2 not found: skipping ${name} test")
        return()
    endif()

    add_executable(${name}_test ${T_SOURCES})
    target_include_directories(${name}_test PRIVATE src)
    if(T_NEEDS_SDL)
        target_include_directories(${name}_test PRIVATE ${SDL2_INCLUDE_DIRS})
    endif()
    if(T_NEEDS_CPU_TABLES)
        add_dependencies(${name}_test gen_cpu_tables)
    endif()
    add_dependencies(unit_tests ${name}_test)
    add_test(NAME ${name} COMMAND ${name}_test)
endfunction()

# dbg_info.c has no dependency on the emulator core, so this links it on its own.
x16_add_test(dbg_info
    SOURCES tests/test_dbg_info.c src/dbg_info.c)

# code_map.c leans on only disasm(), dbg_info and two core symbols, so the test
# supplies its own memory and register file rather than linking the emulator
# core. disasm.c still includes SDL headers, and the generated cpu/mnemonics.h.
x16_add_test(code_map NEEDS_SDL NEEDS_CPU_TABLES
    SOURCES tests/test_code_map.c src/code_map.c src/disasm.c src/dbg_info.c)

# debug_core.c reaches for only the register file, the bank registers and a debug
# memory read, so the test supplies its own and links no emulator core.
x16_add_test(debug_core
    SOURCES tests/test_debug_core.c src/debug_core.c)

# The speed arithmetic in timing.c depends only on the machine's configured
# clock, so the test supplies the few emulator globals it touches.
x16_add_test(timing_speed NEEDS_SDL
    SOURCES tests/test_timing_speed.c src/timing.c)

# irq_ctx.c is pure bookkeeping with no dependencies, so the test links it alone.
x16_add_test(irq_ctx
    SOURCES tests/test_irq_ctx.c src/cpu/irq_ctx.c)

# dbg_load.c reports loads to dbg_info and source_view, so the test links those
# two alongside it. dbg_info.c has no dependency on the emulator core either.
x16_add_test(dbg_load
    SOURCES tests/test_dbg_load.c src/dbg_load.c src/dbg_info.c src/source_view.c)

# vera_pcm.c includes only its own header and libc, so the test links it alone.
x16_add_test(vera_pcm
    SOURCES tests/test_vera_pcm.c tests/support/harness.c src/vera_pcm.c)

# io_trace.c is deliberately free of emulator dependencies -- memory.c calls it,
# and memory.c is linked into tests that pair it with fake devices -- so the
# test links it alone, with no CPU, no memory and no SDL.
x16_add_test(io_trace
    SOURCES tests/test_io_trace.c tests/support/harness.c src/io_trace.c)

# The FAT index parses untrusted binary data, so the test builds synthetic FAT16
# and FAT32 images and reads them back. It stubs the five file-access functions
# onto stdio rather than linking src/files.c, which would drag in SDL and zlib
# to test nothing extra: what is under test is the parser.
x16_add_test(sdcard_fat
    SOURCES tests/test_sdcard_fat.c tests/support/harness.c src/sdcard_fat.c)

# The CPU core reaches outside itself for only six symbols, all supplied by the
# fixture, so this links the real instruction tables against a flat 64K memory
# and nothing else. irq_ctx.c comes along because fake6502.c reports interrupt
# entry and exit to it.
x16_add_test(cpu_smoke NEEDS_CPU_TABLES
    SOURCES tests/test_cpu_smoke.c tests/support/cpu_fixture.c tests/support/harness.c
            src/cpu/fake6502.c src/cpu/irq_ctx.c)

x16_add_test(cpu_loadstore NEEDS_CPU_TABLES
    SOURCES tests/test_cpu_loadstore.c tests/support/cpu_fixture.c tests/support/harness.c
            src/cpu/fake6502.c src/cpu/irq_ctx.c)

x16_add_test(cpu_adc_sbc NEEDS_CPU_TABLES
    SOURCES tests/test_cpu_adc_sbc.c tests/support/cpu_fixture.c tests/support/harness.c
            src/cpu/fake6502.c src/cpu/irq_ctx.c)

x16_add_test(cpu_decimal NEEDS_CPU_TABLES
    SOURCES tests/test_cpu_decimal.c tests/support/cpu_fixture.c tests/support/harness.c
            src/cpu/fake6502.c src/cpu/irq_ctx.c)

x16_add_test(cpu_steprun NEEDS_CPU_TABLES
    SOURCES tests/test_cpu_steprun.c tests/support/cpu_fixture.c tests/support/harness.c
            src/cpu/fake6502.c src/cpu/irq_ctx.c)

# Klaus Dormann's functional tests: a program that walks every opcode and
# checks its own results. The binaries are fetched, not committed, and the test
# reports skipped without them.
x16_add_test(klaus NEEDS_CPU_TABLES
    SOURCES tests/test_klaus.c tests/support/cpu_fixture.c tests/support/harness.c
            src/cpu/fake6502.c src/cpu/irq_ctx.c)

# SingleStepTests/ProcessorTests: every opcode, thousands of randomised states
# each. Fixtures are fetched by tests/fetch_pt.py and not committed.
x16_add_test(processor_tests NEEDS_CPU_TABLES
    SOURCES tests/test_processor_tests.c tests/support/cpu_fixture.c
            tests/support/harness.c src/cpu/fake6502.c src/cpu/irq_ctx.c)

# The same binary over the 65816 suites. One executable rather than three: the
# fixtures differ only in which cases they hold, and each case says which core
# to run it on. Registered separately so a failure names the mode.
if(TARGET processor_tests_test)
    add_test(NAME processor_tests_816_emu
             COMMAND processor_tests_test 816-emu)
    add_test(NAME processor_tests_816_native
             COMMAND processor_tests_test 816-native)
endif()

# The generated opcode tables against the published 65C02 spec. ProcessorTests
# measures what the hardware does; this checks the tables still agree with the
# documentation, which is a different question and has disagreed once.
add_test(NAME opcode_spec
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_opcode_spec.py")

# The VERA tests cite Verilog by file and line, which only means anything
# against one revision -- the one the emulator reports itself to be. This fails
# if src/video.c and the RTL pin in tests/fetch_vera_rtl.py drift apart, so a
# version bump has to be a deliberate step that re-checks the citations.
add_test(NAME vera_version
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_vera_version.py")

# memory.c routes every I/O access, and carries the debugOn flag that says
# whether a read is the debugger looking or the machine running. The devices
# behind it are recording fakes; memory.c, debug_core.c and the CPU core are
# real, so this exercises the actual routing, watchpoint and cycle accounting.
x16_add_test(debugon_contract NEEDS_SDL NEEDS_CPU_TABLES
    SOURCES tests/test_debugon_contract.c tests/support/fake_devices.c
            tests/support/harness.c
            src/memory.c src/debug_core.c src/cpu/fake6502.c src/cpu/irq_ctx.c
            src/io_trace.c)

# The watchpoint hook sits on the store path, so this links the same real
# memory.c, debug_core.c and CPU core as the debugOn tests, and asserts that
# watching an address changes nothing except that a break is reported.
x16_add_test(watchpoint_purity NEEDS_SDL NEEDS_CPU_TABLES
    SOURCES tests/test_watchpoint_purity.c tests/support/fake_devices.c
            tests/support/signature.c tests/support/harness.c
            src/memory.c src/debug_core.c src/cpu/fake6502.c src/cpu/irq_ctx.c
            src/io_trace.c)

# Build summary

if(ENABLE_FLUIDSYNTH STREQUAL "AUTO" AND NOT USE_FLUIDSYNTH)
    set(FLUID_EN_MSG "${ColorYellow}DISABLED (Missing Dependency)${ColorReset}")
elseif(USE_FLUIDSYNTH)
    set(FLUID_EN_MSG "${ColorGreen}ENABLED${ColorReset}")
else()
    set(FLUID_EN_MSG "DISABLED (Explicit)")
endif()

if(ENABLE_TRACE STREQUAL "AUTO" AND NOT USE_TRACE)
    set(TRACE_EN_MSG "${ColorYellow}DISABLED (Missing ROM Headers)${ColorReset}")
elseif(USE_TRACE)
    set(TRACE_EN_MSG "${ColorGreen}ENABLED${ColorReset}")
else()
    set(TRACE_EN_MSG "DISABLED (Explicit)")
endif()

message(STATUS "\n")
message(STATUS "=========================================================================")
message(STATUS "   x16-emulator Build Options Summary")
message(STATUS "=========================================================================")
message(STATUS "   Fluidsynth Support : ${FLUID_EN_MSG}")
message(STATUS "   Trace Support      : ${TRACE_EN_MSG}")
message(STATUS "=========================================================================\n")
