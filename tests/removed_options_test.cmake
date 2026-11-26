# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# cmake/removed_options.cmake: an option the build had, given as an older tree's cache holds it, is said with the value
# given and what holds now; with none given, nothing is said.
# usage: cmake -DSCRIPT=<removed_options.cmake> -P removed_options_test.cmake
# given: the -D arguments. want: a substring of what it says, or "" for nothing said
function(check given want)
    execute_process(COMMAND "${CMAKE_COMMAND}" ${given} -P "${SCRIPT}" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                    ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${given}: stopped (${rc}):\n${err}")
    endif()
    string(REGEX REPLACE "[ \n]+" " " out "${out}") # message() wraps its text
    if(want STREQUAL "")
        if(NOT out STREQUAL "")
            message(FATAL_ERROR "${given}: said \"${out}\", and should have said nothing")
        endif()
        return()
    endif()
    string(FIND "${out}" "${want}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${given}: \"${out}\" lacks \"${want}\"")
    endif()
endfunction()
check("" "")
check("-DSNX_A64_DISASM=OFF" "SNX_A64_DISASM=OFF is no longer an option: scarletnx --mcp lists A64 code through the A64 "
                             "decoder in every build")
check("-DSNX_A64_DISASM:BOOL=ON" "SNX_A64_DISASM=ON is no longer an option")
check("-DSNX_WITH_LLVM=ON" "SNX_WITH_LLVM=ON is no longer an option: the native back ends make all of the code, and the "
                           "build has no LLVM")
check("-DSNX_LLVM_CONFIG=/usr/bin/llvm-config" "SNX_LLVM_CONFIG=/usr/bin/llvm-config is no longer an option")
check("-DSNX_TEST_LLVM_SWEEP=ON" "SNX_TEST_LLVM_SWEEP=ON is now SNX_TEST_QEMU_SWEEP: SNX_TEST_QEMU_SWEEP=ON holds")
check("-DSNX_TEST_LLVM_SWEEP=ON;-DSNX_TEST_QEMU_SWEEP=OFF"
      "SNX_TEST_LLVM_SWEEP=ON is now SNX_TEST_QEMU_SWEEP, which is given too: SNX_TEST_QEMU_SWEEP=OFF holds")
