# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx-shaderc: translates a captured shader corpus directly to SPIR-V.
add_executable(snx-shaderc snx_shaderc.c)
target_link_libraries(snx-shaderc PRIVATE snx_shader)
if(NOT WIN32)
    target_link_libraries(snx-shaderc PRIVATE Threads::Threads m)
endif()
set_target_properties(snx-shaderc PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
# snx-sm86: finds the SM86 programs in a dump of shader memory, reports the opcodes the decoder knows and does not,
# and translates the programs to SPIR-V (src/nx/gpu/shader/sm86.h)
add_executable(snx-sm86 snx_sm86.c)
target_link_libraries(snx-sm86 PRIVATE snx_shader)
if(NOT WIN32)
    target_link_libraries(snx-sm86 PRIVATE Threads::Threads m)
endif()
set_target_properties(snx-sm86 PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
# snx-shaderperf: times SPIR-V shaders on the GPU (timestamps, statistics, pipeline executable properties, performance
# counters) with inputs from the command line, for agents to measure and compare shaders (tools/shaderperf/README.md).
# It stands alone: it opens the Vulkan loader at run time and needs only Vulkan's headers
if(TARGET Vulkan::Headers)
    add_executable(snx-shaderperf shaderperf/main.c shaderperf/gpu.c shaderperf/reflect.c shaderperf/stats.c
                                  shaderperf/texfmt.c)
    set_target_properties(snx-shaderperf PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                                    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    # the {sType} initializers of Vulkan structures leave the other members to 0
    target_compile_options(snx-shaderperf PRIVATE -Wall -Wextra -Wno-missing-field-initializers
                                                  -Werror=missing-prototypes -Werror=missing-variable-declarations)
    target_compile_definitions(snx-shaderperf PRIVATE VK_NO_PROTOTYPES)
    # the drivers (NVIDIA's on FreeBSD) want the program to have threads
    target_link_libraries(snx-shaderperf PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    if(NOT WIN32)
        target_link_libraries(snx-shaderperf PRIVATE Threads::Threads m)
    endif()
endif()
