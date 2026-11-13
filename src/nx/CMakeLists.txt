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
target_include_directories(snx_nx_flags BEFORE INTERFACE "${volk_SOURCE_DIR}" "${renderdoc_app_SOURCE_DIR}")

function(snx_component name)
    add_library(snx_${name} OBJECT ${ARGN})
    target_link_libraries(snx_${name} PUBLIC snx_nx_flags)
endfunction()

snx_component(core runtime/main.c runtime/arm.c runtime/loader.c runtime/linker.c runtime/rtapi.c runtime/hints.c runtime/clock.c runtime/pause.c runtime/state.c runtime/state_fns.c runtime/state_file.c runtime/crash.c runtime/options.c runtime/deprecated_env.c
              runtime/startup.c runtime/gdbstub.c runtime/present.c runtime/guest_jit.c runtime/guest_stats.c
              runtime/cheats.c runtime/nce/nce.c runtime/nce/nce_host.c runtime/nce/nce_net.c)
target_link_libraries(snx_core PRIVATE snx_driver snx_lz4 snx_zstd snx_build_info)
target_link_libraries(snx_core PUBLIC snx_settings_runtime) # the game's .ini's tables (cmake/settings.cmake)
if(APPLE)
    # main.c's weak snx_module_static, which no build defines yet: ld64 refuses an undefined symbol, weak or not,
    # unless told it is resolved at run time
    target_link_options(snx_core INTERFACE "LINKER:-U,_snx_module_static")
endif()
snx_component(kernel hle/hle.c hle/kernel/kernel.c hle/kernel/thread.c hle/kernel/sync.c hle/svc/svc.c hle/svc/svc_mem.c hle/kernel/debug.c
              memory/mem.c memory/own.c hle/kernel/ipc.c hle/result.c)
# the SVC dispatch, generated from hle/svc/hos.svc
include("${PROJECT_SOURCE_DIR}/cmake/svcgen.cmake")
snx_svcgen(snx_kernel)
snx_component(hle hle/services/services.c hle/services/audren.c hle/services/audren_dsp.c hle/services/sm.c hle/services/am.c hle/services/gsv.c hle/services/native_video.c hle/services/native_video_core.c hle/services/native_video_decoder.c hle/services/set.c  hle/services/acc.c hle/services/users.c hle/services/audio.c
              hle/services/misc.c hle/services/ldr.c hle/services/network/bsd.c hle/services/network/dns.c
              hle/services/network/nifm.c hle/services/network/ssl.c hle/services/network/ldn.c hle/services/network/room.c hle/filesystem/fs.c hle/services/hid.c hle/services/hid_motion.c hle/services/hid_touch.c hle/services/vi.c hle/services/nvdrv.c
              hle/hook/hook.c hle/hook/libc.c hle/hook/diag.c hle/hook/codec.c hle/hook/audio_stream.c hle/hook/verify.c hle/hook/version.c
              nn/ro.c nn/fs.c nn/time.c nn/pl.c nn/pl_font.c nn/pctl.c nn/nsd.c nn/ngc.c nn/prepo.c
              nn/audio.c nn/audio_renderer.c nn/codec.c)
# the shared fonts' stand-ins (cmake/shared_fonts.cmake), which nn/pl.c hands out
target_sources(snx_hle PRIVATE "${PROJECT_BINARY_DIR}/generated/shared_fonts.c")
if(APPLE)
    # the media engine's AV1 decoder (hle/services/native_video_vt.c): VideoToolbox decodes and hands back CVPixelBuffers,
    # which is a macOS or iOS library, and no other build has the file
    target_sources(snx_hle PRIVATE hle/services/native_video_vt.c)
    target_link_libraries(snx_hle PUBLIC "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
endif()
# the hooks' adapters and table, generated from hle/hook/hooks.hook and the game patches' address hooks
# (patches/*.hook), whose handlers are the .c files beside them
include("${PROJECT_SOURCE_DIR}/cmake/hookgen.cmake")
snx_hookgen(snx_hle A32_MARKED)
file(GLOB snx_patch_handlers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.c")
target_sources(snx_hle PRIVATE ${snx_patch_handlers})
# the IPC interfaces of the services (docs/platforms/nx/ipc.md): their dispatchers are generated, the handlers are in the services'
# files. A type is defined before the files that use it
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
snx_swipc(snx_hle FNS ROOT "${CMAKE_CURRENT_SOURCE_DIR}" FILES
          hle/services/types.swipc hle/services/sm.swipc hle/services/set.swipc hle/services/acc.swipc
          hle/services/audio.swipc hle/services/ldr.swipc
          hle/services/misc.swipc hle/services/am.swipc hle/services/native_video.swipc hle/services/gsv.swipc hle/services/hid.swipc
          hle/services/vi.swipc hle/services/nvdrv.swipc hle/services/network/bsd.swipc
          hle/services/network/dns.swipc hle/services/network/nifm.swipc hle/services/network/ssl.swipc
          hle/services/network/ldn.swipc)
snx_component(video gpu/video/host1x.c gpu/video/nvdec.c gpu/video/vp9_rebuild.cpp gpu/video/vic.c)
target_link_libraries(snx_video PRIVATE FFmpeg::avcodec)
# hwopus (hle/services/audio.c) decodes Opus with FFmpeg too; acc's user picture is a splash drawing (cmake/splash.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/splash.cmake")
target_link_libraries(snx_hle PRIVATE FFmpeg::avcodec OpenSSL::SSL snx_splash)
snx_component(gpu gpu/gpu.c gpu/guest_pools.c gpu/guest_textures.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c gpu/maxwell/write_pages.c gpu/draw_state.c gpu/maxwell/draw_state.c gpu/maxwell/maxwell3d.c gpu/renderer.c
              gpu/nvn/nvn_loader.c gpu/nvn/nvn_object.c gpu/nvn/nvn_cmd.c gpu/nvn/nvn_queue.c gpu/nvn/nvn_dlss.c
              gpu/nvn/nvn_tex.c gpu/nvn/nvn_driver.c gpu/nvn/nvn_tic.c gpu/nvn/nvn_replay.c gpu/nvn/nvn_draw.c gpu/nvn/nvn_copy.c gpu/nvn/nvn_state.c
              gpu/nvn/nvn_save.c
              gpu/shipped_pass.c)
# the NVN functions nvn implements: their adapters and table, generated from gpu/nvn/nvn.hook
target_link_libraries(snx_gpu PRIVATE snx_zstd) # an Ounce game's .zs shader files (gpu/shipped_pass.c)
snx_hookgen(snx_gpu SPEC "${CMAKE_CURRENT_SOURCE_DIR}/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
include("${PROJECT_SOURCE_DIR}/cmake/ship_walk.cmake")
snx_ship_walk(gpu/shipped_pass.c)

include("${PROJECT_SOURCE_DIR}/cmake/gui_themes.cmake")
set(platform runtime/platform/log.c runtime/platform/plat_sdl.c runtime/platform/unattended.c
             runtime/platform/backend/sdl3.c runtime/platform/audio_downmix.c
             runtime/platform/backend/gamepad.c runtime/platform/connect.c runtime/platform/tracy.c
             runtime/platform/help.c runtime/platform/hotkeys.c runtime/platform/hud.c runtime/platform/overlay.c
             runtime/platform/stats.c
             runtime/platform/touch.c
             runtime/platform/ui.c runtime/platform/vpad.c
             runtime/platform/fatal_screen.c
             runtime/platform/prepare_screen.c runtime/platform/gpu_driver_load.c runtime/platform/resources.c)
if(WIN32)
    list(APPEND platform runtime/platform/plat_win32.c runtime/platform/profiler_win32.c)
else()
    list(APPEND platform runtime/platform/plat_posix.c)
endif()
if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
    # a GPU driver package loaded in place of the system's driver (runtime/platform/gpu_driver_load.c), on the
    # packages' one ABI
    list(APPEND platform runtime/platform/gpu_driver_android.c)
endif()
if(SNX_TRACY)
    # the C++ client's connection id, which the C layer's GPU track keys its epochs on
    list(APPEND platform runtime/platform/tracy_conn.cpp)
endif()
snx_component(platform ${platform})
# the tracy-capture a capture needs (tracy.c), the client's release (cmake/dependencies.cmake)
target_compile_definitions(snx_platform PRIVATE SNX_TRACY_VERSION="${SNX_TRACY_VERSION}")

snx_component(shader gpu/shader/shader.c gpu/shader/decode.c gpu/shader/corpus.c gpu/shader/shipped.c
                     gpu/shader/spirv_translate.c gpu/shader/spirv_compute.c gpu/shader/structure.c
                     gpu/shader/shared_reach.c gpu/shader/registers.c gpu/shader/forms.c gpu/shader/dataflow.c
                     gpu/shader/emit_int.c gpu/shader/emit_float.c gpu/shader/emit_memory.c gpu/shader/emit_texture.c
                     gpu/shader/emit_io.c gpu/shader/emit_flow.c gpu/shader/emit_thread.c
                     gpu/shader/sm86_decode.c gpu/shader/sm86_translate.c gpu/shader/sm86_eval.c gpu/shader/advanced_blend_spirv.c
                     gpu/shader/coverage_spirv.c gpu/shader/capture_tiles_spirv.c gpu/shader/depth_stencil_spirv.c gpu/shader/write_log.c
                     gpu/shader/minmax_spirv.c)
# decode.c and sm86_decode.c include the Maxwell and SM86 decoders tools/decodetree makes from gpu/shader/sm53.decode
# and sm86.decode (cmake/decodetree.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/decodetree.cmake")
snx_decodetree(snx_shader OUTPUT decode-sm53.inc DECODE "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/sm53.decode"
               OPTIONS -w 16 --static-decode=decode_sm53)
snx_decodetree(snx_shader OUTPUT decode-sm86.inc DECODE "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/sm86.decode"
               OPTIONS -w 16 --static-decode=decode_sm86)
snx_component(vk gpu/vk/renderer.c gpu/vk/device.c gpu/vk/descriptors.c gpu/vk/descriptor_heaps.c gpu/vk/descriptor_objects.c gpu/vk/cmd.c gpu/vk/record.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c
                 gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/writelog.c gpu/vk/mirror.c gpu/vk/pipeline.c gpu/vk/pipeline_file.c gpu/vk/render.c gpu/vk/release.c gpu/vk/query.c gpu/vk/xfb.c gpu/vk/display.c gpu/vk/compute.c
                 gpu/vk/pending_pages.c gpu/vk/checkpoints.c gpu/vk/image_out.c gpu/vk/depth_pack.c gpu/vk/upscale.c gpu/vk/memory.c gpu/vk/ledger.c gpu/vk/pagespace.c gpu/vk/range_shape.c gpu/vk/tex_census.c gpu/vk/tex_report.c gpu/vk/resource_stats.c gpu/vk/renderdoc.c gpu/vk/astc.c gpu/vk/bc7_cache.c gpu/vk/blocklinear.c gpu/vk/texconv.c gpu/vk/owned.c gpu/vk/nvdec.c gpu/vk/dma.c gpu/vk/blit.c gpu/vk/msaa.c gpu/vk/ui.c gpu/vk/hdr_swapchain.c gpu/texture/blocklinear.c gpu/texture/depth.c gpu/vk/vic.c gpu/vk/devmem.c gpu/vk/space.c gpu/vk/gpuprof.c gpu/vk/uma.c
                 gpu/vk/ftz_probe.c gpu/vk/vertex_input.c gpu/vk/pipeline_parts.c gpu/vk/cb_fold.c
                 gpu/vk/draw_texture.c gpu/vk/draw_texture_shader.c gpu/vk/tess_passthrough.c gpu/vk/varying_geometry.c gpu/vk/present_quad.c gpu/vk/capture.c gpu/vk/depth_shadow.c gpu/vk/predraw.c
                 "${volk_SOURCE_DIR}/volk.c")
# the ASTC decoder's, the block linear deswizzle and swizzle's, the texture conversions', the VIC composition's and
# the render enable predicates' SPIR-V, compiled from their GLSL at build time (cmake/glsl.cmake). The VIC shader is
# built twice: for word aligned rows, and for the rest
# (gpu/video/vic_compose.glsl)
include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
# vk_hash (gpu/vk/cmd.c); the device setup the 3DS and the Vita share (src/common/gpu/vk); the shader file's units
# (gpu/vk/pipeline_file.c)
target_link_libraries(snx_vk PUBLIC snx_xxhash snx_vk_common snx_fsr2 PRIVATE snx_zstd)
# AMD FidelityFX Super Resolution 2.2.1 (cmake/dependencies.cmake): AMD's library and Vulkan backend as they are
# (C++, third-party flags, gpu/vk/fsr2/fsr2_compat.h before them for the MSVC names they use), with our shader blobs
# (gpu/vk/fsr2/fsr2_blobs.c over the passes cmake/fsr2_shaders.cmake compiles) and C interface (gpu/vk/fsr2/fsr2.cpp)
set(snx_fsr2_spv "${PROJECT_BINARY_DIR}/generated/spirv/fsr2_spv.c")
file(GLOB snx_fsr2_shader_sources CONFIGURE_DEPENDS "${snx_fsr2_api_dir}/shaders/*.glsl" "${snx_fsr2_api_dir}/shaders/*.h")
add_custom_command(OUTPUT "${snx_fsr2_spv}"
    COMMAND "${CMAKE_COMMAND}" "-DGLSLANG=${SNX_GLSLANG}" "-DSPIRV_OPT=${SNX_SPIRV_OPT}" "-DSPIRV_VAL=${SNX_SPIRV_VAL}"
            "-DSHADERS=${snx_fsr2_api_dir}/shaders" "-DWORK=${PROJECT_BINARY_DIR}/generated/spirv/fsr2"
            "-DOUT=${snx_fsr2_spv}" -P "${PROJECT_SOURCE_DIR}/cmake/fsr2_shaders.cmake"
    DEPENDS "${PROJECT_SOURCE_DIR}/cmake/fsr2_shaders.cmake" ${snx_fsr2_shader_sources}
    COMMENT "Compiling FSR 2.2.1's passes to SPIR-V"
    VERBATIM)
set(snx_fsr2_amd "${snx_fsr2_api_dir}/ffx_fsr2.cpp" "${snx_fsr2_api_dir}/ffx_assert.cpp"
                 "${snx_fsr2_api_dir}/vk/ffx_fsr2_vk.cpp")
add_library(snx_fsr2 STATIC ${snx_fsr2_amd} gpu/vk/fsr2/fsr2_blobs.c gpu/vk/fsr2/fsr2.cpp "${snx_fsr2_spv}")
set_target_properties(snx_fsr2 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(snx_fsr2 PRIVATE VK_NO_PROTOTYPES FFX_GCC)
target_include_directories(snx_fsr2 PRIVATE "${snx_fsr2_api_dir}" "${PROJECT_SOURCE_DIR}/src")
set_source_files_properties(${snx_fsr2_amd} PROPERTIES COMPILE_OPTIONS
                            "${snx_third_party_flags};-include;${CMAKE_CURRENT_SOURCE_DIR}/gpu/vk/fsr2/fsr2_compat.h")
target_link_libraries(snx_fsr2 PUBLIC snx_vk_common)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc.comp NAME astc_spv HEADER nx/gpu/texture/astc_spv.h)
# ASTC blocks straight into BC7 blocks, for a GPU that samples BC7 and not ASTC (gpu/vk/astc.c), in the encoder's
# builds: the full one; the fast one, every mode only where mode 6 is not close, for large levels; mode 6 alone for the
# largest; and mode 6 from its first line alone, a very large level's quick encode until the background's fast build is
# in the cache. The full and the fast one are two passes each (SNX_BC7_PASS 1 and 2, gpu/texture/astc_bc7.glsl), and
# the fast one's second pass, the search, takes what the ASTC blocks say of each block (SNX_BC7_HINTS). Then
# all of them with SPIR-V's packed dot product (Vulkan 1.3's shaderIntegerDotProduct), for a GPU that says it has an
# instruction for it: the same blocks
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_a_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               DEFINES SNX_BC7_PASS=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_b_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               DEFINES SNX_BC7_PASS=2)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_fast_a_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               DEFINES SNX_BC7_FAST=1 SNX_BC7_PASS=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_fast_b_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               DEFINES SNX_BC7_FAST=1 SNX_BC7_PASS=2 SNX_BC7_HINTS=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_mode6_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               DEFINES SNX_BC7_FAST=2)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_quick_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               DEFINES SNX_BC7_FAST=3)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_a_dot_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               TARGET vulkan1.3 DEFINES SNX_BC7_PASS=1 SNX_BC7_DOT=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_b_dot_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               TARGET vulkan1.3 DEFINES SNX_BC7_PASS=2 SNX_BC7_DOT=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_fast_a_dot_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               TARGET vulkan1.3 DEFINES SNX_BC7_FAST=1 SNX_BC7_PASS=1 SNX_BC7_DOT=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_fast_b_dot_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               TARGET vulkan1.3 DEFINES SNX_BC7_FAST=1 SNX_BC7_PASS=2 SNX_BC7_HINTS=1 SNX_BC7_DOT=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_mode6_dot_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               TARGET vulkan1.3 DEFINES SNX_BC7_FAST=2 SNX_BC7_DOT=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/astc_bc7.comp NAME astc_bc7_quick_dot_spv HEADER nx/gpu/texture/astc_bc7_spv.h
               TARGET vulkan1.3 DEFINES SNX_BC7_FAST=3 SNX_BC7_DOT=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/blocklinear.comp NAME blocklinear_spv HEADER nx/gpu/texture/blocklinear_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_spv HEADER nx/gpu/texture/texconv_spv.h)
# its ETC2 build: BCn encoded again as ETC2 and EAC, for a GPU that samples those and not BCn (gpu/vk/texconv.c)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_etc2_spv HEADER nx/gpu/texture/texconv_spv.h
               DEFINES SNX_TC_ETC2)
# its ASTC build: BC2, BC3 and BC7 encoded again as ASTC 4x4, for a GPU that samples ASTC and not BCn
# (gpu/texture/astc_encode.glsl over bc7_encode.glsl's fitter)
# in three passes (gpu/texture/texconv.glsl's SNX_ASTC_PASS): the blocks that are an ASTC layout as they stand, moved
# with no texel decoded; then those the BCn block's own lines settle; then the search for the rest
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_astc_spv HEADER nx/gpu/texture/texconv_spv.h
               DEFINES SNX_TC_ASTC SNX_ASTC_ENC=1 SNX_ASTC_PASS=1)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_astc_b_spv HEADER nx/gpu/texture/texconv_spv.h
               DEFINES SNX_TC_ASTC SNX_ASTC_ENC=1 SNX_ASTC_PASS=2)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_astc_c_spv HEADER nx/gpu/texture/texconv_spv.h
               DEFINES SNX_TC_ASTC SNX_ASTC_ENC=1 SNX_ASTC_PASS=3)
# and its conversion build: the blits that convert between pixel formats, which a driver's compiler refused inside the
# plain build (gpu/vk/texconv.c)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_convert_spv HEADER nx/gpu/texture/texconv_spv.h
               DEFINES SNX_TC_CONVERT)
# and its NV12 build: a CPU decoded picture's chroma interleaved, which a movie's first picture needs and the plain
# build is a long compile for on a slow GPU (gpu/vk/texconv.c)
snx_glsl_spirv(snx_vk SOURCE gpu/texture/texconv.comp NAME texconv_nv12_spv HEADER nx/gpu/texture/texconv_spv.h
               DEFINES SNX_TC_NV12)
snx_glsl_spirv(snx_vk SOURCE gpu/video/vic.comp NAME vic_spv HEADER nx/gpu/video/vic_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/video/vic.comp NAME vic_unaligned_spv HEADER nx/gpu/video/vic_spv.h
               DEFINES VIC_UNALIGNED)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/query_predicate.comp NAME query_predicate_spv
               HEADER nx/gpu/vk/query_predicate_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/query_count.comp NAME query_count_spv HEADER nx/gpu/vk/query_count_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/xfb_indirect.comp NAME xfb_indirect_spv HEADER nx/gpu/vk/xfb_indirect_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/depth_pack.comp NAME depth_pack_spv HEADER nx/gpu/vk/depth_pack_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/mirror_harvest.comp NAME mirror_harvest_spv HEADER nx/gpu/vk/mirror_harvest_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/mirror_fold.comp NAME mirror_fold_spv HEADER nx/gpu/vk/mirror_fold_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/wlog_merge.comp NAME wlog_merge_spv HEADER nx/gpu/vk/wlog_merge_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/query_compare.comp NAME query_compare_spv
               HEADER nx/gpu/vk/query_compare_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/ftz_probe.comp NAME ftz_probe_spv HEADER nx/gpu/vk/ftz_probe_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/msaa.vert NAME msaa_vert_spv HEADER nx/gpu/vk/msaa_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/shaders/ui.vert NAME snx_ui_vs_spv HEADER nx/gpu/vk/ui_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/shaders/ui.frag NAME snx_ui_fs_spv HEADER nx/gpu/vk/ui_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/present_quad.vert NAME present_quad_vert_spv HEADER nx/gpu/vk/present_quad_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/present_quad.frag NAME present_quad_frag_spv HEADER nx/gpu/vk/present_quad_spv.h)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/msaa.frag NAME msaa_unpack_spv HEADER nx/gpu/vk/msaa_spv.h DEFINES MS_UNPACK)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/msaa.frag NAME msaa_unpack_depth_spv HEADER nx/gpu/vk/msaa_spv.h
               DEFINES MS_UNPACK_DEPTH)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/msaa.frag NAME msaa_pack_spv HEADER nx/gpu/vk/msaa_spv.h DEFINES MS_PACK)
# the built-in modules: depth-only passes' empty fragment shader (gpu/vk/pipeline.c), and the masked clears' triangle
# and color in each number type (gpu/vk/render.c). Vulkan 1.2's SPIR-V 1.5, for the vertex shader's gl_Layer
snx_glsl_spirv(snx_vk SOURCE gpu/vk/null.frag NAME snx_null_fs_spv HEADER nx/gpu/vk/internal_spv.h TARGET vulkan1.2)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/clear.vert NAME snx_clear_vs_spv HEADER nx/gpu/vk/internal_spv.h TARGET vulkan1.2)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/clear.frag NAME snx_clear_fs_float_spv HEADER nx/gpu/vk/internal_spv.h
               TARGET vulkan1.2)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/clear.frag NAME snx_clear_fs_uint_spv HEADER nx/gpu/vk/internal_spv.h
               TARGET vulkan1.2 DEFINES CLEAR_UINT)
snx_glsl_spirv(snx_vk SOURCE gpu/vk/clear.frag NAME snx_clear_fs_sint_spv HEADER nx/gpu/vk/internal_spv.h
               TARGET vulkan1.2 DEFINES CLEAR_SINT)
set(components core kernel hle video gpu platform shader vk)
# third-party code, cmake/dependencies.cmake
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS
                            "${snx_third_party_flags}")

if(ANDROID)
    # the menu runs the program (src/multicall's libscarletnx.so) as libscarletnx-run.so, an executable named for the
    # app's libraries folder, which takes only lib*.so
    add_executable(scarletnx-run runtime/platform/android_run.c)
    set_target_properties(scarletnx-run PROPERTIES OUTPUT_NAME libscarletnx-run.so RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_link_libraries(scarletnx-run PRIVATE ${CMAKE_DL_LIBS})
    if(SNX_JEMALLOC)
        # jemalloc's malloc for the driver's whole process, its recompiles' LLVM threads among it
        # (cmake/jemalloc.cmake): the C library's own calls to malloc and free and libscarletnx.so's bind to the ones
        # the program exports
        target_link_libraries(scarletnx-run PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,snx_jemalloc>")
        list(TRANSFORM SNX_JEMALLOC_EXPORTS PREPEND "LINKER:--export-dynamic-symbol="
             OUTPUT_VARIABLE snx_jemalloc_exports)
        target_link_options(scarletnx-run PRIVATE ${snx_jemalloc_exports})
    endif()
    install(TARGETS scarletnx-run RUNTIME DESTINATION lib/${ANDROID_ABI})
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        # the two calls Android's Vulkan loader opens its driver with, for the copy of it a run with a GPU driver
        # package loads (runtime/platform/gpu_driver_hook.h): -z global is what puts them before the system's
        add_library(snx_gpu_driver_hook SHARED runtime/platform/gpu_driver_hook.c)
        target_include_directories(snx_gpu_driver_hook PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_options(snx_gpu_driver_hook PRIVATE -Wall -Wextra -Werror=missing-prototypes)
        target_link_options(snx_gpu_driver_hook PRIVATE LINKER:-z,global)
        set_target_properties(snx_gpu_driver_hook PROPERTIES OUTPUT_NAME scarletnx-gpu-hook)
        install(TARGETS snx_gpu_driver_hook LIBRARY DESTINATION lib/${ANDROID_ABI})
        include(GNUInstallDirs)
        install(FILES runtime/platform/ADRENOTOOLS-NOTICE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    endif()
endif()
list(TRANSFORM components PREPEND snx_)
add_library(scarletnx-nx STATIC)
set_target_properties(scarletnx-nx PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(scarletnx-nx PUBLIC snx_nx_flags ${components} snx_driver)
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_link_libraries(scarletnx-nx PUBLIC execinfo) # backtrace, for the crash report's host frames (crash.c)
endif()
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
target_link_libraries(snx_platform PRIVATE SDL3::SDL3 snx_window_icon snx_build_info snx_pad_sdl snx_splash snx_notice)
if(WIN32)
    target_link_libraries(snx_platform PUBLIC synchronization powrprof) # plat_wait_word's WaitOnAddress, the power notification
endif()
if(APPLE AND NOT IOS) # plat_posix.c's power source: IOKit's notifications on CoreFoundation's run loop
    target_link_libraries(snx_platform PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()
# the build is part of the key of the shader files, .vkshaders and .vkshipped (gpu/vk/pipeline.c)
target_link_libraries(snx_vk PRIVATE snx_build_info FFmpeg::avutil) # vk/nvdec.c: FFmpeg's Vulkan device and pictures
# the game's menu in YoRHa's colours, from src/gui/themes/yorha/theme.ini (cmake/gui_themes.cmake), and the shader
# loading screen's when the launcher's theme can't be read
target_include_directories(snx_platform PRIVATE "${PROJECT_BINARY_DIR}/generated")

include("${PROJECT_SOURCE_DIR}/tests/cmake/nx.cmake") # its tests
