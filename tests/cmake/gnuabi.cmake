# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the GNU ABI runtime (src/gnuabi), included by src/gnuabi/CMakeLists.txt where they were: they run in its
# scope, so its variables hold here, and a relative path is from src/gnuabi
set(SNX_GNUABI_PROGRAM "${PROJECT_BINARY_DIR}/scarletnx-gnuabi${CMAKE_EXECUTABLE_SUFFIX}") # what the tests run
if(BUILD_TESTING)
    add_executable(snx-test-gnuabi-runtime "${PROJECT_SOURCE_DIR}/tests/gnuabi_runtime.c")
    target_link_libraries(snx-test-gnuabi-runtime PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-runtime PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-runtime PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_memory_elf_syscalls COMMAND snx-test-gnuabi-runtime)
    # again as on a host with 64 KiB pages (PowerPC64), each holding sixteen guest pages
    add_test(NAME gnuabi_memory_64k_pages COMMAND snx-test-gnuabi-runtime --host-page=65536)
    add_executable(snx-test-gnuabi-vfs "${PROJECT_SOURCE_DIR}/tests/gnuabi_vfs.c")
    target_link_libraries(snx-test-gnuabi-vfs PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-vfs PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-vfs PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_vfs_isolation COMMAND snx-test-gnuabi-vfs)
    if (NOT WIN32)
        add_executable(snx-test-gnuabi-hostfs "${PROJECT_SOURCE_DIR}/tests/gnuabi_hostfs.c")
        target_link_libraries(snx-test-gnuabi-hostfs PRIVATE scarletnx-gnuabi)
        target_compile_options(snx-test-gnuabi-hostfs PRIVATE -Wall -Wextra -Werror)
        set_target_properties(snx-test-gnuabi-hostfs PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
        add_test(NAME gnuabi_hostfs COMMAND snx-test-gnuabi-hostfs)
    endif()
    add_executable(snx-test-gnuabi-network "${PROJECT_SOURCE_DIR}/tests/gnuabi_network.c")
    target_link_libraries(snx-test-gnuabi-network PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-network PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-network PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_network COMMAND snx-test-gnuabi-network)
    set_tests_properties(gnuabi_network PROPERTIES TIMEOUT 30)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gnuabi_elf32 COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gnuabi_elf32.py" "${SNX_GNUABI_PROGRAM}")
        if (NOT WIN32)
            add_test(NAME gnuabi_process COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gnuabi_process.py"
                "${SNX_GNUABI_PROGRAM}")
            set_tests_properties(gnuabi_process PROPERTIES TIMEOUT 300)
            # threads, futexes and signal handlers, from the freestanding fixtures in tests/gnuabi
            add_test(NAME gnuabi_threads COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gnuabi_threads.py" "${SNX_GNUABI_PROGRAM}")
            set_tests_properties(gnuabi_threads PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()

if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: this folder's tests
    snx_test_sanitize(snx-test-gnuabi-runtime snx-test-gnuabi-vfs snx-test-gnuabi-hostfs snx-test-gnuabi-network)
    snx_sanitizer_path()
endif()
