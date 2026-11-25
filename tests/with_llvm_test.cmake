# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# cmake/with_llvm.cmake: ON by default, whatever is built; OFF it takes a build with no driver, and stops one with any,
# naming the ones that are on with their values.
# usage: cmake -DSCRIPT=<with_llvm.cmake> -P with_llvm_test.cmake
set(drivers NX GNUABI 3DS VITA AGB NDS ANDROID)
# given: the -D arguments. want: a substring of the error, or "" for a configure that goes on
function(check given want)
    execute_process(COMMAND "${CMAKE_COMMAND}" ${given} -P "${SCRIPT}" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                    ERROR_VARIABLE err)
    if(want STREQUAL "")
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${given}: stopped (${rc}):\n${err}")
        endif()
        return()
    endif()
    if(rc EQUAL 0)
        message(FATAL_ERROR "${given}: went on, and should have stopped")
    endif()
    string(REGEX REPLACE "[ \n]+" " " err "${err}") # message() wraps its text
    string(FIND "${err}" "${want}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${given}: the error lacks \"${want}\":\n${err}")
    endif()
endfunction()
set(all_on "")
set(all_off "")
set(names "")
foreach(driver IN LISTS drivers)
    list(APPEND all_on -DSNX_BUILD_${driver}=ON)
    list(APPEND all_off -DSNX_BUILD_${driver}=OFF)
    list(APPEND names "SNX_BUILD_${driver}=ON")
endforeach()
list(JOIN names ", " names)
check("${all_on}" "")
check("-DSNX_WITH_LLVM=ON;${all_on}" "")
check("-DSNX_WITH_LLVM=OFF;${all_off}" "")
check("-DSNX_WITH_LLVM=OFF;${all_on}"
      "SNX_WITH_LLVM=OFF builds the recompiler's core alone, and these need LLVM: ${names}. Turn them off")
foreach(driver IN LISTS drivers)
    check("-DSNX_WITH_LLVM=OFF;${all_off};-DSNX_BUILD_${driver}=ON"
          "these need LLVM: SNX_BUILD_${driver}=ON. Turn them off")
endforeach()
check("-DSNX_WITH_LLVM=0;${all_off};-DSNX_BUILD_VITA=yes"
      "SNX_WITH_LLVM=0 builds the recompiler's core alone, and these need LLVM: SNX_BUILD_VITA=yes.")
