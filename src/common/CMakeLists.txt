# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(scarletnx-common STATIC
    utilities/common.c utilities/process.c utilities/font.c utilities/ini_lines.c utilities/game_ini.c utilities/pad_map.c
    utilities/pad_screen.c utilities/users.c utilities/logscan.c utilities/language_tag.c utilities/mods.c utilities/patches.c utilities/cheat_list.c utilities/state_header.c
    utilities/gpu_driver.c
    utilities/spirv_emit.c utilities/script.c memory/host.c
    cpu/aarch64/a64_decoder.c cpu/recompiler/codegen.c cpu/recompiler/variants.c runtime/cas128.c)
target_compile_definitions(scarletnx-common PRIVATE _CRT_SECURE_NO_WARNINGS
    SNX_HOST_COMPILER="${CMAKE_C_COMPILER}"
    SNX_CPU_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/cpu/aarch64"
    SNX_SOURCE_INCLUDE="${PROJECT_SOURCE_DIR}/src")
target_compile_options(scarletnx-common PRIVATE -Wall -Wextra -Werror=missing-prototypes)
# RtAPI.cas128's compare-and-swap is cmpxchg16b with cx16, and a libatomic call without, which nothing here links
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set_source_files_properties(runtime/cas128.c PROPERTIES COMPILE_OPTIONS -mcx16)
endif()
target_link_libraries(scarletnx-common PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(scarletnx-common PRIVATE snx_yyjson) # the GPU driver packages' meta.json (utilities/gpu_driver.c)
# the menus' icons, beside the VGA font (utilities/font.c): the launcher's font and the game menu's atlas draw them
include("${PROJECT_SOURCE_DIR}/cmake/gui_icons.cmake")
target_sources(scarletnx-common PRIVATE "${snx_gui_icons_c}")
# the A64 decoder (cpu/aarch64/a64_decoder.c) tools/a64dec generates from cpu/aarch64/decode/
include("${PROJECT_SOURCE_DIR}/cmake/a64dec.cmake")
snx_a64dec(scarletnx-common)
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

# The same indexed host filesystem and shared immutable file views serve NX
# and CTR. No guest ABI or runtime/platform dependency enters this target.
add_library(scarletnx-filesystem STATIC filesystem/tree.c filesystem/host.c)
set_target_properties(scarletnx-filesystem PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(scarletnx-filesystem PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)
target_compile_options(scarletnx-filesystem PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-filesystem PUBLIC Threads::Threads)

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
target_link_libraries(scarletnx-net PUBLIC Threads::Threads)
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

# What the Vulkan frontends (src/nx/gpu/vk, src/3ds/gpu/vk, src/vita/gpu) share of a device: the instance, queries,
# the device's choice, the submission ring, the swapchain and the SDL window (gpu/vk/). Each frontend compiles volk.c,
# whose functions these call
if(TARGET Vulkan::Headers AND TARGET SDL3::SDL3)
    add_library(snx_vk_common STATIC gpu/vk/vk_common.c gpu/vk/ring.c gpu/vk/swapchain.c gpu/vk/window.c)
    set_target_properties(snx_vk_common PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx_vk_common PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                                 -Werror=missing-variable-declarations)
    target_compile_definitions(snx_vk_common PRIVATE VK_NO_PROTOTYPES)
    target_include_directories(snx_vk_common PUBLIC "${volk_SOURCE_DIR}")
    target_link_libraries(snx_vk_common PUBLIC Vulkan::Headers SDL3::SDL3)
endif()
