# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The PS Vita driver: the VPK's extraction and the eboot's loader, the offline compiler (the A57-profile AArch32 block
# compiler, src/common/cpu/arm, whose ARMv8 AArch32 holds the Vita's ARMv7-A user mode), and the runtime: the kernel and
# system libraries answered by NID, sceGxm on the host's Vulkan device
add_library(snx_vita_loader STATIC loader/inflate.c loader/vpk.c loader/image.c)
target_link_libraries(snx_vita_loader PUBLIC scarletnx-common OpenSSL::Crypto PRIVATE snx_miniz)
target_compile_definitions(snx_vita_loader PRIVATE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)

# the USSE translator, which the runtime and snx-vita-usse (the shader corpus' tool) share
add_library(snx_vita_usse STATIC gpu/usse.c)
target_link_libraries(snx_vita_usse PUBLIC scarletnx-common)
add_executable(snx-vita-usse "${PROJECT_SOURCE_DIR}/tools/snx_vita_usse.c")
target_link_libraries(snx-vita-usse PRIVATE snx_vita_usse)
set_target_properties(snx-vita-usse PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

add_library(snx_vita_program STATIC
    aot/compiler.c aot/load.c
    runtime/process.c runtime/run.c runtime/jit.c runtime/lockstep.c runtime/watch.c
    hle/hle.c hle/kernel.c hle/io.c hle/ctrl.c hle/display.c hle/audio.c hle/misc.c hle/gxm.c hle/heap.c hle/libc.c hle/fiber.c hle/ngs.c
    hle/mp4.c hle/videodec.c hle/audiodec.c hle/dialog.c hle/net.c hle/trophy.c
    gpu/present.c gpu/render.c gpu/vk.c "${volk_SOURCE_DIR}/volk.c"
    platform/input.c)
# FFmpeg: the movies' demuxer and decoders (hle/mp4.c, videodec.c, audiodec.c); scarletnx-filesystem: the game
# module's path as Windows takes it (snx_host_wpath, aot/load.c)
target_link_libraries(snx_vita_program PUBLIC snx_vita_loader snx_vita_usse scarletnx-filesystem FFmpeg::avformat FFmpeg::avcodec
                      snx_settings_runtime snx_build_info snx_window_icon snx_vk_common SDL3::SDL3
                      Vulkan::Headers ${CMAKE_DL_LIBS})
target_include_directories(snx_vita_program PUBLIC "${volk_SOURCE_DIR}")
# the compute shaders (the texture deswizzle, the decoded pictures' layout, YUV textures, 24-bit color surfaces),
# compiled at build time
# (cmake/glsl.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
foreach(shader swizzle picture yuv rgb24)
    snx_glsl_spirv(snx_vita_program SOURCE gpu/${shader}.comp NAME vita_${shader}_spv HEADER vita/gpu/${shader}_spv.h)
endforeach()
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
if(NOT TARGET snx_arm_llvm)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" vita-arm-backend)
endif()
# The LLVM backend (snx_llvm, which the Switch driver's tree, the GNU ABI runtime's or the top level configures): the
# offline compiler's objects and their helpers (armlib.c) linked by its linker, and the blocks the module lacks
# compiled at run time by its executor (exec.c)
target_link_libraries(snx_vita_program PUBLIC snx_arm_llvm snx_llvm)
target_compile_definitions(snx_vita_program PRIVATE _CRT_SECURE_NO_WARNINGS)
# The backend checked against Unicorn as the game runs (runtime/lockstep.h), for debugging the AArch32 backend: Unicorn's
# headers and library (its Python package's libunicorn.so.2 will do) where the build is told
option(SNX_VITA_LOCKSTEP "Check the Vita's AArch32 code against Unicorn while it runs (VITA_LOCKSTEP=import call)" OFF)
if(SNX_VITA_LOCKSTEP)
    find_path(SNX_UNICORN_INCLUDE unicorn/unicorn.h REQUIRED)
    find_library(SNX_UNICORN_LIBRARY NAMES unicorn libunicorn.so.2 REQUIRED)
    target_include_directories(snx_vita_program PRIVATE "${SNX_UNICORN_INCLUDE}")
    target_link_libraries(snx_vita_program PUBLIC "${SNX_UNICORN_LIBRARY}")
    target_compile_definitions(snx_vita_program PRIVATE SNX_VITA_LOCKSTEP)
endif()
foreach(target snx_vita_loader snx_vita_usse snx-vita-usse snx_vita_program)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endforeach()

# the driver's command line (runtime/main.c's snx_vita_main), which scarletnx runs as scarletnx-vita (src/multicall)
target_sources(snx_vita_program PRIVATE runtime/main.c)
if(SNX_LLVM_SHARED)
    target_link_options(snx_vita_program INTERFACE "-Wl,-rpath,${SNX_LLVM_LIBRARY_DIR}")
endif()
