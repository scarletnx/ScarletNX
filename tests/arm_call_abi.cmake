# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_arm_call_abi(<machine> <target triple> [<compiler option>...]): arm_<machine>_call_abi, the blocks
# against the runtime library's functions compiled for the machine (arm_call_abi_callee.c, --callee): an argument a
# block passes as its ABI does not have it shows there alone. Clang alone compiles for another machine, and one built
# without this one's back end makes no test
function(snx_arm_call_abi machine triple)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        return()
    endif()
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/arm_call_abi_callee.c")
    set(object "${CMAKE_CURRENT_BINARY_DIR}/arm-call-abi-${machine}.o")
    set(compile "${CMAKE_C_COMPILER}" --target=${triple} ${ARGN} -std=gnu23 -O2 -ffreestanding -nostdlibinc
        -fno-asynchronous-unwind-tables -fno-stack-protector -isystem "${CMAKE_CURRENT_SOURCE_DIR}/support/freestanding"
        -I "${PROJECT_SOURCE_DIR}/src" -I "${CMAKE_CURRENT_SOURCE_DIR}")
    # -c, not -fsyntax-only: a Clang without the back end checks the syntax for it all the same
    execute_process(COMMAND ${compile} -c "${source}" -o "${object}"
                    RESULT_VARIABLE failed OUTPUT_QUIET ERROR_VARIABLE why)
    if(failed)
        string(REGEX MATCH "[^\n]*error:[^\n]*" why "${why}")
        message(STATUS "arm_${machine}_call_abi: ${CMAKE_C_COMPILER} makes no code for ${triple}: ${why}")
        return()
    endif()
    file(GLOB headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/*.h"
         "${PROJECT_SOURCE_DIR}/src/common/utilities/*.h")
    add_custom_command(OUTPUT "${object}" COMMAND ${compile} -c "${source}" -o "${object}"
                       DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/rt_helpers.c" ${headers}
                               "${CMAKE_CURRENT_SOURCE_DIR}/support/arm_rt_callbacks.h"
                               "${CMAKE_CURRENT_SOURCE_DIR}/support/freestanding/string.h" VERBATIM)
    add_custom_target(snx-test-arm-call-abi-${machine} ALL DEPENDS "${object}")
    add_test(NAME arm_${machine}_call_abi
             COMMAND snx-test-arm-${machine} --callee "${object}" --blocks 600 --states 8)
    set_tests_properties(arm_${machine}_call_abi PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    # the ARM7TDMI's and the ARM946E's bus: the hook and the functions that call it are compiled code too, which
    # clobbers what the ABI lets it
    foreach(profile arm946e arm7tdmi)
        add_test(NAME arm_${machine}_call_abi_${profile}
                 COMMAND snx-test-arm-${machine} --profile ${profile} --callee "${object}" --blocks 600 --states 8)
        set_tests_properties(arm_${machine}_call_abi_${profile} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    endforeach()
endfunction()
