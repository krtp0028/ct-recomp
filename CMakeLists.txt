cmake_minimum_required(VERSION 3.20)
project(ct_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(WIN32)
    # MinGW x64 maps setjmp to the SEH-aware _setjmp, whose longjmp unwinds
    # via RtlUnwind: that fails with STATUS_BAD_STACK on the scheduler's
    # coroutine stack (runtime/compat/ucontext_win.c) and forces unwinding
    # the tests' fatal-hook jumps never wanted. The non-SEH setjmp restores
    # registers only, like glibc's.
    add_compile_definitions(__USE_MINGW_SETJMP_NON_SEH)
endif()

# ccache, when installed: rebuilding after a branch switch or a clean costs
# only what changed.
find_program(CT_CCACHE ccache)
if(CT_CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER ${CT_CCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CT_CCACHE})
endif()

# Investigation builds: -DCMAKE_BUILD_TYPE=Debug (e.g. build-dbg/) builds the
# runtime at -Og -g and the generated code (game/*/out, the bulk of the
# build) at CT_GEN_OPT, -O1 by default: quick to build, fast enough to run a
# replay to where a problem is.
set(CT_GEN_OPT "-O1" CACHE STRING "Optimization for generated code in Debug builds")
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_C_FLAGS_DEBUG MATCHES "-O")
    # -Og: still debuggable, but replays run near Release speed
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Og")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Og")
endif()

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(CT_WARN -Wall -Wextra -Wpedantic)

enable_testing()

# The PSP port builds the engine, the generated game code and ct_psp (see
# game/ct/CMakeLists.txt); the host-side tests and tools are skipped.
# pspdev's toolchain (psp-cmake) sets PSP (and CMAKE_SYSTEM_NAME=Generic).
if(PSP OR CMAKE_SYSTEM_NAME STREQUAL "PSP")
    set(CT_PSP ON)
endif()

# The translator (recomp/), runtime, frontend, and engine tests are
# game-agnostic; each game under game/ supplies its symbols, generated
# code, and ROM-dependent tests. check_agnostic enforces the split.

# ---- vendored SNES core (third_party/snes/, see its README.md) ----
add_library(ct_snes_vendor STATIC
    third_party/snes/ppu.c third_party/snes/dma.c
    third_party/snes/apu.c third_party/snes/spc.c third_party/snes/dsp.c)
target_include_directories(ct_snes_vendor PUBLIC third_party/snes third_party)
# Not our code; don't hold it to our warning flags.

# ---- runtime ----
add_library(ct_runtime STATIC runtime/cpu.c runtime/bus.c
    runtime/snes_adapter.c runtime/png.c runtime/wav.c runtime/hwlog.c
    runtime/cycles.c runtime/replay.c runtime/resample.c
    runtime/spc700_host.cpp)
target_include_directories(ct_runtime PUBLIC runtime)
target_link_libraries(ct_runtime PUBLIC ct_snes_vendor m)
# ares's SPC700 core (ISC), compiled inside runtime/spc700_host.cpp.
target_include_directories(ct_runtime PRIVATE third_party/ares/spc700)
target_compile_options(ct_runtime PRIVATE ${CT_WARN})

# Windows has no <ucontext.h>; runtime/compat provides one with a
# register-level context switch. The PSP (newlib) needs its own backend: a
# MIPS o32 switch.
if(WIN32)
    target_sources(ct_runtime PRIVATE runtime/compat/ucontext_win.c)
    target_include_directories(ct_runtime PUBLIC runtime/compat)
elseif(CT_PSP)
    target_sources(ct_runtime PRIVATE runtime/compat/ucontext_psp.c)
    target_include_directories(ct_runtime PUBLIC runtime/compat)
endif()

# Function tables (func_table.h): a game's generated ct_funcs.c, or these
# empty ones. OBJECT libraries, so the tables are always on the link line
# ahead of the interpreter that reads them.
add_library(ct_tables_empty OBJECT runtime/func_table_empty.c)
target_link_libraries(ct_tables_empty PUBLIC ct_runtime)
target_compile_options(ct_tables_empty PRIVATE ${CT_WARN})

# ---- interpreter: strict test oracle, system mode from reset (interp.h) ----
add_library(ct_interp STATIC runtime/interp.c)
target_link_libraries(ct_interp PUBLIC ct_runtime)
target_compile_options(ct_interp PRIVATE ${CT_WARN})

# ---- frame scheduler (sched.h) ----
add_library(ct_sched STATIC runtime/sched.c runtime/overlay.c)
target_link_libraries(ct_sched PUBLIC ct_interp)
target_compile_options(ct_sched PRIVATE ${CT_WARN})

# ---- SDL2 input mapping (optional: skipped when SDL2 isn't installed) ----
if(NOT CT_PSP)
    find_package(SDL2 CONFIG QUIET)
    if(SDL2_FOUND)
        add_library(ct_input STATIC frontend/input.c)
        target_include_directories(ct_input PUBLIC frontend)
        target_link_libraries(ct_input PUBLIC SDL2::SDL2)
        target_compile_options(ct_input PRIVATE ${CT_WARN})
    else()
        message(STATUS "SDL2 not found: frontends not built")
    endif()
endif()

if(NOT CT_PSP)
# ---- engine tests: no game, no ROM (synthetic image, test/harness.h) ----
set(CT_TEST_ROM ${CMAKE_BINARY_DIR}/test_rom.sfc)
add_custom_command(
    OUTPUT ${CT_TEST_ROM}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_test_rom.py ${CT_TEST_ROM}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/make_test_rom.py
    COMMENT "synthetic test ROM"
    VERBATIM)
add_custom_target(ct_test_rom DEPENDS ${CT_TEST_ROM})

function(ct_engine_test name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE test)
    target_link_libraries(${name} ct_sched ct_tables_empty)
    target_compile_options(${name} PRIVATE ${CT_WARN})
    target_compile_definitions(${name} PRIVATE CT_TEST_ROM="${CT_TEST_ROM}")
    add_dependencies(${name} ct_test_rom)
endfunction()

ct_engine_test(test_runtime test/test_runtime.c)
add_test(NAME runtime COMMAND test_runtime)
foreach(f open_bus rom_write unhooked decimal entry)
    add_test(NAME runtime_fatal_${f} COMMAND test_runtime fatal_${f})
endforeach()
set_tests_properties(runtime_fatal_open_bus PROPERTIES PASS_REGULAR_EXPRESSION "ct: read8 \\$006000: open bus")
set_tests_properties(runtime_fatal_rom_write PROPERTIES PASS_REGULAR_EXPRESSION "ct: write8 \\$E50000: write to ROM")
set_tests_properties(runtime_fatal_unhooked PROPERTIES PASS_REGULAR_EXPRESSION "ct: read8 \\$004016: unhooked")
set_tests_properties(runtime_fatal_decimal PROPERTIES PASS_REGULAR_EXPRESSION "ct: \\$E50000: ADC with D=1")
set_tests_properties(runtime_fatal_entry PROPERTIES PASS_REGULAR_EXPRESSION "ct: \\$C08100: entry state m0 x0 e0, expected m1 x0")

foreach(t snes_bus hw_map interp_modes interp_system sched stack_guard replay resample dsp ppu_sprites)
    ct_engine_test(test_${t} test/test_${t}.c)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()

# Native dispatch: provides its own function table instead of the empty one.
add_executable(test_native test/test_native.c)
target_include_directories(test_native PRIVATE test)
target_link_libraries(test_native ct_sched)
target_compile_options(test_native PRIVATE ${CT_WARN})
target_compile_definitions(test_native PRIVATE CT_TEST_ROM="${CT_TEST_ROM}")
add_dependencies(test_native ct_test_rom)
add_test(NAME native COMMAND test_native)

# tools/lockstep.py's divergence report, against a fake probe.
add_test(NAME lockstep_report COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_lockstep.py)
add_test(NAME ref_compare_report COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_ref_compare.py)
add_test(NAME ctest_cache_report COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_ctest_cache.py)

# TAS movie converter on synthetic .bk2/.lsmv movies.
add_test(NAME tas_convert COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_tas_convert.py)
add_test(NAME decode_calls COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_decode_calls.py)
add_test(NAME batch_runs COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_batch_runs.py)
add_test(NAME funcs_meta COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_funcs_meta.py)

if(SDL2_FOUND)
    add_executable(test_input test/test_input.c)
    target_link_libraries(test_input ct_input)
    target_compile_options(test_input PRIVATE ${CT_WARN})
    add_test(NAME input COMMAND test_input)
endif()

# No game-specific names, addresses, or words outside game/.
add_test(NAME check_agnostic
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_agnostic.py ${CMAKE_SOURCE_DIR})
endif()   # not CT_PSP

# ---- games (need $CT_ROM; their tests are LABELS rom) ----
if(DEFINED ENV{CT_ROM})
    set(CT_ROM $ENV{CT_ROM})
    add_subdirectory(game/ct)
else()
    message(STATUS "CT_ROM not set: game builds and ROM tests skipped")
endif()

# ---- test environment (keep last) ----
# SDL dummy drivers: the frontend test runs headless.
if(NOT CT_PSP)
    get_property(ct_engine_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${ct_engine_tests} PROPERTIES ENVIRONMENT
        "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
