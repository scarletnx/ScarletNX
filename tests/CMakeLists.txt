# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
option(SNX_TEST_SANITIZERS
       "Build the native regression tests, and the in-tree libraries they exercise, with ASan and UBSan (a test build: \
the runtime links those libraries too, and isn't deployed)" OFF)
option(SNX_TEST_VULKAN "Run offscreen Vulkan integration tests (requires a Vulkan 1.3 ICD, e.g. lavapipe)" OFF)
option(SNX_TEST_ONLINE_LIVE
       "Ask each WFC service's DNS server over the internet (online_presets_live, ctest -L network)" OFF)
option(SNX_TEST_ARM_AOT "Build asset-free static ARM11/checked-memory witnesses (requires a native build of the 3DS)" OFF)
option(SNX_TEST_VP9_VECTORS
       "Run NVDEC's VP9 jobs on libvpx's test vectors (configure downloads 32 files from the WebM project)" OFF)
if(SNX_TEST_VP9_VECTORS AND SNX_TOOLS_ONLY)
    message(FATAL_ERROR "SNX_TEST_VP9_VECTORS=ON needs the runtime's FFmpeg, which SNX_TOOLS_ONLY=ON leaves out")
endif()
option(SNX_TEST_AV1_VECTORS
       "Run the native video service's AV1 decoder on AOMedia's test vectors (configure downloads 8 files from it)" OFF)
if(SNX_TEST_AV1_VECTORS AND NOT SNX_TEST_VULKAN)
    message(FATAL_ERROR "SNX_TEST_AV1_VECTORS=ON needs SNX_TEST_VULKAN=ON too: the offscreen Vulkan test runs them "
                        "(vulkan_video_av1)")
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
        # the common code: the platform layer, network, filesystems, LZ4, ZBIC and xxHash, the Vulkan setup the
        # frontends share, the GL layer's state, the launcher's HTTPS and the roots it carries, the zip reader of VPKs
        # and APKs, the glTF reader of a Mii's parts, the DS's and the 3DS's online services, the Bluetooth LE scan and
        # its system backend
        scarletnx-common scarletnx-platform scarletnx-net scarletnx-ldn scarletnx-filesystem snx_demangle snx_lz4
        snx_zbic snx_xxhash snx_vk_common snx_gl snx_https snx_cacert snx_zip snx_cgltf snx_online snx_ble
        snx_ble_system
        snx-logscan
        # the recompiler, its backends and what they take of the common code
        snx_recompiler_base snx_llvm snx_arm_llvm snx_arm_decode snx_arm_rt scarletnx-recompiler
        scarletnx-recompiler-shared
        # the driver, its extraction, the first start's setup, the settings and the build's line
        snx_driver snx_nx_module snx_driver_llvm hactool_lib snx_setup snx_settings snx_settings_runtime snx_build_info
        snx-test-build-info
        # the Switch runtime: its components, the shader translator, the Vulkan backend and its upscalers
        scarletnx snx_core snx_kernel snx_hle snx_video snx_gpu snx_platform snx_shader snx_vk snx_fsr2 snx_upscaler
        # the GNU ABI runtime and its kernel
        scarletnx-gnuabi scarletnx-gnuabi-kernel
        # the Android app driver: its DEX translator, its program and the native bridge
        snx_android_dex snx_android_program snx_android_bridge
        # the 3DS runtime
        scarletnx-3ds snx_ctr_program snx_ctr_board snx_ctr_memory snx_ctr_loader snx_ctr_cpu snx_ctr_process snx_ctr_native
        snx_ctr_audio snx_ctr_gpu snx_ctr_gpu_reference
        # the Vita runtime, its loader and USSE translator
        scarletnx-vita snx_vita_program snx_vita_loader snx_vita_usse snx-vita-usse
        # the GBA board and its runner
        scarletnx-agb snx_agb_program
        # the DS board, its picture on the GPU and its runner
        scarletnx-nds snx_nds_gpu snx_nds_program
        # the Switch driver's entry, which scarletnx runs as it runs the others
        snx_switch_program
        # the menu, its splash, icon and pads
        scarletnx-gui snx_gui_support snx_gui_ui snx_i18n snx_gui_languages snx_gui_languages_test snx_splash snx-test-splash
        snx_window_icon snx-test-window-icon snx_pad_sdl snx_gui_pad
        # the notice of the 3DS's, the Vita's and the GBA's windows
        snx_notice
        # the on-screen controls, their consoles' tables and their side of an SDL window, and the settings each
        # console takes
        snx_vpad snx_vpad_consoles snx_vpad_sdl snx_console_settings
        # the save states' file, every console's
        snx_state_file
        # the shader corpus translator, which shaderc_sm86 runs
        snx-shaderc
        # GLSL ES into SPIR-V
        snx_glslc snx-glslc-cli snx-test-glslc-lex snx-test-glslc-parse snx-test-glslc-emit snx-test-glslc-fuzz
        # the code generators, which tests run on files they refuse, and the one of SMAA's textures
        snx-swipc snx-svcgen snx-hookgen snx-settingsgen snx-uigen snx-decodetree snx-vkcmdgen snx-sailgen
        snx-areatexgen)
endif()
# one Python for the scripted tests: without it they are left out
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME cpu_plan_tools COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cpu_plan_tools.py")
endif()
# each stop of the Switch runtime that the person playing can do something about names its kind of error, whose panel
# offers the action that fixes it (src/common/utilities/error_kind.h)
if(Python3_Interpreter_FOUND)
    add_test(NAME error_kinds COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/error_kinds.py"
                                      "${PROJECT_SOURCE_DIR}")
endif()
# the GNU extensions with an ISO C counterpart stay out of our own sources (#68)
if(Python3_Interpreter_FOUND)
    add_test(NAME gnu_extensions COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gnu_extensions.py"
                                         --repo "${PROJECT_SOURCE_DIR}")
endif()
# every C source of src/ compiled with -Werror=atomic-implicit-seq-cst, no test's own (cmake/atomic_explicit.cmake)
if(Python3_Interpreter_FOUND AND CMAKE_EXPORT_COMPILE_COMMANDS)
    add_test(NAME atomic_explicit COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/atomic_explicit_test.py"
                                          "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}")
endif()
# each native hook of a mod's trampoline (src/nx/nn/ext_skyline.hooks) as the function's own native hook
if(Python3_Interpreter_FOUND)
    add_test(NAME ext_skyline_hooks COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ext_skyline_hooks.py")
endif()
# every RtAPI a test makes carries cas128, which an STXP of doublewords calls on RISC-V, LoongArch, MIPS64 and
# PowerPC64 before POWER8 only
if(Python3_Interpreter_FOUND)
    add_test(NAME rtapi_cas128 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rtapi_cas128.py"
                                       --repo "${PROJECT_SOURCE_DIR}")
endif()
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
    # snx_test_target already links snx_recompiler_base; the service files' plots use its host clock
    snx_swipc(${target} ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc ${ARGN})
endfunction()

# a test's checks are asserts too: -UNDEBUG keeps them in any build type. dialect is c23, strict ISO C23 without
# extensions (-pedantic-errors), or gnu23, C23 with GNU's. A C standard number, from before every test built as C23,
# still reads (99 as c23, 11 and 23 as gnu23), with a warning naming the dialect to write instead
function(snx_test_target target dialect)
    if(dialect MATCHES "^(99|11|23)$")
        set(given ${dialect})
        set(dialect gnu23)
        if(given EQUAL 99)
            set(dialect c23)
        endif()
        message(AUTHOR_WARNING "snx_test_target(${target} ${given}): every test builds as C23, so ${given} reads as "
                               "${dialect}: write ${dialect}")
    endif()
    if(dialect STREQUAL "c23")
        set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
        target_compile_options(${target} PRIVATE -pedantic-errors)
    elseif(dialect STREQUAL "gnu23")
        set_target_properties(${target} PROPERTIES C_EXTENSIONS ON)
    else()
        message(FATAL_ERROR "snx_test_target(${target} ${dialect}): ${dialect} is not c23 (strict ISO C23, "
                            "-pedantic-errors) or gnu23 (C23 with GNU's extensions)")
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads snx_recompiler_base)
    if(SNX_TRACY AND NOT target STREQUAL snx-test-tracy-state)
        # A test may compile a runtime source that emits zones. Give every target the stand-in and the client; a
        # target that does not see SNX_TRACY compiles the stub to an empty object.
        set(_snx_test_stubs "${PROJECT_SOURCE_DIR}/tests/tracy_stubs.c")
        get_target_property(_snx_test_sources ${target} SOURCES)
        if(NOT "tracy_stubs.c" IN_LIST _snx_test_sources AND NOT _snx_test_stubs IN_LIST _snx_test_sources)
            target_sources(${target} PRIVATE "${_snx_test_stubs}")
            target_include_directories(${target} SYSTEM PRIVATE
                $<TARGET_PROPERTY:Tracy::TracyClient,INTERFACE_INCLUDE_DIRECTORIES>)
        endif()
        target_link_libraries(${target} PRIVATE Tracy::TracyClient)
    endif()
    if(TARGET snx_settings) # g_settings, its header, and the tables a test uses (cmake/settings.cmake)
        target_link_libraries(${target} PRIVATE snx_settings_runtime snx_settings)
    endif()
    snx_test_sanitize(${target})
endfunction()

if(TARGET snx_ctr_board)
    add_executable(snx-test-3ds-board 3ds_board.c)
    snx_test_target(snx-test-3ds-board gnu23)
    target_link_libraries(snx-test-3ds-board PRIVATE snx_ctr_board)
    add_test(NAME 3ds_board COMMAND snx-test-3ds-board)
    # the game's clock, which fps_limit paces
    add_executable(snx-test-3ds-clock 3ds_clock.c)
    snx_test_target(snx-test-3ds-clock gnu23)
    target_link_libraries(snx-test-3ds-clock PRIVATE snx_ctr_board m)
    add_test(NAME 3ds_clock COMMAND snx-test-3ds-clock)
    # the SMDH's short title and its 48x48 icon, which the launcher shows the game by
    add_executable(snx-test-3ds-smdh 3ds_smdh.c)
    snx_test_target(snx-test-3ds-smdh gnu23)
    target_link_libraries(snx-test-3ds-smdh PRIVATE snx_ctr_loader)
    add_test(NAME 3ds_smdh COMMAND snx-test-3ds-smdh)
    add_executable(snx-test-3ds-memory 3ds_memory.c)
    snx_test_target(snx-test-3ds-memory gnu23)
        target_link_libraries(snx-test-3ds-memory PRIVATE snx_ctr_memory)
    add_test(NAME 3ds_memory COMMAND snx-test-3ds-memory)
    add_executable(snx-test-3ds-memory-track 3ds_memory_track.c)
    snx_test_target(snx-test-3ds-memory-track gnu23)
    target_link_libraries(snx-test-3ds-memory-track PRIVATE snx_ctr_memory)
    add_test(NAME 3ds_memory_track COMMAND snx-test-3ds-memory-track)
    # the RAM moved into another object, as the GPU's device does when its driver will not import it: every view goes
    add_executable(snx-test-3ds-memory-adopt 3ds_memory_adopt.c)
    snx_test_target(snx-test-3ds-memory-adopt gnu23)
    target_link_libraries(snx-test-3ds-memory-adopt PRIVATE snx_ctr_memory)
    add_test(NAME 3ds_memory_adopt COMMAND snx-test-3ds-memory-adopt)
    set_tests_properties(3ds_memory_adopt PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(snx-test-3ds-kernel 3ds_kernel.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-kernel gnu23)
    target_link_libraries(snx-test-3ds-kernel PRIVATE snx_ctr_process)
    add_test(NAME 3ds_kernel COMMAND snx-test-3ds-kernel)
    add_executable(snx-test-3ds-vfp 3ds_vfp.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-vfp gnu23)
    target_link_libraries(snx-test-3ds-vfp PRIVATE snx_ctr_process snx_llvm)
    add_test(NAME 3ds_vfp COMMAND snx-test-3ds-vfp)
    foreach(test services swkbd mii_selector threads)
        add_executable(snx-test-3ds-${test} 3ds_${test}.c support/3ds_guest.c)
        snx_test_target(snx-test-3ds-${test} gnu23)
        target_link_libraries(snx-test-3ds-${test} PRIVATE snx_ctr_process)
        add_test(NAME 3ds_${test} COMMAND snx-test-3ds-${test})
    endforeach()
    add_executable(snx-test-3ds-cro 3ds_cro.c support/3ds_cro.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-cro gnu23)
    target_link_libraries(snx-test-3ds-cro PRIVATE snx_ctr_process)
    foreach(case fix handle shutdown overlap bss bounds target batch rollback data reuse crs-internal bss-source
            data-source lifetime link late-failure metadata fix0 fix1 fix2 fix3 no-code relative-data private-data
            pid-descriptor segment-overlap file-size name static-bound static-no-jit indexed anonymous copied-process
            old-command crs-empty-segment crs-repeat-segment crs-unmapped-segment crs-unmapped-relocation same-buffer
            same-buffer-zero-data code-range)
        add_test(NAME 3ds_cro_${case} COMMAND snx-test-3ds-cro ${case})
        add_test(NAME 3ds_cro_new_${case} COMMAND snx-test-3ds-cro ${case} new)
    endforeach()
    add_executable(snx-test-3ds-cro-boundaries 3ds_cro_boundaries.c support/3ds_cro.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-cro-boundaries gnu23)
    target_link_libraries(snx-test-3ds-cro-boundaries PRIVATE snx_ctr_process)
    foreach(case name-62 name-63 name-64 name-254 name-255 name-256 module-256 missing-null-256 exit-shim batch-100000
            batch-100001 exports-100000 exports-100001)
        foreach(command 4 9)
            foreach(model old new)
                add_test(NAME 3ds_cro_boundary_${case}_${command}_${model}
                    COMMAND snx-test-3ds-cro-boundaries ${case} ${command} ${model})
            endforeach()
        endforeach()
    endforeach()
    if(TARGET snx_llvm)
        add_executable(snx-test-3ds-cro-jit 3ds_cro_jit.c support/3ds_cro.c support/3ds_guest.c)
        snx_test_target(snx-test-3ds-cro-jit gnu23)
        target_link_libraries(snx-test-3ds-cro-jit PRIVATE snx_ctr_program)
        foreach(case arm thumb static static-window cycles cycles-thumb svc-unload)
            add_test(NAME 3ds_cro_jit_${case} COMMAND snx-test-3ds-cro-jit ${case})
            add_test(NAME 3ds_cro_jit_new_${case} COMMAND snx-test-3ds-cro-jit ${case} new)
            set_tests_properties(3ds_cro_jit_${case} 3ds_cro_jit_new_${case} PROPERTIES SKIP_RETURN_CODE 77)
        endforeach()
    endif()
    add_executable(snx-test-3ds-ipc 3ds_ipc.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-ipc gnu23)
    target_link_libraries(snx-test-3ds-ipc PRIVATE snx_ctr_process)
    add_test(NAME 3ds_ipc COMMAND snx-test-3ds-ipc)
    # the SDK's IPC stubs run without the SVC (aot/hook.c)
    add_executable(snx-test-3ds-hook 3ds_hook.c support/3ds_guest.c support/3ds_stub.c)
    snx_test_target(snx-test-3ds-hook gnu23)
    target_link_libraries(snx-test-3ds-hook PRIVATE snx_ctr_process)
    add_test(NAME 3ds_hook COMMAND snx-test-3ds-hook)
    add_executable(snx-test-3ds-hid 3ds_hid.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-hid gnu23)
    target_link_libraries(snx-test-3ds-hid PRIVATE snx_ctr_process)
    add_test(NAME 3ds_hid COMMAND snx-test-3ds-hid)
    # the gamepads as a 3DS game reads them, on SDL's virtual pads
    add_executable(snx-test-3ds-input 3ds_input.c)
    snx_test_target(snx-test-3ds-input gnu23)
    target_link_libraries(snx-test-3ds-input PRIVATE snx_ctr_gpu m)
    add_test(NAME 3ds_input COMMAND snx-test-3ds-input)
    # the window's Esc, asked twice, as SDL sends it: no device or window
    add_executable(snx-test-3ds-quit-key 3ds_quit_key.c)
    snx_test_target(snx-test-3ds-quit-key gnu23)
    target_link_libraries(snx-test-3ds-quit-key PRIVATE snx_ctr_gpu)
    target_include_directories(snx-test-3ds-quit-key PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}") # support/
    add_test(NAME 3ds_quit_key COMMAND snx-test-3ds-quit-key)
    set_tests_properties(3ds_quit_key PROPERTIES SKIP_RETURN_CODE 77) # no window where SDL has no offscreen driver
    # ir:rst, the New 3DS's ZL, ZR and C-stick, from a virtual pad to its shared memory
    add_executable(snx-test-3ds-irrst 3ds_irrst.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-irrst gnu23)
    target_link_libraries(snx-test-3ds-irrst PRIVATE snx_ctr_process)
    add_test(NAME 3ds_irrst COMMAND snx-test-3ds-irrst)
    # hid:USER's accelerometer and gyroscope from a virtual pad's motion sensors
    add_executable(snx-test-3ds-motion 3ds_motion.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-motion gnu23)
    target_link_libraries(snx-test-3ds-motion PRIVATE snx_ctr_process)
    add_test(NAME 3ds_motion COMMAND snx-test-3ds-motion)
    add_executable(snx-test-3ds-ir 3ds_ir.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-ir gnu23)
    target_link_libraries(snx-test-3ds-ir PRIVATE snx_ctr_process)
    foreach(case startup permissions accessory lifecycle metadata)
        add_test(NAME 3ds_ir_${case} COMMAND snx-test-3ds-ir ${case})
    endforeach()
    # mic:u with no input device, as Azahar's
    add_executable(snx-test-3ds-mic 3ds_mic.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-mic gnu23)
    target_link_libraries(snx-test-3ds-mic PRIVATE snx_ctr_process)
    add_test(NAME 3ds_mic COMMAND snx-test-3ds-mic)
    # cam:u with Azahar's blank camera
    add_executable(snx-test-3ds-cam 3ds_cam.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-cam gnu23)
    target_link_libraries(snx-test-3ds-cam PRIVATE snx_ctr_process)
    add_test(NAME 3ds_cam COMMAND snx-test-3ds-cam)
    # csnd:SND and the CSND hardware, rendered and mixed into the output
    add_executable(snx-test-3ds-csnd 3ds_csnd.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-csnd gnu23)
    target_link_libraries(snx-test-3ds-csnd PRIVATE snx_ctr_process)
    add_test(NAME 3ds_csnd COMMAND snx-test-3ds-csnd)
    # the pointer's touch on the window's bottom screen, from the presenter to hid:USER's samples
    add_executable(snx-test-3ds-mouse-touch 3ds_mouse_touch.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-mouse-touch gnu23)
    target_link_libraries(snx-test-3ds-mouse-touch PRIVATE snx_ctr_process)
    add_test(NAME 3ds_mouse_touch COMMAND snx-test-3ds-mouse-touch)
    set_tests_properties(3ds_mouse_touch PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(snx-test-ctr-swipc ctr_swipc.c)
    if(NOT WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        snx_test_target(snx-test-ctr-swipc c23)
            else()
        snx_test_target(snx-test-ctr-swipc gnu23)
    endif()
    snx_swipc(snx-test-ctr-swipc CTR ROOT "${PROJECT_SOURCE_DIR}" FILES tests/ctr_swipc.swipc)
    add_test(NAME ctr_swipc_dispatch COMMAND snx-test-ctr-swipc)
    add_executable(snx-test-3ds-process 3ds_process.c support/3ds_stub.c)
    snx_test_target(snx-test-3ds-process gnu23)
    target_link_libraries(snx-test-3ds-process PRIVATE snx_ctr_process)
    add_test(NAME 3ds_process COMMAND snx-test-3ds-process)
    add_executable(snx-test-3ds-discover 3ds_discover.c "${PROJECT_SOURCE_DIR}/src/3ds/aot/discover.c")
    snx_test_target(snx-test-3ds-discover gnu23)
    target_link_libraries(snx-test-3ds-discover PRIVATE snx_arm_decode)
    add_test(NAME 3ds_static_discovery COMMAND snx-test-3ds-discover)
    add_executable(snx-test-3ds-native 3ds_native.c)
    snx_test_target(snx-test-3ds-native gnu23)
    target_link_libraries(snx-test-3ds-native PRIVATE snx_ctr_native)
    add_executable(snx-test-3ds-blz 3ds_blz.c)
    snx_test_target(snx-test-3ds-blz gnu23)
    target_link_libraries(snx-test-3ds-blz PRIVATE snx_ctr_loader)
    add_test(NAME 3ds_blz COMMAND snx-test-3ds-blz)
    # snx_seek and snx_tell on a sparse file, at an offset past 2 GiB
    add_executable(snx-test-3ds-file-position 3ds_file_position.c support/sparse_file.c)
    snx_test_target(snx-test-3ds-file-position gnu23)
    target_link_libraries(snx-test-3ds-file-position PRIVATE snx_ctr_loader)
    add_test(NAME 3ds_file_position COMMAND snx-test-3ds-file-position)
    set_tests_properties(3ds_file_position PROPERTIES SKIP_RETURN_CODE 77)
    # the CIA input's 4 GiB check and the extraction budget, on sparse files: no gigabyte is read or written
    add_executable(snx-test-3ds-loader-limit 3ds_loader_limit.c support/sparse_file.c)
    snx_test_target(snx-test-3ds-loader-limit gnu23)
    target_link_libraries(snx-test-3ds-loader-limit PRIVATE snx_ctr_loader)
    add_test(NAME 3ds_loader_limit COMMAND snx-test-3ds-loader-limit)
    set_tests_properties(3ds_loader_limit PROPERTIES SKIP_RETURN_CODE 77)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        # the level-3 view's bound, on a mapped sparse image of its size: 4 GiB + 1 byte is refused, 4 GiB is read
        add_executable(snx-test-3ds-romfs-limit 3ds_romfs_limit.c support/sparse_file.c)
        snx_test_target(snx-test-3ds-romfs-limit gnu23)
        target_link_libraries(snx-test-3ds-romfs-limit PRIVATE snx_ctr_process)
        add_test(NAME 3ds_romfs_limit COMMAND snx-test-3ds-romfs-limit)
        set_tests_properties(3ds_romfs_limit PROPERTIES SKIP_RETURN_CODE 77)
        # a RomFS built from a host file whose data lies past 2 GiB, read through the on-demand path (fd < 0)
        add_executable(snx-test-3ds-romfs-large 3ds_romfs_large.c support/sparse_file.c)
        snx_test_target(snx-test-3ds-romfs-large gnu23)
        target_link_libraries(snx-test-3ds-romfs-large PRIVATE snx_ctr_process)
        add_test(NAME 3ds_romfs_large COMMAND snx-test-3ds-romfs-large)
        set_tests_properties(3ds_romfs_large PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()

    # The GPU, presentation and DSP tests need neither Python nor POSIX, and Windows is a 3DS target too
    # the GPU's transfer engine against the CPU reference (gpu/transfer.c), on whatever Vulkan device there is
    add_executable(snx-test-3ds-vk-transfer 3ds_vk_transfer.c)
    snx_test_target(snx-test-3ds-vk-transfer gnu23)
    target_link_libraries(snx-test-3ds-vk-transfer PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_transfer COMMAND snx-test-3ds-vk-transfer)
    set_tests_properties(3ds_vk_transfer PROPERTIES SKIP_RETURN_CODE 77)
    # what a RAM region's import that the driver refuses says (the call, its VkResult, the region and the device), and
    # what the device's own memory says when it cannot hold the RAM either
    add_executable(snx-test-3ds-vk-import-error 3ds_vk_import_error.c)
    snx_test_target(snx-test-3ds-vk-import-error gnu23)
    target_link_libraries(snx-test-3ds-vk-import-error PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_import_error COMMAND snx-test-3ds-vk-import-error)
    # vk_debug_hide_commands on the 3DS's device: VK_EXT_shader_object's vkCreateShadersEXT taken as missing, the
    # device made again without the extension and the log saying so (common/gpu/vk/extension_commands.c's
    # snx_vk_device_commands). The log is the device's stderr, read back through dup2
    if(NOT WIN32)
        add_executable(snx-test-3ds-vk-hidden-commands 3ds_vk_hidden_commands.c)
        snx_test_target(snx-test-3ds-vk-hidden-commands gnu23)
        target_link_libraries(snx-test-3ds-vk-hidden-commands PRIVATE snx_ctr_gpu)
        add_test(NAME 3ds_vk_hidden_commands COMMAND snx-test-3ds-vk-hidden-commands)
        set_tests_properties(3ds_vk_hidden_commands PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
    # a renderer's background compiles (common/gpu/vk/jobs.c): the workers, each job made and taken once, the hold, the stop
    add_executable(snx-test-vk-jobs vk_jobs.c "${volk_SOURCE_DIR}/volk.c")
    set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
    snx_test_target(snx-test-vk-jobs gnu23)
    target_link_libraries(snx-test-vk-jobs PRIVATE snx_vk_common)
    add_test(NAME vk_jobs COMMAND snx-test-vk-jobs)
    set_tests_properties(vk_jobs PROPERTIES TIMEOUT 60)
    # vk_gpu_profile and vk_checkpoints in a console's renderer (common/gpu/vk/marks.c), on fakes of the driver's calls
    add_executable(snx-test-vk-marks vk_marks.c "${volk_SOURCE_DIR}/volk.c")
    set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
    snx_test_target(snx-test-vk-marks gnu23)
    target_link_libraries(snx-test-vk-marks PRIVATE snx_vk_common)
    add_test(NAME vk_marks COMMAND snx-test-vk-marks)
    # frame_pacing in a console's window: each present's id, the wait for the one before (common/gpu/vk/pacing.c)
    add_executable(snx-test-vk-pacing vk_pacing.c "${volk_SOURCE_DIR}/volk.c")
    set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
    snx_test_target(snx-test-vk-pacing gnu23)
    target_link_libraries(snx-test-vk-pacing PRIVATE snx_vk_common)
    add_test(NAME vk_pacing COMMAND snx-test-vk-pacing)
    # the samplers' anisotropy for the Switch's setting, and what the run says of it (common/gpu/vk/vk_common.c, which
    # calls volk's pointers)
    add_executable(snx-test-vk-anisotropy vk_anisotropy.c "${volk_SOURCE_DIR}/volk.c")
    set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
    snx_test_target(snx-test-vk-anisotropy gnu23)
    target_link_libraries(snx-test-vk-anisotropy PRIVATE snx_vk_common)
    add_test(NAME vk_anisotropy COMMAND snx-test-vk-anisotropy)
    # the RAM as the GPU has it, imported or the device's own memory: the guest's bytes and the GPU's are one memory,
    # in every view, before, during and after the device
    add_executable(snx-test-3ds-vk-ram 3ds_vk_ram.c)
    snx_test_target(snx-test-3ds-vk-ram gnu23)
    target_link_libraries(snx-test-3ds-vk-ram PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_ram COMMAND snx-test-3ds-vk-ram)
    set_tests_properties(3ds_vk_ram PROPERTIES SKIP_RETURN_CODE 77)
    # the Y2R engine's conversion on the GPU against its CPU reference
    add_executable(snx-test-3ds-vk-y2r 3ds_vk_y2r.c)
    snx_test_target(snx-test-3ds-vk-y2r gnu23)
    target_link_libraries(snx-test-3ds-vk-y2r PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_y2r COMMAND snx-test-3ds-vk-y2r)
    set_tests_properties(3ds_vk_y2r PROPERTIES SKIP_RETURN_CODE 77)
    # the pipeline cache kept between runs: an empty one without a file, a written one read back whole, a cut file,
    # no temporary file left, and the renderer's file in the folder's shader_cache
    add_executable(snx-test-3ds-vk-pipeline-cache 3ds_vk_pipeline_cache.c)
    snx_test_target(snx-test-3ds-vk-pipeline-cache gnu23)
    target_link_libraries(snx-test-3ds-vk-pipeline-cache PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_pipeline_cache COMMAND snx-test-3ds-vk-pipeline-cache
                                                "${CMAKE_CURRENT_BINARY_DIR}/pipeline-cache")
    set_tests_properties(3ds_vk_pipeline_cache PROPERTIES SKIP_RETURN_CODE 77)
    # the shader objects' binaries kept between runs: a compiled shader's binary written, read back whole and made
    # again with no compile; another device's file, a cut file, no temporary file left, and the renderer's folder
    add_executable(snx-test-3ds-vk-shader-object 3ds_vk_shader_object.c)
    snx_test_target(snx-test-3ds-vk-shader-object gnu23)
    target_link_libraries(snx-test-3ds-vk-shader-object PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_shader_object COMMAND snx-test-3ds-vk-shader-object
                                               "${CMAKE_CURRENT_BINARY_DIR}/shader-object")
    set_tests_properties(3ds_vk_shader_object PROPERTIES SKIP_RETURN_CODE 77)
    # the GPU's draws against the software renderer, the CPU reference they replaced, scene by scene
    add_executable(snx-test-3ds-vk-draw 3ds_vk_draw.c)
    snx_test_target(snx-test-3ds-vk-draw gnu23)
    target_link_libraries(snx-test-3ds-vk-draw PRIVATE snx_ctr_gpu_reference)
    add_test(NAME 3ds_vk_draw COMMAND snx-test-3ds-vk-draw)
    # async_pipelines: the draw skipped while its pipeline is made on a worker, then drawn
    add_test(NAME 3ds_vk_draw_async COMMAND snx-test-3ds-vk-draw --async)
    # the same two through pipelines, where the device would draw with shader objects (VK_EXT_shader_object)
    add_test(NAME 3ds_vk_draw_pipelines COMMAND snx-test-3ds-vk-draw --vk_ext_shader_object=off)
    add_test(NAME 3ds_vk_draw_async_pipelines COMMAND snx-test-3ds-vk-draw --async --vk_ext_shader_object=off)
    # the pipeline cache kept between runs: the file named for the driver, and a failed write made again by the next.
    # Through pipelines, as a device with shader objects makes none (3ds_vk_shader_object checks their binaries)
    add_test(NAME 3ds_vk_draw_pipeline_cache COMMAND snx-test-3ds-vk-draw --pipeline-cache --vk_ext_shader_object=off)
    # the stand-in shaders (shaders/standin.vert and standin.frag) given every draw they draw, against the CPU
    # reference
    add_test(NAME 3ds_vk_draw_stand_in COMMAND snx-test-3ds-vk-draw --ctr_stand_in=2)
    # the presented screens outside the validation harness, so every host runs that path
    add_test(NAME 3ds_vk_draw_present COMMAND snx-test-3ds-vk-draw --present)
    # a frame of GSP's RGB565 and RGB5A1 formats drawn at render scale 2 shows its target's image, as an RGBA8 one does
    add_test(NAME 3ds_vk_draw_scaled_present COMMAND snx-test-3ds-vk-draw --scaled-present)
    if(NOT WIN32 AND Python3_Interpreter_FOUND)
        # the presenter's read of a scaled target, under Khronos validation
        add_test(NAME 3ds_vk_draw_scaled_present_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-present)
        set_tests_properties(3ds_vk_draw_scaled_present_validation PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    # a target at a render scale and a display transfer's copy of it, which the presenter shows for a framebuffer in
    # the destination
    add_test(NAME 3ds_vk_draw_scaled_transfer COMMAND snx-test-3ds-vk-draw --scaled-transfer)
    # two destinations a screen alternates between: the copy the presenter looks up is the destination's own, and a
    # write over one drops its copy alone
    add_test(NAME 3ds_vk_draw_scaled_rotating COMMAND snx-test-3ds-vk-draw --scaled-rotating)
    # a destination the CPU writes after the transfer: the presenter compares the memory's bytes with the copy's
    add_test(NAME 3ds_vk_draw_scaled_verify COMMAND snx-test-3ds-vk-draw --scaled-verify)
    set_tests_properties(3ds_vk_draw_scaled_transfer 3ds_vk_draw_scaled_rotating 3ds_vk_draw_scaled_verify
                         PROPERTIES SKIP_RETURN_CODE 77)
    # a texture made from a scaled target keeps the target's scale: a second target at the same scale samples its image
    add_test(NAME 3ds_vk_draw_scaled_texture_from_target COMMAND snx-test-3ds-vk-draw --scaled-texture-from-target)
    set_tests_properties(3ds_vk_draw_scaled_texture_from_target PROPERTIES SKIP_RETURN_CODE 77)
    # a texture made from a scaled depth buffer keeps the scale: a second target at the same scale samples it
    add_test(NAME 3ds_vk_draw_scaled_texture_from_depth COMMAND snx-test-3ds-vk-draw --scaled-texture-from-depth)
    set_tests_properties(3ds_vk_draw_scaled_texture_from_depth PROPERTIES SKIP_RETURN_CODE 77)
    # two targets over one depth buffer at the scale: the second takes the first's depth, not the 1x reload
    add_test(NAME 3ds_vk_draw_scaled_shared_depth COMMAND snx-test-3ds-vk-draw --scaled-shared-depth)
    set_tests_properties(3ds_vk_draw_scaled_shared_depth PROPERTIES SKIP_RETURN_CODE 77)
    # a display transfer of a scaled target into a destination target: the destination keeps the render scale, and a
    # target at the same scale samples it. At scale 1 the destination is the transfer's own result
    add_test(NAME 3ds_vk_draw_scaled_transfer_into_target COMMAND snx-test-3ds-vk-draw --scaled-transfer-into-target)
    set_tests_properties(3ds_vk_draw_scaled_transfer_into_target PROPERTIES SKIP_RETURN_CODE 77)
    # shadow maps drawn and read at the render scale, plus flipped 1x fallback and centre-texel store
    add_test(NAME 3ds_vk_draw_shadow_scaled COMMAND snx-test-3ds-vk-draw --shadow-scaled)
    set_tests_properties(3ds_vk_draw_shadow_scaled PROPERTIES SKIP_RETURN_CODE 77)
    # a display transfer into a destination over a scaled target's first rows, and no row it reads: the copy is kept,
    # and a draw into the target after it is checked the same way
    add_test(NAME 3ds_vk_draw_scaled_transfer_overlap COMMAND snx-test-3ds-vk-draw --scaled-transfer-overlap)
    set_tests_properties(3ds_vk_draw_scaled_transfer_overlap PROPERTIES SKIP_RETURN_CODE 77)
    # the render scale of a target that is not a screen's size: the larger setting, or the top's for a 400-row target
    add_test(NAME 3ds_vk_draw_scaled_other_sizes COMMAND snx-test-3ds-vk-draw --scaled-other-sizes)
    set_tests_properties(3ds_vk_draw_scaled_other_sizes PROPERTIES SKIP_RETURN_CODE 77)
    # the screens laid out at the console's proportions whatever each render scale: a top screen at 2x above a bottom
    # one at 1x, and a press on the bottom screen's pixel (160, 220) landing on it
    add_test(NAME 3ds_vk_draw_scaled_layout COMMAND snx-test-3ds-vk-draw --scaled-layout)
    set_tests_properties(3ds_vk_draw_scaled_layout PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
    # the same through FSR 1 and FXAA: each screen's last pass drawn into the window's rendering
    add_test(NAME 3ds_vk_draw_scaled_layout_effects COMMAND snx-test-3ds-vk-draw --scaled-layout-effects)
    set_tests_properties(3ds_vk_draw_scaled_layout_effects PROPERTIES SKIP_RETURN_CODE 77
                         ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
    if(NOT WIN32 AND Python3_Interpreter_FOUND)
        # the copies' blits, lookups and compare, under Khronos validation
        add_test(NAME 3ds_vk_draw_scaled_transfer_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-transfer)
        add_test(NAME 3ds_vk_draw_scaled_rotating_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-rotating)
        add_test(NAME 3ds_vk_draw_scaled_verify_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-verify)
        add_test(NAME 3ds_vk_draw_scaled_texture_from_target_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-texture-from-target)
        add_test(NAME 3ds_vk_draw_scaled_texture_from_depth_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-texture-from-depth)
        add_test(NAME 3ds_vk_draw_scaled_shared_depth_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-shared-depth)
        add_test(NAME 3ds_vk_draw_scaled_transfer_into_target_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-transfer-into-target)
        add_test(NAME 3ds_vk_draw_scaled_transfer_overlap_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-transfer-overlap)
        add_test(NAME 3ds_vk_draw_scaled_other_sizes_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-vk-draw> --scaled-other-sizes)
        set_tests_properties(3ds_vk_draw_scaled_transfer_validation 3ds_vk_draw_scaled_rotating_validation
                             3ds_vk_draw_scaled_verify_validation PROPERTIES SKIP_RETURN_CODE 77)
        set_tests_properties(3ds_vk_draw_scaled_texture_from_target_validation
                             3ds_vk_draw_scaled_texture_from_depth_validation
                             3ds_vk_draw_scaled_shared_depth_validation
                             3ds_vk_draw_scaled_transfer_into_target_validation
                             3ds_vk_draw_scaled_transfer_overlap_validation
                             3ds_vk_draw_scaled_other_sizes_validation PROPERTIES SKIP_RETURN_CODE 77)
        # the window's rendering, with the screens drawn by a copy and through the effects, under Khronos validation
        foreach(snx_mode scaled-layout scaled-layout-effects)
            string(REPLACE "-" "_" snx_name "${snx_mode}")
            add_test(NAME 3ds_vk_draw_${snx_name}_validation
                     COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                         $<TARGET_FILE:snx-test-3ds-vk-draw> --${snx_mode})
            set_tests_properties(3ds_vk_draw_${snx_name}_validation PROPERTIES SKIP_RETURN_CODE 77
                                 ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
        endforeach()
    endif()
    # the scaled modes again on each path the GPU's work can take to the RAM (vk.h)
    foreach(snx_path copy in_place)
        set(snx_value 1)
        if(snx_path STREQUAL "in_place")
            set(snx_value 2)
        endif()
        set(snx_tests "")
        foreach(snx_mode scaled-present scaled-transfer scaled-rotating scaled-verify)
            string(REPLACE "-" "_" snx_name "${snx_mode}")
            add_test(NAME 3ds_vk_draw_${snx_name}_${snx_path}
                     COMMAND snx-test-3ds-vk-draw --${snx_mode} --vk_memory=${snx_value})
            list(APPEND snx_tests 3ds_vk_draw_${snx_name}_${snx_path})
        endforeach()
        set_tests_properties(${snx_tests} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    set_tests_properties(3ds_vk_draw 3ds_vk_draw_async 3ds_vk_draw_pipelines 3ds_vk_draw_async_pipelines
                         3ds_vk_draw_pipeline_cache 3ds_vk_draw_stand_in 3ds_vk_draw_present 3ds_vk_draw_scaled_present
                         PROPERTIES SKIP_RETURN_CODE 77)
    # the GPU's copy of the RAM, page by page: what is copied in and back, and when, and every byte where it belongs
    add_executable(snx-test-3ds-vk-copies 3ds_vk_copies.c)
    snx_test_target(snx-test-3ds-vk-copies gnu23)
    target_link_libraries(snx-test-3ds-vk-copies PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_vk_copies COMMAND snx-test-3ds-vk-copies)
    set_tests_properties(3ds_vk_copies PROPERTIES SKIP_RETURN_CODE 77)
    if(NOT WIN32 AND Python3_Interpreter_FOUND)
        # the draw test's modes that record only the commands under test, run under Khronos validation: any error it
        # reports fails the test, such as a missing barrier (a synchronization hazard), a dispatch, barrier or image
        # transition inside a draw's open rendering, or a command recorded into a buffer that a submission ended
        set(snx_vk_validation_tests 3ds_vk_draw_present_validation 3ds_vk_draw_after_draw_validation
            3ds_vk_draw_texture_after_draw_validation 3ds_vk_draw_index_copy_validation
            3ds_vk_draw_after_submit_validation 3ds_vk_draw_async_validation
            3ds_vk_draw_shadow_validation 3ds_vk_draw_shadow_scaled_validation)
        set(snx_vk_validation_modes --present --after-draw --texture-after-draw --draw-after-index-copy
            --draw-after-submit --async --shadow --shadow-scaled)
        foreach(snx_test snx_mode IN ZIP_LISTS snx_vk_validation_tests snx_vk_validation_modes)
            add_test(NAME ${snx_test}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                    $<TARGET_FILE:snx-test-3ds-vk-draw> ${snx_mode})
            set_tests_properties(${snx_test} PROPERTIES SKIP_RETURN_CODE 77)
        endforeach()
    endif()
    add_executable(snx-test-3ds-gsp 3ds_gsp.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-gsp gnu23)
    target_link_libraries(snx-test-3ds-gsp PRIVATE snx_ctr_process)
    add_test(NAME 3ds_gsp_gpu COMMAND snx-test-3ds-gsp "${CMAKE_CURRENT_BINARY_DIR}/gsp-frames")
    # No Vulkan loader/ICD: presentation cannot run here; the probe skips, never fakes a frame.
    set_tests_properties(3ds_gsp_gpu PROPERTIES SKIP_RETURN_CODE 77)
    # The GPU tests again on each path the GPU's work can take to the RAM (gpu/vk/vk.h), whatever the device would
    # choose: the Switch's vk_memory, 1 on a copy of the RAM in the device's memory, 2 on the RAM in place
    foreach(snx_path copy in_place)
        set(snx_value 1)
        if(snx_path STREQUAL "in_place")
            set(snx_value 2)
        endif()
        set(snx_tests 3ds_gsp_gpu_${snx_path})
        foreach(snx_test 3ds_vk_transfer 3ds_vk_ram 3ds_vk_y2r 3ds_vk_draw 3ds_vk_copies)
            string(REPLACE "_" "-" snx_executable "snx-test-${snx_test}")
            add_test(NAME ${snx_test}_${snx_path} COMMAND ${snx_executable} --vk_memory=${snx_value})
            list(APPEND snx_tests ${snx_test}_${snx_path})
        endforeach()
        add_test(NAME 3ds_gsp_gpu_${snx_path} COMMAND snx-test-3ds-gsp
                 "${CMAKE_CURRENT_BINARY_DIR}/gsp-frames-${snx_path}" --vk_memory=${snx_value})
        if(NOT WIN32 AND Python3_Interpreter_FOUND)
            foreach(snx_test snx_mode IN ZIP_LISTS snx_vk_validation_tests snx_vk_validation_modes)
                add_test(NAME ${snx_test}_${snx_path}
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                        $<TARGET_FILE:snx-test-3ds-vk-draw> ${snx_mode} --vk_memory=${snx_value})
                list(APPEND snx_tests ${snx_test}_${snx_path})
            endforeach()
        endif()
        set_tests_properties(${snx_tests} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    # The GPU tests again on a New 3DS (--new3ds), their buffers and targets in the memory only it has: the upper half
    # of its 256 MB of FCRAM and its extra RAM. Each path to the RAM, and each of the draw test's modes
    set(snx_tests "")
    foreach(snx_path default copy in_place)
        set(snx_flag "")
        set(snx_suffix _${snx_path})
        if(snx_path STREQUAL "copy")
            set(snx_flag --vk_memory=1)
        elseif(snx_path STREQUAL "in_place")
            set(snx_flag --vk_memory=2)
        else()
            set(snx_suffix "")
        endif()
        foreach(snx_test 3ds_vk_transfer 3ds_vk_ram 3ds_vk_y2r 3ds_vk_draw 3ds_vk_copies)
            string(REPLACE "_" "-" snx_executable "snx-test-${snx_test}")
            add_test(NAME ${snx_test}_new3ds${snx_suffix} COMMAND ${snx_executable} --new3ds ${snx_flag})
            list(APPEND snx_tests ${snx_test}_new3ds${snx_suffix})
        endforeach()
    endforeach()
    foreach(snx_mode present after-draw texture-after-draw draw-after-index-copy draw-after-submit async shadow
            shadow-scaled)
        string(REPLACE "-" "_" snx_name "${snx_mode}")
        add_test(NAME 3ds_vk_draw_${snx_name}_new3ds COMMAND snx-test-3ds-vk-draw --new3ds --${snx_mode})
        list(APPEND snx_tests 3ds_vk_draw_${snx_name}_new3ds)
    endforeach()
    if(NOT WIN32 AND Python3_Interpreter_FOUND)
        foreach(snx_test snx_mode IN ZIP_LISTS snx_vk_validation_tests snx_vk_validation_modes)
            add_test(NAME ${snx_test}_new3ds
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                    $<TARGET_FILE:snx-test-3ds-vk-draw> ${snx_mode} --new3ds)
            list(APPEND snx_tests ${snx_test}_new3ds)
        endforeach()
    endif()
    set_tests_properties(${snx_tests} PROPERTIES SKIP_RETURN_CODE 77)
    # the two windows and the per-screen render scales: one window always, Separate screens, a minimized window, a
    # window whose images the display holds, presents between other submissions, and a headless run; the dumps stay the
    # framebuffer's own size whatever the scale
    add_executable(snx-test-3ds-windows 3ds_windows.c)
    snx_test_target(snx-test-3ds-windows gnu23)
    target_link_libraries(snx-test-3ds-windows PRIVATE snx_ctr_gpu)
    # the one window's screen layouts: the rectangles for each, the pure geometry the presenter uses
    add_executable(snx-test-3ds-layouts 3ds_layouts.c)
    snx_test_target(snx-test-3ds-layouts gnu23)
    target_link_libraries(snx-test-3ds-layouts PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_layouts COMMAND snx-test-3ds-layouts)
    add_test(NAME 3ds_windows COMMAND snx-test-3ds-windows "${CMAKE_CURRENT_BINARY_DIR}/window-frames")
    add_test(NAME 3ds_windows_separate
             COMMAND snx-test-3ds-windows "${CMAKE_CURRENT_BINARY_DIR}/window-frames-separate" --separate)
    add_test(NAME 3ds_windows_minimized
             COMMAND snx-test-3ds-windows "${CMAKE_CURRENT_BINARY_DIR}/window-frames-minimized" --minimize)
    add_test(NAME 3ds_windows_no_image
             COMMAND snx-test-3ds-windows "${CMAKE_CURRENT_BINARY_DIR}/window-frames-no-image" --no-image)
    # a smoke test: the validation run below, registered off Windows, is what fails without each image's own semaphore
    add_test(NAME 3ds_windows_submissions
             COMMAND snx-test-3ds-windows "${CMAKE_CURRENT_BINARY_DIR}/window-frames-submissions" --submissions)
    add_test(NAME 3ds_windows_headless
             COMMAND snx-test-3ds-windows "${CMAKE_CURRENT_BINARY_DIR}/window-frames-headless" --headless)
    set_tests_properties(3ds_windows 3ds_windows_separate PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(3ds_windows_minimized 3ds_windows_no_image 3ds_windows_submissions 3ds_windows_headless
                         PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(NOT WIN32 AND Python3_Interpreter_FOUND)
        # under Khronos validation, with SDL's offscreen window: a swapchain with no display, as on CI
        add_test(NAME 3ds_windows_submissions_validation
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_vk_validation.py"
                     $<TARGET_FILE:snx-test-3ds-windows>
                     "${CMAKE_CURRENT_BINARY_DIR}/window-frames-submissions-validation" --submissions)
        set_tests_properties(3ds_windows_submissions_validation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                             ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
    endif()
    # the transfer engine alone: fills, display transfers, texture copies against hand-written goldens
    add_executable(snx-test-3ds-transfer 3ds_transfer.c)
    snx_test_target(snx-test-3ds-transfer gnu23)
    target_link_libraries(snx-test-3ds-transfer PRIVATE snx_ctr_gpu)
    add_test(NAME 3ds_transfer COMMAND snx-test-3ds-transfer)
    # the PICA command processor: lists in FCRAM, uploads, interrupts, jumps, draw and immediate hooks
    add_executable(snx-test-3ds-pica 3ds_pica.c)
    snx_test_target(snx-test-3ds-pica gnu23)
    target_link_libraries(snx-test-3ds-pica PRIVATE snx_ctr_gpu_reference m)
    add_test(NAME 3ds_pica COMMAND snx-test-3ds-pica)
    # the HLE DSP and dsp::DSP: pipes, regions, sources, mixing, decoding, the service as ndsp drives it
    add_executable(snx-test-3ds-dsp 3ds_dsp.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-dsp gnu23)
    target_link_libraries(snx-test-3ds-dsp PRIVATE snx_ctr_process m)
    add_test(NAME 3ds_dsp COMMAND snx-test-3ds-dsp)

    # the loader's probe of a game read where it lies, and a standalone update laid over its game
    if(Python3_Interpreter_FOUND)
        add_executable(snx-test-3ds-container 3ds_container.c support/3ds_guest.c)
        snx_test_target(snx-test-3ds-container gnu23)
        target_link_libraries(snx-test-3ds-container PRIVATE snx_ctr_process)
        add_test(NAME 3ds_update COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_update.py"
            $<TARGET_FILE:snx-test-3ds-container>)
        # a game's DLC laid beside it, served by the NCCH archive and am:app
        add_executable(snx-test-3ds-dlc 3ds_dlc.c support/3ds_guest.c)
        snx_test_target(snx-test-3ds-dlc gnu23)
        target_link_libraries(snx-test-3ds-dlc PRIVATE snx_ctr_process)
        add_test(NAME 3ds_dlc COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_dlc.py"
            $<TARGET_FILE:snx-test-3ds-dlc>)
    endif()

    if(NOT WIN32)
        if(Python3_Interpreter_FOUND)
            add_executable(snx-test-3ds-fs 3ds_fs.c support/3ds_guest.c)
            snx_test_target(snx-test-3ds-fs gnu23)
            target_link_libraries(snx-test-3ds-fs PRIVATE snx_ctr_process)
            add_test(NAME 3ds_filesystem COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_fs.py" $<TARGET_FILE:snx-test-3ds-fs>)
            # a 3DS game's save state: its process, memory, files, DSP and PICA, saved and loaded
            add_executable(snx-test-3ds-state 3ds_state.c support/3ds_guest.c)
            snx_test_target(snx-test-3ds-state gnu23)
            target_link_libraries(snx-test-3ds-state PRIVATE snx_ctr_process snx_state_file)
            add_test(NAME 3ds_state COMMAND snx-test-3ds-state)
            set_tests_properties(3ds_state PROPERTIES TIMEOUT 120 ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
            # and with the GPU's work in it, on each path the device takes to the RAM
            add_executable(snx-test-3ds-state-gpu 3ds_state_gpu.c support/3ds_guest.c)
            snx_test_target(snx-test-3ds-state-gpu gnu23)
            target_link_libraries(snx-test-3ds-state-gpu PRIVATE snx_ctr_process snx_state_file)
            add_test(NAME 3ds_state_gpu_copy COMMAND snx-test-3ds-state-gpu --vk_memory=1)
            add_test(NAME 3ds_state_gpu_in_place COMMAND snx-test-3ds-state-gpu --vk_memory=2)
            set_tests_properties(3ds_state_gpu_copy 3ds_state_gpu_in_place PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                                 ENVIRONMENT "SDL_AUDIO_DRIVER=dummy")
            add_test(NAME ctr_swipc_validation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ctr_swipc.py"
                $<TARGET_FILE:snx-swipc>)
            add_test(NAME 3ds_driver COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_driver.py"
                "${SNX_CTR_PROGRAM}")
            # a game's update and DLC recorded beside it, checked in its addons= and played with it
            add_test(NAME 3ds_addons COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_addons.py"
                "${SNX_CTR_PROGRAM}")
            set_tests_properties(3ds_addons PROPERTIES TIMEOUT 300)
            # a run's unattended save states from its command line: one saves a slot, the next loads it
            add_test(NAME 3ds_states COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_states.py"
                "${SNX_CTR_PROGRAM}")
            set_tests_properties(3ds_states PROPERTIES TIMEOUT 600)
            # a game's console: the command line's, its own Settings', the launcher's, the one its .ini records
            add_test(NAME 3ds_console COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_console.py"
                "${SNX_CTR_PROGRAM}")
            set_tests_properties(3ds_console PROPERTIES TIMEOUT 600)
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
    # the static compile's worker threads each build their chunks for the host's memory model, on Windows too
    if(Python3_Interpreter_FOUND AND NOT APPLE)
        add_test(NAME 3ds_aot_workers COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_aot_workers.py"
            "${SNX_CTR_PROGRAM}")
        # 15 to 35 s here, over a minute on a CI runner, and 170 to 300 s or more there with the sanitizers
        if(CMAKE_CROSSCOMPILING_EMULATOR)
            set_tests_properties(3ds_aot_workers PROPERTIES TIMEOUT 1800)
        elseif(SNX_TEST_SANITIZERS)
            set_tests_properties(3ds_aot_workers PROPERTIES TIMEOUT 900)
        else()
            set_tests_properties(3ds_aot_workers PROPERTIES TIMEOUT 300)
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
    foreach(snx_test 3ds_gsp_gpu 3ds_gsp_gpu_copy 3ds_gsp_gpu_in_place 3ds_static_pipeline 3ds_extract
                     3ds_vk_transfer 3ds_vk_copies 3ds_vk_ram 3ds_vk_y2r 3ds_vk_draw 3ds_vk_draw_present
                     3ds_vk_pipeline_cache 3ds_vk_shader_object 3ds_vk_import_error 3ds_windows 3ds_windows_headless)
        if(TEST ${snx_test})
            set_property(TEST ${snx_test} APPEND PROPERTY ENVIRONMENT
                         "LD_PRELOAD=$<TARGET_FILE:snx-test-vulkan-drivers-kept>")
        endif()
    endforeach()
endif()

add_executable(snx-test-arm-profile arm_profile.c)
snx_test_target(snx-test-arm-profile gnu23)
target_link_libraries(snx-test-arm-profile PRIVATE snx_arm_decode)
add_test(NAME arm_isa_profiles COMMAND snx-test-arm-profile)
# which targets async_pipelines may skip a draw into (src/nx/gpu/vk/redraw.h): those the game regenerates every frame
add_executable(snx-test-vk-redraw vk_redraw.c)
snx_test_target(snx-test-vk-redraw c23)
target_include_directories(snx-test-vk-redraw PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME vk_redraw COMMAND snx-test-vk-redraw)
# a game's cheats by their names, as the launcher lists them and cheats_off= leaves them out
add_executable(snx-test-cheat-list cheat_list.c)
snx_test_target(snx-test-cheat-list c23)
target_link_libraries(snx-test-cheat-list PRIVATE scarletnx-common)
add_test(NAME cheat_list COMMAND snx-test-cheat-list)
# a game's cheats as a frame runs them (src/nx/runtime/cheats.c), over a file and a memory of the test's
add_executable(snx-test-cheats-run cheats_run.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/cheats.c")
snx_test_target(snx-test-cheats-run gnu23)
target_include_directories(snx-test-cheats-run PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-cheats-run PRIVATE scarletnx-common scarletnx-platform)
add_test(NAME cheats_run COMMAND snx-test-cheats-run)
# a GPU driver package's meta.json, folder name and library, the devices that load one, a run's check that it did, and
# Freedreno's variables (common/utilities/gpu_driver.c)
add_executable(snx-test-gpu-driver gpu_driver.c)
snx_test_target(snx-test-gpu-driver c23)
target_link_libraries(snx-test-gpu-driver PRIVATE scarletnx-common)
add_test(NAME gpu_driver COMMAND snx-test-gpu-driver)
# an ELF library's names written over and a loaded one's symbols found (common/utilities/elf_dynamic.c), on libraries
# made in memory (elf_image.h) and on this process's own dynamic linker (elf_dynamic_host.c)
add_executable(snx-test-elf-dynamic elf_dynamic.c elf_dynamic_host.c)
snx_test_target(snx-test-elf-dynamic gnu23)
target_link_libraries(snx-test-elf-dynamic PRIVATE scarletnx-common ${CMAKE_DL_LIBS})
add_test(NAME elf_dynamic COMMAND snx-test-elf-dynamic)
# a GPU driver package's libraries copied under names no library has (common/utilities/gpu_driver_stage.c)
add_executable(snx-test-gpu-driver-stage gpu_driver_stage.c)
snx_test_target(snx-test-gpu-driver-stage gnu23)
target_link_libraries(snx-test-gpu-driver-stage PRIVATE scarletnx-common)
add_test(NAME gpu_driver_stage COMMAND snx-test-gpu-driver-stage)
# the common utilities alone, for the tests every build runs (gui_probe) and the NX ones
add_library(snx_test_common STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/common.c"
    "${PROJECT_SOURCE_DIR}/src/common/utilities/ini_lines.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/config_file.c"
    "${PROJECT_SOURCE_DIR}/src/common/utilities/game_ini.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/script.c")
snx_test_target(snx_test_common c23)
target_link_libraries(snx_test_common PUBLIC snx_libdeflate) # a control.nacp's titles in deflate
target_link_libraries(snx_test_common PUBLIC scarletnx-platform) # the clocks (snx_seconds)
if(WIN32)
    target_link_libraries(snx_test_common PUBLIC advapi32)
endif()

# Run Amberlith's card and report (gui/probe.c)
add_executable(snx-test-gui-probe gui_probe.c "${PROJECT_SOURCE_DIR}/src/gui/probe.c")
snx_test_target(snx-test-gui-probe c23)
target_link_libraries(snx-test-gui-probe PRIVATE snx_test_common snx_yyjson)
add_test(NAME gui_probe COMMAND snx-test-gui-probe)
# the menu's Last Run record, what the system says of a process's end and the kernel's OOM kills (gui/last_run.c)
add_executable(snx-test-gui-last-run gui_last_run.c "${PROJECT_SOURCE_DIR}/src/gui/last_run.c")
snx_test_target(snx-test-gui-last-run c23)
target_link_libraries(snx-test-gui-last-run PRIVATE snx_test_common)
add_test(NAME gui_last_run COMMAND snx-test-gui-last-run)
# a mod's IPS, IPS32 and IPSwitch patches: their formats, which module takes one, the played mods' in their order
add_executable(snx-test-patches patches.c)
snx_test_target(snx-test-patches c23)
target_link_libraries(snx-test-patches PRIVATE scarletnx-common)
add_test(NAME patches COMMAND snx-test-patches)
# a recompiled game's stamp: what it is compared by, the made line after it, and the save states' copy of it
add_executable(snx-test-game-stamp game_stamp.c)
snx_test_target(snx-test-game-stamp c23)
target_link_libraries(snx-test-game-stamp PRIVATE scarletnx-common)
add_test(NAME game_stamp COMMAND snx-test-game-stamp)
if(SNX_TEST_ARM_AOT)
    include(arm_checked.cmake)
endif()
include(arm_native_modes.cmake)

# Board/GUI-only builds do not configure either AOT runtime or LLVM. Do not
# create CPU execution tests that link the absent backend targets in that case.
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
# Exercise NX's real FS tree and service adapters even in the independent CTR
# build. These fixtures supply their own platform/kernel boundary stubs.
add_executable(snx-test-fs-fixture filesystem_fixture.c)
snx_test_target(snx-test-fs-fixture c23)
target_link_libraries(snx-test-fs-fixture PRIVATE scarletnx-common)
add_library(snx_test_fs_tree OBJECT "${PROJECT_SOURCE_DIR}/src/common/filesystem/tree.c")
snx_test_target(snx_test_fs_tree gnu23)
target_compile_options(snx_test_fs_tree PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fs_scan_probe.h")
set(snx_fs_suites tree)
if(TARGET hactool_internals) # the service reads dumps through hactool, built with the NX runtime
    list(APPEND snx_fs_suites service)
endif()
foreach(suite ${snx_fs_suites})
    add_executable(snx-test-fs-${suite} filesystem_${suite}.c $<TARGET_OBJECTS:snx_test_fs_tree>)
    snx_test_target(snx-test-fs-${suite} gnu23)
    add_test(NAME filesystem_${suite}
        COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-${suite}>)
endforeach()
if(TARGET hactool_internals)
    target_compile_options(snx-test-fs-service PRIVATE -Wno-unused-parameter)
    # fs.c reads a game's dump in place through the driver's container.c and hactool
    target_sources(snx-test-fs-service PRIVATE "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/container.c"
)
    target_link_libraries(snx-test-fs-service PRIVATE scarletnx-common hactool_internals)
    add_test(NAME filesystem_raw COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> raw)
    add_test(NAME filesystem_addon_overlay COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> addon-overlay)
    add_test(NAME filesystem_mods COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> mods)
    # a game killed while it saves, at each step of a save's commit and open: its last commit or the new one, whole
    add_test(NAME filesystem_save_kill COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> save-kill)
    add_test(NAME filesystem_mods_stale COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> mods-stale)
    add_test(NAME filesystem_folder_source
             COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> folder-source)
    add_test(NAME filesystem_folder_gone COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> folder-gone)
    foreach(how copying extracting none whole)
        add_test(NAME filesystem_romfs_${how} COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> romfs-${how})
    endforeach()
    if(NOT WIN32) # the dump's link: Windows makes one only with the right to
        add_test(NAME filesystem_dump COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump)
        add_test(NAME filesystem_dump_source COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-source)
        # an NSZ, its program a solid NCZ, read in place as its NCA would be, what that costs said
        add_test(NAME filesystem_dump_nsz COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-nsz)
        add_test(NAME filesystem_dump_none COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-none)
        add_test(NAME filesystem_data_link COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> data-link)
        add_test(NAME filesystem_dump_gone COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-gone)
        add_test(NAME filesystem_dump_keys COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-keys)
        add_test(NAME filesystem_dump_cut COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-cut)
        add_test(NAME filesystem_dump_romfs_cut
                 COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-romfs-cut)
        add_test(NAME filesystem_dump_romfs_zeros
                 COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-romfs-zeros)
        add_test(NAME filesystem_dump_update COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-update)
        add_test(NAME filesystem_mods_image COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> mods-image)
        add_test(NAME filesystem_dump_update_extracted
                 COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-update-extracted)
        add_test(NAME filesystem_dump_dlc COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> dump-dlc)
    endif()
endif()
if(UNIX)
    # the platform's files by themselves: shared files, folders and the mutex
    add_executable(snx-test-fs-host filesystem_host.c "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c")
    snx_test_target(snx-test-fs-host gnu23)
    target_link_libraries(snx-test-fs-host PRIVATE Threads::Threads)
    add_test(NAME filesystem_host COMMAND snx-test-fs-host WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(snx-test-common-mounts common_mounts.c)
    snx_test_target(snx-test-common-mounts c23)
    target_link_libraries(snx-test-common-mounts PRIVATE scarletnx-common)
    add_test(NAME common_mounts COMMAND snx-test-common-mounts)
    add_executable(snx-test-common-appendf common_appendf.c)
    snx_test_target(snx-test-common-appendf c23)
    target_link_libraries(snx-test-common-appendf PRIVATE scarletnx-common)
    add_test(NAME common_appendf COMMAND snx-test-common-appendf)
    add_executable(snx-test-data-dir data_dir.c)
    snx_test_target(snx-test-data-dir c23)
    target_link_libraries(snx-test-data-dir PRIVATE scarletnx-common)
    add_test(NAME data_dir COMMAND snx-test-data-dir)
    add_test(NAME runtime_deploy COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/deploy.cmake")
endif()
# snx_os_error: the system's reason a message names, Windows' GetLastError where its calls leave errno
add_executable(snx-test-os-error os_error.c)
snx_test_target(snx-test-os-error c23)
target_link_libraries(snx-test-os-error PRIVATE scarletnx-common)
add_test(NAME os_error COMMAND snx-test-os-error)
# the host's side of a flat guest space (common/memory/host.c): views of one object are the same memory, a commit zeros
add_executable(snx-test-host-memory host_memory.c)
snx_test_target(snx-test-host-memory c23)
target_link_libraries(snx-test-host-memory PRIVATE scarletnx-common)
add_test(NAME host_memory COMMAND snx-test-host-memory)
# the threads that make its first call together, as the 3DS compile's workers do, all read the host's page
add_executable(snx-test-host-page-threads host_page_threads.c)
snx_test_target(snx-test-host-page-threads gnu23)
target_link_libraries(snx-test-host-page-threads PRIVATE scarletnx-common Threads::Threads)
add_test(NAME host_page_threads COMMAND snx-test-host-page-threads)
# a compile thread's stack (compile_thread.h) takes the recursion LLVM's passes make over a game's biggest function
add_executable(snx-test-compile-thread compile_thread.c)
snx_test_target(snx-test-compile-thread c23)
add_test(NAME compile_thread COMMAND snx-test-compile-thread)
# RtAPI.cas128: the host's 16-byte compare-and-swap and the locked one, a pair stored whole or not at all
add_executable(snx-test-cas128 cas128.c)
snx_test_target(snx-test-cas128 gnu23)
target_link_libraries(snx-test-cas128 PRIVATE scarletnx-common Threads::Threads)
add_test(NAME cas128 COMMAND snx-test-cas128)
# cas128.c's branch for a big-endian host with no instruction of its own (s390x), which none of these builds compiles:
# its syntax, where this compiler takes the target (the header alone tells)
execute_process(COMMAND "${CMAKE_C_COMPILER}" --target=s390x-linux-gnu -std=gnu23 -ffreestanding -fsyntax-only
                        "-I${PROJECT_SOURCE_DIR}/src" -x c "${PROJECT_SOURCE_DIR}/src/common/runtime/interface.h"
                RESULT_VARIABLE snx_s390x_syntax OUTPUT_QUIET ERROR_QUIET)
if(snx_s390x_syntax EQUAL 0)
    add_test(NAME cas128_big_endian_syntax
             COMMAND "${CMAKE_C_COMPILER}" --target=s390x-linux-gnu -std=gnu23 -ffreestanding -fsyntax-only -Werror
                     "-I${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/common/runtime/cas128.c")
endif()
# the drivers' common options (common/utilities/cli.h): their forms, older names, refusals and help
add_executable(snx-test-cli cli.c)
snx_test_target(snx-test-cli c23)
target_link_libraries(snx-test-cli PRIVATE scarletnx-common)
add_test(NAME cli COMMAND snx-test-cli)
# bits.h's helpers with the compiler's builtins and 128-bit integer, and in ISO C alone, strict ISO C23
add_executable(snx-test-bits bits.c)
snx_test_target(snx-test-bits c23)
add_test(NAME bits COMMAND snx-test-bits)
add_executable(snx-test-bits-portable bits.c)
snx_test_target(snx-test-bits-portable c23)
target_compile_definitions(snx-test-bits-portable PRIVATE SNX_BITS_PORTABLE)
add_test(NAME bits_portable COMMAND snx-test-bits-portable)
# guest byte order on any host (src/common/utilities/endian.h), strict ISO C23 as the code that includes it
add_executable(snx-test-endian endian.c)
snx_test_target(snx-test-endian c23)
add_test(NAME endian COMMAND snx-test-endian)
# the SPIR-V of a game's own Vulkan, mended where the console's driver takes what Vulkan doesn't (src/nx/gpu/gvk)
add_executable(snx-test-gvk-spirv gvk_spirv.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gvk/gvk_spirv.c")
snx_test_target(snx-test-gvk-spirv gnu23)
target_include_directories(snx-test-gvk-spirv PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME gvk_spirv COMMAND snx-test-gvk-spirv $<TARGET_FILE:snx-spirv-val>)
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND NOT CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    add_executable(snx-test-gvk-marshal gvk_marshal.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gvk/gvk.c")
    snx_test_target(snx-test-gvk-marshal gnu23)
    target_include_directories(snx-test-gvk-marshal PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(snx-test-gvk-marshal PRIVATE Vulkan::Headers scarletnx-platform)
    add_test(NAME gvk_marshal COMMAND snx-test-gvk-marshal)
endif()
# glslc (src/glslc): its passes, SPIRV-Tools' validator and glslangValidator on its output, a fuzz
add_subdirectory(glslc)
if(SNX_BUILD_3DS)
    add_executable(snx-test-3ds-uds 3ds_uds.c fake_room.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-uds gnu23)
    target_link_libraries(snx-test-3ds-uds PRIVATE snx_ctr_process scarletnx-ldn snx_enet)
    add_test(NAME 3ds_uds_relay COMMAND snx-test-3ds-uds)
    add_test(NAME 3ds_uds_delayed_relay COMMAND snx-test-3ds-uds --delay)
    # a wait of the two lasts 30 s at most and says what is missing (3ds_uds.c), which ctest's own limit must not precede
    # alone: a request within 100 ms is the host's clock, which tests beside them take
    set_tests_properties(3ds_uds_delayed_relay 3ds_uds_relay PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    # nim:aoc's Initialize answers the no-network error a console gives (tests/3ds_nim_aoc.c)
    add_executable(snx-test-3ds-nim-aoc 3ds_nim_aoc.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-nim-aoc gnu23)
    target_link_libraries(snx-test-3ds-nim-aoc PRIVATE snx_ctr_process scarletnx-ldn snx_enet)
    add_test(NAME 3ds_nim_aoc COMMAND snx-test-3ds-nim-aoc)
    # soc:U, ac:u and frd:u: sockets on loopback, the connection, a game's login through a NASC on this host
    add_executable(snx-test-3ds-online 3ds_online.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-online gnu23)
    target_link_libraries(snx-test-3ds-online PRIVATE snx_ctr_process scarletnx-ldn snx_enet snx_online)
    add_test(NAME 3ds_online COMMAND snx-test-3ds-online)
    set_tests_properties(3ds_online PROPERTIES TIMEOUT 60)
    add_executable(snx-test-3ds-uds-offline 3ds_uds_offline.c support/3ds_guest.c)
    snx_test_target(snx-test-3ds-uds-offline gnu23)
    target_link_libraries(snx-test-3ds-uds-offline PRIVATE snx_ctr_process scarletnx-ldn snx_enet)
    add_test(NAME 3ds_uds_offline COMMAND snx-test-3ds-uds-offline)
endif()
if(NOT SNX_BUILD_NX AND NOT SNX_BUILD_GNUABI)
    # The shared room transport also serves 3DS UDS; keep its socket regression
    # available when building only the 3DS driver.
    if(SNX_BUILD_3DS)
        add_executable(snx-test-ldn-room ldn_room.c fake_room.c)
        snx_test_target(snx-test-ldn-room gnu23)
        target_link_libraries(snx-test-ldn-room PRIVATE scarletnx-ldn snx_enet)
        add_test(NAME ldn_room COMMAND snx-test-ldn-room)
        set_tests_properties(ldn_room PROPERTIES TIMEOUT 120)
    endif()
    include(architecture.cmake)
    snx_sanitizer_path()
    return()
endif()

# the shared SPIR-V optimizer's passes on modules the test writes word by word
add_executable(snx-test-spirv-opt spirv_opt.c "${PROJECT_SOURCE_DIR}/src/common/utilities/spirv_opt.c")
snx_test_target(snx-test-spirv-opt c23)
target_include_directories(snx-test-spirv-opt PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME spirv_opt COMMAND snx-test-spirv-opt)
add_executable(snx-test-spirv-opt-values spirv_opt_values.c "${PROJECT_SOURCE_DIR}/src/common/utilities/spirv_opt.c")
snx_test_target(snx-test-spirv-opt-values c23)
target_include_directories(snx-test-spirv-opt-values PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME spirv_opt_values COMMAND snx-test-spirv-opt-values)
add_executable(snx-test-spirv-opt-fold spirv_opt_fold.c "${PROJECT_SOURCE_DIR}/src/common/utilities/spirv_opt.c")
snx_test_target(snx-test-spirv-opt-fold c23)
target_include_directories(snx-test-spirv-opt-fold PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME spirv_opt_fold COMMAND snx-test-spirv-opt-fold)

# Validate the direct writer against SPIRV-Tools (cmake/spirv_tools.cmake)
if(SNX_BUILD_NX AND Python3_Interpreter_FOUND)
    include("${PROJECT_SOURCE_DIR}/cmake/spirv_tools.cmake")
    if(TARGET snx-spirv-dis)
        add_executable(snx-test-spirv-emitter spirv_emitter.c spirv_emitter_fixture.c
            "${PROJECT_SOURCE_DIR}/src/common/utilities/spirv_emit.c")
        snx_test_target(snx-test-spirv-emitter gnu23)
                add_test(NAME spirv_emitter COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/spirv_emitter_test.py"
            --generator $<TARGET_FILE:snx-test-spirv-emitter> --validator $<TARGET_FILE:snx-spirv-val>
            --disassembler $<TARGET_FILE:snx-spirv-dis>)
    else()
        message(STATUS "Skipping spirv_emitter: spirv-dis is required")
    endif()
elseif(SNX_BUILD_NX)
    message(STATUS "Skipping spirv_emitter: Python 3 is required")
endif()

add_executable(snx-test-arm-decode arm_decode.c "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/rt_helpers.c")
snx_test_target(snx-test-arm-decode gnu23)
target_link_libraries(snx-test-arm-decode PRIVATE snx_arm_decode)
add_test(NAME arm_decode COMMAND snx-test-arm-decode)
# the A32 and Thumb decoders from QEMU's .decode files (tools/decodetree) against the hand-written ones they replaced
# (support/arm_decode_ref_*.c): every T16 halfword and T32 pair, and A32 words by a stride (--full: every word)
add_executable(snx-test-arm-decode-tree arm_decode_tree.c support/arm_decode_ref_a32.c support/arm_decode_ref_thumb.c)
snx_test_target(snx-test-arm-decode-tree gnu23)
target_include_directories(snx-test-arm-decode-tree PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(snx-test-arm-decode-tree PRIVATE snx_arm_decode)
add_test(NAME arm_decode_tree COMMAND snx-test-arm-decode-tree)
set_tests_properties(arm_decode_tree PROPERTIES TIMEOUT 600)
add_executable(snx-test-arm-fp-host arm_fp_host.c)
snx_test_target(snx-test-arm-fp-host gnu23)
target_link_libraries(snx-test-arm-fp-host PRIVATE snx_arm_rt)
add_test(NAME arm_fp_host COMMAND snx-test-arm-fp-host)
add_executable(snx-test-arm-exec arm_exec_test.c)
snx_test_target(snx-test-arm-exec gnu23)
target_link_libraries(snx-test-arm-exec PRIVATE snx_llvm)
add_test(NAME arm_exec_dry_svc COMMAND snx-test-arm-exec)
# the same executor where the loader does not take a native block's object (exec.c itself, its loads the test's)
add_executable(snx-test-arm-exec-fallback arm_exec_fallback.c)
snx_test_target(snx-test-arm-exec-fallback gnu23)
target_include_directories(snx-test-arm-exec-fallback PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(snx-test-arm-exec-fallback PRIVATE snx_llvm)
add_test(NAME arm_exec_fallback COMMAND snx-test-arm-exec-fallback WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# the AOT compilers' blocks: the native back end's, LLVM's where it refuses one or the IR is kept, and who made them said
add_executable(snx-test-arm-aot arm_aot.c)
snx_test_target(snx-test-arm-aot gnu23)
target_link_libraries(snx-test-arm-aot PRIVATE snx_llvm scarletnx-common ${CMAKE_DL_LIBS})
add_test(NAME arm_aot COMMAND snx-test-arm-aot)
# a 32-bit game's blocks compiled from an NRO go when its memory is unmapped, so another NRO mapped there runs its own
# code (runtime/arm.c, with the block compiler and the rest of the runtime stubbed)
add_executable(snx-test-arm-code-invalidate arm_code_invalidate.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/arm.c")
snx_test_target(snx-test-arm-code-invalidate gnu23)
target_link_libraries(snx-test-arm-code-invalidate PRIVATE snx_llvm scarletnx-common)
add_test(NAME arm_code_invalidate COMMAND snx-test-arm-code-invalidate)
# a checked block links into the AOT module for Windows x86-64, which has no compiler runtime
add_executable(snx-test-arm-checked-link arm_checked_link.c)
snx_test_target(snx-test-arm-checked-link gnu23)
target_link_libraries(snx-test-arm-checked-link PRIVATE snx_llvm)
add_test(NAME arm_checked_link COMMAND snx-test-arm-checked-link)
set_tests_properties(arm_checked_link PROPERTIES SKIP_RETURN_CODE 77)

# Small architectural witnesses need neither game assets nor an emulator.
add_executable(snx-test-a64-fp a64_fp_test.c "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp gnu23)
if(NOT WIN32)
    target_link_libraries(snx-test-a64-fp PRIVATE m)
endif()
snx_cpu_helpers_flags(snx-test-a64-fp)
add_test(NAME a64_fp_witnesses COMMAND snx-test-a64-fp)

# CRC32 and CRC32C through the LLVM steps, the host's instructions and the portable sliced tables, against the
# bit-serial definition
add_executable(snx-test-crc32-helper crc32_helper.c llvm_steps.c)
snx_test_target(snx-test-crc32-helper gnu23)
target_link_libraries(snx-test-crc32-helper PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME crc32_helper COMMAND snx-test-crc32-helper)

# the NVN header Part C builds on (src/nx/gpu/nvn/nvn.h, h1k421's, MIT) as strict ISO C23, against docs/layers/gpu/nvn.md's fields,
# and the NVN functions' types that hookgen makes of gpu/nvn/nvn.hook, against prototypes in NVN's types
add_executable(snx-test-nvn-header nvn_header.c)
snx_test_target(snx-test-nvn-header c23)
target_include_directories(snx-test-nvn-header PRIVATE "${PROJECT_SOURCE_DIR}/src")
include("${PROJECT_SOURCE_DIR}/cmake/hookgen.cmake")
snx_hookgen(snx-test-nvn-header SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
add_test(NAME nvn_header COMMAND snx-test-nvn-header)
# nvn's TIC and TSC encoders (src/nx/gpu/nvn/nvn_tic.c) read back through the renderer's decoders
add_executable(snx-test-nvn-descriptors nvn_descriptors.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tic.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tex.c")
snx_test_target(snx-test-nvn-descriptors c23)
target_include_directories(snx-test-nvn-descriptors PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-nvn-descriptors PRIVATE m)
add_test(NAME nvn_descriptors COMMAND snx-test-nvn-descriptors)
# gpu_hook_trace (src/nx/gpu/nvn/nvn_trace.c) over synthetic NVN records: off and gpu_hook_trace_from not live, the
# presents, draws with their targets, viewport and constant buffer words, dispatches with their call site and the image
# their c[0] handle names, the records between, the waits, a line past the log's 2048 bytes, the lines used up
add_executable(snx-test-nvn-trace nvn_trace.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_trace.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tic.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tex.c")
snx_test_target(snx-test-nvn-trace 11)
add_dependencies(snx-test-nvn-trace snx_settings)
target_include_directories(snx-test-nvn-trace PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/settingsgen")
target_link_libraries(snx-test-nvn-trace PRIVATE m)
add_test(NAME nvn_trace COMMAND snx-test-nvn-trace)
# nvn's texture storage answers (src/nx/gpu/nvn/nvn_tex.c nvn_tex_storage) against the game driver's rules
add_executable(snx-test-nvn-tex-storage nvn_tex_storage.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tex.c")
snx_test_target(snx-test-nvn-tex-storage c23)
target_include_directories(snx-test-nvn-tex-storage PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME nvn_tex_storage COMMAND snx-test-nvn-tex-storage)
# the render sizes nvn's DLSS stand-in answers (src/nx/gpu/nvn/nvn_dlss_sizes.h): the driver's rule and the setting
add_executable(snx-test-nvn-dlss-sizes nvn_dlss_sizes.c)
snx_test_target(snx-test-nvn-dlss-sizes c23)
target_include_directories(snx-test-nvn-dlss-sizes PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-nvn-dlss-sizes PRIVATE m)
add_test(NAME nvn_dlss_sizes COMMAND snx-test-nvn-dlss-sizes)
# the vector FP instructions with fast paths through the LLVM steps against the soft-float helpers, lane by lane,
# with lanes the host's arithmetic can and can't take in one vector
add_executable(snx-test-llvm-vfp-lanes llvm_vfp_lanes.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-vfp-lanes gnu23)
snx_cpu_helpers_flags(snx-test-llvm-vfp-lanes)
target_link_libraries(snx-test-llvm-vfp-lanes PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_vfp_lanes COMMAND snx-test-llvm-vfp-lanes)
add_executable(snx-test-llvm-vfp-batch llvm_vfp_batch.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-vfp-batch gnu23)
snx_cpu_helpers_flags(snx-test-llvm-vfp-batch)
target_link_libraries(snx-test-llvm-vfp-batch PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_vfp_batch COMMAND snx-test-llvm-vfp-batch)
# exact vector comparisons, including NaN and flushed-denormal lanes
add_executable(snx-test-llvm-vfp-compare llvm_vfp_compare.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-vfp-compare gnu23)
snx_cpu_helpers_flags(snx-test-llvm-vfp-compare)
target_link_libraries(snx-test-llvm-vfp-compare PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_vfp_compare COMMAND snx-test-llvm-vfp-compare)
# the same fast paths taken for normal operands, with FPCR.FZ clear and set and without FPSR's flags: no helper call
# left once LLVM folds the guards
add_executable(snx-test-llvm-fp-fast llvm_fp_fast.c llvm_steps.c)
snx_test_target(snx-test-llvm-fp-fast gnu23)
target_link_libraries(snx-test-llvm-fp-fast PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_fp_fast COMMAND snx-test-llvm-fp-fast)

# Exact vector reciprocal-square-root estimates, through real O0/O2 AOT and JIT steps.
add_executable(snx-test-llvm-rsqrte llvm_rsqrte.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-rsqrte gnu23)
snx_cpu_helpers_flags(snx-test-llvm-rsqrte)
target_link_libraries(snx-test-llvm-rsqrte PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_rsqrte COMMAND snx-test-llvm-rsqrte)

# the helpers' host fast paths (helpers/fp_host.c) against the soft-float they front, bits and FPSR
add_executable(snx-test-fp-host fp_host_test.c "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-fp-host gnu23)
snx_cpu_helpers_flags(snx-test-fp-host)
if(NOT WIN32)
    target_link_libraries(snx-test-fp-host PRIVATE m)
endif()
add_test(NAME fp_host_witnesses COMMAND snx-test-fp-host)
add_executable(snx-test-llvm-recpe llvm_recpe.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-recpe gnu23)
snx_cpu_helpers_flags(snx-test-llvm-recpe)
target_link_libraries(snx-test-llvm-recpe PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_recpe COMMAND snx-test-llvm-recpe)

add_executable(snx-test-llvm-recpx llvm_recpx.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-recpx gnu23)
snx_cpu_helpers_flags(snx-test-llvm-recpx)
target_link_libraries(snx-test-llvm-recpx PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_recpx COMMAND snx-test-llvm-recpx)

add_executable(snx-test-llvm-simd-shift llvm_simd_shift.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-simd-shift gnu23)
snx_cpu_helpers_flags(snx-test-llvm-simd-shift)
target_link_libraries(snx-test-llvm-simd-shift PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_simd_shift COMMAND snx-test-llvm-simd-shift)

add_executable(snx-test-llvm-fp-cold llvm_fp_cold.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-fp-cold c23)
snx_cpu_helpers_flags(snx-test-llvm-fp-cold)
target_link_libraries(snx-test-llvm-fp-cold PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_fp_cold COMMAND snx-test-llvm-fp-cold)
set_tests_properties(llvm_fp_cold PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

add_executable(snx-test-llvm-fp-pointer-chain llvm_fp_pointer_chain.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-fp-pointer-chain gnu23)
snx_cpu_helpers_flags(snx-test-llvm-fp-pointer-chain)
target_link_libraries(snx-test-llvm-fp-pointer-chain PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_fp_pointer_chain COMMAND snx-test-llvm-fp-pointer-chain)
set_tests_properties(llvm_fp_pointer_chain PROPERTIES TIMEOUT 120)

add_executable(snx-test-llvm-vfp-minmax llvm_vfp_minmax.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-vfp-minmax gnu23)
snx_cpu_helpers_flags(snx-test-llvm-vfp-minmax)
target_link_libraries(snx-test-llvm-vfp-minmax PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_vfp_minmax COMMAND snx-test-llvm-vfp-minmax)
if(Python3_Interpreter_FOUND)
    add_test(NAME llvm_half_order_math COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/llvm_perf/half_order_math.py")
    # llvm_perf/run.sh's NZCV harness takes the flag emitters out of ir_state.c by their signatures: that it still
    # finds each one, where the harness itself (a JIT over the system's libLLVM) is run by hand
    add_test(NAME llvm_perf_nzcv_source COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/llvm_perf/test_nzcv.py" "${CMAKE_CURRENT_BINARY_DIR}/llvm_perf_nzcv.c")
endif()

add_executable(snx-test-llvm-round llvm_round.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-round gnu23)
snx_cpu_helpers_flags(snx-test-llvm-round)
target_link_libraries(snx-test-llvm-round PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_round COMMAND snx-test-llvm-round)
if(Python3_Interpreter_FOUND)
    add_test(NAME llvm_half_round_math COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/llvm_perf/half_round_math.py")
endif()

add_executable(snx-test-llvm-estimates llvm_estimates.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-estimates gnu23)
snx_cpu_helpers_flags(snx-test-llvm-estimates)
target_link_libraries(snx-test-llvm-estimates PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_estimates COMMAND snx-test-llvm-estimates)

add_executable(snx-test-llvm-unsigned-estimates llvm_unsigned_estimates.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-unsigned-estimates gnu23)
snx_cpu_helpers_flags(snx-test-llvm-unsigned-estimates)
target_link_libraries(snx-test-llvm-unsigned-estimates PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_unsigned_estimates COMMAND snx-test-llvm-unsigned-estimates)

add_executable(snx-test-llvm-half llvm_half.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-half gnu23)
snx_cpu_helpers_flags(snx-test-llvm-half)
target_link_libraries(snx-test-llvm-half PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_half COMMAND snx-test-llvm-half)
if(Python3_Interpreter_FOUND)
    add_test(NAME llvm_half_widen_math COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/llvm_perf/half_widen_math.py")
endif()

add_executable(snx-test-llvm-fma-residual llvm_fma_residual.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-fma-residual gnu23)
snx_cpu_helpers_flags(snx-test-llvm-fma-residual)
target_link_libraries(snx-test-llvm-fma-residual PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_fma_residual COMMAND snx-test-llvm-fma-residual)

add_executable(snx-test-llvm-vfp-residual llvm_vfp_residual.c llvm_steps.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-llvm-vfp-residual gnu23)
snx_cpu_helpers_flags(snx-test-llvm-vfp-residual)
target_link_libraries(snx-test-llvm-vfp-residual PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_vfp_residual COMMAND snx-test-llvm-vfp-residual)

add_executable(snx-test-llvm-fp-env llvm_fp_env.c llvm_steps.c)
snx_test_target(snx-test-llvm-fp-env gnu23)
target_link_libraries(snx-test-llvm-fp-env PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_fp_env COMMAND snx-test-llvm-fp-env)

add_executable(snx-test-llvm-fp-helper-effects llvm_fp_helper_effects.c)
snx_test_target(snx-test-llvm-fp-helper-effects gnu23)
target_link_libraries(snx-test-llvm-fp-helper-effects PRIVATE snx_llvm snx_driver)
add_test(NAME llvm_fp_helper_effects COMMAND snx-test-llvm-fp-helper-effects)

# ir_promote_registers at every boundary (SVC, BL, BLR, poll, dispatch entry, stale code, each helper class, a
# register left in the CPU structure) with every register kind, against a C model and the same program at O0
add_executable(snx-test-llvm-promote-edges llvm_promote_edges.c)
snx_test_target(snx-test-llvm-promote-edges c23)
target_link_libraries(snx-test-llvm-promote-edges PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_promote_edges COMMAND snx-test-llvm-promote-edges)
set_tests_properties(llvm_promote_edges PROPERTIES TIMEOUT 120)

add_executable(snx-test-llvm-promote-sync llvm_promote_sync.c llvm_steps.c)
snx_test_target(snx-test-llvm-promote-sync c23)
target_link_libraries(snx-test-llvm-promote-sync PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_promote_sync COMMAND snx-test-llvm-promote-sync)
add_test(NAME llvm_promote_sync_portable COMMAND snx-test-llvm-promote-sync --portable)
add_test(NAME llvm_promote_sync_abi COMMAND snx-test-llvm-promote-sync --abi-contract)
add_test(NAME llvm_promote_sync_abi_portable COMMAND snx-test-llvm-promote-sync --portable --abi-contract)

add_executable(snx-test-llvm-fp-reply-abi llvm_fp_reply_abi.c)
snx_test_target(snx-test-llvm-fp-reply-abi gnu23)
target_link_libraries(snx-test-llvm-fp-reply-abi PRIVATE snx_llvm snx_driver)
add_test(NAME llvm_fp_reply_abi COMMAND snx-test-llvm-fp-reply-abi)
set_tests_properties(llvm_fp_reply_abi PROPERTIES SKIP_RETURN_CODE 77)

add_executable(snx-test-a64-fp-arithmetic a64_fp_arithmetic_test.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp-arithmetic gnu23)
snx_cpu_helpers_flags(snx-test-a64-fp-arithmetic)
target_link_libraries(snx-test-a64-fp-arithmetic PRIVATE scarletnx-common) # RtAPI.cas128 (runtime/cas128.c)
if(NOT WIN32)
    target_link_libraries(snx-test-a64-fp-arithmetic PRIVATE m)
endif()
add_test(NAME a64_fp_arithmetic_witnesses COMMAND snx-test-a64-fp-arithmetic)

# the A64 decoder tools/decodetree generates from src/common/cpu/aarch64/decode/ (a64.decode and a64.profile)
include("${PROJECT_SOURCE_DIR}/cmake/decodetree.cmake")
add_executable(snx-test-a64-decode a64_decode_test.c)
snx_test_target(snx-test-a64-decode c23)
snx_decodetree_a64(snx-test-a64-decode)
add_test(NAME a64_decode COMMAND snx-test-a64-decode)
# a decoder of four rows (a64_decode_group/): a row whose body refuses a word hands it to the rows after it
add_executable(snx-test-a64-decode-group a64_decode_group.c)
snx_test_target(snx-test-a64-decode-group c23)
snx_decodetree_a64(snx-test-a64-decode-group PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/a64_decode_group/group.profile"
                   DECODE "${CMAKE_CURRENT_SOURCE_DIR}/a64_decode_group/group.decode")
add_test(NAME a64_decode_group COMMAND snx-test-a64-decode-group)
# a function that is not rising over a field too wide to try whole: refused, with the span and the limit
add_test(NAME a64_decode_unordered
         COMMAND snx-decodetree --a64-profile "${CMAKE_CURRENT_SOURCE_DIR}/a64_decode_group/unordered.profile"
                 --a64-header "${CMAKE_CURRENT_BINARY_DIR}/decodetree/unordered.h"
                 --a64-decoder "${CMAKE_CURRENT_BINARY_DIR}/decodetree/unordered.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/a64_decode_group/unordered.decode")
set_tests_properties(a64_decode_unordered PROPERTIES PASS_REGULAR_EXPRESSION
    "unordered.decode:7: %wide spans 0 to 2097151: !function=xor_2 is not rising, and a span is tried whole up to 1048576 values")
# --rows: a .decode file's patterns as a table of their fixed bits and their fields' (recompile_census draws words of it)
add_test(NAME decodetree_rows COMMAND snx-decodetree --rows=rows_fixture
         "${CMAKE_CURRENT_SOURCE_DIR}/decodetree_rows/rows.decode")
set_tests_properties(decodetree_rows PROPERTIES PASS_REGULAR_EXPRESSION
    "SnxDecodeRow rows_fixture\\[\\] = {[^{]*{0xfff00000u, 0x1a000000u, {0x0000f000u, 0x00000f00u, 0x000f00ffu}}, /\\* ADD \\*/[^{]*{0xffffff00u, 0xf0000000u, {0}}, /\\* NOP")


# sailgen (tools/sailgen/): --extract copies the closure of a root (its clause, what it calls, the enum it names) and
# nothing else; --generate refuses a throw it has no meaning for, with its file and line
include("${PROJECT_SOURCE_DIR}/cmake/sailgen.cmake")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sailgen/extract.sail"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/sailgen"
    COMMAND snx-sailgen --extract "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/fixture.roots"
            -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/extract.sail" "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/upstream.sail"
    DEPENDS snx-sailgen "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/fixture.roots"
            "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/upstream.sail" VERBATIM)
add_custom_target(snx_sailgen_extract_test ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sailgen/extract.sail")
add_test(NAME sailgen_extract COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol
         "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/extract_expected.sail" "${CMAKE_CURRENT_BINARY_DIR}/sailgen/extract.sail")
# GPU mode on the USSE's Sail with the 1 MiB of stack Windows gives a program's main thread (gpu.c's Walk)
if(UNIX AND NOT CMAKE_CROSSCOMPILING AND NOT SNX_TEST_SANITIZERS)
    set(sailgen_usse "${PROJECT_SOURCE_DIR}/src/vita/gpu/sail")
    file(GLOB sailgen_usse_sail CONFIGURE_DEPENDS "${sailgen_usse}/usse_*.sail")
    file(GLOB sailgen_usse_backends CONFIGURE_DEPENDS "${sailgen_usse}/usse_*_spirv.h")
    set(sailgen_usse_args --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/spirv.h" --backend "${sailgen_usse}/usse_spirv.h")
    foreach(backend IN LISTS sailgen_usse_backends)
        list(APPEND sailgen_usse_args --backend "${backend}")
    endforeach()
    add_test(NAME sailgen_stack COMMAND sh -c "ulimit -s 1024 && exec \"$0\" \"$@\"" $<TARGET_FILE:snx-sailgen>
        --gpu "${sailgen_usse}/usse.roots" ${sailgen_usse_args}
        -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/usse_stack.inc" "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sail/gpu.sail"
        "${sailgen_usse}/usse.sail" ${sailgen_usse_sail})
endif()
# --sdm reads pdftotext's layout of an instruction page (a footer's page number, a row whose operands a footnote's
# superscript pushed onto the line above, the stacked tables with a CPUID column) and puts an override's section in
# place of the page's; the fixture is written for the test, not the manual's text
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sailgen/sdm.pages"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/sailgen"
    COMMAND snx-sailgen --sdm "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/sdm_fixture.txt"
            -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/sdm.pages" --overlay "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/sdm_override.sdm"
    DEPENDS snx-sailgen "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/sdm_fixture.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/sdm_override.sdm" VERBATIM)
add_custom_target(snx_sailgen_sdm_test ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sailgen/sdm.pages")
add_test(NAME sailgen_sdm COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol
         "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/sdm_expected.pages" "${CMAKE_CURRENT_BINARY_DIR}/sailgen/sdm.pages")
# the x86-64 back end's instruction selection in the tree (tools/sailgen/sdm/x64_isel.inc) is what the Intel SDM gives:
# made again where its PDF is (SNX_SDM_PDF, else the CPM cache) and pdftotext is, skipped elsewhere, saying which it lacks
option(SNX_TEST_SDM "Download the Intel SDM and make x64_isel.inc again from it (needs pdftotext)" OFF)
if(Python3_Interpreter_FOUND)
    include("${PROJECT_SOURCE_DIR}/cmake/sdm.cmake")
    snx_sdm_pdf(snx_sdm_pdf_path snx_sdm_pdftotext)
    add_test(NAME sailgen_sdm_isel COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/sdm_isel_test.py"
             --sailgen $<TARGET_FILE:snx-sailgen> "--pdf=${snx_sdm_pdf_path}" "--pdftotext=${snx_sdm_pdftotext}")
    # CI's Linux x86-64 job, which has pdftotext, runs it in a step of its own: its poppler may be another tomorrow
    set_tests_properties(sailgen_sdm_isel PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS sdm)
elseif(SNX_TEST_SDM)
    message(FATAL_ERROR "SNX_TEST_SDM=${SNX_TEST_SDM} needs Python 3 (docs/dev/build.md)")
endif()
# --isel derives the selection from pages in the manual's notation (written for the test): ADD for an addition, SUB
# for SUBS with Arm's C the negation of CF and Arm's conditions as Intel's condition codes, CMP and SUB for CMP, SHL's
# loop for LSLV (its count mask Arm's MOD 64), SETcc on a zeroed register for CSET, and a page it can't read, with why
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sailgen/isel.report"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/sailgen"
    COMMAND snx-sailgen --isel "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/isel_fixture.pages"
            --ops "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/isel_fixture.ops" -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/isel.inc"
            --report "${CMAKE_CURRENT_BINARY_DIR}/sailgen/isel.report"
    DEPENDS snx-sailgen "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/isel_fixture.pages"
            "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/isel_fixture.ops" VERBATIM)
add_custom_target(snx_sailgen_isel_test ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sailgen/isel.report")
add_test(NAME sailgen_isel COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol
         "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/isel_expected.report" "${CMAKE_CURRENT_BINARY_DIR}/sailgen/isel.report")
# and over more primitives than its evaluations once had room for: every one evaluated
add_test(NAME sailgen_isel_widths
         COMMAND "${CMAKE_COMMAND}" "-DSAILGEN=$<TARGET_FILE:snx-sailgen>" "-DDIR=${CMAKE_CURRENT_SOURCE_DIR}/sailgen"
                 "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/sailgen/widths" -P "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/isel_widths_check.cmake")
set_tests_properties(sailgen_isel_widths PROPERTIES PASS_REGULAR_EXPRESSION "sailgen_isel_widths: 260 evaluations")
# --encoders of the in-tree a64.sail: the Arm ARM's words for fields of every kind, and each encoder's fields apart
add_executable(snx-test-sailgen-encoders sailgen_encoders.c)
snx_test_target(snx-test-sailgen-encoders c23)
snx_sailgen_a64_encoders(snx-test-sailgen-encoders)
add_test(NAME sailgen_encoders COMMAND snx-test-sailgen-encoders)
add_test(NAME sailgen_refused
         COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused.roots"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h" -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused.sail")
set_tests_properties(sailgen_refused PROPERTIES PASS_REGULAR_EXPRESSION
    "refused.sail:4: error: a throw of other than Error_Undefined\\(\\), Error_See\\(\\), Error_Unpredictable\\(\\) or Error_ConstrainedUnpredictable\\(\\)")
# a register, or a struct register's field, the back end does not keep: refused at the line that reads it, naming
# what the back end lacks
foreach(case IN ITEMS register field)
    add_test(NAME sailgen_refused_${case}
             COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_${case}.roots"
                     --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                     --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                     -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_${case}.inc"
                     "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_${case}.sail")
endforeach()
set_tests_properties(sailgen_refused_register PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_register.sail:6: error: the register DBGDTR_EL0: the back end llvm.h defines no sg_DBGDTR_EL0_read")
set_tests_properties(sailgen_refused_field PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_field.sail:12: error: PSTATE.D: the back end llvm.h defines no SG_PSTATE_D")
# the same two through a back end that only mentions those names (a comment, a string, a declaration, a call, an
# initializer): a name counts where it is defined
foreach(case IN ITEMS register field)
    add_test(NAME sailgen_refused_mention_${case}
             COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_${case}.roots"
                     --backend "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/mention_backend.h"
                     -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_mention_${case}.inc"
                     "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_${case}.sail")
endforeach()
set_tests_properties(sailgen_refused_mention_register PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_register.sail:6: error: the register DBGDTR_EL0: the back end mention_backend.h defines no sg_DBGDTR_EL0_read")
set_tests_properties(sailgen_refused_mention_field PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_field.sail:12: error: PSTATE.D: the back end mention_backend.h defines no SG_PSTATE_D")
# a word handed on to the next clause (Error_See) under a run-time condition, and a write to a field a let holds fixed
foreach(case IN ITEMS see pin)
    add_test(NAME sailgen_refused_${case}
             COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_${case}.roots"
                     --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                     --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                     -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_${case}.inc"
                     "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_${case}.sail")
endforeach()
set_tests_properties(sailgen_refused_see PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_see.sail:7: error: a throw of Error_See under a condition known only at run time")
set_tests_properties(sailgen_refused_pin PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_pin.sail:14: error: PSTATE.EL written, which the let PSTATE_EL holds fixed")
# an UNPREDICTABLE encoding is what a system's overlay says its cores do with it (UnpredictableEncoding()): without one,
# refused at the throw
add_test(NAME sailgen_refused_unpredictable
         COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_unpredictable.roots"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                 -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_unpredictable.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_unpredictable.sail")
set_tests_properties(sailgen_refused_unpredictable PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_unpredictable.sail:4: error: an UNPREDICTABLE encoding, and no UnpredictableEncoding\\(\\) of an overlay")
# a struct's field may take a value known when the recompiler runs, not one known only when the guest runs
add_test(NAME sailgen_refused_struct
         COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_struct.roots"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                 -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_struct.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_struct.sail")
set_tests_properties(sailgen_refused_struct PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_struct.sail:17: error: the field flag assigned a value known only at run time")
# the back end's function gives nothing, bits, a bool, an enum or a tuple of bits: one whose val gives an integer is
# refused where it is called
add_test(NAME sailgen_refused_action
         COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_action.roots"
                 --backend "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/action_backend.h"
                 -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_action.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_action.sail")
set_tests_properties(sailgen_refused_action PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_action.sail:8: error: Peek gives an integer: the back end's functions give nothing, bits, a bool, an enum or a tuple of bits")
# a function that calls itself on each half of its bits is read as the sum of the bits' elements, and only so: one
# that joins the halves' results by other than +, gives other than their sum, or that sum under a condition alone,
# halves other than each half, gives other than the element for one element or writes a register where it joins them
# is refused, naming it and why
foreach(case IN ITEMS op more some sides base state)
    add_test(NAME sailgen_refused_halves_${case}
             COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_halves_${case}.roots"
                     --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                     --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                     -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_halves_${case}.inc"
                     "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/halves.sail")
endforeach()
set_tests_properties(sailgen_refused_halves_op PROPERTIES PASS_REGULAR_EXPRESSION
    "halves.sail:91: error: Fold joins what it gives for each half of its bits other than by \\+ of the two")
set_tests_properties(sailgen_refused_halves_more PROPERTIES PASS_REGULAR_EXPRESSION
    "halves.sail:97: error: Fold gives other than the \\+ of what it gives for each half of its bits")
set_tests_properties(sailgen_refused_halves_some PROPERTIES PASS_REGULAR_EXPRESSION
    "halves.sail:122: error: Some gives other than the \\+ of what it gives for each half of its bits")
set_tests_properties(sailgen_refused_halves_sides PROPERTIES PASS_REGULAR_EXPRESSION
    "halves.sail:103: error: Lower calls itself: sailgen reads a function that gives its bits where they are one element wide")
set_tests_properties(sailgen_refused_halves_base PROPERTIES PASS_REGULAR_EXPRESSION
    "halves.sail:109: error: Base calls itself: sailgen reads a function that gives its bits where they are one element wide")
set_tests_properties(sailgen_refused_halves_state PROPERTIES PASS_REGULAR_EXPRESSION
    "halves.sail:62: error: the register GPR written where a function that calls itself joins what it gives for each half")
add_test(NAME sailgen_halves
         COMMAND "${CMAKE_COMMAND}" "-DSAILGEN=$<TARGET_FILE:snx-sailgen>"
                 "-DBITS=${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" "-DCORE=${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 "-DBACKEND=${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                 "-DDIR=${CMAKE_CURRENT_SOURCE_DIR}/sailgen"
                 "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/sailgen/halves.inc"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/halves_check.cmake")
set_tests_properties(sailgen_halves PROPERTIES PASS_REGULAR_EXPRESSION "sailgen_halves: sum=reduce")
# a function the back end lowers: --extract copies its val and the types it names, not its Sail; called under a
# run-time condition by a back end function that takes none, it is refused with its line
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sailgen/lower.sail"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/sailgen"
    COMMAND snx-sailgen --extract "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/lower.roots"
            -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/lower.sail" "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/lower.sail"
    DEPENDS snx-sailgen "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/lower.roots" "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/lower.sail"
    VERBATIM)
add_custom_target(snx_sailgen_lower_test ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sailgen/lower.sail")
add_test(NAME sailgen_extract_lower COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol
         "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/lower_expected.sail" "${CMAKE_CURRENT_BINARY_DIR}/sailgen/lower.sail")
add_test(NAME sailgen_refused_path
         COMMAND snx-sailgen --generate "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_path.roots"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                 -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/refused_path.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/refused_path.sail")
set_tests_properties(sailgen_refused_path PROPERTIES PASS_REGULAR_EXPRESSION
    "refused_path.sail:9: error: InstructionSynchronizationBarrier, the back end's action, under a condition known only at run time")
# an element loop is one vector operation only when its body sets locals per lane: one that carries a local from one
# element to the next, or whose element is read after it, stays a loop of elements
add_test(NAME sailgen_lanes
         COMMAND "${CMAKE_COMMAND}" "-DSAILGEN=$<TARGET_FILE:snx-sailgen>"
                 "-DBITS=${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" "-DCORE=${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 "-DBACKEND=${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                 "-DDIR=${CMAKE_CURRENT_SOURCE_DIR}/sailgen"
                 "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/sailgen/lanes.inc"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/sailgen/lanes_check.cmake")
set_tests_properties(sailgen_lanes PROPERTIES PASS_REGULAR_EXPRESSION
    "sailgen_lanes: lanes=lanes carried=loop last=loop pairs=lanes fold=lanes start=loop fixed=lanes free=loop")
# and the A64 subsets of src/common/cpu/aarch64/sail/ generate whole, with the NX's fixups
add_test(NAME sailgen_a64
         COMMAND snx-sailgen --generate "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail/a64.roots"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h"
                 --profile "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode/a64.profile"
                 -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/a64_sail.inc"
                 "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail/a64.sail"
                 --overlay "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail/nx.sail")
# and the AArch32 ones of src/common/cpu/arm/sail/, with the architectures' profile and fixups
add_test(NAME sailgen_a32
         COMMAND snx-sailgen --generate "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail/a32.roots"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/bits.h" --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/core.h"
                 --backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm_arm.h"
                 --profile "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail/a32.profile"
                 -o "${CMAKE_CURRENT_BINARY_DIR}/sailgen/a32_sail.inc"
                 "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail/a32.sail"
                 --overlay "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/sail/arm.sail")


# The QEMU reference (tests/qemu_ref/): hand-checked cases, the ported cases and the decoder's UNDEF sets under
# qemu-aarch64 -cpu max and -cpu cortex-a57. Skipped (ctest's skip, code 77) without qemu-aarch64, ld.lld or a Clang
# for AArch64
include("${PROJECT_SOURCE_DIR}/cmake/qemu_ref.cmake")
add_executable(snx-test-qemu-ref qemu_ref/selftest.c qemu_ref/qemu_ref.c)
snx_test_target(snx-test-qemu-ref c23)
target_include_directories(snx-test-qemu-ref PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-qemu-ref PRIVATE snx_test_common)
snx_decodetree_a64(snx-test-qemu-ref)
if(SNX_QEMU_REF)
    snx_qemu_ref_runner()
    add_dependencies(snx-test-qemu-ref snx_qemu_ref_runner)
    add_test(NAME qemu_ref_selftest COMMAND snx-test-qemu-ref "${SNX_QEMU_AARCH64}" "${SNX_QEMU_REF_RUNNER}"
             "${CMAKE_CURRENT_BINARY_DIR}/qemu_ref")
else()
    add_test(NAME qemu_ref_selftest COMMAND snx-test-qemu-ref --skip "${SNX_QEMU_REF_MISSING}")
endif()
set_tests_properties(qemu_ref_selftest PROPERTIES SKIP_RETURN_CODE 77)

# The online services (src/common/online): the WFC presets and the DS's slot, DNS queries and replies, the resolver
# against a DNS server of its own on 127.0.0.1, NASC's form and reply, and the 3DS console's files, which online_ctr
# writes in a folder of the build tree it empties first
if(TARGET snx_online)
    set(snx_online_ctr_folder "${CMAKE_CURRENT_BINARY_DIR}/online_ctr_console")
    foreach(part wfc dns resolve nasc ctr)
        add_executable(snx-test-online-${part} online_${part}.c)
        snx_test_target(snx-test-online-${part} c23)
        target_link_libraries(snx-test-online-${part} PRIVATE snx_online)
        add_test(NAME online_${part}
            COMMAND snx-test-online-${part} $<$<STREQUAL:${part},ctr>:${snx_online_ctr_folder}>)
        set_tests_properties(online_${part} PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME online_ctr_folder COMMAND "${CMAKE_COMMAND}" -E rm -rf "${snx_online_ctr_folder}")
    set_tests_properties(online_ctr_folder PROPERTIES FIXTURES_SETUP snx_online_ctr_folder)
    set_tests_properties(online_ctr PROPERTIES FIXTURES_REQUIRED snx_online_ctr_folder)
    if(SNX_TEST_ONLINE_LIVE) # the internet: how a service that stopped answering is found, not a gate
        add_executable(snx-test-online-presets-live online_presets_live.c)
        snx_test_target(snx-test-online-presets-live c23)
        target_link_libraries(snx-test-online-presets-live PRIVATE snx_online)
        add_test(NAME online_presets_live COMMAND snx-test-online-presets-live)
        set_tests_properties(online_presets_live PROPERTIES LABELS "external;network" TIMEOUT 120)
    endif()
endif()

# The emulated network stack over the host's loopback (src/common/network)
add_executable(snx-test-network-stack network_stack.c)
snx_test_target(snx-test-network-stack gnu23)
target_link_libraries(snx-test-network-stack PRIVATE scarletnx-net)
add_test(NAME network_stack COMMAND snx-test-network-stack)
set_tests_properties(network_stack PROPERTIES TIMEOUT 60)

add_executable(snx-test-network-icmp network_icmp.c)
snx_test_target(snx-test-network-icmp gnu23)
target_link_libraries(snx-test-network-icmp PRIVATE scarletnx-net)
add_test(NAME network_icmp COMMAND snx-test-network-icmp)
set_tests_properties(network_icmp PROPERTIES TIMEOUT 30)

# swipc (tools/swipc): the dispatchers generated from tests/swipc/test.swipc and other.swipc, driven with hand-made
# requests; the same output compiled as strict ISO C23 against a stand-in for ipc.h;
# and definitions the generator has to refuse, each with its line and message
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
# tools/hookgen's adapters (tests/hook/test.hook), and declarations it has to refuse, each with its line and message
include("${PROJECT_SOURCE_DIR}/cmake/hookgen.cmake")
add_executable(snx-test-hook-adapters hook_adapters.c)
snx_test_target(snx-test-hook-adapters c23)
target_include_directories(snx-test-hook-adapters PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-hook-adapters SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/test.hook" A32)
add_test(NAME hook_adapters COMMAND snx-test-hook-adapters)
# hook_install on a 32-bit game (src/nx/nn/hook.c) over tests/hook/a32_marked.hook: the hooks of the 32-bit
# program's table take their functions' slots of the code table, with the runtime stubbed
add_executable(snx-test-hook-install-a32 hook_install_a32.c hook_no_modules.c "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c")
snx_test_target(snx-test-hook-install-a32 gnu23)
target_link_libraries(snx-test-hook-install-a32 PRIVATE scarletnx-common) # jump.c flushes with snx_vm_flush_code
target_include_directories(snx-test-hook-install-a32 PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_service_swipc(snx-test-hook-install-a32)
snx_hookgen(snx-test-hook-install-a32 SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/a32_marked.hook" A32_MARKED)
add_test(NAME hook_install_a32 COMMAND snx-test-hook-install-a32)
# versioned hooks and struct layouts (tests/hook/versions.hook): the entries by version, the layout variant each
# adapter passes, the SDK labels a module's bytes hold and the entry hook_select picks for a module's version
add_executable(snx-test-hook-versions hook_versions.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
snx_test_target(snx-test-hook-versions c23)
target_include_directories(snx-test-hook-versions PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-hook-versions SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/versions.hook")
add_test(NAME hook_versions COMMAND snx-test-hook-versions)
# address hooks (tests/hook/address.hook), read as the second spec of one table after tests/hook/address_first.hook
add_executable(snx-test-hook-address hook_address.c)
snx_test_target(snx-test-hook-address c23)
target_include_directories(snx-test-hook-address PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-hook-address SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/address_first.hook"
            "${CMAKE_CURRENT_SOURCE_DIR}/hook/address.hook")
add_test(NAME hook_address COMMAND snx-test-hook-address)
# hook_install on a 64-bit game (src/nx/nn/hook.c) over tests/hook/install.hook, with the runtime stubbed: the
# hooks' slots, and what is left of them once a save state's g_hle took the running one's place
add_executable(snx-test-hook-install hook_install.c hook_no_modules.c "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c")
snx_test_target(snx-test-hook-install gnu23)
target_link_libraries(snx-test-hook-install PRIVATE scarletnx-common) # jump.c flushes with snx_vm_flush_code
target_include_directories(snx-test-hook-install PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_service_swipc(snx-test-hook-install)
snx_hookgen(snx-test-hook-install SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/install.hook" A32_MARKED)
add_test(NAME hook_install COMMAND snx-test-hook-install)
# the GL call census (src/nx/gpu/gl/gl_census.c), with the runtime stubbed: the driver's _gl exports and the
# addresses its eglGetProcAddress answers count the game's calls and still run, and the file's counts
add_executable(snx-test-gl-census gl_census.c)
snx_test_target(snx-test-gl-census gnu23)
target_link_libraries(snx-test-gl-census PRIVATE scarletnx-common)
target_include_directories(snx-test-gl-census PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME gl_census COMMAND snx-test-gl-census "${CMAKE_CURRENT_BINARY_DIR}/gl_census.tsv")
# the shared GL layer's state machine (src/common/gpu/gl/gl_state.c), with no GPU: contexts, the fixed state, GL's
# errors and clamps, and a save state's copy saved, loaded and refused
add_executable(snx-test-gl-state gl_state.c)
snx_test_target(snx-test-gl-state c23)
target_link_libraries(snx-test-gl-state PRIVATE snx_gl)
add_test(NAME gl_state COMMAND snx-test-gl-state)
# the shared GL layer's shaders and programs (src/common/gpu/gl/gl_program.c), with no GPU: compiled, linked, their
# uniforms and locations, GL's errors, deletes, a program pipeline, the arena, and a save state's copy loaded and refused
add_executable(snx-test-gl-program gl_program.c)
snx_test_target(snx-test-gl-program c23)
target_link_libraries(snx-test-gl-program PRIVATE snx_gl)
add_test(NAME gl_program COMMAND snx-test-gl-program)
# the shared GL layer's buffers and vertex arrays (src/common/gpu/gl/gl_object.c), with no GPU: their bytes in the
# platform's memory, maps, what the platform hears, vertex arrays, and a save state's copy loaded and refused
add_executable(snx-test-gl-object gl_object.c)
snx_test_target(snx-test-gl-object c23)
target_link_libraries(snx-test-gl-object PRIVATE snx_gl)
add_test(NAME gl_object COMMAND snx-test-gl-object)
# the shared GL layer's textures and samplers (src/common/gpu/gl/gl_texture.c), with no GPU: their storage, texels kept
# in the platform's memory by GL's unpack state, parameters, sampler objects, what the platform hears, and a save state
add_executable(snx-test-gl-texture gl_texture.c)
snx_test_target(snx-test-gl-texture c23)
target_link_libraries(snx-test-gl-texture PRIVATE snx_gl)
add_test(NAME gl_texture COMMAND snx-test-gl-texture)
# the shared GL layer's framebuffers and renderbuffers (src/common/gpu/gl/gl_framebuffer.c), with no GPU: bindings,
# storage, attachments and completeness, the window's parameters, what the platform hears, and a save state
add_executable(snx-test-gl-framebuffer gl_framebuffer.c)
snx_test_target(snx-test-gl-framebuffer c23)
target_link_libraries(snx-test-gl-framebuffer PRIVATE snx_gl)
add_test(NAME gl_framebuffer COMMAND snx-test-gl-framebuffer)
# the shared GL layer's sync objects (src/common/gpu/gl/gl_sync.c), with no GPU: fences of a platform's serials, waits,
# their parameters, GL's errors, and a save state's fences signaled
add_executable(snx-test-gl-sync gl_sync.c)
snx_test_target(snx-test-gl-sync c23)
target_link_libraries(snx-test-gl-sync PRIVATE snx_gl)
add_test(NAME gl_sync COMMAND snx-test-gl-sync)
# the console's GL programs as the renderer's host programs (src/nx/gpu/gl/gl_host_program.c), with no GPU: a program
# linked in the console's layout described as a translation's ShaderInfo
add_executable(snx-test-gl-host-program gl_host_program.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_host_program.c")
snx_test_target(snx-test-gl-host-program gnu23)
target_link_libraries(snx-test-gl-host-program PRIVATE snx_gl scarletnx-common)
target_include_directories(snx-test-gl-host-program PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME gl_host_program COMMAND snx-test-gl-host-program)
# the console's GL layer (src/nx/gpu/gl/) over a stubbed GL driver module, guest memory and renderer: the slots taken,
# EGL, glClear and the draws as the renderer's, eglSwapBuffers's present, a function without a handler
add_executable(snx-test-gl-layer gl_layer.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_loader.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_egl.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_calls.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_program_calls.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_object_calls.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_host_program.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tex.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_texture_calls.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_framebuffer_calls.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_sync_calls.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_draw.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tic.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/draw_state.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl_host_nx.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
snx_test_target(snx-test-gl-layer gnu23)
target_link_libraries(snx-test-gl-layer PRIVATE snx_gl scarletnx-common)
target_include_directories(snx-test-gl-layer PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-gl-layer SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/gl/gl.hook" PREFIX gl NAME gpu/gl/gl.hook)
add_test(NAME gl_layer COMMAND snx-test-gl-layer)
# native_cond_var (src/nx/nn/family.c, hook.c) over tests/hook/states.hook: the cond_var family's hooks at 1, the
# game's own wait at 0, and save states refused while the host's wait runs, by a message naming the setting's value
add_executable(snx-test-hook-states hook_states.c hook_no_modules.c "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c")
snx_test_target(snx-test-hook-states gnu23)
target_link_libraries(snx-test-hook-states PRIVATE scarletnx-common) # jump.c flushes with snx_vm_flush_code
target_include_directories(snx-test-hook-states PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_service_swipc(snx-test-hook-states)
snx_hookgen(snx-test-hook-states SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/states.hook" A32_MARKED)
add_test(NAME hook_states COMMAND snx-test-hook-states)
# hook_install with a hook module named after install.hook's plain family (tests/hook_module_plain.c, nn/module.c):
# its code check holding, the family's hook goes in. Failing, or with hooks_off naming it, the function keeps its own
# code
foreach(snx_case IN ITEMS applied refused off)
    add_executable(snx-test-hook-module-${snx_case} hook_install.c hook_module_plain.c
                   "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c"
                   "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c")
    snx_test_target(snx-test-hook-module-${snx_case} gnu23)
    target_link_libraries(snx-test-hook-module-${snx_case} PRIVATE scarletnx-common)
    target_include_directories(snx-test-hook-module-${snx_case} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_service_swipc(snx-test-hook-module-${snx_case})
    snx_hookgen(snx-test-hook-module-${snx_case} SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/install.hook" A32_MARKED)
    add_test(NAME hook_module_${snx_case} COMMAND snx-test-hook-module-${snx_case})
endforeach()
target_compile_definitions(snx-test-hook-module-applied PRIVATE HOOK_MODULE_TEST=1)
target_compile_definitions(snx-test-hook-module-refused PRIVATE HOOK_MODULE_TEST=2)
target_compile_definitions(snx-test-hook-module-off PRIVATE HOOK_MODULE_TEST=3)
# hook modules from libraries (nn/module_lib.c, nx/loader/hook_libs.c): synthetic module libraries of no game, one
# per case from tests/hook_module_lib_mod.c and built against nn/snx_hook_module.h alone, loaded and refused as the
# runtime loads them, and which game their modules apply to. snx-test-hook-module-lib --load PATH prints what the
# runtime makes of any library
set(snx_hx_libs "${CMAKE_CURRENT_BINARY_DIR}/hook_module_libs")
set(snx_hx_targets)
foreach(snx_hx IN ITEMS hx_good:1:/folder hx_off:2:/folder hx_major:3: hx_minor:4: hx_no_entry:5: hx_null:6:
                        hx_misnamed:7: hx_empty:8: hx-dash:3:)
    string(REPLACE ":" ";" snx_hx "${snx_hx}")
    list(GET snx_hx 0 snx_hx_name)
    list(GET snx_hx 1 snx_hx_case)
    list(LENGTH snx_hx snx_hx_parts)
    set(snx_hx_folder "")
    if(snx_hx_parts GREATER 2)
        list(GET snx_hx 2 snx_hx_folder)
    endif()
    add_library(snx-test-${snx_hx_name} MODULE hook_module_lib_mod.c)
    snx_test_target(snx-test-${snx_hx_name} 11)
    target_include_directories(snx-test-${snx_hx_name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(snx-test-${snx_hx_name} PRIVATE HX_CASE=${snx_hx_case})
    set_target_properties(snx-test-${snx_hx_name} PROPERTIES PREFIX "" OUTPUT_NAME ${snx_hx_name}
                          SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
                          LIBRARY_OUTPUT_DIRECTORY "${snx_hx_libs}${snx_hx_folder}")
    list(APPEND snx_hx_targets snx-test-${snx_hx_name})
endforeach()
add_executable(snx-test-hook-module-lib hook_module_lib.c "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/module_lib.c")
snx_test_target(snx-test-hook-module-lib 11)
add_dependencies(snx-test-hook-module-lib snx_settings ${snx_hx_targets})
target_include_directories(snx-test-hook-module-lib PRIVATE "${PROJECT_SOURCE_DIR}/src"
                           "${PROJECT_BINARY_DIR}/settingsgen")
target_compile_definitions(snx-test-hook-module-lib PRIVATE HX_LIBS="${snx_hx_libs}"
                           HX_SUFFIX="${CMAKE_SHARED_LIBRARY_SUFFIX}")
target_link_libraries(snx-test-hook-module-lib PRIVATE snx_nx_module scarletnx-common m)
add_test(NAME hook_module_lib COMMAND snx-test-hook-module-lib)
# the libraries' CPU hooks: hook_install (src/nx/nn/hook.c, its adapter of the libraries'
# hooks) over libraries of no game built from tests/hook_lib_cpu_mod.c, the recompile's route keys
# (nx/loader/hook_libs.c), the hook the route list leaves out, and the handlers' CPU, memory, own code and upscale
set(snx_hx_cpu_targets)
foreach(snx_hx IN ITEMS hx_cpu:1:/cpu hx_cpu_title:2:/cpu hx_cpu_full:5:/cpu hx_cpu_nobuild:3:/cpu_bad
                        hx_cpu_odd:4:/cpu_bad)
    string(REPLACE ":" ";" snx_hx "${snx_hx}")
    list(GET snx_hx 0 snx_hx_name)
    list(GET snx_hx 1 snx_hx_case)
    list(GET snx_hx 2 snx_hx_folder)
    add_library(snx-test-${snx_hx_name} MODULE hook_lib_cpu_mod.c)
    snx_test_target(snx-test-${snx_hx_name} 11)
    target_include_directories(snx-test-${snx_hx_name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(snx-test-${snx_hx_name} PRIVATE HX_CASE=${snx_hx_case})
    set_target_properties(snx-test-${snx_hx_name} PROPERTIES PREFIX "" OUTPUT_NAME ${snx_hx_name}
                          SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
                          LIBRARY_OUTPUT_DIRECTORY "${snx_hx_libs}${snx_hx_folder}")
    list(APPEND snx_hx_cpu_targets snx-test-${snx_hx_name})
endforeach()
add_executable(snx-test-hook-lib-cpu hook_lib_cpu.c "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/module_lib.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c")
snx_test_target(snx-test-hook-lib-cpu 11)
add_dependencies(snx-test-hook-lib-cpu snx_settings ${snx_hx_cpu_targets})
target_include_directories(snx-test-hook-lib-cpu PRIVATE "${PROJECT_SOURCE_DIR}/src"
                           "${PROJECT_BINARY_DIR}/settingsgen")
target_compile_definitions(snx-test-hook-lib-cpu PRIVATE HX_LIBS="${snx_hx_libs}"
                           HX_SUFFIX="${CMAKE_SHARED_LIBRARY_SUFFIX}")
target_link_libraries(snx-test-hook-lib-cpu PRIVATE snx_nx_module scarletnx-common)
snx_service_swipc(snx-test-hook-lib-cpu)
snx_hookgen(snx-test-hook-lib-cpu SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/install.hook" A32_MARKED)
add_test(NAME hook_lib_cpu COMMAND snx-test-hook-lib-cpu)
# the jumps written over an invalidated native hook's start (src/nx/nn/jump.c), every architecture's bytes
# against what llvm-mc disassembles them to, on any host
add_executable(snx-test-hook-jump hook_jump.c "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c")
snx_test_target(snx-test-hook-jump gnu23)
target_link_libraries(snx-test-hook-jump PRIVATE scarletnx-common) # jump.c flushes with snx_vm_flush_code
target_include_directories(snx-test-hook-jump PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME hook_jump COMMAND snx-test-hook-jump)
# a native hook the game invalidates (src/nx/nn/hook.c): its adapter rewritten into a jump to the game's code, on
# this test's own x86_64 code, and the mod's trampolines that an ext_skyline hook (tests/hook/ext_skyline.hooks) takes;
# skipped on another host
add_executable(snx-test-hook-invalidated hook_invalidated.c hook_no_modules.c "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/ext_skyline.c")
snx_test_target(snx-test-hook-invalidated gnu23)
target_link_libraries(snx-test-hook-invalidated PRIVATE scarletnx-common) # jump.c flushes with snx_vm_flush_code
target_include_directories(snx-test-hook-invalidated PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_service_swipc(snx-test-hook-invalidated)
snx_hookgen(snx-test-hook-invalidated SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/install.hook" A32_MARKED)
snx_hookgen(snx-test-hook-invalidated SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/ext_skyline.hooks" PREFIX skyline
            NAME nn/ext_skyline.hooks)
add_test(NAME hook_invalidated COMMAND snx-test-hook-invalidated)
set_tests_properties(hook_invalidated PROPERTIES SKIP_RETURN_CODE 77)
# hook_install over a full table (src/nx/nn/hook.c), the spec written here with as many hooks as tools/hookgen
# takes (its MAX_ENTRIES): one module exporting every function gets each one hooked, and a second exporting them again
# runs out of sites, which stops the game naming the first function left without its hook
set(full_spec "family full\n")
foreach(i RANGE 1023)
    string(APPEND full_spec "hook full_${i}() = test_full\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hook/full.hook" CONTENT "${full_spec}")
add_executable(snx-test-hook-sites hook_sites.c hook_no_modules.c "${PROJECT_SOURCE_DIR}/src/nx/nn/module.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/jump.c")
snx_test_target(snx-test-hook-sites gnu23)
target_link_libraries(snx-test-hook-sites PRIVATE scarletnx-common) # jump.c flushes with snx_vm_flush_code
target_include_directories(snx-test-hook-sites PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_service_swipc(snx-test-hook-sites)
snx_hookgen(snx-test-hook-sites SPEC "${CMAKE_CURRENT_BINARY_DIR}/hook/full.hook" A32_MARKED)
add_test(NAME hook_sites COMMAND snx-test-hook-sites)
add_test(NAME hook_sites_twice COMMAND snx-test-hook-sites twice)
set_tests_properties(hook_sites_twice PROPERTIES
    PASS_REGULAR_EXPRESSION "fatal: hooks: no site left for full_0 at 2000: the runtime holds 1024, one for each hook")
# the route list (src/nx/loader/hook_routes.c): hookgen --routes over three specs (tests/hook/routes.hook,
# versions.hook and address.hook), the runtime's own list, its lookup and the hashes a module carries, and the build
# IDs an address hook's key names
add_executable(snx-test-hook-routes hook_routes.c)
snx_test_target(snx-test-hook-routes c23)
target_link_libraries(snx-test-hook-routes PRIVATE snx_test_common snx_nx_module)
snx_hook_routes(snx-test-hook-routes SPECS "${CMAKE_CURRENT_SOURCE_DIR}/hook/routes.hook"
                "${CMAKE_CURRENT_SOURCE_DIR}/hook/versions.hook" "${CMAKE_CURRENT_SOURCE_DIR}/hook/address.hook")
add_test(NAME hook_routes COMMAND snx-test-hook-routes)
# verify mode (src/nx/nn/verify.c): pure hooks (tests/hook/verify.hook) checked against stand-ins for the
# function's own code, on copies of their buffers, and each kind of difference it reports
add_executable(snx-test-hook-verify hook_verify.c "${PROJECT_SOURCE_DIR}/src/nx/nn/verify.c")
snx_test_target(snx-test-hook-verify c23)
target_include_directories(snx-test-hook-verify PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-hook-verify SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/verify.hook")
add_test(NAME hook_verify COMMAND snx-test-hook-verify)
# nn::diag's abort hook (src/nx/nn/diag.c) on a fake abort: the values it logs, then its own code run
# libc_std.c too: the table names its handlers, which run on the host's C library alone
add_executable(snx-test-diag-abort diag_abort.c "${PROJECT_SOURCE_DIR}/src/nx/nn/diag.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/libc_std.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/libc_wide.c"
               "${PROJECT_SOURCE_DIR}/src/nx/nn/libc_fmt.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/libcxx_string.c")
snx_test_target(snx-test-diag-abort gnu23) # log.h has _Thread_local
target_include_directories(snx-test-diag-abort PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-diag-abort SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
add_test(NAME diag_abort COMMAND snx-test-diag-abort)
# std::string's hooks (src/nx/nn/libcxx_string.c) on libc++'s layout in guest memory
add_executable(snx-test-libcxx-string libcxx_string.c "${PROJECT_SOURCE_DIR}/src/nx/nn/libcxx_string.c")
snx_test_target(snx-test-libcxx-string gnu23)
target_include_directories(snx-test-libcxx-string PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-libcxx-string SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
add_test(NAME libcxx_string COMMAND snx-test-libcxx-string)
# std::vector's storage hook (src/nx/nn/alloc.c): the destructor shapes alloc_vector_shape takes and leaves, and
# alloc_vector_free's empty vector and foreign pointer
if(NOT WIN32)
    add_executable(snx-test-alloc-vector alloc_vector.c "${PROJECT_SOURCE_DIR}/src/nx/nn/alloc.c")
    snx_test_target(snx-test-alloc-vector gnu23)
    target_include_directories(snx-test-alloc-vector PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_hookgen(snx-test-alloc-vector SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
    add_test(NAME alloc_vector COMMAND snx-test-alloc-vector)
endif()
# the sync family's clocks (src/nx/nn/sync.c) against the game's (src/nx/runtime/clock.c): steady_clock stopped
# by a pause and set by a state's load, system_clock the host's; and the cond_var family's C library waits
add_executable(snx-test-sync-clock sync_clock.c "${PROJECT_SOURCE_DIR}/src/nx/nn/sync.c"
               "${PROJECT_SOURCE_DIR}/src/nx/runtime/clock.c")
snx_test_target(snx-test-sync-clock gnu23)
target_include_directories(snx-test-sync-clock PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-sync-clock PRIVATE scarletnx-platform)
snx_hookgen(snx-test-sync-clock SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
add_test(NAME sync_clock COMMAND snx-test-sync-clock)
# the os_light family's LightEvent handlers (src/nx/nn/os_sync.c) on each SDK's layout of nn::os::LightEventType,
# against the bytes the SDK's own InitializeLightEvent leaves
add_executable(snx-test-os-light-event os_light_event.c)
snx_test_target(snx-test-os-light-event gnu23)
target_include_directories(snx-test-os-light-event PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-os-light-event PRIVATE scarletnx-platform)
snx_hookgen(snx-test-os-light-event SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
# its handlers on the layouts of the table hookgen makes, which it reads; os_sync.c's split symbol names are deliberate
target_compile_definitions(snx-test-os-light-event PRIVATE
    OS_LIGHT_INC="${CMAKE_CURRENT_BINARY_DIR}/hookgen/snx-test-os-light-event/nn/hooks.hook.inc")
target_compile_options(snx-test-os-light-event PRIVATE -Wno-string-concatenation)
add_test(NAME os_light_event COMMAND snx-test-os-light-event)
# the alloc family's arena (src/nx/nn/alloc.c) in a program that links jemalloc as scarletnx does, whole and
# unexported: ready, and its allocations in the guest's window above the layout; the SDK heap's functions on its
# blocks, each in the runtime's route list (snx_nx_module's)
if(SNX_JEMALLOC AND NOT WIN32)
    add_executable(snx-test-alloc-arena alloc_arena.c "${PROJECT_SOURCE_DIR}/src/nx/nn/alloc.c")
    snx_test_target(snx-test-alloc-arena gnu23)
    target_include_directories(snx-test-alloc-arena PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(snx-test-alloc-arena PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,snx_jemalloc>" snx_nx_module)
    snx_hookgen(snx-test-alloc-arena SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
    target_compile_definitions(snx-test-alloc-arena PRIVATE LIVE_BITS=8) # a table that fills in a few hundred blocks
    add_test(NAME alloc_arena COMMAND snx-test-alloc-arena)
    set_tests_properties(alloc_arena PROPERTIES TIMEOUT 60)
endif()
# operator new's check (src/nx/nn/alloc.c's alloc_new_shape): a module whose operator delete looks the block's
# allocator up by its address and copies that code where no hook takes it, inlined or to a function no symbol names,
# keeps its own operator new; the deletes' own bodies, the family's functions, loads spread wide, a forwarder, and a
# copied delete without the lookup (a heap's, by the size, on a link-time table with one global) do not count. Nor does
# a delete that asks for its heap without giving it the block, beside an operator new that calls an object through its
# table; one that asks by the block does, and the module keeps its own operator new
if(NOT WIN32)
    add_executable(snx-test-alloc-new-shape alloc_new_shape.c "${PROJECT_SOURCE_DIR}/src/nx/nn/alloc.c")
    snx_test_target(snx-test-alloc-new-shape gnu23)
    target_include_directories(snx-test-alloc-new-shape PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_hookgen(snx-test-alloc-new-shape SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
    add_test(NAME alloc_new_shape COMMAND snx-test-alloc-new-shape)
endif()
# a game's own allocator (src/nx/nn/alloc.c's alloc_object_call and alloc_kept): a malloc, operator new or delete
# that calls an allocator object through its table with the object in x0 is one; a PLT stub, a function pointer in a
# global, a table no object is passed, the object elsewhere or overwritten, a direct call and a virtual call past the
# function's end are not. A program whose malloc is one has the family off; one whose malloc is the SDK allocator's
# keeps it, though its operator new and free call objects
if(NOT WIN32)
    add_executable(snx-test-alloc-kept alloc_kept.c "${PROJECT_SOURCE_DIR}/src/nx/nn/alloc.c")
    snx_test_target(snx-test-alloc-kept gnu23)
    target_include_directories(snx-test-alloc-kept PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_hookgen(snx-test-alloc-kept SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
    add_test(NAME alloc_kept_off COMMAND snx-test-alloc-kept off)
    add_test(NAME alloc_kept_on COMMAND snx-test-alloc-kept on)
endif()
# the C library's functions on guest memory (src/nx/nn/libc_std.c): guest addresses back from pointers into an
# argument, end pointers, strtok's place, div_t and ldiv_t, errno, qsort and bsearch through the guest's comparison
if(NOT WIN32)
    add_executable(snx-test-libc-std libc_std.c "${PROJECT_SOURCE_DIR}/src/nx/nn/libc_std.c"
                   "${PROJECT_SOURCE_DIR}/src/nx/nn/libc_wide.c" "${PROJECT_SOURCE_DIR}/src/nx/nn/libc_fmt.c")
    snx_test_target(snx-test-libc-std gnu23)
    target_include_directories(snx-test-libc-std PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_hookgen(snx-test-libc-std SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
    add_test(NAME libc_std COMMAND snx-test-libc-std)
endif()
# the event a hooked SDK object attaches to the game's SystemEvent (src/nx/nn/guest_call.c's hook_attach_event):
# its references, with the kernel's objects stubbed to their counts
add_executable(snx-test-hook-event-reference hook_event_reference.c)
snx_test_target(snx-test-hook-event-reference gnu23)
target_include_directories(snx-test-hook-event-reference PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_service_swipc(snx-test-hook-event-reference)
add_test(NAME hook_event_reference COMMAND snx-test-hook-event-reference)
# the hooks' NVN key (tests/hook/nvn_versions.hook): the API version read from a driver's nvnDeviceGetInteger
# (src/nx/gpu/nvn/nvn_driver.c), the entry hook_select picks for it, and the versions the log calls described
add_executable(snx-test-nvn-versions nvn_versions.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_driver.c")
snx_test_target(snx-test-nvn-versions c23)
target_include_directories(snx-test-nvn-versions PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-nvn-versions SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/nvn_versions.hook" PREFIX nvn
    NAME gpu/nvn/nvn_versions.hook)
add_test(NAME nvn_versions COMMAND snx-test-nvn-versions)
# nvn's command buffers: recordings across segments, and CopyCommands' copies of them
add_executable(snx-test-nvn-commands nvn_commands.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_cmd.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c")
snx_test_target(snx-test-nvn-commands gnu23)
target_include_directories(snx-test-nvn-commands PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_hookgen(snx-test-nvn-commands SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
add_test(NAME nvn_commands COMMAND snx-test-nvn-commands)
# nvn's constant banks and sprite origin against the game's driver: nvn_draw_init, a program's bind
# and a draw's dynamic state, through the public records
add_executable(snx-test-nvn-draw-banks nvn_draw_banks.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_draw.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c")
snx_test_target(snx-test-nvn-draw-banks gnu23)
target_include_directories(snx-test-nvn-draw-banks PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-nvn-draw-banks PRIVATE m)
snx_hookgen(snx-test-nvn-draw-banks SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
add_test(NAME nvn_draw_banks COMMAND snx-test-nvn-draw-banks)
add_executable(snx-test-nvn-advanced-blend nvn_advanced_blend.c)
snx_test_target(snx-test-nvn-advanced-blend gnu23)
target_include_directories(snx-test-nvn-advanced-blend PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-nvn-advanced-blend PRIVATE m)
add_test(NAME nvn_advanced_blend COMMAND snx-test-nvn-advanced-blend)
# nvn's copies between textures of two formats: the 2D engine's converting blit, as the game's driver copies
add_executable(snx-test-nvn-copy-formats nvn_copy_formats.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_copy.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c")
snx_test_target(snx-test-nvn-copy-formats gnu23)
target_include_directories(snx-test-nvn-copy-formats PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-nvn-copy-formats PRIVATE m)
snx_hookgen(snx-test-nvn-copy-formats SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
add_test(NAME nvn_copy_formats COMMAND snx-test-nvn-copy-formats)
# a window's present transform against the game's driver: lower left is queued to vi flipped
add_executable(snx-test-nvn-present-flip nvn_present_flip.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_queue.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c")
snx_test_target(snx-test-nvn-present-flip gnu23)
target_include_directories(snx-test-nvn-present-flip PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-nvn-present-flip PRIVATE m)
snx_hookgen(snx-test-nvn-present-flip SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
add_test(NAME nvn_present_flip COMMAND snx-test-nvn-present-flip)
# the NvRm sync conversions against the driver's sync layout: what they hand its code and take back
add_executable(snx-test-nvn-nvrm-sync nvn_nvrm_sync.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_object.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sm86_decode.c")
snx_test_target(snx-test-nvn-nvrm-sync gnu23)
target_include_directories(snx-test-nvn-nvrm-sync PRIVATE "${PROJECT_SOURCE_DIR}/src")
# sm86_decode.c's decoder, which tools/decodetree makes from sm86.decode
include("${PROJECT_SOURCE_DIR}/cmake/decodetree.cmake")
snx_decodetree(snx-test-nvn-nvrm-sync OUTPUT decode-sm86.inc DECODE "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sm86.decode"
               OPTIONS -w 16 --static-decode=decode_sm86)
target_link_libraries(snx-test-nvn-nvrm-sync PRIVATE m)
snx_hookgen(snx-test-nvn-nvrm-sync SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
add_test(NAME nvn_nvrm_sync COMMAND snx-test-nvn-nvrm-sync)
add_executable(snx-test-nvn-queue-lifetime nvn_queue_lifetime.c
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_replay.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/hle.c"
    own_stub.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c")
snx_test_target(snx-test-nvn-queue-lifetime gnu23)
add_dependencies(snx-test-nvn-queue-lifetime snx_settings)
target_include_directories(snx-test-nvn-queue-lifetime PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/settingsgen")
add_test(NAME nvn_queue_lifetime COMMAND snx-test-nvn-queue-lifetime)
# the replay with gpu_split's renderer thread: a fence and a present reach the renderer after the draws before them (its platform
# stand-ins are pthreads)
add_executable(snx-test-nvn-replay-split nvn_replay_split.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_replay.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/hle.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/renderer.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/gpu_queue.c" own_stub.c)
snx_test_target(snx-test-nvn-replay-split gnu23)
target_compile_options(snx-test-nvn-replay-split PRIVATE -Wno-missing-field-initializers) # renderer.c's records
add_dependencies(snx-test-nvn-replay-split snx_settings)
target_link_libraries(snx-test-nvn-replay-split PRIVATE scarletnx-platform) # gpu_queue.c times the waits
target_include_directories(snx-test-nvn-replay-split PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/settingsgen")
add_test(NAME nvn_replay_split COMMAND snx-test-nvn-replay-split)
set_tests_properties(nvn_replay_split PROPERTIES TIMEOUT 30) # a wait in the queue that never ends would hang ctest
# the NVN functions of API 53 and 55 nvn took over last, by family: records and their replay into the draw
# state (nvn_cmd.c, nvn_draw.c), the objects' getters and setters (nvn_object.c) and the queue and window (nvn_queue.c)
add_executable(snx-test-nvn-names nvn_names.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_cmd.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_draw.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_object.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_queue.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tex.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_tic.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn_state.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sm86_decode.c")
snx_test_target(snx-test-nvn-names gnu23)
target_include_directories(snx-test-nvn-names PRIVATE "${PROJECT_SOURCE_DIR}/src")
# sm86_decode.c's decoder, which tools/decodetree makes from sm86.decode
snx_decodetree(snx-test-nvn-names OUTPUT decode-sm86.inc DECODE "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sm86.decode"
               OPTIONS -w 16 --static-decode=decode_sm86)
target_link_libraries(snx-test-nvn-names PRIVATE m)
snx_hookgen(snx-test-nvn-names SPEC "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn/nvn.hook" PREFIX nvn NAME gpu/nvn/nvn.hook A32)
add_test(NAME nvn_names COMMAND snx-test-nvn-names)
foreach(bad IN ITEMS
        unknown_type 2 "unknown type 'u128'"
        stack_buffer 2 "i is on the stack, which a buffer can't be"
        duplicate_symbol 4 "f is hooked on line 2 too"
        handler_mismatch 3 "h takes other arguments on line 2"
        argument_c 2 "'c' can't name an argument"
        no_family 1 "after a family line"
        family_twice 2 "family a is named twice"
        version_overlap 4 "f's versions overlap line 3's"
        version_mixed 3 "f is declared without @version on line 2"
        version_keys 3 "f is versioned by sdk on line 2"
        version_key 2 "unknown version key 'gpu'"
        version_malformed 2 "malformed version '12.x'"
        version_empty 2 "no version satisfies this @version"
        version_dangling 2 "@version goes before a hook or a struct"
        struct_field_type 2 "struct S's field a is a u64, and a u32 on line 1"
        struct_field_missing 2 "struct S has no field b, which line 1's has"
        struct_overlap 2 "struct S's versions overlap line 1's"
        struct_mixed 3 "struct S is declared without @version on line 1"
        struct_past_end 3 "field a runs past the end of struct S"
        uses_unknown 2 "unknown struct 'T'"
        uses_uncovered 3 "no declaration of struct S holds for f's versions"
        buffer_length 2 "p's length names n, which isn't an argument"
        buffer_impure 2 "only a pure hook's buffers are checked"
        buffer_type 2 "only an addr or a pointer argument is a buffer, not p"
        buffer_length_type 2 "p's length q is not an integer"
        route_twice 3 "f is routed on line 1 too"
        route_hooked 3 "f is hooked on line 2, which routes it already"
        route_versioned 1 "@version goes before a hook or a struct"
        argument_array 2 "v is an array, which the language doesn't take"
        pointer_unknown 4 "unknown type 'NVNtextur'"
        by_value_struct 3 "NVNtexture is passed by value, but it isn't nx/gpu/nvn/nvn.h's enum or pointer"
        const_value 2 "const i32 has no '\\*' after it"
        include_missing 2 "cannot find nx/none.h"
        function_type_versions 4 "nvnQueueFinish takes other types on line 3"
        handler_underived 2 "memcpy names no handler"
        handler_derived 2 "nvn_DeviceFinalize is the handler that nvnDeviceFinalize names already"
        address_versioned 2 "an address hook is for one build of its module: it has no @version"
        address_handler 2 "an address hook's name is its handler's"
        address_unaligned 2 "offset 0x42 isn't a function's"
        address_short 2 "a build ID is an even number of hex digits, 16 to 64"
        address_zero 2 "a build ID of zeros names no build"
        address_twice 4 "0123456789abcdef:40 is hooked on line 2 too"
        address_keyword 2 "expected 'hook' after the address"
        a32_uses 4 "a hook that holds for a 32-bit program too uses no struct"
        a32_twice 4 "f is hooked on line 3 too"
        a32_word_twice 3 "the a32 before hook says it already"
        a32_keyword 3 "expected 'hook' after a32"
        a32_pure 3 "verify mode checks no hook of a 32-bit program"
        a32_versions_overlap 4 "f's versions overlap line 3's"
        a32_versions_malformed 3 "malformed version '2.x'")
    if(NOT DEFINED bad_name)
        set(bad_name "${bad}")
    elseif(NOT DEFINED bad_line)
        set(bad_line "${bad}")
    else()
        add_test(NAME hookgen_refuses_${bad_name} COMMAND snx-hookgen -I "${PROJECT_SOURCE_DIR}/src"
                 --out "${CMAKE_CURRENT_BINARY_DIR}/refused.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/hook/bad/${bad_name}.hook")
        set_tests_properties(hookgen_refuses_${bad_name} PROPERTIES
                             PASS_REGULAR_EXPRESSION "${bad_name}.hook:${bad_line}: .*${bad}")
        unset(bad_name)
        unset(bad_line)
    endif()
endforeach()
# a declaration for a 32-bit program alone, with --a32, which gives that program every hook's declaration
add_test(NAME hookgen_refuses_a32_all COMMAND snx-hookgen --a32 --out "${CMAKE_CURRENT_BINARY_DIR}/refused.inc"
         "${CMAKE_CURRENT_SOURCE_DIR}/hook/bad/a32_all.hook")
set_tests_properties(hookgen_refuses_a32_all PROPERTIES
                     PASS_REGULAR_EXPRESSION "a32_all.hook:4: .*one for it alone needs --a32-marked")
# NID hooks (hookgen --vita) it has to refuse, and one without --vita, where a bracket holds an address hook's key
foreach(bad IN ITEMS
        vita_address 3 "an address hook's .BUILDID:OFFSET. is a Switch program's"
        vita_handler 3 "a NID hook names its handler"
        vita_twice 4 "00000001 is hooked on line 3 too"
        vita_symbol 3 "--vita takes nothing else"
        vita_long 3 "a NID is at most 8 hex digits"
        vita_variadic 3 "... ends the arguments"
        vita_a32 3 "a NID hook has no uses, check, pure or a32")
    if(NOT DEFINED bad_name)
        set(bad_name "${bad}")
    elseif(NOT DEFINED bad_line)
        set(bad_line "${bad}")
    else()
        add_test(NAME hookgen_refuses_${bad_name} COMMAND snx-hookgen --vita
                 --out "${CMAKE_CURRENT_BINARY_DIR}/refused.inc"
                 "${CMAKE_CURRENT_SOURCE_DIR}/hook/bad/${bad_name}.hook")
        set_tests_properties(hookgen_refuses_${bad_name} PROPERTIES
                             PASS_REGULAR_EXPRESSION "${bad_name}.hook:${bad_line}: .*${bad}")
        unset(bad_name)
        unset(bad_line)
    endif()
endforeach()
add_test(NAME hookgen_refuses_vita_without COMMAND snx-hookgen --out "${CMAKE_CURRENT_BINARY_DIR}/refused.inc"
         "${CMAKE_CURRENT_SOURCE_DIR}/hook/bad/vita_without.hook")
set_tests_properties(hookgen_refuses_vita_without PROPERTIES
                     PASS_REGULAR_EXPRESSION "vita_without.hook:3: .*a build ID is an even number of hex digits")
# a prefix that makes a generated line longer than the generator's line holds: refused, where it was written cut
string(REPEAT "p" 200 snx_hookgen_long_prefix)
add_test(NAME hookgen_refuses_long_line COMMAND snx-hookgen --prefix "${snx_hookgen_long_prefix}"
         -I "${PROJECT_SOURCE_DIR}/src" --header "${CMAKE_CURRENT_BINARY_DIR}/refused.h"
         --out "${CMAKE_CURRENT_BINARY_DIR}/refused.inc" "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
set_tests_properties(hookgen_refuses_long_line PROPERTIES
                     PASS_REGULAR_EXPRESSION "a generated line of [0-9]+ bytes passes the 1023 a line holds")
add_executable(snx-test-swipc-dispatch swipc_dispatch.c)
snx_test_target(snx-test-swipc-dispatch gnu23)
snx_swipc(snx-test-swipc-dispatch ROOT "${PROJECT_SOURCE_DIR}" FILES tests/swipc/test.swipc tests/swipc/other.swipc)
add_test(NAME swipc_dispatch COMMAND snx-test-swipc-dispatch)
add_library(snx-test-swipc-iso OBJECT swipc/iso/iso.c)
set_target_properties(snx-test-swipc-iso PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx-test-swipc-iso PRIVATE -pedantic-errors -Wall -Wextra -Werror)
target_include_directories(snx-test-swipc-iso BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/swipc/iso")
snx_swipc(snx-test-swipc-iso ROOT "${PROJECT_SOURCE_DIR}" FILES tests/swipc/test.swipc tests/swipc/other.swipc)
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
        keyword_c23_argument 4 "constexpr: a C keyword"
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
snx_test_target(snx-test-bsd-service gnu23)
snx_service_swipc(snx-test-bsd-service hle/services/network/bsd.swipc)
target_link_libraries(snx-test-bsd-service PRIVATE scarletnx-ldn)
add_test(NAME bsd_service COMMAND snx-test-bsd-service)
set_tests_properties(bsd_service PROPERTIES TIMEOUT 60)

# the Eden room server the room tests talk to (fake_room.h), with the ENet it is written on: a test that links it
# gets the pinned ENet, never a system one's headers
add_library(snx-test-fake-room STATIC fake_room.c)
snx_test_target(snx-test-fake-room gnu23)
target_link_libraries(snx-test-fake-room PUBLIC scarletnx-net snx_enet)

# sfdnsres and nifm (dns.c, nifm.c) offline, with bsd.c beside them for whether the console is online
add_executable(snx-test-network-service network_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c"
    own_stub.c)
snx_test_target(snx-test-network-service gnu23)
snx_service_swipc(snx-test-network-service hle/services/network/bsd.swipc hle/services/network/dns.swipc
                  hle/services/network/nifm.swipc)
target_link_libraries(snx-test-network-service PRIVATE scarletnx-ldn snx-test-fake-room)
add_test(NAME network_service COMMAND snx-test-network-service)
set_tests_properties(network_service PROPERTIES TIMEOUT 60)

# local wireless over the LAN (common/network/net_ldn.c): the frames, and two consoles on the loopback
add_executable(snx-test-ldn-lan ldn_lan.c)
snx_test_target(snx-test-ldn-lan gnu23)
target_link_libraries(snx-test-ldn-lan PRIVATE scarletnx-ldn)
add_test(NAME ldn_lan COMMAND snx-test-ldn-lan)
set_tests_properties(ldn_lan PROPERTIES TIMEOUT 60)

# an Eden room (common/network/net_room.c) against a room server in the test: the messages, joining, ldn's networks
# and the game's datagram sockets through it
add_executable(snx-test-ldn-room ldn_room.c)
snx_test_target(snx-test-ldn-room gnu23)
target_link_libraries(snx-test-ldn-room PRIVATE scarletnx-ldn snx-test-fake-room)
add_test(NAME ldn_room COMMAND snx-test-ldn-room)
set_tests_properties(ldn_room PROPERTIES TIMEOUT 120)

# ldn:u (ldn.c): two consoles in one process, one creating a network and the other joining, with bsd.c beside for
# the address a game socket binds meanwhile; the session id comes from OpenSSL's generator
if(TARGET OpenSSL::Crypto)
    add_executable(snx-test-ldn-service ldn_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c")
    snx_test_target(snx-test-ldn-service gnu23)
    snx_service_swipc(snx-test-ldn-service hle/services/network/bsd.swipc hle/services/network/ldn.swipc)
    target_link_libraries(snx-test-ldn-service PRIVATE scarletnx-ldn snx-test-fake-room OpenSSL::Crypto)
    add_test(NAME ldn_service COMMAND snx-test-ldn-service)
    set_tests_properties(ldn_service PROPERTIES TIMEOUT 60)
    # both tests take 127.0.0.1 and 127.0.0.2 on the ldn port: never at the same time
    set_tests_properties(ldn_lan ldn_service PROPERTIES RESOURCE_LOCK ldn_port)
endif()

# the Eden room of ldn=room (room.c): room='s address, users/online.ini's password, what stops the game before joining,
# and the stop when the room refuses it
add_executable(snx-test-room-service room_service.c)
snx_test_target(snx-test-room-service gnu23)
target_link_libraries(snx-test-room-service PRIVATE scarletnx-ldn scarletnx-common snx-test-fake-room)
add_test(NAME room_service COMMAND snx-test-room-service)

# the launcher's HTTPS client (common/network/https.c) against HTTP and TLS servers in the test
if(TARGET snx_https)
    add_executable(snx-test-https https.c)
    snx_test_target(snx-test-https gnu23)
    target_link_libraries(snx-test-https PRIVATE snx_https OpenSSL::SSL) # the test's certificates and servers
    if(WIN32)
        target_link_libraries(snx-test-https PRIVATE ws2_32)
    endif()
    add_test(NAME https COMMAND snx-test-https WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(https PROPERTIES TIMEOUT 60)
endif()
# ssl (ssl.c) against a TLS server in the test, on the game's bsd sockets (bsd.c, built beside). OpenSSL comes with
# the NX, 3DS and hactool builds (cmake/dependencies.cmake), not with a GNU ABI-only one
if(TARGET OpenSSL::SSL)
    add_executable(snx-test-ssl-service ssl_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c"
    own_stub.c)
    snx_test_target(snx-test-ssl-service gnu23)
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
    snx_test_target(snx-test-misc-service gnu23)
        snx_service_swipc(snx-test-misc-service hle/services/hid.swipc hle/services/misc.swipc hle/services/acc.swipc)
    target_link_libraries(snx-test-misc-service PRIVATE OpenSSL::Crypto snx_splash scarletnx-common)
    # caps' album pictures are read back with plutovg, which snx_splash holds
    target_include_directories(snx-test-misc-service SYSTEM PRIVATE "${plutovg_SOURCE_DIR}/include")
    target_compile_definitions(snx-test-misc-service PRIVATE PLUTOVG_BUILD_STATIC)
    if(NOT WIN32)
        target_link_libraries(snx-test-misc-service PRIVATE m)
    endif()
    add_test(NAME misc_service COMMAND snx-test-misc-service)

    # the native nn::time (nn/time.c): its handlers on a stand-in CPU, with the guest clock and the lock stubbed
    add_executable(snx-test-nn-time nn_time.c)
    snx_test_target(snx-test-nn-time gnu23)
    snx_service_swipc(snx-test-nn-time)
    snx_hookgen(snx-test-nn-time SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/time.hook" A32_MARKED)
    target_link_libraries(snx-test-nn-time PRIVATE OpenSSL::Crypto)
    add_test(NAME nn_time COMMAND snx-test-nn-time)
    # the native nn::fs (nn/fs.c): a path's mount and limits as the SDK's C library asks them, and a system archive
    # mounted by its data id, with the file server's core stubbed; then the RomFS of another program, from its
    # image, and the stop when the image is not there
    add_executable(snx-test-nn-fs nn_fs.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    snx_test_target(snx-test-nn-fs gnu23)
    snx_service_swipc(snx-test-nn-fs)
    snx_hookgen(snx-test-nn-fs SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/fs.hook" A32_MARKED)
    add_test(NAME nn_fs_handle_flags COMMAND snx-test-nn-fs handle-flags)
    set_tests_properties(nn_fs_handle_flags PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::CloseFile: the game passed the handle 8000000[0-9a-f]+4, whose low 4 bits a native handle has clear")
    add_test(NAME nn_fs COMMAND snx-test-nn-fs)
    add_test(NAME nn_fs_other_program COMMAND snx-test-nn-fs other-program)
    set_tests_properties(nn_fs_other_program PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::QueryMountRomCacheSize: the game asks for the RomFS of program 1 of its application \\(0000000000000001\\), and this is program 0. Put that program's RomFS image in the game's data folder as system/archives/0000000000000001.romfs")
    add_test(NAME nn_fs_commit_null COMMAND snx-test-nn-fs commit-null)
    set_tests_properties(nn_fs_commit_null PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::Commit failed with result 0x2f5e02 \\(error 2002-6063\\)")
    add_test(NAME nn_fs_commit_eleven COMMAND snx-test-nn-fs commit-eleven)
    set_tests_properties(nn_fs_commit_eleven PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::Commit failed with result 0x2f6e02 \\(error 2002-6071\\)")
    add_test(NAME nn_fs_cache_null COMMAND snx-test-nn-fs cache-null)
    set_tests_properties(nn_fs_cache_null PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::EnableIndividualFileDataCache failed with result 0x2f5e02 \\(error 2002-6063\\)")
    add_test(NAME nn_fs_cache_small COMMAND snx-test-nn-fs cache-small)
    set_tests_properties(nn_fs_cache_small PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::EnableIndividualFileDataCache failed with result 0x2f5c02 \\(error 2002-6062\\)")
    add_test(NAME nn_fs_cache_path COMMAND snx-test-nn-fs cache-path)
    set_tests_properties(nn_fs_cache_path PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::DisableIndividualFileDataCache failed with result 0x2ee602 \\(error 2002-6003\\)")
    add_test(NAME nn_fs_restore_null COMMAND snx-test-nn-fs restore-null)
    set_tests_properties(nn_fs_restore_null PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::GetSaveDataRestoreFlag failed with result 0x2f5e02 \\(error 2002-6063\\)")
    add_test(NAME nn_fs_restore_account COMMAND snx-test-nn-fs restore-account)
    set_tests_properties(nn_fs_restore_account PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::GetSaveDataRestoreFlag failed with result 0x2ee002 \\(error 2002-6000\\)")
    add_test(NAME nn_fs_restore_card COMMAND snx-test-nn-fs restore-card)
    set_tests_properties(nn_fs_restore_card PROPERTIES PASS_REGULAR_EXPRESSION
        "fatal: nn::fs::GetSaveDataRestoreFlag failed with result 0x2ee002 \\(error 2002-6000\\)")
    # Unmount with entries open: each one's path logged, then FileNotClosed, or DirectoryNotClosed for folders alone
    add_test(NAME nn_fs_unmount_save COMMAND snx-test-nn-fs unmount-save)
    set_tests_properties(nn_fs_unmount_save PROPERTIES FAIL_REGULAR_EXPRESSION "ng:/" PASS_REGULAR_EXPRESSION
        "nn::fs::Unmount\\(\"save\"\\): the file save:/five is open\nnn::fs::Unmount\\(\"save\"\\): the directory save:/folder/inner is open\nfatal: nn::fs::Unmount\\(\"save\"\\) failed with result 0x326e02 \\(error 2002-6455\\), which the SDK stops the game on \\(open on it, each logged above: files 1, directories 1\\)\n")
    add_test(NAME nn_fs_unmount_rom COMMAND snx-test-nn-fs unmount-rom)
    set_tests_properties(nn_fs_unmount_rom PROPERTIES PASS_REGULAR_EXPRESSION
        "nn::fs::Unmount\\(\"ng\"\\): the file ng:/d/f is open\nnn::fs::Unmount\\(\"ng\"\\): the directory ng:/d is open\nnn::fs::Unmount\\(\"ng\"\\): the directory ng:/ is open\nfatal: nn::fs::Unmount\\(\"ng\"\\) failed with result 0x326e02 \\(error 2002-6455\\), [^\n]*: files 1, directories 2\\)\n")
    add_test(NAME nn_fs_unmount_dir COMMAND snx-test-nn-fs unmount-dir)
    set_tests_properties(nn_fs_unmount_dir PROPERTIES FAIL_REGULAR_EXPRESSION "the file" PASS_REGULAR_EXPRESSION
        "nn::fs::Unmount\\(\"ng\"\\): the directory ng:/d is open\nfatal: nn::fs::Unmount\\(\"ng\"\\) failed with result 0x327002 \\(error 2002-6456\\), [^\n]*: files 0, directories 1\\)\n")
    # the native nn::pctl (nn/pctl.c): the answers of a console with no parental controls set
    add_executable(snx-test-nn-pctl nn_pctl.c)
    snx_test_target(snx-test-nn-pctl gnu23)
    target_include_directories(snx-test-nn-pctl PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_hookgen(snx-test-nn-pctl SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/pctl.hook" A32_MARKED)
    add_test(NAME nn_pctl COMMAND snx-test-nn-pctl)
    # the native nn::nsd (nn/nsd.c): the names of the servers in the retail environment
    add_executable(snx-test-nn-nsd nn_nsd.c)
    snx_test_target(snx-test-nn-nsd gnu23)
    target_include_directories(snx-test-nn-nsd PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_hookgen(snx-test-nn-nsd SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/nsd.hook" A32_MARKED)
    add_test(NAME nn_nsd COMMAND snx-test-nn-nsd)
    # the native nn::ngc (nn/ngc.c): the profanity filter's checks, with no word lists
    add_executable(snx-test-nn-ngc nn_ngc.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    snx_test_target(snx-test-nn-ngc gnu23)
    target_include_directories(snx-test-nn-ngc PRIVATE "${PROJECT_SOURCE_DIR}/src")
    snx_hookgen(snx-test-nn-ngc SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/ngc.hook")
    add_test(NAME nn_ngc COMMAND snx-test-nn-ngc)
    # the native nn::prepo (nn/prepo.c): Save in the two report layouts, and SDK 20's limit
    add_executable(snx-test-nn-prepo nn_prepo.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    snx_test_target(snx-test-nn-prepo gnu23)
    snx_service_swipc(snx-test-nn-prepo)
    snx_hookgen(snx-test-nn-prepo SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/prepo.hook" A32_MARKED)
    add_test(NAME nn_prepo COMMAND snx-test-nn-prepo)
    # the native audio outs (nn/audio.c): what OpenAudioOut writes into the game's AudioOut, the buffers' play and
    # release on the clock, the buffer event, and audio.hook's table
    add_executable(snx-test-nn-audio nn_audio.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    snx_test_target(snx-test-nn-audio gnu23)
    snx_service_swipc(snx-test-nn-audio)
    snx_hookgen(snx-test-nn-audio SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/audio.hook" A32_MARKED)
    add_test(NAME nn_audio COMMAND snx-test-nn-audio)
    # the movie library's native JPEG decoder and encoder (nn/tvmr_jpeg.c) over FFmpeg: pictures through both, their
    # surfaces' layouts, the statuses of what NVJPG refuses, and tvmr_jpeg.hook's table by movie version
    add_executable(snx-test-nn-tvmr-jpeg nn_tvmr_jpeg.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    snx_test_target(snx-test-nn-tvmr-jpeg gnu23)
    snx_service_swipc(snx-test-nn-tvmr-jpeg)
    snx_hookgen(snx-test-nn-tvmr-jpeg SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/tvmr_jpeg.hook" A32_MARKED)
    target_link_libraries(snx-test-nn-tvmr-jpeg PRIVATE FFmpeg::avcodec FFmpeg::avutil)
    add_test(NAME nn_tvmr_jpeg COMMAND snx-test-nn-tvmr-jpeg)
    # the native audio renderer (nn/audio_renderer.c): the parameters, the handle, RequestUpdateAudioRenderer over a
    # stand-in SDK's managers, and audio_renderer.hook's table by SDK version
    add_executable(snx-test-nn-audio-renderer nn_audio_renderer.c "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    snx_test_target(snx-test-nn-audio-renderer gnu23)
    snx_service_swipc(snx-test-nn-audio-renderer)
    snx_hookgen(snx-test-nn-audio-renderer SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/audio_renderer.hook" A32_MARKED)
    add_test(NAME nn_audio_renderer COMMAND snx-test-nn-audio-renderer)
    # the input streams of the Ounce's SDK (nn/audio_stream.c) over a synthetic microphone, never a real one, and
    # the noise reduction (runtime/platform/audio_denoise.c): RNNoise built in, and the system's libspeexdsp when it is
    # there
    add_executable(snx-test-audio-mic audio_mic.c)
    snx_test_target(snx-test-audio-mic gnu23)
    snx_service_swipc(snx-test-audio-mic)
    snx_hookgen(snx-test-audio-mic SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/hooks.hook")
    target_link_libraries(snx-test-audio-mic PRIVATE scarletnx-common snx_rnnoise)
    if(NOT WIN32)
        target_link_libraries(snx-test-audio-mic PRIVATE m)
    endif()
    add_test(NAME audio_mic COMMAND snx-test-audio-mic)
endif()

# Inline software keyboard protocol and applet completion events.
add_executable(snx-test-am-service am_service.c am_service_stubs.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ctrl_request.c")
snx_test_target(snx-test-am-service gnu23)
snx_service_swipc(snx-test-am-service hle/services/am.swipc hle/services/vi.swipc) # vi: grc's video proxy
target_link_libraries(snx-test-am-service PRIVATE snx_test_common) # snx_nacp_aoc_base
add_test(NAME am_service COMMAND snx-test-am-service)

# gsv.c's display session: what a game is told of the display's HDR (GetHdrStatus, GetHdrPaperWhite, the HDR status
# event), of its largest picture (GetBufferMaxResolution) and of its refresh rates (the ones it takes, the one it runs
# at, a game turning 120 Hz on and off, the refresh rate event), with the renderer and the window's display stood in for
add_executable(snx-test-gsv-hdr gsv_hdr.c)
snx_test_target(snx-test-gsv-hdr gnu23)
snx_service_swipc(snx-test-gsv-hdr hle/services/native_video.swipc hle/services/gsv.swipc hle/services/nvdrv.swipc)
add_test(NAME gsv_hdr COMMAND snx-test-gsv-hdr)

# the save state loader's staging of the HLE's copy (runtime/state.c, included for its statics): what a state file says
# of a service object's size and of an am storage's bytes is checked. The rest of the loader is not run, and what it
# calls of the runtime is left unresolved, which the linkers of Linux and FreeBSD take an option for. The linker
# leaves a relocation of no type in the PLT's table for each: the loader skips those where it binds a program whole at
# its start (-z now), and glibc's lazy binding on AArch64 stops on them ("unexpected PLT reloc type 0x00")
if(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD")
    add_executable(snx-test-state-stage state_stage.c)
    snx_test_target(snx-test-state-stage gnu23)
    target_link_options(snx-test-state-stage PRIVATE LINKER:--unresolved-symbols=ignore-all LINKER:-z,now)
    add_test(NAME state_stage COMMAND snx-test-state-stage)
endif()

# am.c with a control.nacp: the desired language against its supported languages, its version and save sizes
add_executable(snx-test-am-nacp am_nacp.c am_service_stubs.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ctrl_request.c")
snx_test_target(snx-test-am-nacp gnu23)
snx_service_swipc(snx-test-am-nacp hle/services/am.swipc hle/services/vi.swipc)
target_link_libraries(snx-test-am-nacp PRIVATE snx_test_common) # snx_nacp_aoc_base
add_test(NAME am_nacp COMMAND snx-test-am-nacp)

# acc.c and users.c: the console's users from users/users.ini, as acc lists them, every file that lists none refused,
# and the picture of a user with none of its own, Wimsy as a 256x256 JPEG (the splash's drawing), decoded by plutovg;
# acc:su's registration makes a user's uid of OpenSSL's random bytes
if(TARGET OpenSSL::Crypto AND NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-acc-service acc_service.c)
    snx_test_target(snx-test-acc-service gnu23)
    snx_service_swipc(snx-test-acc-service hle/services/acc.swipc)
    target_link_libraries(snx-test-acc-service PRIVATE snx_splash scarletnx-common OpenSSL::Crypto)
    target_include_directories(snx-test-acc-service SYSTEM PRIVATE "${plutovg_SOURCE_DIR}/include")
    target_compile_definitions(snx-test-acc-service PRIVATE PLUTOVG_BUILD_STATIC)
    set_target_properties(snx-test-acc-service PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME acc_service COMMAND snx-test-acc-service)
endif()

# set and set:sys: the firmware version's layout and the language codes (set.c)
add_executable(snx-test-set-service set_service.c)
snx_test_target(snx-test-set-service gnu23)
snx_service_swipc(snx-test-set-service hle/services/set.swipc)
add_test(NAME set_service COMMAND snx-test-set-service)

# sm: the service names it accepts, and an application's RegisterService and UnregisterService (sm.c)
add_executable(snx-test-sm-service sm_service.c)
snx_test_target(snx-test-sm-service gnu23)
snx_service_swipc(snx-test-sm-service hle/services/sm.swipc)
add_test(NAME sm_service COMMAND snx-test-sm-service)

# vi's binder: the native window and the buffer queue's parcels (vi.c), with the GPU thread and the renderer stubbed
add_executable(snx-test-vi-service vi_service.c)
snx_test_target(snx-test-vi-service gnu23)
snx_service_swipc(snx-test-vi-service hle/services/vi.swipc)
add_test(NAME vi_service COMMAND snx-test-vi-service)

# the guest clock and vi's presenting side (clock.c, present.c): the system tick, fps_limit's pacing, CPU-read buffers
add_executable(snx-test-runtime-present runtime_present.c)
snx_test_target(snx-test-runtime-present gnu23)
add_test(NAME runtime_present COMMAND snx-test-runtime-present)

# the shared presentation effects (common/gpu/vk/present_fx.c) on a real device: FSR 1, FXAA, SMAA, SSAA
if(TARGET Vulkan::Headers AND TARGET snx_vk_common)
    add_executable(snx-test-present-fx present_fx.c support/fxaa_ref.c "${volk_SOURCE_DIR}/volk.c")
    snx_test_sanitize(snx-test-present-fx)
    target_include_directories(snx-test-present-fx PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests"
                                                           "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-present-fx PRIVATE snx_vk_common Vulkan::Headers ${CMAKE_DL_LIBS} m)
    add_test(NAME present_fx COMMAND snx-test-present-fx)
    set_tests_properties(present_fx PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # the same passes under the Khronos layer: the SSAA copy once updated a set it had bound, which only the layer sees
    add_test(NAME present_fx_validation COMMAND snx-test-present-fx --validation)
    set_tests_properties(present_fx_validation PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()

# nvdrv's IPC side (nvdrv.c), with the GPU and host1x stand-ins in the test
add_executable(snx-test-nvdrv-service nvdrv_service.c)
snx_test_target(snx-test-nvdrv-service gnu23)
snx_service_swipc(snx-test-nvdrv-service hle/services/nvdrv.swipc)
add_test(NAME nvdrv_service COMMAND snx-test-nvdrv-service)

# ipc.c's parsing and replies (hle/kernel/ipc.c, included by the test) with raw HIPC messages laid out as libnx
# writes them, a test service, and the kernel's handles and objects stubbed
add_executable(snx-test-ipc-message ipc_message.c)
snx_test_target(snx-test-ipc-message gnu23)
add_test(NAME ipc_message COMMAND snx-test-ipc-message)

# the crash report's lines about a thread (hle/kernel/debug.c, included by the test): the last SVCs with the thread
# of each, and its last call when it went to a hooked memcpy, memmove or memset, with guest memory, the hooks' sites
# and the log stubbed
add_executable(snx-test-kernel-dump kernel_dump.c)
snx_test_target(snx-test-kernel-dump gnu23)
add_test(NAME kernel_dump COMMAND snx-test-kernel-dump)

if(SNX_BUILD_NX)
add_test(NAME llvm_dependency COMMAND "${CMAKE_COMMAND}"
    "-DSNX_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DSNX_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/llvm-dependency"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/llvm_dependency.cmake")

include("${PROJECT_SOURCE_DIR}/cmake/gui_themes.cmake")
if(TARGET SDL3::SDL3) # F1's and the Shortcuts pages name the runtime's keys as SDL names them
    add_executable(snx-test-overlay overlay.c tracy_stubs.c
        "${PROJECT_SOURCE_DIR}/src/common/utilities/overlay.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/overlay_host.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ctrl_applet.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ctrl_request.c"
        "${PROJECT_SOURCE_DIR}/src/common/utilities/fatal_screen.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/prepare_screen.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/wait_screen.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/hud.c"
        "${PROJECT_SOURCE_DIR}/src/common/utilities/hotkeys.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/key_text.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
    if(SNX_TRACY)
        # the keys page has the capture's row in a build with the client, which the test expects of either
        target_compile_definitions(snx-test-overlay PRIVATE SNX_TRACY=1
            $<TARGET_PROPERTY:Tracy::TracyClient,INTERFACE_COMPILE_DEFINITIONS>)
        # the platform files this test compiles now include tracy.h, which needs the client's headers. The client
        # library is linked once, by snx_test_target, from the SNX_TRACY definition above
        target_include_directories(snx-test-overlay SYSTEM PRIVATE
            $<TARGET_PROPERTY:Tracy::TracyClient,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    snx_test_target(snx-test-overlay gnu23)
    target_link_libraries(snx-test-overlay PRIVATE snx_vpad scarletnx-common snx_splash m SDL3::SDL3 snx_notice)
    target_include_directories(snx-test-overlay PRIVATE "${PROJECT_BINARY_DIR}/generated")
    add_test(NAME overlay COMMAND snx-test-overlay)
    # the controller applet's dialog: its request, cards and gestures, and its pictures drawn on the CPU
    add_executable(snx-test-ctrl-applet ctrl_applet.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ctrl_applet.c"
        "${PROJECT_SOURCE_DIR}/src/common/utilities/fatal_screen.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ctrl_scan.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ctrl_request.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/ui_ring.c" support/ui_raster.c support/ui_shape_ref.c)
    snx_test_target(snx-test-ctrl-applet gnu23)
    # the scan's fake alone (ble_scan.h's snx_ble_fake_open): the test never reaches the system's Bluetooth
    target_link_libraries(snx-test-ctrl-applet PRIVATE scarletnx-common snx_splash snx_ble m SDL3::SDL3)
    target_include_directories(snx-test-ctrl-applet PRIVATE "${PROJECT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ctrl_applet COMMAND snx-test-ctrl-applet)
endif()

# the screens the player waits on (runtime/platform/wait_screen.c): the game closing's from half a second into its
# save, a close that stops it and the log's line with the MiB not saved, its row's MiB and seconds; a save state's box
# from 0.3 s into its load
add_executable(snx-test-wait-screen wait_screen.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/wait_screen.c")
snx_test_target(snx-test-wait-screen gnu23)
add_test(NAME wait_screen COMMAND snx-test-wait-screen)

add_executable(snx-test-vpad vpad.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-vpad gnu23)
target_link_libraries(snx-test-vpad PRIVATE snx_vpad scarletnx-common snx_splash m)
target_include_directories(snx-test-vpad PRIVATE "${PROJECT_BINARY_DIR}/generated") # ui.c's theme
add_test(NAME vpad COMMAND snx-test-vpad)
# every row of the Switch's settings with its line for each console, and the drivers' reading by those lines
# (common/utilities/console_settings.c)
add_executable(snx-test-console-settings console_settings.c)
snx_test_target(snx-test-console-settings c23)
target_link_libraries(snx-test-console-settings PRIVATE snx_console_settings scarletnx-common)
add_test(NAME console_settings COMMAND snx-test-console-settings)
# a console's own on-screen controls and its game's settings of them (common/utilities/vpad.c)
add_executable(snx-test-vpad-console vpad_console.c)
snx_test_target(snx-test-vpad-console c23)
target_link_libraries(snx-test-vpad-console PRIVATE snx_vpad snx_vpad_consoles scarletnx-common)
add_test(NAME vpad_console COMMAND snx-test-vpad-console)
set(vpad_home "${CMAKE_CURRENT_BINARY_DIR}/vpad-console-home") # a home of its own: the config folder each system keeps
set_tests_properties(vpad_console PROPERTIES ENVIRONMENT
    "SNX_TEST_HOME=${vpad_home};HOME=${vpad_home};XDG_DATA_HOME=${vpad_home}/data;XDG_CONFIG_HOME=${vpad_home}/config;APPDATA=${vpad_home}")
# the layer's quads as rectangles, for the windows with no pipeline of their own (common/utilities/ui_rects.c): each pixel
# the one ui.frag writes
add_executable(snx-test-ui-rects ui_rects.c support/ui_shape_ref.c)
snx_test_target(snx-test-ui-rects c23)
target_link_libraries(snx-test-ui-rects PRIVATE snx_vpad scarletnx-common m)
target_include_directories(snx-test-ui-rects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME ui_rects COMMAND snx-test-ui-rects)
# the kinds of error, their lines and actions, the record's file and the run's "error: " lines
# (common/utilities/error_kind.c)
add_executable(snx-test-error-kind error_kind.c)
snx_test_target(snx-test-error-kind c23)
target_link_libraries(snx-test-error-kind PRIVATE snx_error scarletnx-common)
add_test(NAME error_kind COMMAND snx-test-error-kind "${CMAKE_CURRENT_BINARY_DIR}")
# the error's panel: its layout from a GBA's window to a phone's, its colours, its keys and presses, Details
# (common/utilities/error_panel.c)
if(TARGET snx_error_panel)
    add_executable(snx-test-error-panel error_panel.c)
    snx_test_target(snx-test-error-panel c23)
    target_link_libraries(snx-test-error-panel PRIVATE snx_error_panel snx_vpad scarletnx-common m)
    target_include_directories(snx-test-error-panel PRIVATE "${PROJECT_BINARY_DIR}/generated") # the YoRHa colours
    add_test(NAME error_panel COMMAND snx-test-error-panel)
endif()
# the panel in an SDL window, its keys and presses, and its own process (common/utilities/error_sdl.c, error_spawn.c), on
# SDL's dummy video: this test starts itself as the panel's process
if(TARGET snx_error_sdl AND NOT WIN32)
    add_executable(snx-test-error-sdl error_sdl.c)
    snx_test_target(snx-test-error-sdl c23)
    target_link_libraries(snx-test-error-sdl PRIVATE snx_error_sdl scarletnx-common)
    add_test(NAME error_sdl COMMAND snx-test-error-sdl)
    set_tests_properties(error_sdl PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 60)
endif()

add_executable(snx-test-hud hud.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/hud.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/ui_ring.c")
snx_test_target(snx-test-hud gnu23)
target_link_libraries(snx-test-hud PRIVATE scarletnx-common snx_splash m)
target_include_directories(snx-test-hud PRIVATE "${PROJECT_BINARY_DIR}/generated") # the theme's colours
add_test(NAME hud COMMAND snx-test-hud)

if(TARGET SDL3::SDL3) # the keys' names are SDL's
    # the runtime's keys (common/utilities/hotkeys.c): settings.txt's Shortcuts read as SDL names keys, a key pressed
    # found by them, a value that isn't a key refused with the one in effect
    add_executable(snx-test-hotkeys hotkeys.c "${PROJECT_SOURCE_DIR}/src/common/utilities/hotkeys.c"
                   "${PROJECT_SOURCE_DIR}/src/common/utilities/key_text.c")
    snx_test_target(snx-test-hotkeys gnu23)
    target_link_libraries(snx-test-hotkeys PRIVATE SDL3::SDL3)
    add_test(NAME hotkeys COMMAND snx-test-hotkeys)
    # the notice of the windows with no menu of their own (common/utilities/notice.c): the Switch's notice's letters,
    # place and colours
    add_executable(snx-test-notice notice.c)
    snx_test_target(snx-test-notice c23)
    target_link_libraries(snx-test-notice PRIVATE snx_notice)
    target_include_directories(snx-test-notice PRIVATE "${PROJECT_BINARY_DIR}/generated") # the theme's colours
    add_test(NAME notice COMMAND snx-test-notice)
    # the hints every program sets before its SDL video starts (common/utilities/sdl_desktop.h)
    add_executable(snx-test-sdl-desktop-hints sdl_desktop_hints.c)
    snx_test_target(snx-test-sdl-desktop-hints gnu23)
    target_link_libraries(snx-test-sdl-desktop-hints PRIVATE SDL3::SDL3)
    add_test(NAME sdl_desktop_hints COMMAND snx-test-sdl-desktop-hints)
endif()
# F1's text (common/utilities/help.c): Ctrl+Shift+F12 while RenderDoc can capture, the debugger while one traces the
# game
add_executable(snx-test-help help.c "${PROJECT_SOURCE_DIR}/src/common/utilities/help.c")
snx_test_target(snx-test-help c23)
target_include_directories(snx-test-help PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME help COMMAND snx-test-help)

add_executable(snx-test-stats stats.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-stats gnu23)
target_link_libraries(snx-test-stats PRIVATE snx_vpad scarletnx-common snx_splash m)
target_include_directories(snx-test-stats PRIVATE "${PROJECT_BINARY_DIR}/generated") # the theme's colours
add_test(NAME stats COMMAND snx-test-stats)

# the game threads' costs in the log (runtime/guest_stats.c): the SVCs and misses of a window, a thread's time on the
# processors read from another thread, and the lines that say them
add_executable(snx-test-guest-stats guest_stats.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/guest_stats.c")
snx_test_target(snx-test-guest-stats gnu23)
target_include_directories(snx-test-guest-stats PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-guest-stats PRIVATE scarletnx-common scarletnx-filesystem)
add_test(NAME guest_stats COMMAND snx-test-guest-stats)
# nce_module (runtime/nce/nce.c): the modules of a layout that NCE moved up (Android), and the main thread's entry in
# them, the moved one
add_executable(snx-test-nce-module nce_module.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce.c")
snx_test_target(snx-test-nce-module gnu23)
target_link_libraries(snx-test-nce-module PRIVATE snx_nx_flags snx_settings_runtime)
# nce_module is the same on every host; on an AArch64 Linux one nce.c would also build NCE itself, which needs the rest
# of the runtime
target_compile_definitions(snx-test-nce-module PRIVATE SNX_NCE_HOST=0)
add_test(NAME nce_module COMMAND snx-test-nce-module)
# rt_miss (runtime/rtapi.c): a pc with no code runs the game's native code under NCE (an NVN memory callback), and the
# JIT's without it
add_executable(snx-test-rtapi-miss rtapi_miss.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/rtapi.c")
snx_test_target(snx-test-rtapi-miss gnu23)
target_link_libraries(snx-test-rtapi-miss PRIVATE snx_nx_flags snx_settings_runtime)
add_test(NAME rtapi_miss COMMAND snx-test-rtapi-miss)
# the crash report of a runtime thread (runtime/crash.c): its name as the system has it and what it noted it was doing,
# a pipeline the driver was making (common/platform/thread.c's plat_thread_note), or that it noted nothing
add_executable(snx-test-crash-runtime-thread crash_runtime_thread.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/crash.c")
snx_test_target(snx-test-crash-runtime-thread gnu23)
target_link_libraries(snx-test-crash-runtime-thread PRIVATE snx_nx_flags Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_link_libraries(snx-test-crash-runtime-thread PRIVATE execinfo) # crash.c's host frames
endif()
add_test(NAME crash_runtime_thread COMMAND snx-test-crash-runtime-thread)

add_executable(snx-test-ini-deprecated ini_deprecated.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-ini-deprecated gnu23)
add_test(NAME ini_deprecated COMMAND snx-test-ini-deprecated)

# options.c's TV mode Auto over a faked host power source, a key its .ini gives twice, controls_skin's default as a .ini
# opens, controls_opacity read as controls_alpha and vk_descriptors as vk_descriptors_mode, and the FPS limit's,
# showcase's and stats's keys; the rest of the runtime stubbed
add_executable(snx-test-options-tv-mode options_tv_mode.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/options.c")
snx_test_target(snx-test-options-tv-mode gnu23)
target_link_libraries(snx-test-options-tv-mode PRIVATE scarletnx-common snx_error) # --from-launcher's note
add_test(NAME options_tv_mode COMMAND snx-test-options-tv-mode)
# the game's .ini and picks through tests/config_files.h's situations (config_files_options.c), a process each
add_test(NAME config_files_options COMMAND snx-test-options-tv-mode --config-files game.ini)
add_test(NAME config_files_picks COMMAND snx-test-options-tv-mode --config-files game.picks)
# the player's pause (runtime/pause.c), hotkey_pause's: each press's line, a pause that didn't happen, and another pause
# in force; the guest threads, the GPU, host1x and the clocks stubbed
add_executable(snx-test-pause-toggle pause_toggle.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/pause.c")
snx_test_target(snx-test-pause-toggle gnu23)
add_test(NAME pause_toggle COMMAND snx-test-pause-toggle)
# vk_descriptors read as vk_descriptors_mode, a startup setting, which only a process's first .ini sets: a process each
foreach(case old pick cli)
    add_test(NAME options_vk_descriptors_${case} COMMAND snx-test-options-tv-mode --vk-descriptors-${case})
endforeach()
# a game's own picks (<name>.picks) over the launcher's and its .ini, under this run's command line: a process each
foreach(case order menu hand reload clean next perf perf-run kind kind-pick kind-auto kind-cli kind-old0 kind-launcher
        kind-oldpick)
    add_test(NAME options_picks_${case} COMMAND snx-test-options-tv-mode --picks-${case})
endforeach()

add_executable(snx-test-pad-map pad_map.c)
snx_test_target(snx-test-pad-map gnu23)
target_link_libraries(snx-test-pad-map PRIVATE scarletnx-common)
add_test(NAME pad_map COMMAND snx-test-pad-map)
add_executable(snx-test-pad-screen pad_screen.c)
snx_test_target(snx-test-pad-screen gnu23)
target_link_libraries(snx-test-pad-screen PRIVATE scarletnx-common)
add_test(NAME pad_screen COMMAND snx-test-pad-screen)

include("${PROJECT_SOURCE_DIR}/cmake/pad_sdl.cmake")
# controllers taking players as a console's do (gamepad_assign=1)
add_executable(snx-test-connect connect.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/connect.c")
snx_test_target(snx-test-connect c23)
add_test(NAME connect COMMAND snx-test-connect)
# the Joy-Con 2's protocol and one controller's setup on made-up bytes, no Bluetooth (runtime/platform/joycon2_*.c)
add_executable(snx-test-joycon2 joycon2.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/joycon2_proto.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/joycon2_session.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/joycon2_att.c"
    "${PROJECT_SOURCE_DIR}/src/common/platform/ble_scan.c") # the launcher's advert check, compared
snx_test_target(snx-test-joycon2 c23)
target_link_libraries(snx-test-joycon2 PRIVATE m)
add_test(NAME joycon2 COMMAND snx-test-joycon2)

add_executable(snx-test-gamepad gamepad.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/backend/gamepad.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/connect.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/ui_ring.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/joycon2_sdl.c")
snx_test_target(snx-test-gamepad gnu23)
target_link_libraries(snx-test-gamepad PRIVATE scarletnx-common snx_pad_sdl snx_splash m)
target_include_directories(snx-test-gamepad PRIVATE "${PROJECT_BINARY_DIR}/generated") # ui.c's theme
add_test(NAME gamepad COMMAND snx-test-gamepad)
# a Joy-Con 2 as an SDL virtual gamepad, read back through the gamepads' code (runtime/platform/joycon2_sdl.c)
add_executable(snx-test-joycon2-sdl joycon2_sdl.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/joycon2_sdl.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/backend/gamepad.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/connect.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/ui_ring.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-joycon2-sdl gnu23)
target_link_libraries(snx-test-joycon2-sdl PRIVATE scarletnx-common snx_pad_sdl snx_splash m)
target_include_directories(snx-test-joycon2-sdl PRIVATE "${PROJECT_BINARY_DIR}/generated")
add_test(NAME joycon2_sdl COMMAND snx-test-joycon2-sdl)

add_executable(snx-test-fatal-screen fatal_screen.c "${PROJECT_SOURCE_DIR}/src/common/utilities/fatal_screen.c")
snx_test_target(snx-test-fatal-screen gnu23)
target_link_libraries(snx-test-fatal-screen PRIVATE scarletnx-common)
add_test(NAME fatal_screen COMMAND snx-test-fatal-screen)

add_executable(snx-test-ini ini.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-ini gnu23)
add_test(NAME ini COMMAND snx-test-ini)
# the settings' names on a command line, each key's with a - for each _ (tools/settingsgen's settings_args.c): sorted,
# each read back as its key, none a common option's
add_executable(snx-test-settings-args settings_args.c)
snx_test_target(snx-test-settings-args gnu23)
target_link_libraries(snx-test-settings-args PRIVATE scarletnx-common)
add_test(NAME settings_args COMMAND snx-test-settings-args)
# the Linux AArch64 signal frame the GNU ABI runtime and the Android bridge share
add_executable(snx-test-linux-signal linux_signal.c "${PROJECT_SOURCE_DIR}/src/common/runtime/linux_signal.c")
snx_test_target(snx-test-linux-signal gnu23)
add_test(NAME linux_signal COMMAND snx-test-linux-signal)
add_executable(snx-test-ini-lines ini_lines.c "${PROJECT_SOURCE_DIR}/src/common/utilities/ini_lines.c")
snx_test_target(snx-test-ini-lines c23)
add_test(NAME ini_lines COMMAND snx-test-ini-lines)
# the rule of every settings file the project writes and reads (common/utilities/config_file.c)
add_executable(snx-test-config-file config_file.c)
snx_test_target(snx-test-config-file c23)
target_link_libraries(snx-test-config-file PRIVATE snx_test_common)
add_test(NAME config_file COMMAND snx-test-config-file)
# tests/config_files.h's situations: common/utilities' settings files (config_files_common.c), setup.ini where the
# setup is built
add_executable(snx-test-config-files-common config_files_common.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/joycon2_known.c" # the runtime's joycon2.txt
    "${PROJECT_SOURCE_DIR}/src/3ds/runtime/game_record.c") # a 3DS game's record and picks
snx_test_target(snx-test-config-files-common c23)
target_link_libraries(snx-test-config-files-common PRIVATE scarletnx-common snx_vpad snx_vpad_consoles)
if(TARGET snx_setup)
    target_link_libraries(snx-test-config-files-common PRIVATE snx_setup)
    target_compile_definitions(snx-test-config-files-common PRIVATE SNX_CONFIG_SETUP)
endif()
add_test(NAME config_files_common COMMAND snx-test-config-files-common)
# the runner of those situations: a reader that follows the rule passes, and each way of breaking it fails
add_executable(snx-test-config-files-checker config_files_checker.c)
snx_test_target(snx-test-config-files-checker c23)
target_link_libraries(snx-test-config-files-checker PRIVATE scarletnx-common)
add_test(NAME config_files_checker COMMAND snx-test-config-files-checker)
# the R_ result helpers (src/nx/hle/result.h), strict ISO C23 as the code that may use them
add_executable(snx-test-result-macros result_macros.c)
snx_test_target(snx-test-result-macros c23)
add_test(NAME result_macros COMMAND snx-test-result-macros)
# the results the runtime answers (src/nx/hle/result_codes.inc) and their lookup (result.c), strict ISO C23 as result.h
add_executable(snx-test-result-codes result_codes.c)
snx_test_target(snx-test-result-codes c23)
add_test(NAME result_codes COMMAND snx-test-result-codes)
# and none made outside it: no RC() but result.h's, no result written as a number in the HLE
if(Python3_Interpreter_FOUND)
    add_test(NAME result_codes_sources COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/result_codes.py"
                                               --repo "${PROJECT_SOURCE_DIR}")
endif()

# generated and checked in (the controls' atlas, their shaders): their sources must still make them
if(Python3_Interpreter_FOUND)
    add_test(NAME cpu_sample_recovery COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cpu_sample.py")
    add_test(NAME nvn_coverage COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/nvn_coverage.py")
    add_test(NAME nvn_rendering_harness COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_nvn_rendering_test.py")
    add_test(NAME nvn_contracts COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/nvn_contracts_test.py")
    add_test(NAME analyze_gpu_budget COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/analyze_gpu_budget_test.py")
    add_test(NAME analyze_descriptor_cache COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/analyze_descriptor_cache_test.py")
    add_test(NAME gpu_hotpath_benchmark COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bench_gpu_hotpaths_test.py")
    add_test(NAME gpu_tuner COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tune_gpu_test.py")
    add_test(NAME nvn_draw_benchmark COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bench_nvn_draws_test.py")
    add_test(NAME nvn_game_benchmark COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bench_nvn_game_test.py")
    add_test(NAME pipeline_compile_probe_inputs COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_pipeline_probe_test.py")
    add_test(NAME gpu_descriptor_benchmark COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bench_gpu_descriptors_test.py")
    # every 3D method that acts is marked in the fast path's table (engines.c g_m3d_group)
    add_test(NAME m3d_acts COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/m3d_acts_test.py")
    # no keys, dumps, stray binaries, piracy sites or the console maker's name in prose among the tracked files: the
    # checks on planted ones (self test), then on the checkout. Skipped (77) without git or outside a checkout
    add_test(NAME tracked_material_self_test
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tracked_material.py" --self-test)
    add_test(NAME tracked_material
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tracked_material.py" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(tracked_material_self_test tracked_material PROPERTIES SKIP_RETURN_CODE 77)
    # the docs held to the checkout (scripts/check-docs.py): their links and anchors, the ## Index of each long one,
    # docs/README.md linking every doc, docs/user/settings.md's keys and defaults against src/nx/runtime/settings.txt,
    # and docs/layers/gpu/driver-quirks.md's ban table against src/common/gpu/vk/extension_bans.txt; then the checks on
    # checkouts made to fail them
    add_test(NAME docs_check COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check-docs.py")
    add_test(NAME docs_check_script COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/docs_test.py")
    # every key settings.txt dropped in main's history named by INI_DEPRECATED and docs/user/deprecations.md, the two
    # alike (scripts/check-deprecations.py); then the checks on histories made to fail them. Skipped (77) without git
    # or a checkout; a shallow clone's states are the ones it has
    add_test(NAME deprecations_check COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/check-deprecations.py")
    add_test(NAME deprecations_check_script COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/deprecations_test.py")
    set_tests_properties(deprecations_check PROPERTIES SKIP_RETURN_CODE 77)
    # which GitHub Actions caches the cleanup workflow deletes (scripts/trim-caches.py)
    add_test(NAME trim_caches COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/trim_caches_test.py")
    # build.yml's LLVM builds, each saved by the step right after it: a later failure or a cancelled run keeps it;
    # and its compiler caches, which a pull request's run does not save
    add_test(NAME build_caches COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_caches_test.py"
                                       "${PROJECT_SOURCE_DIR}")
    # which of the recompiler's long tests build.yml runs for a pull request (scripts/recompiler-changes.py), and what
    # each machine's tests under Unicorn did, in ctest's JUnit file (scripts/unicorn-tests.py)
    add_test(NAME recompiler_changes COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/recompiler_changes_test.py")
    add_test(NAME unicorn_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/unicorn_tests_test.py")
    # what it shows of the native back end's counts of whole programs (recompile_census), scripts/recompile-census.py
    add_test(NAME recompile_census_script COMMAND "${Python3_EXECUTABLE}"
             "${CMAKE_CURRENT_SOURCE_DIR}/recompile_census_test.py")
    # the runs a closed pull request left going, which pr-closed.yml cancels (scripts/cancel-pr-runs.py)
    add_test(NAME cancel_pr_runs COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cancel_pr_runs_test.py")
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
    # game-reports.yml's own build of snx-logscan, run on the files its sparse checkout lists alone: a file the log
    # summary comes to include that the list lacks fails here, not on the next issue. Linux, where the workflow runs,
    # with the cc and the C++ runtime its step links
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME game_reports_workflow COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/game_reports_workflow_test.py" "${PROJECT_SOURCE_DIR}")
    endif()
    # the game tools/run_game.py finds by its name (recompiled/, else an older version's games/)
    add_test(NAME run_game COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_game_test.py")
    # the tag a game goes by in place of its title and title ID (tools/game_tag.py): the HMAC, the key, which and scan
    add_test(NAME game_tag COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/game_tag_test.py")
    # the nightly release's title and notes (scripts/nightly-notes.py)
    add_test(NAME nightly_notes COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/nightly_notes_test.py")
    # the builds published as releases and on Discord (scripts/publish-builds.py), against fakes. Where publish.yml
    # runs it, Linux: the fake gh is a script Windows does not run by its name
    if(NOT WIN32)
        add_test(NAME publish_builds
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/publish_builds_test.py")
        # edenprobe's runs held against a baseline (scripts/edenprobe.py), with a fake runtime and game
        add_test(NAME edenprobe_script COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/edenprobe_test.py")
        # a Build run cancelled once a job of it failed, by the jobs still running (scripts/fail-fast.py), against a
        # fake GitHub, and build.yml's jobs held to it
        add_test(NAME fail_fast COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fail_fast_test.py")
        # the pull request's comment linking its builds (scripts/pr-builds.sh), against a fake gh; 77 without jq
        add_test(NAME pr_builds COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/pr_builds_test.py")
        set_tests_properties(pr_builds PROPERTIES SKIP_RETURN_CODE 77)
        # LLVM's release source by its pinned SHA-256 (scripts/llvm-source.sh, which the LLVM scripts download
        # through), against a fake curl
        add_test(NAME llvm_source COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/llvm_source_test.py")
    endif()
endif()

# what CPMUtil and snx_package_file keep in the package cache: a failed download said, a cut package fetched again,
# one fetch of a package at a time, the patches applied without fuzz (cmake/packages.cmake)
add_test(NAME cpmutil_cache COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cpmutil-cache-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/cpmutil_cache_test.cmake")
# the dependency cache's keys and how a build is published to it (cmake/deps_cache.cmake)
add_test(NAME deps_cache COMMAND "${CMAKE_COMMAND}" "-DCC=${CMAKE_C_COMPILER}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/deps-cache-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/deps_cache_test.cmake")
# where a package comes from by CPMUtil's options and the CPM.cmake ones they replace (cmake/packages.cmake)
# the translator's compile flags in the shader files' key (cmake/shader_flags.cmake): none for a normal build's, a
# stamp for each other set
add_test(NAME shader_flags COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         -P "${CMAKE_CURRENT_SOURCE_DIR}/shader_flags_test.cmake")
add_test(NAME cpmutil_options COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cpmutil-options-test" -P
         "${CMAKE_CURRENT_SOURCE_DIR}/cpmutil_options_test.cmake")
# a test option without the tests stops the configure, and the presets for this machine leave the tests out
add_test(NAME test_options COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/test-options-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/test_options_test.cmake")
# the x86-64 back end needs neither the Intel SDM nor pdftotext, and SNX_TEST_SDM says what it lacks (cmake/sdm.cmake)
add_test(NAME sdm_configure COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/sdm-configure-test" -P
         "${CMAKE_CURRENT_SOURCE_DIR}/sdm_configure_test.cmake")
# where spirv-opt, spirv-val and spirv-dis come from (cmake/spirv_tools.cmake): the fake tools are shell scripts
if(NOT CMAKE_HOST_WIN32)
    add_test(NAME spirv_tools COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" "-DCC=${CMAKE_C_COMPILER}"
             "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/spirv-tools-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/spirv_tools_test.cmake")
endif()
# SNX_SYSTEM_SDL3 takes a system SDL3 from 3.4.12, and says what it found where it takes none (cmake/system_sdl3.cmake)
add_test(NAME system_sdl3 COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/system-sdl3-test" "-DGENERATOR=${CMAKE_GENERATOR}"
         "-DMAKE=${CMAKE_MAKE_PROGRAM}" -P "${CMAKE_CURRENT_SOURCE_DIR}/system_sdl3_test.cmake")
# shaderperf's reflection of a module whose spec constant's id is past the id bound (tools/shaderperf/reflect.c)
add_executable(snx-test-shaderperf-reflect shaderperf_reflect.c "${PROJECT_SOURCE_DIR}/tools/shaderperf/reflect.c")
snx_test_target(snx-test-shaderperf-reflect gnu23)
target_include_directories(snx-test-shaderperf-reflect PRIVATE "${PROJECT_SOURCE_DIR}/tools/shaderperf")
target_link_libraries(snx-test-shaderperf-reflect PRIVATE Vulkan::Headers)
add_test(NAME shaderperf_reflect COMMAND snx-test-shaderperf-reflect)
# a cross build's generators run on the machine that builds: a test of one names no emulator of the target
add_test(NAME host_tools_emulator COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/host-tools-emulator-test" "-DGENERATOR=${CMAKE_GENERATOR}"
         "-DMAKE=${CMAKE_MAKE_PROGRAM}" -P "${CMAKE_CURRENT_SOURCE_DIR}/host_tools_emulator_test.cmake")
# libcurl's cached build is looked up by the same key from every build tree
add_test(NAME curl_cache_key COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/curl-cache-key-test" "-DGENERATOR=${CMAKE_GENERATOR}"
         "-DMAKE=${CMAKE_MAKE_PROGRAM}" -P "${CMAKE_CURRENT_SOURCE_DIR}/curl_cache_key_test.cmake")
# libz.a, which FFmpeg's configure links its zlib check against with no -flto, stays machine code with SNX_LTO
# (cmake/zlib.cmake). Not on Apple, where cmake/lto.cmake refuses SNX_LTO, nor in a cross build, whose target flags the
# test's project would not get
if(TARGET snx_zlib AND NOT APPLE AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME zlib_lto COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" "-DZLIB=${zlib_SOURCE_DIR}"
             "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/zlib-lto-test" "-DGENERATOR=${CMAKE_GENERATOR}"
             "-DMAKE=${CMAKE_MAKE_PROGRAM}" "-DCC=${CMAKE_C_COMPILER}" "-DCXX=${CMAKE_CXX_COMPILER}" -P
             "${CMAKE_CURRENT_SOURCE_DIR}/zlib_lto_test.cmake")
    set_tests_properties(zlib_lto PROPERTIES SKIP_REGULAR_EXPRESSION "zlib_lto skipped:")
endif()
# where cmake/cross.cmake puts -mabi=elfv2: PowerPC64's compiles and links, no other processor's
add_test(NAME cross_flags COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" -P
         "${CMAKE_CURRENT_SOURCE_DIR}/cross_flags_test.cmake")
# every Configure target cmake/openssl.cmake names is one the pinned OpenSSL defines
if(EXISTS "${snx_openssl_src}/Configure")
    add_test(NAME openssl_targets COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" "-DOPENSSL=${snx_openssl_src}" -P
             "${CMAKE_CURRENT_SOURCE_DIR}/openssl_targets_test.cmake")
endif()
# the meson machine file dav1d's build gets for each target (cmake/meson_machine.cmake), read back by meson where the
# configure found one
add_test(NAME meson_machine COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/meson-machine-test" "-DMESON=${SNX_MESON}" -P
         "${CMAKE_CURRENT_SOURCE_DIR}/meson_machine_test.cmake")
# the compiler cmake/host_cc.cmake takes for a cross build's own programs, another clang first on PATH: Xcode's on a
# Mac host, whose path the Mac case needs
if(EXISTS /usr/bin/clang)
    add_test(NAME host_cc COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
             "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/host-cc-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/host_cc_test.cmake")
endif()
# the JDK cmake/android_jdk.cmake takes for the Android app driver, and the reason it gives when it takes none
add_test(NAME android_jdk COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/android-jdk-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/android_jdk_test.cmake")
# the Android app tells the system it is a game (its game mode and the makers' game launchers)
add_test(NAME android_manifest COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" -P
         "${CMAKE_CURRENT_SOURCE_DIR}/android_manifest.cmake")
# every SNX_ variable a preset of CMakePresets.json sets is one the build reads
add_test(NAME preset_variables COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" -P
         "${CMAKE_CURRENT_SOURCE_DIR}/preset_variables_test.cmake")

# hid's shared memory layout (src/nx/hle/services/hid_layout.h) is checked against Eden's declarations at a pinned
# revision by check_hid_layout.py, run by hand: `python3 tests/check_hid_layout.py --eden <an Eden checkout>`. It
# needs that checkout, so it isn't a test here
add_executable(snx-test-hid-touch hid_touch.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/touch.c"
    "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_touch.c")
snx_test_target(snx-test-hid-touch gnu23)
target_link_libraries(snx-test-hid-touch PRIVATE scarletnx-common)
add_test(NAME hid_touch COMMAND snx-test-hid-touch)

# the Ounce's native video service: layouts, descriptors and a codec's queues (hle/services/native_video_core.c)
add_executable(snx-test-native-video native_video.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/native_video_core.c")
snx_test_target(snx-test-native-video c23)
target_link_libraries(snx-test-native-video PRIVATE scarletnx-common)
add_test(NAME native_video COMMAND snx-test-native-video)

add_executable(snx-test-hid-motion hid_motion.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_motion.c")
snx_test_target(snx-test-hid-motion gnu23)
target_link_libraries(snx-test-hid-motion PRIVATE scarletnx-common)
if(NOT WIN32)
    target_link_libraries(snx-test-hid-motion PRIVATE m)
endif()
add_test(NAME hid_motion COMMAND snx-test-hid-motion)

add_executable(snx-test-startup startup_options.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/startup.c")
snx_test_target(snx-test-startup c23)
target_link_libraries(snx-test-startup PRIVATE scarletnx-common) # an option's older name said (snx_option_renamed)
if(SNX_TRACY)
    # startup.c must see SNX_TRACY so its Tracy flag checks match the runtime.
    target_compile_definitions(snx-test-startup PRIVATE SNX_TRACY=1)
endif()
add_test(NAME startup_options COMMAND snx-test-startup)
if(SNX_TRACY)
    # the Tracy client's lifetime (runtime/platform/tracy.c) over a stand-in client, Tracy's headers without its
    # library: the gate shut before the start and for good after the stop, the client started and stopped once
    add_executable(snx-test-tracy-state tracy_state.c)
    snx_test_target(snx-test-tracy-state gnu23)
    target_include_directories(snx-test-tracy-state PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_include_directories(snx-test-tracy-state SYSTEM PRIVATE
        $<TARGET_PROPERTY:Tracy::TracyClient,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(snx-test-tracy-state PRIVATE SNX_TRACY=1 SNX_TRACY_VERSION="${SNX_TRACY_VERSION}"
        $<TARGET_PROPERTY:Tracy::TracyClient,INTERFACE_COMPILE_DEFINITIONS>)
    target_link_libraries(snx-test-tracy-state PRIVATE scarletnx-common scarletnx-filesystem snx_build_info)
    add_test(NAME tracy_state COMMAND snx-test-tracy-state)
endif()

# the profiler's macros without its client (runtime/platform/tracy.h): none evaluates an argument, so a clock read or a
# lookup written into one costs nothing in a build without Tracy; and nvn_queue.h's context setter likewise
add_executable(snx-test-tracy-off tracy_off.c)
snx_test_target(snx-test-tracy-off c23)
target_include_directories(snx-test-tracy-off PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME tracy_off COMMAND snx-test-tracy-off)

# a collection's programs (common.h's snx_program_index, game_ini.h's hand-over, mods.h's own mods of a program)
add_executable(snx-test-collection-programs collection_programs.c)
snx_test_target(snx-test-collection-programs c23)
target_link_libraries(snx-test-collection-programs PRIVATE scarletnx-common)
add_test(NAME collection_programs COMMAND snx-test-collection-programs)

# the rows of a program a collection's program started (common/utilities/program_start.c), the runtime's screen and
# the launcher's page: each one known, each one not known as "?", a row a screen can't say left out
add_executable(snx-test-program-start program_start.c "${PROJECT_SOURCE_DIR}/src/common/utilities/program_start.c")
snx_test_target(snx-test-program-start c23)
add_test(NAME program_start COMMAND snx-test-program-start)

# a start's mark (common/utilities/run_mark.c): made, ended, said by the next start, a held one left, a long folder,
# a look that deletes none
add_executable(snx-test-run-mark run_mark.c)
snx_test_target(snx-test-run-mark c23)
target_link_libraries(snx-test-run-mark PRIVATE scarletnx-common)
add_test(NAME run_mark COMMAND snx-test-run-mark)

# the console a game's folder plays and its copy of the runtime (common/utilities/game_ini.c), which the launcher reads
add_executable(snx-test-game-system game_system.c)
snx_test_target(snx-test-game-system c23)
target_link_libraries(snx-test-game-system PRIVATE snx_test_common)
add_test(NAME game_system COMMAND snx-test-game-system)

# a run's scripted presses (common/utilities/script.c), which the 3DS, Vita and GBA drivers read
add_executable(snx-test-script script.c)
snx_test_target(snx-test-script c23)
target_link_libraries(snx-test-script PRIVATE snx_test_common)
add_test(NAME script COMMAND snx-test-script)

# a recompile's threads left open (common.h's SnxAotGovernor, recompiler/auto_workers.h): one more a tick while the
# memory measured from the units holds it, on readings of the test's own
add_executable(snx-test-aot-governor aot_governor.c)
snx_test_target(snx-test-aot-governor c23)
target_link_libraries(snx-test-aot-governor PRIVATE snx_test_common)
add_test(NAME aot_governor COMMAND snx-test-aot-governor)

# the summary's build line: its commit's day, and the days from it to the run (common/utilities/logscan.c)
add_executable(snx-test-logscan-age logscan_age.c "${PROJECT_SOURCE_DIR}/src/common/utilities/logscan.c")
snx_test_target(snx-test-logscan-age c23)
add_test(NAME logscan_age COMMAND snx-test-logscan-age)

add_executable(snx-test-driver driver_workflow.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/driver.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/setup.c" "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
    "${PROJECT_SOURCE_DIR}/src/nx/loader/match.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/process.c"
    "${PROJECT_SOURCE_DIR}/src/common/utilities/logscan.c")
snx_test_target(snx-test-driver c23)
target_compile_definitions(snx-test-driver PRIVATE SNX_TRACY_VERSION="${SNX_TRACY_VERSION}")
target_link_libraries(snx-test-driver PRIVATE snx_test_common snx_setup snx_lz4 OpenSSL::Crypto snx_demangle snx_nx_module)
set_target_properties(snx-test-driver PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME driver_workflow COMMAND snx-test-driver)
set_tests_properties(driver_workflow PROPERTIES TIMEOUT 60)
# the same driver as an app's child, which names the game it made ready (SNX_DRIVER_READY, the Android and iOS
# driver's); its collection case, as the other runs need a game played
if(NOT WIN32)
    add_executable(snx-test-driver-ready driver_workflow.c
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/driver.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/setup.c" "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
        "${PROJECT_SOURCE_DIR}/src/nx/loader/match.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/process.c"
        "${PROJECT_SOURCE_DIR}/src/common/utilities/logscan.c")
    snx_test_target(snx-test-driver-ready c23)
    target_compile_definitions(snx-test-driver-ready PRIVATE SNX_DRIVER_READY=1 SNX_TRACY_VERSION="${SNX_TRACY_VERSION}")
    target_link_libraries(snx-test-driver-ready PRIVATE snx_test_common snx_setup snx_lz4 OpenSSL::Crypto snx_demangle
                                                        snx_nx_module)
    set_target_properties(snx-test-driver-ready PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME driver_ready COMMAND snx-test-driver-ready)
    set_tests_properties(driver_ready PROPERTIES TIMEOUT 60)
endif()
add_executable(snx-test-link-file link_file.c)
snx_test_target(snx-test-link-file c23)
target_link_libraries(snx-test-link-file PRIVATE snx_test_common)
add_test(NAME link_file COMMAND snx-test-link-file)
# the macOS Vulkan driver a run picks, in folders the test makes: on every system (docs/hosts/apple.md)
add_executable(snx-test-vulkan-driver-pick vulkan_driver_pick.c)
snx_test_target(snx-test-vulkan-driver-pick c23)
target_link_libraries(snx-test-vulkan-driver-pick PRIVATE snx_test_common)
add_test(NAME vulkan_driver_pick COMMAND snx-test-vulkan-driver-pick)
if(APPLE)
    # MoltenVK and KosmicKrisp, which the app carries and a game's runtime copy (outside the bundle) gets beside it: the
    # paths, and the KosmicKrisp device of the Mac through its ICD and the loader (docs/hosts/apple.md)
    add_executable(snx-test-vulkan-driver vulkan_driver.c)
    snx_test_target(snx-test-vulkan-driver c23)
    target_link_libraries(snx-test-vulkan-driver PRIVATE snx_test_common Vulkan::Headers)
    target_compile_definitions(snx-test-vulkan-driver PRIVATE VK_NO_PROTOTYPES)
    add_test(NAME vulkan_driver COMMAND snx-test-vulkan-driver)
endif()
add_executable(snx-test-launcher-name launcher_name.c)
snx_test_target(snx-test-launcher-name c23)
target_link_libraries(snx-test-launcher-name PRIVATE scarletnx-common)
add_test(NAME launcher_name COMMAND snx-test-launcher-name)
add_executable(snx-test-game-title game_title.c)
snx_test_target(snx-test-game-title c23)
target_link_libraries(snx-test-game-title PRIVATE scarletnx-common)
add_test(NAME game_title COMMAND snx-test-game-title)
if(UNIX)
    foreach(name user_dirs left_behind)
        string(REPLACE "_" "-" target "snx-test-${name}")
        add_executable(${target} ${name}.c)
        snx_test_target(${target} c23)
        target_link_libraries(${target} PRIVATE scarletnx-common)
        add_test(NAME ${name} COMMAND ${target})
    endforeach()
endif()
# the setup's files and choices (common/utilities/setup.c), its notices against docs/user/deprecations.md
if(UNIX AND TARGET snx_setup)
    add_executable(snx-test-setup setup.c)
    snx_test_target(snx-test-setup c23)
    target_link_libraries(snx-test-setup PRIVATE snx_setup)
    target_compile_definitions(snx-test-setup PRIVATE SNX_DEPRECATIONS="${PROJECT_SOURCE_DIR}/docs/user/deprecations.md")
    add_test(NAME setup COMMAND snx-test-setup)
    # the driver's setup in a terminal, its answers from a file
    add_executable(snx-test-setup-terminal setup_terminal.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/setup.c")
    snx_test_target(snx-test-setup-terminal c23)
    target_link_libraries(snx-test-setup-terminal PRIVATE snx_setup)
    add_test(NAME setup_terminal COMMAND snx-test-setup-terminal)
endif()

if(UNIX)
    add_executable(snx-test-gdbstub gdbstub.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/gdbstub.c")
    snx_test_target(snx-test-gdbstub gnu23)
        target_link_libraries(snx-test-gdbstub PRIVATE snx_test_common)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gdbstub_native COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdbstub.py" $<TARGET_FILE:snx-test-gdbstub>)
        set_tests_properties(gdbstub_native PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(snx-test-run-ticking run_ticking.c)
snx_test_target(snx-test-run-ticking c23)
target_link_libraries(snx-test-run-ticking PRIVATE snx_test_common)
add_test(NAME run_ticking COMMAND snx-test-run-ticking)

# GNU C23 for hactool's headers, which build the synthetic dumps (hac_synth.h) and their NCZs (ncz_synth.h); extract.c
# itself stays ISO C23 in the driver
add_executable(snx-test-extract extract.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/extract.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/container.c" "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
    "${PROJECT_SOURCE_DIR}/src/nx/loader/match.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c")
snx_test_target(snx-test-extract gnu23)
target_link_libraries(snx-test-extract PRIVATE snx_test_common hactool_internals snx_lz4 OpenSSL::Crypto snx_nx_module)
if(WIN32) # a scenario's crash says where it was (one ended 0xC0000005 on Windows on Arm, saying nothing)
    target_sources(snx-test-extract PRIVATE win32_crash.c)
endif()
add_test(NAME extraction_native COMMAND snx-test-extract)

# scarletnx --pack: synthetic encrypted NCAs packed into an NSP and an XCI of plain ones (hac_synth.h, as above)
add_executable(snx-test-pack pack.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/pack.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/fabricate.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/container.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/extract.c" "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
    "${PROJECT_SOURCE_DIR}/src/nx/loader/match.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c")
snx_test_target(snx-test-pack gnu23)
target_link_libraries(snx-test-pack PRIVATE snx_test_common hactool_internals snx_lz4 OpenSSL::Crypto snx_nx_module)
add_test(NAME pack_plain COMMAND snx-test-pack)

add_executable(snx-test-loader loader_test.c "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
    "${PROJECT_SOURCE_DIR}/src/nx/loader/match.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c")
snx_test_target(snx-test-loader c23)
target_link_libraries(snx-test-loader PRIVATE snx_test_common snx_lz4 OpenSSL::Crypto snx_nx_module) # the route list
add_test(NAME loader_native COMMAND snx-test-loader)
# the linker (src/nx/loader/relocate.c): three modules relocated into copies and in place, and the two compared
add_executable(snx-test-relocate relocate_test.c "${PROJECT_SOURCE_DIR}/src/nx/loader/relocate.c")
snx_test_target(snx-test-relocate c23)
target_link_libraries(snx-test-relocate PRIVATE scarletnx-common)
add_test(NAME loader_relocate COMMAND snx-test-relocate)
# the read-only function matcher: names for a stripped module's functions from the sdk module's exports
add_executable(snx-test-function-match function_match.c "${PROJECT_SOURCE_DIR}/src/nx/loader/match.c")
snx_test_target(snx-test-function-match c23)
target_link_libraries(snx-test-function-match PRIVATE scarletnx-common)
add_test(NAME function_match COMMAND snx-test-function-match)

add_executable(snx-test-runtime-nro runtime_nro.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/loader.c")
snx_test_target(snx-test-runtime-nro gnu23)
target_link_libraries(snx-test-runtime-nro PRIVATE snx_lz4 scarletnx-common)
add_test(NAME runtime_nro COMMAND snx-test-runtime-nro)

if(NOT WIN32)
    # a guest's call into a loaded NRO's code that the module lacks, through RtAPI.miss, is llvm_module's
    add_executable(snx-test-ldr-service ldr_service.c
        "${PROJECT_SOURCE_DIR}/src/nx/hle/services/ldr.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_test_target(snx-test-ldr-service gnu23)
    snx_service_swipc(snx-test-ldr-service hle/services/ldr.swipc)
    target_link_libraries(snx-test-ldr-service PRIVATE OpenSSL::Crypto m)
    snx_cpu_helpers_flags(snx-test-ldr-service)
    add_test(NAME ldr_service COMMAND snx-test-ldr-service)
    # again as on a host with 64 KiB pages (PowerPC64), each holding sixteen guest pages
    add_test(NAME ldr_service_64k_pages COMMAND snx-test-ldr-service --host_page=65536)
    set_tests_properties(ldr_service ldr_service_64k_pages PROPERTIES TIMEOUT 60)
endif()

# Deterministic FFmpeg API double: no decoder dependency in tools-only builds.
add_executable(snx-test-nvdec-packets nvdec_packets.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp"
    support/nvdec_ref.c)
snx_test_target(snx-test-nvdec-packets gnu23)
target_include_directories(snx-test-nvdec-packets BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_stub")
add_test(NAME nvdec_packets COMMAND snx-test-nvdec-packets)
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-nvdec-decode nvdec_packets.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp"
        support/nvdec_ref.c)
    snx_test_target(snx-test-nvdec-decode gnu23)
    target_compile_definitions(snx-test-nvdec-decode PRIVATE SNX_TEST_REAL_FFMPEG)
    target_link_libraries(snx-test-nvdec-decode PRIVATE FFmpeg::avcodec)
    add_test(NAME nvdec_decode COMMAND snx-test-nvdec-decode)
endif()
# FFmpeg's image codecs, libswscale and zlib come with the Android driver alone (cmake/ffmpeg.cmake)
if(TARGET FFmpeg::avcodec AND Python3_Interpreter_FOUND)
    if(snx_ffmpeg_images)
        set(snx_ffmpeg_images_want 1)
    else()
        set(snx_ffmpeg_images_want 0)
    endif()
    add_test(NAME ffmpeg_images COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_images.py"
                                        "${snx_ffmpeg_prefix}/private" ${snx_ffmpeg_images_want})
endif()
# FFmpeg's configure finds the build's own dav1d with pkg-config (cmake/ffmpeg.cmake) whatever PKG_CONFIG_SYSROOT_DIR
# the build runs with: a Linux cross preset's would put its system root in front of dav1d's folder
if(snx_dav1d_ready)
    find_program(SNX_TEST_PKG_CONFIG NAMES pkg-config pkgconf)
    if(SNX_TEST_PKG_CONFIG)
        add_test(NAME ffmpeg_dav1d_pkgconfig
                 COMMAND "${CMAKE_COMMAND}" -E env PKG_CONFIG_SYSROOT_DIR=/snx-test-sysroot "${CMAKE_COMMAND}" -E env
                         ${snx_ffmpeg_env} "${SNX_TEST_PKG_CONFIG}" --cflags --libs --static dav1d)
        set_tests_properties(ffmpeg_dav1d_pkgconfig PROPERTIES PASS_REGULAR_EXPRESSION "-ldav1d"
                                                               FAIL_REGULAR_EXPRESSION "snx-test-sysroot")
    endif()
endif()
if(SNX_TEST_VP9_VECTORS)
    # nvdec.c's VP9 jobs on libvpx's test vectors, each image against the MD5 libvpx lists for it, at its own job's
    # fence (nvdec_vp9_vectors.c). FFmpeg's decoder works the jobs out of each vector from its private state
    # (nvdec_vp9_jobs.c), so that part builds against FFmpeg's source and configuration. The vectors and their lists
    # come from the WebM project's test data (BSD-3-Clause), downloaded once into CPM_SOURCE_CACHE: snx_vp9_<name> and
    # snx_vp9_<name>_md5 in cpmfile.json, for each name here. Hidden frames, contexts that adapt, frame parallel mode, a
    # superframe of hidden frames, frames shown again, intra-only frames, spatial layers, tiles, size changes,
    # segmentation, loop filter and quantizer deltas. Off by default: a build without the network (a Flatpak's, a
    # distribution's) would wait on the downloads
    set(snx_vp9_vectors
        vp90-2-02-size-66x66.webm vp90-2-03-deltaq.webm vp90-2-05-resize.ivf vp90-2-07-frame_parallel.webm
        vp90-2-08-tile-4x4.webm vp90-2-09-lf_deltas.webm vp90-2-10-show-existing-frame.webm vp90-2-11-size-351x287.webm
        vp90-2-15-segkey.webm vp90-2-15-segkey_adpq.webm vp90-2-16-intra-only.webm vp90-2-17-show-existing-frame.webm
        vp90-2-19-skip.webm vp90-2-20-big_superframe-01.webm vp90-2-21-resize_inter_320x180_5_1-2.webm
        vp90-2-22-svc_1280x720_3.ivf)
    add_library(snx-test-nvdec-vp9-jobs STATIC nvdec_vp9_jobs.c)
    snx_test_target(snx-test-nvdec-vp9-jobs gnu23)
    target_include_directories(snx-test-nvdec-vp9-jobs PRIVATE ${snx_ffmpeg_private_include})
    # FFmpeg's headers as system headers, for their warnings (on Windows, compat/w32pthreads.h's unused parameters);
    # as -isystem folders they would come after a -I that holds another FFmpeg's (cmake/ffmpeg.cmake)
    target_compile_options(snx-test-nvdec-vp9-jobs PRIVATE --system-header-prefix=libavcodec/
                           --system-header-prefix=libavutil/ --system-header-prefix=compat/)
    target_link_libraries(snx-test-nvdec-vp9-jobs PUBLIC FFmpeg::avcodec)
    add_executable(snx-test-nvdec-vp9-vectors nvdec_vp9_vectors.c
                   "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp"
                   support/nvdec_ref.c)
    snx_test_target(snx-test-nvdec-vp9-vectors gnu23)
    target_link_libraries(snx-test-nvdec-vp9-vectors PRIVATE snx-test-nvdec-vp9-jobs)
    foreach(name IN LISTS snx_vp9_vectors)
        string(REGEX REPLACE "^vp90-2-|\\.(webm|ivf)$" "" short "${name}")
        snx_package_file(snx_vp9_${short})
        snx_package_file(snx_vp9_${short}_md5)
        add_test(NAME nvdec_vp9_${short}
                 COMMAND snx-test-nvdec-vp9-vectors "${snx_vp9_${short}_SOURCE_DIR}/${name}"
                         "${snx_vp9_${short}_md5_SOURCE_DIR}/${name}.md5")
    endforeach()
endif()
# VP9 packets rebuilt from NVDEC jobs (gpu/video/vp9_rebuild.cpp): the destination of the frame each emits, and
# setups with invalid tile layouts refused before a header is built
add_executable(snx-test-vp9-rebuild vp9_rebuild.cpp "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp")
snx_test_target(snx-test-vp9-rebuild gnu23)
add_test(NAME vp9_rebuild COMMAND snx-test-vp9-rebuild)
# the 2D engine's blit in guest memory (fermi2d.c, dma.c's bl_span_ptr): block linear scaling, an odd window, the
# renderer's hooks once a blit; conversions between color formats through the shader's CPU build
add_executable(snx-test-fermi2d-blit fermi2d_blit.c support/fermi2d_ref.c support/half.c glsl/blit_convert_cpu.cpp)
snx_test_target(snx-test-fermi2d-blit gnu23)
target_include_directories(snx-test-fermi2d-blit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/glsl")
set_target_properties(snx-test-fermi2d-blit PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME fermi2d_blit COMMAND snx-test-fermi2d-blit)
# vic.c's CPU composition, through vic_execute, with a renderer that declines the GPU
add_executable(snx-test-vic-compose vic_compose.c support/vic_ref.c)
snx_test_target(snx-test-vic-compose gnu23)
add_test(NAME vic_compose COMMAND snx-test-vic-compose)
if(NOT WIN32)
    # host1x.c's channels with pthread stand-ins for the platform: IOVAs, GET_SYNCPOINT, SUBMIT's checks and fences,
    # the command stream, a host class wait and a close that runs the queue first
    add_executable(snx-test-host1x-channel host1x_channel.c)
    snx_test_target(snx-test-host1x-channel gnu23)
    add_test(NAME host1x_channel COMMAND snx-test-host1x-channel)
endif()
if(NOT SNX_TOOLS_ONLY)
    # tex_upload (gpu/vk/texture.c) into the staging buffer: block-linear rows against the layout, mip levels, 3D,
    # pitch linear, a converted format; a format uploaded as it is needs no level-sized buffer
    add_executable(snx-test-tex-upload-rows tex_upload_rows.c tracy_stubs.c
                   "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/blocklinear.c") # g_gob_native, which astc_kind.h reads
    snx_test_target(snx-test-tex-upload-rows gnu23)
    target_link_libraries(snx-test-tex-upload-rows PRIVATE snx_nx_flags)
    add_test(NAME tex_upload_rows COMMAND snx-test-tex-upload-rows)
    # the page cache's reads (gpu/vk/pagespace.c) over a model of guest memory, the write tracking, the ring and the
    # region's buffer: every range it serves holds guest memory's bytes
    add_executable(snx-test-vk-pagespace vk_pagespace.c tracy_stubs.c)
    snx_test_target(snx-test-vk-pagespace gnu23)
    target_link_libraries(snx-test-vk-pagespace PRIVATE snx_nx_flags)
    add_test(NAME vk_pagespace COMMAND snx-test-vk-pagespace)
    # the barriers of an image's uses (gpu/vk/surface.c vk_surf_use) in the layouts that VK_LAYOUT names: with unified
    # image layouts, GENERAL and no barrier between reads of another kind; without them, each use's own layout
    add_executable(snx-test-vk-surf-layouts vk_surf_layouts.c tracy_stubs.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-vk-surf-layouts gnu23)
    target_link_libraries(snx-test-vk-surf-layouts PRIVATE snx_nx_flags)
    add_test(NAME vk_surf_layouts COMMAND snx-test-vk-surf-layouts)
    # the timeline of NVN's timestamp reports (gpu/vk/stamp.c) over a model of the queue's timestamp queries, with no
    # device: a present's submissions back to back, the runtime's own work left out, values that only rise
    add_executable(snx-test-vk-stamp vk_stamp.c tracy_stubs.c)
    snx_test_target(snx-test-vk-stamp gnu23)
    target_link_libraries(snx-test-vk-stamp PRIVATE snx_nx_flags)
    add_test(NAME vk_stamp COMMAND snx-test-vk-stamp)
    # a draw that binds an owner where it is (gpu/vk/buffer.c buf_get_owned): one that a draw or the page cache wrote
    # in this submission takes a pass end and a barrier first, any other none
    add_executable(snx-test-vk-buffer-owned vk_buffer_owned.c tracy_stubs.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-vk-buffer-owned gnu23)
    target_link_libraries(snx-test-vk-buffer-owned PRIVATE snx_nx_flags)
    add_test(NAME vk_buffer_owned COMMAND snx-test-vk-buffer-owned)
    # the GPU pages of a range as the buffer cache reads them (gpu/vk/buffer.c), over a model of the GMMU: the walks
    # one run at a time give the pages, run flags and byte spans of a walk one page at a time
    add_executable(snx-test-vk-buffer-pages vk_buffer_pages.c tracy_stubs.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-vk-buffer-pages gnu23)
    target_link_libraries(snx-test-vk-buffer-pages PRIVATE snx_nx_flags)
    add_test(NAME vk_buffer_pages COMMAND snx-test-vk-buffer-pages)
    # the scans of written pages on Windows (gpu/vk/buffer.c, WW_SCANS_SERIAL), on any host: the scanner starts no scan
    # while a check scans
    add_executable(snx-test-vk-write-scan vk_write_scan.c tracy_stubs.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-vk-write-scan gnu23)
    target_link_libraries(snx-test-vk-write-scan PRIVATE snx_nx_flags)
    add_test(NAME vk_write_scan COMMAND snx-test-vk-write-scan)
    # the file of shaders and pipelines kept across runs (gpu/vk/pipeline_file.c): records back as written, pipelines
    # by their shaders' records (a shipped shader's reference among them), a cut or changed record ending the reading,
    # another key starting over, a full file; its records packed into zstd units and unpacked in their place, a unit
    # cut, changed or not as it says ending the reading, a file of version 2 rebuilt; and the shipped shaders' file
    # read through its mapping
    add_executable(snx-test-pipeline-file pipeline_file.c tracy_stubs.c)
    snx_test_target(snx-test-pipeline-file gnu23)
    target_link_libraries(snx-test-pipeline-file PRIVATE snx_nx_flags snx_zstd scarletnx-common snx_vk_common)
    add_test(NAME pipeline_file COMMAND snx-test-pipeline-file)
    add_executable(snx-test-vk-pipeline-probe-flags vk_pipeline_probe_flags.c)
    snx_test_target(snx-test-vk-pipeline-probe-flags gnu23)
    target_link_libraries(snx-test-vk-pipeline-probe-flags PRIVATE snx_nx_flags)
    add_test(NAME vk_pipeline_probe_flags COMMAND snx-test-vk-pipeline-probe-flags)
    add_executable(snx-test-vk-resource-stats vk_resource_stats.c)
    snx_test_target(snx-test-vk-resource-stats gnu23)
    add_test(NAME vk_resource_stats COMMAND snx-test-vk-resource-stats)
    # the shape of a set of ranges (gpu/vk/range_shape.c), which the buffer cache's measurements report: the bytes
    # held twice, the joined pieces, the granules they touch and the sizes
    add_executable(snx-test-vk-range-shape vk_range_shape.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/range_shape.c")
    snx_test_target(snx-test-vk-range-shape gnu23)
    add_test(NAME vk_range_shape COMMAND snx-test-vk-range-shape)
    # the calculations of the texture cache's measurements (gpu/vk/tex_census.c): a key's hash, the table of evicted
    # keys, a texture among the guest's live texture objects, the sums by host format and ASTC block size
    add_executable(snx-test-vk-tex-census vk_tex_census.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/tex_census.c"
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/range_shape.c")
    snx_test_target(snx-test-vk-tex-census gnu23)
    add_test(NAME vk_tex_census COMMAND snx-test-vk-tex-census)
    # the registry of the guest's live texture objects (gpu/guest_textures.c): counted addresses, each in reach while
    # the table grows and while others leave it
    add_executable(snx-test-guest-textures guest_textures.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/guest_textures.c")
    snx_test_target(snx-test-guest-textures gnu23)
    target_link_libraries(snx-test-guest-textures PRIVATE scarletnx-platform) # the registry's PMutex
    add_test(NAME guest_textures COMMAND snx-test-guest-textures)
    # the BC7 cache of ASTC levels (gpu/vk/bc7_cache.c), without a GPU: every input that changes a level's blocks
    # changes its key, blocks come back as stored while it holds them, past its bound it keeps the levels used most,
    # a low memory word frees what no recent present used, and a hit queued for the recording thread is copied from
    # blocks that are still there
    add_executable(snx-test-bc7-cache bc7_cache.c tracy_stubs.c)
    snx_test_target(snx-test-bc7-cache gnu23)
    target_link_libraries(snx-test-bc7-cache PRIVATE snx_nx_flags snx_xxhash scarletnx-common scarletnx-platform)
    add_test(NAME bc7_cache COMMAND snx-test-bc7-cache)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The real cache saver with a fake driver: compilation defers periodic snapshots without losing dirty work.
        # Include only its implementation; sanitizer-retained callback tables make whole-file linker GC unsuitable.
        # It writes through the common writer (common/gpu/vk/vk_common.c), which calls volk's pointers
        if(TARGET snx_vk_common)
            add_executable(snx-test-vk-pipeline-save vk_pipeline_save.c "${volk_SOURCE_DIR}/volk.c")
            set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS
                                        "${snx_third_party_flags}")
            snx_test_target(snx-test-vk-pipeline-save gnu23)
            target_link_libraries(snx-test-vk-pipeline-save PRIVATE snx_nx_flags snx_vk_common)
            add_test(NAME vk_pipeline_save COMMAND snx-test-vk-pipeline-save)
        endif()
    endif()
    # graphics storage images' write-backs (gpu/vk/image_out.c) against a stand-in GPU: a draw over one whose copy is
    # recorded goes on from it with no read or wait, and they land in order, each over the bytes the last one left
    add_executable(snx-test-vk-image-out vk_image_out.c tracy_stubs.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/image_out.c"
                   "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-vk-image-out gnu23)
    target_link_libraries(snx-test-vk-image-out PRIVATE snx_nx_flags)
    add_test(NAME vk_image_out COMMAND snx-test-vk-image-out)
    # RenderDoc's captures (gpu/vk/renderdoc.c) against a stand-in RenderDoc: neither renderdoc= nor RenderDoc's program,
    # renderdoc= without the library, RenderDoc's program that started the game, and renderdoc= loading it; the capture
    # key's capture and renderdoc_frames', what the stats box is told, the notices over the game and the write's
    # progress lines (a thread the test lets tick), and RenderDoc's own captures. A process each
    add_executable(snx-test-vk-renderdoc vk_renderdoc.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/renderdoc.c"
        "${PROJECT_SOURCE_DIR}/src/common/gpu/vk/renderdoc.c")
    snx_test_target(snx-test-vk-renderdoc gnu23)
    target_link_libraries(snx-test-vk-renderdoc PRIVATE snx_nx_flags)
    foreach(case none missing started setting write theirs)
        add_test(NAME vk_renderdoc_${case} COMMAND snx-test-vk-renderdoc ${case})
    endforeach()
    # the game's menu in a console's window (common/utilities/menu_window.c), under SDL's offscreen driver
    add_executable(snx-test-menu-window menu_window.c)
    snx_test_target(snx-test-menu-window gnu23)
    target_link_libraries(snx-test-menu-window PRIVATE snx_menu snx_vpad_consoles)
    target_include_directories(snx-test-menu-window PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}") # support/
    add_test(NAME menu_window COMMAND snx-test-menu-window)
    set_tests_properties(menu_window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # RenderDoc's lines in a console's window (common/gpu/vk/renderdoc_window.c): its say and notice take them
    add_executable(snx-test-vk-renderdoc-window vk_renderdoc_window.c)
    snx_test_target(snx-test-vk-renderdoc-window gnu23)
    target_link_libraries(snx-test-vk-renderdoc-window PRIVATE snx_vk_renderdoc)
    add_test(NAME vk_renderdoc_window COMMAND snx-test-vk-renderdoc-window)
    # vertex_input_dynamic_state (gpu/vk/vertex_input.c): its values, read from a .ini without it and with it, the
    # ban the table holds for stand-in devices and the log line of each case
    include("${PROJECT_SOURCE_DIR}/cmake/vk_extension_bans.cmake")
    set(snx_vk_extension_bans_sources "${PROJECT_SOURCE_DIR}/src/common/gpu/vk/extension_bans.c"
                                      "${PROJECT_BINARY_DIR}/generated/vk_extension_bans_text.c")
    add_executable(snx-test-vk-vertex-input vk_vertex_input.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/vertex_input.c"
                   "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c" ${snx_vk_extension_bans_sources})
    snx_test_target(snx-test-vk-vertex-input c23)
    target_link_libraries(snx-test-vk-vertex-input PRIVATE snx_nx_flags)
    add_test(NAME vk_vertex_input COMMAND snx-test-vk-vertex-input)
    # gpu_pstate (common/gpu/vk/gpu_pstate.c) against a stand-in kernel, ioctl wrapped at link time, on a stand-in /sys
    # and /dev: its values, auto and on on discrete and integrated GPUs, a level set on the PC, each refusal said
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        add_executable(snx-test-vk-gpu-pstate vk_gpu_pstate.c "${PROJECT_SOURCE_DIR}/src/common/gpu/vk/gpu_pstate.c")
        snx_test_target(snx-test-vk-gpu-pstate gnu23)
        target_include_directories(snx-test-vk-gpu-pstate PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_options(snx-test-vk-gpu-pstate PRIVATE "LINKER:--wrap=ioctl")
        if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
            target_link_options(snx-test-vk-gpu-pstate PRIVATE "LINKER:--wrap=devname") # devfs's name of the node
        endif()
        add_test(NAME vk_gpu_pstate COMMAND snx-test-vk-gpu-pstate)
    endif()
    # the block texture formats a device is given (gpu/vk/format.c), with no GPU: a player's Mali-G52 from its vkjson
    # (ASTC and ETC2 sampled as the games' blocks, BCn encoded again as ETC2 and EAC), a desktop GPU, native_bc=0 and the
    # other settings at 0, and the log's line for each family
    if(TARGET snx_vk_common)
        add_executable(snx-test-vk-block-formats vk_block_formats.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/format.c"
                       "${volk_SOURCE_DIR}/volk.c")
        set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
        # format.c's VkFmt initializers leave the last members to 0, as the renderer's flags allow
        set_source_files_properties("${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/format.c" PROPERTIES COMPILE_OPTIONS
                                    -Wno-missing-field-initializers)
        snx_test_target(snx-test-vk-block-formats gnu23)
        target_link_libraries(snx-test-vk-block-formats PRIVATE snx_nx_flags snx_vk_common)
        add_test(NAME vk_block_formats COMMAND snx-test-vk-block-formats)
    endif()
    # graphics pipeline libraries (gpu/vk/pipeline_parts.c): the choice from the setting and stand-in devices, each
    # condition that turns them off named, and the dynamic pre-rasterization state each device member gives where the
    # table of bans holds none for it
    add_executable(snx-test-vk-pipeline-parts vk_pipeline_parts.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/pipeline_parts.c"
                   ${snx_vk_extension_bans_sources})
    snx_test_target(snx-test-vk-pipeline-parts gnu23)
    target_link_libraries(snx-test-vk-pipeline-parts PRIVATE snx_nx_flags)
    add_test(NAME vk_pipeline_parts COMMAND snx-test-vk-pipeline-parts)
    # an Ounce's shading rate image (gpu/vk/shading_rate_table.c, shading_rate.glsl): the extension each stand-in
    # device takes, each palette entry's value, the NVN palette, and the conversion's body run on the CPU
    # (tests/glsl/glsl.hpp) against a reference made by pixels: no GPU needed
    add_executable(snx-test-vk-shading-rate vk_shading_rate.c glsl/shading_rate_glsl.cpp support/shading_rate_ref.c
                   "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/shading_rate_table.c")
    snx_test_target(snx-test-vk-shading-rate gnu23)
    target_include_directories(snx-test-vk-shading-rate PRIVATE glsl ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(snx-test-vk-shading-rate PRIVATE snx_nx_flags)
    add_test(NAME vk_shading_rate COMMAND snx-test-vk-shading-rate)
    # constant buffer words folded into a translation (gpu/vk/cb_fold.c): which words fold, what a translation records
    # of its loads, when it holds for a draw's buffers, and how many of a program's translations fold by value
    add_executable(snx-test-vk-cb-fold vk_cb_fold.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/cb_fold.c")
    snx_test_target(snx-test-vk-cb-fold gnu23)
    target_link_libraries(snx-test-vk-cb-fold PRIVATE snx_nx_flags)
    add_test(NAME vk_cb_fold COMMAND snx-test-vk-cb-fold)
    # the drivers known to break an extension or feature (common/gpu/vk/extension_bans.c), with no GPU: the table's errors,
    # each driver's versions at every bound's edge, the filter and the features asked for against stand-in devices,
    # the table the build carries, and vulkan_ban_ignore read from a .ini. With --devices it lists the lines that hold
    # for this machine's drivers, the workspace's retest reads that
    if(TARGET snx_vk_common)
        add_executable(snx-test-vk-bans vk_extension_bans.c ${snx_vk_extension_bans_sources}
                       "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c" "${volk_SOURCE_DIR}/volk.c")
        set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
        snx_test_target(snx-test-vk-bans gnu23)
        target_link_libraries(snx-test-vk-bans PRIVATE snx_nx_flags snx_vk_common)
        add_test(NAME vk_extension_bans COMMAND snx-test-vk-bans)
        # the commands each Vulkan device extension requires (common/gpu/vk/extension_commands.c), with no GPU: the
        # table tools/vkcmdgen generates from the registry, its depends, a stand-in device's check and the hide list
        add_executable(snx-test-vk-commands vk_extension_commands.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-commands gnu23)
        target_link_libraries(snx-test-vk-commands PRIVATE snx_nx_flags snx_vk_common)
        add_test(NAME vk_extension_commands COMMAND snx-test-vk-commands)
        # the one way a renderer makes its device (common/gpu/vk/device.c), with stand-in drivers: the settings before
        # the bans, an extension no setting names refused, one lacking a command out of the device made again, the
        # devices passed over with their reasons, and the log
        add_executable(snx-test-vk-device vk_device_make.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-device gnu23)
        target_link_libraries(snx-test-vk-device PRIVATE snx_nx_flags snx_vk_common)
        add_test(NAME vk_device_make COMMAND snx-test-vk-device)
    endif()
    if(ANDROID)
        # the GPU driver a run asks for (common/gpu/vk/gpu_driver_load.c) where none loads: Freedreno's variables set,
        # a wrong one, a package asked for here and a folder name that is none stopping the run with why, a run a case
        add_executable(snx-test-gpu-driver-load gpu_driver_load.c)
        snx_test_target(snx-test-gpu-driver-load gnu23)
        target_link_libraries(snx-test-gpu-driver-load PRIVATE snx_nx_flags snx_vk_common scarletnx-common)
        foreach(case IN ITEMS env sets badenv here badid)
            add_test(NAME gpu_driver_load_${case} COMMAND snx-test-gpu-driver-load ${case})
        endforeach()
    endif()
endif()

add_executable(snx_recompiler_test recompiler_native.c)
snx_test_target(snx_recompiler_test gnu23)
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
    snx_test_target(snx-test-llvm-qemu-differential gnu23)
    target_include_directories(snx-test-llvm-qemu-differential PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-llvm-qemu-differential PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    snx_decodetree_a64(snx-test-llvm-qemu-differential)
    set(snx_lqd_expected "${CMAKE_CURRENT_SOURCE_DIR}/llvm_qemu_expected.txt")
    if(SNX_QEMU_REF)
        add_dependencies(snx-test-llvm-qemu-differential snx_qemu_ref_runner)
        add_test(NAME llvm_qemu_differential COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu" "${snx_lqd_expected}" --quiet)
        # the same steps loaded with the JIT's loader, built without the host's extensions (the emitters'
        # portable forms), and built without its architecture too (the generic path PowerPC64, RISC-V, LoongArch and
        # MIPS64 hosts take)
        add_test(NAME llvm_qemu_differential_jit COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_jit" "${snx_lqd_expected}" --quiet
                 --jit)
        add_test(NAME llvm_qemu_differential_portable COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_portable" "${snx_lqd_expected}"
                 --quiet --portable)
        add_test(NAME llvm_qemu_differential_generic COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_generic" "${snx_lqd_expected}"
                 --quiet --generic)
        # an AVX-512 host as one with AVX2 alone (--without-avx512), skipped on a host without AVX-512
        add_test(NAME llvm_qemu_differential_avx2 COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_avx2" "${snx_lqd_expected}" --quiet
                 --without-avx512)
        add_test(NAME llvm_qemu_differential_abi COMMAND snx-test-llvm-qemu-differential "${SNX_QEMU_AARCH64}"
                 "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/llvm_qemu_abi" "${snx_lqd_expected}"
                 --quiet --abi-contract)
        set_tests_properties(llvm_qemu_differential_jit llvm_qemu_differential_portable llvm_qemu_differential_generic
                             llvm_qemu_differential_abi PROPERTIES TIMEOUT 600)
        set_tests_properties(llvm_qemu_differential_avx2 PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
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
    # The lowering census (llvm_lowering.c): each A64 SIMD, FP and crypto op's class on nine emitter paths against
    # llvm_lowering_expected.txt, which a change that moves an op updates in the same commit (--update)
    add_executable(snx-test-llvm-lowering llvm_lowering.c llvm_qemu_words.c)
    snx_test_target(snx-test-llvm-lowering c23)
    target_include_directories(snx-test-llvm-lowering PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-llvm-lowering PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    snx_decodetree_a64(snx-test-llvm-lowering)
    add_test(NAME llvm_lowering COMMAND snx-test-llvm-lowering "${CMAKE_CURRENT_SOURCE_DIR}/llvm_lowering_expected.txt")
    set_tests_properties(llvm_lowering PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    # the integer, branch, general-purpose load and store and scalar floating-point emitters sailgen generates from the
    # Sail against the hand-written ones they replaced (support/ir_int_reference.c, ir_ldst_reference.c,
    # ir_fpu_reference.c): the same optimized IR word by word on three paths, the same results (registers, flags, FPCR,
    # FPSR, guest memory) run here
    add_executable(snx-test-llvm-sail-ir llvm_sail_ir.c llvm_qemu_words.c llvm_steps.c support/ir_int_reference.c
                   support/ir_ldst_reference.c support/ir_fpu_reference.c support/ir_vec_reference.c)
    snx_test_target(snx-test-llvm-sail-ir gnu23)
    target_include_directories(snx-test-llvm-sail-ir PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-llvm-sail-ir PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    snx_decodetree_a64(snx-test-llvm-sail-ir)
    # a third of the words each, so the sanitizers' build runs each part in its timeout and the parts run side by side
    foreach(part 1 2 3)
        add_test(NAME llvm_sail_ir_${part} COMMAND snx-test-llvm-sail-ir --part ${part}/3)
        set_tests_properties(llvm_sail_ir_${part} PROPERTIES TIMEOUT 600)
    endforeach()
    # sailgen's x86-64 back end (its instruction selection derived from the Intel SDM, cmake/sdm.cmake) against its LLVM
    # back end: the same registers, flags, next pc and UNDEF from the same random states, word by word, on the host's
    # BMI1, BMI2 and LZCNT and without them
    # sailgen's other native back ends against the LLVM back end (sail_mach.c, one executable each: arm64's encoders the
    # A64 Sail's, riscv64's LLVM's TableGen records'), their code run under Unicorn on any host (the test's memory
    # mapped into it at the same addresses), where Unicorn is installed
    find_path(SNX_UNICORN_INCLUDE_DIR unicorn/unicorn.h)
    find_library(SNX_UNICORN_LIBRARY unicorn)
    if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
        # machine, Unicorn's architecture (host_unicorn.h), the machine's features the code may use
        # (a big-endian host's, ppc64be and mips64be: its machine's emitters with RK_BIG_ENDIAN)
        foreach(mach arm64:HOSTUC_ARM64:3 rv64:HOSTUC_RV64:0 ppc64:HOSTUC_PPC64LE:0 mips64:HOSTUC_MIPS64EL:0
                     la64:HOSTUC_LA64:0 ppc64be:HOSTUC_PPC64:0 mips64be:HOSTUC_MIPS64:0 s390x:HOSTUC_S390X:0)
            string(REPLACE ":" ";" parts "${mach}")
            list(GET parts 0 m)
            list(GET parts 1 uc)
            list(GET parts 2 features)
            string(REGEX REPLACE "be$" "" gen "${m}")
            add_executable(snx-test-sail-${m} sail_mach.c sail_mach_emit.c llvm_qemu_words.c llvm_steps.c
                           qemu_ref/qemu_ref.c support/host_unicorn.c support/la64_interp.c
                           "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                           "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
            snx_cpu_helpers_flags(snx-test-sail-${m})
            snx_test_target(snx-test-sail-${m} gnu23)
            target_compile_definitions(snx-test-sail-${m} PRIVATE SAIL_MACH_INC="a64_${gen}.inc" SAIL_MACH_UC=${uc}
                                                                     SAIL_MACH_HOST=${features})
            if(NOT gen STREQUAL m)
                target_compile_definitions(snx-test-sail-${m} PRIVATE RK_BIG_ENDIAN=1)
            endif()
            target_include_directories(snx-test-sail-${m} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${SNX_UNICORN_INCLUDE_DIR}")
            target_link_libraries(snx-test-sail-${m} PRIVATE snx_llvm snx_driver "${SNX_UNICORN_LIBRARY}" ${CMAKE_DL_LIBS})
            snx_decodetree_a64(snx-test-sail-${m})
            if(m STREQUAL "arm64")
                snx_sailgen_a64_arm64(snx-test-sail-${m})
            else()
                snx_sailgen_a64_risc(snx-test-sail-${m} ${gen})
            endif()
            add_test(NAME sail_${m} COMMAND snx-test-sail-${m})
            add_test(NAME sail_${m}_spill COMMAND snx-test-sail-${m} --pool 3 --states 32)
            set_tests_properties(sail_${m} sail_${m}_spill PROPERTIES TIMEOUT 900)
            if(NOT m STREQUAL "arm64")
                # the Advanced SIMD a machine without vector registers makes of its integers' exact values, of the
                # floating-point helpers, and of tables: the back end must make words of each op
                add_test(NAME sail_${m}_integers COMMAND snx-test-sail-${m} --states 16 --ops SQADD_v UQSUB_s SUQADD_v
                         SQABS_v SQRSHL_v UQSHL_s SRSHL_v SQSHLU_vi SQRSHRUN_v SQXTN_v SQDMULH_v SQDMULL_v SQRDMULH_vi
                         SABA_v URHADD_v RADDHN SSHR_v UMAX_v)
                add_test(NAME sail_${m}_floats COMMAND snx-test-sail-${m} --states 16 --ops FRECPE_v FRSQRTS_v FMULX_v
                         FRECPX_s FCVTXN_v FCMGT_v FACGE_s FMAXNMV_s FMAXNMP_s URECPE_v)
                add_test(NAME sail_${m}_tables COMMAND snx-test-sail-${m} --states 16 --ops TBL_TBX CNT_v AESE AESIMC
                         SHA1C SHA256H)
                set_tests_properties(sail_${m}_integers sail_${m}_floats sail_${m}_tables PROPERTIES TIMEOUT 900)
            endif()
            if(gen STREQUAL "ppc64") # an unpatched Unicorn has none: skipped, as it compares nothing
                set_tests_properties(sail_${m} sail_${m}_spill sail_${m}_integers sail_${m}_floats sail_${m}_tables
                                     PROPERTIES SKIP_RETURN_CODE 77)
            endif()
        endforeach()
    endif()
endif()
# the x86-64 back end's, on Windows too: its emitters and its AArch32 blocks against LLVM's
include("${PROJECT_SOURCE_DIR}/cmake/sdm.cmake")
# a host that runs its code for Windows: support/host_win64.c, whose signals are Linux's
if(SNX_X64 AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(snx_host_win64 ON)
endif()
if(SNX_X64)
    add_executable(snx-test-sail-x64 sail_x64.c sail_x64_emit.c llvm_qemu_words.c llvm_steps.c
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_cpu_helpers_flags(snx-test-sail-x64)
    snx_test_target(snx-test-sail-x64 gnu23)
    target_include_directories(snx-test-sail-x64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-sail-x64 PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    snx_decodetree_a64(snx-test-sail-x64)
    snx_sailgen_a64_x64(snx-test-sail-x64)
    add_test(NAME sail_x64 COMMAND snx-test-sail-x64)
    # three registers for values; on Windows four, as a pair's and a shift count's fixed ones are among its first (x64.h)
    set(snx_x64_pool 3)
    set(snx_x64_xmm 6)
    if(WIN32)
        set(snx_x64_pool 4)
        set(snx_x64_xmm 4) # it has six
    endif()
    add_test(NAME sail_x64_spill COMMAND snx-test-sail-x64 --pool ${snx_x64_pool} --states 32)
    add_test(NAME sail_x64_spill_xmm COMMAND snx-test-sail-x64 --pool ${snx_x64_pool} --xmm ${snx_x64_xmm} --states 16)
    set_tests_properties(sail_x64 sail_x64_spill sail_x64_spill_xmm PROPERTIES TIMEOUT 600)
    # its code for Windows (the calling convention), run by this host's processor as Windows' C would call it
    # (support/host_win64.c): the same words against LLVM, as the other machines' under Unicorn
    if(snx_host_win64)
        add_executable(snx-test-sail-win64 sail_mach.c sail_mach_emit.c llvm_qemu_words.c llvm_steps.c
                       qemu_ref/qemu_ref.c support/host_win64.c
                       "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                       "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
        snx_cpu_helpers_flags(snx-test-sail-win64)
        snx_test_target(snx-test-sail-win64 gnu23)
        target_compile_definitions(snx-test-sail-win64 PRIVATE SAIL_MACH_INC="a64_x64.inc" SAIL_MACH_UC=HOSTUC_WIN64
                                                                 SAIL_MACH_WIN64=1)
        target_include_directories(snx-test-sail-win64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(snx-test-sail-win64 PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
        snx_decodetree_a64(snx-test-sail-win64)
        snx_sailgen_a64_x64(snx-test-sail-win64)
        add_test(NAME sail_win64 COMMAND snx-test-sail-win64)
        add_test(NAME sail_win64_spill COMMAND snx-test-sail-win64 --pool 4 --states 32)
        add_test(NAME sail_win64_spill_xmm COMMAND snx-test-sail-win64 --pool 4 --xmm 4 --states 16)
        add_test(NAME sail_win64_plain COMMAND snx-test-sail-win64 --without 0x3d07 --states 16)
        set_tests_properties(sail_win64 sail_win64_spill sail_win64_spill_xmm sail_win64_plain PROPERTIES TIMEOUT 600)
    endif()
    # its A32 blocks (x64_arm.c) against the AArch32 block compiler's LLVM ones: the same words consumed and the
    # same results from random states, the A32 instructions it makes alone and in rows; again with three host
    # registers (the values spill) and without BMI1, BMI2 and LZCNT
    add_executable(snx-test-arm-x64 arm_x64.c)
    snx_test_target(snx-test-arm-x64 gnu23)
    target_link_libraries(snx-test-arm-x64 PRIVATE snx_llvm snx_arm_llvm snx_driver ${CMAKE_DL_LIBS})
    add_test(NAME arm_x64 COMMAND snx-test-arm-x64)
    add_test(NAME arm_x64_spill COMMAND snx-test-arm-x64 --pool ${snx_x64_pool} --blocks 2000 --seed 2)
    add_test(NAME arm_x64_plain COMMAND snx-test-arm-x64 --features 0 --blocks 2000 --seed 3)
    set_tests_properties(arm_x64 arm_x64_spill arm_x64_plain PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    snx_arm_native_modes(x64)
    # and as a Windows host's, run by this host's processor (support/host_win64.c)
    if(snx_host_win64)
        add_executable(snx-test-arm-win64 arm_x64.c support/host_win64.c)
        snx_test_target(snx-test-arm-win64 gnu23)
        target_compile_definitions(snx-test-arm-win64 PRIVATE SNX_TEST_WIN64=1)
        target_include_directories(snx-test-arm-win64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(snx-test-arm-win64 PRIVATE snx_llvm snx_arm_llvm snx_driver ${CMAKE_DL_LIBS})
        add_test(NAME arm_win64 COMMAND snx-test-arm-win64)
        add_test(NAME arm_win64_spill COMMAND snx-test-arm-win64 --pool 4 --blocks 2000 --seed 2)
        add_test(NAME arm_win64_plain COMMAND snx-test-arm-win64 --features 0 --blocks 2000 --seed 3)
        add_test(NAME arm_win64_step COMMAND snx-test-arm-win64 --step --blocks 600 --thumb 600 --states 2 --seed 4)
        add_test(NAME arm_win64_step_bus COMMAND snx-test-arm-win64 --step --profile arm7tdmi --trace --blocks 300
                 --thumb 200 --states 2 --seed 5)
        add_test(NAME arm_win64_step_checked COMMAND snx-test-arm-win64 --step --memory checked --blocks 300 --thumb 200
                 --states 2 --seed 6)
        set_tests_properties(arm_win64 arm_win64_spill arm_win64_plain arm_win64_step arm_win64_step_bus
                             arm_win64_step_checked PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        snx_arm_native_modes(win64 --blocks 600 --states 8)
    endif()
endif()
if(NOT WIN32)
    # the T16 emitters sailgen generates from the Sail against the ones written by hand they replaced
    # (snx_thumb_llvm_block_t16_reference): the same optimized IR halfword by halfword on every profile and memory mode,
    # alone and in an IT block, and the same results run here. Each part takes a sixteenth of its third, spread over
    # every field: the whole (--every 1) takes 45 minutes on 4 threads
    add_executable(snx-test-arm-sail-ir arm_sail_ir.c)
    snx_test_target(snx-test-arm-sail-ir gnu23)
    target_link_libraries(snx-test-arm-sail-ir PRIVATE snx_llvm snx_arm_llvm snx_driver ${CMAKE_DL_LIBS})
    foreach(part 1 2 3)
        add_test(NAME arm_sail_ir_${part} COMMAND snx-test-arm-sail-ir --part ${part}/3 --every 16)
        set_tests_properties(arm_sail_ir_${part} PROPERTIES TIMEOUT 900)
        # and the A32 data processing and branches, against llvm.c's (snx_arm_llvm_block_reference)
        add_test(NAME arm_sail_ir_a32_${part} COMMAND snx-test-arm-sail-ir --a32 --part ${part}/3 --every 16)
        set_tests_properties(arm_sail_ir_a32_${part} PROPERTIES TIMEOUT 900)
    endforeach()
    # snx_ir_object's check of the symbols an object names (llvm_object_symbols.c)
    add_executable(snx-test-llvm-object-symbols llvm_object_symbols.c)
    snx_test_target(snx-test-llvm-object-symbols c23)
    target_link_libraries(snx-test-llvm-object-symbols PRIVATE snx_llvm)
    add_test(NAME llvm_object_symbols COMMAND snx-test-llvm-object-symbols)
    set_tests_properties(llvm_object_symbols PROPERTIES SKIP_RETURN_CODE 77)
    # the LSE atomics' read-modify-write as each target's LLVM lowers it right (llvm_atomic_rmw.c)
    add_executable(snx-test-llvm-atomic-rmw llvm_atomic_rmw.c)
    snx_test_target(snx-test-llvm-atomic-rmw c23)
    target_link_libraries(snx-test-llvm-atomic-rmw PRIVATE snx_llvm)
    snx_decodetree_a64(snx-test-llvm-atomic-rmw)
    add_test(NAME llvm_atomic_rmw COMMAND snx-test-llvm-atomic-rmw)
    set_tests_properties(llvm_atomic_rmw PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(snx-test-llvm-block-fuzz llvm_block_fuzz.c llvm_steps.c llvm_qemu_words.c qemu_ref/qemu_ref.c)
    snx_test_target(snx-test-llvm-block-fuzz gnu23)
    snx_decodetree_a64(snx-test-llvm-block-fuzz)
    target_link_libraries(snx-test-llvm-block-fuzz PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    if(SNX_QEMU_REF)
        add_dependencies(snx-test-llvm-block-fuzz snx_qemu_ref_runner)
        foreach(mode default o0 jit portable jit_o0 mutant exclusive_mutant)
            set(args)
            if(mode STREQUAL o0)
                list(APPEND args --opt=0)
            elseif(mode STREQUAL jit)
                list(APPEND args --jit --hook)
            elseif(mode STREQUAL portable)
                list(APPEND args --portable --volatile)
            elseif(mode STREQUAL jit_o0)
                list(APPEND args --jit --opt=0 --hook --volatile)
            elseif(mode STREQUAL mutant)
                list(APPEND args --self-test)
            elseif(mode STREQUAL exclusive_mutant)
                list(APPEND args --self-test-exclusive)
            endif()
            add_test(NAME llvm_block_fuzz_${mode} COMMAND snx-test-llvm-block-fuzz "${SNX_QEMU_AARCH64}"
                "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/block_fuzz_${mode}" ${args})
            set_tests_properties(llvm_block_fuzz_${mode} PROPERTIES TIMEOUT 180)
        endforeach()
        if(Python3_Interpreter_FOUND)
            add_test(NAME llvm_block_fuzz_seed_matrix COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/llvm_block_fuzz_matrix.py" $<TARGET_FILE:snx-test-llvm-block-fuzz>
                "${SNX_QEMU_AARCH64}" "${SNX_QEMU_REF_RUNNER}" "${CMAKE_CURRENT_BINARY_DIR}/block_fuzz_seed_matrix")
            set_tests_properties(llvm_block_fuzz_seed_matrix PROPERTIES TIMEOUT 600)
        endif()
    else()
        add_test(NAME llvm_block_fuzz COMMAND snx-test-llvm-block-fuzz --skip "${SNX_QEMU_REF_MISSING}")
        set_tests_properties(llvm_block_fuzz PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
# the variant table's hash, which must match jit_service.c's cache hash
add_executable(snx-test-variants variants.c)
snx_test_target(snx-test-variants gnu23)
target_link_libraries(snx-test-variants PRIVATE scarletnx-common)
add_test(NAME variants COMMAND snx-test-variants)
# the AOT build linking a profile cache's unit and emitting snx_variants
add_executable(snx-test-aot-variants aot_variants.c)
snx_test_target(snx-test-aot-variants gnu23)
target_link_libraries(snx-test-aot-variants PRIVATE snx_driver_llvm ${CMAKE_DL_LIBS})
add_test(NAME aot_variants COMMAND snx-test-aot-variants)
set_tests_properties(aot_variants PROPERTIES TIMEOUT 120)
# a pack of objects for iOS made by the arm64 back end, loaded as the app does and run (Unicorn) where it is here
if(NOT WIN32)
    add_executable(snx-test-ios-pack ios_pack.c)
    if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
        target_sources(snx-test-ios-pack PRIVATE support/host_unicorn.c support/la64_interp.c)
        target_include_directories(snx-test-ios-pack PRIVATE "${SNX_UNICORN_INCLUDE_DIR}")
        target_compile_definitions(snx-test-ios-pack PRIVATE SNX_TEST_UNICORN=1)
        target_link_libraries(snx-test-ios-pack PRIVATE "${SNX_UNICORN_LIBRARY}")
    endif()
    snx_test_target(snx-test-ios-pack gnu23)
    target_include_directories(snx-test-ios-pack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-ios-pack PRIVATE snx_driver_llvm snx_driver snx_llvm ${CMAKE_DL_LIBS})
    add_test(NAME ios_pack COMMAND snx-test-ios-pack "${CMAKE_CURRENT_BINARY_DIR}/ios_pack.snxobj")
    set_tests_properties(ios_pack PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
# the x86-64 back end's units held in memory (x64_cache.h) under the module build: reuse, room, the limit
add_executable(snx-test-unit-cache unit_cache.c)
snx_test_target(snx-test-unit-cache gnu23)
target_link_libraries(snx-test-unit-cache PRIVATE snx_driver_llvm ${CMAKE_DL_LIBS})
add_test(NAME unit_cache COMMAND snx-test-unit-cache)
set_tests_properties(unit_cache PROPERTIES TIMEOUT 120)
# a function past the buffer its unit started with, on every machine's back end
if(NOT WIN32)
    add_executable(snx-test-native-grow native_grow.c)
    snx_test_target(snx-test-native-grow gnu23)
    target_link_libraries(snx-test-native-grow PRIVATE scarletnx-recompiler)
    snx_decodetree_a64(snx-test-native-grow)
    add_test(NAME native_grow COMMAND snx-test-native-grow)
    set_tests_properties(native_grow PROPERTIES TIMEOUT 120)
endif()
# a 32-bit game's blocks going on to the next by themselves (nx/loader/module_arm.c): loops, interworking, the poll
add_executable(snx-test-arm-module-chain arm_module_chain.c)
snx_test_target(snx-test-arm-module-chain gnu23)
target_link_libraries(snx-test-arm-module-chain PRIVATE snx_driver_llvm ${CMAKE_DL_LIBS})
add_test(NAME arm_module_chain COMMAND snx-test-arm-module-chain)
set_tests_properties(arm_module_chain PROPERTIES TIMEOUT 120)
# what its recompile says of the blocks the native back end leaves to LLVM
add_executable(snx-test-arm-module-refused arm_module_refused.c)
snx_test_target(snx-test-arm-module-refused gnu23)
target_link_libraries(snx-test-arm-module-refused PRIVATE snx_driver_llvm ${CMAKE_DL_LIBS})
add_test(NAME arm_module_refused COMMAND snx-test-arm-module-refused)
set_tests_properties(arm_module_refused PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
# the AArch32 dispatch loop's lookup keeps no frame and no stack canary: compile(), its slow path, stays out of line
# (arm_dispatch_frame.py reads arm.c's object with the build's llvm-objdump). With ThinLTO the object is bitcode and the
# link makes the machine code, so it reads the program, which the line table ties to arm.c (cmake/lto.cmake)
# snx_core and scarletnx come from src/nx and src/multicall, which the root CMakeLists adds after tests/
if(Python3_Interpreter_FOUND AND SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND SNX_LLVM_CONFIG)
    get_filename_component(snx_llvm_bin "${SNX_LLVM_CONFIG}" DIRECTORY)
    find_program(SNX_LLVM_OBJDUMP llvm-objdump HINTS "${snx_llvm_bin}" NO_DEFAULT_PATH)
    find_program(SNX_LLVM_NM llvm-nm HINTS "${snx_llvm_bin}" NO_DEFAULT_PATH)
    find_program(SNX_LLVM_SYMBOLIZER llvm-symbolizer HINTS "${snx_llvm_bin}" NO_DEFAULT_PATH)
    if(SNX_LLVM_OBJDUMP AND (NOT SNX_LTO_GENEX OR (SNX_LLVM_NM AND SNX_LLVM_SYMBOLIZER)))
        add_test(NAME arm_dispatch_frame COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_dispatch_frame.py"
            --objdump "${SNX_LLVM_OBJDUMP}"
            "$<IF:${SNX_LTO_GENEX},--program=$<TARGET_FILE:scarletnx>,$<TARGET_OBJECTS:snx_core>>"
            "$<${SNX_LTO_GENEX}:--nm=${SNX_LLVM_NM}>" "$<${SNX_LTO_GENEX}:--symbolizer=${SNX_LLVM_SYMBOLIZER}>")
    endif()
endif()
# the runtime looking a pc up in the AOT variant table instead of compiling it
add_executable(snx-test-runtime-variants runtime_variants.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/guest_jit.c")
snx_test_target(snx-test-runtime-variants gnu23)
target_link_libraries(snx-test-runtime-variants PRIVATE scarletnx-common scarletnx-filesystem snx_settings)
# the runtime's linker beside the game's own (runtime/linker.c): nn::init::Start's slot taken until its first call,
# when the game's memory is compared with the copy the runtime linked
add_executable(snx-test-runtime-linker runtime_linker.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/linker.c"
    "${PROJECT_SOURCE_DIR}/src/nx/loader/relocate.c")
snx_test_target(snx-test-runtime-linker gnu23)
target_link_libraries(snx-test-runtime-linker PRIVATE scarletnx-common scarletnx-filesystem snx_settings)
add_test(NAME runtime_linker COMMAND snx-test-runtime-linker)
add_test(NAME runtime_linker_differ COMMAND snx-test-runtime-linker differ)
add_test(NAME runtime_linker_switch COMMAND snx-test-runtime-linker switch)
# an NRO's own AOT module (nro_module_fixture.c, a descriptor and two functions by hand), which guest_jit.c loads for
# the NRO mapped at the address it was made for: runtime_variants copies it under the names the NROs' hashes give
add_library(snx-test-nro-module MODULE nro_module_fixture.c)
set_target_properties(snx-test-nro-module PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_compile_options(snx-test-nro-module PRIVATE -Wall -Wextra -Werror)
target_include_directories(snx-test-nro-module PRIVATE "${PROJECT_SOURCE_DIR}/src")
snx_test_sanitize(snx-test-nro-module) # loaded into the test's process, which is built with them
add_dependencies(snx-test-runtime-variants snx-test-nro-module)
add_test(NAME runtime_variants COMMAND snx-test-runtime-variants --nro-module=$<TARGET_FILE:snx-test-nro-module>)
# a hand-edited jit_opt the JIT does not take, a letter and an empty value
add_test(NAME runtime_variants_jit_opt_letter COMMAND snx-test-runtime-variants --jit-opt=x)
add_test(NAME runtime_variants_jit_opt_empty COMMAND snx-test-runtime-variants --jit-opt=)
add_executable(snx-test-runtime-hints runtime_hints.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/hints.c")
snx_test_target(snx-test-runtime-hints gnu23)
target_link_libraries(snx-test-runtime-hints PRIVATE scarletnx-common scarletnx-filesystem snx_settings Threads::Threads)
add_test(NAME runtime_hints COMMAND snx-test-runtime-hints)
set_tests_properties(runtime_hints PROPERTIES TIMEOUT 15)
# the runtime taking a module of SNX_ABI and refusing any other, on modules that are a descriptor alone (the fixture,
# also built as a 32-bit program's): plain ISO C23, without sanitizers the runtime may lack
foreach(snx_fixture module-abi module-abi-arm)
    add_library(snx-test-${snx_fixture} MODULE module_abi_fixture.c)
    set_target_properties(snx-test-${snx_fixture} PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-${snx_fixture} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    target_include_directories(snx-test-${snx_fixture} PRIVATE "${PROJECT_SOURCE_DIR}/src")
endforeach()
target_compile_definitions(snx-test-module-abi-arm PRIVATE SNX_TEST_MODULE_ARM)
add_executable(snx-test-runtime-module-abi module_abi.c)
snx_test_target(snx-test-runtime-module-abi c23)
target_link_libraries(snx-test-runtime-module-abi PRIVATE snx_test_common)
# what each thread of the runtime pays of its stack for thread-local storage: a table a back end keeps there is paid for
# each back end (the sanitizers' runtime keeps its own there)
add_test(NAME runtime_tls_self COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime_tls.py" --self-test)
if(NOT SNX_TEST_SANITIZERS)
    set(snx_tls_limit 16384)
    if(SNX_BUILD_ANDROID AND NOT WIN32)
        # t_dyn (src/android/bridge/arm32.c), 1024 entries of 16 bytes: to go when that cache leaves the thread-locals
        math(EXPR snx_tls_limit "${snx_tls_limit} + 1024 * 16")
    endif()
    add_test(NAME runtime_tls COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime_tls.py"
             --limit ${snx_tls_limit} $<TARGET_FILE:scarletnx>)
    set_tests_properties(runtime_tls PROPERTIES SKIP_RETURN_CODE 77)
endif()
add_test(NAME runtime_module_abi COMMAND snx-test-runtime-module-abi $<TARGET_FILE:scarletnx>
    $<TARGET_FILE:snx-test-module-abi> $<TARGET_FILE:snx-test-module-abi-arm>)
# Amberlith (edenprobe before 1.0.0; docs/dev/edenprobe.md) as CI runs it, with no FPS limit, against its baseline,
# once with guest buffers copied and once read in place (vk_memory=2: the path a GPU that shares memory takes,
# lavapipe's in CI).
# -DSNX_TEST_EDENPROBE=/path/to/the.nro, of the release that .github/workflows/build.yml pins
if(SNX_TEST_EDENPROBE AND Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    get_filename_component(snx_edenprobe_nro "${SNX_TEST_EDENPROBE}" ABSOLUTE)
    foreach(snx_memory 1 2)
        add_test(NAME edenprobe_vk_memory_${snx_memory}
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/edenprobe.py" run
                --runtime $<TARGET_FILE:scarletnx> --nro "${snx_edenprobe_nro}"
                --mode "quick notime cpu fs video display kernel audio hooks"
                --out "${CMAKE_CURRENT_BINARY_DIR}/edenprobe-vk_memory-${snx_memory}"
                --baseline "${PROJECT_SOURCE_DIR}/tests/edenprobe/linux-x86_64.txt" --timeout 120
                --setting fps-limit=0 --setting vk_memory=${snx_memory})
        set_tests_properties(edenprobe_vk_memory_${snx_memory} PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE LABELS external)
    endforeach()
    # #2442's case, the wimsy suite's heavy level after the reference suite, which it needs: each compute texture read
    # back after a judged frame holds that frame's texels, where a write scan between the texture's move into the alias
    # pool and its trap leaves the frame before's in some of its pages
    add_test(NAME edenprobe_compute_readback
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/edenprobe.py" run
            --runtime $<TARGET_FILE:scarletnx> --nro "${snx_edenprobe_nro}" --mode "quick hooks game level=3 frames=8"
            --out "${CMAKE_CURRENT_BINARY_DIR}/edenprobe-compute-readback"
            --baseline "${PROJECT_SOURCE_DIR}/tests/edenprobe/compute-readback.txt" --timeout 600
            --setting fps-limit=0)
    set_tests_properties(edenprobe_compute_readback PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE LABELS external)
endif()
# the program's malloc is the pinned jemalloc's (SNX_JEMALLOC). On macOS its prefix, je_, names the options' variable
if(SNX_JEMALLOC AND NOT CMAKE_CROSSCOMPILING)
    if(APPLE)
        set(snx_jemalloc_conf JE_MALLOC_CONF)
        set(snx_jemalloc_nm "")
        set(snx_jemalloc_preload "")
        # inserted into the program, it names the zone a new allocation comes from, which must be jemalloc's. A dylib:
        # dyld inserts no bundle
        add_library(snx-test-jemalloc-zone SHARED jemalloc_zone.c)
        set(snx_jemalloc_zone "$<TARGET_FILE:snx-test-jemalloc-zone>")
        # a program with the system's malloc, for which the library must name another zone
        add_executable(snx-test-jemalloc-zone-control jemalloc_zone_control.c)
        set(snx_jemalloc_zone_control "$<TARGET_FILE:snx-test-jemalloc-zone-control>")
    else()
        set(snx_jemalloc_conf MALLOC_CONF)
        set(snx_jemalloc_nm "${CMAKE_NM}")
        # a preloaded pthread_cond_init that allocates, as the AppImage's cross-libc-dlopen.so
        add_library(snx-test-jemalloc-preload MODULE jemalloc_preload.c)
        target_link_libraries(snx-test-jemalloc-preload PRIVATE ${CMAKE_DL_LIBS})
        set(snx_jemalloc_preload "$<TARGET_FILE:snx-test-jemalloc-preload>")
        set(snx_jemalloc_zone "")
        set(snx_jemalloc_zone_control "")
    endif()
    add_test(NAME jemalloc COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:scarletnx>"
        "-DVERSION=${snx_jemalloc_version}" "-DCONF=${snx_jemalloc_conf}" "-DNM=${snx_jemalloc_nm}"
        "-DPRELOAD=${snx_jemalloc_preload}" "-DZONE=${snx_jemalloc_zone}"
        "-DZONE_CONTROL=${snx_jemalloc_zone_control}" -P "${CMAKE_CURRENT_SOURCE_DIR}/jemalloc.cmake")
elseif(SNX_JEMALLOC AND ANDROID)
    # Android's program, the driver's executable (src/nx), runs on a device only: what it exports is read here
    list(JOIN SNX_JEMALLOC_EXPORTS "," snx_jemalloc_names)
    add_test(NAME jemalloc COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:scarletnx-run>" "-DNM=${CMAKE_NM}"
        "-DNAMES=${snx_jemalloc_names}" -P "${CMAKE_CURRENT_SOURCE_DIR}/jemalloc.cmake")
endif()
# the module contract's layout as recorded when SNX_ABI last rose (docs/dev/architecture.md, Module ABI)
add_executable(snx-test-module-contract module_contract.c)
snx_test_target(snx-test-module-contract c23)
add_test(NAME module_contract COMMAND snx-test-module-contract)
# the build-side reader of the runtime JIT's cache
add_executable(snx-test-jit-cache jit_cache.c)
snx_test_target(snx-test-jit-cache gnu23)
target_link_libraries(snx-test-jit-cache PRIVATE snx_llvm)
add_test(NAME jit_cache COMMAND snx-test-jit-cache)
# LLVM's SLP vectorizer turned off for a process (the setting slp_vectorizer), and the JIT cache's key saying so
add_executable(snx-test-slp-vectorizer slp_vectorizer.c)
snx_test_target(snx-test-slp-vectorizer gnu23)
target_link_libraries(snx-test-slp-vectorizer PRIVATE snx_llvm)
add_test(NAME slp_vectorizer COMMAND snx-test-slp-vectorizer)
# the launcher's AOT pack action: the cache's path and header, and the optimization the AOT build must share
add_executable(snx-test-aot-pack aot_pack.c "${PROJECT_SOURCE_DIR}/src/gui/aot.c")
snx_test_target(snx-test-aot-pack gnu23)
target_link_libraries(snx-test-aot-pack PRIVATE scarletnx-common)
add_test(NAME aot_pack COMMAND snx-test-aot-pack)
# the runtime JIT (jit_service.c): misses compiled, published and shared between threads, on that loader
add_executable(snx-test-jit-service jit_service.c)
snx_test_target(snx-test-jit-service gnu23)
target_link_libraries(snx-test-jit-service PRIVATE snx_llvm Threads::Threads)
add_test(NAME jit_service COMMAND snx-test-jit-service)
set_tests_properties(jit_service PROPERTIES TIMEOUT 120)
# Offline replay of captured executable images. It compiles code but never executes guest instructions.
add_executable(snx-bench-jit-latency jit_latency.c)
snx_test_target(snx-bench-jit-latency gnu23)
target_link_libraries(snx-bench-jit-latency PRIVATE snx_llvm Threads::Threads)
# longjmp across the loader's code, which on Windows unwinds through its registered function table
# what the loader says where the system refuses its memory, on any host
add_executable(snx-test-code-loader-refused code_loader_refused.c)
snx_test_target(snx-test-code-loader-refused gnu23)
target_link_libraries(snx-test-code-loader-refused PRIVATE snx_llvm)
add_test(NAME code_loader_refused COMMAND snx-test-code-loader-refused)
add_executable(snx-test-code-loader-unwind code_loader_unwind.c)
snx_test_target(snx-test-code-loader-unwind gnu23)
target_link_libraries(snx-test-code-loader-unwind PRIVATE snx_llvm)
add_test(NAME code_loader_unwind COMMAND snx-test-code-loader-unwind)
# a generated function's frame of a page or more probes the stack on Windows x86-64 (rtlib.c's snx_stack_probe)
add_executable(snx-test-stack-probe stack_probe.c)
snx_test_target(snx-test-stack-probe gnu23)
target_link_libraries(snx-test-stack-probe PRIVATE snx_llvm)
add_test(NAME stack_probe COMMAND snx-test-stack-probe)
set_tests_properties(stack_probe PROPERTIES SKIP_RETURN_CODE 77)
# the loader's objects as GDB's JIT interface lists them (gdb_jit.c); skipped on Windows, which registers none
add_executable(snx-test-gdb-jit gdb_jit.c)
snx_test_target(snx-test-gdb-jit gnu23)
target_link_libraries(snx-test-gdb-jit PRIVATE snx_llvm)
add_test(NAME gdb_jit COMMAND snx-test-gdb-jit)
set_tests_properties(gdb_jit PROPERTIES SKIP_RETURN_CODE 77)
if(NOT WIN32)
    # the relocations both ELF paths share (elf_object.c), AArch64's included, checked on any ELF host and on macOS,
    # whose JIT loads ELF objects
    add_executable(snx-test-elf-relocate elf_relocate.c)
    snx_test_target(snx-test-elf-relocate gnu23)
    target_link_libraries(snx-test-elf-relocate PRIVATE snx_llvm)
    add_test(NAME elf_relocate COMMAND snx-test-elf-relocate)
    set_tests_properties(elf_relocate PROPERTIES TIMEOUT 120)
endif()
# Windows on Arm's COFF objects (code_loader_coff.c, link_coff.c): relocations, veneers, a module and a JIT load, read
# back on any host
add_executable(snx-test-coff-arm64 coff_arm64.c)
snx_test_target(snx-test-coff-arm64 gnu23)
target_link_libraries(snx-test-coff-arm64 PRIVATE snx_llvm)
add_test(NAME coff_arm64 COMMAND snx-test-coff-arm64)
set_tests_properties(coff_arm64 PROPERTIES TIMEOUT 120)
# not a test: timings of the generated code (llvm_bench SCARLETNX REPO CLANG)
add_executable(snx-llvm-bench llvm_bench.c)
snx_test_target(snx-llvm-bench gnu23)
target_link_libraries(snx-llvm-bench PRIVATE snx_driver ${CMAKE_DL_LIBS})
# not a test: a game's functions recompiled on their own (llvm_pieces DUMP ADDR...), for the recompile's time and output
add_executable(snx-llvm-pieces llvm_pieces.c)
snx_test_target(snx-llvm-pieces gnu23)
target_link_libraries(snx-llvm-pieces PRIVATE scarletnx-recompiler)
# the backend's own switches (ir.h), in the library's objects: its headers, not its target, whose objects the library
# holds already
target_include_directories(snx-llvm-pieces PRIVATE "$<TARGET_PROPERTY:snx_llvm,INTERFACE_INCLUDE_DIRECTORIES>")
target_include_directories(snx-llvm-pieces SYSTEM PRIVATE "${SNX_LLVM_INCLUDE_DIR}")
include(arm_blocks.cmake)

# LLVM Internals: passes cut out of LLVM's pipelines (nx/runtime/driver/llvm_tuning.c), which LLVM still runs
add_executable(snx-test-llvm-tuning llvm_tuning.c)
snx_test_target(snx-test-llvm-tuning gnu23)
target_link_libraries(snx-test-llvm-tuning PRIVATE snx_driver)
add_test(NAME llvm_tuning COMMAND snx-test-llvm-tuning)
# the offline branch profile (ir_profile.c, the setting ir_branch_profile): weights where address and word match, and
# what it cannot read said
add_executable(snx-test-llvm-branch-profile llvm_branch_profile.c)
snx_test_target(snx-test-llvm-branch-profile gnu23)
target_link_libraries(snx-test-llvm-branch-profile PRIVATE snx_driver)
add_test(NAME llvm_branch_profile COMMAND snx-test-llvm-branch-profile "${CMAKE_CURRENT_BINARY_DIR}")
# the X registers' cache within a block (tools/sailgen/llvm.h, ir.h's accessors by ir_sail.c)
add_executable(snx-test-llvm-gpr-cache llvm_gpr_cache.c)
snx_test_target(snx-test-llvm-gpr-cache gnu23)
target_link_libraries(snx-test-llvm-gpr-cache PRIVATE snx_llvm snx_driver)
add_test(NAME llvm_gpr_cache COMMAND snx-test-llvm-gpr-cache)
# the flags' cache within a block, and the compare their origin gives a condition as (tools/sailgen/llvm.h)
add_executable(snx-test-llvm-flags-cache llvm_flags_cache.c)
snx_test_target(snx-test-llvm-flags-cache gnu23)
target_link_libraries(snx-test-llvm-flags-cache PRIVATE snx_llvm snx_driver)
add_test(NAME llvm_flags_cache COMMAND snx-test-llvm-flags-cache)
add_executable(snx-test-llvm-fp-store llvm_fp_store.c llvm_steps.c)
snx_test_target(snx-test-llvm-fp-store gnu23)
target_link_libraries(snx-test-llvm-fp-store PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_fp_store COMMAND snx-test-llvm-fp-store)
add_executable(snx-test-llvm-module llvm_module.c llvm_steps.c)
snx_test_target(snx-test-llvm-module gnu23)
target_link_libraries(snx-test-llvm-module PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
# the NROs of a game's RomFS made ahead of time: the game module's table of where they go, and each one's own module
add_executable(snx-test-nro-modules nro_modules.c)
snx_test_target(snx-test-nro-modules gnu23)
target_link_libraries(snx-test-nro-modules PRIVATE snx_llvm snx_driver OpenSSL::Crypto ${CMAKE_DL_LIBS})
add_test(NAME nro_modules COMMAND snx-test-nro-modules $<TARGET_FILE:scarletnx> "${CMAKE_CURRENT_BINARY_DIR}/nro-modules-data")
# a game module's calls through its PLT stubs: direct once the game's linker has bound the slot to the one function
# that defines the stub's symbol, through the dispatch table otherwise
add_executable(snx-test-plt-calls plt_calls.c)
snx_test_target(snx-test-plt-calls gnu23)
target_link_libraries(snx-test-plt-calls PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME plt_calls COMMAND snx-test-plt-calls $<TARGET_FILE:scarletnx> "${CMAKE_CURRENT_BINARY_DIR}/plt-calls-data")
add_test(NAME llvm_module COMMAND snx-test-llvm-module $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}")
add_test(NAME llvm_module_stats COMMAND snx-test-llvm-module $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}" --stats)
# what the host's native back end makes of whole programs (recompile_census.c): a fixed one of each kind the drivers
# recompile, every function or block of which must be the back end's. Each prints its count, which the CI's jobs show
# (scripts/recompile-census.py), by the label
add_executable(snx-test-recompile-census recompile_census.c llvm_qemu_words.c)
snx_test_target(snx-test-recompile-census c23)
target_link_libraries(snx-test-recompile-census PRIVATE scarletnx-recompiler snx_arm_decode scarletnx-common
                      OpenSSL::Crypto)
snx_decodetree_a64(snx-test-recompile-census)
# AArch32's words: the patterns of its decoders' .decode files
foreach(isa a32 a32-uncond t32 t16)
    string(REPLACE "-" "_" snx_rows "rows_${isa}")
    set(snx_width 32)
    if(isa STREQUAL "t16")
        set(snx_width 16)
    endif()
    snx_decodetree(snx-test-recompile-census OUTPUT rows-${isa}.inc
                   DECODE "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/decode/${isa}.decode"
                   OPTIONS --rows=${snx_rows} --insnwidth=${snx_width})
endforeach()
set(snx_census switch ounce nro switch32)
if(TARGET snx_ctr_board)
    list(APPEND snx_census 3ds)
endif()
foreach(program IN LISTS snx_census)
    add_test(NAME recompile_census_${program} COMMAND snx-test-recompile-census ${program} $<TARGET_FILE:scarletnx>
             "${SNX_CTR_PROGRAM}")
    set_tests_properties(recompile_census_${program} PROPERTIES SKIP_RETURN_CODE 77 LABELS census)
    # under a second here, minutes under a cross build's emulator
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        set_tests_properties(recompile_census_${program} PROPERTIES TIMEOUT 1800)
    else()
        set_tests_properties(recompile_census_${program} PROPERTIES TIMEOUT 600)
    endif()
endforeach()
# the recompiler as a library (snx_recompiler.h): A64 on every backend built, A32, from code in memory
add_executable(snx-test-recompiler-library recompiler_library.c)
snx_test_target(snx-test-recompiler-library gnu23)
target_link_libraries(snx-test-recompiler-library PRIVATE scarletnx-recompiler)
add_test(NAME recompiler_library COMMAND snx-test-recompiler-library)
# the same run with an inaccessible page kept below every code arena (arena_guard_below.c, preloaded): a call that
# reads the bytes before an arena's first function faults there in every run, where it faulted only in the runs whose
# system had mapped nothing below the arena (the function sanitizer's check of an indirect call, SNX_TEST_SANITIZERS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(snx-test-arena-guard-below MODULE arena_guard_below.c)
    set_target_properties(snx-test-arena-guard-below PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(snx-test-arena-guard-below PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(snx-test-arena-guard-below PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME recompiler_library_arena_edge COMMAND snx-test-recompiler-library)
    set_tests_properties(recompiler_library_arena_edge PROPERTIES
        ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:snx-test-arena-guard-below>")
endif()
# the same header from C++, as the embedders include it
add_executable(snx-test-recompiler-library-cpp recompiler_library_cpp.cpp)
set_target_properties(snx-test-recompiler-library-cpp PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_options(snx-test-recompiler-library-cpp PRIVATE -Wall -Wextra -Werror)
target_link_libraries(snx-test-recompiler-library-cpp PRIVATE scarletnx-recompiler Threads::Threads)
snx_test_sanitize(snx-test-recompiler-library-cpp) # this target is not made through snx_test_target
add_test(NAME recompiler_library_cpp COMMAND snx-test-recompiler-library-cpp)
# the host's native back end (x86-64's, arm64's...) against LLVM, function by function (recompiler_backends.c). The
# features of the host it goes without in plain runs (--without): x86-64's BMI, AVX, FMA, F16C, AES and SHA, arm64's all
set(snx_plain_features 0xd07)
set(snx_plain_ops_features 0x3d07)
set(snx_plain_fma_features 0x800)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(snx_plain_features 0x7ffff)
    set(snx_plain_ops_features 0x7ffff)
    set(snx_plain_fma_features 0x7ffff)
endif()
add_executable(snx-test-recompiler-backends recompiler_backends.c llvm_qemu_words.c qemu_ref/qemu_ref.c)
snx_test_target(snx-test-recompiler-backends gnu23)
target_include_directories(snx-test-recompiler-backends PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(snx-test-recompiler-backends PRIVATE scarletnx-recompiler scarletnx-common)
snx_decodetree_a64(snx-test-recompiler-backends)
add_test(NAME recompiler_backends COMMAND snx-test-recompiler-backends)
# the back ends' Advanced SIMD (x64_vec.c, arm64_vec.c), a family at a time: each word of these ops (the decoder's samples
# of them) made by the host's back end, a function of its own, and its results LLVM's from states of the lanes' edge
# values
foreach(family IN ITEMS
        "pairs;FADDP_v;FADDP_s;FMAXP_v;FMINP_v;FMAXNMP_v;FMINNMP_v;FMAXP_s;FMINP_s;FMAXNMP_s;FMINNMP_s;FMAXV_s;FMINV_s;FMAXNMV_s;FMINNMV_s"
        "elements;FMUL_si;FMLA_si;FMLS_si"
        "estimates;FRECPE_v;FRECPE_s;FRSQRTE_v;FRSQRTE_s;FRECPS_v;FRECPS_s;FRSQRTS_v;FRSQRTS_s"
        "rounds;FRINTN_v;FRINTP_v;FRINTM_v;FRINTZ_v;FRINTA_v"
        "bits;NOT_v;EXT_d;CLZ_v;CLS_v;SRI_v;SRI_s;SLI_v;SLI_s"
        "sums;UADDLV;SADDLV;UADDLP_v;SADDLP_v;UADALP_v;SADALP_v;UADDL_v;SADDL_v;USUBL_v;SSUBL_v;USUBW;SSUBW;UHSUB_v;SHSUB_v"
        "shifts;SRSHR_v;URSHR_v;SRSRA_v;URSRA_v;SRSHR_s;URSHR_s;SRSRA_s;URSRA_s;RSHRN_v"
        "differences;UABD_v;SABD_v;UABA_v;SABA_v;UABDL_v;SABDL_v;UABAL_v;SABAL_v"
        "doublewords;ADD_s;SUB_s;CMEQ_s;CMGT_s;CMGE_s;CMHI_s;CMHS_s;CMTST_s"
        "saturating;SQADD_s;UQADD_s;SQSUB_s;UQSUB_s;SQXTN_s;UQXTN_s;SQXTUN_s;SQSHRN_si;UQSHRN_si;SQSHRUN_si;SQRSHRN_v;UQRSHRN_v;SQRSHRUN_v;SQRSHRN_si;UQRSHRN_si;SQRSHRUN_si"
        "saturating_sums;SQADD_v;UQADD_v;SQSUB_v;UQSUB_v;SUQADD_v;USQADD_v;SUQADD_s;USQADD_s;SQABS_v;SQNEG_v;SQABS_s;SQNEG_s"
        "byte_shifts;SHL_v;USHR_v;SSHR_v;USRA_v;SSRA_v;SRSHR_v;URSHR_v;SRSRA_v;URSRA_v;SRI_v;SLI_v;RBIT_v"
        "reversals;RBIT;REV16;REV32;REV64"
        "shifts_by_register;SSHL_v;USHL_v;SRSHL_v;URSHL_v;SSHL_s;USHL_s;SRSHL_s;URSHL_s;SQSHL_v;UQSHL_v;SQRSHL_v;UQRSHL_v;SQSHL_s;UQSHL_s;SQRSHL_s;UQRSHL_s"
        "saturating_shifts;SQSHL_vi;UQSHL_vi;SQSHLU_vi;SQSHL_si;UQSHL_si;SQSHLU_si"
        "doubling;SQDMULH_v;SQRDMULH_v;SQDMULH_vi;SQRDMULH_vi;SQDMULH_s;SQRDMULH_s;SQDMULH_si;SQRDMULH_si;SQDMULL_v;SQDMLAL_v;SQDMLSL_v;SQDMULL_vi;SQDMLAL_vi;SQDMLSL_vi;SQDMULL_si;SQDMLAL_si;SQDMLSL_si;SRHADD_v;RADDHN;RSUBHN"
        "crypto;AESE;AESD;AESMC;AESIMC;SHA1C;SHA1P;SHA1M;SHA1SU0;SHA1H;SHA1SU1;SHA256H;SHA256H2;SHA256SU0;SHA256SU1;PMULL_p64")
    list(POP_FRONT family name)
    add_test(NAME recompiler_backends_${name} COMMAND snx-test-recompiler-backends --states 48 --ops ${family})
    set_tests_properties(recompiler_backends_${name} PROPERTIES TIMEOUT 600)
endforeach()
# half precision (an Android phone's): the ops whose words x64_vec.c makes of singles and doubles
add_test(NAME recompiler_backends_half COMMAND snx-test-recompiler-backends --target android --states 48 --ops FADD_v FSUB_v
         FMUL_v FDIV_v FABD_v FABD_s FMAX_v FMIN_v FMAXNM_v FMINNM_v FMLA_v FMLS_v FMUL_vi FMLA_vi FMLS_vi FNEG_v FABS_v
         FSQRT_v FCMEQ_v FCMGE_v FCMGT_v FCMEQ_s FCMGE_s FCMGT_s FACGE_v FACGT_v FCMEQ0_v FCMGE0_v FCMGT0_v FCMLE0_v
         FCMLT0_v FCVTZS_vi FCVTZU_vi SCVTF_vi UCVTF_vi FCVTZS_vf FCVTZU_vf SCVTF_vf UCVTF_vf SCVTF_f UCVTF_f FCVTZS_f
         FCVTZU_f FADDP_v FADDP_s FMAXP_v FMINP_v FMAXNMP_v FMINNMP_v FMAXP_s FMINP_s FMAXNMP_s FMINNMP_s FMAXV_h FMINV_h
         FMAXNMV_h FMINNMV_h FMUL_si FMLA_si FMLS_si FRECPE_v FRECPE_s FRSQRTE_v FRSQRTE_s FRECPS_v FRECPS_s FRSQRTS_v
         FRSQRTS_s FRINTN_v FRINTP_v FRINTM_v FRINTZ_v FRINTA_v)
set_tests_properties(recompiler_backends_half PROPERTIES TIMEOUT 900)
# the same without the host's optional features (FMA, AES-NI and SHA-NI, or all of arm64's), where the helpers do their work
add_test(NAME recompiler_backends_plain_ops COMMAND snx-test-recompiler-backends --without ${snx_plain_ops_features}
         --states 48 --ops
         FRECPS_v FRECPS_s FRSQRTS_v FRSQRTS_s FMLA_si FMLS_si AESE AESD AESMC AESIMC SHA256H SHA256H2 SHA256SU0
         SHA256SU1)
set_tests_properties(recompiler_backends_plain_ops PROPERTIES TIMEOUT 600)
# the lanes' shifts and the saturating sums without GFNI (0x10000) and AVX2 (0x8000)
add_test(NAME recompiler_backends_plain_lanes COMMAND snx-test-recompiler-backends --without 0x18000 --states 32 --ops
         SHL_v USHR_v SSHR_v SRSHR_v URSRA_v SRI_v SLI_v RBIT_v RBIT SSHL_v USHL_v SRSHL_v URSHL_v SQSHL_v UQSHL_v
         SQRSHL_v UQRSHL_v SQSHL_s UQRSHL_s)
set_tests_properties(recompiler_backends_plain_lanes PROPERTIES TIMEOUT 600)
# half-precision words (an Android phone's): x64_vec.c makes singles and doubles of no half's lanes
set(snx_half_words 0e421420 4ec31462 6e441c83 2e453ca4 0e460cc5)
add_test(NAME recompiler_backends_half_words COMMAND snx-test-recompiler-backends --target android --states 40 --words
         ${snx_half_words})
# a loop that keeps its vector registers in host ones, left by a BR (its target in the link register): the registers
# stored on the way to the dispatch
set(snx_br_pins d2800070 1e210800 1f010802 1e232843 1e653884 1e61c086 1e274867 1e2020e0 9a83b041 f1000610 54fffee1
    d61f03c0)
add_test(NAME recompiler_backends_br_pins COMMAND snx-test-recompiler-backends --words ${snx_br_pins})
# X17 written in a loop, kept in a host register, and then an undefined word, whose runtime function changes it in the
# state: the exit past it stores none
set(snx_undef_pins d2800050 91000631 f1000610 54ffffc1 00000000)
add_test(NAME recompiler_backends_undef_pins COMMAND snx-test-recompiler-backends --words ${snx_undef_pins})
# X30 kept in a host register, in step with the state (an SVC), and then stripped by XPACLRI: what the exit stores
add_test(NAME recompiler_backends_xpac COMMAND snx-test-recompiler-backends --target max --words aa0003fe d4000001
         d2800050 8b1e03c2 f1000610 54ffffc1 d50320ff d2800005 d65f00a0)
# functions that call each other, with loops, SVCs, polls and switches: what a call, an exit and an entry keep of the
# registers kept in host ones
add_test(NAME recompiler_backends_calls COMMAND snx-test-recompiler-backends --calls --functions 120 --states 8)
add_test(NAME recompiler_backends_calls_2 COMMAND snx-test-recompiler-backends --calls --functions 120 --states 8 --seed 2)
add_test(NAME recompiler_backends_calls_3 COMMAND snx-test-recompiler-backends --calls --functions 1000 --states 8 --seed 4)
set_tests_properties(recompiler_backends_calls recompiler_backends_calls_2 recompiler_backends_calls_3 PROPERTIES
                     TIMEOUT 600)
# words of the lanes' arithmetic, each a function of its own that the back end must make, from the lanes' edge values
add_test(NAME recompiler_backends_lanes COMMAND snx-test-recompiler-backends --states 48 --ops ADD_v SUB_v MUL_v MLA_v
         FADD_v FMUL_v FMLA_v FMAX_v FMINNM_v FCMGE_v SQADD_v UQXTN_v SQSHL_v ADDV ZIP1 TBL_TBX)
set_tests_properties(recompiler_backends_lanes PROPERTIES TIMEOUT 600)
# what the native back ends say of an instruction they have no code for, at the recompile and where it runs, on every
# machine's (native_refused.c)
if(NOT WIN32)
    add_executable(snx-test-native-refused native_refused.c)
    snx_test_target(snx-test-native-refused gnu23)
    target_link_libraries(snx-test-native-refused PRIVATE scarletnx-recompiler)
    snx_decodetree_a64(snx-test-native-refused)
    add_test(NAME native_refused COMMAND snx-test-native-refused)
    set_tests_properties(native_refused PROPERTIES TIMEOUT 300)
endif()
# the exclusives whatever the fields they leave unused hold
add_test(NAME recompiler_backends_exclusives COMMAND snx-test-recompiler-backends --refused --ops LDXR LDXP STXR)
# each of the Switch's ops, of an Android phone's (half precision, the dot products, the rounding doubling
# multiplications), of the Ounce's and of all the decoder knows is the native back end's: none of their words is left to
# LLVM
foreach(target switch android ounce max)
    add_test(NAME recompiler_backends_refused_${target} COMMAND snx-test-recompiler-backends --refused --target ${target})
    set_tests_properties(recompiler_backends_refused_${target} PROPERTIES TIMEOUT 600)
endforeach()
# the native back ends other than the host's against LLVM, function by function, their code under Unicorn on any
# host (where installed)
if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
    foreach(m arm64 rv64 ppc64 mips64 la64 ppc64be mips64be s390x)
        string(TOUPPER "${m}" upper)
        add_executable(snx-test-recompiler-${m} recompiler_backends.c llvm_qemu_words.c qemu_ref/qemu_ref.c
                       support/host_unicorn.c support/la64_interp.c
                       "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                       "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
        snx_cpu_helpers_flags(snx-test-recompiler-${m})
        snx_test_target(snx-test-recompiler-${m} gnu23)
        target_compile_definitions(snx-test-recompiler-${m} PRIVATE SNX_TEST_${upper}=1)
        target_include_directories(snx-test-recompiler-${m} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${SNX_UNICORN_INCLUDE_DIR}")
        target_link_libraries(snx-test-recompiler-${m} PRIVATE scarletnx-recompiler snx_llvm scarletnx-common
                              "${SNX_UNICORN_LIBRARY}")
        snx_decodetree_a64(snx-test-recompiler-${m})
        add_test(NAME recompiler_${m} COMMAND snx-test-recompiler-${m})
        # a JIT unit's: its literals hold addresses on MIPS64
        add_test(NAME recompiler_${m}_jit COMMAND snx-test-recompiler-${m} --jit --made --states 40 --words 1e622820 1e621841
                 9ac10c00 d65f03c0)
        add_test(NAME recompiler_${m}_br_pins COMMAND snx-test-recompiler-${m} --words ${snx_br_pins})
        add_test(NAME recompiler_${m}_undef_pins COMMAND snx-test-recompiler-${m} --words ${snx_undef_pins})
        add_test(NAME recompiler_${m}_calls COMMAND snx-test-recompiler-${m} --calls --functions 40 --states 4)
        set_tests_properties(recompiler_${m}_calls PROPERTIES TIMEOUT 600)
        add_test(NAME recompiler_${m}_half_words COMMAND snx-test-recompiler-${m} --target android --states 40 --words
                 ${snx_half_words})
        # MRS and MSR of EL0's registers, the cache maintenance, DC ZVA
        add_test(NAME recompiler_${m}_system COMMAND snx-test-recompiler-${m} --ops SYS)
        set_tests_properties(recompiler_${m}_system PROPERTIES TIMEOUT 600)
        # no Unicorn of the machine (an unpatched one's PPC64): skipped, as it compares nothing
        set_tests_properties(recompiler_${m} recompiler_${m}_jit recompiler_${m}_br_pins recompiler_${m}_undef_pins
                             recompiler_${m}_calls recompiler_${m}_half_words recompiler_${m}_system PROPERTIES
                             SKIP_RETURN_CODE 77)
        # the exclusives whatever the fields they leave unused hold
        add_test(NAME recompiler_${m}_exclusives COMMAND snx-test-recompiler-${m} --refused --ops LDXR LDXP STXR)
        foreach(target switch android ounce max)
            add_test(NAME recompiler_${m}_refused_${target} COMMAND snx-test-recompiler-${m} --refused --target ${target})
            set_tests_properties(recompiler_${m}_refused_${target} PROPERTIES TIMEOUT 600)
        endforeach()
    endforeach()
    # an arm64 host with no optional feature (SNX_ARM64_F_ALL): every word made, and the results of the random functions'
    # and of CRC32's
    add_test(NAME recompiler_arm64_refused_plain COMMAND snx-test-recompiler-arm64 --refused --without 0x7ffff
             --target max)
    add_test(NAME recompiler_arm64_plain COMMAND snx-test-recompiler-arm64 --without 0x7ffff)
    add_test(NAME recompiler_arm64_plain_crc32 COMMAND snx-test-recompiler-arm64 --without 0x7ffff --made --states 40
             --words 1ac14000 1ac24400 1ac34800 9ac44c00 1ac55000 1ac65400 1ac75800 9ac85c00 1ac14be9 9adf5c0a)
    set_tests_properties(recompiler_arm64_refused_plain recompiler_arm64_plain recompiler_arm64_plain_crc32
                         PROPERTIES TIMEOUT 600)
    # a function longer than a machine's short jumps reach: B's 128 KiB on MIPS64, JAL's 1 MiB on riscv64, B.cond's and
    # LDR (literal)'s 1 MiB on arm64 (words an optional feature would make short); in an AOT module and in a JIT unit
    foreach(long IN ITEMS "mips64;0;400;131072;4ee20c20;1e622820" "mips64be;0;400;131072;4ee20c20;1e622820"
            "rv64;0;3000;1048576;4ee20c20;1e622820" "arm64;0x7ffff;1200;1048576;4e056000;5e024020")
        list(POP_FRONT long m without times bytes)
        add_test(NAME recompiler_${m}_long COMMAND snx-test-recompiler-${m} --without ${without} --states 8
                 --long ${times} ${bytes} --words ${long})
        add_test(NAME recompiler_${m}_long_jit COMMAND snx-test-recompiler-${m} --without ${without} --states 8 --jit
                 --long ${times} ${bytes} --words ${long})
        set_tests_properties(recompiler_${m}_long recompiler_${m}_long_jit PROPERTIES TIMEOUT 600)
    endforeach()
    # arm64's functions as a Windows host's: COFF objects, loaded by the JIT's own loader, their frames unwound by the
    # function table where they call out, with --step at every instruction; one of more than a record's 1 MiB
    add_executable(snx-test-recompiler-arm64win recompiler_backends.c llvm_qemu_words.c qemu_ref/qemu_ref.c
                   support/host_unicorn.c support/la64_interp.c
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_cpu_helpers_flags(snx-test-recompiler-arm64win)
    snx_test_target(snx-test-recompiler-arm64win gnu23)
    target_compile_definitions(snx-test-recompiler-arm64win PRIVATE SNX_TEST_ARM64=1 SNX_TEST_ARM64WIN=1)
    target_include_directories(snx-test-recompiler-arm64win PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${SNX_UNICORN_INCLUDE_DIR}")
    target_link_libraries(snx-test-recompiler-arm64win PRIVATE scarletnx-recompiler snx_llvm scarletnx-common
                          "${SNX_UNICORN_LIBRARY}")
    snx_decodetree_a64(snx-test-recompiler-arm64win)
    add_test(NAME recompiler_arm64win COMMAND snx-test-recompiler-arm64win)
    add_test(NAME recompiler_arm64win_step COMMAND snx-test-recompiler-arm64win --step --functions 60 --states 2)
    add_test(NAME recompiler_arm64win_jit COMMAND snx-test-recompiler-arm64win --jit --functions 200 --states 8 --seed 2)
    add_test(NAME recompiler_arm64win_hook COMMAND snx-test-recompiler-arm64win --hook --functions 200 --states 8 --seed 3)
    add_test(NAME recompiler_arm64win_plain COMMAND snx-test-recompiler-arm64win --without 0x7ffff --functions 200
             --states 8 --seed 4)
    add_test(NAME recompiler_arm64win_long COMMAND snx-test-recompiler-arm64win --without 0x7ffff --states 8
             --long 1200 1048576 --words 4e056000 5e024020)
    set_tests_properties(recompiler_arm64win recompiler_arm64win_step recompiler_arm64win_jit recompiler_arm64win_hook
                         recompiler_arm64win_plain recompiler_arm64win_long PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    # their A32 blocks (arm64_arm.c, rv64_arm.c) against LLVM's, as arm_x64's, under Unicorn; again with three host
    # registers
    foreach(m arm64 rv64 ppc64 mips64 la64 ppc64be mips64be s390x)
        string(TOUPPER "${m}" upper)
        add_executable(snx-test-arm-${m} arm_x64.c support/host_unicorn.c support/la64_interp.c)
        snx_test_target(snx-test-arm-${m} gnu23)
        target_compile_definitions(snx-test-arm-${m} PRIVATE SNX_TEST_${upper}=1)
        target_include_directories(snx-test-arm-${m} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${SNX_UNICORN_INCLUDE_DIR}")
        target_link_libraries(snx-test-arm-${m} PRIVATE snx_llvm snx_arm_llvm snx_driver "${SNX_UNICORN_LIBRARY}"
                              ${CMAKE_DL_LIBS})
        add_test(NAME arm_${m} COMMAND snx-test-arm-${m})
        add_test(NAME arm_${m}_spill COMMAND snx-test-arm-${m} --pool 3 --blocks 2000 --seed 2)
        # VCVTB and VCVTT, from and to a single and a double, the half in the top of an S register or at its start: a
        # big-endian host's word has it elsewhere, and some hosts load aligned words only
        add_test(NAME arm_${m}_half COMMAND snx-test-arm-${m} --word eef21ae3 --word eeb21a44 --word eeb25be4
                 --word eeb26b6f --word eef35ac2 --word eeb36a62 --word eef36bc3 --word eeb37bc3 --word eef37b42)
        # VCVT of an odd S register to a double, both signs: a load of a double there is not aligned
        add_test(NAME arm_${m}_odd COMMAND snx-test-arm-${m} --word eef82bef --word eef82b6f)
        set_tests_properties(arm_${m} arm_${m}_spill arm_${m}_half arm_${m}_odd PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
        snx_arm_native_modes(${m} --blocks 600 --states 8)
    endforeach()
    # arm64's as a Windows host's
    add_executable(snx-test-arm-arm64win arm_x64.c support/host_unicorn.c support/la64_interp.c)
    snx_test_target(snx-test-arm-arm64win gnu23)
    target_compile_definitions(snx-test-arm-arm64win PRIVATE SNX_TEST_ARM64=1 SNX_TEST_ARM64WIN=1)
    target_include_directories(snx-test-arm-arm64win PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${SNX_UNICORN_INCLUDE_DIR}")
    target_link_libraries(snx-test-arm-arm64win PRIVATE snx_llvm snx_arm_llvm snx_driver "${SNX_UNICORN_LIBRARY}"
                          ${CMAKE_DL_LIBS})
    add_test(NAME arm_arm64win COMMAND snx-test-arm-arm64win --blocks 1500 --thumb 1000 --states 8)
    add_test(NAME arm_arm64win_spill COMMAND snx-test-arm-arm64win --pool 3 --blocks 1500 --thumb 1000 --states 8 --seed 2)
    add_test(NAME arm_arm64win_step COMMAND snx-test-arm-arm64win --step --blocks 300 --thumb 200 --states 2 --seed 3)
    add_test(NAME arm_arm64win_step_hosted COMMAND snx-test-arm-arm64win --step --memory hosted --blocks 200 --thumb 150
             --states 2 --seed 4)
    add_test(NAME arm_arm64win_step_bus COMMAND snx-test-arm-arm64win --step --profile arm7tdmi --trace --blocks 200
             --thumb 150 --states 2 --seed 5)
    add_test(NAME arm_arm64win_step_checked COMMAND snx-test-arm-arm64win --step --memory checked --blocks 200 --thumb 150
             --states 2 --seed 6)
    set_tests_properties(arm_arm64win arm_arm64win_spill arm_arm64win_step arm_arm64win_step_hosted
                         arm_arm64win_step_bus arm_arm64win_step_checked PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    # rv64's ABI extends a narrow argument with its sign, arm64's does not (the control); la64's and the others' tests
    # run the back end's own words (la64_interp) or skip, so they have no compiled function
    include(arm_call_abi.cmake)
    snx_arm_call_abi(rv64 riscv64-unknown-elf -march=rv64gc -mabi=lp64d -mcmodel=medany -mno-relax)
    # X18 is the system's on Windows and Apple, where the back end never touches it: the callee must not either
    snx_arm_call_abi(arm64 aarch64-unknown-elf -ffixed-x18)
endif()
# the x86-64 back end's functions as a Windows host's (the calling convention, COFF objects), run by this host's processor
# as Windows' C would call them (support/host_win64.c): against LLVM, as the other machines' under Unicorn
if(snx_host_win64)
    add_executable(snx-test-recompiler-win64 recompiler_backends.c llvm_qemu_words.c qemu_ref/qemu_ref.c
                   support/host_win64.c
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_cpu_helpers_flags(snx-test-recompiler-win64)
    snx_test_target(snx-test-recompiler-win64 gnu23)
    target_compile_definitions(snx-test-recompiler-win64 PRIVATE SNX_TEST_WIN64=1)
    target_include_directories(snx-test-recompiler-win64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-recompiler-win64 PRIVATE scarletnx-recompiler snx_llvm scarletnx-common)
    snx_decodetree_a64(snx-test-recompiler-win64)
    add_test(NAME recompiler_win64 COMMAND snx-test-recompiler-win64)
    add_test(NAME recompiler_win64_jit COMMAND snx-test-recompiler-win64 --jit)
    add_test(NAME recompiler_win64_hook COMMAND snx-test-recompiler-win64 --hook --functions 300 --seed 2)
    add_test(NAME recompiler_win64_plain COMMAND snx-test-recompiler-win64 --without 0x3d07 --functions 300 --seed 3)
    add_test(NAME recompiler_win64_br_pins COMMAND snx-test-recompiler-win64 --words ${snx_br_pins})
    add_test(NAME recompiler_win64_undef_pins COMMAND snx-test-recompiler-win64 --words ${snx_undef_pins})
    add_test(NAME recompiler_win64_calls COMMAND snx-test-recompiler-win64 --calls --functions 120 --states 8)
    set_tests_properties(recompiler_win64 recompiler_win64_jit recompiler_win64_hook recompiler_win64_plain PROPERTIES
                         TIMEOUT 600)
endif()
# its code on Windows unwinds at every instruction, by the system's unwinder there and by support/host_win64.c's here
# (native_unwind.c)
if(snx_host_win64 OR (SNX_X64 AND WIN32))
    add_executable(snx-test-x64-unwind native_unwind.c "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    if(NOT WIN32)
        target_sources(snx-test-x64-unwind PRIVATE support/host_win64.c)
    endif()
    snx_cpu_helpers_flags(snx-test-x64-unwind)
    snx_test_target(snx-test-x64-unwind gnu23)
    target_include_directories(snx-test-x64-unwind PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-x64-unwind PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    snx_decodetree_a64(snx-test-x64-unwind)
    add_test(NAME x64_unwind COMMAND snx-test-x64-unwind)
    set_tests_properties(x64_unwind PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
# and arm64's, on Windows on Arm and under Unicorn elsewhere
if((SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY) OR (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"))
    add_executable(snx-test-arm64-unwind native_unwind.c "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/x64/x64_helpers.c"
                   "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_cpu_helpers_flags(snx-test-arm64-unwind)
    snx_test_target(snx-test-arm64-unwind gnu23)
    target_compile_definitions(snx-test-arm64-unwind PRIVATE SNX_TEST_ARM64WIN=1)
    target_include_directories(snx-test-arm64-unwind PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-arm64-unwind PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    if(NOT WIN32)
        target_sources(snx-test-arm64-unwind PRIVATE support/host_unicorn.c support/la64_interp.c)
        target_include_directories(snx-test-arm64-unwind PRIVATE "${SNX_UNICORN_INCLUDE_DIR}")
        target_link_libraries(snx-test-arm64-unwind PRIVATE "${SNX_UNICORN_LIBRARY}")
    endif()
    snx_decodetree_a64(snx-test-arm64-unwind)
    add_test(NAME arm64_unwind COMMAND snx-test-arm64-unwind)
    set_tests_properties(arm64_unwind PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
# the objects every native back end writes for an AOT module: no absolute address in their code, the little-endian ones
# linked
if(NOT WIN32)
    add_executable(snx-test-native-link native_link.c)
    snx_test_target(snx-test-native-link gnu23)
    target_include_directories(snx-test-native-link PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-native-link PRIVATE scarletnx-recompiler snx_driver_llvm snx_driver snx_llvm
                          snx_arm_llvm ${CMAKE_DL_LIBS})
    add_test(NAME native_link COMMAND snx-test-native-link)
endif()
# the words past the guests' CPUs (a64.profile's max): the back end makes each op's, and from random states they give what
# qemu-aarch64 -cpu max gives (tests/qemu_ref), where this host has it: LLVM's back end has no code for half of them
foreach(family IN ITEMS
        "ints;CNT;CTZ;ABS;SMAX;SMIN;UMAX;UMIN;SMAX_i;SMIN_i;UMAX_i;UMIN_i;AXFLAG;XAFLAG;SUBP;SUBPS;ADDG_i;SUBG_i;GMI;IRG"
        "sha;BCAX;EOR3;RAX1;XAR;SHA512H;SHA512H2;SHA512SU0;SHA512SU1"
        "sm;SM3PARTW1;SM3PARTW2;SM3SS1;SM3TT1A;SM3TT1B;SM3TT2A;SM3TT2B;SM4E;SM4EKEY"
        "matrices;SMMLA;UMMLA;USMMLA;USDOT_v;USDOT_vi;SUDOT_vi;BFDOT_v;BFDOT_vi;BFMLAL_v;BFMLAL_vi;BFMMLA;BFCVTN_v;BFCVT_s"
        "fhm;FMLAL_v;FMLAL2_v;FMLSL_v;FMLSL2_v;FMLAL_vi;FMLAL2_vi;FMLSL_vi;FMLSL2_vi"
        "complex;FCADD_90;FCADD_270;FCMLA_v;FCMLA_vi;FRINT32X_v;FRINT32Z_v;FRINT64X_v;FRINT64Z_v;FRINT32X_s;FRINT32Z_s;FRINT64X_s;FRINT64Z_s;FJCVTZS"
        "newer;FCVTAS_g_simd;FCVTAU_g_simd;FCVTMS_g_simd;FCVTMU_g_simd;FCVTNS_g_simd;FCVTNU_g_simd;FCVTPS_g_simd;FCVTPU_g_simd;FCVTZS_g_simd;FCVTZU_g_simd;SCVTF_simd;UCVTF_simd;FAMAX;FAMIN;LUTI2_1b;LUTI2_1h;LUTI4_1b;LUTI4_2h")
    list(POP_FRONT family name)
    set(backends backends)
    if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
        list(APPEND backends arm64 rv64 ppc64 mips64 la64 ppc64be mips64be s390x)
    endif()
    foreach(m IN LISTS backends)
        add_test(NAME recompiler_${m}_past_${name}_made COMMAND snx-test-recompiler-${m} --refused --target max
                 --ops ${family})
        if(SNX_QEMU_REF)
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qemu_past/${m}_${name}")
            add_test(NAME recompiler_${m}_past_${name} COMMAND snx-test-recompiler-${m} --target max --states 16 --ops
                     ${family} --qemu "${SNX_QEMU_AARCH64}" "${SNX_QEMU_REF_RUNNER}"
                     "${CMAKE_CURRENT_BINARY_DIR}/qemu_past/${m}_${name}")
            set_tests_properties(recompiler_${m}_past_${name} PROPERTIES TIMEOUT 900)
            if(m MATCHES "^ppc64") # an unpatched Unicorn has none: skipped, as it compares nothing
                set_tests_properties(recompiler_${m}_past_${name} PROPERTIES SKIP_RETURN_CODE 77)
            endif()
        elseif(m STREQUAL "backends") # no QEMU: the words' values are not compared
            add_test(NAME recompiler_${m}_past_${name} COMMAND snx-test-recompiler-${m} --skip "${SNX_QEMU_REF_MISSING}")
        endif()
    endforeach()
endforeach()
# the tags' loads and stores, which LLVM's back end has
foreach(m IN LISTS backends)
    add_test(NAME recompiler_${m}_past_tags COMMAND snx-test-recompiler-${m} --target max --states 16 --ops STG STZG ST2G
             STZ2G STGP LDG)
    if(m MATCHES "^ppc64")
        set_tests_properties(recompiler_${m}_past_tags PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endforeach()
# an AOT module's descriptor for each little-endian RISC host (module_x64.c over mach/risc_desc.inc), its functions
# run under Unicorn (loongarch64's in support/la64_interp.c)
if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
    add_executable(snx-test-module-desc module_desc.c support/host_unicorn.c support/la64_interp.c)
    snx_test_target(snx-test-module-desc gnu23)
    target_include_directories(snx-test-module-desc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${SNX_UNICORN_INCLUDE_DIR}")
    target_link_libraries(snx-test-module-desc PRIVATE snx_driver_llvm snx_driver snx_llvm "${SNX_UNICORN_LIBRARY}"
                          ${CMAKE_DL_LIBS})
    # one test a machine: one whose Unicorn is missing (an unpatched one's PPC64) is skipped, not passed
    foreach(m riscv64 ppc64le mips64el loongarch64 arm64)
        add_test(NAME module_desc_${m} COMMAND snx-test-module-desc ${m})
        set_tests_properties(module_desc_${m} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endif()
# and arm64's for Windows, a COFF object
if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
    add_executable(snx-test-module-desc-arm64win module_desc.c support/host_unicorn.c support/la64_interp.c)
    snx_test_target(snx-test-module-desc-arm64win gnu23)
    target_compile_definitions(snx-test-module-desc-arm64win PRIVATE SNX_TEST_ARM64WIN=1)
    target_include_directories(snx-test-module-desc-arm64win PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                               "${SNX_UNICORN_INCLUDE_DIR}")
    target_link_libraries(snx-test-module-desc-arm64win PRIVATE snx_driver_llvm snx_driver snx_llvm
                          "${SNX_UNICORN_LIBRARY}" ${CMAKE_DL_LIBS})
    add_test(NAME module_desc_arm64win COMMAND snx-test-module-desc-arm64win)
    set_tests_properties(module_desc_arm64win PROPERTIES SKIP_RETURN_CODE 77)
endif()
# and x86-64's for Windows, a COFF object, its functions run by this host's processor (support/host_win64.c)
if(snx_host_win64)
    add_executable(snx-test-module-desc-win64 module_desc.c support/host_win64.c)
    snx_test_target(snx-test-module-desc-win64 gnu23)
    target_compile_definitions(snx-test-module-desc-win64 PRIVATE SNX_TEST_WIN64=1)
    target_include_directories(snx-test-module-desc-win64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-module-desc-win64 PRIVATE snx_driver_llvm snx_driver snx_llvm ${CMAKE_DL_LIBS})
    add_test(NAME module_desc_win64 COMMAND snx-test-module-desc-win64)
endif()
# a division in a loop of enough registers that R10 and R11 hold pins, where DIV moves what RAX and RDX held
add_test(NAME recompiler_backends_div_pins COMMAND snx-test-recompiler-backends --words d2800070 d503201f d503201f
         d503201f d503201f d503201f d503201f d503201f d503201f 8b0600ed 1ac50ce5 9ac90d0f f9000f21 f1000610 54fffe61)
# one where no register of the pool is free when DIV takes RDX from the divisor it holds
add_test(NAME recompiler_backends_div_divisor COMMAND snx-test-recompiler-backends --words d2800030 f9400ac7 1acc096b
         d503201f aa0b00cd aa0b000a d503201f d503201f d503201f d503201f 9ac8092e d503201f f9401746 f1000610 54fffe61)
# scalar floating point in a loop, its results made in the registers the loop keeps them in and its cold paths taken from
# each of their tests
add_test(NAME recompiler_backends_fp_pins COMMAND snx-test-recompiler-backends --states 600 --words d2800070 1e210800
         1f010802 1e232843 1e653884 1e61c086 1e274867 1e2020e0 9a83b041 f1000610 54fffee1)
# SHA256H of a value that XMM0 holds and nothing reads after, where SHA256RNDS2 wants the message in XMM0 (on a host with
# SHA-NI: elsewhere a helper makes the word, and the path does not run)
add_test(NAME recompiler_backends_sha_xmm0 COMMAND snx-test-recompiler-backends --made --states 40 --words 4ea684a5
         5e0640c5)
# FMLA of a vector in a loop on an x86-64 host with AVX and F16C but no FMA (0x800, FMA alone, taken away), where it is a
# cold call always taken (on arm64 the words run without its optional features): its result stays in the destination's
# pin, past the pins' reload
add_test(NAME recompiler_backends_cold_pin COMMAND snx-test-recompiler-backends --without ${snx_plain_fma_features}
         --states 40 --words
         d2800030 4ee1d41f 4f821bff 9a8211c3 0f406000 f1000610 54ffff61)
# random functions on a host without optional features: SSE4.2 alone (no BMI1, BMI2, LZCNT, F16C, AVX or FMA) on x86-64,
# the architecture's baseline on arm64
add_test(NAME recompiler_backends_plain COMMAND snx-test-recompiler-backends --without ${snx_plain_features} --seed 209458
         --functions 1200)
# what the library's archive needs from outside it: LLVM and the system's libraries alone
# (tests/recompiler_symbols.cmake). Its program links every object of the archive with those, named here rather than
# taken from the archive's own link line, which a new library would join. Built by the test, so a library more fails
# the test and not the build
add_executable(snx-test-recompiler-symbols EXCLUDE_FROM_ALL recompiler_symbols.c)
set_target_properties(snx-test-recompiler-symbols PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx-test-recompiler-symbols PRIVATE -pedantic-errors -Wall -Wextra -Werror)
add_dependencies(snx-test-recompiler-symbols scarletnx-recompiler)
target_link_directories(snx-test-recompiler-symbols PRIVATE "${SNX_LLVM_LIBRARY_DIR}")
target_link_libraries(snx-test-recompiler-symbols PRIVATE
                      "$<LINK_LIBRARY:WHOLE_ARCHIVE,$<TARGET_FILE:scarletnx-recompiler>>" ${SNX_LLVM_LIBS}
                      ${SNX_LLVM_SYSTEM_LIBS} Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(snx-test-recompiler-symbols PRIVATE advapi32)
else()
    target_link_libraries(snx-test-recompiler-symbols PRIVATE m)
endif()
snx_test_sanitize(snx-test-recompiler-symbols) # the sanitizers' runtime, which the archive's objects call there
add_test(NAME recompiler_symbols COMMAND "${CMAKE_COMMAND}" "-DBUILD=${PROJECT_BINARY_DIR}"
         -DTARGET=snx-test-recompiler-symbols "-DARCHIVE=$<TARGET_FILE:scarletnx-recompiler>" "-DNM=${CMAKE_NM}"
         "-DCONFIG=$<CONFIG>" -P "${CMAKE_CURRENT_SOURCE_DIR}/recompiler_symbols.cmake")
# The Mach-O linker on objects Clang compiles for arm64 macOS, which any host's Clang with AArch64 can: what macOS
# would check of the dylib (link_macho.c), on hosts that can't load one
if(TARGET OpenSSL::Crypto)
    set(snx_macho_probe "${CMAKE_CURRENT_BINARY_DIR}/link_macho_probe.o")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" --target=arm64-apple-macos13 -std=gnu23 -ffreestanding -c
        "${CMAKE_CURRENT_SOURCE_DIR}/link_macho_fixture.c" -DPART=2 -o "${snx_macho_probe}"
        RESULT_VARIABLE snx_macho_result OUTPUT_QUIET ERROR_QUIET)
    if(snx_macho_result EQUAL 0)
        foreach(part 1 2 3 4)
            # LLVM's modules have no common symbols, which the linker refuses: a tentative definition is not one
            # whatever the compiler's default
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/link_macho_${part}.o"
                COMMAND "${CMAKE_C_COMPILER}" --target=arm64-apple-macos13 -std=gnu23 -ffreestanding -O2 -fPIC -fno-common
                    -DPART=${part} -c "${CMAKE_CURRENT_SOURCE_DIR}/link_macho_fixture.c"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/link_macho_${part}.o"
                DEPENDS link_macho_fixture.c VERBATIM)
        endforeach()
        # the same parts as ELF, with the ABI and the visibility the runtime library's embedded object has
        # (recompiler/llvm/CMakeLists.txt): what the native back ends write for this host
        foreach(part 1 2 3 4 5 6 7)
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_${part}.o"
                COMMAND "${CMAKE_C_COMPILER}" --target=arm64-apple-macos13-elf -std=gnu23 -ffreestanding -O2 -fPIC
                    -fvisibility=hidden -fno-common -DPART=${part} -c "${CMAKE_CURRENT_SOURCE_DIR}/link_macho_fixture.c"
                    -o "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_${part}.o"
                DEPENDS link_macho_fixture.c VERBATIM)
        endforeach()
        set(snx_link_sources "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm")
        set(snx_macho_objects)
        foreach(object 1 2 3 4 elf_1 elf_2 elf_3 elf_4 elf_5 elf_6 elf_7)
            list(APPEND snx_macho_objects "${CMAKE_CURRENT_BINARY_DIR}/link_macho_${object}.o")
        endforeach()
        add_executable(snx-test-link-macho link_macho.c "${snx_link_sources}/link.c" "${snx_link_sources}/link_elf.c"
            "${snx_link_sources}/elf_object.c" "${snx_link_sources}/link_coff.c" "${snx_link_sources}/link_macho.c"
            ${snx_macho_objects})
        # the objects are the test's data, not part of its own link
        set_source_files_properties(${snx_macho_objects} PROPERTIES EXTERNAL_OBJECT FALSE HEADER_FILE_ONLY TRUE)
        snx_test_target(snx-test-link-macho gnu23)
        target_include_directories(snx-test-link-macho PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(snx-test-link-macho PRIVATE OpenSSL::Crypto)
        if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY) # the dylib's code, run
            target_sources(snx-test-link-macho PRIVATE support/host_unicorn.c support/la64_interp.c)
            target_include_directories(snx-test-link-macho PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                                   "${SNX_UNICORN_INCLUDE_DIR}")
            target_compile_definitions(snx-test-link-macho PRIVATE SNX_TEST_UNICORN=1)
            # the harness loads objects with the JIT's loader; the sources above stay this test's own
            target_link_libraries(snx-test-link-macho PRIVATE snx_llvm "${SNX_UNICORN_LIBRARY}" ${CMAKE_DL_LIBS})
        endif()
        # the linker's ELF side is not on Windows, whose objects are COFF
        if(NOT WIN32)
            set(snx_macho_elf_refusals
                "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_3.o" "a call to the undefined weak symbol _missing"
                "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_4.o" "aligned beyond a page"
                "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_5.o" "absolute address in read-only section .rodata"
                "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_6.o" "thread-local data is not supported")
        else()
            set(snx_macho_elf_refusals)
        endif()
        add_test(NAME link_macho COMMAND snx-test-link-macho "${CMAKE_CURRENT_BINARY_DIR}/link_macho_1.o"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_2.o" "${CMAKE_CURRENT_BINARY_DIR}/link_macho.dylib"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_1.o" "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_2.o"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_elf_7.o"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_3.o" "a call to the undefined weak symbol _missing"
            "${CMAKE_CURRENT_BINARY_DIR}/link_macho_4.o" "aligned beyond a page"
            ${snx_macho_elf_refusals})
    endif()
endif()

# the generated decoder, function discovery, and LLVM steps against expected values (llvm_steps.c)
add_executable(snx_codegen_test codegen_test.c llvm_steps.c)
snx_test_target(snx_codegen_test c23)
target_link_libraries(snx_codegen_test PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME native_codegen COMMAND snx_codegen_test)
# what an x86-64 unit's object is made from, which the unit cache keys it by (x64_unit_facts.c)
add_executable(snx-test-x64-unit-facts x64_unit_facts.c)
snx_test_target(snx-test-x64-unit-facts gnu23)
target_link_libraries(snx-test-x64-unit-facts PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME x64_unit_facts COMMAND snx-test-x64-unit-facts)
# what it makes of Advanced SIMD, by the size of its code and its writes of FPSR (x64_vec_code.c): ELF objects, so not
# on Windows
if(NOT WIN32)
    add_executable(snx-test-x64-vec-code x64_vec_code.c)
    snx_test_target(snx-test-x64-vec-code gnu23)
    target_link_libraries(snx-test-x64-vec-code PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
    snx_decodetree_a64(snx-test-x64-vec-code)
    add_test(NAME x64_vec_code COMMAND snx-test-x64-vec-code)
    set_tests_properties(x64_vec_code PROPERTIES SKIP_RETURN_CODE 77)
endif()

# NCE's patcher (src/nx/runtime/nce/nce_patch.c) on a synthetic module: the words that Eden's patcher changes, found
# in the code and kept where the code reads them as data, and the words of their stubs. Words only, on any host
add_executable(snx-test-nce-patch nce_patch.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce_net.c")
snx_test_target(snx-test-nce-patch c23)
target_link_libraries(snx-test-nce-patch PRIVATE snx_llvm snx_driver Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME nce_patch COMMAND snx-test-nce-patch "${CMAKE_CURRENT_BINARY_DIR}")

# NCE's machine layer (src/nx/runtime/nce/nce_host.c): guest code, patched with the patcher's stubs, runs natively.
# Its outcalls keep the guest's state, a hook runs its handler or the function's own code, the runs nest, a kick stops
# a loop and BRK stops in the runtime. On an AArch64 Linux host it runs as it is. Elsewhere Clang builds it for
# aarch64-linux-gnu against SNX_AARCH64_SYSROOT (a glibc sysroot; SNX_AARCH64_GCC_TOOLCHAIN gives its crt files), and
# it runs under qemu-aarch64. Without them there is no such test
set(SNX_AARCH64_SYSROOT "" CACHE PATH "A glibc sysroot of aarch64-linux-gnu, for the NCE test under qemu-aarch64")
set(SNX_AARCH64_GCC_TOOLCHAIN "" CACHE PATH "The GCC toolchain of SNX_AARCH64_SYSROOT (Clang's --gcc-toolchain)")
set(nce_host_sources "${CMAKE_CURRENT_SOURCE_DIR}/nce_host.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce_host.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce_net.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/clock.c")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    # twice: as Linux runs it, and in Android's indirect form (TLS_SLOT_APP), with an initial-exec pointer as the word
    foreach(form "" "-indirect")
        add_executable(snx-test-nce-host${form} ${nce_host_sources})
        snx_test_target(snx-test-nce-host${form} gnu23)
        target_compile_definitions(snx-test-nce-host${form} PRIVATE _GNU_SOURCE
                                   $<$<STREQUAL:${form},-indirect>:SNX_NCE_INDIRECT=1>)
        target_link_libraries(snx-test-nce-host${form} PRIVATE snx_driver Threads::Threads)
        string(REPLACE "-" "_" test_name "nce_host${form}")
        add_test(NAME ${test_name} COMMAND snx-test-nce-host${form})
    endforeach()
else()
    find_program(SNX_QEMU_AARCH64 qemu-aarch64)
    if(SNX_QEMU_AARCH64 AND SNX_AARCH64_SYSROOT AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(nce_host_toolchain "")
        if(SNX_AARCH64_GCC_TOOLCHAIN)
            set(nce_host_toolchain "--gcc-toolchain=${SNX_AARCH64_GCC_TOOLCHAIN}")
        endif()
        # both forms, as natively. The scanner's half of nce_patch.c is never called: --gc-sections drops it, with
        # what it needs of the build
        foreach(form "" "_indirect")
            set(nce_host_out "${CMAKE_CURRENT_BINARY_DIR}/nce_host${form}_aarch64")
            set(nce_host_form "")
            if(form)
                set(nce_host_form "-DSNX_NCE_INDIRECT=1")
            endif()
            add_custom_command(OUTPUT "${nce_host_out}"
                COMMAND "${CMAKE_C_COMPILER}" --target=aarch64-linux-gnu "--sysroot=${SNX_AARCH64_SYSROOT}"
                        ${nce_host_toolchain} -fuse-ld=lld -static -O2 -std=c23 -pedantic -Wall -Wextra -Werror
                        ${nce_host_form} -ffunction-sections -fdata-sections -Wl,--gc-sections
                        "-I${PROJECT_SOURCE_DIR}/src" "-I${PROJECT_BINARY_DIR}/src/common/decodetree/a64" ${nce_host_sources}
                        "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce_patch.c" -lpthread -o "${nce_host_out}"
                DEPENDS ${nce_host_sources} "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce_patch.c"
                        "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce_patch.h"
                        "${PROJECT_SOURCE_DIR}/src/nx/runtime/nce/nce_host.h"
                        "${PROJECT_BINARY_DIR}/src/common/decodetree/a64/a64_decode.h"
                COMMENT "Building NCE's machine-layer test for AArch64${form}" VERBATIM)
            add_custom_target(snx-test-nce-host${form}-aarch64 ALL DEPENDS "${nce_host_out}")
            add_test(NAME nce_host${form} COMMAND "${SNX_QEMU_AARCH64}" "${nce_host_out}")
            set_tests_properties(nce_host${form} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
        endforeach()
    else()
        message(STATUS "NCE machine-layer test skipped: not an AArch64 Linux host, and no qemu-aarch64 with "
                       "SNX_AARCH64_SYSROOT")
    endif()
endif()

# A64 cases ported from rEden's dynarmic tests (a64_ported_cases.h) through the LLVM backend's steps, loaded with the
# JIT's loader (llvm_steps.c): their expected values on any host and, where qemu_ref runs, each run against QEMU
# from the same state
add_executable(snx-test-a64-ported a64_ported.c llvm_steps.c qemu_ref/qemu_ref.c)
snx_test_target(snx-test-a64-ported gnu23)
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
snx_test_target(snx-test-astc gnu23)
add_test(NAME astc_decode COMMAND snx-test-astc)
# the generated MiiModel archive (hle/filesystem/mii_model.c, which the test includes): its tables against the
# console's part list and layout, every part's zlib stream, its textures decoded by the CPU references against what
# they were drawn from, its shapes' attributes and triangles, each file no bigger than the console's
add_executable(snx-test-mii-model mii_model.c support/astc.c support/bcn.c
               "${PROJECT_SOURCE_DIR}/src/common/utilities/mii_db.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/blocklinear.c") # g_gob_native, which the layout reads
snx_test_target(snx-test-mii-model c23)
target_include_directories(snx-test-mii-model PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-mii-model PRIVATE snx_libdeflate) # mii_model.c deflates the parts, the test inflates
if(NOT WIN32)
    target_link_libraries(snx-test-mii-model PRIVATE m)
endif()
add_test(NAME mii_model COMMAND snx-test-mii-model)
# the 3DS's Mii parts file the same model writes (CFL_Res.dat): its RomFS, sections, items, meshes and textures read
# back as the face library's layout says, the same bytes twice, a cube as a head and a mask alone written, the meshes
# the file cannot hold refused; and the same parts through a glTF model (mii_gltf.c), its broken parts refused
add_executable(snx-test-mii-cfl mii_cfl.c "${PROJECT_SOURCE_DIR}/src/common/utilities/mii_model.c"
               "${PROJECT_SOURCE_DIR}/src/common/utilities/mii_gltf.c")
snx_test_target(snx-test-mii-cfl c23)
target_include_directories(snx-test-mii-cfl PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-mii-cfl PRIVATE snx_libdeflate snx_cgltf) # mii_model.c's Switch files deflate their parts
if(NOT WIN32)
    target_link_libraries(snx-test-mii-cfl PRIVATE m)
endif()
add_test(NAME mii_cfl COMMAND snx-test-mii-cfl)
# the ASTC shader's GLSL run on the CPU (tests/glsl/glsl.hpp) against the same decoder: no GPU needed
add_executable(snx-test-astc-glsl glsl/astc_glsl.cpp support/astc.c)
snx_test_target(snx-test-astc-glsl gnu23)
target_include_directories(snx-test-astc-glsl PRIVATE glsl)
add_test(NAME astc_glsl COMMAND snx-test-astc-glsl)
# the BC7 encoder's GLSL run on the CPU (tests/glsl/glsl.hpp), its blocks decoded by the CPU reference: no GPU needed
add_executable(snx-test-bc7-glsl glsl/bc7_glsl.cpp glsl/bc7_cpu.cpp support/bcn.c)
snx_test_target(snx-test-bc7-glsl gnu23)
target_include_directories(snx-test-bc7-glsl PRIVATE glsl)
add_test(NAME bc7_glsl COMMAND snx-test-bc7-glsl)
# astc_bc7.comp's GLSL, ASTC blocks straight into BC7 ones, run on the CPU (tests/glsl/glsl.hpp): its blocks are the
# encoder's of the texels the CPU's ASTC decoder makes: no GPU needed
add_executable(snx-test-astc-bc7-glsl glsl/astc_bc7_glsl.cpp glsl/astc_bc7_cpu.cpp glsl/bc7_cpu.cpp support/astc.c
               support/bcn.c)
snx_test_target(snx-test-astc-bc7-glsl gnu23)
target_include_directories(snx-test-astc-bc7-glsl PRIVATE glsl)
add_test(NAME astc_bc7_glsl COMMAND snx-test-astc-bc7-glsl)
# astc_bcn.comp's GLSL, ASTC blocks straight into BC4, BC5 and BC1 ones for the textures whose blocks hold no more
# (gpu/texture/astc_kind.h), run on the CPU (tests/glsl/glsl.hpp): what each loses against the CPU decoder's texels,
# held to the floor measured; --measure FILE.astc... gives each file's format and loss beside BC7's. No GPU needed
add_executable(snx-test-astc-bcn-glsl glsl/astc_bcn_glsl.cpp glsl/astc_bcn_cpu.cpp glsl/astc_bc7_cpu.cpp support/astc.c
               support/bcn.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/blocklinear.c") # g_gob_native
snx_test_target(snx-test-astc-bcn-glsl gnu23)
target_include_directories(snx-test-astc-bcn-glsl PRIVATE glsl "${CMAKE_CURRENT_SOURCE_DIR}") # support/astc_kind_ref.h
add_test(NAME astc_bcn_glsl COMMAND snx-test-astc-bcn-glsl)
# astc_kind.comp's GLSL, what an ASTC texture's blocks hold read on the GPU (gpu/texture/astc_kind.h), run on the CPU
# (tests/glsl/glsl.hpp): each block's kind and each level's, its early stop too, are the CPU's. No GPU needed
add_executable(snx-test-astc-kind-glsl glsl/astc_kind_glsl.cpp
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/blocklinear.c") # g_gob_native
snx_test_target(snx-test-astc-kind-glsl gnu23)
target_include_directories(snx-test-astc-kind-glsl PRIVATE glsl "${CMAKE_CURRENT_SOURCE_DIR}") # support/astc_kind_ref.h
add_test(NAME astc_kind_glsl COMMAND snx-test-astc-kind-glsl)
# the ASTC 4x4 encoder's GLSL (BCn encoded again as ASTC) run on the CPU (tests/glsl/glsl.hpp), its blocks decoded by
# the CPU's ASTC decoder, each GPU vendor's build beside the generic one: no GPU needed
add_executable(snx-test-astc-encode-glsl glsl/astc_encode_glsl.cpp glsl/astc_encode_cpu.cpp glsl/bcn_astc_cpu.cpp
               glsl/astc_vendor_cpu.cpp glsl/bc7_cpu.cpp support/astc.c support/bcn.c)
snx_test_target(snx-test-astc-encode-glsl gnu23)
target_include_directories(snx-test-astc-encode-glsl PRIVATE glsl)
add_test(NAME astc_encode_glsl COMMAND snx-test-astc-encode-glsl)
# the BCn to ETC2 and EAC transcoder's GLSL (texconv.glsl's ETC2 build) run on the CPU (tests/glsl/glsl.hpp), its blocks
# decoded by the CPU references, the BCn pictures it is held to made by its own encoders and the BC7 one: no GPU needed
add_executable(snx-test-bcn-etc2-glsl glsl/bcn_etc2_glsl.cpp glsl/bcn_etc2_cpu.cpp glsl/bc7_cpu.cpp support/etc2.c
               support/bcn.c)
snx_test_target(snx-test-bcn-etc2-glsl gnu23)
target_include_directories(snx-test-bcn-etc2-glsl PRIVATE glsl)
add_test(NAME bcn_etc2_glsl COMMAND snx-test-bcn-etc2-glsl)
# the VIC shader's GLSL, both builds, run on the CPU against vic.c's composition of random jobs: no GPU needed
add_executable(snx-test-vic-glsl glsl/vic_jobs.c glsl/vic_glsl.cpp support/vic_ref.c)
snx_test_target(snx-test-vic-glsl gnu23)
target_include_directories(snx-test-vic-glsl PRIVATE glsl)
add_test(NAME vic_glsl COMMAND snx-test-vic-glsl)
# the block linear deswizzle and swizzle shader's GLSL run on the CPU against bl_offset's layout: no GPU needed
add_executable(snx-test-blocklinear-glsl glsl/blocklinear_glsl.cpp)
snx_test_target(snx-test-blocklinear-glsl gnu23)
target_include_directories(snx-test-blocklinear-glsl PRIVATE glsl)
add_test(NAME blocklinear_glsl COMMAND snx-test-blocklinear-glsl)
# the Vita's texture deswizzle shader's GLSL run on the CPU against Vita3K's loops: no GPU needed
add_executable(snx-test-vita-swizzle-glsl glsl/vita_swizzle_glsl.cpp)
snx_test_target(snx-test-vita-swizzle-glsl gnu23)
target_include_directories(snx-test-vita-swizzle-glsl PRIVATE glsl)
add_test(NAME vita_swizzle_glsl COMMAND snx-test-vita-swizzle-glsl)
# the Vita's decoded picture layout shader's GLSL run on the CPU against the decoder's layout: no GPU needed
add_executable(snx-test-vita-picture-glsl glsl/vita_picture_glsl.cpp)
snx_test_target(snx-test-vita-picture-glsl gnu23)
target_include_directories(snx-test-vita-picture-glsl PRIVATE glsl)
add_test(NAME vita_picture_glsl COMMAND snx-test-vita-picture-glsl)
# the Vita's YUV420 texture shader's GLSL run on the CPU against the conversion written out: no GPU needed
add_executable(snx-test-vita-yuv-glsl glsl/vita_yuv_glsl.cpp)
snx_test_target(snx-test-vita-yuv-glsl gnu23)
target_include_directories(snx-test-vita-yuv-glsl PRIVATE glsl)
add_test(NAME vita_yuv_glsl COMMAND snx-test-vita-yuv-glsl)
# the Vita's 24-bit color surface shader's GLSL run on the CPU against the layout written out: no GPU needed
add_executable(snx-test-vita-rgb24-glsl glsl/vita_rgb24_glsl.cpp)
snx_test_target(snx-test-vita-rgb24-glsl gnu23)
target_include_directories(snx-test-vita-rgb24-glsl PRIVATE glsl)
add_test(NAME vita_rgb24_glsl COMMAND snx-test-vita-rgb24-glsl)
# the Vita's imports given memory that isn't the game's: their errors, and no access there
# the VPK's members and the eboot's compressed segments, inflated by libdeflate (vita/loader/inflate.c, vpk.c), from
# streams and a zip miniz made
if(TARGET snx_vita_loader AND TARGET snx_miniz)
    add_executable(snx-test-vita-inflate vita_inflate.c)
    snx_test_target(snx-test-vita-inflate gnu23)
    target_link_libraries(snx-test-vita-inflate PRIVATE snx_vita_loader snx_miniz)
    add_test(NAME vita_inflate COMMAND snx-test-vita-inflate)
endif()
if(TARGET snx_vita_program AND NOT SNX_VITA_LOCKSTEP)
    # vita_lockstep in a build without Unicorn, said; the variables an older version read for it
    add_executable(snx-test-vita-lockstep vita_lockstep.c)
    snx_test_target(snx-test-vita-lockstep gnu23)
    target_link_libraries(snx-test-vita-lockstep PRIVATE snx_vita_program)
    add_test(NAME vita_lockstep COMMAND snx-test-vita-lockstep)
endif()
if(TARGET snx_vita_program)
    add_executable(snx-test-vita-hle-memory vita_hle_memory.c)
    snx_test_target(snx-test-vita-hle-memory gnu23)
    target_link_libraries(snx-test-vita-hle-memory PRIVATE snx_vita_program)
    add_test(NAME vita_hle_memory COMMAND snx-test-vita-hle-memory)
    # the save states: what a load puts back of a process the test drives through the imports (vita_state.c)
    add_executable(snx-test-vita-state vita_state.c)
    snx_test_target(snx-test-vita-state gnu23)
    target_link_libraries(snx-test-vita-state PRIVATE snx_vita_program)
    add_test(NAME vita_state COMMAND snx-test-vita-state)
    # tools/hookgen --vita's adapters (tests/hook/vita.hook): the arguments where AAPCS32 puts them, the results
    add_executable(snx-test-vita-hle-adapters vita_hle_adapters.c)
    snx_test_target(snx-test-vita-hle-adapters gnu23)
    target_link_libraries(snx-test-vita-hle-adapters PRIVATE snx_vita_program)
    include("${PROJECT_SOURCE_DIR}/cmake/hookgen.cmake")
    snx_vita_hookgen(snx-test-vita-hle-adapters SPEC "${CMAKE_CURRENT_SOURCE_DIR}/hook/vita.hook" PREFIX vita_test
                     NAME vita/hle/test.hook)
    add_test(NAME vita_hle_adapters COMMAND snx-test-vita-hle-adapters)
    # an image compiled by the native back end and by LLVM, both modules run as the run loop runs one
    add_executable(snx-test-vita-aot vita_aot.c)
    snx_test_target(snx-test-vita-aot gnu23)
    target_link_libraries(snx-test-vita-aot PRIVATE snx_vita_program)
    add_test(NAME vita_aot COMMAND snx-test-vita-aot)
    add_executable(snx-test-vita-watch vita_watch.c)
    snx_test_target(snx-test-vita-watch gnu23)
    target_link_libraries(snx-test-vita-watch PRIVATE snx_vita_program)
    add_test(NAME vita_watch COMMAND snx-test-vita-watch)
    # the Vita's device as a Switch game's is made (gpu/vk.c): VK_KHR_dynamic_rendering_local_read off when the check
    # of each extension's commands takes its command as missing (vk_debug_hide_commands), the log saying so, and when
    # its setting says off. The log is the device's stderr, read back through dup2
    if(NOT WIN32)
        add_executable(snx-test-vita-vk-device vita_vk_device.c)
        snx_test_target(snx-test-vita-vk-device gnu23)
        target_link_libraries(snx-test-vita-vk-device PRIVATE snx_vita_program)
        add_test(NAME vita_vk_device COMMAND snx-test-vita-vk-device)
        set_tests_properties(vita_vk_device PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
    # which device it is where one does not import guest memory, on the host's device listed twice (device.c compiled in)
    if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        add_executable(snx-test-vita-vk-import vita_vk_import.c)
        snx_test_target(snx-test-vita-vk-import gnu23)
        target_link_libraries(snx-test-vita-vk-import PRIVATE snx_vita_program)
        add_test(NAME vita_vk_import COMMAND snx-test-vita-vk-import)
        set_tests_properties(vita_vk_import PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
    # guest memory as the GPU reads and writes it, imported and, with VK_EXT_external_memory_host off, copied
    add_executable(snx-test-vita-render-memory vita_render_memory.c)
    snx_test_target(snx-test-vita-render-memory gnu23)
    target_link_libraries(snx-test-vita-render-memory PRIVATE snx_vita_program)
    add_test(NAME vita_render_memory COMMAND snx-test-vita-render-memory)
    set_tests_properties(vita_render_memory PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # the save states with the GPU's work in them: waited for by a save, the renderer made again by a load, on an
    # importing device and on a copying one (vita_state_gpu.c)
    add_executable(snx-test-vita-state-gpu vita_state_gpu.c)
    snx_test_target(snx-test-vita-state-gpu gnu23)
    target_link_libraries(snx-test-vita-state-gpu PRIVATE snx_vita_program)
    add_test(NAME vita_state_gpu COMMAND snx-test-vita-state-gpu)
    set_tests_properties(vita_state_gpu PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # the window's Esc, asked twice, as SDL sends it: no device or window
    add_executable(snx-test-vita-quit-key vita_quit_key.c)
    snx_test_target(snx-test-vita-quit-key gnu23)
    target_link_libraries(snx-test-vita-quit-key PRIVATE snx_vita_program)
    add_test(NAME vita_quit_key COMMAND snx-test-vita-quit-key)
    # the on-screen controls and the front panel in its window, skipped (77) with no Vulkan window
    add_executable(snx-test-vita-input vita_input.c)
    snx_test_target(snx-test-vita-input gnu23)
    target_link_libraries(snx-test-vita-input PRIVATE snx_vita_program)
    target_include_directories(snx-test-vita-input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}") # support/
    add_test(NAME vita_input COMMAND snx-test-vita-input)
    set_tests_properties(vita_input PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # the driver's command line, the recompile's options every driver shares: no game
    if(Python3_Interpreter_FOUND)
        add_test(NAME vita_driver COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vita_driver.py"
            "${PROJECT_BINARY_DIR}/scarletnx-vita${CMAKE_EXECUTABLE_SUFFIX}")
    endif()
endif()
if(TARGET snx_vita_usse)
    add_executable(snx-test-vita-usse-vdual vita_usse_vdual.c)
    snx_test_target(snx-test-vita-usse-vdual gnu23)
    target_link_libraries(snx-test-vita-usse-vdual PRIVATE snx_vita_usse)
    add_test(NAME vita_usse_vdual COMMAND snx-test-vita-usse-vdual)
    add_executable(snx-test-vita-usse-cube vita_usse_cube.c)
    snx_test_target(snx-test-vita-usse-cube gnu23)
    target_link_libraries(snx-test-vita-usse-cube PRIVATE snx_vita_usse)
    add_test(NAME vita_usse_cube COMMAND snx-test-vita-usse-cube)
    # a program whose counts and indices name more than it holds is refused, nothing read or written outside it
    add_executable(snx-test-vita-usse-bounds vita_usse_bounds.c)
    snx_test_target(snx-test-vita-usse-bounds gnu23)
    target_link_libraries(snx-test-vita-usse-bounds PRIVATE snx_vita_usse)
    add_test(NAME vita_usse_bounds COMMAND snx-test-vita-usse-bounds)
endif()
# the texture conversion shader's GLSL run on the CPU against the CPU's conversions and ETC2 and BCn decoders: no GPU
# needed
add_executable(snx-test-texconv-glsl glsl/texconv_glsl.cpp support/etc2.c support/bcn.c support/half.c)
snx_test_target(snx-test-texconv-glsl gnu23)
target_include_directories(snx-test-texconv-glsl PRIVATE glsl)
add_test(NAME texconv_glsl COMMAND snx-test-texconv-glsl)

# single blocks from rEden's ASTC decoder tests, against edenprobe's and astcenc's texels (astc_blocks.c)
add_executable(snx-test-astc-blocks astc_blocks.c support/astc.c)
snx_test_target(snx-test-astc-blocks gnu23)
add_test(NAME astc_blocks COMMAND snx-test-astc-blocks)
# what an ASTC block can hold, read from its endpoint modes (gpu/texture/astc_kind.h), against the CPU decoder's
# texels for random blocks of every mode; a level's kind through its layout; the BCn format each kind takes
add_executable(snx-test-astc-kind astc_kind.c support/astc.c
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/blocklinear.c") # g_gob_native, which the layout reads
snx_test_target(snx-test-astc-kind c23)
add_test(NAME astc_kind COMMAND snx-test-astc-kind)

add_executable(snx-test-nvn-command-gap nvn_command_gap.c support/nvn_command_gap.c)
snx_test_target(snx-test-nvn-command-gap gnu23)
add_test(NAME nvn_command_gap COMMAND snx-test-nvn-command-gap)

add_executable(snx-test-nvn-control nvn_control.c support/nvn_control.c)
snx_test_target(snx-test-nvn-control gnu23)
add_test(NAME nvn_control COMMAND snx-test-nvn-control)

add_executable(snx-test-nvn-section nvn_section.c)
snx_test_target(snx-test-nvn-section c23)
target_include_directories(snx-test-nvn-section PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME nvn_section COMMAND snx-test-nvn-section)

# the shared fonts pl.c hands out when a game folder has none: the stand-ins compiled in (cmake/shared_fonts.cmake)
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-pl-fonts pl_fonts.c "${PROJECT_SOURCE_DIR}/src/nx/nn/pl_font.c" "${PROJECT_BINARY_DIR}/generated/shared_fonts.c")
    snx_test_target(snx-test-pl-fonts gnu23)
    snx_service_swipc(snx-test-pl-fonts)
    snx_hookgen(snx-test-pl-fonts SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/pl.hook" A32_MARKED)
    add_test(NAME pl_fonts COMMAND snx-test-pl-fonts)
endif()

# aud:u and the hardware Opus decoder (nn/codec.c) in a fixture, with FFmpeg's Opus decoder for the decoders that open. FFmpeg::avcodec comes
# from cmake/dependencies.cmake, before this folder, in every build but a tools-only one, which checks the fixture
# without it (SNX_NO_H264, which no runtime build defines)
if(SNX_TOOLS_ONLY)
    add_executable(snx-test-audio-out audio_out.c)
    snx_test_target(snx-test-audio-out gnu23)
    snx_service_swipc(snx-test-audio-out hle/services/audio.swipc)
    snx_hookgen(snx-test-audio-out SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/codec.hook")
    target_sources(snx-test-audio-out PRIVATE "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    target_compile_definitions(snx-test-audio-out PRIVATE SNX_NO_H264)
    if(NOT WIN32)
        target_link_libraries(snx-test-audio-out PRIVATE m)
    endif()
    add_test(NAME audio_out COMMAND snx-test-audio-out)
else()
    add_executable(snx-test-audio-opus audio_out.c)
    snx_test_target(snx-test-audio-opus gnu23)
    snx_service_swipc(snx-test-audio-opus hle/services/audio.swipc)
    snx_hookgen(snx-test-audio-opus SPEC "${PROJECT_SOURCE_DIR}/src/nx/nn/codec.hook")
    target_sources(snx-test-audio-opus PRIVATE "${PROJECT_SOURCE_DIR}/src/nx/nn/version.c")
    target_link_libraries(snx-test-audio-opus PRIVATE FFmpeg::avcodec)
    if(NOT WIN32)
        target_link_libraries(snx-test-audio-opus PRIVATE m)
    endif()
    add_test(NAME audio_opus COMMAND snx-test-audio-opus)
endif()

# the player's Volume on an SDL output, the Switch's and the 3DS's (common/utilities/audio_volume.h), on SDL's dummy driver
if(TARGET SDL3::SDL3)
    add_executable(snx-test-audio-volume audio_volume.c)
    snx_test_target(snx-test-audio-volume gnu23)
    target_link_libraries(snx-test-audio-volume PRIVATE scarletnx-common SDL3::SDL3)
    add_test(NAME audio_volume COMMAND snx-test-audio-volume)
endif()
# a 5.1 game's PCM mixed to stereo for a host output of two channels (runtime/platform/audio_downmix.c)
add_executable(snx-test-audio-downmix audio_downmix.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/audio_downmix.c")
snx_test_target(snx-test-audio-downmix c23)
target_link_libraries(snx-test-audio-downmix PRIVATE scarletnx-common)
add_test(NAME audio_downmix COMMAND snx-test-audio-downmix)

if(UNIX)
    add_executable(snx-test-audren audren_render.c)
    snx_test_target(snx-test-audren gnu23)
        target_link_libraries(snx-test-audren PRIVATE m)
    add_test(NAME audren_render COMMAND snx-test-audren)
endif()

# audren.c's host side with the DSP stubbed: the output device's volume on every renderer's host stream
add_executable(snx-test-audren-host audren_host.c)
snx_test_target(snx-test-audren-host gnu23)
add_test(NAME audren_host COMMAND snx-test-audren-host)

# the audio clock (audio.c) and the renderer's frames it queues (audren.c) on a host the test makes, whose clock
# thread stalls: how far the sound trails the clock afterwards. Without FFmpeg: no Opus decoder opens here
add_executable(snx-test-audio-clock audio_clock.c)
snx_test_target(snx-test-audio-clock gnu23)
snx_service_swipc(snx-test-audio-clock hle/services/audio.swipc)
target_compile_definitions(snx-test-audio-clock PRIVATE SNX_NO_H264)
if(NOT WIN32)
    target_link_libraries(snx-test-audio-clock PRIVATE m)
endif()
add_test(NAME audio_clock COMMAND snx-test-audio-clock)

# gpu/renderer.c's side of the front end with a stand-in renderer and no device: the copy of each draw state that the
# renderer reads, and what the front end counts and holds back
add_executable(snx-test-gpu-records gpu_records.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gpu_queue.c")
snx_test_target(snx-test-gpu-records gnu23)
target_compile_options(snx-test-gpu-records PRIVATE -Wno-missing-field-initializers) # renderer.c's records, as -Wextra sees them
target_include_directories(snx-test-gpu-records PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-gpu-records PRIVATE scarletnx-common scarletnx-platform) # gpu_split's thread
add_test(NAME gpu_records COMMAND snx-test-gpu-records)
# the same records on gpu_split's renderer thread
add_test(NAME gpu_records_split COMMAND snx-test-gpu-records split)
set_tests_properties(gpu_records_split PROPERTIES TIMEOUT 30) # a wait in the queue that never ends would hang ctest

# gpu/gpu_queue.c's ring with stand-ins for the work of its records, with no device and no gpu.c: the records run in
# their order with their bytes, also when the ring wraps
add_executable(snx-test-gpu-queue gpu_queue_records.c)
snx_test_target(snx-test-gpu-queue gnu23)
target_include_directories(snx-test-gpu-queue PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-gpu-queue PRIVATE scarletnx-common scarletnx-platform) # gpu_split's thread
add_test(NAME gpu_queue COMMAND snx-test-gpu-queue)
# the same ring with gpu_split's renderer thread running its records
add_test(NAME gpu_queue_split COMMAND snx-test-gpu-queue split)
set_tests_properties(gpu_queue_split PROPERTIES TIMEOUT 30)

add_executable(snx-test-render-enable render_enable.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/draw_state.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/draw_state.c")
snx_test_target(snx-test-render-enable gnu23)
add_test(NAME render_enable COMMAND snx-test-render-enable)

add_executable(snx-test-draw-state-groups draw_state_groups.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/draw_state.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/draw_state.c")
snx_test_target(snx-test-draw-state-groups gnu23)
add_test(NAME draw_state_groups COMMAND snx-test-draw-state-groups)

add_executable(snx-test-clear-flags clear_flags.c)
snx_test_target(snx-test-clear-flags gnu23)
add_test(NAME clear_flags COMMAND snx-test-clear-flags)

add_executable(snx-test-instance-array instance_array.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/draw_state.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/draw_state.c")
snx_test_target(snx-test-instance-array gnu23)
add_test(NAME instance_array COMMAND snx-test-instance-array)

add_executable(snx-test-draw-texture draw_texture.c "${PROJECT_SOURCE_DIR}/tests/support/gpu_front_stub.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/draw_state.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/draw_state.c")
snx_test_target(snx-test-draw-texture gnu23)
add_test(NAME draw_texture COMMAND snx-test-draw-texture)
# its blit's rectangles, cut together to both images at the console's pixels (gpu/vk/draw_texture.c)
add_executable(snx-test-vk-draw-texture-blit vk_draw_texture_blit.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/draw_texture.c")
snx_test_target(snx-test-vk-draw-texture-blit c23)
target_link_libraries(snx-test-vk-draw-texture-blit PRIVATE m)
add_test(NAME vk_draw_texture_blit COMMAND snx-test-vk-draw-texture-blit)

# gpu.c's queue over the real puller, 3D engine and macros: a channel waiting on a semaphore, a syncpoint, a fence or
# inside a macro lets the others run, and goes on where it stopped (its platform stand-ins are pthreads, and Windows'
# own locks for the platform's types there)
add_executable(snx-test-gpu-park gpu_park.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gpu.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/gpu_queue.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/draw_state.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/draw_state.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/mme.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/dma.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/blocklinear.c"
    support/dma_ref.c support/gpu_front_stub.c own_stub.c)
snx_test_target(snx-test-gpu-park gnu23)
target_compile_definitions(snx-test-gpu-park PRIVATE SNX_TEST_GPU_QUEUE) # the real gq_call, not the stub's
target_link_libraries(snx-test-gpu-park PRIVATE scarletnx-common) # gpu.c says why a thread did not start
add_test(NAME gpu_park COMMAND snx-test-gpu-park)
set_tests_properties(gpu_park PROPERTIES TIMEOUT 30) # a deadlock in the queue would otherwise hang ctest
# the same queue with gpu_split's renderer thread running its records
add_test(NAME gpu_park_split COMMAND snx-test-gpu-park split)
set_tests_properties(gpu_park_split PROPERTIES TIMEOUT 30)

# the shader programs a game ships, found in containers built by the test (gpu/shader/shipped.c)
add_executable(snx-test-shipped-shaders shipped_shaders.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/shipped.c")
snx_test_target(snx-test-shipped-shaders c23)
add_test(NAME shipped_shaders COMMAND snx-test-shipped-shaders)
# the walk over a game's files and segments for those programs (gpu/shipped_pass.c), on a RomFS made by the test
add_executable(snx-test-shipped-pass shipped_pass.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/shipped_pass.c"
               "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/shipped.c")
snx_test_target(snx-test-shipped-pass c23)
target_link_libraries(snx-test-shipped-pass PRIVATE snx_zstd) # an Ounce game's .zs files
include("${PROJECT_SOURCE_DIR}/cmake/ship_walk.cmake")
snx_ship_walk("${PROJECT_SOURCE_DIR}/src/nx/gpu/shipped_pass.c")
add_test(NAME shipped_pass COMMAND snx-test-shipped-pass)
# the walk's version, which keys the .vkscan file: the same in every folder, and with CRLF line ends
add_test(NAME ship_walk_version COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/ship-walk-test" "-DGENERATOR=${CMAKE_GENERATOR}"
         "-DMAKE=${CMAKE_MAKE_PROGRAM}" -P "${CMAKE_CURRENT_SOURCE_DIR}/ship_walk_test.cmake")
# ZBIC, the other compression of an Ounce game's NSO segments (common/utilities/zbic.c), on a frame of synthetic text
if(TARGET snx_zbic)
    add_executable(snx-test-zbic zbic.c)
    snx_test_target(snx-test-zbic c23)
    target_include_directories(snx-test-zbic PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(snx-test-zbic PRIVATE snx_zbic snx_zstd)
    add_test(NAME zbic COMMAND snx-test-zbic)
endif()

add_executable(snx-test-etc2 etc2_decode.c support/etc2.c)
snx_test_target(snx-test-etc2 gnu23)
add_test(NAME etc2_decode COMMAND snx-test-etc2)
add_executable(snx-test-bcn bcn_decode.c support/bcn.c)
snx_test_target(snx-test-bcn gnu23)
add_test(NAME bcn_decode COMMAND snx-test-bcn)

# the LLVM backend's IR rewrite for an older llvm-as (a cross build's LLVM older than its Clang): what it takes out
add_test(NAME rtlib_ir_compat COMMAND "${CMAKE_COMMAND}"
         -DSCRIPT=${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm/rtlib_ir_compat.cmake
         -DWORK=${CMAKE_CURRENT_BINARY_DIR} -P "${CMAKE_CURRENT_SOURCE_DIR}/rtlib_ir_compat_test.cmake")

if(UNIX)
    add_executable(snx-test-kernel-mutex kernel_mutex.c)
    snx_test_target(snx-test-kernel-mutex gnu23)
        add_test(NAME kernel_mutex COMMAND snx-test-kernel-mutex)
    add_executable(snx-test-kernel-wait kernel_wait.c)
    snx_test_target(snx-test-kernel-wait gnu23)
        add_test(NAME kernel_wait COMMAND snx-test-kernel-wait)
    add_executable(snx-test-kernel-cores kernel_cores.c)
    snx_test_target(snx-test-kernel-cores gnu23)
    target_link_libraries(snx-test-kernel-cores PRIVATE scarletnx-common) # its g_rtapi's cas128 (runtime/cas128.c)
        add_test(NAME kernel_cores COMMAND snx-test-kernel-cores)
    # g_hle holds no pointer and nothing of the host's, so a save state writes it as it is (hle_flat.c)
    add_executable(snx-test-hle-flat hle_flat.c)
    snx_test_target(snx-test-hle-flat gnu23)
        add_test(NAME hle_flat COMMAND snx-test-hle-flat)
    if(NOT WIN32)
        # a save state's file: sections read back, damage refused, a failed save leaves the old state (state_file.c)
        add_executable(snx-test-state-file state_file.c "${PROJECT_SOURCE_DIR}/src/common/utilities/state_file.c")
        snx_test_target(snx-test-state-file gnu23)
        target_link_libraries(snx-test-state-file PRIVATE snx_zstd scarletnx-common)
        add_test(NAME state_file COMMAND snx-test-state-file "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # the hang report's slots, of threads that come and go (common/platform/thread.c)
    add_executable(snx-test-plat-wait-slots plat_wait_slots.c)
    snx_test_target(snx-test-plat-wait-slots gnu23)
    target_link_libraries(snx-test-plat-wait-slots PRIVATE SDL3::SDL3)
    target_link_libraries(snx-test-plat-wait-slots PRIVATE scarletnx-common)
    target_link_libraries(snx-test-plat-wait-slots PRIVATE scarletnx-platform)
    add_test(NAME plat_wait_slots COMMAND snx-test-plat-wait-slots)
    set_tests_properties(plat_wait_slots PROPERTIES TIMEOUT 60)
    add_executable(snx-test-plat-cpus plat_cpus.c)
    snx_test_target(snx-test-plat-cpus gnu23)
    target_link_libraries(snx-test-plat-cpus PRIVATE SDL3::SDL3) # plat_posix.c's threads end SDL's per-thread state
    target_link_libraries(snx-test-plat-cpus PRIVATE scarletnx-common) # plat_posix.c names its threads (snx_thread_name)
    target_link_libraries(snx-test-plat-cpus PRIVATE scarletnx-platform) # plat_thread, plat_cpus, the waits
    add_test(NAME plat_cpus COMMAND snx-test-plat-cpus)
endif()

# the profiler's view of a named lock (common/platform/thread.c): its release is reported while it is held
add_executable(snx-test-pm-tracy pm_tracy.c)
snx_test_target(snx-test-pm-tracy gnu23)
target_link_libraries(snx-test-pm-tracy PRIVATE scarletnx-platform)
add_test(NAME pm_tracy COMMAND snx-test-pm-tracy)

# the process's memory as common.c reads it, which memory_fit takes (process_memory.c)
add_executable(snx-test-process-memory process_memory.c)
snx_test_target(snx-test-process-memory gnu23)
target_link_libraries(snx-test-process-memory PRIVATE scarletnx-common)
add_test(NAME process_memory COMMAND snx-test-process-memory)
# the process's threads as common.c lists them, which the log's threads line takes (process_threads.c)
add_executable(snx-test-process-threads process_threads.c)
snx_test_target(snx-test-process-threads gnu23)
target_link_libraries(snx-test-process-threads PRIVATE scarletnx-common)
add_test(NAME process_threads COMMAND snx-test-process-threads)
# a thread at the system's low priority as common.c sets it, the pipeline workers' past the first two
# (thread_priority.c)
add_executable(snx-test-thread-priority thread_priority.c)
snx_test_target(snx-test-thread-priority gnu23)
target_link_libraries(snx-test-thread-priority PRIVATE scarletnx-common)
add_test(NAME thread_priority COMMAND snx-test-thread-priority)
# the debugger tracing the process as common.c reads it, which the stats box and F1's help name (process_debugger.c)
add_executable(snx-test-process-debugger process_debugger.c)
snx_test_target(snx-test-process-debugger gnu23)
target_link_libraries(snx-test-process-debugger PRIVATE scarletnx-common)
add_test(NAME process_debugger COMMAND snx-test-process-debugger)
# what the log's lines on the process's resources compute (runtime/platform/resources.c): the threads line
add_executable(snx-test-resources resources.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/resources.c")
snx_test_target(snx-test-resources gnu23)
target_include_directories(snx-test-resources PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx-test-resources PRIVATE scarletnx-common)
add_test(NAME resources COMMAND snx-test-resources)

# EAC worked examples and edenprobe's blocks from rEden's decode_eac test (eac_blocks.c)
add_executable(snx-test-eac-blocks eac_blocks.c support/etc2.c)
snx_test_target(snx-test-eac-blocks gnu23)
add_test(NAME eac_blocks COMMAND snx-test-eac-blocks)

if(UNIX)
    add_executable(snx-test-memory memory_accounting.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/hle.c")
    snx_test_target(snx-test-memory gnu23)
        add_test(NAME memory_accounting COMMAND snx-test-memory)
    set_tests_properties(memory_accounting PROPERTIES TIMEOUT 60)
    # views through the alias pool, with the real POSIX platform layer (77 where the host has no pool)
    add_executable(snx-test-memory-alias memory_alias.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/own.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/hle.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_posix.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread_spawn.c")
    snx_test_target(snx-test-memory-alias gnu23)
    target_link_libraries(snx-test-memory-alias PRIVATE SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_link_libraries(snx-test-memory-alias PRIVATE scarletnx-common) # plat_posix.c names its threads (snx_thread_name)
    if(APPLE AND NOT IOS) # plat_posix.c's power source: IOKit's notifications on CoreFoundation's run loop
        target_link_libraries(snx-test-memory-alias PRIVATE "-framework IOKit" "-framework CoreFoundation")
    endif()
    add_test(NAME memory_alias COMMAND snx-test-memory-alias)
    set_tests_properties(memory_alias PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # the same calls with the alias pool off (alias_pool=0): views are copies, as on Windows
    add_test(NAME memory_alias_copies COMMAND snx-test-memory-alias --copies)
    set_tests_properties(memory_alias_copies PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # views over pages the write tracking protects (write_tracking=1: the tracking of hosts without Linux 6.7's),
    # without its hot pages and with them (write_tracking_hot_pages's Auto, the default, turns them on there)
    add_test(NAME memory_alias_signal_ww COMMAND snx-test-memory-alias --write_tracking=1 --write_tracking_hot_pages=0)
    add_test(NAME memory_alias_signal_ww_auto COMMAND snx-test-memory-alias --write_tracking=1)
    set_tests_properties(memory_alias_signal_ww memory_alias_signal_ww_auto
                         PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # the write tracking in NCE's layout (g_mem 0, moved up from 64 GiB), each kind: Android's is the protection
    add_test(NAME memory_alias_nce COMMAND snx-test-memory-alias --nce)
    set_tests_properties(memory_alias_nce PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_test(NAME memory_alias_nce_signal_ww COMMAND snx-test-memory-alias --nce --write_tracking=1
             --write_tracking_hot_pages=0)
    set_tests_properties(memory_alias_nce_signal_ww PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # a source given back while the pool can't give its bytes: the fatal says which step failed, and the error
    add_test(NAME memory_alias_unpool_fails COMMAND snx-test-memory-alias --unpool-read-fails)
    set_tests_properties(memory_alias_unpool_fails PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # GPU-owned memory over the real pool and traps, owners whose GPU copy is host memory (77 without traps)
    add_executable(snx-test-memory-own memory_own.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/own.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_posix.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread_spawn.c")
    snx_test_target(snx-test-memory-own gnu23)
    target_link_libraries(snx-test-memory-own PRIVATE Threads::Threads SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_link_libraries(snx-test-memory-own PRIVATE scarletnx-common) # plat_posix.c names its threads (snx_thread_name)
    if(APPLE AND NOT IOS) # plat_posix.c's power source: IOKit's notifications on CoreFoundation's run loop
        target_link_libraries(snx-test-memory-own PRIVATE "-framework IOKit" "-framework CoreFoundation")
    endif()
    add_test(NAME memory_own COMMAND snx-test-memory-own)
    set_tests_properties(memory_own PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # traps over pages the write tracking protects (write_tracking=1)
    add_test(NAME memory_own_signal_ww COMMAND snx-test-memory-own --write_tracking=1 --write_tracking_hot_pages=0)
    # traps by protection and the fault handler (memory_traps=1: those of Linux before 5.13 and Android), beside
    # userfaultfd's write tracking and beside the tracking by protection
    add_test(NAME memory_own_signal_traps COMMAND snx-test-memory-own --memory_traps=1)
    add_test(NAME memory_own_signal_both COMMAND snx-test-memory-own --memory_traps=1 --write_tracking=1
             --write_tracking_hot_pages=0)
    # no traps, asked for (memory_traps=2) and where the protection has no pool to trap: the log says which
    add_test(NAME memory_own_no_traps COMMAND snx-test-memory-own --memory_traps=2)
    add_test(NAME memory_own_signal_traps_no_pool COMMAND snx-test-memory-own --memory_traps=1 --alias_pool=0)
    set_tests_properties(memory_own_signal_ww memory_own_signal_traps memory_own_signal_both memory_own_no_traps
                         memory_own_signal_traps_no_pool PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
if(WIN32 AND TARGET SDL3::SDL3)
    # GPU-owned memory over Windows' traps (platform/plat_win32.c): page protection and the vectored exception
    # handler on the reservation's own pages, landings in place, views that are copies, ReadFile and WriteFile refused
    add_executable(snx-test-memory-own memory_own.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/own.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_win32.c"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c" "${PROJECT_SOURCE_DIR}/src/common/platform/wpath.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread_spawn.c")
    snx_test_target(snx-test-memory-own gnu23)
    target_link_libraries(snx-test-memory-own PRIVATE Threads::Threads pthread SDL3::SDL3 synchronization powrprof)
    target_link_libraries(snx-test-memory-own PRIVATE scarletnx-common) # plat_win32.c names its threads
    add_test(NAME memory_own COMMAND snx-test-memory-own)
    # memory_traps=1 is the same protection there; 2 has none, and the log says so
    add_test(NAME memory_own_protection COMMAND snx-test-memory-own --memory_traps=1)
    add_test(NAME memory_own_no_traps COMMAND snx-test-memory-own --memory_traps=2)
    set_tests_properties(memory_own memory_own_protection memory_own_no_traps
                         PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

endif() # SNX_BUILD_NX

# the bands a block linear target is published or written back in (vk/rt_bands.h): they tile the span within the cap
add_executable(snx-test-rt-bands rt_bands.c)
snx_test_target(snx-test-rt-bands gnu23)
add_test(NAME rt_bands COMMAND snx-test-rt-bands)
# whether a guest write covers a render target (vk/rt_cover.h): every byte from its first to its last, nothing wraps
add_executable(snx-test-rt-cover rt_cover.c)
snx_test_target(snx-test-rt-cover 23)
add_test(NAME rt_cover COMMAND snx-test-rt-cover)
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
snx_test_target(snx-test-depth-texel gnu23)
add_test(NAME depth_texel COMMAND snx-test-depth-texel)
add_executable(snx-test-write-pages write_pages.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c")
snx_test_target(snx-test-write-pages gnu23)
add_test(NAME write_pages COMMAND snx-test-write-pages)
add_executable(snx-test-memory-own-cut memory_own_cut.c)
snx_test_target(snx-test-memory-own-cut gnu23)
target_link_libraries(snx-test-memory-own-cut PRIVATE scarletnx-platform) # own.c's landing times (plat_ns)
add_test(NAME memory_own_cut COMMAND snx-test-memory-own-cut)
add_executable(snx-test-draw-state-mx draw_state_mx.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/draw_state.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/draw_state.c")
snx_test_target(snx-test-draw-state-mx c23)
add_test(NAME draw_state_mx COMMAND snx-test-draw-state-mx)
add_executable(snx-bench-write-pages EXCLUDE_FROM_ALL bench_write_pages.c
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/write_pages.c")
snx_test_target(snx-bench-write-pages gnu23)
# the free space of a block of device memory (src/nx/gpu/vk/space.c)
add_executable(snx-test-vk-space vk_space.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/space.c")
snx_test_target(snx-test-vk-space c23)
target_include_directories(snx-test-vk-space PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME vk_space COMMAND snx-test-vk-space)
add_executable(snx-test-pending-pages pending_pages.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/pending_pages.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
snx_test_target(snx-test-pending-pages gnu23)
target_link_libraries(snx-test-pending-pages PRIVATE scarletnx-filesystem Threads::Threads) # pend_lock's PMutex
add_test(NAME pending_pages COMMAND snx-test-pending-pages)
# indexed quads made into triangles on the CPU, in the order the GPU's conversion makes them
add_executable(snx-test-quad-indices quad_indices.c)
snx_test_target(snx-test-quad-indices c23)
target_include_directories(snx-test-quad-indices PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME quad_indices COMMAND snx-test-quad-indices)
# a draw's primitives made into lines or points on the CPU: polygons in the polygon mode Line or Point, and line loops
add_executable(snx-test-polygon-edges polygon_edges.c)
snx_test_target(snx-test-polygon-edges c23)
target_include_directories(snx-test-polygon-edges PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME polygon_edges COMMAND snx-test-polygon-edges)
add_executable(snx-test-unattended-script unattended_script.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c"
    "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c"
    "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c") # plat_mkdir, the slots' folder
snx_test_target(snx-test-unattended-script gnu23)
target_link_libraries(snx-test-unattended-script PRIVATE m scarletnx-common) # snx_read_file, snx_fopen
add_test(NAME unattended_script COMMAND snx-test-unattended-script "${CMAKE_CURRENT_BINARY_DIR}")
# the environment variables an older runtime read, this run's values for their settings and said with them
add_executable(snx-test-deprecated-env deprecated_env.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/deprecated_env.c")
snx_test_target(snx-test-deprecated-env gnu23)
add_test(NAME deprecated_env COMMAND snx-test-deprecated-env)
# the log's file, early lines, stamps, redirect, limits, fatal and crash mode (platform/log.c)
add_executable(snx-test-platform-log platform_log.c "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c")
snx_test_target(snx-test-platform-log gnu23)
target_link_libraries(snx-test-platform-log PRIVATE snx_error scarletnx-common) # the runs' logs: snx_list_dir, snx_file_time
add_test(NAME platform_log COMMAND snx-test-platform-log "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT WIN32 AND TARGET SDL3::SDL3) # SDL3 is there with a driver that draws (cmake/dependencies.cmake)
    # written-page tracking, decommit, pread, file maps, replace, folders, waits and threads (platform/plat_posix.c,
    # plat_sdl.c and common/platform)
    add_executable(snx-test-platform-posix platform_posix.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread_spawn.c")
    snx_test_target(snx-test-platform-posix gnu23)
    target_link_libraries(snx-test-platform-posix PRIVATE SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_link_libraries(snx-test-platform-posix PRIVATE scarletnx-common) # plat_posix.c names its threads (snx_thread_name)
    # a library no test links, for plat_dl_loaded to find only once it is opened
    add_library(snx-test-dl-probe SHARED dl_probe.c)
    snx_test_sanitize(snx-test-dl-probe) # the test's process loads it: built as the test is
    target_compile_definitions(snx-test-platform-posix PRIVATE SNX_TEST_DL_PROBE="$<TARGET_FILE:snx-test-dl-probe>")
    add_dependencies(snx-test-platform-posix snx-test-dl-probe)
    if(APPLE AND NOT IOS) # plat_posix.c's power source: IOKit's notifications on CoreFoundation's run loop
        target_link_libraries(snx-test-platform-posix PRIVATE "-framework IOKit" "-framework CoreFoundation")
    endif()
    add_test(NAME platform_posix COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}")
    # the written-page tracking macOS, FreeBSD, Android and older Linux have: protection and the fault handler, also
    # without and with its hot pages (write_tracking_hot_pages 0 and 1, which says it does nothing where userfaultfd
    # tracks), and with Auto, the default, which turns them on there;
    # and none: the scan says so, and the caches compare contents. Each in a folder of its own, as ctest may run them
    # at once and each writes and replaces the same files
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_signal_ww"
         "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_signal_ww_hot" "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_hot"
         "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_signal_ww_auto" "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_no_tracking")
    add_test(NAME platform_posix_signal_ww
             COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_signal_ww" --write_tracking=1
                     --write_tracking_hot_pages=0)
    add_test(NAME platform_posix_signal_ww_hot
             COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_signal_ww_hot"
                     --write_tracking=1 --write_tracking_hot_pages=1)
    add_test(NAME platform_posix_signal_ww_auto
             COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_signal_ww_auto"
                     --write_tracking=1)
    add_test(NAME platform_posix_hot
             COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_hot"
                     --write_tracking_hot_pages=1)
    add_test(NAME platform_posix_no_tracking
             COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}/platform_posix_no_tracking" --write_tracking=2)
    # what the tracking costs on this host, with no game (bench_ww): page protection without and with hot pages, and
    # the host's own way. Its timings are printed; it checks the scans' reports, and that hot pages take fewer faults
    add_test(NAME platform_posix_bench_ww
             COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}" --bench-ww --write_tracking=1)
    add_test(NAME platform_posix_bench_ww_own COMMAND snx-test-platform-posix "${CMAKE_CURRENT_BINARY_DIR}" --bench-ww)
    set_tests_properties(platform_posix_bench_ww platform_posix_bench_ww_own PROPERTIES TIMEOUT 60)
endif()
if(WIN32 AND TARGET SDL3::SDL3)
    # the write watch of the reservation, the kernel's writes included (platform/plat_win32.c)
    add_executable(snx-test-platform-win32 platform_win32.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/plat_sdl.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_map.c" "${PROJECT_SOURCE_DIR}/src/common/platform/file_share.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/file_replace.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread.c"
        "${PROJECT_SOURCE_DIR}/src/common/platform/thread_spawn.c")
    snx_test_target(snx-test-platform-win32 gnu23)
    target_link_libraries(snx-test-platform-win32 PRIVATE SDL3::SDL3 synchronization powrprof)
    # plat_win32.c names its threads and says the system's reasons (snx_thread_name, snx_os_error)
    target_link_libraries(snx-test-platform-win32 PRIVATE scarletnx-common)
    # the protection's tracking has no Windows version: the watch tracks, said; and none, which the scan says. Each in a
    # folder of its own, as ctest may run them at once and each writes, reads and removes the same file
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/platform_win32" "${CMAKE_CURRENT_BINARY_DIR}/platform_win32_protection"
         "${CMAKE_CURRENT_BINARY_DIR}/platform_win32_no_tracking")
    add_test(NAME platform_win32 COMMAND snx-test-platform-win32 "${CMAKE_CURRENT_BINARY_DIR}/platform_win32")
    add_test(NAME platform_win32_protection
             COMMAND snx-test-platform-win32 "${CMAKE_CURRENT_BINARY_DIR}/platform_win32_protection" --write_tracking=1)
    add_test(NAME platform_win32_no_tracking
             COMMAND snx-test-platform-win32 "${CMAKE_CURRENT_BINARY_DIR}/platform_win32_no_tracking" --write_tracking=2)
endif()
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-shader-brx shader_brx.c)
    snx_test_target(snx-test-shader-brx gnu23)
    target_link_libraries(snx-test-shader-brx PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-brx PRIVATE m)
    endif()
    add_test(NAME shader_brx COMMAND snx-test-shader-brx)
    # the Maxwell decoder tools/decodetree makes from src/nx/gpu/shader/sm53.decode
    add_executable(snx-test-shader-decode shader_decode.c)
    snx_test_target(snx-test-shader-decode gnu23)
    target_link_libraries(snx-test-shader-decode PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-decode PRIVATE m)
    endif()
    add_test(NAME shader_decode COMMAND snx-test-shader-decode)
    # dual vertex programs, the static CRS stack and the corpus files (gpu/shader/shader.c, corpus.c)
    add_executable(snx-test-shader-program shader_program.c)
    snx_test_target(snx-test-shader-program gnu23)
    target_link_libraries(snx-test-shader-program PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-program PRIVATE m)
    endif()
    add_test(NAME shader_program COMMAND snx-test-shader-program "${CMAKE_CURRENT_BINARY_DIR}")
    # structured control flow (gpu/shader/structure.c) against random programs' own
    add_executable(snx-test-shader-structure shader_structure.c)
    snx_test_target(snx-test-shader-structure gnu23)
    target_link_libraries(snx-test-shader-structure PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-structure PRIVATE m)
    endif()
    add_test(NAME shader_structure COMMAND snx-test-shader-structure)
    # the SM86 frontend (gpu/shader/sm86.h): the decoder on words made by NAK's encoding rules, the search for
    # programs, and small translations, which spirv-val checks (shader_sm86_test.py, which passes it
    # each module as an argument: cmd.exe refused the quoted command system() gave it)
    # sail/sm86.sail run on values against sm86_eval (cmake/sailgen.cmake's EVAL)
    add_executable(snx-test-shader-sail-sm86 shader_sail_sm86.c)
    snx_test_target(snx-test-shader-sail-sm86 gnu23)
    target_link_libraries(snx-test-shader-sail-sm86 PRIVATE snx_shader)
    include("${PROJECT_SOURCE_DIR}/cmake/sailgen.cmake")
    snx_sailgen_gpu(snx-test-shader-sail-sm86 sm86 EVAL)
    add_test(NAME shader_sail_sm86 COMMAND snx-test-shader-sail-sm86)
    # sail/lut3.sail is what tools/lut3gen.py writes (the shortest program of each truth table of three inputs)
    if(Python3_Interpreter_FOUND)
        add_test(NAME shader_lut3_current COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/lut3gen.py"
                 "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/sail/lut3.sail" --check)
    endif()
    add_executable(snx-test-shader-sm86 shader_sm86.c support/spv_eval.c)
    snx_test_target(snx-test-shader-sm86 gnu23)
    target_link_libraries(snx-test-shader-sm86 PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-sm86 PRIVATE m)
    endif()
    if(Python3_Interpreter_FOUND)
        include("${PROJECT_SOURCE_DIR}/cmake/spirv_tools.cmake")
        add_test(NAME shader_sm86 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/shader_sm86_test.py"
                 --test $<TARGET_FILE:snx-test-shader-sm86> --validator $<TARGET_FILE:snx-spirv-val>)
    else()
        add_test(NAME shader_sm86 COMMAND snx-test-shader-sm86 "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    add_executable(snx-test-shader-direct-memh shader_direct_memh.c)
    snx_test_target(snx-test-shader-direct-memh 23)
    target_link_libraries(snx-test-shader-direct-memh PRIVATE snx_shader)
    add_test(NAME shader_direct_memh COMMAND snx-test-shader-direct-memh)
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
        # tools/vkcmdgen on the registry the build has and on registries of its own: the table's lines for what the
        # renderer met, the same file from a second run, and each registry it refuses, at its line
        if(TARGET snx-vkcmdgen)
            get_property(snx_vk_commands_registry GLOBAL PROPERTY snx_vk_commands_registry)
            add_test(NAME vkcmdgen COMMAND "${CMAKE_COMMAND}" "-DVKCMDGEN=$<TARGET_FILE:snx-vkcmdgen>"
                     "-DREGISTRY=${snx_vk_commands_registry}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/vkcmdgen-test"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/vkcmdgen.cmake")
            # every device extension a renderer can make its device with (each source of src/ that makes one) has its
            # setting @VK_ line in settings.txt, which the bans and the player's on and off go through
            if(Python3_Interpreter_FOUND)
                add_test(NAME vk_extension_settings COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/vk_extension_settings.py" --repo "${PROJECT_SOURCE_DIR}"
                         --registry "${snx_vk_commands_registry}")
            endif()
        endif()
        # tools/areatexgen: SMAA's area and search textures as the distribution's AreaTex.h and SearchTex.h have them,
        # byte for byte, in the headers it writes and in the ones the build wrote
        if(TARGET snx-areatexgen)
            get_property(snx_areatex_h GLOBAL PROPERTY snx_areatex_h)
            get_property(snx_searchtex_h GLOBAL PROPERTY snx_searchtex_h)
            add_test(NAME areatexgen COMMAND "${CMAKE_COMMAND}" "-DAREATEXGEN=$<TARGET_FILE:snx-areatexgen>"
                     "-DBUILT_AREA=${snx_areatex_h}" "-DBUILT_SEARCH=${snx_searchtex_h}"
                     "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/areatexgen-test"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/areatexgen.cmake")
        endif()
        # its output where iOS compiles it: a default-if SNX_IOS value, which needs common.h. Not on Apple hosts, whose
        # own headers read the TargetConditionals.h the test puts in their place
        if(NOT APPLE)
            add_test(NAME settingsgen_ios COMMAND "${CMAKE_COMMAND}" "-DSETTINGSGEN=$<TARGET_FILE:snx-settingsgen>"
                     "-DCC=${CMAKE_C_COMPILER}" "-DSRC=${PROJECT_SOURCE_DIR}/src"
                     "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/settingsgen-ios-test"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/settingsgen_ios.cmake")
        endif()
        if(UNIX) # the opts the test runs are shell scripts
            # cmake/llvm_opt.cmake: the pipelines of an opt of the LLVM's exact version, and what another opt gets
            add_test(NAME llvm_opt COMMAND "${CMAKE_COMMAND}"
                     "-DLLVM_OPT_CMAKE=${PROJECT_SOURCE_DIR}/cmake/llvm_opt.cmake"
                     "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/llvm-opt-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/llvm_opt.cmake")
        endif()
    endif()
    add_executable(snx-test-vulkan-release vulkan_release.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/pending_pages.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-vulkan-release gnu23)
    target_link_libraries(snx-test-vulkan-release PRIVATE scarletnx-common) # release.c's snx_os_error
    add_test(NAME vulkan_release COMMAND snx-test-vulkan-release)
    # the GPU mirror's bookkeeping on the CPU (gpu/vk/mirror.c), with no device
    add_executable(snx-test-mirror-landing mirror_landing.c tracy_stubs.c
                   "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/pending_pages.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-mirror-landing gnu23)
    target_link_libraries(snx-test-mirror-landing PRIVATE snx_nx_flags)
    add_test(NAME mirror_landing COMMAND snx-test-mirror-landing)
    add_executable(snx-test-vulkan-backing vulkan_backing.c)
    snx_test_target(snx-test-vulkan-backing gnu23)
    add_test(NAME vulkan_backing COMMAND snx-test-vulkan-backing)
    add_executable(snx-test-astc-bc-suggest astc_bc_suggest.c)
    snx_test_target(snx-test-astc-bc-suggest c23)
    add_test(NAME astc_bc_suggest COMMAND snx-test-astc-bc-suggest)
    if(TARGET snx_vk_common)
        # where a device's memory is, which the Switch's and the 3DS's paths for guest memory are chosen by
        add_executable(snx-test-vk-memory-model vk_memory_model.c "${volk_SOURCE_DIR}/volk.c")
        set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS "${snx_third_party_flags}")
        snx_test_target(snx-test-vk-memory-model gnu23)
        target_link_libraries(snx-test-vk-memory-model PRIVATE snx_vk_common)
        add_test(NAME vk_memory_model COMMAND snx-test-vk-memory-model)
        # how many pipeline workers there are, and how many of them run at the system's low priority
        add_executable(snx-test-vk-pipeline-workers vk_pipeline_workers.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-pipeline-workers gnu23)
        target_link_libraries(snx-test-vk-pipeline-workers PRIVATE snx_vk_common)
        add_test(NAME vk_pipeline_workers COMMAND snx-test-vk-pipeline-workers)
        # which pipeline cache files go to the driver: its own data, whole, of this device and driver build
        add_executable(snx-test-vk-pipeline-cache-check vk_pipeline_cache_check.c)
        snx_test_target(snx-test-vk-pipeline-cache-check gnu23)
        target_link_libraries(snx-test-vk-pipeline-cache-check PRIVATE snx_vk_common)
        add_test(NAME vk_pipeline_cache_check COMMAND snx-test-vk-pipeline-cache-check)
        # the subgroups a compute pipeline asks for: their size, and whole ones
        add_executable(snx-test-vk-compute-subgroups vk_compute_subgroups.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-compute-subgroups gnu23)
        target_link_libraries(snx-test-vk-compute-subgroups PRIVATE snx_vk_common)
        add_test(NAME vk_compute_subgroups COMMAND snx-test-vk-compute-subgroups)
        if(snx_fsr2_api_dir AND SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY) # snx_fsr2 is src/nx's, added after the tests
            # FSR 2.2.1 (gpu/vk/fsr2) on a real device, with no renderer: a jittered still scene upscaled over 40
            # frames comes out nearer the scene than a bilinear upscale. Skipped (77) without a Vulkan device
            add_executable(snx-test-vk-fsr2 vk_fsr2.c "${volk_SOURCE_DIR}/volk.c")
            snx_test_target(snx-test-vk-fsr2 gnu23)
            target_include_directories(snx-test-vk-fsr2 PRIVATE "${PROJECT_SOURCE_DIR}/src")
            target_link_libraries(snx-test-vk-fsr2 PRIVATE snx_fsr2 snx_vk_common)
            set_target_properties(snx-test-vk-fsr2 PROPERTIES LINKER_LANGUAGE CXX)
            add_test(NAME vk_fsr2 COMMAND snx-test-vk-fsr2)
            set_tests_properties(vk_fsr2 PROPERTIES SKIP_RETURN_CODE 77)
            # a hook module's upscale (the replay of an upscale its CPU hook recorded) through the upscalers' interface
            # (gpu/vk/upscaler) on a real device: the choice among the upscalers, the spatial one sharpened or not, the
            # copy, none for FSR 2 without depth, and FSR 2 through the interface. Skipped (77) without a device
            add_executable(snx-test-vk-hook-upscale vk_hook_upscale.c "${volk_SOURCE_DIR}/volk.c")
            snx_test_target(snx-test-vk-hook-upscale gnu23)
            add_dependencies(snx-test-vk-hook-upscale snx_settings)
            target_include_directories(snx-test-vk-hook-upscale PRIVATE "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_BINARY_DIR}/settingsgen")
            target_link_libraries(snx-test-vk-hook-upscale PRIVATE snx_upscaler snx_vk_common m)
            set_target_properties(snx-test-vk-hook-upscale PROPERTIES LINKER_LANGUAGE CXX)
            add_test(NAME vk_hook_upscale COMMAND snx-test-vk-hook-upscale)
            set_tests_properties(vk_hook_upscale PROPERTIES SKIP_RETURN_CODE 77)
        endif()
        # a compute program's layouts and what its descriptor set is written with, for every frontend's dispatches
        add_executable(snx-test-vk-compute-program vk_compute_program.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-compute-program gnu23)
        target_link_libraries(snx-test-vk-compute-program PRIVATE snx_vk_common)
        add_test(NAME vk_compute_program COMMAND snx-test-vk-compute-program)
        # the presentation the vsync setting picks, for every frontend's swapchain
        add_executable(snx-test-vk-present-mode vk_present_mode.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-present-mode gnu23)
        target_link_libraries(snx-test-vk-present-mode PRIVATE snx_vk_common)
        add_test(NAME vk_present_mode COMMAND snx-test-vk-present-mode)
        # a game's own HDR output: what a window's frames hold by its textures' format, and the swapchain image for
        # each kind from a surface's formats
        add_executable(snx-test-hdr-output hdr_output.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/hdr_swapchain.c"
            "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-hdr-output gnu23)
        target_include_directories(snx-test-hdr-output PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(snx-test-hdr-output PRIVATE snx_vk_common m)
        add_test(NAME hdr_output COMMAND snx-test-hdr-output)
        # the submission ring's uploads run before the recording they belong to, on whatever device there is
        add_executable(snx-test-vk-ring-uploads vk_ring_uploads.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-ring-uploads gnu23)
        target_link_libraries(snx-test-vk-ring-uploads PRIVATE snx_vk_common)
        add_test(NAME vk_ring_uploads COMMAND snx-test-vk-ring-uploads)
        set_tests_properties(vk_ring_uploads PROPERTIES SKIP_RETURN_CODE 77)
        # a window's notice copied over its image after the picture's transfers (common/gpu/vk/notice.c), on whatever
        # device there is
        add_executable(snx-test-vk-notice vk_notice.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-notice c23)
        target_link_libraries(snx-test-vk-notice PRIVATE snx_vk_common)
        add_test(NAME vk_notice COMMAND snx-test-vk-notice)
        set_tests_properties(vk_notice PROPERTIES SKIP_RETURN_CODE 77)
        # the on-screen controls' rectangles over a window's image (common/gpu/vk/rects.c), on whatever device there is
        add_executable(snx-test-vk-rects vk_rects.c "${volk_SOURCE_DIR}/volk.c")
        snx_test_target(snx-test-vk-rects c23)
        target_link_libraries(snx-test-vk-rects PRIVATE snx_vk_common snx_vpad)
        add_test(NAME vk_rects COMMAND snx-test-vk-rects)
        set_tests_properties(vk_rects PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(snx-test-vk-uma-budget vk_uma_budget.c)
    snx_test_target(snx-test-vk-uma-budget c23)
    add_test(NAME vk_uma_budget COMMAND snx-test-vk-uma-budget)
    # a range of pages as the bits it takes of each word of the page model's tables (gpu/vk/uma_pages.h)
    add_executable(snx-test-vk-uma-pages vk_uma_pages.c)
    snx_test_target(snx-test-vk-uma-pages c23)
    add_test(NAME vk_uma_pages COMMAND snx-test-vk-uma-pages)
    # vk_gpu_profile as a device runs it: 0 on a portability driver (src/nx/gpu/vk/gpuprof_setting.h)
    add_executable(snx-test-vk-gpuprof-setting vk_gpuprof_setting.c)
    snx_test_target(snx-test-vk-gpuprof-setting c23)
    add_test(NAME vk_gpuprof_setting COMMAND snx-test-vk-gpuprof-setting)
    # the time between two of a queue's timestamps, the later read first and the counter's wrap (gpuprof_time.h)
    add_executable(snx-test-vk-gpuprof-time vk_gpuprof_time.c)
    snx_test_target(snx-test-vk-gpuprof-time c23)
    add_test(NAME vk_gpuprof_time COMMAND snx-test-vk-gpuprof-time)
    # a gap of the graphics queue split as vk_gpu_profile reports it (src/nx/gpu/vk/gpuprof_gap.h)
    add_executable(snx-test-vk-gpuprof-gap vk_gpuprof_gap.c)
    snx_test_target(snx-test-vk-gpuprof-gap c23)
    add_test(NAME vk_gpuprof_gap COMMAND snx-test-vk-gpuprof-gap)
    add_executable(snx-test-vk-descriptor-key vk_descriptor_key.c)
    snx_test_target(snx-test-vk-descriptor-key gnu23)
    target_link_libraries(snx-test-vk-descriptor-key PRIVATE Vulkan::Headers)
    add_test(NAME vk_descriptor_key COMMAND snx-test-vk-descriptor-key)
    add_executable(snx-test-vk-descriptor-addresses vk_descriptor_addresses.c)
    snx_test_target(snx-test-vk-descriptor-addresses c23)
    target_link_libraries(snx-test-vk-descriptor-addresses PRIVATE Vulkan::Headers)
    add_test(NAME vk_descriptor_addresses COMMAND snx-test-vk-descriptor-addresses)
    add_executable(snx-test-vk-indexed-handles vk_indexed_handles.c)
    snx_test_target(snx-test-vk-indexed-handles c23)
    add_test(NAME vk_indexed_handles COMMAND snx-test-vk-indexed-handles)
    add_executable(snx-test-vk-texture-arrays vk_texture_arrays.c)
    snx_test_target(snx-test-vk-texture-arrays c23)
    add_test(NAME vk_texture_arrays COMMAND snx-test-vk-texture-arrays)
    add_executable(snx-test-vk-cb-windows vk_cb_windows.c)
    snx_test_target(snx-test-vk-cb-windows c23)
    add_test(NAME vk_cb_windows COMMAND snx-test-vk-cb-windows)
    add_executable(snx-test-vk-inline-bank vk_inline_bank.c)
    snx_test_target(snx-test-vk-inline-bank c23)
    add_test(NAME vk_inline_bank COMMAND snx-test-vk-inline-bank)
    add_executable(snx-test-vk-tess-passthrough vk_tess_passthrough.c
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/tess_passthrough.c"
        "${PROJECT_SOURCE_DIR}/src/common/utilities/spirv_emit.c")
    snx_test_target(snx-test-vk-tess-passthrough gnu23)
    target_include_directories(snx-test-vk-tess-passthrough PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-tess-passthrough PRIVATE Vulkan::Headers)
    add_test(NAME vk_tess_passthrough COMMAND snx-test-vk-tess-passthrough)
    # the shader lookups' tables hold a frame's keys (src/nx/gpu/vk/lookup_ways.h)
    add_executable(snx-test-vk-lookup-ways vk_lookup_ways.c)
    snx_test_target(snx-test-vk-lookup-ways gnu23)
    add_test(NAME vk_lookup_ways COMMAND snx-test-vk-lookup-ways)
    add_executable(snx-test-vk-sample-locations vk_sample_locations.c)
    snx_test_target(snx-test-vk-sample-locations gnu23)
    target_link_libraries(snx-test-vk-sample-locations PRIVATE Vulkan::Headers)
    add_test(NAME vk_sample_locations COMMAND snx-test-vk-sample-locations)
    add_executable(snx-test-vk-coverage vk_coverage.c)
    snx_test_target(snx-test-vk-coverage gnu23)
    add_test(NAME vk_coverage COMMAND snx-test-vk-coverage)
    add_executable(snx-test-vk-coverage-factors vk_coverage_factors.c)
    snx_test_target(snx-test-vk-coverage-factors gnu23)
    add_test(NAME vk_coverage_factors COMMAND snx-test-vk-coverage-factors)
    add_executable(snx-test-vk-descriptor-encoding vk_descriptor_encoding.c)
    snx_test_target(snx-test-vk-descriptor-encoding gnu23)
    target_link_libraries(snx-test-vk-descriptor-encoding PRIVATE Vulkan::Headers)
    add_test(NAME vk_descriptor_encoding COMMAND snx-test-vk-descriptor-encoding)
    add_executable(snx-test-vk-descriptor-heaps vk_descriptor_heaps.c)
    snx_test_target(snx-test-vk-descriptor-heaps gnu23)
    target_include_directories(snx-test-vk-descriptor-heaps PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-descriptor-heaps PRIVATE Vulkan::Headers)
    add_test(NAME vk_descriptor_heaps COMMAND snx-test-vk-descriptor-heaps)
    add_executable(snx-test-vk-descriptor-heap-batch vk_descriptor_heap_batch.c)
    snx_test_target(snx-test-vk-descriptor-heap-batch gnu23)
    target_include_directories(snx-test-vk-descriptor-heap-batch PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-descriptor-heap-batch PRIVATE Vulkan::Headers)
    add_test(NAME vk_descriptor_heap_batch COMMAND snx-test-vk-descriptor-heap-batch)
    add_executable(snx-test-vk-descriptor-objects vk_descriptor_objects.c
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/descriptor_objects.c")
    snx_test_target(snx-test-vk-descriptor-objects gnu23)
    target_include_directories(snx-test-vk-descriptor-objects PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-descriptor-objects PRIVATE Vulkan::Headers)
    add_test(NAME vk_descriptor_objects COMMAND snx-test-vk-descriptor-objects)
    add_executable(snx-test-vk-checkpoints vk_checkpoints.c)
    snx_test_target(snx-test-vk-checkpoints gnu23)
    target_include_directories(snx-test-vk-checkpoints PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-checkpoints PRIVATE Vulkan::Headers)
    add_test(NAME vk_checkpoints COMMAND snx-test-vk-checkpoints)
    # the ranges a constant buffer, a root table and a storage copy are bound with (gpu/vk/vk.h vk_cb_range,
    # vk_sb_round): whole robustBufferAccess2 steps, at least one, within maxUniformBufferRange
    add_executable(snx-test-vk-robust-ranges vk_robust_ranges.c)
    snx_test_target(snx-test-vk-robust-ranges gnu23)
    target_include_directories(snx-test-vk-robust-ranges PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-robust-ranges PRIVATE Vulkan::Headers)
    add_test(NAME vk_robust_ranges COMMAND snx-test-vk-robust-ranges)
    # the renderer's device memory (gpu/vk/devmem.c) over a driver the test plays, which checks each call as the
    # specification reads: alignment, memory types, no byte and no granularity page shared, memory made for a
    # resource alone, whole atoms flushed; the cap of the GPU's own heap, and requests from several threads
    add_executable(snx-test-vk-devmem vk_devmem.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/space.c")
    snx_test_target(snx-test-vk-devmem gnu23)
    target_include_directories(snx-test-vk-devmem PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-devmem PRIVATE Vulkan::Headers scarletnx-common) # snx_thread_name
    add_test(NAME vk_devmem COMMAND snx-test-vk-devmem)
    # the constant buffer slots bound as dynamic uniform buffers (gpu/vk/cb_dynamic.c), from one run's moves to the next
    add_executable(snx-test-vk-cb-dynamic vk_cb_dynamic.c)
    snx_test_target(snx-test-vk-cb-dynamic gnu23)
    target_compile_definitions(snx-test-vk-cb-dynamic PRIVATE VK_NO_PROTOTYPES)
    target_include_directories(snx-test-vk-cb-dynamic PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-cb-dynamic PRIVATE Vulkan::Headers)
    add_test(NAME vk_cb_dynamic COMMAND snx-test-vk-cb-dynamic)
    # the trim of the caches (gpu/vk/surface.c vk_mem_trim) over stubs of the rest of the renderer: when it takes the
    # render targets that later ones drew all over, where the driver moves memory by priority and where it does not
    add_executable(snx-test-vk-mem-trim vk_mem_trim.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/addr_gens.c")
    snx_test_target(snx-test-vk-mem-trim gnu23)
    target_compile_definitions(snx-test-vk-mem-trim PRIVATE VK_NO_PROTOTYPES)
    target_include_directories(snx-test-vk-mem-trim PRIVATE "${volk_SOURCE_DIR}" "${renderdoc_app_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-mem-trim PRIVATE Vulkan::Headers)
    add_test(NAME vk_mem_trim COMMAND snx-test-vk-mem-trim)
    add_executable(snx-test-vk-descriptor-heap-setup vk_descriptor_heap_setup.c
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/descriptor_heaps.c")
    snx_test_target(snx-test-vk-descriptor-heap-setup gnu23)
    target_include_directories(snx-test-vk-descriptor-heap-setup PRIVATE "${volk_SOURCE_DIR}")
    target_link_libraries(snx-test-vk-descriptor-heap-setup PRIVATE Vulkan::Headers)
    add_test(NAME vk_descriptor_heap_setup COMMAND snx-test-vk-descriptor-heap-setup)
    add_executable(snx-test-vk-advanced-blend vk_advanced_blend.c)
    snx_test_target(snx-test-vk-advanced-blend gnu23)
    target_link_libraries(snx-test-vk-advanced-blend PRIVATE Vulkan::Headers)
    add_test(NAME vk_advanced_blend COMMAND snx-test-vk-advanced-blend)
    add_executable(snx-test-shader-advanced-blend shader_advanced_blend.c
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/advanced_blend_spirv.c"
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/capture_tiles_spirv.c"
        "${PROJECT_SOURCE_DIR}/src/common/utilities/spirv_emit.c")
    snx_test_target(snx-test-shader-advanced-blend gnu23)
    add_test(NAME shader_advanced_blend COMMAND snx-test-shader-advanced-blend)
endif()
# these recompile with --repo of the checkout, whose cache folder (the extractions' work/) is the user's: a folder of
# the build tree's instead, where XDG names it
foreach(snx_test native_recompiler llvm_module nro_modules plt_calls nx_runtime_homebrew nx_runtime_homebrew_64k_pages)
    if(TEST ${snx_test})
        set_property(TEST ${snx_test} APPEND PROPERTY ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg-cache")
    endif()
endforeach()
# recompiler_backends on a host that has no native back end, or for words that it leaves to LLVM: nothing to compare
get_property(snx_tests DIRECTORY PROPERTY TESTS)
foreach(snx_test IN LISTS snx_tests)
    if(snx_test MATCHES "^recompiler_backends(_|$)")
        set_property(TEST ${snx_test} PROPERTY SKIP_RETURN_CODE 77)
    endif()
endforeach()
# the tests that Unicorn runs a native back end's code for: ctest -L unicorn, each with its machine's label too
# (scripts/unicorn-tests.py, which counts what each machine ran)
if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
    get_property(snx_tests DIRECTORY PROPERTY TESTS)
    foreach(snx_test IN LISTS snx_tests)
        if(snx_test MATCHES "^(sail|recompiler|arm)_(arm64win|arm64|rv64|ppc64be|ppc64|mips64be|mips64|la64|s390x)(_|$)")
            set_property(TEST ${snx_test} APPEND PROPERTY LABELS unicorn ${CMAKE_MATCH_2})
        elseif(snx_test MATCHES "^module_desc_(arm64win|arm64|riscv64|ppc64le|mips64el|loongarch64)$")
            set(snx_machine ${CMAKE_MATCH_1})
            string(REGEX REPLACE "^riscv64$" "rv64" snx_machine "${snx_machine}")
            string(REGEX REPLACE "^loongarch64$" "la64" snx_machine "${snx_machine}")
            string(REGEX REPLACE "(le|el)$" "" snx_machine "${snx_machine}")
            set_property(TEST ${snx_test} APPEND PROPERTY LABELS unicorn ${snx_machine})
        elseif(snx_test STREQUAL "arm64_unwind")
            set_property(TEST ${snx_test} APPEND PROPERTY LABELS unicorn arm64win)
        endif()
    endforeach()
endif()
snx_sanitizer_path()
# the tests with a fatal of their own that build runtime code which stops with a kind of error (platform/log.h's
# fatal_as, fatal_given): those through their fatal (support/fatal_kind.c)
foreach(t arm-code-invalidate audio-clock audio-opus crash-runtime-thread fs-service gl-layer gvk-marshal misc-service
          nn-fs nn-time options-tv-mode os-light-event room-service rtapi-miss runtime-linker sm-service)
    if(TARGET snx-test-${t})
        target_sources(snx-test-${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support/fatal_kind.c")
    endif()
endforeach()
