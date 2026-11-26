# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A nightly's build: cmake/commit.cmake takes SNX_BUILD_NAME, nightly-YYYY-MM-DD, for the branch the build line names,
# keeps the commit and its day, and refuses any other value, naming it and the branch kept.
#   cmake -DSRC=<source tree> -DOUT=<a header to write> -P tests/build_info_nightly.cmake
execute_process(COMMAND "${CMAKE_COMMAND}" -E env SNX_BUILD_NAME=nightly-2026-11-26
    "${CMAKE_COMMAND}" "-DSRC=${SRC}" "-DOUT=${OUT}" -P "${SRC}/cmake/commit.cmake" RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "cmake/commit.cmake failed for a nightly: ${failed}")
endif()
file(READ "${OUT}" header)
if(NOT header MATCHES "^#define SNX_BRANCH \"nightly-2026-11-26\"\n#define SNX_COMMIT \"[^\"]+\"\n#define SNX_COMMIT_DATE")
    message(FATAL_ERROR "a nightly's build names no nightly-2026-11-26 for its branch:\n${header}")
endif()
foreach(bad "2026-11-26" "nightly-26-11-2026" "nightly-2026-11-26-x")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "SNX_BUILD_NAME=${bad}"
        "${CMAKE_COMMAND}" "-DSRC=${SRC}" "-DOUT=${OUT}.bad" -P "${SRC}/cmake/commit.cmake"
        RESULT_VARIABLE failed ERROR_VARIABLE said)
    string(REGEX REPLACE "[ \n]+" " " said "${said}") # CMake wraps a message's lines
    if(NOT failed OR NOT said MATCHES "SNX_BUILD_NAME=${bad} is no nightly-YYYY-MM-DD: the build line keeps ")
        message(FATAL_ERROR "SNX_BUILD_NAME=${bad} was not refused by name: ${failed} ${said}")
    endif()
endforeach()
