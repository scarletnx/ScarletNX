# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
if(Python3_Interpreter_FOUND) # tests/CMakeLists.txt looked it up
    # in a sanitized build, also that every program the tests run, and what it links, is built with the sanitizers
    if(SNX_TEST_SANITIZERS)
        set(snx_architecture_sanitizers --sanitizers --ctest "${CMAKE_CTEST_COMMAND}")
    endif()
    add_test(NAME runtime_architecture
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/architecture.py"
            --repo "${PROJECT_SOURCE_DIR}" --build "${PROJECT_BINARY_DIR}" --cmake "${CMAKE_COMMAND}"
            ${snx_architecture_sanitizers})
    set_tests_properties(runtime_architecture PROPERTIES TIMEOUT 180)
    add_test(NAME architecture_checker COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/architecture_test.py")
    add_test(NAME gnuabi_differential_harness COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/gnuabi_diff_test.py")
    if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND SNX_TEST_VULKAN)
        add_test(NAME nx_runtime_homebrew
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/nx_runtime_smoke.py"
                --runtime $<TARGET_FILE:scarletnx> --repo "${PROJECT_SOURCE_DIR}"
                --artifact-dir "${PROJECT_BINARY_DIR}/nx-smoke-failures")
        set_tests_properties(nx_runtime_homebrew PROPERTIES TIMEOUT 150)
        # again as on a host with 64 KiB pages (PowerPC64, some AArch64): views copy, a host page holds sixteen guest
        # pages (mem.c)
        add_test(NAME nx_runtime_homebrew_64k_pages
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/nx_runtime_smoke.py"
                --runtime $<TARGET_FILE:scarletnx> --repo "${PROJECT_SOURCE_DIR}"
                --artifact-dir "${PROJECT_BINARY_DIR}/nx-smoke-64k-failures"
                --runtime-arg=--host_page=65536 "--log-has=memory: 64 KiB host pages")
        set_tests_properties(nx_runtime_homebrew_64k_pages PROPERTIES TIMEOUT 150)
        # a 32-bit program, A32 and Thumb, built by the project's Clang for AArch32 (no ARM linker needed)
        add_test(NAME nx_runtime_arm32
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/nx_arm32_smoke.py"
                --runtime $<TARGET_FILE:scarletnx> --cc "${CMAKE_C_COMPILER}" --repo "${PROJECT_SOURCE_DIR}"
                --artifact-dir "${PROJECT_BINARY_DIR}/nx-arm32-failures")
        set_tests_properties(nx_runtime_arm32 PROPERTIES TIMEOUT 300)
    endif()
    if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND UNIX AND NOT APPLE)
        add_test(NAME xdg_layout
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/xdg_layout.py" --driver $<TARGET_FILE:scarletnx>)
        set_tests_properties(xdg_layout PROPERTIES TIMEOUT 240)
    endif()
endif()
