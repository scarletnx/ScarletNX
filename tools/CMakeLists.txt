# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx-shaderc: translates a captured shader corpus directly to SPIR-V.
add_executable(snx-shaderc snx_shaderc.c)
target_link_libraries(snx-shaderc PRIVATE snx_shader)
if(NOT WIN32)
    target_link_libraries(snx-shaderc PRIVATE Threads::Threads m)
endif()
set_target_properties(snx-shaderc PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
# snx-sm70: finds the SM70+ programs in a dump of shader memory, reports the opcodes the decoder knows and does not,
# and translates the programs to SPIR-V (src/nx/gpu/shader/sm70.h)
add_executable(snx-sm70 snx_sm70.c)
target_link_libraries(snx-sm70 PRIVATE snx_shader)
if(NOT WIN32)
    target_link_libraries(snx-sm70 PRIVATE Threads::Threads m)
endif()
set_target_properties(snx-sm70 PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
