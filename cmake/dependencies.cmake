# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The third-party libraries, fetched at configure time with CPMUtil (cmake/packages.cmake): each one cpmfile.json pins
# is added here, under the conditions that need it.
#
# Bumping a package: change its entry's version (and artifact or url) and hash together, the hash the SHA-512 of a
# download checked by hand against the release's own digest, never of what the next configure happens to fetch
# (docs/dev/build.md, "Third-party packages").
#
# Patching a package: a patch in .patch/<key>/, named in its entry's "patches", which CPMUtil applies without fuzz to
# the release it extracts.
include("${PROJECT_SOURCE_DIR}/cmake/packages.cmake")

# Tracy's release: the client an SNX_TRACY build pins below, and the tracy-capture the driver's help and the runtime's
# warning name, which has to be the same release to capture that client
snx_cpmfile(SNX_TRACY_VERSION tracy version)
string(REGEX REPLACE "^v" "" SNX_TRACY_VERSION "${SNX_TRACY_VERSION}")

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
# extraction, beside ZBIC (snx_zbic, after zstd below) for the segments an Ounce game compresses so. Only lib/lz4.c
# is built (BSD-2-Clause): the rest of LZ4 is its frame format, the HC compressor and the command-line program, none
# of which an NSO uses
if(SNX_BUILD_NX)
    snx_cpm_options(lz4)
    AddJsonPackage(NAME lz4 DOWNLOAD_ONLY)
    if(lz4_ADDED)
        add_library(snx_lz4 STATIC "${lz4_SOURCE_DIR}/lib/lz4.c")
        target_include_directories(snx_lz4 PUBLIC "${lz4_SOURCE_DIR}/lib")
        snx_quiet(snx_lz4)
    else() # the system's library (lz4_FORCE_SYSTEM, CPMUTIL_FORCE_SYSTEM)
        add_library(snx_lz4 INTERFACE IMPORTED)
        target_link_libraries(snx_lz4 INTERFACE LZ4::lz4)
    endif()
endif()

# yyjson (MIT) for the driver's MCP server (src/nx/runtime/driver/mcp*.c) and, in every build, the GPU driver packages'
# meta.json (src/common/utilities/gpu_driver.c)
AddJsonPackage(NAME yyjson DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
add_library(snx_yyjson STATIC "${yyjson_SOURCE_DIR}/src/yyjson.c")
target_include_directories(snx_yyjson PUBLIC "${yyjson_SOURCE_DIR}/src")
snx_quiet(snx_yyjson)

# cgltf (MIT) for the Mii parts a glTF 2.0 model gives (src/common/utilities/mii_gltf.c): its implementation in a
# target of ours, built quiet
AddJsonPackage(NAME cgltf DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/generated/cgltf.c" CONTENT "#define CGLTF_IMPLEMENTATION
#include \"cgltf.h\"
")
add_library(snx_cgltf STATIC "${PROJECT_BINARY_DIR}/generated/cgltf.c")
target_include_directories(snx_cgltf SYSTEM PUBLIC "${cgltf_SOURCE_DIR}")
snx_quiet(snx_cgltf)

# xxHash (BSD-2-Clause) for the renderer's content hashes (src/nx/gpu/vk/cmd.c's vk_hash): XXH3, about three times
# XXH64's speed. src/nx/gpu/vk/xxh3.c builds the dispatcher on x86 (the CPU's SSE2, AVX2 or AVX-512 loop, chosen at
# run time) and xxhash.c on every other architecture, by the compiler's own macros
if(SNX_BUILD_NX)
    AddJsonPackage(NAME xxhash DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    add_library(snx_xxhash STATIC "${PROJECT_SOURCE_DIR}/src/nx/gpu/vk/xxh3.c")
    target_include_directories(snx_xxhash PUBLIC "${xxhash_SOURCE_DIR}")
    snx_quiet(snx_xxhash)
endif()

# RNNoise 0.1.1 (BSD-3-Clause) for the microphone's noise reduction (src/nx/runtime/platform/audio_denoise.c): a small
# recurrent network that gives each band of a 10 ms frame at 48 kHz its gain. Its model is rnn_data.c, 87503 weights of
# one byte (425 KB of C). Its sources but the training program (denoise.c's TRAINING part, off by default), with their
# symbols hidden, so that no other librnnoise a library loads into the process binds to them. It installs no CMake
# package (its build is autotools), so find_package has none to find: the pinned copy is the one the runtime runs, and
# another version's network would give other gains and another delay
if(SNX_BUILD_NX)
    AddJsonPackage(NAME rnnoise DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    add_library(snx_rnnoise STATIC "${rnnoise_SOURCE_DIR}/src/denoise.c" "${rnnoise_SOURCE_DIR}/src/rnn.c"
                "${rnnoise_SOURCE_DIR}/src/rnn_data.c" "${rnnoise_SOURCE_DIR}/src/kiss_fft.c"
                "${rnnoise_SOURCE_DIR}/src/pitch.c" "${rnnoise_SOURCE_DIR}/src/celt_lpc.c")
    target_include_directories(snx_rnnoise SYSTEM PUBLIC "${rnnoise_SOURCE_DIR}/include")
    set_target_properties(snx_rnnoise PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
    if(NOT WIN32)
        target_link_libraries(snx_rnnoise PRIVATE m)
    endif()
    snx_quiet(snx_rnnoise)
endif()

# ENet (MIT) and zstd (BSD-3-Clause) for ldn's rooms (src/common/network/net_room.c): an Eden room is ENet on UDP,
# and a game's datagrams in it are zstd frames. ENet's own sources, with the POSIX calls every target but Windows
# has; zstd's compressor and decompressor alone, in C (no assembly, so every target builds the same). The tests of
# the network services link them in a build without the runtime too
if(SNX_BUILD_NX OR SNX_BUILD_3DS OR BUILD_TESTING)
    AddJsonPackage(NAME enet DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
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
    AddJsonPackage(NAME zstd DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    file(GLOB snx_zstd_sources "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c"
         "${zstd_SOURCE_DIR}/lib/decompress/*.c")
    add_library(snx_zstd STATIC ${snx_zstd_sources})
    target_include_directories(snx_zstd PUBLIC "${zstd_SOURCE_DIR}/lib")
    target_compile_definitions(snx_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD_SUPPORT_DEFAULT=0)
    snx_quiet(snx_zstd)
    # ZBIC (src/common/utilities/zbic.c), the other compression of an Ounce game's NSO segments: zstd's decoder from
    # these sources once more, its symbols local or renamed, with ZBIC's magic and FSE table headers. Everything that
    # decodes NSO segments with snx_lz4 takes both
    if(TARGET snx_lz4)
        add_library(snx_zbic STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/zbic.c")
        target_include_directories(snx_zbic PRIVATE "${zstd_SOURCE_DIR}/lib")
        snx_quiet(snx_zbic)
        target_link_libraries(snx_lz4 INTERFACE snx_zbic)
    endif()
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
# The archives' hashes are also part of their cache keys (cmake/deps_cache.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/deps_cache.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/apple.cmake")
if(SNX_BUILD_NX OR SNX_BUILD_3DS OR SNX_BUILD_GNUABI OR snx_vita_runtime) # the GNU ABI runtime's random bytes and TLS sockets
    snx_cpmfile(snx_openssl_hash openssl hash)
    AddJsonPackage(NAME openssl DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    include("${PROJECT_SOURCE_DIR}/cmake/openssl.cmake")
endif()

# libcurl (curl's MIT-like license) for the launcher's downloads (src/common/network/https.c), built after OpenSSL
# (cmake/curl.cmake), and the roots it trusts: Mozilla's, curl's dated extract of them (cmake/cacert.cmake)
if(TARGET OpenSSL::SSL)
    snx_cpmfile(snx_curl_hash curl hash)
    AddJsonPackage(NAME curl DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    include("${PROJECT_SOURCE_DIR}/cmake/curl.cmake")
    snx_cpmfile(snx_cacert_date cacert version)
    snx_package_file(cacert)
    include("${PROJECT_SOURCE_DIR}/cmake/cacert.cmake")
endif()

# jemalloc as the scarletnx program's malloc, the whole process's (cmake/jemalloc.cmake): a recompile's LLVM threads
# spend less time in malloc and free than with the C library's. On by default where the program's malloc is every
# library's: Linux and FreeBSD (whose libc's is an older jemalloc), and Android's driver, the executable the menu runs
# recompiles in (libscarletnx-run.so, src/nx), whose C library calls malloc through the program's as glibc does, and
# macOS through jemalloc's zone, which it puts in front of the system's. Windows' C runtime is a DLL with a heap of its
# own, which LLVM's DLL and the C runtime's strdup allocate from, and an iOS app keeps the system's. The sanitizers
# bring their own malloc
# Android's program is the Switch driver's executable, which a build without the runtime does not make
if(ANDROID AND (NOT SNX_BUILD_NX OR SNX_TOOLS_ONLY))
    set(snx_jemalloc_has_program OFF)
else()
    set(snx_jemalloc_has_program ON)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD|Android|Darwin)$" AND snx_jemalloc_has_program AND
   NOT SNX_TEST_SANITIZERS)
    set(snx_jemalloc_default ON)
else()
    set(snx_jemalloc_default OFF)
endif()
option(SNX_JEMALLOC "Link jemalloc into scarletnx as its malloc (default ON on Linux, FreeBSD, Android and macOS)"
       ${snx_jemalloc_default})
if(SNX_JEMALLOC)
    if(SNX_TEST_SANITIZERS)
        message(FATAL_ERROR "SNX_JEMALLOC=ON with SNX_TEST_SANITIZERS=ON: the sanitizers replace malloc themselves. "
                            "Configure with -DSNX_JEMALLOC=OFF.")
    endif()
    if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD|Darwin|Android)$")
        message(FATAL_ERROR "SNX_JEMALLOC=ON on ${CMAKE_SYSTEM_NAME}: jemalloc takes the program's malloc on Linux, "
                            "FreeBSD, macOS and Android only. On Windows the C runtime DLL keeps its own heap, which "
                            "LLVM's DLL and its strdup allocate from, and an iOS app keeps the system's. Configure "
                            "with -DSNX_JEMALLOC=OFF.")
    endif()
    if(NOT snx_jemalloc_has_program)
        message(FATAL_ERROR "SNX_JEMALLOC=ON on Android goes into libscarletnx-run.so, the Switch driver's executable "
                            "(src/nx), which this build does not make (SNX_BUILD_NX=${SNX_BUILD_NX}, "
                            "SNX_TOOLS_ONLY=${SNX_TOOLS_ONLY}). Configure with -DSNX_JEMALLOC=OFF.")
    endif()
    snx_cpmfile(snx_jemalloc_version jemalloc version) # also what tests/jemalloc.cmake finds in the program
    snx_cpmfile(snx_jemalloc_hash jemalloc hash)
    AddJsonPackage(NAME jemalloc DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    include("${PROJECT_SOURCE_DIR}/cmake/jemalloc.cmake")
endif()

if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY) OR SNX_BUILD_3DS OR SNX_BUILD_NDS OR snx_vita_runtime OR
   (SNX_BUILD_ANDROID AND NOT SNX_TOOLS_ONLY))
    # the system's package is named VulkanHeaders (cpmfile.json's find_args)
    snx_cpm_options(vulkan-headers)
    AddJsonPackage(vulkan-headers)
    if(Vulkan-Headers_ADDED)
        set(snx_vulkan_include_dir "${Vulkan-Headers_SOURCE_DIR}/include")
    else()
        get_target_property(snx_vulkan_include_dir Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
        snx_system_vulkan_registry(snx_vulkan_local_registry)
    endif()
    # volk.c is compiled into the vk components
    AddJsonPackage(NAME volk DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    # the 3DS system archives' open stand-ins (cmake/ctr_system_archives.cmake): the RomFS images
    # B3n30/citra_system_archives makes (GPL-2.0-or-later), as the C arrays Azahar's source holds them in, each at the
    # commit that last changed it. The Mii data is ours (src/common/utilities/mii_model.c), made when a game opens it
    if(SNX_BUILD_3DS)
        snx_package_file(azahar-country-list)
        snx_package_file(azahar-shared-font)
        snx_package_file(azahar-bad-word-list)
        include("${PROJECT_SOURCE_DIR}/cmake/ctr_system_archives.cmake")
    endif()
    # volk.c compiled into a target of ours: no warnings, and not
    # the sanitizers SNX_TEST_SANITIZERS gives our code (tests/CMakeLists.txt)
    set(snx_third_party_flags -w)
    if(SNX_TEST_SANITIZERS)
        list(APPEND snx_third_party_flags -fno-sanitize=address,undefined)
    endif()
endif()

# FFmpeg for the Switch runtime's NVDEC and hwopus, the 3DS DSP's AAC decoder, the Vita's movies and the Android
# driver's images (src/android/hwui/image.c). It builds with its own configure and make, which cmake/ffmpeg.cmake runs
if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY) OR SNX_BUILD_3DS OR snx_vita_runtime OR
   (SNX_BUILD_ANDROID AND NOT SNX_TOOLS_ONLY))
    # the Android driver's alone: FFmpeg's image codecs and libswscale (cmake/ffmpeg.cmake), and zlib
    set(snx_ffmpeg_images OFF)
    if(SNX_BUILD_ANDROID AND NOT SNX_TOOLS_ONLY)
        set(snx_ffmpeg_images ON)
        # zlib for FFmpeg's PNG decoder and encoder and the Android driver's libz.so (cmake/zlib.cmake)
        AddJsonPackage(NAME zlib DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
        snx_cpmfile(snx_zlib_hash zlib hash)
        include("${PROJECT_SOURCE_DIR}/cmake/zlib.cmake")
    endif()
    snx_cpmfile(snx_ffmpeg_hash ffmpeg hash)
    AddJsonPackage(NAME ffmpeg DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    # dav1d (BSD-2-Clause) for the Ounce's AV1 movies, which FFmpeg decodes through it: built with meson and ninja,
    # which cmake/dav1d.cmake runs, before FFmpeg
    if(SNX_BUILD_NX)
        snx_cpmfile(snx_dav1d_hash dav1d hash)
        AddJsonPackage(NAME dav1d DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
        include("${PROJECT_SOURCE_DIR}/cmake/dav1d.cmake")
    endif()
    include("${PROJECT_SOURCE_DIR}/cmake/ffmpeg.cmake")
endif()

# Extraction-only and tools-only builds do not contain the NX runtime or its graphics/media dependencies.
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY)
    # the shared fonts' stand-ins (cmake/shared_fonts.cmake): Noto Sans subsets for the five text fonts, one per
    # script, because a game asks the font for the characters it draws and the Open Sans Eden ships has none of the
    # CJK ones; and yuzu's controller button glyphs (GPL-2.0-or-later) in the source file Eden keeps them in. The
    # subsets (SIL OFL 1.1, docs/platforms/nx/filesystem.md) are fetched from the commit of this repository that last changed
    # them, so a checkout, and a project fetching ScarletNX for its recompiler alone, carries none of their 11 MB
    foreach(script jp sc tc kr)
        snx_package_file(noto-sans-${script}-subset)
    endforeach()
    snx_package_file(yuzu-oss-extension)
    include("${PROJECT_SOURCE_DIR}/cmake/shared_fonts.cmake")

    # NVIDIA Streamline's headers (MIT, NVIDIA-RTX/Streamline v2.14.16), the DLSS backend's (src/nx/gpu/vk/upscaler/dlss.cpp),
    # in a Windows build alone, as Streamline runs there alone. One file each, the ones it includes: the repository also
    # holds NVIDIA's NGX SDK, whose licence keeps it out of this program, and nothing else of it is fetched. NVIDIA's
    # runtime (its DLLs) is never fetched or shipped: the user downloads it (docs/user/dlss.md)
    if(WIN32)
        set(snx_streamline_include "${PROJECT_BINARY_DIR}/streamline/include")
        # each pinned in cpmfile.json by its SHA-512, as streamline_<its name, sl_dlss_h>
        foreach(snx_sl_name IN ITEMS sl.h sl_struct.h sl_consts.h sl_version.h sl_result.h sl_appidentity.h
                                     sl_device_wrappers.h sl_core_api.h sl_core_types.h sl_dlss.h sl_helpers_vk.h)
            string(MAKE_C_IDENTIFIER "${snx_sl_name}" snx_sl_id)
            snx_package_file(streamline_${snx_sl_id})
            # one folder, as they include each other by name
            configure_file("${streamline_${snx_sl_id}_SOURCE_DIR}/${snx_sl_name}"
                           "${snx_streamline_include}/${snx_sl_name}" COPYONLY)
        endforeach()
    endif()

    # AMD FidelityFX Super Resolution 2.2.1 (MIT), the temporal upscaler the Ounce's DLSS evaluations run through
    # (src/nx/gpu/vk/fsr2, gpu/nvn/nvn_dlss.c): its library and Vulkan backend's sources and its GLSL passes, which the
    # build compiles (cmake/fsr2_shaders.cmake). The archive's prebuilt tools (tools/sc, Windows programs) are not used
    AddJsonPackage(NAME fidelityfx-fsr2 DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    # Its library carries two patches (.patch/fidelityfx-fsr2). 0001-memory-types.patch: the backend takes device-local
    # memory only from a type that is not host-visible, and found none where every device-local type is (lavapipe, some
    # integrated GPUs), so no context could be made there. 0002-fsr1-easu-glsl.patch: FSR 1's EASU in its ffx_fsr1.h,
    # which FSR 2 never compiles, puts a comparison in an integer, which GLSL refuses. The upscalers' spatial pass
    # compiles it (src/nx/gpu/vk/upscaler/up_easu.comp)
    set(snx_fsr2_api_dir "${FidelityFX-FSR2_SOURCE_DIR}/src/ffx-fsr2-api")
    # SMAA 1x (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, MIT), the
    # anti-aliasing the upscalers run before a copy or a spatial upscale (src/nx/gpu/vk/upscaler/up_smaa.comp): its
    # reference shader and its precomputed area and search textures, each pinned in cpmfile.json at the commit its
    # repository ends at
    snx_package_file(smaa-shader)
    snx_package_file(smaa-area-tex)
    snx_package_file(smaa-search-tex)
    # RenderDoc's in-application API, one header (renderdoc, gpu/vk/renderdoc.c), from the release tag
    snx_package_file(renderdoc_app)

    # Tracy client, for SNX_TRACY development builds.
    if(SNX_TRACY)
        option(SNX_TRACY_WAIT_STACKS "Keep Tracy's wait stacks on: they walk the stack at every context switch and need root and tracefs" OFF)
        # The sampler and the callstacks it needs are on; the zones take no callstacks of their own, so it costs
        # nothing per zone. The wait stacks are off by default: SNX_TRACY_WAIT_STACKS=ON keeps them on. They walk the
        # stack at every context switch, and they need root and tracefs. The zones' own validation is on: it queues a
        # ZoneValidation next to every zone begin, end, text and value, doubled per zone and only while a server is
        # attached, which is the price of a run that says when this runtime's zone pairs are unbalanced. The code the
        # zones are in and the pictures of the presented frame are on too: both cost nothing until a GUI asks for them.
        # Fibers are on: the guest's waits get a lane each, named by what the thread blocked on. Tracy's set_option
        # makes TRACY_FIBERS a PUBLIC define, so the runtime that emits them sees it too.
        set(snx_tracy_options "TRACY_ENABLE ON" "TRACY_ON_DEMAND ON" "TRACY_NO_CRASH_HANDLER ON" "TRACY_STATIC ON"
                              "TRACY_MANUAL_LIFETIME ON" "TRACY_NO_WAIT_STACKS ON" "TRACY_FIBERS ON")
        if(SNX_TRACY_WAIT_STACKS)
            list(REMOVE_ITEM snx_tracy_options "TRACY_NO_WAIT_STACKS ON")
        endif()
        if(NOT ANDROID)
            list(APPEND snx_tracy_options "TRACY_ONLY_LOCALHOST ON" "TRACY_NO_BROADCAST ON")
        endif()
        AddJsonPackage(NAME tracy FORCE_BUNDLED_PACKAGE ON OPTIONS ${snx_tracy_options})
        # The sampler waits for the runtime to ask for it and samples at 1 kHz; TRACY_SAMPLING_HZ is this build's
        # compile-time define, not an environment variable.
        target_compile_definitions(TracyClient PRIVATE TRACY_SAMPLING_PROFILER_MANUAL_START TRACY_SAMPLING_HZ=1000)
        snx_quiet(TracyClient)
    endif()
endif()

set(SNX_SDL3_FROM_SOURCE OFF)
# The system's SDL3 from find_package, or the pinned source build. FreeBSD's ports package lags the pin (3.4.16 against
# 3.4.18), so CPMUtil's version check refuses it and builds SDL3 itself: SNX_SYSTEM_SDL3 takes the native package
# instead, from 3.4.12 (cmake/system_sdl3.cmake). It is the default on FreeBSD, whose build installs the sdl3 package
# (docs/dev/build.md), and on NetBSD, whose pkgsrc SDL3 is 3.4.16 too (docs/hosts/netbsd.md); the pinned source build
# is had with -DSNX_SYSTEM_SDL3=OFF
include("${CMAKE_CURRENT_LIST_DIR}/system_sdl3.cmake")
set(snx_sdl3_system_default OFF)
if(CMAKE_SYSTEM_NAME MATCHES "^(FreeBSD|NetBSD)$")
    set(snx_sdl3_system_default ON)
endif()
option(SNX_SYSTEM_SDL3 "Use the system's SDL3 from find_package, not the pinned source build"
       ${snx_sdl3_system_default})
if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY) OR SNX_BUILD_GUI OR SNX_BUILD_3DS OR snx_vita_runtime OR SNX_BUILD_AGB OR
   SNX_BUILD_NDS OR (SNX_BUILD_ANDROID AND NOT SNX_TOOLS_ONLY))
    if(SNX_SYSTEM_SDL3)
        snx_system_sdl3()
    else()
    # A system package at the pin or newer, else the pinned source, static (CPMUtil's choice, which SDL3_FORCE_SYSTEM
    # and SDL3_FORCE_BUNDLED make). Of its options in cpmfile.json, SDL_ALTIVEC OFF only changes PowerPC builds: SDL's
    # AltiVec blitters prefetch with dstt, which the ISA phased out and qemu's POWER10 traps as illegal (the window
    # icon's and the menu's software surfaces, which nothing here needs fast)
    snx_cpm_options(sdl3)
    AddJsonPackage(sdl3)
    if(SDL3_ADDED)
        set(SNX_SDL3_FROM_SOURCE ON)
        # its headers as system headers to the code that includes them, as FetchContent's SYSTEM gives them
        get_directory_property(snx_sdl3_targets DIRECTORY "${SDL3_SOURCE_DIR}" BUILDSYSTEM_TARGETS)
        set_target_properties(${snx_sdl3_targets} PROPERTIES SYSTEM ON)
        snx_quiet(SDL3-static)
        # ccache can't reuse a compile that read a precompiled header: under a launcher, SDL3 goes without one
        if(CMAKE_C_COMPILER_LAUNCHER)
            set_target_properties(SDL3-static PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
        endif()
    endif()
    endif()
endif()

# libdeflate (MIT), every deflate the project writes or reads: the titles a control.nacp keeps in deflate
# (src/common/utilities/common.c, so every build), the launcher's Share Log (src/gui/logzip.c), the Vita driver's VPK
# members and compressed eboot segments (src/vita/loader/inflate.c), and their CRC-32, a file's too (snx_crc32 in
# src/common/utilities/zip.c: the DS's ARM7 BIOS). Its library without gzip, from its own sources; it picks the CPU's
# SIMD loops at run time
AddJsonPackage(NAME libdeflate DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
add_library(snx_libdeflate STATIC
    "${libdeflate_SOURCE_DIR}/lib/utils.c" "${libdeflate_SOURCE_DIR}/lib/arm/cpu_features.c"
    "${libdeflate_SOURCE_DIR}/lib/x86/cpu_features.c" "${libdeflate_SOURCE_DIR}/lib/deflate_compress.c"
    "${libdeflate_SOURCE_DIR}/lib/deflate_decompress.c" "${libdeflate_SOURCE_DIR}/lib/adler32.c"
    "${libdeflate_SOURCE_DIR}/lib/zlib_compress.c" "${libdeflate_SOURCE_DIR}/lib/zlib_decompress.c"
    "${libdeflate_SOURCE_DIR}/lib/crc32.c")
target_include_directories(snx_libdeflate SYSTEM PUBLIC "${libdeflate_SOURCE_DIR}")
snx_quiet(snx_libdeflate)

# The Android app driver's text (src/android/hwui/), fetched only for a build with the driver: HarfBuzz (MIT) shapes
# it and gives the glyphs' outlines and metrics, built from its one-file amalgamation without FreeType. Its default
# family is the Noto Sans JP subset of the shared fonts (SIL OFL 1.1, above), which has Latin and the math signs too:
# the same package, so a build with the Switch driver fetches it once
if(SNX_BUILD_ANDROID AND NOT SNX_TOOLS_ONLY)
    AddJsonPackage(NAME harfbuzz DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    add_library(snx_harfbuzz STATIC "${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc")
    target_include_directories(snx_harfbuzz SYSTEM PUBLIC "${harfbuzz_SOURCE_DIR}/src")
    set_target_properties(snx_harfbuzz PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    snx_quiet(snx_harfbuzz)
    snx_package_file(noto-sans-jp-subset)
    # SQLite (public domain), the amalgamation, for android.database.sqlite (src/android/db/sqlite_jni.c). Its
    # options are the ones of Android's own build that apps can see: the FTS3, FTS4 and FTS5 modules, no extension
    # loading
    AddJsonPackage(NAME sqlite DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    add_library(snx_sqlite STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
    target_include_directories(snx_sqlite SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
    target_compile_definitions(snx_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_ENABLE_FTS3
                               SQLITE_ENABLE_FTS3_BACKWARDS SQLITE_ENABLE_FTS4 SQLITE_ENABLE_FTS5
                               SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=524288)
    snx_quiet(snx_sqlite)
endif()

# miniz (MIT) for the launcher's Share Log, the logs in one zip (src/gui/logzip.c): the zip around the deflate
# libdeflate makes. Its one-file release, miniz.c, built alone, without its stdio functions (the zip is made in memory
# and written as every file is, for Windows' UTF-8 paths) and without zlib's names, which another library's zlib could
# share
if(SNX_BUILD_GUI)
    AddJsonPackage(NAME miniz DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    add_library(snx_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
    target_include_directories(snx_miniz SYSTEM PUBLIC "${miniz_SOURCE_DIR}")
    target_compile_definitions(snx_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    snx_quiet(snx_miniz)
endif()

# GNU Unifont 18.0.01 (GPL-2.0-or-later with the font embedding exception, or OFL-1.1), the .hex of all its planes GNU
# releases: the menu draws the CJK and Hangul blocks of game titles with its glyphs, which scripts/build-glyphs.py
# takes from it (src/gui/CMakeLists.txt)
if(SNX_BUILD_GUI)
    snx_package_file(unifont)
    snx_cpmfile(snx_unifont_version unifont version)
    set(snx_unifont_hex "${Unifont_SOURCE_DIR}/unifont_all-${snx_unifont_version}.hex.gz")
endif()

# The boot splash's drawings are SVGs, drawn at the splash's size by lunasvg (MIT) on plutovg (MIT; its rasterizer and
# stroker come from FreeType, under the FreeType License, source/FTL.TXT). cmake/splash.cmake builds both. The runtime
# draws one too, the user's picture (hle/services/acc.c)
if(SNX_BUILD_GUI OR (SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY))
    AddJsonPackage(NAME plutovg DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
    AddJsonPackage(NAME lunasvg DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
endif()

# hactool, in the tree (src/nx/runtime/driver/hactool), which scarletnx --extract calls in its own process
if(SNX_BUILD_NX)
    include("${PROJECT_SOURCE_DIR}/cmake/hactool.cmake")
endif()
