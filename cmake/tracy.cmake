# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Tracy, the profiler, for every console's runtime (src/common/utilities/tracy.c and tracy.h): snx_tracy gives a target
# the SNX_TRACY=1 its zones need and the client, in a build with SNX_TRACY; snx_tracy_client starts and stops the
# client and F10's captures, which a build without it compiles to the one variable tracy.h reads
include_guard(GLOBAL)
add_library(snx_tracy INTERFACE)
if(SNX_TRACY)
    target_compile_definitions(snx_tracy INTERFACE SNX_TRACY=1)
    target_link_libraries(snx_tracy INTERFACE Tracy::TracyClient)
endif()
add_library(snx_tracy_client STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/tracy.c")
target_link_libraries(snx_tracy_client PUBLIC snx_tracy PRIVATE scarletnx-common snx_settings snx_build_info)
target_compile_options(snx_tracy_client PRIVATE -Wall -Wextra -Werror=missing-prototypes)
# the tracy-capture a capture needs, the client's release (cmake/dependencies.cmake)
target_compile_definitions(snx_tracy_client PRIVATE SNX_TRACY_VERSION="${SNX_TRACY_VERSION}")
if(SNX_TRACY)
    # the C++ client's connection id, which the C layer's GPU track keys its epochs on; tracy-capture's process
    target_sources(snx_tracy_client PRIVATE "${PROJECT_SOURCE_DIR}/src/common/utilities/tracy_conn.cpp")
    target_link_libraries(snx_tracy_client PRIVATE SDL3::SDL3)
endif()
