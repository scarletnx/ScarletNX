# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(scarletnx-common STATIC
    utilities/common.c utilities/process.c utilities/font.c utilities/ini_lines.c utilities/game_ini.c utilities/program_start.c utilities/pad_map.c
    utilities/pad_screen.c utilities/users.c utilities/mii_model.c utilities/mii_gltf.c utilities/mii_db.c utilities/logscan.c utilities/language_tag.c utilities/mods.c utilities/patches.c utilities/cheat_list.c utilities/state_header.c
    utilities/gpu_driver.c utilities/gpu_driver_stage.c utilities/elf_dynamic.c utilities/run_mark.c utilities/cli.c
    utilities/spirv_emit.c utilities/spirv_opt.c utilities/script.c memory/host.c memory/jemalloc.c
    cpu/aarch64/a64_decoder.c cpu/recompiler/codegen.c cpu/recompiler/variants.c runtime/cas128.c runtime/linux_signal.c)
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
# RtAPI.cas128's compare-and-swap is cmpxchg16b with cx16, and a libatomic call without, which nothing here links
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set_source_files_properties(runtime/cas128.c PROPERTIES COMPILE_OPTIONS -mcx16)
endif()
target_link_libraries(scarletnx-common PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(scarletnx-common PUBLIC scarletnx-platform) # the clocks (snx_seconds)
target_link_libraries(scarletnx-common PRIVATE snx_yyjson) # the GPU driver packages' meta.json (utilities/gpu_driver.c)
target_link_libraries(scarletnx-common PRIVATE snx_libdeflate) # a control.nacp's titles in deflate (utilities/common.c)
target_link_libraries(scarletnx-common PRIVATE snx_cgltf) # a glTF model's Mii parts (utilities/mii_gltf.c)
# the menus' icons, beside the VGA font (utilities/font.c): the launcher's font and the game menu's atlas draw them
include("${PROJECT_SOURCE_DIR}/cmake/gui_icons.cmake")
target_sources(scarletnx-common PRIVATE "${snx_gui_icons_c}")
# the A64 decoder (cpu/aarch64/a64_decoder.c) tools/decodetree generates from cpu/aarch64/decode/
include("${PROJECT_SOURCE_DIR}/cmake/decodetree.cmake")
snx_decodetree_a64(scarletnx-common)
if(WIN32)
    target_link_libraries(scarletnx-common PUBLIC advapi32)
else()
    target_link_libraries(scarletnx-common PUBLIC m)
endif()

# snx_demangle (utilities/demangle.h): C++ names through the C++ runtime's demangler, for the log summary. C, but what
# links it links the C++ runtime (LINKER_LANGUAGE CXX)
add_library(snx_demangle STATIC utilities/demangle.c)
set_target_properties(snx_demangle PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_demangle PRIVATE -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes)
# snx-logscan: the log summary (utilities/logscan.h) as a program, as scarletnx --scan-log, for the game reports
# workflow and the tests: only the C library and the demangler. Not for the phones, which run no programs of their own
if(NOT ANDROID AND NOT IOS)
    add_executable(snx-logscan utilities/logscan_main.c utilities/logscan.c)
    set_target_properties(snx-logscan PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
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
# (platform/vm.h); and shared libraries (platform/dl.h). No guest type, setting or SDL enters this target
add_library(scarletnx-platform STATIC platform/thread.c platform/thread_spawn.c platform/time.c platform/sleep.c
    platform/file.c platform/file_map.c platform/file_share.c platform/file_replace.c platform/vm.c platform/dl.c)
set_target_properties(scarletnx-platform PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(scarletnx-platform PRIVATE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)
target_compile_options(scarletnx-platform PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-platform PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_sources(scarletnx-platform PRIVATE platform/wpath.c) # snx_host_wpath
    target_link_libraries(scarletnx-platform PUBLIC synchronization) # plat_wait_word's WaitOnAddress
endif()

# The same indexed host filesystem and shared immutable file views (platform/file.h) serve NX
# and CTR. No guest ABI or runtime/platform dependency enters this target.
add_library(scarletnx-filesystem STATIC filesystem/tree.c)
set_target_properties(scarletnx-filesystem PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
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
# library of its own so the C99 tools don't link Winsock
add_library(scarletnx-net STATIC network/net.c network/net_icmp.c network/net_event.c network/net_dns.c
    network/net_iface.c)
target_compile_options(scarletnx-net PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-net PUBLIC scarletnx-platform Threads::Threads)
if(WIN32)
    target_link_libraries(scarletnx-net PUBLIC ws2_32 iphlpapi)
elseif(APPLE) # the system resolver's DNS servers (network/net_iface.c)
    target_link_libraries(scarletnx-net PUBLIC resolv)
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
# What the Vulkan frontends (src/nx/gpu/vk, src/3ds/gpu/vk, src/vita/gpu) share of a device: the instance, queries,
# the device's choice, the drivers known to break an extension or feature (gpu/vk/extension_bans.txt, compiled in),
# the submission ring, VK_EXT_shader_object's shader binaries and dynamic state, the swapchain and the SDL window
# (gpu/vk/). Each frontend compiles volk.c, whose functions these call
if(TARGET SDL3::SDL3)
    include("${PROJECT_SOURCE_DIR}/cmake/notice.cmake") # the windows' notices and the keys' text (snx_notice)
endif()
if(TARGET Vulkan::Headers AND TARGET SDL3::SDL3)
    include("${PROJECT_SOURCE_DIR}/cmake/vk_extension_bans.cmake")
    add_library(snx_vk_common STATIC gpu/vk/vk_common.c gpu/vk/driver_tag.c gpu/vk/extension_bans.c
                                     "${PROJECT_BINARY_DIR}/generated/vk_extension_bans_text.c" gpu/vk/ring.c
                                     gpu/vk/shader_object.c gpu/vk/swapchain.c gpu/vk/window.c gpu/vk/present_fx.c
                                     gpu/vk/fx/fx_fsr.c gpu/vk/notice.c)
    set_target_properties(snx_vk_common PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
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
endif()
