# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# SNX_JEMALLOC (cmake/jemalloc.cmake): the scarletnx program's malloc is the pinned jemalloc's. Run with jemalloc's
# statistics asked for at exit, the program prints them with the pinned version (a FreeBSD libc's own jemalloc is an
# older one); jemalloc starts at load, so they print whether or not it is the process's malloc. On ELF the program's
# dynamic symbols define NAMES (malloc and free unless named; Android's program exports its C library's whole allocator
# by name), so the C library's own calls and every shared library's bind to the program's: without them a pointer the
# C library allocates would reach jemalloc's free. A program built for another machine (Android's) is only read:
# VERSION empty
cmake_minimum_required(VERSION 3.25)
if(NOT PROGRAM OR (VERSION AND NOT CONF) OR (NOT VERSION AND NOT NM))
    message(FATAL_ERROR "Set PROGRAM; VERSION and CONF (the variable jemalloc reads its options from) to run it, empty "
                        "for a program built for another machine; NM to read its dynamic symbols (empty where the "
                        "program is not ELF) and NAMES, the comma-separated functions it must export (default "
                        "malloc,free)")
endif()

if(VERSION)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${CONF}=stats_print:true" "${PROGRAM}" --help
                    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${PROGRAM} --help exited with ${rc}: ${err}")
    endif()
    # jemalloc writes its statistics to standard error
    if(NOT err MATCHES "Version: \"([0-9.]+)-")
        message(FATAL_ERROR "${PROGRAM} printed no jemalloc statistics with ${CONF}=stats_print:true: its malloc is "
                            "the C library's")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL VERSION)
        message(FATAL_ERROR "${PROGRAM}'s malloc is jemalloc ${CMAKE_MATCH_1}, not the pinned ${VERSION}")
    endif()
endif()

if(NM)
    if(NOT NAMES)
        set(NAMES malloc,free)
    endif()
    string(REPLACE "," ";" names "${NAMES}")
    execute_process(COMMAND "${NM}" -D --defined-only "${PROGRAM}" OUTPUT_VARIABLE symbols ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${NM} -D --defined-only ${PROGRAM} exited with ${rc}: ${err}")
    endif()
    set(missing "")
    foreach(name IN LISTS names)
        if(NOT symbols MATCHES "(^|\n)[0-9a-fA-F]+ [TW] ${name}(\n|$)")
            list(APPEND missing ${name})
        endif()
    endforeach()
    if(missing)
        list(JOIN missing ", " missing)
        message(FATAL_ERROR "${PROGRAM} does not export ${missing}: the C library and the shared libraries keep their "
                            "own")
    endif()
endif()
# With PRELOAD, a library that wraps pthread_cond_init and allocates inside it: the program still starts and ends.
# jemalloc's background threads initialise a condition variable inside malloc's first call, which then enters itself
# and waits on its own lock (the AppImage's cross-libc-dlopen.so did it, cmake/jemalloc_defs.cmake)
if(PRELOAD)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "LD_PRELOAD=${PRELOAD}" "${PROGRAM}" --help
                    OUTPUT_QUIET ERROR_VARIABLE err RESULT_VARIABLE rc TIMEOUT 30)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${PROGRAM} --help with ${PRELOAD} preloaded, a pthread_cond_init that allocates, ended "
                            "with \"${rc}\" (a timeout of 30 s is malloc's first call waiting on itself): ${err}")
    endif()
endif()
if(VERSION)
    message(STATUS "${PROGRAM}: jemalloc ${VERSION} is its malloc")
else()
    message(STATUS "${PROGRAM} exports ${NAMES}")
endif()
