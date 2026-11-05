# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> -P tests/host_cc_test.cmake
#
# Which compiler cmake/host_cc.cmake takes for the programs a cross build runs, with another clang first on PATH (the
# NDK's, on a Mac): Xcode's /usr/bin/clang on a Mac host, and elsewhere the first on PATH. Each host's run is a cmake of
# its own, since host_cc.cmake includes once.
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "host_cc_test.cmake needs -DSRC=... -DWORK=...")
endif()
if(DEFINED MAC)
    set(CMAKE_HOST_APPLE ${MAC})
    set(ENV{PATH} "${WORK}/bin:$ENV{PATH}")
    include("${SRC}/cmake/host_cc.cmake")
    message("${SNX_HOST_CC}")
    return()
endif()

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/bin/clang" "#!/bin/sh\nexit 1\n")
file(CHMOD "${WORK}/bin/clang" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(failed 0)
foreach(case IN ITEMS 1:/usr/bin/clang 0:${WORK}/bin/clang)
    string(REGEX REPLACE ":.*" "" mac "${case}")
    string(REGEX REPLACE "^[^:]*:" "" want "${case}")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSRC=${SRC}" "-DWORK=${WORK}" "-DMAC=${mac}"
                            -P "${CMAKE_CURRENT_LIST_FILE}" OUTPUT_VARIABLE out ERROR_VARIABLE out RESULT_VARIABLE rc)
    string(STRIP "${out}" out)
    if(NOT rc EQUAL 0 OR NOT out STREQUAL want)
        message(SEND_ERROR "CMAKE_HOST_APPLE=${mac} with ${WORK}/bin/clang first on PATH: host_cc.cmake exited ${rc} "
                           "and took '${out}', not ${want}")
        set(failed 1)
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "host_cc.cmake took the wrong compiler")
endif()
message("host_cc.cmake takes /usr/bin/clang on a Mac host and the first clang on PATH elsewhere")
