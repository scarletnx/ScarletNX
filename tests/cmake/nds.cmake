# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the NDS driver (src/nds), included by src/nds/CMakeLists.txt: they run in its scope, so its variables
# hold here. Each makes the card image it runs (tests/support/nds_image.c): no game's data
set(SNX_NDS_PROGRAM "${PROJECT_BINARY_DIR}/scarletnx-nds${CMAKE_EXECUTABLE_SUFFIX}") # what the tests run

if(BUILD_TESTING)
    add_library(snx_test_nds_image STATIC "${PROJECT_SOURCE_DIR}/tests/support/nds_image.c")
    target_include_directories(snx_test_nds_image PUBLIC "${PROJECT_SOURCE_DIR}/tests")
    set_target_properties(snx_test_nds_image PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    # the board without its CPU: the loader, memory, the registers, the BIOS, the ARM7's answers, the geometry engine
    add_executable(snx-test-nds-board "${PROJECT_SOURCE_DIR}/tests/nds_board.c")
    target_link_libraries(snx-test-nds-board PRIVATE scarletnx-nds snx_test_nds_image)
    target_compile_options(snx-test-nds-board PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-nds-board PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
    add_test(NAME nds_board COMMAND snx-test-nds-board)
    # the machine end to end: a program compiled at run time, then ahead of time into a module and run from it
    add_executable(snx-test-nds-cpu "${PROJECT_SOURCE_DIR}/tests/nds_cpu.c")
    target_link_libraries(snx-test-nds-cpu PRIVATE snx_nds_program snx_test_nds_image)
    target_compile_options(snx-test-nds-cpu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-nds-cpu PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
    add_test(NAME nds_cpu COMMAND snx-test-nds-cpu)
    # the picture on the Vulkan device, read back: skipped (77) where the host has no Vulkan 1.3 device
    add_executable(snx-test-nds-vk "${PROJECT_SOURCE_DIR}/tests/nds_vk.c")
    target_link_libraries(snx-test-nds-vk PRIVATE scarletnx-nds snx_test_nds_image)
    target_compile_options(snx-test-nds-vk PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(snx-test-nds-vk PRIVATE VK_NO_PROTOTYPES)
    set_target_properties(snx-test-nds-vk PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
    add_test(NAME nds_vk COMMAND snx-test-nds-vk)
    set_tests_properties(nds_vk PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()

if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: this folder's tests
    snx_test_sanitize(snx_test_nds_image snx-test-nds-board snx-test-nds-cpu snx-test-nds-vk)
    snx_sanitizer_path()
endif()
