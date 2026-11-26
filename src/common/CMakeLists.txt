# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# What the recompiler's core (src/common/cpu/recompiler/CMakeLists.txt) takes from this folder: the A64 decoder, the
# function discovery, the 128-bit compare-and-swap, and the host calls they and the backends make (utilities/base.c's
# allocation, error line, files and folders; the platform's clocks, address space and file replace). A library of its
# own so the recompiler's archive holds these objects and not the rest of common and platform, which link it for them.
# The C library and the system's alone, as tests/recompiler_symbols.cmake checks of the archive
add_library(snx_recompiler_base STATIC utilities/base.c cpu/aarch64/a64_decoder.c cpu/recompiler/codegen.c
    runtime/cas128.c platform/time.c platform/vm.c platform/file_replace.c)
# position-independent, as the platform is: a shared library that links the platform takes these objects too
set_target_properties(snx_recompiler_base PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(snx_recompiler_base PRIVATE _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(platform/time.c platform/vm.c platform/file_replace.c PROPERTIES
                            COMPILE_DEFINITIONS _FILE_OFFSET_BITS=64)
target_compile_options(snx_recompiler_base PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_recompiler_base PUBLIC Threads::Threads)
# RtAPI.cas128's compare-and-swap is cmpxchg16b with cx16, and a libatomic call without, which nothing here links
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set_source_files_properties(runtime/cas128.c PROPERTIES COMPILE_OPTIONS -mcx16)
endif()
# the A64 decoder (cpu/aarch64/a64_decoder.c) tools/decodetree generates from cpu/aarch64/decode/
include("${PROJECT_SOURCE_DIR}/cmake/decodetree.cmake")
snx_decodetree_a64(snx_recompiler_base)
if(WIN32)
    # UTF-8 paths for the W calls (platform/wpath.c); a temporary folder's rights (utilities/base.c)
    target_sources(snx_recompiler_base PRIVATE platform/wpath.c)
    target_link_libraries(snx_recompiler_base PUBLIC advapi32)
endif()

add_library(scarletnx-common STATIC
    utilities/common.c utilities/process.c utilities/font.c utilities/ui_quads.c utilities/ini_lines.c utilities/config_file.c utilities/game_ini.c utilities/program_start.c utilities/pad_map.c
    utilities/pad_screen.c utilities/users.c utilities/mii_model.c utilities/mii_gltf.c utilities/mii_db.c utilities/logscan.c utilities/language_tag.c utilities/mods.c utilities/patches.c utilities/cheat_list.c utilities/state_header.c
    utilities/gpu_driver.c utilities/gpu_driver_stage.c utilities/elf_dynamic.c utilities/run_mark.c utilities/cli.c
    utilities/spirv_emit.c utilities/spirv_opt.c utilities/script.c utilities/pace.c memory/host.c memory/jemalloc.c
    cpu/recompiler/variants.c runtime/linux_signal.c)
target_compile_definitions(scarletnx-common PRIVATE _CRT_SECURE_NO_WARNINGS
    SNX_HOST_COMPILER="${CMAKE_C_COMPILER}"
    SNX_CPU_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/cpu/aarch64"
    SNX_SOURCE_INCLUDE="${PROJECT_SOURCE_DIR}/src")
# memory/jemalloc.c reads the heap jemalloc's mallctl keeps (SNX_JEMALLOC, cmake/jemalloc.cmake): the program links it,
# the unit tests do not, and the object comes in only where snx_heap_used is called
if(SNX_JEMALLOC AND NOT ANDROID)
    target_compile_definitions(scarletnx-common PRIVATE SNX_JEMALLOC=1)
endif()
target_compile_options(scarletnx-common PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-common PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(scarletnx-common PUBLIC scarletnx-platform) # the host's threads and files, for what links it
# the A64 decoder, the function discovery, and the allocation, error line, files and folders of common.h
target_link_libraries(scarletnx-common PUBLIC snx_recompiler_base)
target_link_libraries(scarletnx-common PRIVATE snx_yyjson) # the GPU driver packages' meta.json (utilities/gpu_driver.c)
target_link_libraries(scarletnx-common PRIVATE snx_libdeflate) # a control.nacp's titles in deflate (utilities/common.c)
target_link_libraries(scarletnx-common PRIVATE snx_cgltf) # a glTF model's Mii parts (utilities/mii_gltf.c)
# the menus' icons, beside the VGA font (utilities/font.c): the launcher's font and the game menu's atlas draw them
include("${PROJECT_SOURCE_DIR}/cmake/gui_icons.cmake")
target_sources(scarletnx-common PRIVATE "${snx_gui_icons_c}")
if(WIN32)
    target_link_libraries(scarletnx-common PUBLIC advapi32)
else()
    target_link_libraries(scarletnx-common PUBLIC m)
endif()

# the GL layer that translates a guest's GL to Vulkan, shared by the Android bridge and the console (gpu/gl/gl_state.h):
# its state machine, flat records a save state holds as they are; glslc compiles its shaders (gpu/gl/gl_program.h)
add_library(snx_gl STATIC gpu/gl/gl_state.c gpu/gl/gl_program.c gpu/gl/gl_object.c gpu/gl/gl_format.c
                           gpu/gl/gl_texture.c gpu/gl/gl_framebuffer.c
                           gpu/gl/gl_sync.c)
set_target_properties(snx_gl PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_gl PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                      -Werror=missing-variable-declarations)
target_include_directories(snx_gl PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(snx_gl PRIVATE snx_glslc)

# snx_demangle (utilities/demangle.h): C++ names through the C++ runtime's demangler, for the log summary. C, but what
# links it links the C++ runtime (LINKER_LANGUAGE CXX)
add_library(snx_demangle STATIC utilities/demangle.c)
set_target_properties(snx_demangle PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_demangle PRIVATE -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes)
# snx-logscan: the log summary (utilities/logscan.h) as a program, as scarletnx --scan-log, for the game reports
# workflow and the tests: only the C library and the demangler. Not for the phones, which run no programs of their own
if(NOT ANDROID AND NOT IOS)
    add_executable(snx-logscan utilities/logscan_main.c utilities/logscan.c)
    set_target_properties(snx-logscan PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                                 LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-logscan PRIVATE -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes)
    target_link_libraries(snx-logscan PRIVATE snx_demangle)
endif()

# The setup a first start goes through (utilities/setup.h), the launcher's and the driver's: OpenSSL for the first
# user's random uid
if(TARGET OpenSSL::Crypto)
    add_library(snx_setup STATIC utilities/setup.c)
    target_compile_options(snx_setup PRIVATE -Wall -Wextra -Werror=missing-prototypes)
    target_compile_definitions(snx_setup PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(snx_setup PUBLIC scarletnx-common OpenSSL::Crypto)
endif()

# The host's thread primitives (platform/thread.h) that every driver shares: mutexes, condition variables, waits on
# a word, thread ids; the clocks and sleep (platform/time.h); the files (platform/file.h); the address space
# (platform/vm.h); and shared libraries (platform/dl.h). No guest type, setting or SDL enters this target. The clocks,
# the address space and a file's replace are snx_recompiler_base's, which the recompiler's core calls too
add_library(scarletnx-platform STATIC platform/thread.c platform/thread_spawn.c platform/sleep.c platform/file.c
    platform/file_map.c platform/file_share.c platform/dl.c)
set_target_properties(scarletnx-platform PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(scarletnx-platform PRIVATE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)
target_compile_options(scarletnx-platform PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-platform PUBLIC Threads::Threads ${CMAKE_DL_LIBS} snx_recompiler_base)
if(WIN32)
    target_link_libraries(scarletnx-platform PUBLIC synchronization) # plat_wait_word's WaitOnAddress
endif()

# Bluetooth LE adverts a scan hears (platform/ble_scan.h): the scan over its backend, the fake backend and the Joy-Con
# 2's advert, no OS. The system's backend is a library of its own, snx_ble_system, which only the launcher links (the
# tests link a fake in its place, tests/support/ble_fake_system.c): BlueZ over libsystemd's sd-bus, opened at run time,
# on Linux; none elsewhere yet (Windows' designed in ble_none.c)
add_library(snx_ble STATIC platform/ble_scan.c)
set_target_properties(snx_ble PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ble PRIVATE -Wall -Wextra -Werror=missing-prototypes)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(snx_ble_system STATIC platform/ble_bluez.c)
else()
    add_library(snx_ble_system STATIC platform/ble_none.c)
endif()
set_target_properties(snx_ble_system PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ble_system PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_ble_system PUBLIC snx_ble scarletnx-platform)

# The same indexed host filesystem and shared immutable file views (platform/file.h) serve NX
# and CTR. No guest ABI or runtime/platform dependency enters this target.
add_library(scarletnx-filesystem STATIC filesystem/tree.c)
set_target_properties(scarletnx-filesystem PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(scarletnx-filesystem PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)
target_compile_options(scarletnx-filesystem PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-filesystem PUBLIC scarletnx-platform Threads::Threads)

# The launcher's HTTPS client (network/https.h) through libcurl, trusting Mozilla's roots compiled in: where the build
# has OpenSSL, which libcurl's TLS is (cmake/dependencies.cmake)
if(TARGET CURL::libcurl)
    add_library(snx_https STATIC network/https.c)
    target_compile_options(snx_https PRIVATE -Wall -Wextra -Werror=missing-prototypes)
    target_link_libraries(snx_https PRIVATE CURL::libcurl snx_cacert)
endif()

# The emulated network stack (network/net.h), shared by nx's bsd and sfdnsres and the GNU ABI's socket syscalls. A
# library of its own so the ISO C tools don't link Winsock
add_library(scarletnx-net STATIC network/net.c network/net_icmp.c network/net_event.c network/net_dns.c
    network/net_iface.c)
target_compile_options(scarletnx-net PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-net PUBLIC scarletnx-platform Threads::Threads)
if(WIN32)
    target_link_libraries(scarletnx-net PUBLIC ws2_32 iphlpapi)
elseif(APPLE) # the system resolver's DNS servers (network/net_iface.c)
    target_link_libraries(scarletnx-net PUBLIC resolv)
endif()

# The DS's and the 3DS's online services through their communities' replacements (online/, its README.md): the WFC
# services and the DS firmware's connection slot, a DNS resolver, NASC's login on Pretendo Network and the 3DS
# console's files. Strict ISO C23 on the host's own sockets
if(SNX_BUILD_NDS OR SNX_BUILD_3DS OR BUILD_TESTING)
    add_library(snx_online STATIC online/wfc.c online/dns.c online/resolve.c online/socket.c online/nasc.c
        online/ctr.c online/hosts.c)
    set_target_properties(snx_online PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
    target_compile_options(snx_online PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
    if(WIN32)
        target_link_libraries(snx_online PUBLIC ws2_32)
    endif()
endif()

# Local wireless for nx's ldn and bsd (network/net_ldn.h): over the LAN, or through an Eden room (network/net_room.h),
# which is ENet and zstd, fetched for the NX build and the tests
if(TARGET snx_enet)
    add_library(scarletnx-ldn STATIC network/net_ldn.c network/net_room.c)
    target_compile_options(scarletnx-ldn PRIVATE -Wall -Wextra -Werror=missing-prototypes)
    target_link_libraries(scarletnx-ldn PUBLIC scarletnx-net PRIVATE snx_enet snx_zstd)
endif()

# The zip reader (utilities/zip.h) of the Vita driver's VPKs and the Android driver's APKs
if(TARGET snx_libdeflate)
    add_library(snx_zip STATIC utilities/zip.c)
    target_link_libraries(snx_zip PUBLIC scarletnx-common PRIVATE snx_libdeflate)
    target_compile_options(snx_zip PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endif()
# A save state's file (state_file.h), every console's: the Switch's runtime plots its counts in its profiler
if(TARGET snx_zstd)
    add_library(snx_state_file STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/state_file.c")
    target_link_libraries(snx_state_file PUBLIC scarletnx-common PRIVATE snx_zstd)
    target_compile_options(snx_state_file PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endif()
include("${PROJECT_SOURCE_DIR}/cmake/vpad.cmake") # the on-screen controls (snx_vpad)
# What the Vulkan frontends (src/nx/gpu/vk, src/3ds/gpu/vk, src/vita/gpu) share of a device: the instance, queries,
# the one way a renderer makes its device (gpu/vk/device.c), the drivers known to break an extension or feature (gpu/vk/extension_bans.txt, compiled in),
# the commands each extension requires (generated from the registry, cmake/vk_extension_commands.cmake), the submission
# ring, VK_EXT_shader_object's shader binaries and dynamic state, the swapchain and the SDL window (gpu/vk/). Each
# frontend compiles volk.c, whose functions these call
if(TARGET SDL3::SDL3)
    include("${PROJECT_SOURCE_DIR}/cmake/notice.cmake") # the windows' notices (snx_notice)
endif()
if(TARGET Vulkan::Headers AND TARGET SDL3::SDL3)
    include("${PROJECT_SOURCE_DIR}/cmake/vk_extension_bans.cmake")
    include("${PROJECT_SOURCE_DIR}/cmake/vk_extension_commands.cmake")
    add_library(snx_vk_common STATIC gpu/vk/vk_common.c gpu/vk/device.c gpu/vk/driver_tag.c gpu/vk/extension_bans.c
                                     "${PROJECT_BINARY_DIR}/generated/vk_extension_bans_text.c"
                                     gpu/vk/extension_commands.c "${snx_vk_extension_commands_c}" gpu/vk/ring.c
                                     gpu/vk/shader_object.c gpu/vk/swapchain.c gpu/vk/window.c gpu/vk/notice.c
                                     gpu/vk/rects.c gpu/vk/present_fx.c gpu/vk/fx/fx_fsr.c gpu/vk/gpu_pstate.c)
    set_target_properties(snx_vk_common PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    # the GPU's clocks held while a game runs (gpu/vk/gpu_pstate.h): the kernel's interface, which the strict standard
    # of the files above hides, as it does POSIX's open and ioctl
    set_source_files_properties(gpu/vk/gpu_pstate.c PROPERTIES COMPILE_OPTIONS "-std=gnu23")
    target_compile_options(snx_vk_common PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                                 -Werror=missing-variable-declarations)
    target_compile_definitions(snx_vk_common PRIVATE VK_NO_PROTOTYPES)
    target_include_directories(snx_vk_common PUBLIC "${volk_SOURCE_DIR}")
    # scarletnx-common: on macOS the window names the Vulkan driver beside the program (snx_executable,
    # snx_vulkan_driver)
    target_link_libraries(snx_vk_common PUBLIC Vulkan::Headers SDL3::SDL3 scarletnx-common)
    target_link_libraries(snx_vk_common PUBLIC snx_notice) # a window's notice copied over its picture (gpu/vk/notice.c)
    # the spatial upscalers and the anti-aliasing (gpu/vk/present_fx.c, gpu/vk/fx/)
    include("${PROJECT_SOURCE_DIR}/cmake/glsl.cmake")
    foreach(pair IN ITEMS "fsr.vert=fx_fsr_vert" "copy.frag=fx_copy"
                          "fsr_easu.frag=fx_fsr_easu" "fsr_rcas.frag=fx_fsr_rcas" "fxaa.frag=fx_fxaa"
                          "smaa_edge.frag=fx_smaa_edge" "smaa_weights.frag=fx_smaa_weights"
                          "smaa_blend.frag=fx_smaa_blend")
        string(REPLACE "=" ";" parts "${pair}")
        list(GET parts 0 shader)
        list(GET parts 1 symbol)
        snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/fx/${shader} NAME ${symbol}_spv
                       HEADER common/gpu/vk/fx_spv.h)
    endforeach()
    # FSR 1's fp16 builds of the same two passes, where the device's shaderFloat16 runs them
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/fx/fsr_easu.frag NAME fx_fsr_easu_fp16_spv
                   HEADER common/gpu/vk/fx_spv.h DEFINES A_HALF=1)
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/fx/fsr_rcas.frag NAME fx_fsr_rcas_fp16_spv
                   HEADER common/gpu/vk/fx_spv.h DEFINES A_HALF=1)
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/fx/nis.comp NAME fx_nis_spv HEADER common/gpu/vk/fx_spv.h)
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/fx/nis.comp NAME fx_nis8_spv HEADER common/gpu/vk/fx_spv.h
                   DEFINES NIS_OUTPUT_RGBA8)
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/fx/sgsr1.vert NAME fx_sgsr_vert_spv HEADER common/gpu/vk/fx_spv.h)
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/fx/sgsr1.frag NAME fx_sgsr_frag_spv HEADER common/gpu/vk/fx_spv.h)
    # SMAA's area and search textures, which tools/areatexgen writes (cmake/areatex.cmake)
    include("${PROJECT_SOURCE_DIR}/cmake/areatex.cmake")
    target_sources(snx_vk_common PRIVATE "${snx_areatex_h}" "${snx_searchtex_h}")
    # a notice's band drawn in a window's rendering (gpu/vk/notice.c), over the effects' fullscreen triangle
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/notice.frag NAME notice_frag_spv HEADER common/gpu/vk/notice_spv.h)
    # the on-screen controls' rectangles over a window's picture (gpu/vk/rects.c)
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/rects.vert NAME rects_vert_spv HEADER common/gpu/vk/rects_spv.h)
    snx_glsl_spirv(snx_vk_common SOURCE gpu/vk/rects.frag NAME rects_frag_spv HEADER common/gpu/vk/rects_spv.h)
    # The GPU driver a run asks for (gpu/vk/gpu_driver_load.h), which the window's instance and surface are made through:
    # the Switch runtime's and the Vita's share this one loader. It reads the environment and the process's maps, which
    # the strict standard of the files above hides, so its files are compiled as the runtimes' are
    target_sources(snx_vk_common PRIVATE gpu/vk/gpu_driver_load.c)
    set(snx_gpu_driver_sources gpu/vk/gpu_driver_load.c)
    if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
        # packages' one ABI: the linker namespaces the package's driver loads in (gpu/vk/gpu_driver_android.c)
        target_sources(snx_vk_common PRIVATE gpu/vk/gpu_driver_android.c)
        list(APPEND snx_gpu_driver_sources gpu/vk/gpu_driver_android.c)
    endif()
    set_source_files_properties(${snx_gpu_driver_sources} PROPERTIES COMPILE_OPTIONS "-std=gnu23")
    if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
        # the two calls Android's Vulkan loader opens its driver with, for the copy of it a run with a GPU driver
        # package loads (gpu/vk/gpu_driver_hook.h): -z global is what puts them before the system's
        add_library(snx_gpu_driver_hook SHARED gpu/vk/gpu_driver_hook.c)
        target_include_directories(snx_gpu_driver_hook PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_options(snx_gpu_driver_hook PRIVATE -Wall -Wextra -Werror=missing-prototypes)
        target_link_options(snx_gpu_driver_hook PRIVATE LINKER:-z,global)
        set_target_properties(snx_gpu_driver_hook PROPERTIES OUTPUT_NAME scarletnx-gpu-hook)
        include(GNUInstallDirs)
        install(TARGETS snx_gpu_driver_hook LIBRARY DESTINATION lib/${ANDROID_ABI})
        install(FILES gpu/vk/ADRENOTOOLS-NOTICE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    endif()
endif()
