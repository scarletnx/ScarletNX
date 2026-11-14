# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_build_info: the line the window titles carry, "ScarletNX <branch> <commit> (version <n>, <compiler>, <OS>
# <arch>)", and snx_build_log_line the logs', with ", committed <YYYY-MM-DD>" after the commit. The branch, the commit
# and its day are read at every build (cmake/commit.cmake), the version when a number it adds up changes
# (cmake/version.cmake), the rest is the compiler's own macros
include_guard(GLOBAL)
set(snx_commit_header "${PROJECT_BINARY_DIR}/generated/snx_commit.h")
add_custom_target(snx_commit
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" "-DOUT=${snx_commit_header}"
            -P "${PROJECT_SOURCE_DIR}/cmake/commit.cmake"
    BYPRODUCTS "${snx_commit_header}" VERBATIM)
include("${PROJECT_SOURCE_DIR}/cmake/version_parts.cmake")
set(snx_version_header "${PROJECT_BINARY_DIR}/generated/snx_version.h")
set(snx_version_inputs "")
foreach(part IN LISTS SNX_VERSION_PARTS)
    string(REGEX REPLACE ":[^:]*$" "" file "${part}")
    list(APPEND snx_version_inputs "${PROJECT_SOURCE_DIR}/${file}")
endforeach()
list(REMOVE_DUPLICATES snx_version_inputs)
add_custom_command(OUTPUT "${snx_version_header}"
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" "-DOUT=${snx_version_header}"
            -P "${PROJECT_SOURCE_DIR}/cmake/version.cmake"
    DEPENDS ${snx_version_inputs} "${PROJECT_SOURCE_DIR}/cmake/version.cmake"
            "${PROJECT_SOURCE_DIR}/cmake/version_parts.cmake"
    VERBATIM)
add_library(snx_build_info STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/build_info.c" "${snx_version_header}")
add_dependencies(snx_build_info snx_commit)
target_include_directories(snx_build_info PRIVATE "${PROJECT_BINARY_DIR}/generated")
set_target_properties(snx_build_info PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_build_info PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
include("${PROJECT_SOURCE_DIR}/tests/cmake/build_info.cmake") # its tests
