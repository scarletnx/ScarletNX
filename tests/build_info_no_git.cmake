# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A tree git cannot read, as a source archive's: cmake/commit.cmake says "unknown" for its branch, its commit and that
# commit's day, the values the build line then carries.
#   cmake -DSRC=<source tree> -DOUT=<a header to write> -P tests/build_info_no_git.cmake
set(ENV{GIT_DIR} "${OUT}.no-repository-here")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSRC=${SRC}" "-DOUT=${OUT}" -P "${SRC}/cmake/commit.cmake"
    RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "cmake/commit.cmake failed outside a git checkout: ${failed}")
endif()
file(READ "${OUT}" header)
set(want "#define SNX_BRANCH \"unknown\"\n#define SNX_COMMIT \"unknown\"\n#define SNX_COMMIT_DATE \"unknown\"\n")
if(NOT header STREQUAL want)
    message(FATAL_ERROR "outside a git checkout ${OUT} says\n${header}not\n${want}")
endif()
