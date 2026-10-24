# SPDX-License-Identifier: MPL-2.0
# Copyright 2026 the mGBA contributors
# Copyright 2026 the ScarletNX contributors
# The AGB (Game Boy Advance) board runtime: the board emulation scarletnx-agb runs GBA ROMs on. The board
# library is independent of the NX and GNU ABI runtimes; the executable is its system type entry point.
#
#   scarletnx-agb-runtime   the board: cartridge, memory map, devices, and the AOT recompiler
#   scarletnx-agb-run       the system type's executable, named scarletnx-agb

# The ARM and Thumb recompiler of src/common/cpu/arm, the same one the other system types use.
if(NOT TARGET snx_arm_llvm)
    include("${CMAKE_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/arm" agb-arm-backend)
endif()

add_library(scarletnx-agb STATIC board/board.c board/memory.c board/dma.c board/timers.c board/save.c
                                 cartridge/cartridge.c bios/bios.c ppu/ppu.c apu/apu.c cpu/memmap.c cpu/recomp.c
                                 cpu/cpu.c)
set_target_properties(scarletnx-agb PROPERTIES OUTPUT_NAME scarletnx-agb-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_link_libraries(scarletnx-agb PUBLIC scarletnx-common snx_arm_llvm)
target_compile_definitions(scarletnx-agb PRIVATE SNX_AGB_CC="${CMAKE_C_COMPILER}")
target_compile_options(scarletnx-agb PRIVATE -Wall -Wextra -Werror)
# the blocks a run reaches, compiled in this process by the LLVM backend's executor (exec.c, in snx_llvm, which the
# Switch driver's build defines), as the 3DS and the Vita do; without it each batch is a module linked by the compiler
if(TARGET snx_llvm)
    target_link_libraries(scarletnx-agb PUBLIC snx_llvm)
    target_compile_definitions(scarletnx-agb PUBLIC SNX_AGB_JIT) # the runner says how its blocks are kept
endif()
# The runtime library the blocks call (cpu/runtime.h), which the ROM's module links as the 3DS driver's does
set(agb_runtime_source "${CMAKE_CURRENT_BINARY_DIR}/agb-runtime-$<CONFIG>.c")
add_custom_command(OUTPUT "${agb_runtime_source}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:snx_arm_rt>" "-DOUTPUT=${agb_runtime_source}"
        -DSYMBOL=agb_arm_runtime -DHEADER=agb/cpu/runtime.h -P "${PROJECT_SOURCE_DIR}/cmake/ctr-runtime.cmake"
    DEPENDS snx_arm_rt "${PROJECT_SOURCE_DIR}/cmake/ctr-runtime.cmake" VERBATIM)
target_sources(scarletnx-agb PRIVATE "${agb_runtime_source}")

# the runner: the board in a window, with sound and the keypad (runtime/host.c), SDL here and never in the board
add_executable(scarletnx-agb-run runtime/main.c runtime/host.c)
target_link_libraries(scarletnx-agb-run PRIVATE scarletnx-agb SDL3::SDL3 snx_window_icon)
set_target_properties(scarletnx-agb-run PROPERTIES OUTPUT_NAME scarletnx-agb
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" C_STANDARD 23 C_STANDARD_REQUIRED ON)
# after each build the runner is copied beside every GBA game (recompiled/<name>/<name>, its .ini saying system=gba),
# as scarletnx is beside every Switch game (cmake/deploy.cmake). A sanitized test build's stays here
if(SNX_DEPLOY AND NOT (SNX_TEST_SANITIZERS AND BUILD_TESTING))
    add_custom_command(TARGET scarletnx-agb-run POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DAGB_RUNTIME=$<TARGET_FILE:scarletnx-agb-run>" "-DGAMES=${SNX_RECOMPILED_DIR}"
                "-DWINDOWS=${WIN32}" "-DMACOS=${APPLE}" -P "${PROJECT_SOURCE_DIR}/cmake/deploy.cmake"
        VERBATIM)
endif()

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
    target_compile_definitions(snx-test-agb-cpu PRIVATE SNX_AGB_TEST_CC="${CMAKE_C_COMPILER}")
    target_compile_options(snx-test-agb-cpu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-cpu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_cpu COMMAND snx-test-agb-cpu)
    add_executable(snx-test-agb-host "${PROJECT_SOURCE_DIR}/tests/agb_host.c" runtime/host.c)
    target_link_libraries(snx-test-agb-host PRIVATE scarletnx-agb SDL3::SDL3 snx_window_icon)
    target_compile_options(snx-test-agb-host PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-host PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_host COMMAND snx-test-agb-host)
    find_package(Python3 COMPONENTS Interpreter)
    # the runner's scripted presses, the BIOS's interrupt wait and its SoftReset, and an EEPROM save, each through a ROM
    # the test makes
    if(Python3_Interpreter_FOUND)
        add_test(NAME agb_keys COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_keys.py"
            $<TARGET_FILE:scarletnx-agb-run>)
        add_test(NAME agb_irq COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_irq.py"
            $<TARGET_FILE:scarletnx-agb-run>)
        add_test(NAME agb_reset COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_reset.py"
            $<TARGET_FILE:scarletnx-agb-run>)
        add_test(NAME agb_eeprom COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_eeprom.py"
            $<TARGET_FILE:scarletnx-agb-run>)
    endif()
    # jsmolka's gba-tests (cmake/dependencies.cmake), each ROM a test of its own
    if(Python3_Interpreter_FOUND AND gba-tests_SOURCE_DIR)
        foreach(rom IN ITEMS arm thumb memory bios nes unsafe save_none save_sram save_flash64 save_flash128 ppu_hello
                        ppu_shades ppu_stripes)
            add_test(NAME agb_gba_${rom} COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_gba_tests.py"
                --runner $<TARGET_FILE:scarletnx-agb-run> --tests "${gba-tests_SOURCE_DIR}" ${rom})
        endforeach()
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS scarletnx-agb-run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: this folder's tests
    snx_test_sanitize(snx-test-agb-board snx-test-agb-bios snx-test-agb-ppu snx-test-agb-apu snx-test-agb-cpu
                      snx-test-agb-host)
    snx_sanitizer_path()
endif()
