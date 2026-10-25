# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The runtime: one object library per component, linked into scarletnx. Each component sees src/nx/ as its include
# root (#include "hle/kernel/kernel.h"), and the flags and third-party headers every component shares (snx_nx_flags).
#
#   core      startup, options, the game module, crash reports      kernel    threads, sync, memory, IPC
#   hle       the services the game talks to                         video     NVDEC and VIC (host1x)
#   gpu       the GPU front end: GPFIFO, engines, the renderer interface
#   shader    Maxwell shaders to SPIR-V
#   vk        the renderer                                            platform  the OS layer and a window backend

add_library(snx_tracy INTERFACE)
if(SNX_TRACY)
    target_compile_definitions(snx_tracy INTERFACE SNX_TRACY=1)
    target_link_libraries(snx_tracy INTERFACE Tracy::TracyClient)
endif()

add_library(snx_nx_flags INTERFACE)
target_link_libraries(snx_nx_flags INTERFACE scarletnx-common scarletnx-net scarletnx-ldn scarletnx-filesystem
                                           snx_settings snx_tracy)
# Every function and global used across files is declared once, in the header of the file that defines it, and
# everything else is static: a declaration that disagrees with the definition is then a compile error instead of a
# miscompile. Third-party sources keep their own warnings off.
target_compile_options(snx_nx_flags INTERFACE
                       $<$<COMPILE_LANGUAGE:C>:-Werror=missing-prototypes -Werror=missing-variable-declarations>)
target_compile_definitions(snx_nx_flags INTERFACE VK_NO_PROTOTYPES)
target_link_libraries(snx_nx_flags INTERFACE Vulkan::Headers)
target_include_directories(snx_nx_flags BEFORE INTERFACE "${volk_SOURCE_DIR}"
                                                "${VulkanMemoryAllocator_SOURCE_DIR}/include" "${renderdoc_app_SOURCE_DIR}")

function(snx_component name)
    add_library(snx_${name} OBJECT ${ARGN})
    target_link_libraries(snx_${name} PUBLIC snx_nx_flags)
endfunction()

snx_component(core runtime/main.c runtime/arm.c runtime/loader.c runtime/rtapi.c runtime/clock.c runtime/pause.c runtime/state.c runtime/state_fns.c runtime/state_file.c runtime/crash.c runtime/options.c
              runtime/startup.c runtime/gdbstub.c runtime/present.c runtime/guest_jit.c
              runtime/cheats.c)
target_link_libraries(snx_core PRIVATE snx_driver snx_lz4 snx_zstd snx_build_info)
target_link_libraries(snx_core PUBLIC snx_settings_runtime) # the game's .ini's tables (cmake/settings.cmake)
if(APPLE)
    # main.c's weak snx_module_static, which no build defines yet: ld64 refuses an undefined symbol, weak or not,
    # unless told it is resolved at run time
    target_link_options(snx_core INTERFACE "LINKER:-U,_snx_module_static")
endif()
snx_component(kernel hle/hle.c hle/kernel/kernel.c hle/kernel/thread.c hle/kernel/sync.c hle/svc/svc.c hle/svc/svc_mem.c hle/kernel/debug.c
              memory/mem.c memory/own.c hle/kernel/ipc.c)
# the SVC dispatch, generated from hle/svc/hos.svc
include("${PROJECT_SOURCE_DIR}/cmake/svcgen.cmake")
snx_svcgen(snx_kernel)
snx_component(hle hle/services/services.c hle/services/audren.c hle/services/audren_dsp.c hle/services/pl.c hle/services/pl_font.c hle/services/sm.c hle/services/am.c hle/services/set.c hle/services/time.c hle/services/acc.c hle/services/users.c hle/services/audio.c
              hle/services/misc.c hle/services/ldr.c hle/services/network/bsd.c hle/services/network/dns.c
              hle/services/network/nifm.c hle/services/network/ssl.c hle/services/network/ldn.c hle/services/network/room.c hle/filesystem/fs.c hle/services/hid.c hle/services/hid_motion.c hle/services/hid_touch.c hle/services/vi.c hle/services/nvdrv.c
              hle/hook/hook.c hle/hook/libc.c)
# the shared fonts' stand-ins (cmake/shared_fonts.cmake), which hle/services/pl.c hands out
target_sources(snx_hle PRIVATE "${PROJECT_BINARY_DIR}/generated/shared_fonts.c")
# the hooks' adapters and table, generated from hle/hook/hooks.hook
include("${PROJECT_SOURCE_DIR}/cmake/hookgen.cmake")
snx_hookgen(snx_hle)
# the IPC interfaces of the services (docs/IPC.md): their dispatchers are generated, the handlers are in the services'
# files. A type is defined before the files that use it
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
snx_swipc(snx_hle FNS ROOT "${CMAKE_CURRENT_SOURCE_DIR}" FILES
          hle/services/types.swipc hle/services/sm.swipc hle/services/set.swipc hle/services/acc.swipc
          hle/services/audio.swipc hle/services/ldr.swipc hle/services/pl.swipc hle/services/time.swipc
          hle/services/misc.swipc hle/services/am.swipc hle/services/hid.swipc
          hle/services/vi.swipc hle/services/nvdrv.swipc hle/services/network/bsd.swipc
          hle/services/network/dns.swipc hle/services/network/nifm.swipc hle/services/network/ssl.swipc
          hle/services/network/ldn.swipc
          hle/filesystem/fs.swipc)
snx_component(video gpu/video/host1x.c gpu/video/nvdec.c gpu/video/vp9_rebuild.cpp gpu/video/vic.c)
target_link_libraries(snx_video PRIVATE FFmpeg::avcodec)
# hwopus (hle/services/audio.c) decodes Opus with FFmpeg too; acc's user picture is a splash drawing (cmake/splash.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/splash.cmake")
target_link_libraries(snx_hle PRIVATE FFmpeg::avcodec OpenSSL::SSL snx_splash)
snx_component(gpu gpu/gpu.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/mme_hle.c gpu/maxwell/mme_jit.c gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c gpu/maxwell/write_pages.c gpu/maxwell/maxwell3d.c gpu/renderer.c
              gpu/nvn_trace.c gpu/nvn/nvn_direct.c gpu/nvn/nvn_object.c gpu/nvn/nvn_cmd.c gpu/nvn/nvn_queue.c
              gpu/nvn/nvn_tex.c gpu/nvn/nvn_driver.c gpu/shipped_pass.c)
# the NVN functions nvn_direct implements: their adapters and table, generated from gpu/nvn/nvn.hook
snx_hookgen(snx_gpu SPEC "${CMAKE_CURRENT_SOURCE_DIR}/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook)
# mme_jit.c compiles the macros mme_hle.c has no implementation for with the LLVM backend
target_link_libraries(snx_gpu PRIVATE snx_llvm)
include("${PROJECT_SOURCE_DIR}/cmake/ship_walk.cmake")
snx_ship_walk(gpu/shipped_pass.c)

include("${PROJECT_SOURCE_DIR}/cmake/controls_atlas.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/gui_themes.cmake")
set(platform runtime/platform/plat.c runtime/platform/plat_sdl.c runtime/platform/unattended.c
             runtime/platform/backend/sdl3.c
             runtime/platform/backend/gamepad.c runtime/platform/tracy.c
             runtime/platform/hud.c runtime/platform/overlay.c runtime/platform/stats.c runtime/platform/touch.c
             runtime/platform/ui.c runtime/platform/vpad.c runtime/platform/fatal_screen.c
             runtime/platform/prepare_screen.c
             "${PROJECT_BINARY_DIR}/generated/controls_png.c")
if(WIN32)
    list(APPEND platform runtime/platform/plat_win32.c runtime/platform/profiler_win32.c)
else()
    list(APPEND platform runtime/platform/plat_posix.c)
endif()
if(SNX_TRACY)
    # the C++ client's connection id, which the C layer's GPU track keys its epochs on
    list(APPEND platform runtime/platform/tracy_conn.cpp)
endif()
snx_component(platform ${platform})

snx_component(shader gpu/shader/shader.c gpu/shader/decode.c gpu/shader/corpus.c gpu/shader/shipped.c
                     gpu/shader/spirv_translate.c gpu/shader/spirv_compute.c gpu/shader/structure.c)
# the opcode enum and decoder, generated from gpu/shader/maxwell_opcodes.def
include("${PROJECT_SOURCE_DIR}/cmake/shaderdec.cmake")
snx_shaderdec(snx_shader)
snx_component(vk gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/record.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c
                 gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/pipeline.c gpu/vk/pipeline_file.c gpu/vk/render.c gpu/vk/release.c gpu/vk/query.c gpu/vk/compute.c
                 gpu/vk/pending_pages.c gpu/vk/memory.c gpu/vk/renderdoc.c gpu/vk/astc.c gpu/vk/blocklinear.c gpu/vk/texconv.c gpu/vk/owned.c gpu/vk/nvdec.c gpu/vk/dma.c gpu/vk/blit.c gpu/vk/ui.c gpu/texture/blocklinear.c gpu/texture/depth.c gpu/vk/vic.c gpu/vk/vma.cpp gpu/vk/gpuprof.c
                 gpu/vk/ftz_probe.c
                 "${volk_SOURCE_DIR}/volk.c")
# the ASTC decoder's, the block linear deswizzle and swizzle's, the texture conversions', the VIC composition's and
# the render enable predicates' SPIR-V, compiled from their GLSL at build time (cmake/glsl.cmake). The VIC shader is
# built twice: for word aligned rows, and for the rest
# (gpu/video/vic_compose.glsl)
include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
# vk_hash (gpu/vk/cmd.c); the device setup the 3DS and the Vita share (src/common/gpu/vk)
target_link_libraries(snx_vk PUBLIC snx_xxhash snx_vk_common)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc.comp NAME astc_spv HEADER nx/gpu/texture/astc_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/bc7.comp NAME bc7_spv HEADER nx/gpu/texture/bc7_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/blocklinear.comp NAME blocklinear_spv HEADER nx/gpu/texture/blocklinear_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_spv HEADER nx/gpu/texture/texconv_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/video/vic.comp NAME vic_spv HEADER nx/gpu/video/vic_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/video/vic.comp NAME vic_unaligned_spv HEADER nx/gpu/video/vic_spv.h
               DEFINES VIC_UNALIGNED)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/query_predicate.comp NAME query_predicate_spv
               HEADER nx/gpu/vk/query_predicate_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/ftz_probe.comp NAME ftz_probe_spv HEADER nx/gpu/vk/ftz_probe_spv.h)
set(components core kernel hle video gpu platform shader vk)
# third-party code (vma.cpp is VMA's implementation), cmake/dependencies.cmake
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" gpu/vk/vma.cpp PROPERTIES COMPILE_OPTIONS
                            "${snx_third_party_flags}")

if(ANDROID)
    # the menu runs the program (src/multicall's libscarletnx.so) as libscarletnx-run.so, an executable named for the
    # app's libraries folder, which takes only lib*.so
    add_executable(scarletnx-run runtime/platform/android_run.c)
    set_target_properties(scarletnx-run PROPERTIES OUTPUT_NAME libscarletnx-run.so RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_link_libraries(scarletnx-run PRIVATE ${CMAKE_DL_LIBS})
    install(TARGETS scarletnx-run RUNTIME DESTINATION lib/${ANDROID_ABI})
endif()
list(TRANSFORM components PREPEND snx_)
add_library(scarletnx-nx STATIC)
set_target_properties(scarletnx-nx PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(scarletnx-nx PUBLIC snx_nx_flags ${components} snx_driver)
# The Switch driver's entry (runtime/entry.c's snx_switch_main), which scarletnx runs (src/multicall). Started with
# nothing to run it opens the menu (src/gui), which runs games with it; Android's app has the menu as a library of its
# own
add_library(snx_switch_program STATIC runtime/entry.c)
target_link_libraries(snx_switch_program PUBLIC scarletnx-nx SDL3::SDL3)
if(TARGET scarletnx-gui AND NOT ANDROID)
    target_link_libraries(snx_switch_program PUBLIC scarletnx-gui)
    target_compile_definitions(snx_switch_program PRIVATE SNX_WITH_GUI)
endif()

# SDL3 is either the system package or the pinned static source build (cmake/dependencies.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/window_icon.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/build_info.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/pad_sdl.cmake")
target_link_libraries(snx_platform PRIVATE SDL3::SDL3 snx_window_icon snx_build_info snx_pad_sdl snx_splash)
# the build is part of the key of the shader files, .vkshaders and .vkshipped (gpu/vk/pipeline.c)
target_link_libraries(snx_vk PRIVATE snx_build_info FFmpeg::avutil) # vk/nvdec.c: FFmpeg's Vulkan device and pictures
# the game's menu in YoRHa's colours, from src/gui/themes/yorha/theme.ini (cmake/gui_themes.cmake), and the shader
# loading screen's when the launcher's theme can't be read
target_include_directories(snx_platform PRIVATE "${PROJECT_BINARY_DIR}/generated")

if(BUILD_TESTING)
    add_executable(snx-test-shader-direct "${PROJECT_SOURCE_DIR}/tests/shader_direct.c")
    snx_test_target(snx-test-shader-direct 11)
    target_link_libraries(snx-test-shader-direct PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-direct PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_direct_structure COMMAND $<TARGET_FILE:snx-test-shader-direct> --check-only)
    add_executable(snx-test-shader-direct-regressions "${PROJECT_SOURCE_DIR}/tests/shader_direct_regressions.c")
    snx_test_target(snx-test-shader-direct-regressions 11)
    target_link_libraries(snx-test-shader-direct-regressions PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-direct-regressions PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_direct_regressions COMMAND $<TARGET_FILE:snx-test-shader-direct-regressions>)
    add_test(NAME runtime_settings_list COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
             -P "${PROJECT_SOURCE_DIR}/tests/settings_list.cmake")
    add_test(NAME runtime_unknown_option COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
             -P "${PROJECT_SOURCE_DIR}/tests/unknown_option.cmake")
    add_test(NAME runtime_recompile_settings COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
             -P "${PROJECT_SOURCE_DIR}/tests/recompile_settings.cmake")
    if(NOT WIN32) # Windows' error box would wait for a click
        add_test(NAME runtime_game_copy COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
                 -P "${PROJECT_SOURCE_DIR}/tests/game_copy.cmake")
    endif()
    find_program(SNX_SPIRV_VAL spirv-val)
    find_program(SNX_SPIRV_DIS spirv-dis)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND) # scarletnx --mcp over stdio (runtime/driver/mcp.c)
        add_test(NAME mcp COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/mcp_test.py"
                 $<TARGET_FILE:scarletnx>)
    endif()
    if(SNX_SPIRV_VAL AND SNX_SPIRV_DIS AND Python3_Interpreter_FOUND)
        add_test(NAME shader_direct COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/shader_direct_test.py"
            --generator $<TARGET_FILE:snx-test-shader-direct> --validator "${SNX_SPIRV_VAL}"
            --disassembler "${SNX_SPIRV_DIS}"
            --regressions $<TARGET_FILE:snx-test-shader-direct-regressions>)
    else()
        message(STATUS "Skipping shader_direct: Python3, spirv-val, and spirv-dis are required")
    endif()
endif()

# Opt-in because normal unit-test runs need neither an ICD nor a GPU. All rendering uses the real runtime.
if(BUILD_TESTING AND SNX_TEST_VULKAN)
    add_executable(snx-test-vulkan "${PROJECT_SOURCE_DIR}/tests/vulkan_headless.c"
        "${PROJECT_SOURCE_DIR}/tests/spirv_emitter_fixture.c" "${PROJECT_SOURCE_DIR}/tests/support/nvdec_ref.c"
        "${PROJECT_SOURCE_DIR}/tests/support/vic_ref.c" "${PROJECT_SOURCE_DIR}/tests/support/dma_ref.c"
        "${PROJECT_SOURCE_DIR}/tests/support/fermi2d_ref.c" "${PROJECT_SOURCE_DIR}/tests/support/half.c")
    target_link_libraries(snx-test-vulkan PRIVATE snx_nx_flags ${components} snx_driver FFmpeg::avutil Threads::Threads
                                                  ${CMAKE_DL_LIBS})
    set_target_properties(snx-test-vulkan PROPERTIES LINKER_LANGUAGE CXX)
    if(WIN32)
        target_link_libraries(snx-test-vulkan PRIVATE user32)
    else()
        target_link_libraries(snx-test-vulkan PRIVATE m)
    endif()
    target_link_libraries(snx-test-vulkan PRIVATE SDL3::SDL3)
    add_test(NAME vulkan_headless COMMAND snx-test-vulkan -H)
    set_tests_properties(vulkan_headless PROPERTIES TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # VIC only, under the validation layer when it is installed: its job binds the same buffer as a uniform and a
    # storage buffer, which the layer checks against the buffer's usage flags (VUID-VkWriteDescriptorSet-
    # descriptorType-00330). Without the layer this is VIC's own checks again.
    add_test(NAME vulkan_headless_vic_validation COMMAND snx-test-vulkan -H --vk_validation=1)
    set_tests_properties(vulkan_headless_vic_validation PROPERTIES TIMEOUT 60
        ENVIRONMENT "SNX_TEST_VIC_ONLY=1"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the ASTC upload against the CPU decoder, block linear moves on the GPU against bl_offset's layout and texture
    # conversions against the CPU's, with the
    # backend's own frames, staging and allocations and the SPIR-V
    # the build makes (the file snx_glsl_spirv generated for snx_vk above). Again under a 4 MiB allocation cap, so
    # images in pieces are tested too. `snx-test-astc-vulkan --large` decodes two 512 MiB textures (minutes on lavapipe)
    # cmd.c records the frame's commands through record.c's thread, with the platform's threads and locks
    if(WIN32)
        set(snx_astc_plat runtime/platform/plat_win32.c runtime/platform/plat_sdl.c)
    else()
        set(snx_astc_plat runtime/platform/plat_posix.c runtime/platform/plat_sdl.c)
    endif()
    add_executable(snx-test-astc-vulkan "${PROJECT_SOURCE_DIR}/tests/astc_vulkan.c" "${PROJECT_SOURCE_DIR}/tests/tracy_stubs.c"
        "${PROJECT_SOURCE_DIR}/tests/support/astc.c" gpu/vk/astc.c
        gpu/vk/blocklinear.c gpu/texture/blocklinear.c gpu/vk/texconv.c "${PROJECT_SOURCE_DIR}/tests/support/etc2.c"
        "${PROJECT_SOURCE_DIR}/tests/support/bcn.c"
        gpu/vk/cmd.c gpu/vk/memory.c gpu/vk/record.c gpu/vk/vma.cpp ${snx_astc_plat} "${volk_SOURCE_DIR}/volk.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_spv.c" "${PROJECT_BINARY_DIR}/generated/spirv/blocklinear_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/texconv_spv.c" "${PROJECT_BINARY_DIR}/generated/spirv/bc7_spv.c"
        "${PROJECT_SOURCE_DIR}/tests/glsl/bc7_cpu.cpp")
    target_include_directories(snx-test-astc-vulkan PRIVATE "${PROJECT_SOURCE_DIR}/tests/glsl")
    target_link_libraries(snx-test-astc-vulkan PRIVATE snx_nx_flags scarletnx-filesystem snx_xxhash Threads::Threads
                                                      SDL3::SDL3 ${CMAKE_DL_LIBS})
    set_target_properties(snx-test-astc-vulkan PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME astc_vulkan COMMAND snx-test-astc-vulkan)
    add_test(NAME astc_vulkan_pieces COMMAND snx-test-astc-vulkan --vk_max_allocation_mb=4)
    # pieces, and then the line the program prints only once every comparison passed: a pass expression alone
    # passes the test whatever the exit code
    set_tests_properties(astc_vulkan_pieces PROPERTIES PASS_REGULAR_EXPRESSION
                         "memory in pieces.*All ASTC Vulkan comparisons passed")
    set_tests_properties(astc_vulkan astc_vulkan_pieces PROPERTIES TIMEOUT 300)
endif()
if(BUILD_TESTING)
    snx_sanitizer_path() # tests/CMakeLists.txt: this folder's tests
endif()
