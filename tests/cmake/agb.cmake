# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the AGB runtime (src/agb), included by src/agb/CMakeLists.txt where they were: they run in its scope, so
# its variables hold here, and a relative path is from src/agb
# jsmolka's gba-tests (MIT), ROMs that check the ARM7TDMI, the BIOS's open bus, the memory map, the save memory and
# the display, each leaving the number of its first failed test in a register. The AGB board's tests run them
# (tests/agb_gba_tests.py). Test data, not shipped
if(SNX_BUILD_AGB AND BUILD_TESTING)
    AddJsonPackage(NAME gba-tests DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    # DenSinH's FuzzARM (GPL-3.0): five ROMs of 10000 random ARM and Thumb instructions each, checked against the
    # values its generator computed (tests/agb_fuzzarm.py). Test data, not shipped
    AddJsonPackage(NAME fuzzarm DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
endif()
# mGBA's test suite (MIT), 13 sections of results measured on the console, as the ROM of mgba-emu/suite e694203 that
# scarletnx/mgba-suite-rom's release keeps (tests/agb_mgba_suite.py). Off by default, as DraVee asked of slow GBA tests:
# a section takes 5 to 14 s, the 13 about 90 s one after another in an -O0 tree (2026-11-13)
option(SNX_TEST_AGB_MGBA_SUITE "Run mGBA's test suite's sections as AGB tests (downloads its ROM; about 90 s in all)" OFF)
if(SNX_BUILD_AGB AND BUILD_TESTING AND SNX_TEST_AGB_MGBA_SUITE)
    snx_package_file(mgba-suite)
endif()

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
    # the save states: the frames after a state loaded are the frames the run that went on had (agb_state.c)
    add_executable(snx-test-agb-state "${PROJECT_SOURCE_DIR}/tests/agb_state.c")
    target_link_libraries(snx-test-agb-state PRIVATE snx_agb_program)
    target_compile_options(snx-test-agb-state PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-state PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_state COMMAND snx-test-agb-state "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(snx-test-agb-host "${PROJECT_SOURCE_DIR}/tests/agb_host.c")
    target_link_libraries(snx-test-agb-host PRIVATE snx_agb_program)
    target_include_directories(snx-test-agb-host PRIVATE "${PROJECT_SOURCE_DIR}/tests") # support/
    target_compile_options(snx-test-agb-host PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-host PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_host COMMAND snx-test-agb-host)
    # the window through Vulkan: the screen where SDL's renderer puts it, and its frames and notice under SDL's
    # offscreen driver (agb_host_vk.c)
    add_executable(snx-test-agb-host-vk "${PROJECT_SOURCE_DIR}/tests/agb_host_vk.c")
    target_link_libraries(snx-test-agb-host-vk PRIVATE snx_agb_program)
    target_compile_options(snx-test-agb-host-vk PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-host-vk PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_host_vk COMMAND snx-test-agb-host-vk)
    set_tests_properties(agb_host_vk PROPERTIES SKIP_RETURN_CODE 77)
    if(SNX_TRACY AND NOT WIN32)
        # a console's Tracy capture is not empty: F10's over a GBA window, read back by Tracy's tracy-csvexport
        # (agb_tracy.c); skipped without Tracy's tools, found on PATH or given by these two
        find_program(SNX_TRACY_CAPTURE_TOOL tracy-capture)
        find_program(SNX_TRACY_CSVEXPORT tracy-csvexport)
        add_executable(snx-test-agb-tracy "${PROJECT_SOURCE_DIR}/tests/agb_tracy.c")
        target_link_libraries(snx-test-agb-tracy PRIVATE snx_agb_program)
        target_compile_options(snx-test-agb-tracy PRIVATE -Wall -Wextra -Werror)
        set_target_properties(snx-test-agb-tracy PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        add_test(NAME agb_tracy COMMAND snx-test-agb-tracy "${SNX_TRACY_CAPTURE_TOOL}" "${SNX_TRACY_CSVEXPORT}")
        set_tests_properties(agb_tracy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
    find_package(Python3 COMPONENTS Interpreter)
    # the runner's scripted presses, the BIOS's interrupt wait and its SoftReset, the open bus a write-only register
    # reads and an EEPROM save, each through a ROM the test makes
    if(Python3_Interpreter_FOUND)
        add_test(NAME agb_keys COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_keys.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_irq COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_irq.py"
            "${SNX_AGB_PROGRAM}")
        # the run's unattended save states (state_save, state_load), done between two frames by its loop
        add_test(NAME agb_states COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_states.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_reset COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_reset.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_open_bus COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_open_bus.py"
            "${SNX_AGB_PROGRAM}")
        add_test(NAME agb_eeprom COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_eeprom.py"
            "${SNX_AGB_PROGRAM}")
        # the recompile's options every driver shares (common/utilities/cli.h), on a ROM the test makes
        add_test(NAME agb_recompile_options COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/agb_recompile_options.py" "${SNX_AGB_PROGRAM}")
    endif()
    # jsmolka's gba-tests (cmake/dependencies.cmake), each ROM a test of its own
    if(Python3_Interpreter_FOUND AND gba-tests_SOURCE_DIR)
        foreach(rom IN ITEMS arm thumb memory bios nes unsafe save_none save_sram save_flash64 save_flash128 ppu_hello
                        ppu_shades ppu_stripes)
            add_test(NAME agb_gba_${rom} COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_gba_tests.py"
                --runner "${SNX_AGB_PROGRAM}" --tests "${gba-tests_SOURCE_DIR}" ${rom})
        endforeach()
    endif()
    if(Python3_Interpreter_FOUND AND FuzzARM_SOURCE_DIR)
        foreach(rom IN ITEMS ARM_Any ARM_DataProcessing THUMB_Any THUMB_DataProcessing FuzzARM)
            add_test(NAME agb_fuzzarm_${rom} COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_fuzzarm.py"
                --runner "${SNX_AGB_PROGRAM}" --roms "${FuzzARM_SOURCE_DIR}" ${rom})
        endforeach()
    endif()
    # each section of mGBA's suite a test, labelled mgba-suite: the ones short of all their results pass at the count
    # they reach today, named with the issue that keeps them there, and fail at any other
    if(Python3_Interpreter_FOUND AND mgba-suite_SOURCE_DIR)
        foreach(section IN ITEMS memory io_read timing timer_count_up timer_irq shifter carry multiply_long bios_math
                                 dma sio_read sio_timing misc_edge:5:2058)
            string(REPLACE ":" ";" parts "${section}")
            list(GET parts 0 name)
            set(expect)
            if(section MATCHES ":")
                list(GET parts 1 passes)
                list(GET parts 2 issue)
                set(expect --expect ${passes} --issue "#${issue}")
            endif()
            add_test(NAME agb_mgba_${name} COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/agb_mgba_suite.py"
                --runner "${SNX_AGB_PROGRAM}" --rom "${mgba-suite_SOURCE_DIR}/suite.gba" ${expect} ${name})
            set_tests_properties(agb_mgba_${name} PROPERTIES LABELS "mgba-suite")
        endforeach()
    endif()
endif()

if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: this folder's tests
    snx_test_sanitize(snx-test-agb-board snx-test-agb-bios snx-test-agb-ppu snx-test-agb-apu snx-test-agb-cpu
                      snx-test-agb-state snx-test-agb-host snx-test-agb-host-vk)
    snx_sanitizer_path()
endif()
