# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
option(SNX_TEST_SANITIZERS
       "Build the native regression tests, and the in-tree libraries they exercise, with ASan and UBSan (a test build: \
the runtime links those libraries too, and isn't deployed)" OFF)
option(SNX_TEST_VULKAN "Run offscreen Vulkan integration tests (requires a Vulkan 1.3 ICD, e.g. lavapipe)" OFF)
option(SNX_TEST_ARM_AOT "Build asset-free static ARM11/checked-memory witnesses (requires native LLVM SDK)" OFF)
option(SNX_TEST_VP9_VECTORS
       "Run NVDEC's VP9 jobs on libvpx's test vectors (configure downloads 32 files from the WebM project)" OFF)
if(SNX_TEST_VP9_VECTORS AND SNX_TOOLS_ONLY)
    message(FATAL_ERROR "SNX_TEST_VP9_VECTORS=ON needs the runtime's FFmpeg, which SNX_TOOLS_ONLY=ON leaves out")
endif()
# the in-tree libraries the tests exercise and the programs they run: an overflow in them shows only in code built
# with the sanitizers. src/nx doesn't deploy a sanitized runtime. A tool made by a host build (cross-compiling) is
# that build's
function(snx_test_sanitize)
    if(NOT SNX_TEST_SANITIZERS)
        return()
    endif()
    foreach(target ${ARGN})
        if(TARGET ${target})
            get_target_property(imported ${target} IMPORTED)
            if(imported)
                continue()
            endif()
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "OBJECT_LIBRARY")
                target_link_options(${target} INTERFACE -fsanitize=address,undefined) # the program that links it
            else()
                target_link_options(${target} PUBLIC -fsanitize=address,undefined) # its own link too
            endif()
        endif()
    endforeach()
endfunction()
# once every folder has made its targets: most come after this one, and which folder makes a shared one (the window
# icon, the code generators) depends on the build. runtime_architecture (tests/architecture.py --sanitizers) names
# what the tests reach that this list misses
if(SNX_TEST_SANITIZERS)
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL snx_test_sanitize
        # the common code: network, filesystems, LZ4 and xxHash, the Vulkan setup the frontends share
        scarletnx-common scarletnx-net scarletnx-ldn scarletnx-filesystem snx_demangle snx_lz4
        snx_xxhash snx_vk_common
        snx-logscan
        # the recompiler and its backends
        snx_llvm snx_arm_llvm scarletnx-recompiler scarletnx-recompiler-shared
        # the driver, its extraction, the first start's setup, the settings and the build's line
        snx_driver snx_nx_module snx_driver_llvm hactool_lib snx_setup snx_settings snx_settings_runtime snx_build_info
        snx-test-build-info
        # the Switch runtime: its components, the shader translator and the Vulkan backend
        scarletnx snx_core snx_kernel snx_hle snx_video snx_gpu snx_platform snx_shader snx_vk
        # the GNU ABI runtime
        scarletnx-gnuabi
        # the 3DS runtime
        scarletnx-3ds snx_ctr_program snx_ctr_board snx_ctr_memory snx_ctr_loader snx_ctr_cpu snx_ctr_process snx_ctr_native
        snx_ctr_audio snx_ctr_gpu snx_ctr_gpu_reference
        # the Vita runtime, its loader and USSE translator
        scarletnx-vita snx_vita_program snx_vita_loader snx_vita_usse snx-vita-usse
        # the GBA board and its runner
        scarletnx-agb snx_agb_program
        # the Switch driver's entry, which scarletnx runs as it runs the others
        snx_switch_program
        # the menu, its splash, icon and pads
        scarletnx-gui snx_gui_support snx_gui_ui snx_i18n snx_gui_languages snx_gui_languages_test snx_splash snx-test-splash
        snx_window_icon snx-test-window-icon snx_pad_sdl snx_gui_pad
        # the code generators, which tests run on files they refuse
        snx-swipc snx-svcgen snx-shaderdec snx-settingsgen snx-uigen snx-a64dec)
endif()
# one Python for the scripted tests: without it they are left out
find_package(Python3 QUIET COMPONENTS Interpreter)
# CLAUDE.md and .claude/ in ASD-STE100 (.claude/skills/ste100), and the checker's own test
if(Python3_Interpreter_FOUND)
    add_test(NAME claude_ste100 COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/.claude/skills/ste100/check.py" --all)
    if(UNIX)
        add_test(NAME claude_ste100_checker COMMAND sh "${PROJECT_SOURCE_DIR}/.claude/skills/ste100/test/test-check.sh")
    endif()
endif()
# x86-64, by any of the names hosts give it
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(snx_test_x86 ON)
else()
    set(snx_test_x86 OFF)
endif()

# On Windows a sanitized test needs the sanitizer runtime's DLLs on PATH. Each folder that adds tests (this one, the
# top level, src/nx, src/gnuabi, src/gui) calls this for the ones it has added (the others don't mind the DLLs)
function(snx_sanitizer_path)
    if(NOT (SNX_TEST_SANITIZERS AND WIN32))
        return()
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    get_property(tests DIRECTORY PROPERTY TESTS)
    if(tests)
        set_tests_properties(${tests} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${resource}/lib/windows")
    endif()
endfunction()
# cpu_helpers.c built as the game's helpers are (snx_rtlib_flags in src/common/cpu/recompiler/llvm/CMakeLists.txt),
# with -mcx16 on x86-64: its 16-byte compare-and-swap is then lock cmpxchg16b, not a libatomic call
function(snx_cpu_helpers_flags target)
    if(snx_test_x86)
        target_compile_options(${target} PRIVATE -mcx16)
    endif()
endfunction()
# a service's dispatchers, generated from types.swipc and the interfaces named (paths under src/nx). A handler takes
# every argument of its command, used or not, so -Wextra's unused-parameter warning is off
function(snx_service_swipc target)
    target_compile_options(${target} PRIVATE -Wno-unused-parameter)
    snx_swipc(${target} ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc ${ARGN})
endfunction()

# a test's checks are asserts too: -UNDEBUG keeps them in any build type
function(snx_test_target target standard)
    set(strict OFF)
    if(standard EQUAL 99) # strict ISO C99, without extensions
        set(strict ON)
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD ${standard} C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    if(strict)
        set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
        target_compile_options(${target} PRIVATE -pedantic-errors)
    else()
        set_target_properties(${target} PROPERTIES C_EXTENSIONS ON)
    endif()
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(TARGET snx_settings) # g_settings, its header, and the tables a test uses (cmake/settings.cmake)
        target_link_libraries(${target} PRIVATE snx_settings_runtime snx_settings)
    endif()
    snx_test_sanitize(${target})
endfunction()

if(TARGET snx_ctr_board)
    add_executable(snx-test-3ds-board 3ds_board.c)
    snx_test_target(snx-test-3ds-board 11)
    target_link_libraries(snx-test-3ds-board PRIVATE snx_ctr_board)
    add_test(NAME 3ds_board COMMAND snx-test-3ds-board)
    add_executable(snx-test-3ds-memory 3ds_memory.c)
    snx_test_target(snx-test-3ds-memory 11)
        target_link_libraries(snx-test-3ds-memory PRIVATE snx_ctr_memory)
    add_test(NAME 3ds_memory COMMAND snx-test-3ds-memory)
    add_executable(snx-test-3ds-memory-track 3ds_memory_track.c)
    snx_test_target(snx-test-3ds-memory-track 11)
    target_link_libraries(snx-test-3ds-memory-track PRIVATE snx_ctr_memory)
    add_test(NAME 3ds_memory_track COMMAND snx-test-3ds-memory-track)
    add_executable(snx-test-3ds-kernel 3ds_kernel.c)
    snx_test_target(snx-test-3ds-kernel 11)
    target_link_libraries(snx-test-3ds-kernel PRIVATE snx_ctr_process)
    add_test(NAME 3ds_kernel COMMAND snx-test-3ds-kernel)
    foreach(test services threads)
        add_executable(snx-test-3ds-${test} 3ds_${test}.c)
        snx_test_target(snx-test-3ds-${test} 11)
        target_link_libraries(snx-test-3ds-${test} PRIVATE snx_ctr_process)
        add_test(NAME 3ds_${test} COMMAND snx-test-3ds-${test})
    endforeach()
    add_executable(snx-test-3ds-ipc 3ds_ipc.c)
    snx_test_target(snx-test-3ds-ipc 11)
    target_link_libraries(snx-test-3ds-ipc PRIVATE snx_ctr_process)
    add_test(NAME 3ds_ipc COMMAND snx-test-3ds-ipc)
    add_executable(snx-test-3ds-hid 3ds_hid.c)
    snx_test_target(snx-test-3ds-hid 11)
    target_link_libraries(snx-test-3ds-hid PRIVATE snx_ctr_process)
    add_test(NAME 3ds_hid COMMAND snx-test-3ds-hid)
    add_executable(snx-test-ctr-swipc ctr_swipc.c)
    if(NOT WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        snx_test_target(snx-test-ctr-swipc 99)
            else()
        snx_test_target(snx-test-ctr-swipc 11)
    endif()
    snx_swipc(snx-test-ctr-swipc CTR ROOT "${PROJECT_SOURCE_DIR}" FILES tests/ctr_swipc.swipc)
    add_test(NAME ctr_swipc_dispatch COMMAND snx-test-ctr-swipc)
    add_executable(snx-test-3ds-process 3ds_process.c)
    snx_test_target(snx-test-3ds-process 11)
    target_link_libraries(snx-test-3ds-process PRIVATE snx_ctr_process)
    add_test(NAME 3ds_process COMMAND snx-test-3ds-process)
    add_executable(snx-test-3ds-discover 3ds_discover.c "${PROJECT_SOURCE_DIR}/src/3ds/aot/discover.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c" "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/thumb_decode.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/vector_decode.c" "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/profile.c")
    snx_test_target(snx-test-3ds-discover 11)
    add_test(NAME 3ds_static_discovery COMMAND snx-test-3ds-discover)
    add_executable(snx-test-3ds-native 3ds_native.c)
    snx_test_target(snx-test-3ds-native 11)
    target_link_libraries(snx-test-3ds-native PRIVATE snx_ctr_native)
    add_executable(snx-test-3ds-blz 3ds_blz.c)
    snx_test_target(snx-test-3ds-blz 11)
    target_link_libraries(snx-test-3ds-blz PRIVATE snx_ctr_loader)
    add_test(NAME 3ds_blz COMMAND snx-test-3ds-blz)

    # The GPU, presentation and DSP tests need neither Python nor POSIX, and Windows is a 3DS target too
    # the GPU's transfer engine against the CPU reference (gpu/transfer.c), on whatever Vulkan device there is
    add_executable(snx-test-3ds-vk-transfer 3ds_vk_transfer.c)
    snx_test_target(snx-test-3ds-vk-transfer 11)
    target_link_libraries(snx-test-3ds-vk-transfer PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_transfer COMMAND snx-test-3ds-vk-transfer)
    set_tests_properties(3ds_vk_transfer PROPERTIES SKIP_RETURN_CODE 77)
    # the Y2R engine's conversion on the GPU against its CPU reference
    add_executable(snx-test-3ds-vk-y2r 3ds_vk_y2r.c)
    snx_test_target(snx-test-3ds-vk-y2r 11)
    target_link_libraries(snx-test-3ds-vk-y2r PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_y2r COMMAND snx-test-3ds-vk-y2r)
    set_tests_properties(3ds_vk_y2r PROPERTIES SKIP_RETURN_CODE 77)
    # the GPU's draws against the software renderer, the CPU reference they replaced, scene by scene
    add_executable(snx-test-3ds-vk-draw 3ds_vk_draw.c)
    snx_test_target(snx-test-3ds-vk-draw 11)
    target_link_libraries(snx-test-3ds-vk-draw PRIVATE snx_ctr_gpu_reference)
    add_test(NAME 3ds_vk_draw COMMAND snx-test-3ds-vk-draw)
    set_tests_properties(3ds_vk_draw PROPERTIES SKIP_RETURN_CODE 77)
    if(NOT WIN32 AND Python3_Interpreter_FOUND)
        # the window's clear and blits under Khronos validation, when the run has a window: the blits overwrite the
        # clear's pixels, so a missing barrier between them is a write-after-write hazard
        add_test(NAME 3ds_vk_draw_present_validation
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                $<TARGET_FILE:snx-test-3ds-vk-draw> --present)
        set_tests_properties(3ds_vk_draw_present_validation PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(snx-test-3ds-gsp 3ds_gsp.c)
    snx_test_target(snx-test-3ds-gsp 11)
    target_link_libraries(snx-test-3ds-gsp PRIVATE snx_ctr_process)
    add_test(NAME 3ds_gsp_gpu COMMAND snx-test-3ds-gsp "${CMAKE_CURRENT_BINARY_DIR}/gsp-frames")
    # No Vulkan loader/ICD: presentation cannot run here; the probe skips, never fakes a frame.
    set_tests_properties(3ds_gsp_gpu PROPERTIES SKIP_RETURN_CODE 77)
    # the transfer engine alone: fills, display transfers, texture copies against hand-written goldens
    add_executable(snx-test-3ds-transfer 3ds_transfer.c)
    snx_test_target(snx-test-3ds-transfer 11)
    target_link_libraries(snx-test-3ds-transfer PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_transfer COMMAND snx-test-3ds-transfer)
    # the PICA command processor: lists in FCRAM, uploads, interrupts, jumps, draw and immediate hooks
    add_executable(snx-test-3ds-pica 3ds_pica.c)
    snx_test_target(snx-test-3ds-pica 11)
    target_link_libraries(snx-test-3ds-pica PRIVATE snx_ctr_gpu_reference m)
    add_test(NAME 3ds_pica COMMAND snx-test-3ds-pica)
    # the HLE DSP and dsp::DSP: pipes, regions, sources, mixing, decoding, the service as ndsp drives it
    add_executable(snx-test-3ds-dsp 3ds_dsp.c)
    snx_test_target(snx-test-3ds-dsp 11)
    target_link_libraries(snx-test-3ds-dsp PRIVATE snx_ctr_process)
    add_test(NAME 3ds_dsp COMMAND snx-test-3ds-dsp)

    if(NOT WIN32)
        if(Python3_Interpreter_FOUND)
            add_executable(snx-test-3ds-fs 3ds_fs.c)
            snx_test_target(snx-test-3ds-fs 11)
            target_link_libraries(snx-test-3ds-fs PRIVATE snx_ctr_process)
            add_test(NAME 3ds_filesystem COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_fs.py" $<TARGET_FILE:snx-test-3ds-fs>)
            add_test(NAME ctr_swipc_validation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ctr_swipc.py"
                $<TARGET_FILE:snx-swipc>)
            add_test(NAME 3ds_driver COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_driver.py"
                "${SNX_CTR_PROGRAM}")
            # a CIA read where it lies (snx_ctr_cia_open) against its extraction, on the same fixtures and mutants
            add_executable(snx-test-3ds-container 3ds_container.c)
            snx_test_target(snx-test-3ds-container 11)
            target_link_libraries(snx-test-3ds-container PRIVATE snx_ctr_process)
            add_test(NAME 3ds_container COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_container.py"
                "${SNX_CTR_PROGRAM}" $<TARGET_FILE:snx-test-3ds-container>)
            # a cartridge image or a bare NCCH against its extraction, and the update partition played as the update
            add_test(NAME 3ds_cci COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_cci.py"
                "${SNX_CTR_PROGRAM}" $<TARGET_FILE:snx-test-3ds-container>)
            set(snx_ctr_extract_test_args "")

            set(snx_ctr_extract_test_args --aot)
            # TODO: fix this
            if (NOT APPLE)
                add_test(NAME 3ds_static_pipeline COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_aot.py"
                    "${SNX_CTR_PROGRAM}" $<TARGET_FILE:snx-test-3ds-native>)
                # about 20 s here, 4 minutes under a cross build's emulator
                if(CMAKE_CROSSCOMPILING_EMULATOR)
                    set_tests_properties(3ds_static_pipeline PROPERTIES TIMEOUT 900)
                else()
                    set_tests_properties(3ds_static_pipeline PROPERTIES TIMEOUT 180)
                endif()
            endif()
            add_test(NAME 3ds_decrypt COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_decrypt.py"
                "${SNX_CTR_PROGRAM}")
            set_tests_properties(3ds_decrypt PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
            add_test(NAME 3ds_extract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_extract.py"
                "${SNX_CTR_PROGRAM}" ${snx_ctr_extract_test_args})
            set_tests_properties(3ds_extract PROPERTIES TIMEOUT 120)
        endif()
    endif()
    # The pinned SuperTux gate runs on Windows too, the other Python tests not yet
    if(SNX_TEST_3DS_SUPERTUX AND Python3_Interpreter_FOUND)
        get_filename_component(snx_supertux_cia "${SNX_TEST_3DS_SUPERTUX}" ABSOLUTE)
        set(snx_supertux_oracle_args "")
        if(SNX_TEST_ARM_DIFF)
            set(snx_supertux_oracle_args --oracle)
        endif()
        add_test(NAME 3ds_supertux_bootstrap COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_supertux.py"
            "${SNX_CTR_PROGRAM}" "${snx_supertux_cia}" $<TARGET_FILE:snx-test-3ds-native> ${snx_supertux_oracle_args})
        set_tests_properties(3ds_supertux_bootstrap PROPERTIES TIMEOUT 1800 LABELS "external;bootstrap")
    endif()
endif()
# the 3DS tests that present through Vulkan, sanitized: the Vulkan driver stays loaded (vulkan_drivers_kept.c), as its
# loader unloads it with an allocation of its own still held, which LeakSanitizer would report from a library gone
if(SNX_TEST_SANITIZERS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(snx-test-vulkan-drivers-kept MODULE vulkan_drivers_kept.c)
    target_compile_options(snx-test-vulkan-drivers-kept PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(snx-test-vulkan-drivers-kept PRIVATE ${CMAKE_DL_LIBS})
    foreach(snx_test 3ds_gsp_gpu 3ds_static_pipeline 3ds_extract)
        if(TEST ${snx_test})
            set_property(TEST ${snx_test} APPEND PROPERTY ENVIRONMENT
                         "LD_PRELOAD=$<TARGET_FILE:snx-test-vulkan-drivers-kept>")
        endif()
    endforeach()
endif()

add_executable(snx-test-arm-profile arm_profile.c "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/thumb_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/vector_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/profile.c")
snx_test_target(snx-test-arm-profile 11)
add_test(NAME arm_isa_profiles COMMAND snx-test-arm-profile)
# a game's cheats by their names, as the launcher lists them and cheats_off= leaves them out
add_executable(snx-test-cheat-list cheat_list.c)
snx_test_target(snx-test-cheat-list 99)
target_link_libraries(snx-test-cheat-list PRIVATE scarletnx-common)
add_test(NAME cheat_list COMMAND snx-test-cheat-list)
# a mod's IPS, IPS32 and IPSwitch patches: their formats, which module takes one, the played mods' in their order
add_executable(snx-test-patches patches.c)
snx_test_target(snx-test-patches 99)
target_link_libraries(snx-test-patches PRIVATE scarletnx-common)
add_test(NAME patches COMMAND snx-test-patches)
# a recompiled game's stamp: what it is compared by, the made line after it, and the save states' copy of it
add_executable(snx-test-game-stamp game_stamp.c)
snx_test_target(snx-test-game-stamp 99)
target_link_libraries(snx-test-game-stamp PRIVATE scarletnx-common)
add_test(NAME game_stamp COMMAND snx-test-game-stamp)
if(SNX_TEST_ARM_AOT)
    include(arm_checked.cmake)
endif()

# Board/GUI-only builds do not configure either AOT runtime or LLVM. Do not
# create CPU execution tests that link the absent backend targets in that case.
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
# Exercise NX's real FS tree and service adapters even in the independent CTR
# build. These fixtures supply their own platform/kernel boundary stubs.
add_executable(snx-test-fs-fixture filesystem_fixture.c)
snx_test_target(snx-test-fs-fixture 99)
target_link_libraries(snx-test-fs-fixture PRIVATE scarletnx-common)
add_library(snx_test_fs_tree OBJECT "${PROJECT_SOURCE_DIR}/src/common/filesystem/tree.c")
snx_test_target(snx_test_fs_tree 11)
target_compile_options(snx_test_fs_tree PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fs_scan_probe.h")
set(snx_fs_suites tree)
if(TARGET hactool_internals) # the service reads dumps through hactool, built with the NX runtime
    list(APPEND snx_fs_suites service)
endif()
foreach(suite ${snx_fs_suites})
    add_executable(snx-test-fs-${suite} filesystem_${suite}.c $<TARGET_OBJECTS:snx_test_fs_tree>)
    snx_test_target(snx-test-fs-${suite} 11)
    add_test(NAME filesystem_${suite}
        COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-${suite}>)
endforeach()
if(TARGET hactool_internals)
    target_compile_options(snx-test-fs-service PRIVATE -Wno-unused-parameter)
    snx_swipc(snx-test-fs-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc hle/filesystem/fs.swipc)
    # fs.c reads a game's dump in place through the driver's container.c and hactool
    target_sources(snx-test-fs-service PRIVATE "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/container.c")
    target_link_libraries(snx-test-fs-service PRIVATE scarletnx-common hactool_internals)
    add_test(NAME filesystem_raw COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> raw)
    add_test(NAME filesystem_addon_overlay COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> addon-overlay)
    add_test(NAME filesystem_mods COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> mods)
    add_test(NAME filesystem_mods_stale COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> mods-stale)
    if(NOT WIN32) # the dump's link: Windows makes one only with the right to
        add_test(NAME filesystem_dump COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump)
        add_test(NAME filesystem_dump_source COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-source)
        add_test(NAME filesystem_dump_none COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-none)
        add_test(NAME filesystem_dump_gone COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-gone)
        add_test(NAME filesystem_dump_update COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-update)
        add_test(NAME filesystem_mods_image COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> mods-image)
        add_test(NAME filesystem_dump_update_extracted
                 COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-update-extracted)
        add_test(NAME filesystem_dump_dlc COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-dlc)
    endif()
endif()
if(UNIX)
    # host.c by itself: shared files, folders and the mutex
    add_executable(snx-test-fs-host filesystem_host.c "${PROJECT_SOURCE_DIR}/src/common/filesystem/host.c")
    snx_test_target(snx-test-fs-host 11)
    target_link_libraries(snx-test-fs-host PRIVATE Threads::Threads)
    add_test(NAME filesystem_host COMMAND snx-test-fs-host WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(snx-test-data-dir data_dir.c)
    snx_test_target(snx-test-data-dir 99)
    target_link_libraries(snx-test-data-dir PRIVATE scarletnx-common)
    add_test(NAME data_dir COMMAND snx-test-data-dir)
    add_test(NAME runtime_deploy COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/deploy.cmake")
endif()
# snx_os_error: the system's reason a message names, Windows' GetLastError where its calls leave errno
add_executable(snx-test-os-error os_error.c)
snx_test_target(snx-test-os-error 99)
target_link_libraries(snx-test-os-error PRIVATE scarletnx-common)
add_test(NAME os_error COMMAND snx-test-os-error)
# the host's side of a flat guest space (common/memory/host.c): views of one object are the same memory, a commit zeros
add_executable(snx-test-host-memory host_memory.c)
snx_test_target(snx-test-host-memory 99)
target_link_libraries(snx-test-host-memory PRIVATE scarletnx-common)
add_test(NAME host_memory COMMAND snx-test-host-memory)
# RtAPI.cas128: the host's 16-byte compare-and-swap and the locked one, a pair stored whole or not at all
add_executable(snx-test-cas128 cas128.c)
snx_test_target(snx-test-cas128 11)
target_link_libraries(snx-test-cas128 PRIVATE scarletnx-common Threads::Threads)
add_test(NAME cas128 COMMAND snx-test-cas128)
# guest byte order on any host (src/common/utilities/endian.h), strict C99 as the code that includes it
add_executable(snx-test-endian endian.c)
snx_test_target(snx-test-endian 99)
add_test(NAME endian COMMAND snx-test-endian)
if(SNX_BUILD_3DS)
    add_executable(snx-test-3ds-uds 3ds_uds.c fake_room.c)
    snx_test_target(snx-test-3ds-uds 11)
    target_link_libraries(snx-test-3ds-uds PRIVATE snx_ctr_process scarletnx-ldn snx_enet)
    add_test(NAME 3ds_uds_relay COMMAND snx-test-3ds-uds)
    add_test(NAME 3ds_uds_delayed_relay COMMAND snx-test-3ds-uds --delay)
    set_tests_properties(3ds_uds_delayed_relay PROPERTIES TIMEOUT 60)
    set_tests_properties(3ds_uds_relay PROPERTIES TIMEOUT 60)
endif()
if(NOT SNX_BUILD_NX AND NOT SNX_BUILD_GNUABI)
    # The shared room transport also serves 3DS UDS; keep its socket regression
    # available when building only the 3DS driver.
    if(SNX_BUILD_3DS)
        add_executable(snx-test-ldn-room ldn_room.c fake_room.c)
        snx_test_target(snx-test-ldn-room 23)
        target_link_libraries(snx-test-ldn-room PRIVATE scarletnx-ldn snx_enet)
        add_test(NAME ldn_room COMMAND snx-test-ldn-room)
        set_tests_properties(ldn_room PROPERTIES TIMEOUT 120)
    endif()
    include(architecture.cmake)
    snx_sanitizer_path()
    return()
endif()

# Validate the direct writer against SPIRV-Tools when the system tools are installed.
if(SNX_BUILD_NX)
    find_program(SNX_SPIRV_VAL spirv-val)
    find_program(SNX_SPIRV_DIS spirv-dis)
    if(SNX_SPIRV_VAL AND SNX_SPIRV_DIS AND Python3_Interpreter_FOUND)
        add_executable(snx-test-spirv-emitter spirv_emitter.c spirv_emitter_fixture.c
            "${PROJECT_SOURCE_DIR}/src/common/utilities/spirv_emit.c")
        snx_test_target(snx-test-spirv-emitter 11)
                add_test(NAME spirv_emitter COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/spirv_emitter_test.py"
            --generator $<TARGET_FILE:snx-test-spirv-emitter> --validator "${SNX_SPIRV_VAL}" --disassembler "${SNX_SPIRV_DIS}")
    else()
        message(STATUS "Skipping spirv_emitter: Python3, spirv-val, and spirv-dis are required")
    endif()
endif()

add_executable(snx-test-arm-decode arm_decode.c "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/rt_helpers.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/thumb_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/vector_decode.c")
snx_test_target(snx-test-arm-decode 11)
add_test(NAME arm_decode COMMAND snx-test-arm-decode)
add_executable(snx-test-arm-fp-host arm_fp_host.c)
snx_test_target(snx-test-arm-fp-host 11)
target_link_libraries(snx-test-arm-fp-host PRIVATE snx_arm_rt)
add_test(NAME arm_fp_host COMMAND snx-test-arm-fp-host)
add_executable(snx-test-arm-exec arm_exec_test.c)
snx_test_target(snx-test-arm-exec 11)
target_link_libraries(snx-test-arm-exec PRIVATE snx_llvm)
add_test(NAME arm_exec_dry_svc COMMAND snx-test-arm-exec)

# Small architectural witnesses need neither game assets nor an emulator.
add_executable(snx-test-a64-fp a64_fp_test.c "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp 11)
if(NOT WIN32)
    target_link_libraries(snx-test-a64-fp PRIVATE m)
endif()
snx_cpu_helpers_flags(snx-test-a64-fp)
add_test(NAME a64_fp_witnesses COMMAND snx-test-a64-fp)

# CRC32 and CRC32C through the LLVM steps, the host's instructions and the portable sliced tables, against the
# bit-serial definition
add_executable(snx-test-crc32-helper crc32_helper.c llvm_steps.c)
snx_test_target(snx-test-crc32-helper 11)
target_link_libraries(snx-test-crc32-helper PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME crc32_helper COMMAND snx-test-crc32-helper)

# the FP fast paths taken for normal operands, with FPCR.FZ clear and set and without FPSR's flags: no helper call
# left once LLVM folds the guards
add_executable(snx-test-llvm-fp-fast llvm_fp_fast.c llvm_steps.c)
snx_test_target(snx-test-llvm-fp-fast 11)
target_link_libraries(snx-test-llvm-fp-fast PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_fp_fast COMMAND snx-test-llvm-fp-fast)

add_executable(snx-test-a64-fp-arithmetic a64_fp_arithmetic_test.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp-arithmetic 11)
snx_cpu_helpers_flags(snx-test-a64-fp-arithmetic)
if(NOT WIN32)
    target_link_libraries(snx-test-a64-fp-arithmetic PRIVATE m)
endif()
add_test(NAME a64_fp_arithmetic_witnesses COMMAND snx-test-a64-fp-arithmetic)

# the A64 decoder tools/a64dec generates from src/common/cpu/aarch64/decode/ (a64.decode and a64.profile)
include("${PROJECT_SOURCE_DIR}/cmake/a64dec.cmake")
add_executable(snx-test-a64-decode a64_decode_test.c)
snx_test_target(snx-test-a64-decode 99)
snx_a64dec(snx-test-a64-decode)
add_test(NAME a64_decode COMMAND snx-test-a64-decode)

# The QEMU reference (tests/qemu_ref/): hand-checked cases, the ported cases and the decoder's UNDEF sets under
# qemu-aarch64 -cpu max and -cpu cortex-a57. Skipped (ctest's skip, code 77) without qemu-aarch64, ld.lld or a Clang
# for AArch64
include("${PROJECT_SOURCE_DIR}/cmake/qemu_ref.cmake")
add_executable(snx-test-qemu-ref qemu_ref/selftest.c qemu_ref/qemu_ref.c)
snx_test_target(snx-test-qemu-ref 99)
target_include_directories(snx-test-qemu-ref PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-qemu-ref PRIVATE snx_test_common)
snx_a64dec(snx-test-qemu-ref)
if(SNX_QEMU_REF)
    snx_qemu_ref_runner()
    add_dependencies(snx-test-qemu-ref snx_qemu_ref_runner)
    add_test(NAME qemu_ref_selftest COMMAND snx-test-qemu-ref "${SNX_QEMU_AARCH64}" "${SNX_QEMU_REF_RUNNER}"
             "${CMAKE_CURRENT_BINARY_DIR}/qemu_ref")
else()
    add_test(NAME qemu_ref_selftest COMMAND snx-test-qemu-ref --skip "${SNX_QEMU_REF_MISSING}")
endif()
set_tests_properties(qemu_ref_selftest PROPERTIES SKIP_RETURN_CODE 77)

# The emulated network stack over the host's loopback (src/common/network)
add_executable(snx-test-network-stack network_stack.c)
snx_test_target(snx-test-network-stack 23)
target_link_libraries(snx-test-network-stack PRIVATE scarletnx-net)
add_test(NAME network_stack COMMAND snx-test-network-stack)
set_tests_properties(network_stack PROPERTIES TIMEOUT 60)

add_executable(snx-test-network-icmp network_icmp.c)
snx_test_target(snx-test-network-icmp 23)
target_link_libraries(snx-test-network-icmp PRIVATE scarletnx-net)
add_test(NAME network_icmp COMMAND snx-test-network-icmp)
set_tests_properties(network_icmp PROPERTIES TIMEOUT 30)

# swipc (tools/swipc): the dispatchers generated from tests/swipc/test.swipc and other.swipc, driven with hand-made
# requests; the same output compiled as strict C99 against a stand-in for ipc.h;
# and definitions the generator has to refuse, each with its line and message
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
add_executable(snx-test-swipc-dispatch swipc_dispatch.c)
snx_test_target(snx-test-swipc-dispatch 23)
snx_swipc(snx-test-swipc-dispatch ROOT "${PROJECT_SOURCE_DIR}" FILES tests/swipc/test.swipc tests/swipc/other.swipc)
add_test(NAME swipc_dispatch COMMAND snx-test-swipc-dispatch)
add_library(snx-test-swipc-c99 OBJECT swipc/c99/c99.c)
set_target_properties(snx-test-swipc-c99 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx-test-swipc-c99 PRIVATE -pedantic-errors -Wall -Wextra -Werror)
target_include_directories(snx-test-swipc-c99 BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/swipc/c99")
snx_swipc(snx-test-swipc-c99 ROOT "${PROJECT_SOURCE_DIR}" FILES tests/swipc/test.swipc tests/swipc/other.swipc)
foreach(bad IN ITEMS
        array_of_arrays 5 "give the element a name"
        bad_number 4 "bad number 12ab"
        big_command_id 4 "number too large"
        buffer_two_modes 5 "exactly one of MapAlias"
        duplicate_argument 4 "two arguments named a"
        duplicate_command 6 "command 0 is defined twice"
        duplicate_field 5 "two fields named x"
        duplicate_service 5 "service test:a is served twice"
        exchange_buffer 5 "exchange \\(W\\) buffers"
        fixed_size_array 4 "it is not FixedSize"
        keyword_argument 4 "a C keyword"
        long_service_name 3 "service name abcdefghi has more than 8 characters"
        move_handle_in 5 "a client can't send a move handle"
        output_too_big 5 "the reply takes 260 bytes"
        padding_name 4 "padN_ names"
        pid_field 4 "not struct fields"
        pid_output 4 "pid is an input"
        reply_with_handles 5 "the reply takes 272 bytes"
        request_too_big 5 "the request takes 280 bytes"
        reserved_command 5 "the name of a generated function"
        scoped_argument 4 "a scoped name"
        struct_in_place 5 "give it a name with type"
        stub_handle 5 "a @stub answers zeros"
        stub_object 5 "a @stub answers zeros"
        too_many_moves 5 "more than 15 move handles"
        two_pids 5 "one pid or aruid at most"
        type_before_definition 5 "unknown type test::Later"
        unknown_interface 5 "object<test::INope>: no such interface"
        unnamed_argument 5 "has a name"
        zero_alignment 4 "bad alignment 0")
    if(NOT DEFINED bad_name)
        set(bad_name "${bad}")
    elseif(NOT DEFINED bad_line)
        set(bad_line "${bad}")
    else()
        add_test(NAME swipc_refuses_${bad_name} COMMAND snx-swipc --root "${CMAKE_CURRENT_SOURCE_DIR}/swipc/bad"
                 --out "${CMAKE_CURRENT_BINARY_DIR}" ${bad_name}.swipc)
        set_tests_properties(swipc_refuses_${bad_name} PROPERTIES
                             PASS_REGULAR_EXPRESSION "${bad_name}.swipc:${bad_line}: error: .*${bad}")
        unset(bad_name)
        unset(bad_line)
    endif()
endforeach()
add_test(NAME swipc_says_why_it_cannot_open COMMAND snx-swipc --root "${CMAKE_CURRENT_SOURCE_DIR}/swipc/bad"
         --out "${CMAKE_CURRENT_BINARY_DIR}" missing.swipc)
set_tests_properties(swipc_says_why_it_cannot_open PROPERTIES PASS_REGULAR_EXPRESSION
                     "swipc: cannot open [^\n]*missing\\.swipc: [A-Za-z]")

# This IPC fixture stubs the kernel and needs no NX graphics/media dependencies
add_executable(snx-test-bsd-service bsd_service.c)
snx_test_target(snx-test-bsd-service 23)
snx_service_swipc(snx-test-bsd-service hle/services/network/bsd.swipc)
target_link_libraries(snx-test-bsd-service PRIVATE scarletnx-ldn)
add_test(NAME bsd_service COMMAND snx-test-bsd-service)
set_tests_properties(bsd_service PROPERTIES TIMEOUT 60)

# the Eden room server the room tests talk to (fake_room.h), with the ENet it is written on: a test that links it
# gets the pinned ENet, never a system one's headers
add_library(snx-test-fake-room STATIC fake_room.c)
snx_test_target(snx-test-fake-room 23)
target_link_libraries(snx-test-fake-room PUBLIC scarletnx-net snx_enet)

# sfdnsres and nifm (dns.c, nifm.c) offline, with bsd.c beside them for whether the console is online
add_executable(snx-test-network-service network_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c"
    own_stub.c)
snx_test_target(snx-test-network-service 23)
snx_service_swipc(snx-test-network-service hle/services/network/bsd.swipc hle/services/network/dns.swipc
                  hle/services/network/nifm.swipc)
target_link_libraries(snx-test-network-service PRIVATE scarletnx-ldn snx-test-fake-room)
add_test(NAME network_service COMMAND snx-test-network-service)
set_tests_properties(network_service PROPERTIES TIMEOUT 60)

# local wireless over the LAN (common/network/net_ldn.c): the frames, and two consoles on the loopback
add_executable(snx-test-ldn-lan ldn_lan.c)
snx_test_target(snx-test-ldn-lan 23)
target_link_libraries(snx-test-ldn-lan PRIVATE scarletnx-ldn)
add_test(NAME ldn_lan COMMAND snx-test-ldn-lan)
set_tests_properties(ldn_lan PROPERTIES TIMEOUT 60)

# an Eden room (common/network/net_room.c) against a room server in the test: the messages, joining, ldn's networks
# and the game's datagram sockets through it
add_executable(snx-test-ldn-room ldn_room.c)
snx_test_target(snx-test-ldn-room 23)
target_link_libraries(snx-test-ldn-room PRIVATE scarletnx-ldn snx-test-fake-room)
add_test(NAME ldn_room COMMAND snx-test-ldn-room)
set_tests_properties(ldn_room PROPERTIES TIMEOUT 120)

# ldn:u (ldn.c): two consoles in one process, one creating a network and the other joining, with bsd.c beside for
# the address a game socket binds meanwhile; the session id comes from OpenSSL's generator
if(TARGET OpenSSL::Crypto)
    add_executable(snx-test-ldn-service ldn_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c")
    snx_test_target(snx-test-ldn-service 23)
    snx_service_swipc(snx-test-ldn-service hle/services/network/bsd.swipc hle/services/network/ldn.swipc)
    target_link_libraries(snx-test-ldn-service PRIVATE scarletnx-ldn snx-test-fake-room OpenSSL::Crypto)
    add_test(NAME ldn_service COMMAND snx-test-ldn-service)
    set_tests_properties(ldn_service PROPERTIES TIMEOUT 60)
    # both tests take 127.0.0.1 and 127.0.0.2 on the ldn port: never at the same time
    set_tests_properties(ldn_lan ldn_service PROPERTIES RESOURCE_LOCK ldn_port)
endif()

# the Eden room of ldn=room (room.c): room='s address, users/online.ini's password, what stops the game before joining
add_executable(snx-test-room-service room_service.c)
snx_test_target(snx-test-room-service 23)
target_link_libraries(snx-test-room-service PRIVATE scarletnx-ldn scarletnx-common)
add_test(NAME room_service COMMAND snx-test-room-service)

# ssl (ssl.c) against a TLS server in the test, on the game's bsd sockets (bsd.c, built beside). OpenSSL comes with
# the NX, 3DS and hactool builds (cmake/dependencies.cmake), not with a GNU ABI-only one
if(TARGET OpenSSL::SSL)
    add_executable(snx-test-ssl-service ssl_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c"
    own_stub.c)
    snx_test_target(snx-test-ssl-service 23)
        snx_service_swipc(snx-test-ssl-service hle/services/network/bsd.swipc hle/services/network/ssl.swipc)
    target_link_libraries(snx-test-ssl-service PRIVATE scarletnx-ldn OpenSSL::SSL)
    add_test(NAME ssl_service COMMAND snx-test-ssl-service)
    set_tests_properties(ssl_service PROPERTIES TIMEOUT 60)
endif()

# mm:u and the smaller services, hid's IHidServer and acc (misc.c, hid.c, acc.c, whose user picture snx_splash draws),
# with the kernel objects stubbed
if(TARGET OpenSSL::Crypto AND NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-misc-service misc_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_touch.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_motion.c")
    snx_test_target(snx-test-misc-service 23)
        snx_service_swipc(snx-test-misc-service hle/services/hid.swipc hle/services/misc.swipc hle/services/acc.swipc)
    target_link_libraries(snx-test-misc-service PRIVATE OpenSSL::Crypto snx_splash scarletnx-common)
    if(NOT WIN32)
        target_link_libraries(snx-test-misc-service PRIVATE m)
    endif()
    add_test(NAME misc_service COMMAND snx-test-misc-service)

    # time.c's clocks, time zones and shared memory, with the guest clock and the kernel objects stubbed
    add_executable(snx-test-time-service time_service.c)
    snx_test_target(snx-test-time-service 23)
    snx_service_swipc(snx-test-time-service hle/services/time.swipc)
    target_link_libraries(snx-test-time-service PRIVATE OpenSSL::Crypto)
    add_test(NAME time_service COMMAND snx-test-time-service)
endif()

# Inline software keyboard protocol and applet completion events.
add_executable(snx-test-am-service am_service.c am_service_stubs.c)
snx_test_target(snx-test-am-service 23)
snx_service_swipc(snx-test-am-service hle/services/am.swipc)
target_link_libraries(snx-test-am-service PRIVATE snx_test_common) # snx_nacp_aoc_base
add_test(NAME am_service COMMAND snx-test-am-service)

# am.c with a control.nacp: the desired language against its supported languages, its version and save sizes
add_executable(snx-test-am-nacp am_nacp.c am_service_stubs.c)
snx_test_target(snx-test-am-nacp 23)
snx_service_swipc(snx-test-am-nacp hle/services/am.swipc)
target_link_libraries(snx-test-am-nacp PRIVATE snx_test_common) # snx_nacp_aoc_base
add_test(NAME am_nacp COMMAND snx-test-am-nacp)

# acc.c and users.c: the console's users from users/users.ini, as acc lists them, every file that lists none refused,
# and the picture of a user with none of its own, Wimsy as a 256x256 JPEG (the splash's drawing), decoded by plutovg
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-acc-service acc_service.c)
    snx_test_target(snx-test-acc-service 23)
    snx_service_swipc(snx-test-acc-service hle/services/acc.swipc)
    target_link_libraries(snx-test-acc-service PRIVATE snx_splash scarletnx-common)
    target_include_directories(snx-test-acc-service SYSTEM PRIVATE "${plutovg_SOURCE_DIR}/include")
    target_compile_definitions(snx-test-acc-service PRIVATE PLUTOVG_BUILD_STATIC)
    set_target_properties(snx-test-acc-service PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME acc_service COMMAND snx-test-acc-service)
endif()

# set and set:sys: the firmware version's layout and the language codes (set.c)
add_executable(snx-test-set-service set_service.c)
snx_test_target(snx-test-set-service 23)
snx_service_swipc(snx-test-set-service hle/services/set.swipc)
add_test(NAME set_service COMMAND snx-test-set-service)

# sm: the service names it accepts, and an application's RegisterService and UnregisterService (sm.c)
add_executable(snx-test-sm-service sm_service.c)
snx_test_target(snx-test-sm-service 23)
snx_service_swipc(snx-test-sm-service hle/services/sm.swipc)
add_test(NAME sm_service COMMAND snx-test-sm-service)

# vi's binder: the native window and the buffer queue's parcels (vi.c), with the GPU thread and the renderer stubbed
add_executable(snx-test-vi-service vi_service.c)
snx_test_target(snx-test-vi-service 23)
snx_service_swipc(snx-test-vi-service hle/services/vi.swipc)
add_test(NAME vi_service COMMAND snx-test-vi-service)

# the guest clock and vi's presenting side (clock.c, present.c): the system tick, fps_limit's pacing, CPU-read buffers
add_executable(snx-test-runtime-present runtime_present.c)
snx_test_target(snx-test-runtime-present 11)
add_test(NAME runtime_present COMMAND snx-test-runtime-present)

# nvdrv's IPC side (nvdrv.c), with the GPU and host1x stand-ins in the test
add_executable(snx-test-nvdrv-service nvdrv_service.c)
snx_test_target(snx-test-nvdrv-service 23)
snx_service_swipc(snx-test-nvdrv-service hle/services/nvdrv.swipc)
add_test(NAME nvdrv_service COMMAND snx-test-nvdrv-service)

# ipc.c's parsing and replies (hle/kernel/ipc.c, included by the test) with raw HIPC messages laid out as libnx
# writes them, a test service, and the kernel's handles and objects stubbed
add_executable(snx-test-ipc-message ipc_message.c)
snx_test_target(snx-test-ipc-message 23)
add_test(NAME ipc_message COMMAND snx-test-ipc-message)

if(SNX_BUILD_NX)
add_test(NAME llvm_dependency COMMAND "${CMAKE_COMMAND}"
    "-DSNX_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DSNX_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/llvm-dependency"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/llvm_dependency.cmake")

add_executable(snx-test-overlay overlay.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/overlay.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/fatal_screen.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/prepare_screen.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/vpad.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/stats.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-overlay 11)
target_link_libraries(snx-test-overlay PRIVATE scarletnx-common snx_splash m)
include("${PROJECT_SOURCE_DIR}/cmake/gui_themes.cmake")
target_include_directories(snx-test-overlay PRIVATE "${PROJECT_BINARY_DIR}/generated")
add_test(NAME overlay COMMAND snx-test-overlay)

add_executable(snx-test-vpad vpad.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/vpad.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/stats.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-vpad 11)
target_link_libraries(snx-test-vpad PRIVATE scarletnx-common snx_splash m)
target_include_directories(snx-test-vpad PRIVATE "${PROJECT_BINARY_DIR}/generated") # ui.c's theme
add_test(NAME vpad COMMAND snx-test-vpad)

add_executable(snx-test-hud hud.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/hud.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c")
snx_test_target(snx-test-hud 11)
target_link_libraries(snx-test-hud PRIVATE scarletnx-common snx_splash m)
target_include_directories(snx-test-hud PRIVATE "${PROJECT_BINARY_DIR}/generated") # the theme's colours
add_test(NAME hud COMMAND snx-test-hud)

add_executable(snx-test-stats stats.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/stats.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/vpad.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-stats 11)
target_link_libraries(snx-test-stats PRIVATE scarletnx-common snx_splash m)
target_include_directories(snx-test-stats PRIVATE "${PROJECT_BINARY_DIR}/generated") # the theme's colours
add_test(NAME stats COMMAND snx-test-stats)

add_executable(snx-test-ini-deprecated ini_deprecated.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-ini-deprecated 11)
add_test(NAME ini_deprecated COMMAND snx-test-ini-deprecated)

add_executable(snx-test-pad-map pad_map.c)
snx_test_target(snx-test-pad-map 11)
target_link_libraries(snx-test-pad-map PRIVATE scarletnx-common)
add_test(NAME pad_map COMMAND snx-test-pad-map)
add_executable(snx-test-pad-screen pad_screen.c)
snx_test_target(snx-test-pad-screen 11)
target_link_libraries(snx-test-pad-screen PRIVATE scarletnx-common)
add_test(NAME pad_screen COMMAND snx-test-pad-screen)

include("${PROJECT_SOURCE_DIR}/cmake/pad_sdl.cmake")
add_executable(snx-test-gamepad gamepad.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/backend/gamepad.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-gamepad 11)
target_link_libraries(snx-test-gamepad PRIVATE scarletnx-common snx_pad_sdl snx_splash m)
target_include_directories(snx-test-gamepad PRIVATE "${PROJECT_BINARY_DIR}/generated") # ui.c's theme
add_test(NAME gamepad COMMAND snx-test-gamepad)

add_executable(snx-test-fatal-screen fatal_screen.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/fatal_screen.c")
snx_test_target(snx-test-fatal-screen 11)
target_link_libraries(snx-test-fatal-screen PRIVATE scarletnx-common)
add_test(NAME fatal_screen COMMAND snx-test-fatal-screen)

add_executable(snx-test-ini ini.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-ini 11)
add_test(NAME ini COMMAND snx-test-ini)
add_executable(snx-test-ini-lines ini_lines.c "${PROJECT_SOURCE_DIR}/src/common/utilities/ini_lines.c")
snx_test_target(snx-test-ini-lines 99)
add_test(NAME ini_lines COMMAND snx-test-ini-lines)
# the R_ result helpers (src/nx/hle/result.h), strict C99 as the code that may use them
add_executable(snx-test-result-macros result_macros.c)
snx_test_target(snx-test-result-macros 99)
add_test(NAME result_macros COMMAND snx-test-result-macros)

# generated and checked in (the controls' atlas, their shaders): their sources must still make them
if(Python3_Interpreter_FOUND)
    add_test(NAME controls_atlas_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-controls.py" --check)
    add_test(NAME ui_shaders_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-ui-shaders.py" --check)
    add_test(NAME generators_crlf COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generators_crlf.py")
    # every 3D method that acts is marked in the fast path's table (engines.c g_m3d_group)
    add_test(NAME m3d_acts COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/m3d_acts_test.py")
    # no keys, dumps, stray binaries, piracy sites or the console maker's name in prose among the tracked files: the
    # checks on planted ones (self test), then on the checkout. Skipped (77) without git or outside a checkout
    add_test(NAME tracked_material_self_test
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tracked_material.py" --self-test)
    add_test(NAME tracked_material
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tracked_material.py" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(tracked_material_self_test tracked_material PROPERTIES SKIP_RETURN_CODE 77)
    # which GitHub Actions caches the cleanup workflow deletes (scripts/trim-caches.py)
    add_test(NAME trim_caches COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/trim_caches_test.py")
    # the check that keeps a pull request red while its title says donotmerge (scripts/pr-title.py, pr-checks.yml)
    add_test(NAME pr_title COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/pr_title_test.py")
    # the labels a title's [area] prefix asks for, which the same job adds (scripts/area-labels.py, pr-checks.yml)
    add_test(NAME area_labels COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/area_labels_test.py")
    # the check that holds a pull request until its assignees, its author aside, approve it (scripts/pr-assignees.py,
    # pr-checks.yml)
    add_test(NAME pr_assignees COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/pr_assignees_test.py")
    # the target LLVM's llvm-config of the linux-*-cross presets, run under its machine's qemu (scripts/)
    if(UNIX)
        add_test(NAME cross_llvm_config
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cross_llvm_config_test.py")
    endif()
    # the log summary (src/common/utilities/logscan.c), and the issues with a log: summarized, labelled and grouped
    # (scripts/game-reports.py, game-reports.yml), both through snx-logscan
    if(TARGET snx-logscan)
        add_test(NAME logscan
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/logscan_test.py" $<TARGET_FILE:snx-logscan>)
        add_test(NAME game_reports COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/game_reports_test.py"
                                           $<TARGET_FILE:snx-logscan>)
    endif()
    # the game tools/run_game.py finds by its name (recompiled/, else an older version's games/)
    add_test(NAME run_game COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_game_test.py")
    # the nightly release's title and notes (scripts/nightly-notes.py)
    add_test(NAME nightly_notes COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/nightly_notes_test.py")
    # the builds published as releases and on Discord (scripts/publish-builds.py), against fakes. Where publish.yml
    # runs it, Linux: the fake gh is a script Windows does not run by its name
    if(NOT WIN32)
        add_test(NAME publish_builds
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/publish_builds_test.py")
        # edenprobe's runs held against a baseline (scripts/edenprobe.py), with a fake runtime and game
        add_test(NAME edenprobe_script COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/edenprobe_test.py")
        # the pull request's comment linking its builds (scripts/pr-builds.sh), against a fake gh; 77 without jq
        add_test(NAME pr_builds COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/pr_builds_test.py")
        set_tests_properties(pr_builds PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# a package patched in the build tree from the cache's copy as released (cmake/patched_copy.cmake)
add_test(NAME patched_copy COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/patched-copy-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/patched_copy_test.cmake")
# the dependency cache's keys and how a build is published to it (cmake/deps_cache.cmake)
add_test(NAME deps_cache COMMAND "${CMAKE_COMMAND}" "-DCC=${CMAKE_C_COMPILER}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/deps-cache-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/deps_cache_test.cmake")
# LLVM's revision for the unit cache's key, read by CMake from its C++ header (cmake/llvm_revision.cmake)
add_test(NAME llvm_revision COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/llvm-revision-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/llvm_revision_test.cmake")

# hid's shared memory layout (src/nx/hle/services/hid_layout.h) is checked against Eden's declarations at a pinned
# revision by check_hid_layout.py, run by hand: `python3 tests/check_hid_layout.py --eden <an Eden checkout>`. It
# needs that checkout, so it isn't a test here
add_executable(snx-test-hid-touch hid_touch.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/touch.c"
    "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_touch.c")
snx_test_target(snx-test-hid-touch 11)
target_link_libraries(snx-test-hid-touch PRIVATE scarletnx-common)
add_test(NAME hid_touch COMMAND snx-test-hid-touch)

add_executable(snx-test-hid-motion hid_motion.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_motion.c")
snx_test_target(snx-test-hid-motion 11)
target_link_libraries(snx-test-hid-motion PRIVATE scarletnx-common)
if(NOT WIN32)
    target_link_libraries(snx-test-hid-motion PRIVATE m)
endif()
add_test(NAME hid_motion COMMAND snx-test-hid-motion)

add_executable(snx-test-startup startup_options.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/startup.c")
snx_test_target(snx-test-startup 99)
target_link_libraries(snx-test-startup PRIVATE scarletnx-common) # an option's older name said (snx_option_renamed)
if(SNX_TRACY)
    # startup.c must see SNX_TRACY so its Tracy flag checks match the runtime.
    target_compile_definitions(snx-test-startup PRIVATE SNX_TRACY=1)
endif()
add_test(NAME startup_options COMMAND snx-test-startup)

add_library(snx_test_common STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/common.c"
    "${PROJECT_SOURCE_DIR}/src/common/utilities/ini_lines.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/game_ini.c"
    "${PROJECT_SOURCE_DIR}/src/common/utilities/script.c")
snx_test_target(snx_test_common 99)
if(WIN32)
    target_link_libraries(snx_test_common PUBLIC advapi32)
endif()

# the console a game's folder plays and its copy of the runtime (common/utilities/game_ini.c), which the launcher reads
add_executable(snx-test-game-system game_system.c)
snx_test_target(snx-test-game-system 99)
target_link_libraries(snx-test-game-system PRIVATE snx_test_common)
add_test(NAME game_system COMMAND snx-test-game-system)

# a run's scripted presses (common/utilities/script.c), which the 3DS, Vita and GBA drivers read
add_executable(snx-test-script script.c)
snx_test_target(snx-test-script 99)
target_link_libraries(snx-test-script PRIVATE snx_test_common)
add_test(NAME script COMMAND snx-test-script)

add_executable(snx-test-driver driver_workflow.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/driver.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/setup.c" "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
    "${PROJECT_SOURCE_DIR}/src/common/utilities/process.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/logscan.c")
snx_test_target(snx-test-driver 99)
target_link_libraries(snx-test-driver PRIVATE snx_test_common snx_setup snx_lz4 OpenSSL::Crypto snx_demangle)
set_target_properties(snx-test-driver PROPERTIES LINKER_LANGUAGE CXX)
if(APPLE) # driver/setup.c's weak snx_menu_language, as snx_driver has it: ld64 wants it named
    target_link_options(snx-test-driver PRIVATE "LINKER:-U,_snx_menu_language")
endif()
add_test(NAME driver_workflow COMMAND snx-test-driver)
set_tests_properties(driver_workflow PROPERTIES TIMEOUT 60)
add_executable(snx-test-link-file link_file.c)
snx_test_target(snx-test-link-file 99)
target_link_libraries(snx-test-link-file PRIVATE snx_test_common)
add_test(NAME link_file COMMAND snx-test-link-file)
if(APPLE)
    # MoltenVK, which the app carries and a game's runtime copy (outside the bundle) gets beside it (docs/APPLE.md)
    add_executable(snx-test-vulkan-driver vulkan_driver.c)
    snx_test_target(snx-test-vulkan-driver 99)
    target_link_libraries(snx-test-vulkan-driver PRIVATE snx_test_common)
    add_test(NAME vulkan_driver COMMAND snx-test-vulkan-driver)
endif()
add_executable(snx-test-launcher-name launcher_name.c)
snx_test_target(snx-test-launcher-name 99)
target_link_libraries(snx-test-launcher-name PRIVATE scarletnx-common)
add_test(NAME launcher_name COMMAND snx-test-launcher-name)
add_executable(snx-test-game-title game_title.c)
snx_test_target(snx-test-game-title 99)
target_link_libraries(snx-test-game-title PRIVATE scarletnx-common)
add_test(NAME game_title COMMAND snx-test-game-title)
if(UNIX)
    foreach(name user_dirs left_behind)
        string(REPLACE "_" "-" target "snx-test-${name}")
        add_executable(${target} ${name}.c)
        snx_test_target(${target} 99)
        target_link_libraries(${target} PRIVATE scarletnx-common)
        add_test(NAME ${name} COMMAND ${target})
    endforeach()
endif()
# the setup's files and choices (common/utilities/setup.c), its notices against docs/DEPRECATIONS.md
if(UNIX AND TARGET snx_setup)
    add_executable(snx-test-setup setup.c)
    snx_test_target(snx-test-setup 99)
    target_link_libraries(snx-test-setup PRIVATE snx_setup)
    target_compile_definitions(snx-test-setup PRIVATE SNX_DEPRECATIONS="${PROJECT_SOURCE_DIR}/docs/DEPRECATIONS.md")
    add_test(NAME setup COMMAND snx-test-setup)
    # the driver's setup in a terminal, its answers from a file
    add_executable(snx-test-setup-terminal setup_terminal.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/setup.c")
    snx_test_target(snx-test-setup-terminal 99)
    target_link_libraries(snx-test-setup-terminal PRIVATE snx_setup)
    add_test(NAME setup_terminal COMMAND snx-test-setup-terminal)
endif()

if(UNIX)
    add_executable(snx-test-gdbstub gdbstub.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/gdbstub.c")
    snx_test_target(snx-test-gdbstub 11)
        target_link_libraries(snx-test-gdbstub PRIVATE snx_test_common)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gdbstub_native COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdbstub.py" $<TARGET_FILE:snx-test-gdbstub>)
        set_tests_properties(gdbstub_native PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(snx-test-run-ticking run_ticking.c)
snx_test_target(snx-test-run-ticking 99)
target_link_libraries(snx-test-run-ticking PRIVATE snx_test_common)
add_test(NAME run_ticking COMMAND snx-test-run-ticking)

# C11 for hactool's headers, which build the synthetic dumps (hac_synth.h); extract.c itself stays C99 in the driver
add_executable(snx-test-extract extract.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/extract.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/container.c")
snx_test_target(snx-test-extract 11)
target_link_libraries(snx-test-extract PRIVATE snx_test_common hactool_internals)
add_test(NAME extraction_native COMMAND snx-test-extract)

add_executable(snx-test-loader loader_test.c "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c")
snx_test_target(snx-test-loader 99)
target_link_libraries(snx-test-loader PRIVATE snx_test_common snx_lz4 OpenSSL::Crypto)
add_test(NAME loader_native COMMAND snx-test-loader)

add_executable(snx-test-runtime-nro runtime_nro.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/loader.c")
snx_test_target(snx-test-runtime-nro 11)
target_link_libraries(snx-test-runtime-nro PRIVATE snx_lz4 scarletnx-common)
add_test(NAME runtime_nro COMMAND snx-test-runtime-nro)

if(NOT WIN32)
    # a guest's call into a loaded NRO's code that the module lacks, through RtAPI.miss, is llvm_module's
    add_executable(snx-test-ldr-service ldr_service.c
        "${PROJECT_SOURCE_DIR}/src/nx/hle/services/ldr.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_test_target(snx-test-ldr-service 11)
    snx_service_swipc(snx-test-ldr-service hle/services/ldr.swipc)
    target_link_libraries(snx-test-ldr-service PRIVATE OpenSSL::Crypto m)
    snx_cpu_helpers_flags(snx-test-ldr-service)
    add_test(NAME ldr_service COMMAND snx-test-ldr-service)
    # again as on a host with 64 KiB pages (PowerPC64), each holding sixteen guest pages
    add_test(NAME ldr_service_64k_pages COMMAND snx-test-ldr-service)
    set_tests_properties(ldr_service ldr_service_64k_pages PROPERTIES TIMEOUT 60)
    set_tests_properties(ldr_service_64k_pages PROPERTIES ENVIRONMENT SNX_HOST_PAGE=65536)
endif()

# Deterministic FFmpeg API double: no decoder dependency in tools-only builds.
add_executable(snx-test-nvdec-packets nvdec_packets.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp"
    support/nvdec_ref.c)
snx_test_target(snx-test-nvdec-packets 11)
target_include_directories(snx-test-nvdec-packets BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_stub")
add_test(NAME nvdec_packets COMMAND snx-test-nvdec-packets)
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-nvdec-decode nvdec_packets.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp"
        support/nvdec_ref.c)
    snx_test_target(snx-test-nvdec-decode 11)
    target_compile_definitions(snx-test-nvdec-decode PRIVATE SNX_TEST_REAL_FFMPEG)
    target_link_libraries(snx-test-nvdec-decode PRIVATE FFmpeg::avcodec)
    add_test(NAME nvdec_decode COMMAND snx-test-nvdec-decode)
endif()
if(SNX_TEST_VP9_VECTORS)
    # nvdec.c's VP9 jobs on libvpx's test vectors, each image against the MD5 libvpx lists for it, at its own job's
    # fence (nvdec_vp9_vectors.c). FFmpeg's decoder works the jobs out of each vector from its private state
    # (nvdec_vp9_jobs.c), so that part builds against FFmpeg's source and configuration. The vectors and their lists
    # come from the WebM project's test data (BSD-3-Clause), downloaded once into CPM_SOURCE_CACHE: a vector's name,
    # then the SHA-256 of the vector and of its list. Hidden frames, contexts that adapt, frame parallel mode, a
    # superframe of hidden frames, frames shown again, intra-only frames, spatial layers, tiles, size changes,
    # segmentation, loop filter and quantizer deltas. Off by default: a build without the network (a Flatpak's, a
    # distribution's) would wait on the downloads
    set(snx_vp9_vectors
        vp90-2-02-size-66x66.webm 749e4260de357b2704117389cd0a620f73f8229043d214528d1cc7d81524faaa
            96cdfc13db1458a93a3cecaf73acc5cee5ec8d5112a0406f7321d88fcee486e0
        vp90-2-03-deltaq.webm c1ef6ca3eea22a53a79c7f82bdf295f2bf4d5c6dce7bb9659e0ab0e14bd6a72b
            fe813160663227127f8de16053fc23aeefe2516301e3c740da3a96b98511d404
        vp90-2-05-resize.ivf 84a345546e47e37de87d2959347bf731f9f440f9f2ab32bede3f0e416100c585
            3158d3e404f06137bd6d8a9a1ec33ea37ccfad7aea8c9f61fa691ca5ab6e6f7c
        vp90-2-07-frame_parallel.webm b101f5f71920b893ff301bd506d43d62f2f678ab9196ffc06fe605f1fa4caf4f
            b3c0d7a1412209c78c5b8113fdbf946ad44b2fa84cadcfdf84e23b0e077dc269
        vp90-2-08-tile-4x4.webm 89ce4c213d7f83f677e67a9e9906b61f1053949489b8343356034f0b9577e33e
            7a24e9c0e883ea4fffe88cac475ec8547386b48022ae280a7a78e056aec01dba
        vp90-2-09-lf_deltas.webm 651fc818fea9687fb1acccac35433c555cf005d2a78d39d5168d40760fd23fa1
            feebf034c3306affc93903dd8b9eaaa8f24f0c6eb8fe7a75cc8309fa107a4ede
        vp90-2-10-show-existing-frame.webm 21d60d305ed05f06d838f90f0e62bbd2a37c4b994304ad7f8556430cc551aa1b
            62bff400e0cb7e3ab0681971f623dc68d12dbf33fa5c8f2f9db7e2210d03da5c
        vp90-2-11-size-351x287.webm 4e8fa03af4fc90e97f0f0ac7f6808819586e8b063e369a7c47b13de443718b19
            bb7094ab6c422e1bbd35a150e823bc8fc483eded9d9f2cc3206baaaa240ccbb8
        vp90-2-15-segkey.webm 2809d3c237383bcecffee926c624dbbccd2146331cc569344caabbd8b8141449
            ad85c8e90bf15c7e80ea582aaa83762482f391e4be2cb7c7d98ddac851bed077
        vp90-2-15-segkey_adpq.webm c402502113c173ce4ba827fa50e7f1c4cc657ad0c0893ee550c4ca82f497355b
            9b0e367e2e0feb170a69b592f4cc855f7db5ca8453ca544357bdc8581a154646
        vp90-2-16-intra-only.webm 328abd0bbb630f76ed5e6a5ed94e8065f8dda53c8f5ad120fba515a03438512b
            2e037d40bfd4b4eca40409be440e8deeaeb23e87eeb429bc0bc5825406a8cc3f
        vp90-2-17-show-existing-frame.webm f78980c5e9db098946d4784c2d5e875da129050098bef7afc988e793dbb2edb1
            1d0a6b53c33cdb45690495fcc356cec1693bdd1f43e901bdc6457ea78b4b87f5
        vp90-2-19-skip.webm 7aa1de5c86fdb89fbd7443b79ef412771e29ca996f7ba2ea8f13375e37683bdd
            aed7e371a3706139f797d2e84ad055c67be78823c911d33a86d4c546ebebeb71
        vp90-2-20-big_superframe-01.webm 08c30e4e653e6f0bbcb58f0d506e843a45d9630060bb9568b6a04b3a8c61bdfa
            bc498d393ddecfa7742920d459362c13cf7c0d66cea5cbfa99c208c8d3943ce7
        vp90-2-21-resize_inter_320x180_5_1-2.webm 56de7662174fc1e07960ffec74e7e43a1ab77a86e382239ff62aa698568fee51
            ea204491ee6f70f9c73fdc71d6cf38da26817d3bb5a8d6b63708c98de106bef4
        vp90-2-22-svc_1280x720_3.ivf 41f2ac2086be31e4cf39bad75612ebfd5a3d5445b4328f064ec4f2996d286666
            adade013ec6d91e80e1f296dc65e27f8966c384ee052939b43baffcfc6a0a30c)
    add_library(snx-test-nvdec-vp9-jobs STATIC nvdec_vp9_jobs.c)
    snx_test_target(snx-test-nvdec-vp9-jobs 11)
    target_include_directories(snx-test-nvdec-vp9-jobs PRIVATE ${snx_ffmpeg_private_include})
    # FFmpeg's headers as system headers, for their warnings (on Windows, compat/w32pthreads.h's unused parameters);
    # as -isystem folders they would come after a -I that holds another FFmpeg's (cmake/ffmpeg.cmake)
    target_compile_options(snx-test-nvdec-vp9-jobs PRIVATE --system-header-prefix=libavcodec/
                           --system-header-prefix=libavutil/ --system-header-prefix=compat/)
    target_link_libraries(snx-test-nvdec-vp9-jobs PUBLIC FFmpeg::avcodec)
    add_executable(snx-test-nvdec-vp9-vectors nvdec_vp9_vectors.c
                   "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp"
                   support/nvdec_ref.c)
    snx_test_target(snx-test-nvdec-vp9-vectors 11)
    target_link_libraries(snx-test-nvdec-vp9-vectors PRIVATE snx-test-nvdec-vp9-jobs)
    while(snx_vp9_vectors)
        list(POP_FRONT snx_vp9_vectors name vector_sha256 list_sha256)
        string(REGEX REPLACE "^vp90-2-|\\.(webm|ivf)$" "" short "${name}")
        foreach(file "${name}" "${name}.md5")
            if(file STREQUAL name)
                set(sha256 ${vector_sha256})
                set(package snx_vp9_${short})
            else()
                set(sha256 ${list_sha256})
                set(package snx_vp9_${short}_md5)
            endif()
            CPMAddPackage(NAME ${package}
                          URL https://storage.googleapis.com/downloads.webmproject.org/test_data/libvpx/${file}
                          URL_HASH SHA256=${sha256}
                          DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
            # checked on every configure: CPM takes the empty folder a failed download leaves for a filled one
            set(path "${${package}_SOURCE_DIR}/${file}")
            set(have "")
            if(EXISTS "${path}")
                file(SHA256 "${path}" have)
            endif()
            if(NOT have STREQUAL sha256)
                message(FATAL_ERROR "${path} is missing or is not the pinned file (SHA-256 ${sha256}): delete "
                                    "${${package}_SOURCE_DIR} and configure again with the network")
            endif()
        endforeach()
        add_test(NAME nvdec_vp9_${short}
                 COMMAND snx-test-nvdec-vp9-vectors "${snx_vp9_${short}_SOURCE_DIR}/${name}"
                         "${snx_vp9_${short}_md5_SOURCE_DIR}/${name}.md5")
    endwhile()
endif()
# VP9 packets rebuilt from NVDEC jobs (gpu/video/vp9_rebuild.cpp): the destination of the frame each emits, and
# setups with invalid tile layouts refused before a header is built
add_executable(snx-test-vp9-rebuild vp9_rebuild.cpp "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp")
snx_test_target(snx-test-vp9-rebuild 23)
add_test(NAME vp9_rebuild COMMAND snx-test-vp9-rebuild)
# the 2D engine's blit in guest memory (fermi2d.c, dma.c's bl_span_ptr): block linear scaling, an odd window, the
# renderer's hooks once a blit
add_executable(snx-test-fermi2d-blit fermi2d_blit.c support/fermi2d_ref.c support/half.c)
snx_test_target(snx-test-fermi2d-blit 23)
add_test(NAME fermi2d_blit COMMAND snx-test-fermi2d-blit)
# vic.c's CPU composition, through vic_execute, with a renderer that declines the GPU
add_executable(snx-test-vic-compose vic_compose.c support/vic_ref.c)
snx_test_target(snx-test-vic-compose 23)
add_test(NAME vic_compose COMMAND snx-test-vic-compose)
if(NOT WIN32)
    # host1x.c's channels with pthread stand-ins for the platform: IOVAs, GET_SYNCPOINT, SUBMIT's checks and fences,
    # the command stream, a host class wait and a close that runs the queue first
    add_executable(snx-test-host1x-channel host1x_channel.c)
    snx_test_target(snx-test-host1x-channel 23)
    add_test(NAME host1x_channel COMMAND snx-test-host1x-channel)
endif()
if(NOT SNX_TOOLS_ONLY)
    # tex_upload (gpu/vk/texture.c) into the staging buffer: block-linear rows against the layout, mip levels, 3D,
    # pitch linear, a converted format; a format uploaded as it is needs no level-sized buffer
    add_executable(snx-test-tex-upload-rows tex_upload_rows.c tracy_stubs.c)
    snx_test_target(snx-test-tex-upload-rows 23)
    target_compile_definitions(snx-test-tex-upload-rows PRIVATE SNX_TEST_VMA_STUBS=1)
    target_link_libraries(snx-test-tex-upload-rows PRIVATE snx_nx_flags)
    add_test(NAME tex_upload_rows COMMAND snx-test-tex-upload-rows)
    # the file of shaders and pipelines kept across runs (gpu/vk/pipeline_file.c): records back as written, pipelines
    # by their shaders' records (a shipped shader's reference among them), a cut or changed record ending the reading,
    # another key starting over, a full file; and the shipped shaders' file read through its mapping
    add_executable(snx-test-pipeline-file pipeline_file.c tracy_stubs.c)
    snx_test_target(snx-test-pipeline-file 23)
    target_link_libraries(snx-test-pipeline-file PRIVATE snx_nx_flags scarletnx-common)
    add_test(NAME pipeline_file COMMAND snx-test-pipeline-file)
endif()

add_executable(snx_recompiler_test recompiler_native.c)
snx_test_target(snx_recompiler_test 23)
target_link_libraries(snx_recompiler_test PRIVATE snx_driver ${CMAKE_DL_LIBS})

# LLVM is always available to the recompiler.
add_test(NAME native_recompiler COMMAND snx_recompiler_test
    $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}" "${SNX_TARGET_C_COMPILER}")
# the whole-encoding sweeps (llvm_qemu_differential_sweep): opt in, they cost minutes
option(SNX_TEST_LLVM_SWEEP "Run the whole-encoding LLVM differential sweep (slow)" OFF)
# The LLVM backend against QEMU (tests/qemu_ref), instruction by instruction: llvm_qemu_differential.c, with the
# words of the generated decoder's rows (llvm_qemu_words.c, its table's own translation unit). The groups known to
# differ are in llvm_qemu_expected.txt. Skipped as qemu_ref_selftest is
if(NOT WIN32)
    add_executable(snx-test-llvm-qemu-differential llvm_qemu_differential.c llvm_qemu_words.c llvm_steps.c
        qemu_ref/qemu_ref.c)
    snx_test_target(snx-test-llvm-qemu-differential 11)
    target_include_directories(snx-test-llvm-qemu-differential PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-llvm-qemu-differential PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    snx_a64dec(snx-test-llvm-qemu-differential)
    set(snx_lqd_expected "${CMAKE_CURRENT_SOURCE_DIR}/llvm_qemu_expected.txt")
    if(SNX_QEMU_REF)
        add_dependencies(snx-test-llvm-qemu-differential snx_qemu_ref_runner)
        add_test(NAME llvm_qemu_differential COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu" "${snx_lqd_expected}" --quiet)
        # the same steps loaded with the JIT's loader, and built without the host's extensions (the emitters'
        # portable forms)
        add_test(NAME llvm_qemu_differential_jit COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_jit" "${snx_lqd_expected}" --quiet
                 --jit)
        add_test(NAME llvm_qemu_differential_portable COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_portable" "${snx_lqd_expected}"
                 --quiet --portable)
        set_tests_properties(llvm_qemu_differential_jit llvm_qemu_differential_portable PROPERTIES TIMEOUT 600)
        if(SNX_TEST_LLVM_SWEEP)
            add_test(NAME llvm_qemu_differential_sweep COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                     "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_sweep" "${snx_lqd_expected}"
                     --quiet --sweep)
            set_tests_properties(llvm_qemu_differential_sweep PROPERTIES TIMEOUT 3600 COST 100)
        endif()
    else()
        add_test(NAME llvm_qemu_differential COMMAND snx-test-llvm-qemu-differential --skip "${SNX_QEMU_REF_MISSING}")
    endif()
    set_tests_properties(llvm_qemu_differential PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
# the variant table's hash, which must match jit_service.c's cache hash
add_executable(snx-test-variants variants.c)
snx_test_target(snx-test-variants 11)
target_link_libraries(snx-test-variants PRIVATE scarletnx-common)
add_test(NAME variants COMMAND snx-test-variants)
# the AOT build linking a profile cache's unit and emitting snx_variants
add_executable(snx-test-aot-variants aot_variants.c)
snx_test_target(snx-test-aot-variants 11)
target_link_libraries(snx-test-aot-variants PRIVATE snx_driver_llvm ${CMAKE_DL_LIBS})
add_test(NAME aot_variants COMMAND snx-test-aot-variants)
set_tests_properties(aot_variants PROPERTIES TIMEOUT 120)
# the code units' cache (nx/loader/unit_cache.c) under the LLVM module builds, a 32-bit game's too: reuse, keys, broken
# objects, the limit
add_executable(snx-test-unit-cache unit_cache.c)
snx_test_target(snx-test-unit-cache 11)
target_link_libraries(snx-test-unit-cache PRIVATE snx_driver_llvm ${CMAKE_DL_LIBS})
add_test(NAME unit_cache COMMAND snx-test-unit-cache)
set_tests_properties(unit_cache PROPERTIES TIMEOUT 120)
# a 32-bit game's blocks going on to the next by themselves (nx/loader/module_arm.c): loops, interworking, the poll
add_executable(snx-test-arm-module-chain arm_module_chain.c)
snx_test_target(snx-test-arm-module-chain 11)
target_link_libraries(snx-test-arm-module-chain PRIVATE snx_driver_llvm ${CMAKE_DL_LIBS})
add_test(NAME arm_module_chain COMMAND snx-test-arm-module-chain)
set_tests_properties(arm_module_chain PROPERTIES TIMEOUT 120)
# the runtime looking a pc up in the AOT variant table instead of compiling it
add_executable(snx-test-runtime-variants runtime_variants.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/guest_jit.c")
snx_test_target(snx-test-runtime-variants 11)
target_link_libraries(snx-test-runtime-variants PRIVATE scarletnx-common scarletnx-filesystem snx_settings)
add_test(NAME runtime_variants COMMAND snx-test-runtime-variants)
# the runtime taking a module of SNX_ABI and refusing any other, on modules that are a descriptor alone (the fixture,
# also built as a 32-bit program's): plain C99, without sanitizers the runtime may lack
foreach(snx_fixture module-abi module-abi-arm)
    add_library(snx-test-${snx_fixture} MODULE module_abi_fixture.c)
    set_target_properties(snx-test-${snx_fixture} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-${snx_fixture} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    target_include_directories(snx-test-${snx_fixture} PRIVATE "${PROJECT_SOURCE_DIR}/src")
endforeach()
target_compile_definitions(snx-test-module-abi-arm PRIVATE SNX_TEST_MODULE_ARM)
add_executable(snx-test-runtime-module-abi module_abi.c)
snx_test_target(snx-test-runtime-module-abi 99)
target_link_libraries(snx-test-runtime-module-abi PRIVATE snx_test_common)
add_test(NAME runtime_module_abi COMMAND snx-test-runtime-module-abi $<TARGET_FILE:scarletnx>
    $<TARGET_FILE:snx-test-module-abi> $<TARGET_FILE:snx-test-module-abi-arm>)
# the program's malloc is the pinned jemalloc's (SNX_JEMALLOC). On macOS its prefix, je_, names the options' variable
if(SNX_JEMALLOC AND NOT CMAKE_CROSSCOMPILING)
    if(APPLE)
        set(snx_jemalloc_conf JE_MALLOC_CONF)
        set(snx_jemalloc_nm "")
        set(snx_jemalloc_preload "")
    else()
        set(snx_jemalloc_conf MALLOC_CONF)
        set(snx_jemalloc_nm "${CMAKE_NM}")
        # a preloaded pthread_cond_init that allocates, as the AppImage's cross-libc-dlopen.so
        add_library(snx-test-jemalloc-preload MODULE jemalloc_preload.c)
        target_link_libraries(snx-test-jemalloc-preload PRIVATE ${CMAKE_DL_LIBS})
        set(snx_jemalloc_preload "$<TARGET_FILE:snx-test-jemalloc-preload>")
    endif()
    add_test(NAME jemalloc COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:scarletnx>"
        "-DVERSION=${snx_jemalloc_version}" "-DCONF=${snx_jemalloc_conf}" "-DNM=${snx_jemalloc_nm}"
        "-DPRELOAD=${snx_jemalloc_preload}" -P "${CMAKE_CURRENT_SOURCE_DIR}/jemalloc.cmake")
endif()
# the module contract's layout as recorded when SNX_ABI last rose (docs/ARCHITECTURE.md, Module ABI)
add_executable(snx-test-module-contract module_contract.c)
snx_test_target(snx-test-module-contract 99)
add_test(NAME module_contract COMMAND snx-test-module-contract)
# the build-side reader of the runtime JIT's cache
add_executable(snx-test-jit-cache jit_cache.c)
snx_test_target(snx-test-jit-cache 11)
target_link_libraries(snx-test-jit-cache PRIVATE snx_llvm)
add_test(NAME jit_cache COMMAND snx-test-jit-cache)
# LLVM's SLP vectorizer turned off for a process (the setting slp_vectorizer), and the JIT cache's key saying so
add_executable(snx-test-slp-vectorizer slp_vectorizer.c)
snx_test_target(snx-test-slp-vectorizer 11)
target_link_libraries(snx-test-slp-vectorizer PRIVATE snx_llvm)
add_test(NAME slp_vectorizer COMMAND snx-test-slp-vectorizer)
# the launcher's AOT pack action: the cache's path and header, and the optimization the AOT build must share
add_executable(snx-test-aot-pack aot_pack.c "${PROJECT_SOURCE_DIR}/src/gui/aot.c")
snx_test_target(snx-test-aot-pack 11)
target_link_libraries(snx-test-aot-pack PRIVATE scarletnx-common)
add_test(NAME aot_pack COMMAND snx-test-aot-pack)
# the runtime JIT (jit_service.c): misses compiled, published and shared between threads, on that loader
add_executable(snx-test-jit-service jit_service.c)
snx_test_target(snx-test-jit-service 11)
target_link_libraries(snx-test-jit-service PRIVATE snx_llvm Threads::Threads)
add_test(NAME jit_service COMMAND snx-test-jit-service)
set_tests_properties(jit_service PROPERTIES TIMEOUT 120)
# longjmp across the loader's code, which on Windows unwinds through its registered function table
add_executable(snx-test-code-loader-unwind code_loader_unwind.c)
snx_test_target(snx-test-code-loader-unwind 11)
target_link_libraries(snx-test-code-loader-unwind PRIVATE snx_llvm)
add_test(NAME code_loader_unwind COMMAND snx-test-code-loader-unwind)
# the loader's objects as GDB's JIT interface lists them (gdb_jit.c); skipped on Windows, which registers none
add_executable(snx-test-gdb-jit gdb_jit.c)
snx_test_target(snx-test-gdb-jit 11)
target_link_libraries(snx-test-gdb-jit PRIVATE snx_llvm)
add_test(NAME gdb_jit COMMAND snx-test-gdb-jit)
set_tests_properties(gdb_jit PROPERTIES SKIP_RETURN_CODE 77)
if(NOT WIN32)
    # the relocations both ELF paths share (elf_object.c), AArch64's included, checked on any ELF host and on macOS,
    # whose JIT loads ELF objects
    add_executable(snx-test-elf-relocate elf_relocate.c)
    snx_test_target(snx-test-elf-relocate 11)
    target_link_libraries(snx-test-elf-relocate PRIVATE snx_llvm)
    add_test(NAME elf_relocate COMMAND snx-test-elf-relocate)
    set_tests_properties(elf_relocate PROPERTIES TIMEOUT 120)
endif()
# not a test: timings of the generated code (llvm_bench SCARLETNX REPO CLANG)
add_executable(snx-llvm-bench llvm_bench.c)
snx_test_target(snx-llvm-bench 11)
target_link_libraries(snx-llvm-bench PRIVATE snx_driver ${CMAKE_DL_LIBS})
# not a test: a game's functions recompiled on their own (llvm_pieces DUMP ADDR...), for the recompile's time and output
add_executable(snx-llvm-pieces llvm_pieces.c)
snx_test_target(snx-llvm-pieces 11)
target_link_libraries(snx-llvm-pieces PRIVATE scarletnx-recompiler)
# the backend's own switches (ir.h), in the library's objects: its headers, not its target, whose objects the library
# holds already
target_include_directories(snx-llvm-pieces PRIVATE "$<TARGET_PROPERTY:snx_llvm,INTERFACE_INCLUDE_DIRECTORIES>")
target_include_directories(snx-llvm-pieces SYSTEM PRIVATE "${SNX_LLVM_INCLUDE_DIR}")
# A32 emits self-contained host objects, without the A64 interpreter or RtAPI.
add_executable(snx-test-arm-emit arm_emit.c)
snx_test_target(snx-test-arm-emit 11)
target_link_libraries(snx-test-arm-emit PRIVATE snx_arm_llvm)
# Thumb16/32 emits self-contained host objects the same way.
add_executable(snx-test-thumb-emit thumb_emit.c)
snx_test_target(snx-test-thumb-emit 11)
target_link_libraries(snx-test-thumb-emit PRIVATE snx_arm_llvm)
option(SNX_TEST_ARM_DIFF "Compare A32 LLVM execution with Python unicorn 2.1.4" OFF)
if(UNIX AND NOT CMAKE_CROSSCOMPILING)
    if(Python3_Interpreter_FOUND)
        # the ARM tests run emitted code and Unicorn in Python: -X faulthandler names where a crash in either was
        add_test(NAME arm_llvm_witnesses COMMAND "${Python3_EXECUTABLE}" -X faulthandler
            "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
            --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}")
        add_test(NAME arm7tdmi_witnesses COMMAND "${Python3_EXECUTABLE}" -X faulthandler
            "${CMAKE_CURRENT_SOURCE_DIR}/arm7tdmi_test.py"
            --arm-emitter $<TARGET_FILE:snx-test-arm-emit> --thumb-emitter $<TARGET_FILE:snx-test-thumb-emit>
            --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}")
        if(SNX_TEST_ARM_DIFF)
            add_test(NAME arm_llvm_differential COMMAND "${Python3_EXECUTABLE}" -X faulthandler
                "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
                --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}" --unicorn)
            set_tests_properties(arm_llvm_differential PROPERTIES TIMEOUT 900)
            # VFP/NEON uses the same emitter plus the raw-bit FP and crypto runtime library.
            get_filename_component(snx_llvm_bin "${SNX_LLVM_CONFIG}" DIRECTORY)
            find_program(SNX_LLVM_MC llvm-mc HINTS "${snx_llvm_bin}" NO_DEFAULT_PATH)
            if(SNX_LLVM_MC)
                add_test(NAME arm_llvm_vector COMMAND "${Python3_EXECUTABLE}" -X faulthandler
                    "${CMAKE_CURRENT_SOURCE_DIR}/arm_vector_test.py"
                    --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt>
                    --mc "${SNX_LLVM_MC}" --cc "${CMAKE_C_COMPILER}")
                set_tests_properties(arm_llvm_vector PROPERTIES TIMEOUT 1800)
                add_test(NAME thumb_llvm COMMAND "${Python3_EXECUTABLE}" -X faulthandler
                    "${CMAKE_CURRENT_SOURCE_DIR}/thumb_test.py"
                    --emitter $<TARGET_FILE:snx-test-thumb-emit> --runtime $<TARGET_FILE:snx_arm_rt> --mc "${SNX_LLVM_MC}"
                    --cc "${CMAKE_C_COMPILER}")
                set_tests_properties(thumb_llvm PROPERTIES TIMEOUT 900)
                # random encodings of every class, llvm-objdump keeping the A57's, from random FP and flag state
                find_program(SNX_LLVM_OBJDUMP llvm-objdump HINTS "${snx_llvm_bin}" NO_DEFAULT_PATH)
                if(SNX_LLVM_OBJDUMP)
                    add_test(NAME arm_llvm_fuzz COMMAND "${Python3_EXECUTABLE}" -X faulthandler
                        "${CMAKE_CURRENT_SOURCE_DIR}/arm_fuzz.py"
                        --arm-emitter $<TARGET_FILE:snx-test-arm-emit> --thumb-emitter $<TARGET_FILE:snx-test-thumb-emit>
                        --runtime $<TARGET_FILE:snx_arm_rt> --mc "${SNX_LLVM_MC}" --objdump "${SNX_LLVM_OBJDUMP}"
                        --cc "${CMAKE_C_COMPILER}" --count 1500)
                    set_tests_properties(arm_llvm_fuzz PROPERTIES TIMEOUT 1800)
                endif()
            endif()
        endif()
    endif()
endif()

# LLVM Internals: passes cut out of LLVM's pipelines (nx/runtime/driver/llvm_tuning.c), which LLVM still runs
add_executable(snx-test-llvm-tuning llvm_tuning.c)
snx_test_target(snx-test-llvm-tuning 11)
target_link_libraries(snx-test-llvm-tuning PRIVATE snx_driver)
add_test(NAME llvm_tuning COMMAND snx-test-llvm-tuning)
# the X registers' cache within a block (ir_state.c)
add_executable(snx-test-llvm-gpr-cache llvm_gpr_cache.c)
snx_test_target(snx-test-llvm-gpr-cache 11)
target_link_libraries(snx-test-llvm-gpr-cache PRIVATE snx_llvm snx_driver)
add_test(NAME llvm_gpr_cache COMMAND snx-test-llvm-gpr-cache)
add_executable(snx-test-llvm-module llvm_module.c llvm_steps.c)
snx_test_target(snx-test-llvm-module 11)
target_link_libraries(snx-test-llvm-module PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_module COMMAND snx-test-llvm-module $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}")
# the recompiler as a library (snx_recompiler.h): A64 on every backend built, A32, from code in memory
add_executable(snx-test-recompiler-library recompiler_library.c)
snx_test_target(snx-test-recompiler-library 11)
target_link_libraries(snx-test-recompiler-library PRIVATE scarletnx-recompiler)
add_test(NAME recompiler_library COMMAND snx-test-recompiler-library)
# The Mach-O linker on objects Clang compiles for arm64 macOS, which any host's Clang with AArch64 can: what macOS
# would check of the dylib (link_macho.c), on hosts that can't load one
if(TARGET OpenSSL::Crypto)
    set(snx_macho_probe "${CMAKE_CURRENT_BINARY_DIR}/link_macho_probe.o")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" --target=arm64-apple-macos13 -ffreestanding -c
        "${CMAKE_CURRENT_SOURCE_DIR}/link_macho_fixture.c" -DPART=2 -o "${snx_macho_probe}"
        RESULT_VARIABLE snx_macho_result OUTPUT_QUIET ERROR_QUIET)
    if(snx_macho_result EQUAL 0)
        foreach(part 1 2 3 4)
            # LLVM's modules have no common symbols, which the linker refuses: a tentative definition is not one
            # whatever the compiler's default
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/link_macho_${part}.o"
                COMMAND "${CMAKE_C_COMPILER}" --target=arm64-apple-macos13 -ffreestanding -O2 -fPIC -fno-common
                    -DPART=${part} -c "${CMAKE_CURRENT_SOURCE_DIR}/link_macho_fixture.c"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/link_macho_${part}.o"
                DEPENDS link_macho_fixture.c VERBATIM)
        endforeach()
        set(snx_link_sources "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm")
        add_executable(snx-test-link-macho link_macho.c "${snx_link_sources}/link.c" "${snx_link_sources}/link_elf.c"
            "${snx_link_sources}/elf_object.c" "${snx_link_sources}/link_coff.c" "${snx_link_sources}/link_macho.c"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_1.o" "${CMAKE_CURRENT_BINARY_DIR}/link_macho_2.o"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_3.o" "${CMAKE_CURRENT_BINARY_DIR}/link_macho_4.o")
        # the objects are the test's data, not part of its own link
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/link_macho_1.o"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_2.o" "${CMAKE_CURRENT_BINARY_DIR}/link_macho_3.o"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_4.o" PROPERTIES EXTERNAL_OBJECT FALSE HEADER_FILE_ONLY TRUE)
        snx_test_target(snx-test-link-macho 11)
        target_include_directories(snx-test-link-macho PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(snx-test-link-macho PRIVATE OpenSSL::Crypto)
        add_test(NAME link_macho COMMAND snx-test-link-macho "${CMAKE_CURRENT_BINARY_DIR}/link_macho_1.o"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_2.o" "${CMAKE_CURRENT_BINARY_DIR}/link_macho.dylib"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_3.o" "a call to the undefined weak symbol _missing"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_4.o" "aligned beyond a page")
    endif()
endif()
if(APPLE)
    # aot_variants links a JIT cache unit as ELF into an AOT module that is Mach-O on this host: it needs its own fix
    # before it can run here
    set_tests_properties(aot_variants PROPERTIES DISABLED TRUE)
endif()

# the generated decoder, function discovery, and LLVM steps against expected values (llvm_steps.c)
add_executable(snx_codegen_test codegen_test.c llvm_steps.c)
snx_test_target(snx_codegen_test 99)
target_link_libraries(snx_codegen_test PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME native_codegen COMMAND snx_codegen_test)

# A64 cases ported from rEden's dynarmic tests (a64_ported_cases.h) through the LLVM backend's steps, loaded with the
# JIT's loader (llvm_steps.c): their expected values on any host and, where qemu_ref runs, each run against QEMU
# from the same state
add_executable(snx-test-a64-ported a64_ported.c llvm_steps.c qemu_ref/qemu_ref.c)
snx_test_target(snx-test-a64-ported 11)
target_include_directories(snx-test-a64-ported PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(snx-test-a64-ported PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
if(SNX_QEMU_REF)
    add_dependencies(snx-test-a64-ported snx_qemu_ref_runner)
    add_test(NAME a64_ported_cases COMMAND snx-test-a64-ported "${SNX_QEMU_AARCH64}" "${SNX_QEMU_REF_RUNNER}"
             "${CMAKE_CURRENT_BINARY_DIR}/a64_ported")
else()
    add_test(NAME a64_ported_cases COMMAND snx-test-a64-ported)
endif()

add_executable(snx-test-astc astc_decode.c support/astc.c)
snx_test_target(snx-test-astc 23)
add_test(NAME astc_decode COMMAND snx-test-astc)
# the ASTC shader's GLSL run on the CPU (tests/glsl/glsl.hpp) against the same decoder: no GPU needed
add_executable(snx-test-astc-glsl glsl/astc_glsl.cpp support/astc.c)
snx_test_target(snx-test-astc-glsl 23)
target_include_directories(snx-test-astc-glsl PRIVATE glsl)
add_test(NAME astc_glsl COMMAND snx-test-astc-glsl)
# the BC7 encoder's GLSL run on the CPU (tests/glsl/glsl.hpp), its blocks decoded by the CPU reference: no GPU needed
add_executable(snx-test-bc7-glsl glsl/bc7_glsl.cpp glsl/bc7_cpu.cpp support/bcn.c)
snx_test_target(snx-test-bc7-glsl 23)
target_include_directories(snx-test-bc7-glsl PRIVATE glsl)
add_test(NAME bc7_glsl COMMAND snx-test-bc7-glsl)
# the VIC shader's GLSL, both builds, run on the CPU against vic.c's composition of random jobs: no GPU needed
add_executable(snx-test-vic-glsl glsl/vic_jobs.c glsl/vic_glsl.cpp support/vic_ref.c)
snx_test_target(snx-test-vic-glsl 23)
target_include_directories(snx-test-vic-glsl PRIVATE glsl)
add_test(NAME vic_glsl COMMAND snx-test-vic-glsl)
# the block linear deswizzle and swizzle shader's GLSL run on the CPU against bl_offset's layout: no GPU needed
add_executable(snx-test-blocklinear-glsl glsl/blocklinear_glsl.cpp)
snx_test_target(snx-test-blocklinear-glsl 23)
target_include_directories(snx-test-blocklinear-glsl PRIVATE glsl)
add_test(NAME blocklinear_glsl COMMAND snx-test-blocklinear-glsl)
# the Vita's texture deswizzle shader's GLSL run on the CPU against Vita3K's loops: no GPU needed
add_executable(snx-test-vita-swizzle-glsl glsl/vita_swizzle_glsl.cpp)
snx_test_target(snx-test-vita-swizzle-glsl 23)
target_include_directories(snx-test-vita-swizzle-glsl PRIVATE glsl)
add_test(NAME vita_swizzle_glsl COMMAND snx-test-vita-swizzle-glsl)
# the Vita's decoded picture layout shader's GLSL run on the CPU against the decoder's layout: no GPU needed
add_executable(snx-test-vita-picture-glsl glsl/vita_picture_glsl.cpp)
snx_test_target(snx-test-vita-picture-glsl 23)
target_include_directories(snx-test-vita-picture-glsl PRIVATE glsl)
add_test(NAME vita_picture_glsl COMMAND snx-test-vita-picture-glsl)
# the Vita's YUV420 texture shader's GLSL run on the CPU against the conversion written out: no GPU needed
add_executable(snx-test-vita-yuv-glsl glsl/vita_yuv_glsl.cpp)
snx_test_target(snx-test-vita-yuv-glsl 23)
target_include_directories(snx-test-vita-yuv-glsl PRIVATE glsl)
add_test(NAME vita_yuv_glsl COMMAND snx-test-vita-yuv-glsl)
# the Vita's 24-bit color surface shader's GLSL run on the CPU against the layout written out: no GPU needed
add_executable(snx-test-vita-rgb24-glsl glsl/vita_rgb24_glsl.cpp)
snx_test_target(snx-test-vita-rgb24-glsl 23)
target_include_directories(snx-test-vita-rgb24-glsl PRIVATE glsl)
add_test(NAME vita_rgb24_glsl COMMAND snx-test-vita-rgb24-glsl)
# the Vita's imports given memory that isn't the game's: their errors, and no access there
if(TARGET snx_vita_program)
    add_executable(snx-test-vita-hle-memory vita_hle_memory.c)
    snx_test_target(snx-test-vita-hle-memory 11)
    target_link_libraries(snx-test-vita-hle-memory PRIVATE snx_vita_program)
    add_test(NAME vita_hle_memory COMMAND snx-test-vita-hle-memory)
endif()
# the texture conversion shader's GLSL run on the CPU against the CPU's conversions and ETC2 and BCn decoders: no GPU
# needed
add_executable(snx-test-texconv-glsl glsl/texconv_glsl.cpp support/etc2.c support/bcn.c support/half.c)
snx_test_target(snx-test-texconv-glsl 23)
target_include_directories(snx-test-texconv-glsl PRIVATE glsl)
add_test(NAME texconv_glsl COMMAND snx-test-texconv-glsl)

# single blocks from rEden's ASTC decoder tests, against edenprobe's and astcenc's texels (astc_blocks.c)
add_executable(snx-test-astc-blocks astc_blocks.c support/astc.c)
snx_test_target(snx-test-astc-blocks 23)
add_test(NAME astc_blocks COMMAND snx-test-astc-blocks)

add_executable(snx-test-nvn-command-gap nvn_command_gap.c support/nvn_command_gap.c)
snx_test_target(snx-test-nvn-command-gap 23)
add_test(NAME nvn_command_gap COMMAND snx-test-nvn-command-gap)

add_executable(snx-test-nvn-control nvn_control.c support/nvn_control.c)
snx_test_target(snx-test-nvn-control 23)
add_test(NAME nvn_control COMMAND snx-test-nvn-control)

# the shared fonts pl.c hands out when a game folder has none: the stand-ins compiled in (cmake/shared_fonts.cmake)
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-pl-fonts pl_fonts.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/pl_font.c" "${PROJECT_BINARY_DIR}/generated/shared_fonts.c")
    snx_test_target(snx-test-pl-fonts 23)
    snx_service_swipc(snx-test-pl-fonts hle/services/pl.swipc)
    add_test(NAME pl_fonts COMMAND snx-test-pl-fonts)
endif()

# audout and hwopus in a fixture, with FFmpeg's Opus decoder for the hwopus decoders that open. FFmpeg::avcodec comes
# from cmake/dependencies.cmake, before this folder, in every build but a tools-only one, which checks the fixture
# without it (SNX_NO_H264, which no runtime build defines)
if(SNX_TOOLS_ONLY)
    add_executable(snx-test-audio-out audio_out.c)
    snx_test_target(snx-test-audio-out 11)
    snx_service_swipc(snx-test-audio-out hle/services/audio.swipc)
    target_compile_definitions(snx-test-audio-out PRIVATE SNX_NO_H264)
    if(NOT WIN32)
        target_link_libraries(snx-test-audio-out PRIVATE m)
    endif()
    add_test(NAME audio_out COMMAND snx-test-audio-out)
else()
    add_executable(snx-test-audio-opus audio_out.c)
    snx_test_target(snx-test-audio-opus 11)
    snx_service_swipc(snx-test-audio-opus hle/services/audio.swipc)
    target_link_libraries(snx-test-audio-opus PRIVATE FFmpeg::avcodec)
    if(NOT WIN32)
        target_link_libraries(snx-test-audio-opus PRIVATE m)
    endif()
    add_test(NAME audio_opus COMMAND snx-test-audio-opus)
endif()

if(UNIX)
    add_executable(snx-test-audren audren_render.c)
    snx_test_target(snx-test-audren 11)
        target_link_libraries(snx-test-audren PRIVATE m)
    add_test(NAME audren_render COMMAND snx-test-audren)
endif()

# audren.c's host side with the DSP stubbed: the output device's volume on every renderer's host stream
add_executable(snx-test-audren-host audren_host.c)
snx_test_target(snx-test-audren-host 23)
add_test(NAME audren_host COMMAND snx-test-audren-host)

add_executable(snx-test-render-enable render_enable.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c")
snx_test_target(snx-test-render-enable 23)
add_test(NAME render_enable COMMAND snx-test-render-enable)

add_executable(snx-test-clear-flags clear_flags.c)
snx_test_target(snx-test-clear-flags 23)
add_test(NAME clear_flags COMMAND snx-test-clear-flags)

add_executable(snx-test-instance-array instance_array.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c")
snx_test_target(snx-test-instance-array 23)
add_test(NAME instance_array COMMAND snx-test-instance-array)

add_executable(snx-test-draw-texture draw_texture.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c")
snx_test_target(snx-test-draw-texture 23)
add_test(NAME draw_texture COMMAND snx-test-draw-texture)

# gpu.c's queue over the real puller, 3D engine and macros: a channel waiting on a semaphore, a syncpoint, a fence or
# inside a macro lets the others run, and goes on where it stopped (its platform stand-ins are pthreads)
if(UNIX)
    add_executable(snx-test-gpu-park gpu_park.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gpu.c"
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c"
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/mme.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/mme_hle.c"
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/dma.c"
        support/dma_ref.c own_stub.c)
    snx_test_target(snx-test-gpu-park 23)
    target_link_libraries(snx-test-gpu-park PRIVATE scarletnx-common) # gpu.c says why a thread did not start
        add_test(NAME gpu_park COMMAND snx-test-gpu-park)
    set_tests_properties(gpu_park PROPERTIES TIMEOUT 30) # a deadlock in the queue would otherwise hang ctest
endif()

# mme.c's native macros chosen by the hash of their code, the interpreter for unknown code, and the verify mode
add_executable(snx-test-mme-native mme_native.c)
snx_test_target(snx-test-mme-native 23)
add_test(NAME mme_native COMMAND snx-test-mme-native)

# mme_hle.c's native macros against the interpreter, each over a macro written in the test from what it does
add_executable(snx-test-mme-hle mme_hle.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/mme_hle.c")
snx_test_target(snx-test-mme-hle 23)
add_test(NAME mme_hle COMMAND snx-test-mme-hle)

# the macros mme_hle.c has no implementation for, compiled by mme_jit.c: every instruction and branch case compared
# with the interpreter's, then seeded random programs
add_executable(snx-test-mme-jit mme_jit.c)
snx_test_target(snx-test-mme-jit 23)
target_link_libraries(snx-test-mme-jit PRIVATE snx_llvm)
add_test(NAME mme_jit COMMAND snx-test-mme-jit)

# the shader programs a game ships, found in containers built by the test (gpu/shader/shipped.c)
add_executable(snx-test-shipped-shaders shipped_shaders.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/shipped.c")
snx_test_target(snx-test-shipped-shaders 99)
add_test(NAME shipped_shaders COMMAND snx-test-shipped-shaders)
# the walk over a game's files and segments for those programs (gpu/shipped_pass.c), on a RomFS made by the test
add_executable(snx-test-shipped-pass shipped_pass.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/shipped_pass.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/shipped.c")
snx_test_target(snx-test-shipped-pass 99)
include("${PROJECT_SOURCE_DIR}/cmake/ship_walk.cmake")
snx_ship_walk("${PROJECT_SOURCE_DIR}/src/nx/gpu/shipped_pass.c")
add_test(NAME shipped_pass COMMAND snx-test-shipped-pass)

add_executable(snx-test-etc2 etc2_decode.c support/etc2.c)
snx_test_target(snx-test-etc2 23)
add_test(NAME etc2_decode COMMAND snx-test-etc2)
add_executable(snx-test-bcn bcn_decode.c support/bcn.c)
snx_test_target(snx-test-bcn 23)
add_test(NAME bcn_decode COMMAND snx-test-bcn)

# the LLVM backend's IR rewrite for an older llvm-as (a cross build's LLVM older than its Clang): what it takes out
add_test(NAME rtlib_ir_compat COMMAND "${CMAKE_COMMAND}"
         -DSCRIPT=${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm/rtlib_ir_compat.cmake
         -DWORK=${CMAKE_CURRENT_BINARY_DIR} -P "${CMAKE_CURRENT_SOURCE_DIR}/rtlib_ir_compat_test.cmake")

if(UNIX)
    add_executable(snx-test-kernel-mutex kernel_mutex.c)
    snx_test_target(snx-test-kernel-mutex 11)
        add_test(NAME kernel_mutex COMMAND snx-test-kernel-mutex)
    add_executable(snx-test-kernel-wait kernel_wait.c)
    snx_test_target(snx-test-kernel-wait 11)
        add_test(NAME kernel_wait COMMAND snx-test-kernel-wait)
    add_executable(snx-test-kernel-cores kernel_cores.c)
    snx_test_target(snx-test-kernel-cores 11)
        add_test(NAME kernel_cores COMMAND snx-test-kernel-cores)
    # g_hle holds no pointer and nothing of the host's, so a save state writes it as it is (hle_flat.c)
    add_executable(snx-test-hle-flat hle_flat.c)
    snx_test_target(snx-test-hle-flat 11)
        add_test(NAME hle_flat COMMAND snx-test-hle-flat)
    if(NOT WIN32)
        # a save state's file: sections read back, damage refused, a failed save leaves the old state (state_file.c)
        add_executable(snx-test-state-file state_file.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/state_file.c")
        snx_test_target(snx-test-state-file 11)
        target_link_libraries(snx-test-state-file PRIVATE snx_zstd scarletnx-common)
        add_test(NAME state_file COMMAND snx-test-state-file "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(snx-test-plat-cpus plat_cpus.c)
    snx_test_target(snx-test-plat-cpus 11)
    target_link_libraries(snx-test-plat-cpus PRIVATE SDL3::SDL3) # plat_posix.c's threads end SDL's per-thread state
    add_test(NAME plat_cpus COMMAND snx-test-plat-cpus)
endif()

# the process's memory as common.c reads it, which memory_fit takes (process_memory.c)
add_executable(snx-test-process-memory process_memory.c)
snx_test_target(snx-test-process-memory 11)
target_link_libraries(snx-test-process-memory PRIVATE scarletnx-common)
add_test(NAME process_memory COMMAND snx-test-process-memory)

# EAC worked examples and edenprobe's blocks from rEden's decode_eac test (eac_blocks.c)
add_executable(snx-test-eac-blocks eac_blocks.c support/etc2.c)
snx_test_target(snx-test-eac-blocks 23)
add_test(NAME eac_blocks COMMAND snx-test-eac-blocks)

if(UNIX)
    add_executable(snx-test-memory memory_accounting.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/hle.c")
    snx_test_target(snx-test-memory 11)
        add_test(NAME memory_accounting COMMAND snx-test-memory)
    set_tests_properties(memory_accounting PROPERTIES TIMEOUT 60)
    # views through the alias pool, with the real POSIX platform layer (77 where the host has no pool)
    add_executable(snx-test-memory-alias memory_alias.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/own.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/hle.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_posix.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c" "${PROJECT_SOURCE_DIR}/src/common/filesystem/host.c")
    snx_test_target(snx-test-memory-alias 11)
    target_link_libraries(snx-test-memory-alias PRIVATE SDL3::SDL3 ${CMAKE_DL_LIBS})
    add_test(NAME memory_alias COMMAND snx-test-memory-alias)
    set_tests_properties(memory_alias PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # the same calls with the alias pool off (alias_pool=0): views are copies, as on Windows
    add_test(NAME memory_alias_copies COMMAND snx-test-memory-alias --copies)
    set_tests_properties(memory_alias_copies PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # GPU-owned memory over the real pool and traps, owners whose GPU copy is host memory (77 without traps)
    add_executable(snx-test-memory-own memory_own.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/own.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_posix.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c" "${PROJECT_SOURCE_DIR}/src/common/filesystem/host.c")
    snx_test_target(snx-test-memory-own 11)
    target_link_libraries(snx-test-memory-own PRIVATE Threads::Threads SDL3::SDL3 ${CMAKE_DL_LIBS})
    add_test(NAME memory_own COMMAND snx-test-memory-own)
    set_tests_properties(memory_own PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

endif() # SNX_BUILD_NX

# dist/icons/ is what scripts/make-icons.py draws from dist/wimsy.svg: a changed SVG fails until it runs again
if(Python3_Interpreter_FOUND)
    add_test(NAME dist_icons COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/make-icons.py" --check)
endif()
if(NOT WIN32)
    include(GNUInstallDirs)
    add_test(NAME dist_install COMMAND "${CMAKE_COMMAND}" -DBUILD=${PROJECT_BINARY_DIR}
        -DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/dist-install -DDATADIR=${CMAKE_INSTALL_DATADIR} -DNX=${SNX_BUILD_NX}
        -DGUI=${SNX_BUILD_GUI} -DDIST=${PROJECT_SOURCE_DIR}/dist -P "${CMAKE_CURRENT_SOURCE_DIR}/dist_install.cmake")
endif()
include(architecture.cmake)

add_executable(snx-test-depth-texel depth_texel.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/depth.c")
snx_test_target(snx-test-depth-texel 23)
add_test(NAME depth_texel COMMAND snx-test-depth-texel)
add_executable(snx-test-write-pages write_pages.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c")
snx_test_target(snx-test-write-pages 23)
add_test(NAME write_pages COMMAND snx-test-write-pages)
add_executable(snx-test-pending-pages pending_pages.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/pending_pages.c")
snx_test_target(snx-test-pending-pages 23)
add_test(NAME pending_pages COMMAND snx-test-pending-pages)
add_executable(snx-test-unattended-script unattended_script.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-unattended-script 11)
target_link_libraries(snx-test-unattended-script PRIVATE m)
add_test(NAME unattended_script COMMAND snx-test-unattended-script)
# the log's file, early lines, stamps, redirect, limits, fatal and crash mode (platform/plat.c)
add_executable(snx-test-platform-log platform_log.c "${PROJECT_SOURCE_DIR}/src/common/filesystem/host.c")
snx_test_target(snx-test-platform-log 11)
target_link_libraries(snx-test-platform-log PRIVATE scarletnx-common) # the runs' logs: snx_list_dir, snx_file_time
add_test(NAME platform_log COMMAND snx-test-platform-log "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT WIN32 AND TARGET SDL3::SDL3) # SDL3 is there with a driver that draws (cmake/dependencies.cmake)
    # written-page tracking, decommit, pread, file maps, replace, folders, waits and threads (platform/plat_posix.c and
    # plat_sdl.c)
    add_executable(snx-test-platform-posix platform_posix.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c"
        "${PROJECT_SOURCE_DIR}/src/common/filesystem/host.c")
    snx_test_target(snx-test-platform-posix 11)
    target_link_libraries(snx-test-platform-posix PRIVATE SDL3::SDL3 ${CMAKE_DL_LIBS})
    add_test(NAME platform_posix COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-shader-brx shader_brx.c)
    snx_test_target(snx-test-shader-brx 23)
    target_link_libraries(snx-test-shader-brx PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-brx PRIVATE m)
    endif()
    add_test(NAME shader_brx COMMAND snx-test-shader-brx)
    # the decoder generated by tools/shaderdec from src/nx/gpu/shader/maxwell_opcodes.def
    add_executable(snx-test-shader-decode shader_decode.c)
    snx_test_target(snx-test-shader-decode 23)
    target_link_libraries(snx-test-shader-decode PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-decode PRIVATE m)
    endif()
    add_test(NAME shader_decode COMMAND snx-test-shader-decode)
    # dual vertex programs, the static CRS stack and the corpus files (gpu/shader/shader.c, corpus.c)
    add_executable(snx-test-shader-program shader_program.c)
    snx_test_target(snx-test-shader-program 23)
    target_link_libraries(snx-test-shader-program PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-program PRIVATE m)
    endif()
    add_test(NAME shader_program COMMAND snx-test-shader-program "${CMAKE_CURRENT_BINARY_DIR}")
    # structured control flow (gpu/shader/structure.c) against random programs' own
    add_executable(snx-test-shader-structure shader_structure.c)
    snx_test_target(snx-test-shader-structure 23)
    target_link_libraries(snx-test-shader-structure PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-structure PRIVATE m)
    endif()
    add_test(NAME shader_structure COMMAND snx-test-shader-structure)
    if(NOT CMAKE_CROSSCOMPILING)
        # the SVC registers tools/svcgen lays out from src/nx/hle/svc/hos.svc, in both ABIs
        include("${PROJECT_SOURCE_DIR}/cmake/svcgen.cmake")
        add_test(NAME svcgen_layout COMMAND "${CMAKE_COMMAND}" "-DSVCGEN=$<TARGET_FILE:snx-svcgen>"
                 "-DSPEC=${PROJECT_SOURCE_DIR}/src/nx/hle/svc/hos.svc"
                 "-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/svcgen_layout.txt"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/svcgen_layout.cmake")
        # tools/settingsgen on headers of its own: LLVM's setters however their declarations are broken over lines,
        # the passes of a pipeline, and an @ID this LLVM lacks
        add_test(NAME settingsgen COMMAND "${CMAKE_COMMAND}" "-DSETTINGSGEN=$<TARGET_FILE:snx-settingsgen>"
                 "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/settingsgen-test"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/settingsgen.cmake")
        if(UNIX) # the opts the test runs are shell scripts
            # cmake/llvm_opt.cmake: the pipelines of an opt of the LLVM's exact version, and what another opt gets
            add_test(NAME llvm_opt COMMAND "${CMAKE_COMMAND}"
                     "-DLLVM_OPT_CMAKE=${PROJECT_SOURCE_DIR}/cmake/llvm_opt.cmake"
                     "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/llvm-opt-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/llvm_opt.cmake")
        endif()
    endif()
    if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
        include("${PROJECT_SOURCE_DIR}/cmake/shaderdec.cmake")
        add_test(NAME shaderdec_spec COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/shaderdec_test.py"
            $<TARGET_FILE:snx-shaderdec> "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/maxwell_opcodes.def")
    endif()
    add_executable(snx-test-vulkan-release vulkan_release.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/pending_pages.c")
    snx_test_target(snx-test-vulkan-release 11)
    target_link_libraries(snx-test-vulkan-release PRIVATE scarletnx-common) # release.c's snx_os_error
    add_test(NAME vulkan_release COMMAND snx-test-vulkan-release)
    add_executable(snx-test-vulkan-backing vulkan_backing.c)
    snx_test_target(snx-test-vulkan-backing 11)
    add_test(NAME vulkan_backing COMMAND snx-test-vulkan-backing)
endif()
# these recompile with --repo of the checkout, which keeps the code units (nx/loader/unit_cache.c) in the user's cache
# folder: a folder of the build tree's instead, where XDG names it
foreach(snx_test native_recompiler llvm_module nx_runtime_homebrew nx_runtime_homebrew_64k_pages)
    if(TEST ${snx_test})
        set_property(TEST ${snx_test} APPEND PROPERTY ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg-cache")
    endif()
endforeach()
snx_sanitizer_path()
