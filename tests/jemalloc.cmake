# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# SNX_JEMALLOC (cmake/jemalloc.cmake): the scarletnx program's malloc is the pinned jemalloc's. Run with jemalloc's
# statistics asked for at exit, the program prints them with the pinned version (a FreeBSD libc's own jemalloc is an
# older one). On ELF its dynamic symbols define malloc and free, so the C library's own calls and every shared
# library's bind to the program's: without them a pointer the C library allocates would reach jemalloc's free
cmake_minimum_required(VERSION 3.25)
foreach(name PROGRAM VERSION CONF)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "Set PROGRAM, VERSION, CONF (the variable jemalloc reads its options from) and NM (empty "
                            "where the program is not ELF)")
    endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${CONF}=stats_print:true" "${PROGRAM}" --help
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${PROGRAM} --help exited with ${rc}: ${err}")
endif()
# jemalloc writes its statistics to standard error
if(NOT err MATCHES "Version: \"([0-9.]+)-")
    message(FATAL_ERROR "${PROGRAM} printed no jemalloc statistics with ${CONF}=stats_print:true: its malloc is the C "
                        "library's")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL VERSION)
    message(FATAL_ERROR "${PROGRAM}'s malloc is jemalloc ${CMAKE_MATCH_1}, not the pinned ${VERSION}")
endif()

if(NM)
    execute_process(COMMAND "${NM}" -D --defined-only "${PROGRAM}" OUTPUT_VARIABLE symbols ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${NM} -D --defined-only ${PROGRAM} exited with ${rc}: ${err}")
    endif()
    foreach(name malloc free)
        if(NOT symbols MATCHES "(^|\n)[0-9a-fA-F]+ [TW] ${name}(\n|$)")
            message(FATAL_ERROR "${PROGRAM} does not export ${name}: the C library and the shared libraries keep "
                                "their own")
        endif()
    endforeach()
endif()
message(STATUS "${PROGRAM}: jemalloc ${VERSION} is its malloc")
