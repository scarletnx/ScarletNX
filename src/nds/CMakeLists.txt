# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The NDS (the DS) runtime: the board scarletnx-nds runs card images on, its offline ARM9 compiler and its Vulkan
# renderer. Independent of the NX and GNU ABI runtimes; a target here compiles only src/nds.
#
#   scarletnx-nds-runtime   the board: the card image, memory, the registers, the HLE BIOS and ARM7, the block cache
#   snx_nds_program         the runner and the offline compiler, which scarletnx runs as scarletnx-nds (src/multicall)

# The ARM and Thumb recompiler of src/common/cpu/arm, the same one the other system types use
if(NOT TARGET snx_arm_llvm)
    include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" nds-arm-backend)
endif()

add_library(scarletnx-nds STATIC cartridge/rom.c board/board.c board/memory.c board/io.c board/lcd.c board/dma.c
                                 board/timers.c board/card.c bios/bios.c hle/pxi.c hle/sound.c audio/spu.c cpu/cpu.c cpu/recomp.c
                                 gpu/gpu3d.c gpu/present.c)
set_target_properties(scarletnx-nds PROPERTIES OUTPUT_NAME scarletnx-nds-runtime C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(scarletnx-nds PUBLIC scarletnx-common snx_arm_llvm snx_llvm)
target_compile_options(scarletnx-nds PRIVATE -Wall -Wextra -Werror=missing-prototypes)

# the Vulkan device: the 2D engines' frame, the 3D engine's picture and the window. volk.c is compiled into each
# frontend, as the 3DS's
add_library(snx_nds_gpu STATIC gpu/vk/device.c gpu/vk/frame.c gpu/vk/render3d.c gpu/vk/present.c
            "${volk_SOURCE_DIR}/volk.c")
target_include_directories(snx_nds_gpu PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(snx_nds_gpu PUBLIC scarletnx-common Vulkan::Headers SDL3::SDL3 snx_vk_common ${CMAKE_DL_LIBS})
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
set_target_properties(snx_nds_gpu PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_nds_gpu PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_compile_definitions(snx_nds_gpu PRIVATE VK_NO_PROTOTYPES)
# the GPU's shaders, compiled at build time (cmake/glsl.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
snx_glsl_spirv(snx_nds_gpu SOURCE gpu/vk/shaders/compose.comp NAME snx_nds_compose_spv
               HEADER nds/gpu/vk/shaders/compose_spv.h)
foreach(stage vert frag)
    snx_glsl_spirv(snx_nds_gpu SOURCE gpu/vk/shaders/poly.${stage} NAME snx_nds_poly_${stage}_spv
                   HEADER nds/gpu/vk/shaders/poly_spv.h)
endforeach()
snx_glsl_spirv(snx_nds_gpu SOURCE gpu/vk/shaders/screen.vert NAME snx_nds_screen_vert_spv
               HEADER nds/gpu/vk/shaders/post_spv.h)
snx_glsl_spirv(snx_nds_gpu SOURCE gpu/vk/shaders/post.frag NAME snx_nds_post_frag_spv
               HEADER nds/gpu/vk/shaders/post_spv.h)
snx_glsl_spirv(snx_nds_gpu SOURCE gpu/vk/shaders/stencil.frag NAME snx_nds_stencil_frag_spv
               HEADER nds/gpu/vk/shaders/post_spv.h)
snx_glsl_spirv(snx_nds_gpu SOURCE gpu/vk/shaders/clear.frag NAME snx_nds_clear_frag_spv
               HEADER nds/gpu/vk/shaders/post_spv.h)
snx_glsl_spirv(snx_nds_gpu SOURCE gpu/vk/shaders/aa.frag NAME snx_nds_aa_frag_spv
               HEADER nds/gpu/vk/shaders/post_spv.h)
target_link_libraries(scarletnx-nds PUBLIC snx_nds_gpu)

add_library(snx_nds_program STATIC runtime/main.c runtime/host.c aot/compiler.c aot/discover.c)
target_link_libraries(snx_nds_program PUBLIC scarletnx-nds snx_nds_gpu snx_build_info snx_window_icon snx_pad_sdl)
set_target_properties(snx_nds_program PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_nds_program PRIVATE -Wall -Wextra -Werror=missing-prototypes)
include("${PROJECT_SOURCE_DIR}/tests/cmake/nds.cmake") # its tests
