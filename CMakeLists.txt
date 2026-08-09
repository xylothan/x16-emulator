cmake_minimum_required(VERSION 3.15)

string(ASCII 27 Esc)
set(ColorReset  "${Esc}[m")
set(ColorYellow "${Esc}[33m")
set(ColorGreen  "${Esc}[32m")
set(ColorRed    "${Esc}[31m")

project(x16emu LANGUAGES C CXX)

# --- Options (Replacements for Env Vars) ---
option(ENABLE_LTO "Enable Link Time Optimization" ON)

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
    find_package(SDL2 REQUIRED)
    find_package(ZLIB REQUIRED)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(PkgConfig QUIET)

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
    src/main.c
    src/debugger.c
    src/dbg_info.c
    src/source_view.c
    src/code_map.c
    src/debug_core.c
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

if(WIN32)
    list(APPEND X16_SOURCES src/video_win32.c)
endif()

# --- Main Executable Target ---
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

# --- Optional: cc65 debug-info parser checks ---
# Off by default so it never affects the normal `x16emu` build. Enable with
# -DBUILD_DBG_INFO_TEST=ON to build the standalone parser checks. dbg_info.c
# has no dependency on the emulator core, so this links it on its own.
option(BUILD_DBG_INFO_TEST "Build the cc65 debug-info parser test" OFF)
if(BUILD_DBG_INFO_TEST AND NOT EMSCRIPTEN)
    add_executable(dbg_info_test
        tests/test_dbg_info.c
        src/dbg_info.c
    )
    target_include_directories(dbg_info_test PRIVATE src)
endif()

# --- Optional: anchored disassembler checks ---
# Off by default, same as above. code_map.c leans on only disasm(), dbg_info and
# two core symbols, so the test supplies its own memory and register file rather
# than linking the emulator core. disasm.c still includes SDL headers, hence the
# SDL include directories.
option(BUILD_CODE_MAP_TEST "Build the anchored disassembler test" OFF)
if(BUILD_CODE_MAP_TEST AND NOT EMSCRIPTEN)
    add_executable(code_map_test
        tests/test_code_map.c
        src/code_map.c
        src/disasm.c
        src/dbg_info.c
    )
    target_include_directories(code_map_test PRIVATE src ${SDL2_INCLUDE_DIRS})
endif()

# --- Optional: breakpoint core checks ---
# Off by default, same as above. debug_core.c reaches for only the register file,
# the bank registers and a debug memory read, so the test supplies its own and
# links no emulator core.
option(BUILD_DEBUG_CORE_TEST "Build the debugger breakpoint core test" OFF)
if(BUILD_DEBUG_CORE_TEST AND NOT EMSCRIPTEN)
    add_executable(debug_core_test
        tests/test_debug_core.c
        src/debug_core.c
    )
    target_include_directories(debug_core_test PRIVATE src)
endif()

# --- Optional: emulation-speed control checks ---
# Off by default, same as above. The speed arithmetic in timing.c depends only
# on the machine's configured clock, so the test supplies the few emulator
# globals it touches and links nothing else.
option(BUILD_TIMING_SPEED_TEST "Build the emulation-speed control test" OFF)
if(BUILD_TIMING_SPEED_TEST AND NOT EMSCRIPTEN)
    add_executable(timing_speed_test
        tests/test_timing_speed.c
        src/timing.c
    )
    target_include_directories(timing_speed_test PRIVATE src ${SDL2_INCLUDE_DIRS})
endif()

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
