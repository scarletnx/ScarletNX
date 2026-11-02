# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The third-party libraries, fetched at configure time with CPM.cmake (https://github.com/cpm-cmake/CPM.cmake). Each
# package is pinned to a release archive and its SHA-256, and downloaded once into CPM_SOURCE_CACHE (.cache/cpm in the
# repository by default, shared by every build folder). Configuring offline works once the cache is filled.
#
# Bumping a package: change its version, URL and hash together, the hash taken from the release asset's digest or from
# a download checked by hand, never from what the next configure happens to fetch (snx-build skill).
#
# Patching a package: snx_patched_copy (cmake/patched_copy.cmake) on a copy in the build tree, never CPM's PATCHES, which
# patches only what CPM itself downloads into the cache.

set(CPM_VERSION 0.43.2)
set(CPM_HASH 49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa)
if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${PROJECT_SOURCE_DIR}/.cache/cpm" CACHE PATH "Where CPM.cmake keeps downloaded packages")
endif()
if(DEFINED CPM_SOURCE_CACHE)
    set(cpm_file "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_VERSION}.cmake")
else()
    set(cpm_file "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_VERSION}.cmake")
endif()
# Checked on every configure, not only after downloading it: a download that failed its check or broke off leaves a
# file behind, and the next configure would include it. The download goes to a .part file, renamed once checked
set(cpm_have "")
if(EXISTS "${cpm_file}")
    file(SHA256 "${cpm_file}" cpm_have)
endif()
if(NOT cpm_have STREQUAL CPM_HASH)
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
         "${cpm_file}.part" EXPECTED_HASH SHA256=${CPM_HASH})
    file(RENAME "${cpm_file}.part" "${cpm_file}")
endif()
include("${cpm_file}")

# Packages whose headers a system may also have (Vulkan) are added without SYSTEM. The compiler searches every
# -I folder before any -isystem one, so as -isystem they lost to an -I/usr/local/include in CFLAGS, usual on FreeBSD,
# and the build mixed in the system's copies. Third-party code builds with its own flags and stays out of the warning
# output
function(snx_quiet)
    foreach(target IN LISTS ARGN)
        target_compile_options(${target} PRIVATE -w)
    endforeach()
endfunction()

# LZ4's block decoder for NSO segments (src/nx/loader/loader.c, src/nx/runtime/loader.c) and hactool's NSO
# extraction. Only lib/lz4.c is built (BSD-2-Clause): the rest of LZ4 is its frame format, the HC compressor and the
# command-line program, none of which an NSO uses
if(SNX_BUILD_NX)
    CPMAddPackage(NAME lz4 VERSION 1.10.0
                  URL https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz
                  URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
                  DOWNLOAD_ONLY YES)
    add_library(snx_lz4 STATIC "${lz4_SOURCE_DIR}/lib/lz4.c")
    target_include_directories(snx_lz4 PUBLIC "${lz4_SOURCE_DIR}/lib")
    snx_quiet(snx_lz4)
endif()

# yyjson (MIT) for the driver's MCP server (src/nx/runtime/driver/mcp*.c) and, in every build, the GPU driver packages'
# meta.json (src/common/utilities/gpu_driver.c)
CPMAddPackage(NAME yyjson VERSION 0.13.0
              URL https://github.com/ibireme/yyjson/archive/refs/tags/0.13.0.tar.gz
              URL_HASH SHA256=34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae
              DOWNLOAD_ONLY YES)
add_library(snx_yyjson STATIC "${yyjson_SOURCE_DIR}/src/yyjson.c")
target_include_directories(snx_yyjson PUBLIC "${yyjson_SOURCE_DIR}/src")
snx_quiet(snx_yyjson)

# xxHash (BSD-2-Clause) for the renderer's content hashes (src/nx/gpu/vk/cmd.c's vk_hash): XXH3, about three times
# XXH64's speed. src/nx/gpu/vk/xxh3.c builds the dispatcher on x86 (the CPU's SSE2, AVX2 or AVX-512 loop, chosen at
# run time) and xxhash.c on every other architecture, by the compiler's own macros
if(SNX_BUILD_NX)
    CPMAddPackage(NAME xxhash VERSION 0.8.4
                  URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz
                  URL_HASH SHA256=5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32
                  DOWNLOAD_ONLY YES)
    add_library(snx_xxhash STATIC "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/xxh3.c")
    target_include_directories(snx_xxhash PUBLIC "${xxhash_SOURCE_DIR}")
    snx_quiet(snx_xxhash)
endif()

# jsmolka's gba-tests (MIT), ROMs that check the ARM7TDMI, the BIOS's open bus, the memory map, the save memory and
# the display, each leaving the number of its first failed test in a register. The AGB board's tests run them
# (tests/agb_gba_tests.py). Test data, not shipped
if(SNX_BUILD_AGB AND BUILD_TESTING)
    CPMAddPackage(NAME gba-tests VERSION 2025.04.19
                  URL https://github.com/jsmolka/gba-tests/archive/a7113b67e63f83a9b321696ddd7042ccfad6c881.tar.gz
                  URL_HASH SHA256=b9373ef18700c48ccab0940ec480517fba47accc86516df5fd43ed9ec6cdf082
                  DOWNLOAD_ONLY YES)
endif()

# ENet (MIT) and zstd (BSD-3-Clause) for ldn's rooms (src/common/network/net_room.c): an Eden room is ENet on UDP,
# and a game's datagrams in it are zstd frames. ENet's own sources, with the POSIX calls every target but Windows
# has; zstd's compressor and decompressor alone, in C (no assembly, so every target builds the same). The tests of
# the network services link them in a build without the runtime too
if(SNX_BUILD_NX OR SNX_BUILD_3DS OR BUILD_TESTING)
    CPMAddPackage(NAME enet VERSION 1.3.18
                  URL https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
                  URL_HASH SHA256=28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd
                  DOWNLOAD_ONLY YES)
    add_library(snx_enet STATIC "${enet_SOURCE_DIR}/callbacks.c" "${enet_SOURCE_DIR}/compress.c"
                "${enet_SOURCE_DIR}/host.c" "${enet_SOURCE_DIR}/list.c" "${enet_SOURCE_DIR}/packet.c"
                "${enet_SOURCE_DIR}/peer.c" "${enet_SOURCE_DIR}/protocol.c" "${enet_SOURCE_DIR}/unix.c"
                "${enet_SOURCE_DIR}/win32.c")
    target_include_directories(snx_enet PUBLIC "${enet_SOURCE_DIR}/include")
    if(WIN32)
        target_link_libraries(snx_enet PUBLIC ws2_32 winmm)
    else()
        target_compile_definitions(snx_enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
                                   HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
    endif()
    snx_quiet(snx_enet)
    CPMAddPackage(NAME zstd VERSION 1.5.7
                  URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
                  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
                  DOWNLOAD_ONLY YES)
    file(GLOB snx_zstd_sources "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c"
         "${zstd_SOURCE_DIR}/lib/decompress/*.c")
    add_library(snx_zstd STATIC ${snx_zstd_sources})
    target_include_directories(snx_zstd PUBLIC "${zstd_SOURCE_DIR}/lib")
    target_compile_definitions(snx_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD_SUPPORT_DEFAULT=0)
    snx_quiet(snx_zstd)
endif()

# the Vita driver (src/vita), which the root CMakeLists.txt adds when it is built and the build isn't tools-only: its
# loader's OpenSSL, its renderer's Vulkan headers and volk, its movies' FFmpeg and its window's SDL3
if(SNX_BUILD_VITA AND NOT SNX_TOOLS_ONLY)
    set(snx_vita_runtime ON)
else()
    set(snx_vita_runtime OFF)
endif()

# OpenSSL for the hashes and ciphers (the NX, 3DS and Vita loaders, ldr, csrng, hactool) and the ssl service's TLS. It
# builds with its own Configure and make, which cmake/openssl.cmake runs
# The archives' SHA-256 are also part of their cache keys (cmake/deps_cache.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/deps_cache.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/apple.cmake")
if(SNX_BUILD_NX OR SNX_BUILD_3DS OR SNX_BUILD_GNUABI OR snx_vita_runtime) # the GNU ABI runtime's random bytes and TLS sockets
    set(snx_openssl_sha256 325b5c806167c13b40b1ffeadfe0248197c00eccc4cf123ec1e28d2d2fd216d9)
    CPMAddPackage(NAME OpenSSL VERSION 4.0.3
                  URL https://github.com/openssl/openssl/releases/download/openssl-4.0.3/openssl-4.0.3.tar.gz
                  URL_HASH SHA256=${snx_openssl_sha256}
                  DOWNLOAD_ONLY YES)
    include("${PROJECT_SOURCE_DIR}/cmake/openssl.cmake")
endif()

# libcurl (curl's MIT-like license) for the launcher's downloads (src/common/network/https.c), built after OpenSSL
# (cmake/curl.cmake), and the roots it trusts: Mozilla's, curl's dated extract of them (cmake/cacert.cmake)
if(TARGET OpenSSL::SSL)
    set(snx_curl_sha256 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5)
    CPMAddPackage(NAME curl VERSION 8.23.0
                  URL https://curl.se/download/curl-8.23.0.tar.xz
                  URL_HASH SHA256=${snx_curl_sha256}
                  DOWNLOAD_ONLY YES)
    include("${PROJECT_SOURCE_DIR}/cmake/curl.cmake")
    set(snx_cacert_date 2026-10-20)
    CPMAddPackage(NAME cacert VERSION 2026.10.20
                  URL https://curl.se/ca/cacert-${snx_cacert_date}.pem
                  URL_HASH SHA256=796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe
                  DOWNLOAD_ONLY YES
                  DOWNLOAD_NO_EXTRACT YES)
    include("${PROJECT_SOURCE_DIR}/cmake/cacert.cmake")
endif()

# jemalloc as the scarletnx program's malloc, the whole process's (cmake/jemalloc.cmake): a recompile's LLVM threads
# spend less time in malloc and free than with the C library's. On by default where the program's malloc is every
# library's, Linux and FreeBSD (whose libc's is an older jemalloc); macOS takes it through jemalloc's zone, not by
# default yet. The sanitizers bring their own malloc
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$" AND NOT SNX_TEST_SANITIZERS)
    set(snx_jemalloc_default ON)
else()
    set(snx_jemalloc_default OFF)
endif()
option(SNX_JEMALLOC "Link jemalloc into scarletnx as its malloc (default ON on Linux and FreeBSD)"
       ${snx_jemalloc_default})
if(SNX_JEMALLOC)
    if(SNX_TEST_SANITIZERS)
        message(FATAL_ERROR "SNX_JEMALLOC=ON with SNX_TEST_SANITIZERS=ON: the sanitizers replace malloc themselves. "
                            "Configure with -DSNX_JEMALLOC=OFF.")
    endif()
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD|Darwin)$")
        message(FATAL_ERROR "SNX_JEMALLOC=ON on ${CMAKE_SYSTEM_NAME}: jemalloc takes the program's malloc on Linux, "
                            "FreeBSD and macOS only. On Windows the LLVM DLL keeps the C runtime's, and an Android or "
                            "iOS app the system's. Configure with -DSNX_JEMALLOC=OFF.")
    endif()
    set(snx_jemalloc_version 5.4.0) # also what tests/jemalloc.cmake finds in the program
    set(snx_jemalloc_sha256 200776fac271093e7c2f21edd6d62657ecd2be578d9328633f2a86bfa6ef4f1d)
    CPMAddPackage(NAME jemalloc VERSION ${snx_jemalloc_version}
                  URL https://github.com/jemalloc/jemalloc/releases/download/${snx_jemalloc_version}/jemalloc-${snx_jemalloc_version}.tar.bz2
                  URL_HASH SHA256=${snx_jemalloc_sha256}
                  DOWNLOAD_ONLY YES)
    include("${PROJECT_SOURCE_DIR}/cmake/jemalloc.cmake")
endif()

if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY) OR SNX_BUILD_3DS OR snx_vita_runtime)
    CPMAddPackage(NAME Vulkan-Headers VERSION 1.4.367
                  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.367.tar.gz
                  URL_HASH SHA256=0df7d58b5df617e0a6a17dec521af459206719f9c7cb12d05d09275597c16f70
                  EXCLUDE_FROM_ALL YES)
    # volk.c is compiled into the vk components, VMA is a header
    CPMAddPackage(NAME volk VERSION 1.4.363
                  URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
                  URL_HASH SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3
                  DOWNLOAD_ONLY YES)
    # the 3DS system archives' open stand-ins (cmake/ctr_system_archives.cmake): the RomFS images
    # B3n30/citra_system_archives makes (GPL-2.0-or-later), as the C arrays Azahar's source holds them in, each at the
    # commit that last changed it
    if(SNX_BUILD_3DS)
        CPMAddPackage(NAME AzaharMiiData VERSION 2019
                      URL https://raw.githubusercontent.com/azahar-emu/azahar/36ae10f555f73fb4e8d05cb676583efc67ecacef/externals/open_source_archives/include/mii.app.romfs.h
                      URL_HASH SHA256=4538bb4964ce4449732adc0bbf0faf69d7c054a0eee6d5f1dafce17bad409ad1
                      DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
        CPMAddPackage(NAME AzaharCountryList VERSION 2019
                      URL https://raw.githubusercontent.com/azahar-emu/azahar/a1517612d37f17e673849e04673a0f3c146d6208/externals/open_source_archives/include/country_list.app.romfs.h
                      URL_HASH SHA256=bd1c384c9788f99ed2cf9157727820c4c0000f6bbc01843ff8642a32e6f90651
                      DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
        CPMAddPackage(NAME AzaharSharedFont VERSION 2019
                      URL https://raw.githubusercontent.com/azahar-emu/azahar/7f1303a8344724598015a3e48a4bc5aef77090fc/externals/open_source_archives/include/shared_font.app.romfs.h
                      URL_HASH SHA256=316a05284c0746bcd1b5623c54ff770f2db736bdb90a592c2eda176e2c3ac6c1
                      DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
        CPMAddPackage(NAME AzaharBadWordList VERSION 2019
                      URL https://raw.githubusercontent.com/azahar-emu/azahar/8a2c44be5688db1cea22ba7c485e8befc892e673/externals/open_source_archives/include/bad_word_list.app.romfs.h
                      URL_HASH SHA256=fe040590fa035e2602c9ce25ba3cf6a208fcb72cb9d53945cbc5747610ad4e15
                      DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
        include("${PROJECT_SOURCE_DIR}/cmake/ctr_system_archives.cmake")
    endif()
    # volk.c and VMA's implementation (src/nx/gpu/vk/vma.cpp) compiled into a target of ours: no warnings, and not
    # the sanitizers SNX_TEST_SANITIZERS gives our code (tests/CMakeLists.txt)
    set(snx_third_party_flags -w)
    if(SNX_TEST_SANITIZERS)
        list(APPEND snx_third_party_flags -fno-sanitize=address,undefined)
    endif()
endif()

# FFmpeg for the Switch runtime's NVDEC and hwopus and the Vita's movies. It builds with its own configure and make,
# which cmake/ffmpeg.cmake runs
if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY) OR snx_vita_runtime)
    set(snx_ffmpeg_sha256 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e)
    CPMAddPackage(NAME FFmpeg VERSION 9.0.2
                  URL https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
                  URL_HASH SHA256=${snx_ffmpeg_sha256}
                  DOWNLOAD_ONLY YES)
    include("${PROJECT_SOURCE_DIR}/cmake/ffmpeg.cmake")
endif()

# libadrenotools and liblinkernsbypass (BSD-2-Clause) for the GPU drivers a player adds on Android arm64, the only ABI
# they build for (cmake/adrenotools.cmake). linkernsbypass is libadrenotools' submodule at that commit, which GitHub's
# archives leave out
if(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a" AND SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY)
    CPMAddPackage(NAME libadrenotools VERSION 2024.09.10
                  URL https://github.com/bylaws/libadrenotools/archive/8fae8ce254dfc1344527e05301e43f37dea2df80.tar.gz
                  URL_HASH SHA256=ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb
                  DOWNLOAD_ONLY YES)
    CPMAddPackage(NAME linkernsbypass VERSION 2024.09.10
                  URL https://github.com/bylaws/liblinkernsbypass/archive/aa3975893d83ef1bc84c321ec60c65fbf1287887.tar.gz
                  URL_HASH SHA256=da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638
                  DOWNLOAD_ONLY YES)
    include("${PROJECT_SOURCE_DIR}/cmake/adrenotools.cmake")
endif()

# Extraction-only and tools-only builds do not contain the NX runtime or its graphics/media dependencies.
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY)
    # the shared fonts' stand-ins (cmake/shared_fonts.cmake): Noto Sans subsets for the five text fonts, one per
    # script, because a game asks the font for the characters it draws and the Open Sans Eden ships has none of the
    # CJK ones; and yuzu's controller button glyphs (GPL-2.0-or-later) in the source file Eden keeps them in. The
    # subsets (SIL OFL 1.1, docs/SHARED_FONTS.md) are fetched from the commit of this repository that last changed
    # them, so a checkout, and a project fetching ScarletNX for its recompiler alone, carries none of their 11 MB
    set(snx_noto_url https://raw.githubusercontent.com/scarletnx/ScarletNX/777f377013806bc4cdbbc0306eca6e7e9c835df2/dist/fonts)
    foreach(font IN ITEMS "JP 0941fc22edbe0b3b9e8306d4a2feae5e9c0fbd25942600564cc3edb69da88e62"
                          "SC fa874173ff129ef89f6e24e432ab14a51124a21aae7a1860e9c8146337631312"
                          "TC 58aac9005ce3f61a1ed1bf052fce327b4a97e99540c45a25da23a8bf162402fa"
                          "KR ce9a4a8c87c60350440b8e0c77c0af77738ed86a145c85ce031cddf51dbbb9eb")
        string(REPLACE " " ";" font "${font}")
        list(GET font 0 script)
        list(GET font 1 hash)
        CPMAddPackage(NAME NotoSans${script}Subset VERSION 2026
                      URL ${snx_noto_url}/NotoSans${script}-Subset.ttf
                      URL_HASH SHA256=${hash}
                      DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
    endforeach()
    CPMAddPackage(NAME YuzuOSSExtension VERSION 2019
                  URL https://raw.githubusercontent.com/eden-emulator/mirror/2b87305d319f0bd04da36fb96314934f37b12637/src/core/file_sys/system_archive/data/font_nintendo_extended.cpp
                  URL_HASH SHA256=6fbbf5176ff4fd2024f9d150c889477f6b45ece4ac5e45d51286e32f8b13a6f2
                  DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)
    include("${PROJECT_SOURCE_DIR}/cmake/shared_fonts.cmake")

    CPMAddPackage(NAME VulkanMemoryAllocator VERSION 3.5.0
                  URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.5.0.tar.gz
                  URL_HASH SHA256=c5f3db848fca9cb9726b41f256cc3007773321fa92a86d1e0902e66474e8a9e3
                  DOWNLOAD_ONLY YES)
    # RenderDoc's in-application API, one header (renderdoc, gpu/vk/renderdoc.c), from the release tag
    CPMAddPackage(NAME renderdoc_app VERSION 1.46
                  URL https://raw.githubusercontent.com/baldurk/renderdoc/v1.46/renderdoc/api/app/renderdoc_app.h
                  URL_HASH SHA256=b7005e7dc34c3635046868bbd76d81b9b055aede0f56daa0bd39fedee0639ffb
                  DOWNLOAD_ONLY YES DOWNLOAD_NO_EXTRACT YES)

    # Tracy client, for SNX_TRACY development builds.
    if(SNX_TRACY)
        # The sampler and the callstacks it needs are on; the zones take no callstacks of their own, so it costs
        # nothing per zone. The wait stacks are off: they walk the stack at every context switch. The zones' own
        # validation is off: it queues a ZoneValidation next to every zone begin, end, text and value, which in a
        # build whose whole purpose is to observe zones doubles the queue traffic they cost.
        set(snx_tracy_options "TRACY_ENABLE ON" "TRACY_ON_DEMAND ON" "TRACY_NO_CRASH_HANDLER ON" "TRACY_STATIC ON"
                              "TRACY_MANUAL_LIFETIME ON" "TRACY_NO_CODE_TRANSFER ON" "TRACY_NO_FRAME_IMAGE ON"
                              "TRACY_NO_WAIT_STACKS ON" "TRACY_NO_VERIFY ON")
        if(NOT ANDROID)
            list(APPEND snx_tracy_options "TRACY_ONLY_LOCALHOST ON" "TRACY_NO_BROADCAST ON")
        endif()
        CPMAddPackage(NAME tracy VERSION 0.14.1
                      URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.tar.gz
                      URL_HASH SHA256=bf4af567e9c7524d07f3caa745fad02fb33bd5694f11910750382d1efbb251c1
                      EXCLUDE_FROM_ALL YES
                      OPTIONS ${snx_tracy_options})
        # The sampler waits for the runtime to ask for it, and samples at 1 kHz unless TRACY_SAMPLING_HZ says
        # otherwise.
        target_compile_definitions(TracyClient PRIVATE TRACY_SAMPLING_PROFILER_MANUAL_START TRACY_SAMPLING_HZ=1000)
        snx_quiet(TracyClient)
    endif()
endif()

set(SNX_SDL3_FROM_SOURCE OFF)
# The system's SDL3 from find_package, or CPM's pinned source build. FreeBSD's ports package lags the pin (3.4.16
# against 3.4.18), so CPM's version check refuses it and builds SDL3 itself: SNX_SYSTEM_SDL3 takes the native package
# instead, which needs only 3.4.16, the version every SDL call in the tree is in. It is the default on FreeBSD, whose
# build installs the sdl3 package (docs/BUILD.md); the pinned source build is had with -DSNX_SYSTEM_SDL3=OFF
set(snx_sdl3_system_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(snx_sdl3_system_default ON)
endif()
option(SNX_SYSTEM_SDL3 "Use the system's SDL3 from find_package, not CPM's pinned source build"
       ${snx_sdl3_system_default})
if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY) OR SNX_BUILD_GUI OR SNX_BUILD_3DS OR snx_vita_runtime OR SNX_BUILD_AGB)
    if(SNX_SYSTEM_SDL3)
        find_package(SDL3 3.4.16 CONFIG QUIET)
        if(NOT SDL3_FOUND)
            message(FATAL_ERROR "SNX_SYSTEM_SDL3: no SDL3 3.4.16 or newer package found (FreeBSD: pkg install sdl3). "
                                "Use -DSNX_SYSTEM_SDL3=OFF for CPM's pinned source build.")
        endif()
        if(NOT TARGET SDL3::SDL3)
            message(FATAL_ERROR "SNX_SYSTEM_SDL3: the system's SDL3 ${SDL3_VERSION} has no SDL3::SDL3 target")
        endif()
        message(STATUS "Using the system's SDL3 ${SDL3_VERSION}")
    else()
    # Prefer a compatible system package. CPM handles the pinned static fallback and user overrides. SDL_ALTIVEC only
    # changes PowerPC builds: SDL's AltiVec blitters prefetch with dstt, which the ISA phased out and qemu's POWER10
    # traps as illegal (the window icon's and the menu's software surfaces, which nothing here needs fast)
    CPMFindPackage(NAME SDL3 VERSION 3.4.18 FIND_PACKAGE_ARGUMENTS CONFIG
                   URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
                   URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90
                   EXCLUDE_FROM_ALL YES SYSTEM YES
                   OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF"
                           "SDL_EXAMPLES OFF" "SDL_INSTALL OFF" "SDL_UNIX_CONSOLE_BUILD ON" "SDL_ALTIVEC OFF")
    if(SDL3_ADDED)
        set(SNX_SDL3_FROM_SOURCE ON)
        snx_quiet(SDL3-static)
        # ccache can't reuse a compile that read a precompiled header: under a launcher, SDL3 goes without one
        if(CMAKE_C_COMPILER_LAUNCHER)
            set_target_properties(SDL3-static PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
        endif()
    endif()
    endif()
endif()

# libdeflate (MIT), every deflate the project writes or reads: the launcher's Share Log (src/gui/logzip.c), the Vita
# driver's VPK members and compressed eboot segments (src/vita/loader/inflate.c), and their CRC-32. Its library
# without gzip, from its own sources; it picks the CPU's SIMD loops at run time
if(SNX_BUILD_GUI OR (SNX_BUILD_VITA AND NOT SNX_TOOLS_ONLY))
    CPMAddPackage(NAME libdeflate VERSION 1.26
                  URL https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.26.tar.gz
                  URL_HASH SHA256=bba03fffc5538576213675ce6968fcff6ce2e67d82e4d5febea2d05f9f13cf85
                  DOWNLOAD_ONLY YES)
    add_library(snx_libdeflate STATIC
        "${libdeflate_SOURCE_DIR}/lib/utils.c" "${libdeflate_SOURCE_DIR}/lib/arm/cpu_features.c"
        "${libdeflate_SOURCE_DIR}/lib/x86/cpu_features.c" "${libdeflate_SOURCE_DIR}/lib/deflate_compress.c"
        "${libdeflate_SOURCE_DIR}/lib/deflate_decompress.c" "${libdeflate_SOURCE_DIR}/lib/adler32.c"
        "${libdeflate_SOURCE_DIR}/lib/zlib_compress.c" "${libdeflate_SOURCE_DIR}/lib/zlib_decompress.c"
        "${libdeflate_SOURCE_DIR}/lib/crc32.c")
    target_include_directories(snx_libdeflate SYSTEM PUBLIC "${libdeflate_SOURCE_DIR}")
    snx_quiet(snx_libdeflate)
endif()

# miniz (MIT) for the launcher's Share Log, the logs in one zip (src/gui/logzip.c): the zip around the deflate
# libdeflate makes. Its one-file release, miniz.c, built alone, without its stdio functions (the zip is made in memory
# and written as every file is, for Windows' UTF-8 paths) and without zlib's names, which another library's zlib could
# share
if(SNX_BUILD_GUI)
    CPMAddPackage(NAME miniz VERSION 3.1.2
                  URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
                  URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a
                  DOWNLOAD_ONLY YES)
    add_library(snx_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
    target_include_directories(snx_miniz SYSTEM PUBLIC "${miniz_SOURCE_DIR}")
    target_compile_definitions(snx_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    snx_quiet(snx_miniz)
endif()

# The boot splash's drawings are SVGs, drawn at the splash's size by lunasvg (MIT) on plutovg (MIT; its rasterizer and
# stroker come from FreeType, under the FreeType License, source/FTL.TXT). cmake/splash.cmake builds both. The runtime
# draws one too, the user's picture (hle/services/acc.c)
if(SNX_BUILD_GUI OR (SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY))
    CPMAddPackage(NAME plutovg VERSION 1.3.3
                  URL https://github.com/sammycage/plutovg/archive/refs/tags/v1.3.3.tar.gz
                  URL_HASH SHA256=2b0d17a6e016f47b86f9c00e2cb82600041b1ea1f7d2a00c2d46ae542cbfed3c
                  DOWNLOAD_ONLY YES)
    CPMAddPackage(NAME lunasvg VERSION 3.5.0
                  URL https://github.com/sammycage/lunasvg/archive/refs/tags/v3.5.0.tar.gz
                  URL_HASH SHA256=1abf1472ee6c4d19797916e8cc3c2e4b628e0d81178ffac60bdb0d457e32c690
                  DOWNLOAD_ONLY YES)
endif()

# hactool, in the tree (src/nx/runtime/driver/hactool), which scarletnx --extract calls in its own process
if(SNX_BUILD_NX)
    include("${PROJECT_SOURCE_DIR}/cmake/hactool.cmake")
endif()
