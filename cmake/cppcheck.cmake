# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# `cmake --build <tree> --target cppcheck` runs cppcheck over every file of src/ and tests/ this tree compiles, each
# with the flags its compile uses (compile_commands.json) and the macros this tree's compiler predefines, so it reads
# the code each platform builds. cmake/cppcheck.cfg tells it what our headers cannot. Every finding of our files is
# shown; cmake/cppcheck-not-zero.txt names the checks that still have findings, which do not fail the target, and a
# finding of any other check does. A check brought to zero leaves that list, so the list only shrinks. Not part of
# the build: run it after one, so the generated headers are there.
include_guard(GLOBAL)

# NO_CACHE: found on every configure, so a cppcheck installed or removed since is seen
find_program(snx_cppcheck NAMES cppcheck NO_CACHE NO_CMAKE_FIND_ROOT_PATH)
set(snx_cppcheck_dir "${CMAKE_BINARY_DIR}/cppcheck")

if(NOT snx_cppcheck)
    add_custom_target(cppcheck
        COMMAND "${CMAKE_COMMAND}" -E echo "cppcheck was not found on PATH when this tree was configured: install it, then configure again"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM)
    return()
endif()
if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    add_custom_target(cppcheck
        COMMAND "${CMAKE_COMMAND}" -E echo "CMAKE_EXPORT_COMPILE_COMMANDS is ${CMAKE_EXPORT_COMPILE_COMMANDS}, and cppcheck reads compile_commands.json: configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (every preset sets it)"
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM)
    return()
endif()

# cppcheck predefines no compiler's macros, and our code tests them (__SIZEOF_INT128__ for STXP, __clang__ for the
# CPU helpers, the host's system): the compiler's own list, read once here.
file(MAKE_DIRECTORY "${snx_cppcheck_dir}/build")
file(WRITE "${snx_cppcheck_dir}/empty.c" "")
execute_process(COMMAND "${CMAKE_C_COMPILER}" -std=gnu${CMAKE_C_STANDARD} -dM -E -x c "${snx_cppcheck_dir}/empty.c"
    OUTPUT_FILE "${snx_cppcheck_dir}/predefined.h"
    RESULT_VARIABLE snx_cppcheck_rc
    ERROR_VARIABLE snx_cppcheck_err)
if(NOT snx_cppcheck_rc EQUAL 0)
    message(FATAL_ERROR "${CMAKE_C_COMPILER} -dM -E, for cppcheck's predefined macros, exited ${snx_cppcheck_rc}: "
                        "${snx_cppcheck_err}")
endif()
# cppcheck 2.20 reads C11's _Noreturn as nothing (SNX_NORETURN), so the code after plat_exit_now seemed to run; it
# reads the GNU attribute
file(APPEND "${snx_cppcheck_dir}/predefined.h" "#define _Noreturn __attribute__((noreturn))\n")

# the third-party headers our files include are theirs to check: the build tree's (FFmpeg's) and CPM's sources, which
# sit in the build tree without a CPM_SOURCE_CACHE
set(snx_cppcheck_theirs "--suppress=*:${CMAKE_BINARY_DIR}/*")
if(CPM_SOURCE_CACHE)
    list(APPEND snx_cppcheck_theirs "--suppress=*:${CPM_SOURCE_CACHE}/*")
endif()

cmake_host_system_information(RESULT snx_cppcheck_jobs QUERY NUMBER_OF_LOGICAL_CORES)
add_custom_target(cppcheck
    COMMAND "${snx_cppcheck}"
        "--project=${CMAKE_BINARY_DIR}/compile_commands.json"
        "--file-filter=${CMAKE_SOURCE_DIR}/src/*"
        "--file-filter=${CMAKE_SOURCE_DIR}/tests/*"
        ${snx_cppcheck_theirs}
        "--include=${snx_cppcheck_dir}/predefined.h"
        --std=c23 --std=c++20
        "--library=${CMAKE_SOURCE_DIR}/cmake/cppcheck.cfg"
        "--exitcode-suppressions=${CMAKE_SOURCE_DIR}/cmake/cppcheck-not-zero.txt"
        --enable=warning,portability,performance
        "--cppcheck-build-dir=${snx_cppcheck_dir}/build"
        -j ${snx_cppcheck_jobs}
        --error-exitcode=1
        --quiet
        --template=gcc
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM)
