# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the Switch runtime (src/nx), included by src/nx/CMakeLists.txt where they were: they run in its scope, so
# its variables hold here, and a relative path is from src/nx
if(BUILD_TESTING)
    add_executable(snx-test-shader-direct "${PROJECT_SOURCE_DIR}/tests/shader_direct.c")
    snx_test_target(snx-test-shader-direct 11)
    target_link_libraries(snx-test-shader-direct PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-direct PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_direct_structure COMMAND $<TARGET_FILE:snx-test-shader-direct> --check-only)
    add_executable(snx-test-shader-direct-regressions "${PROJECT_SOURCE_DIR}/tests/shader_direct_regressions.c"
                   "${PROJECT_SOURCE_DIR}/tests/support/spv_eval.c")
    snx_test_target(snx-test-shader-direct-regressions 11)
    target_link_libraries(snx-test-shader-direct-regressions PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-direct-regressions PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_direct_regressions COMMAND $<TARGET_FILE:snx-test-shader-direct-regressions>)
    add_executable(snx-test-shader-mixed-sample-safe "${PROJECT_SOURCE_DIR}/tests/shader_mixed_sample_safe.c")
    snx_test_target(snx-test-shader-mixed-sample-safe 11)
    target_link_libraries(snx-test-shader-mixed-sample-safe PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-mixed-sample-safe PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_mixed_sample_safe COMMAND $<TARGET_FILE:snx-test-shader-mixed-sample-safe>)
    add_executable(snx-test-shader-float-rounding "${PROJECT_SOURCE_DIR}/tests/shader_float_rounding.c")
    snx_test_target(snx-test-shader-float-rounding 11)
    target_link_libraries(snx-test-shader-float-rounding PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-float-rounding PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_float_rounding COMMAND $<TARGET_FILE:snx-test-shader-float-rounding>)
    add_executable(snx-test-shader-indexed-ms "${PROJECT_SOURCE_DIR}/tests/shader_indexed_ms.c")
    snx_test_target(snx-test-shader-indexed-ms 11)
    target_link_libraries(snx-test-shader-indexed-ms PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-indexed-ms PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_indexed_ms COMMAND $<TARGET_FILE:snx-test-shader-indexed-ms>)
    add_executable(snx-test-shader-integer-half "${PROJECT_SOURCE_DIR}/tests/shader_integer_half.c")
    snx_test_target(snx-test-shader-integer-half 11)
    target_link_libraries(snx-test-shader-integer-half PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-integer-half PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_integer_half COMMAND $<TARGET_FILE:snx-test-shader-integer-half>)
    add_executable(snx-test-shader-tcs-dead-outputs "${PROJECT_SOURCE_DIR}/tests/shader_tcs_dead_outputs.c")
    snx_test_target(snx-test-shader-tcs-dead-outputs 11)
    target_link_libraries(snx-test-shader-tcs-dead-outputs PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-tcs-dead-outputs PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_tcs_dead_outputs COMMAND $<TARGET_FILE:snx-test-shader-tcs-dead-outputs>)
    add_executable(snx-test-shader-blend-images "${PROJECT_SOURCE_DIR}/tests/shader_blend_images.c")
    snx_test_target(snx-test-shader-blend-images 11)
    target_link_libraries(snx-test-shader-blend-images PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-blend-images PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_blend_images COMMAND $<TARGET_FILE:snx-test-shader-blend-images>)
    add_executable(snx-test-shader-half-native "${PROJECT_SOURCE_DIR}/tests/shader_half_native.c")
    snx_test_target(snx-test-shader-half-native 11)
    target_link_libraries(snx-test-shader-half-native PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-half-native PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_half_native COMMAND $<TARGET_FILE:snx-test-shader-half-native>)
    add_executable(snx-test-shader-txd-uniform "${PROJECT_SOURCE_DIR}/tests/shader_txd_uniform.c")
    snx_test_target(snx-test-shader-txd-uniform 11)
    target_link_libraries(snx-test-shader-txd-uniform PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-txd-uniform PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_txd_uniform COMMAND $<TARGET_FILE:snx-test-shader-txd-uniform>)
    add_executable(snx-test-shader-half-packing "${PROJECT_SOURCE_DIR}/tests/shader_half_packing.c")
    snx_test_target(snx-test-shader-half-packing 11)
    target_link_libraries(snx-test-shader-half-packing PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-half-packing PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_half_packing COMMAND $<TARGET_FILE:snx-test-shader-half-packing>)
    foreach(fixture texture_limits alpha_coverage coverage_resolve blend_resolve known_bits capture_tiles fragment_mask mixed_coverage depth_stencil cast_cache)
        string(REPLACE "_" "-" name "${fixture}")
        add_executable(snx-test-shader-${name} "${PROJECT_SOURCE_DIR}/tests/shader_${fixture}.c")
        snx_test_target(snx-test-shader-${name} 11)
        target_link_libraries(snx-test-shader-${name} PRIVATE snx_shader)
        if(NOT WIN32)
            target_link_libraries(snx-test-shader-${name} PRIVATE Threads::Threads m)
        endif()
        add_test(NAME shader_${fixture} COMMAND $<TARGET_FILE:snx-test-shader-${name}>)
    endforeach()
    add_executable(snx-test-shader-shared-reach "${PROJECT_SOURCE_DIR}/tests/shader_shared_reach.c")
    snx_test_target(snx-test-shader-shared-reach 11)
    target_link_libraries(snx-test-shader-shared-reach PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-shared-reach PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_shared_reach COMMAND $<TARGET_FILE:snx-test-shader-shared-reach>)
    add_test(NAME runtime_settings_list COMMAND "${CMAKE_COMMAND}" "-DSNX_TRACY=${SNX_TRACY}"
             -DRUNTIME=$<TARGET_FILE:scarletnx>
             -DAPPLE_HOST=$<BOOL:${APPLE}> -P "${PROJECT_SOURCE_DIR}/tests/settings_list.cmake")
    add_test(NAME runtime_unknown_option COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
             -P "${PROJECT_SOURCE_DIR}/tests/unknown_option.cmake")
    add_test(NAME runtime_tracy_version COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
             "-DTRACY_VERSION=${SNX_TRACY_VERSION}" -P "${PROJECT_SOURCE_DIR}/tests/tracy_version.cmake")
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
    # the SM86 translator on the device against the CPU reference (tests/support/sm86_ref.c), whose rounding modes
    # come from fesetround: -frounding-math keeps the compiler from folding them away
    add_executable(snx-test-shader-sm86-gpu "${PROJECT_SOURCE_DIR}/tests/shader_sm86_gpu.c"
        "${PROJECT_SOURCE_DIR}/tests/support/sm86_ref.c" "${volk_SOURCE_DIR}/volk.c")
    target_include_directories(snx-test-shader-sm86-gpu PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(snx-test-shader-sm86-gpu PRIVATE snx_nx_flags snx_shader ${CMAKE_DL_LIBS})
    set_source_files_properties("${PROJECT_SOURCE_DIR}/tests/support/sm86_ref.c" PROPERTIES COMPILE_OPTIONS
                                "-frounding-math")
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-sm86-gpu PRIVATE m)
    endif()
    add_test(NAME shader_sm86_gpu COMMAND snx-test-shader-sm86-gpu)
    set_tests_properties(shader_sm86_gpu PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # the BC7 encoder run on the CPU (tests/glsl/bc7_cpu.cpp): what the encoder's blocks between frames are held to
    # (tests/vulkan_astc_later.inc)
    add_executable(snx-test-vulkan "${PROJECT_SOURCE_DIR}/tests/vulkan_headless.c"
        "${PROJECT_SOURCE_DIR}/tests/spirv_emitter_fixture.c" "${PROJECT_SOURCE_DIR}/tests/support/nvdec_ref.c"
        "${PROJECT_SOURCE_DIR}/tests/support/vic_ref.c" "${PROJECT_SOURCE_DIR}/tests/support/dma_ref.c"
        "${PROJECT_SOURCE_DIR}/tests/support/fermi2d_ref.c" "${PROJECT_SOURCE_DIR}/tests/support/half.c"
        "${PROJECT_SOURCE_DIR}/tests/support/sm86_ref.c" "${PROJECT_SOURCE_DIR}/tests/glsl/bc7_cpu.cpp"
        "${PROJECT_SOURCE_DIR}/tests/glsl/astc_bc7_cpu.cpp"
        "${PROJECT_SOURCE_DIR}/tests/support/ui_shape_ref.c")
    target_include_directories(snx-test-vulkan PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(snx-test-vulkan PRIVATE snx_nx_flags ${components} snx_driver FFmpeg::avutil Threads::Threads
                                                  ${CMAKE_DL_LIBS})
    target_include_directories(snx-test-vulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/hookgen/snx_gpu"
                                                       "${PROJECT_SOURCE_DIR}/tests/glsl")
    set_target_properties(snx-test-vulkan PROPERTIES LINKER_LANGUAGE CXX)
    if(WIN32)
        target_link_libraries(snx-test-vulkan PRIVATE user32)
    else()
        target_link_libraries(snx-test-vulkan PRIVATE m)
    endif()
    target_link_libraries(snx-test-vulkan PRIVATE SDL3::SDL3)
    if(SNX_JEMALLOC AND NOT ANDROID)
        # the renderer reads the heap jemalloc's mallctl keeps (common/memory/jemalloc.c)
        target_link_libraries(snx-test-vulkan PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,snx_jemalloc>")
    endif()
    add_test(NAME vulkan_headless COMMAND snx-test-vulkan -H)
    # an Ounce game's mesh draws, which the experimental mesh_draws setting turns on (check_mesh_draws)
    add_test(NAME vulkan_headless_mesh COMMAND snx-test-vulkan -H --mesh_draws=1)
    set_tests_properties(vulkan_headless_mesh PROPERTIES TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # a save state of a game whose NVN the runtime runs, saved and loaded through state.c: the frame before the save
    # comes back after the load (tests/vulkan_nvn_savestate.inc). Alone, as the load replaces the guest memory. With
    # new memory filled (jemalloc's junk, under either name of its options' variable, and glibc's MALLOC_PERTURB_):
    # memory a load reads that nothing wrote, as the buffer cache's entry 0, isn't zero by chance
    add_test(NAME vulkan_nvn_savestate COMMAND snx-test-vulkan -H)
    set_tests_properties(vulkan_nvn_savestate PROPERTIES TIMEOUT 60
        ENVIRONMENT "SNX_TEST_NVN_CASE=savestate;MALLOC_CONF=junk:true;JE_MALLOC_CONF=junk:true;MALLOC_PERTURB_=90"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    add_test(NAME vulkan_storage_lookup COMMAND snx-test-vulkan -H --vk_memory=1 --bench-storage-lookup)
    set_tests_properties(vulkan_storage_lookup PROPERTIES TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    if(SNX_TEST_AV1_VECTORS)
        # the native video service's AV1 decoder on AOMedia's test vectors, each picture against the MD5 its list gives
        # (tests/vulkan_video_av1.inc): with Vulkan Video where the device decodes AV1, on the CPU elsewhere, and a
        # monochrome vector that Vulkan Video refuses on any device. The vectors and their lists come from libaom's
        # test data, downloaded once into CPM_SOURCE_CACHE: a vector's name, then the SHA-256 of the vector and of its
        # list. Off by default, as the VP9 vectors are (tests/CMakeLists.txt)
        set(snx_av1_vectors
            av1-1-b8-01-size-66x66.ivf 5f9285cbc417b0457c806df9154a1898f0f9ca09066abfdfd828cb3b2ba0371b
                c9e5553ef735c82723595ce9317d55e54e0a8c916902960e65a644090fbf5653
            av1-1-b8-05-mv.ivf 222a9050059b254dab17cfb802ff829c778e3f93af18961a622c8268576c1395
                76eeebe50c438619b4cadce90fc194010c50c4a5a876441444d726d12de58c50
            av1-1-b8-23-film_grain-50.ivf ba3edd82a58414f009e1c821b947ec8de4e0420f33803ca7bfea39af6aeea155
                8996cc98e68102e60620d28208246009601b2906f05e1d1cc0da74479c828ac9
            av1-1-b8-24-monochrome.ivf 3dcc2a3c181f74788bd9575d94f0862d9e914415ac358a4da4ed7d904f0535b7
                01658b28fc7b6760de63602b472c3732af5debef53059b0a3b6f237437afe652)
        set(snx_av1_paths "")
        while(snx_av1_vectors)
            list(POP_FRONT snx_av1_vectors name vector_sha256 list_sha256)
            string(REGEX REPLACE "^av1-1-|\\.ivf$" "" short "${name}")
            foreach(file "${name}" "${name}.md5")
                if(file STREQUAL name)
                    set(sha256 ${vector_sha256})
                    set(package snx_av1_${short})
                else()
                    set(sha256 ${list_sha256})
                    set(package snx_av1_${short}_md5)
                endif()
                CPMAddPackage(NAME ${package}
                              URL https://storage.googleapis.com/aom-test-data/${file}
                              URL_HASH SHA256=${sha256}
                              DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
                # checked on every configure, as a package of one file is (cmake/cpm_cache.cmake)
                snx_cpm_file_check("${${package}_SOURCE_DIR}" "${file}" ${sha256})
                string(APPEND snx_av1_paths "${${package}_SOURCE_DIR}/${file}\n")
            endforeach()
        endwhile()
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/vulkan_video_av1.txt" "${snx_av1_paths}")
        add_test(NAME vulkan_video_av1 COMMAND snx-test-vulkan -H)
        set_tests_properties(vulkan_video_av1 PROPERTIES TIMEOUT 60
            ENVIRONMENT "SNX_TEST_AV1_VECTORS=${CMAKE_CURRENT_BINARY_DIR}/vulkan_video_av1.txt"
            FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    endif()
    # 120 as its variants: the aarch64 runner's lavapipe takes 55 to 58 s for it
    set_tests_properties(vulkan_headless PROPERTIES TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the recording thread's own vk_rec_sync must return: the fatal path of a lost device calls it from that thread, and
    # a wait there waits for the thread that runs it (record.c); and the window's release for the error's screen must
    # return while that thread is stopped in a packet. These need no shaders, so they run on any driver, and a run
    # that hangs fails at the timeout
    add_test(NAME vulkan_recorder COMMAND snx-test-vulkan -H)
    set_tests_properties(vulkan_recorder PROPERTIES TIMEOUT 30 ENVIRONMENT "SNX_TEST_RECORDER_ONLY=1"
        PASS_REGULAR_EXPRESSION "PASS headless Vulkan recorder only")
    # the writable storage buffers' check (tests/vulkan_storage_buffer_keep.inc) with guest buffers copied: the default
    # run takes unified memory on an integrated device and on lavapipe, which writes them in guest memory instead of
    # keeping them on the GPU. A run that hangs fails at the timeout
    add_test(NAME vulkan_storage_buffer_keep COMMAND snx-test-vulkan -H --vk_memory=1)
    set_tests_properties(vulkan_storage_buffer_keep PROPERTIES TIMEOUT 60 ENVIRONMENT "SNX_TEST_BUFFER_KEEP_ONLY=1"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # guest memory read with the renderer's lock held on pages the GPU owns bytes of (tests/vulkan_owned_page_reads.inc),
    # with guest buffers copied, as a discrete GPU and Android without host imports have them: the default run takes
    # unified memory on lavapipe, whose reads take other paths. Traps by userfaultfd, and by page protection as Android's
    add_test(NAME vulkan_owned_page_reads COMMAND snx-test-vulkan -H --vk_memory=1)
    add_test(NAME vulkan_owned_page_reads_protection COMMAND snx-test-vulkan -H --vk_memory=1 --memory_traps=1)
    set_tests_properties(vulkan_owned_page_reads vulkan_owned_page_reads_protection PROPERTIES TIMEOUT 60
        ENVIRONMENT "SNX_TEST_NVN_CASE=owned_page_reads" SKIP_REGULAR_EXPRESSION "SKIP owned page reads"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the targets a release watches and the CPU's first read lands, color and depth (check_rt_watch, check_zt_watch),
    # with guest memory copied as on a discrete GPU: on unified memory they are skipped, as lavapipe's default run takes it
    add_test(NAME vulkan_rt_watch COMMAND snx-test-vulkan -H --vk_memory=1)
    set_tests_properties(vulkan_rt_watch PROPERTIES TIMEOUT 60 ENVIRONMENT "SNX_TEST_RT_WATCH_ONLY=1"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR;watch: needs traps")
    # the budget's trim alone (tests/vulkan_trim.inc): a trim that wants more than everything held would take what the
    # other checks hold
    add_test(NAME vulkan_trim COMMAND snx-test-vulkan -H)
    set_tests_properties(vulkan_trim PROPERTIES TIMEOUT 30 ENVIRONMENT "SNX_TEST_TRIM_ONLY=1"
        PASS_REGULAR_EXPRESSION "PASS headless Vulkan trim only" FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with graphics descriptors in descriptor buffers (gpu/vk/descriptors.c, vk_descriptors_mode),
    # also with guest memory read in place, under the validation layer when it is installed. A device without
    # VK_EXT_descriptor_buffer skips them (77). The layer reports errors only: the targetless-stores check draws with
    # a fragment output and no color target, as the console does, which the layer warns about with sets too
    add_test(NAME vulkan_headless_descriptor_buffers
        COMMAND snx-test-vulkan -H --vk_descriptors_mode=1 --vk_validation=1)
    add_test(NAME vulkan_headless_descriptor_buffers_uma
        COMMAND snx-test-vulkan -H --vk_descriptors_mode=1 --vk_memory=2 --vk_validation=1)
    set_tests_properties(vulkan_headless_descriptor_buffers vulkan_headless_descriptor_buffers_uma PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77 ENVIRONMENT "VK_KHRONOS_VALIDATION_REPORT_FLAGS=error"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with graphics descriptors in heaps (vk_descriptors_mode=2), which the default, Automatic, takes
    # only on a GPU that shares the system's memory: a discrete GPU runs the default with sets. A device without
    # VK_EXT_descriptor_heap skips it, as the buffers' tests do without VK_EXT_descriptor_buffer
    add_test(NAME vulkan_headless_descriptor_heaps COMMAND snx-test-vulkan -H --vk_descriptors_mode=2)
    set_tests_properties(vulkan_headless_descriptor_heaps PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again the Vulkan 1.2 way on a newer device, as a game's .ini with vk_api=1.2 asks (gpu/vk/device.c
    # vk12_extensions): the 1.3 commands by their extensions' names, shaders in SPIR-V 1.5, under the validation layer
    # when it is installed, which takes the instance's 1.2 as the version and flags any 1.3 use left
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/vulkan_headless_vk12.ini" "[Debug]\nvk_api=1.2\n")
    add_test(NAME vulkan_headless_vk12 COMMAND snx-test-vulkan -H
        "--settings-ini=${CMAKE_CURRENT_BINARY_DIR}/vulkan_headless_vk12.ini" --vk_validation=1)
    set_tests_properties(vulkan_headless_vk12 PROPERTIES TIMEOUT 120
        ENVIRONMENT "VK_KHRONOS_VALIDATION_REPORT_FLAGS=error" FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with each vertex layout in its pipeline: the default run sets it as dynamic state where the
    # device has VK_EXT_vertex_input_dynamic_state (gpu/vk/vertex_input.c), and both find each draw's own layout
    add_test(NAME vulkan_headless_vertex_input_off COMMAND snx-test-vulkan -H --vertex_input_dynamic_state=off)
    # every check again under the validation layer with graphics pipeline libraries (pipeline_libraries=1, where the
    # device has them) and without them: the parts, their layouts and their dynamic state checked by the layer
    add_test(NAME vulkan_headless_libraries COMMAND snx-test-vulkan -H --pipeline_libraries=1 --vk_validation=1)
    add_test(NAME vulkan_headless_libraries_off COMMAND snx-test-vulkan -H --pipeline_libraries=0 --vk_validation=1)
    set_tests_properties(vulkan_headless_libraries vulkan_headless_libraries_off PROPERTIES TIMEOUT 120
        ENVIRONMENT "VK_KHRONOS_VALIDATION_REPORT_FLAGS=error" FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    set_tests_properties(vulkan_headless_vertex_input_off PROPERTIES TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with each draw binding the pipeline linked from its parts, never the one made whole, under the
    # validation layer when it is installed (gpu/vk/pipeline.c, VK_EXT_graphics_pipeline_library; a device that
    # doesn't link them skips, 77); and with each pipeline made whole, with the dynamic state the libraries gave
    add_test(NAME vulkan_headless_pipeline_library_linked COMMAND snx-test-vulkan -H --vk_validation=1)
    add_test(NAME vulkan_headless_pipeline_library_whole COMMAND snx-test-vulkan -H)
    set_tests_properties(vulkan_headless_pipeline_library_linked PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        ENVIRONMENT "SNX_TEST_GPL=linked;VK_KHRONOS_VALIDATION_REPORT_FLAGS=error"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    set_tests_properties(vulkan_headless_pipeline_library_whole PROPERTIES TIMEOUT 120 ENVIRONMENT "SNX_TEST_GPL=whole"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the on-screen layer's pass (gpu/vk/ui.c, tests/vulkan_ui.inc) under the validation layer when it is installed:
    # quads as instances, sampled in texels, and the device's buffer given only the quads that changed
    add_test(NAME vulkan_ui COMMAND snx-test-vulkan -H --vk_validation=1)
    set_tests_properties(vulkan_ui PROPERTIES TIMEOUT 60
        ENVIRONMENT "SNX_TEST_UI_ONLY=1;VK_KHRONOS_VALIDATION_REPORT_FLAGS=error"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the texture arrays of 32 elements a device that binds 128 samplers a stage gets (PanVK on Bifrost;
    # gpu/vk/texture_arrays.h), under the validation layer: their layouts, pools and the draws that index them
    add_test(NAME vulkan_texture_arrays COMMAND snx-test-vulkan -H --vk_validation=1)
    set_tests_properties(vulkan_texture_arrays PROPERTIES TIMEOUT 60
        ENVIRONMENT "SNX_TEST_TEX_ARRAY=32;VK_KHRONOS_VALIDATION_REPORT_FLAGS=error"
        PASS_REGULAR_EXPRESSION "PASS headless Vulkan texture arrays of 32"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with guest memory read in place where the device can import it (gpu/vk/uma.c)
    add_test(NAME vulkan_headless_uma COMMAND snx-test-vulkan -H --vk_memory=2)
    set_tests_properties(vulkan_headless_uma PROPERTIES TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with the guest memory that draws read kept by page in sparse buffers, and what draws write to
    # guest memory kept there too (gpu/vk/pagespace.c). A device without sparse buffers or write tracking runs the
    # checks as before
    add_test(NAME vulkan_headless_pages COMMAND snx-test-vulkan -H --vk_buffer_pages=2 --vk_memory=1)
    set_tests_properties(vulkan_headless_pages PROPERTIES TIMEOUT 180
        FAIL_REGULAR_EXPRESSION "failed: VK_ERROR")
    # and with guest memory kept by the range each draw asks for, as on a device without sparse buffers
    add_test(NAME vulkan_headless_ranges COMMAND snx-test-vulkan -H --vk_buffer_pages=0 --vk_memory=1)
    set_tests_properties(vulkan_headless_ranges PROPERTIES TIMEOUT 180
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with the rings that the CPU fills in cached host memory (vk_stage_memory, cmd.c's chunk_new),
    # which is the default only on Intel's Mesa driver
    add_test(NAME vulkan_headless_stage_cached COMMAND snx-test-vulkan -H --vk_stage_memory=2)
    set_tests_properties(vulkan_headless_stage_cached PROPERTIES TIMEOUT 180
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # every check again with the write tracking by page protection and its hot pages (write_tracking_hot): each render
    # target's baseline cools its pages, or its write-back keeps the CPU's old bytes over its pixels
    add_test(NAME vulkan_headless_hot COMMAND snx-test-vulkan -H --write_tracking=1 --write_tracking_hot=1)
    set_tests_properties(vulkan_headless_hot PROPERTIES TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the checks of guest memory read in place again with the write tracking by page protection: the driver takes an
    # import's pages for writing, and amdgpu refuses those a scan left protected (VK_ERROR_INVALID_EXTERNAL_HANDLE)
    add_test(NAME vulkan_uma_hot COMMAND snx-test-vulkan -H --vk_memory=2 --write_tracking=1 --write_tracking_hot=1)
    set_tests_properties(vulkan_uma_hot PROPERTIES TIMEOUT 60 ENVIRONMENT "SNX_TEST_UMA_ONLY=1"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the write tracking's contract alone (tests/vulkan_write_tracking.inc), with each way the host tracks: its own
    # (userfaultfd on Linux), page protection and the fault handler for the tracking and the traps (macOS, FreeBSD,
    # Android, older Linux), with its hot pages (write_tracking_hot), unified memory's reads in place, and none (the
    # caches compare contents: the cases that need the tracking skip, the others check the comparisons)
    add_test(NAME vulkan_write_tracking COMMAND snx-test-vulkan -H)
    add_test(NAME vulkan_write_tracking_protection COMMAND snx-test-vulkan -H --write_tracking=1 --memory_traps=1)
    add_test(NAME vulkan_write_tracking_hot
        COMMAND snx-test-vulkan -H --write_tracking=1 --memory_traps=1 --write_tracking_hot=1)
    add_test(NAME vulkan_write_tracking_uma COMMAND snx-test-vulkan -H --vk_memory=2)
    add_test(NAME vulkan_write_tracking_off COMMAND snx-test-vulkan -H --write_tracking=2)
    set_tests_properties(vulkan_write_tracking vulkan_write_tracking_protection vulkan_write_tracking_hot
        vulkan_write_tracking_uma vulkan_write_tracking_off PROPERTIES
        TIMEOUT 120 ENVIRONMENT "SNX_TEST_WW_ONLY=1" FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # a texture's changes loaded a block at a time (tests/vulkan_tex_partial.inc), apart from vulkan_headless: with
    # each way the host tracks writes, and without write tracking, where they load the whole texture. The ASTC cases,
    # which lavapipe decodes and encodes as BC7 in software, run with the host's own tracking, as long as astc_vulkan
    add_test(NAME vulkan_tex_partial COMMAND snx-test-vulkan -H)
    add_test(NAME vulkan_tex_partial_protection COMMAND snx-test-vulkan -H --write_tracking=1 --memory_traps=1)
    add_test(NAME vulkan_tex_partial_untracked COMMAND snx-test-vulkan -H --write_tracking=2)
    set_tests_properties(vulkan_tex_partial PROPERTIES TIMEOUT 300 ENVIRONMENT "SNX_TEST_TEX_PARTIAL_ONLY=1"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    set_tests_properties(vulkan_tex_partial_protection vulkan_tex_partial_untracked PROPERTIES
        TIMEOUT 120 ENVIRONMENT "SNX_TEST_TEX_PARTIAL_ONLY=no-astc"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # an argument the test doesn't take stops it, named, before any device is made; the setting before it, as
    # the runtime takes it, goes through
    add_test(NAME vulkan_headless_options COMMAND snx-test-vulkan -H --memory_traps=2 --memory_trap=2)
    set_tests_properties(vulkan_headless_options PROPERTIES TIMEOUT 10
        PASS_REGULAR_EXPRESSION "--memory_trap=2 is not an option it takes")
    # VIC only, under the validation layer when it is installed: its job binds the same buffer as a uniform and a
    # storage buffer, which the layer checks against the buffer's usage flags (VUID-VkWriteDescriptorSet-
    # descriptorType-00330). Without the layer this is VIC's own checks again.
    add_test(NAME vulkan_headless_vic_validation COMMAND snx-test-vulkan -H --vk_validation=1)
    set_tests_properties(vulkan_headless_vic_validation PROPERTIES TIMEOUT 60
        ENVIRONMENT "SNX_TEST_VIC_ONLY=1"
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
    # the ASTC upload against the CPU decoder, block linear moves on the GPU against bl_offset's layout and texture
    # conversions (the blits between pixel formats too) against the CPU's, with the
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
        "${PROJECT_SOURCE_DIR}/tests/support/astc.c" gpu/vk/astc.c gpu/vk/bc7_cache.c
        gpu/vk/blocklinear.c gpu/texture/blocklinear.c gpu/vk/texconv.c "${PROJECT_SOURCE_DIR}/tests/support/etc2.c"
        "${PROJECT_SOURCE_DIR}/tests/support/bcn.c"
        gpu/vk/cmd.c gpu/vk/descriptors.c gpu/vk/descriptor_heaps.c gpu/vk/descriptor_objects.c
        gpu/vk/memory.c gpu/vk/ledger.c gpu/vk/resource_stats.c gpu/vk/record.c gpu/vk/checkpoints.c gpu/vk/devmem.c gpu/vk/space.c ${snx_astc_plat} "${volk_SOURCE_DIR}/volk.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_spv.c" "${PROJECT_BINARY_DIR}/generated/spirv/blocklinear_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/texconv_spv.c" "${PROJECT_BINARY_DIR}/generated/spirv/texconv_etc2_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/texconv_convert_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/texconv_nv12_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/texconv_astc_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/texconv_astc_b_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/texconv_astc_c_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_a_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_b_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_fast_a_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_fast_b_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_mode6_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_quick_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_a_dot_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_b_dot_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_fast_a_dot_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_fast_b_dot_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_mode6_dot_spv.c"
        "${PROJECT_BINARY_DIR}/generated/spirv/astc_bc7_quick_dot_spv.c"
        "${PROJECT_SOURCE_DIR}/tests/glsl/bc7_cpu.cpp" "${PROJECT_SOURCE_DIR}/tests/glsl/astc_bc7_cpu.cpp"
        "${PROJECT_SOURCE_DIR}/tests/glsl/bcn_etc2_cpu.cpp" "${PROJECT_SOURCE_DIR}/tests/glsl/bcn_astc_cpu.cpp"
        "${PROJECT_SOURCE_DIR}/tests/glsl/blit_convert_cpu.cpp")
    target_include_directories(snx-test-astc-vulkan PRIVATE "${PROJECT_SOURCE_DIR}/tests/glsl")
    target_link_libraries(snx-test-astc-vulkan PRIVATE snx_nx_flags scarletnx-filesystem snx_xxhash snx_vk_common
                                                      Threads::Threads SDL3::SDL3 ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(snx-test-astc-vulkan PRIVATE powrprof)
    endif()
    if(APPLE AND NOT IOS) # plat_posix.c's power source: IOKit's notifications on CoreFoundation's run loop
        target_link_libraries(snx-test-astc-vulkan PRIVATE "-framework IOKit" "-framework CoreFoundation")
    endif()
    set_target_properties(snx-test-astc-vulkan PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME astc_vulkan COMMAND snx-test-astc-vulkan)
    add_test(NAME astc_vulkan_pieces COMMAND snx-test-astc-vulkan --vk_max_allocation_mb=4)
    add_test(NAME astc_vulkan_upload_wait COMMAND snx-test-astc-vulkan --upload-wait)
    add_test(NAME astc_vulkan_upload_retire COMMAND snx-test-astc-vulkan --upload-retire)
    set_tests_properties(astc_vulkan_upload_wait astc_vulkan_upload_retire PROPERTIES SKIP_RETURN_CODE 77)
    # pieces, and then the line the program prints only once every comparison passed: a pass expression alone
    # passes the test whatever the exit code
    set_tests_properties(astc_vulkan_pieces PROPERTIES PASS_REGULAR_EXPRESSION
                         "memory in pieces.*All ASTC Vulkan comparisons passed")
    set_tests_properties(astc_vulkan astc_vulkan_pieces astc_vulkan_upload_wait astc_vulkan_upload_retire
        PROPERTIES TIMEOUT 300)
endif()
if(BUILD_TESTING)
    snx_sanitizer_path() # tests/CMakeLists.txt: this folder's tests
endif()
