# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the build line (cmake/build_info.cmake), included by cmake/build_info.cmake where they were: they run in
# its scope, so its variables hold here, and a relative path is the top level's
if(BUILD_TESTING)
    add_executable(snx-test-build-info "${PROJECT_SOURCE_DIR}/tests/build_info.c")
    target_link_libraries(snx-test-build-info PRIVATE snx_build_info)
    target_include_directories(snx-test-build-info PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_compile_options(snx-test-build-info PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME build_info COMMAND snx-test-build-info)
    add_test(NAME build_info_commit COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
             "-DOUT=${snx_commit_header}" -DCHECK=1 -P "${PROJECT_SOURCE_DIR}/cmake/commit.cmake")
    add_test(NAME build_info_no_git COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
             "-DOUT=${PROJECT_BINARY_DIR}/build_info_no_git.h" -P "${PROJECT_SOURCE_DIR}/tests/build_info_no_git.cmake")
    # the version: the header the build made is what the tree's numbers make, each number raises it by one, and each
    # number named like them in src/ is listed as a part of it or left out
    add_test(NAME version_current COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
             "-DOUT=${snx_version_header}" -DCHECK=1 -P "${PROJECT_SOURCE_DIR}/cmake/version.cmake")
    add_test(NAME version_bump COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
             "-DWORK=${PROJECT_BINARY_DIR}/version_bump" -P "${PROJECT_SOURCE_DIR}/tests/version_bump.cmake")
    add_test(NAME version_census COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
             -P "${PROJECT_SOURCE_DIR}/tests/version_census.cmake")
endif()
