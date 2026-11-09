# SPDX-License-Identifier: MPL-2.0
# Copyright 2026 the mGBA contributors
# Copyright 2026 the ScarletNX contributors
# The AGB (Game Boy Advance) board runtime: the board emulation scarletnx-agb runs GBA ROMs on. The board
# library is independent of the NX and GNU ABI runtimes; the executable is its system type entry point.
#
#   scarletnx-agb-runtime   the board: cartridge, memory map, devices, and the AOT recompiler
#   snx_agb_program         the runner: the board in a window, and its command line, which scarletnx runs as
#                           scarletnx-agb (src/multicall)

# The ARM and Thumb recompiler of src/common/cpu/arm, the same one the other system types use.
if(NOT TARGET snx_arm_llvm)
    include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" agb-arm-backend)
endif()

add_library(scarletnx-agb STATIC board/board.c board/memory.c board/dma.c board/timers.c board/save.c
                                 cartridge/cartridge.c bios/bios.c ppu/ppu.c apu/apu.c cpu/memmap.c cpu/recomp.c
                                 cpu/cpu.c)
set_target_properties(scarletnx-agb PROPERTIES OUTPUT_NAME scarletnx-agb-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
# The LLVM backend (snx_llvm, which the Switch driver's tree, the GNU ABI runtime's or the top level configures):
# the module's objects and their helpers (armlib.c) linked by its linker, and the blocks a run reaches compiled in
# this process by its executor (exec.c), as the 3DS and the Vita do
target_link_libraries(scarletnx-agb PUBLIC scarletnx-common snx_arm_llvm snx_llvm)
target_compile_options(scarletnx-agb PRIVATE -Wall -Wextra -Werror)

# the runner: the board in a window, with sound and the keypad (runtime/host.c), SDL here and never in the board. Its
# command line goes beside every GBA game (recompiled/<name>/<name>, its .ini saying system=gba)
add_library(snx_agb_program STATIC runtime/host.c runtime/main.c)
target_link_libraries(snx_agb_program PUBLIC scarletnx-agb SDL3::SDL3 snx_window_icon snx_pad_sdl snx_build_info)
set_target_properties(snx_agb_program PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
set(SNX_AGB_PROGRAM "${PROJECT_BINARY_DIR}/scarletnx-agb${CMAKE_EXECUTABLE_SUFFIX}") # what the tests run

if(BUILD_TESTING)
    add_executable(snx-test-agb-board "${PROJECT_SOURCE_DIR}/tests/agb_board.c")
    target_link_libraries(snx-test-agb-board PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-board PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-board PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_board COMMAND snx-test-agb-board)
    add_executable(snx-test-agb-bios "${PROJECT_SOURCE_DIR}/tests/agb_bios.c")
    target_link_libraries(snx-test-agb-bios PRIVATE scarletnx-agb m)
    target_compile_options(snx-test-agb-bios PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-bios PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_bios COMMAND snx-test-agb-bios)
    add_executable(snx-test-agb-ppu "${PROJECT_SOURCE_DIR}/tests/agb_ppu.c")
    target_link_libraries(snx-test-agb-ppu PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-ppu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-ppu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_ppu COMMAND snx-test-agb-ppu)
    add_executable(snx-test-agb-apu "${PROJECT_SOURCE_DIR}/tests/agb_apu.c")
    target_link_libraries(snx-test-agb-apu PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-apu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-apu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_apu COMMAND snx-test-agb-apu)
    add_executable(snx-test-agb-cpu "${PROJECT_SOURCE_DIR}/tests/agb_cpu.c")
    target_link_libraries(snx-test-agb-cpu PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-cpu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-cpu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_cpu COMMAND snx-test-agb-cpu)
    add_executable(snx-test-agb-host "${PROJECT_SOURCE_DIR}/tests/agb_host.c")
    target_link_libraries(snx-test-agb-host PRIVATE snx_agb_program)
    target_compile_options(snx-test-agb-host PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-host PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_host COMMAND snx-test-agb-host)
    find_package(Python3 COMPONENTS Interpreter)
    # the runner's scripted presses, the BIOS's interrupt wait and its SoftReset, the open bus a write-only register
    # reads and an EEPROM save, each through a ROM the test makes
    if(Python3_Interpreter_FOUND)
        add_test(NAME agb_keys COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_keys.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_irq COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_irq.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_reset COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_reset.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_open_bus COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_open_bus.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_eeprom COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_eeprom.py"
            "${SNX_AGB_PROGRAM}")
    endif()
    # jsmolka's gba-tests (cmake/dependencies.cmake), each ROM a test of its own
    if(Python3_Interpreter_FOUND AND gba-tests_SOURCE_DIR)
        foreach(rom IN ITEMS arm thumb memory bios nes unsafe save_none save_sram save_flash64 save_flash128 ppu_hello
                        ppu_shades ppu_stripes)
            add_test(NAME agb_gba_${rom} COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_gba_tests.py"
                --runner "${SNX_AGB_PROGRAM}" --tests "${gba-tests_SOURCE_DIR}" ${rom})
        endforeach()
    endif()
endif()

if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: this folder's tests
    snx_test_sanitize(snx-test-agb-board snx-test-agb-bios snx-test-agb-ppu snx-test-agb-apu snx-test-agb-cpu
                      snx-test-agb-host)
    snx_sanitizer_path()
endif()
