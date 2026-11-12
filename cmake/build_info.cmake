# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_build_info: the line the window titles carry, "ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)", and
# snx_build_log_line the logs', with ", committed <YYYY-MM-DD>" after the commit. The branch, the commit and its day are
# read at every build (cmake/commit.cmake), the rest is the compiler's own macros
include_guard(GLOBAL)
set(snx_commit_header "${PROJECT_BINARY_DIR}/generated/snx_commit.h")
add_custom_target(snx_commit
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" "-DOUT=${snx_commit_header}"
            -P "${PROJECT_SOURCE_DIR}/cmake/commit.cmake"
    BYPRODUCTS "${snx_commit_header}" VERBATIM)
add_library(snx_build_info STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/build_info.c")
add_dependencies(snx_build_info snx_commit)
target_include_directories(snx_build_info PRIVATE "${PROJECT_BINARY_DIR}/generated")
set_target_properties(snx_build_info PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_build_info PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
include("${PROJECT_SOURCE_DIR}/tests/cmake/build_info.cmake") # its tests
