# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -P tests/cross_flags_test.cmake
#
# Where cmake/cross.cmake puts -mabi=elfv2: big-endian and little-endian PowerPC64's compiles and links with
# SNX_PPC64_ELFV2 on (its default), and no other processor's, as Clang names a RISC-V or LoongArch program's loader by
# its -mabi too (ld-linux-riscv64-elfv2.so.1, which no system has). Each processor's run is a cmake of its own, since
# cross.cmake includes once. And where cross.cmake refuses big-endian PowerPC64's ELFv1: without LLVM alone, with both
# options' values, as the ppc64 back end writes ELFv2's calls.
if(NOT DEFINED SRC)
    message(FATAL_ERROR "cross_flags_test.cmake needs -DSRC=...")
endif()
if(DEFINED PROCESSOR)
    set(CMAKE_SYSTEM_PROCESSOR ${PROCESSOR})
    include("${SRC}/cmake/cross.cmake")
    message("compile [${CMAKE_C_FLAGS}][${CMAKE_CXX_FLAGS}] link [${CMAKE_EXE_LINKER_FLAGS}]"
            "[${CMAKE_SHARED_LINKER_FLAGS}][${CMAKE_MODULE_LINKER_FLAGS}]")
    return()
endif()

set(failed 0)
# processor:SNX_PPC64_ELFV2 (- for its default):whether every compile and link names -mabi=elfv2
foreach(case IN ITEMS ppc64:-:1 ppc64le:-:1 ppc64:OFF:0 powerpc64:-:1 riscv64:-:0 loongarch64:-:0 x86_64:-:0
                      aarch64:-:0 mips64:-:0 s390x:-:0)
    string(REPLACE ":" ";" parts "${case}")
    list(GET parts 0 processor)
    list(GET parts 1 elfv2)
    list(GET parts 2 want)
    set(options "-DSRC=${SRC}" "-DPROCESSOR=${processor}")
    if(NOT elfv2 STREQUAL "-")
        list(APPEND options "-DSNX_PPC64_ELFV2=${elfv2}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${options} -P "${CMAKE_CURRENT_LIST_FILE}" OUTPUT_VARIABLE out
                    ERROR_VARIABLE out RESULT_VARIABLE rc)
    string(STRIP "${out}" out)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "^compile \\[([^]]*)\\]\\[([^]]*)\\] link \\[([^]]*)\\]\\[([^]]*)\\]\\[([^]]*)\\]$")
        message(SEND_ERROR "${processor} (SNX_PPC64_ELFV2 ${elfv2}): cross.cmake exited ${rc}: ${out}")
        set(failed 1)
        continue()
    endif()
    set(all "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}" "${CMAKE_MATCH_4}" "${CMAKE_MATCH_5}")
    foreach(flags IN LISTS all)
        string(FIND " ${flags} " " -mabi=elfv2 " at)
        if(want AND at EQUAL -1)
            message(SEND_ERROR "${processor} (SNX_PPC64_ELFV2 ${elfv2}): flags [${flags}] (${out}) lack -mabi=elfv2")
            set(failed 1)
        elseif(NOT want AND NOT at EQUAL -1)
            message(SEND_ERROR "${processor} (SNX_PPC64_ELFV2 ${elfv2}): flags [${flags}] (${out}) name -mabi=elfv2, "
                               "which only PowerPC64 with SNX_PPC64_ELFV2 on may")
            set(failed 1)
        endif()
    endforeach()
    message(STATUS "${processor} (SNX_PPC64_ELFV2 ${elfv2}): ${out}")
endforeach()
# processor:SNX_PPC64_ELFV2:SNX_WITH_LLVM (- for unset):whether cross.cmake refuses it
foreach(case IN ITEMS ppc64:OFF:OFF:1 powerpc64:OFF:OFF:1 ppc64:OFF:0:1 ppc64:OFF:ON:0 ppc64:OFF:-:0 ppc64:ON:OFF:0
                      ppc64:-:OFF:0 ppc64le:OFF:OFF:0 x86_64:OFF:OFF:0)
    string(REPLACE ":" ";" parts "${case}")
    list(GET parts 0 processor)
    list(GET parts 1 elfv2)
    list(GET parts 2 llvm)
    list(GET parts 3 refused)
    set(options "-DSRC=${SRC}" "-DPROCESSOR=${processor}")
    if(NOT elfv2 STREQUAL "-")
        list(APPEND options "-DSNX_PPC64_ELFV2=${elfv2}")
    endif()
    if(NOT llvm STREQUAL "-")
        list(APPEND options "-DSNX_WITH_LLVM=${llvm}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${options} -P "${CMAKE_CURRENT_LIST_FILE}" OUTPUT_VARIABLE out
                    ERROR_VARIABLE out RESULT_VARIABLE rc)
    string(REGEX REPLACE "[ \n]+" " " said "${out}") # CMake wraps a message's lines
    set(of "${processor} (SNX_PPC64_ELFV2 ${elfv2}, SNX_WITH_LLVM ${llvm})")
    if(NOT refused AND NOT rc EQUAL 0)
        message(SEND_ERROR "${of}: cross.cmake exited ${rc}: ${out}")
        set(failed 1)
    elseif(refused AND (rc EQUAL 0 OR NOT said MATCHES "SNX_PPC64_ELFV2=${elfv2} and SNX_WITH_LLVM=${llvm}: nothing" OR
                        NOT said MATCHES "needs ELFv2 and this build has ELFv1"))
        message(SEND_ERROR "${of}: cross.cmake exited ${rc}, and has to refuse it with both values and the ABIs: ${out}")
        set(failed 1)
    endif()
    message(STATUS "${of}: exit ${rc}")
endforeach()
if(failed)
    message(FATAL_ERROR "cross_flags: see the errors above")
endif()
