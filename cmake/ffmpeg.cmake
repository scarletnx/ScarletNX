# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# FFmpeg's libavcodec, libswresample and libavutil as static libraries (targets FFmpeg::avcodec, FFmpeg::swresample and
# FFmpeg::avutil), built from the release source in cmake/dependencies.cmake with FFmpeg's own configure and make, which
# it has instead of a CMake build. NVDEC's VP9 jobs need a change to its VP9 decoder (cmake/ffmpeg-vp9-nvdec.patch), so
# it builds from a patched copy of the release in the build tree. Only the decoders NVDEC handles are in (H.264, VP8,
# VP9), and Opus for hwopus (FFmpeg's Opus decoder resamples its SILK layer with libswresample, which is why that
# library is in): no programs, formats, filters, devices, network or autodetected system libraries, so the result needs
# nothing but libc and builds the same everywhere. All of it is LGPL (no --enable-gpl). It needs a POSIX shell and make
# (gmake on the BSDs), and NASM for the x86-64 assembly. macOS and iOS build it with Xcode's Clang and SDK
# (cmake/apple.cmake), iOS as a cross build.
# This file is included into the top-level scope: its variables carry the snx_ffmpeg_ prefix.
set(snx_ffmpeg_src "${CMAKE_BINARY_DIR}/_deps/ffmpeg-patched")
set(snx_ffmpeg_prefix "${CMAKE_BINARY_DIR}/ffmpeg")
# FFmpeg's configure stops on a source path with a space in it, which would only show at build time
if(snx_ffmpeg_src MATCHES "[ \t]")
    message(FATAL_ERROR "FFmpeg can't be built from a path with spaces in it (${snx_ffmpeg_src}). Use a build folder "
                        "whose path has none. FFmpeg is required by the NX runtime.")
endif()
set(snx_ffmpeg_patch "${CMAKE_SOURCE_DIR}/cmake/ffmpeg-vp9-nvdec.patch")
file(SHA256 "${snx_ffmpeg_patch}" snx_ffmpeg_patch_sha256)
include("${CMAKE_SOURCE_DIR}/cmake/patched_copy.cmake")
snx_patched_copy(SOURCE "${FFmpeg_SOURCE_DIR}" DEST "${snx_ffmpeg_src}" PATCHES "${snx_ffmpeg_patch}"
                 FILES libavcodec/vp9.c 459e8ca79e45680c98ba70ac574172e2389b177c6d19e5466fa63c3f0ff652e1)
# sh before bash: on Windows a bash.exe in System32 is WSL's launcher, and w64devkit's sh is the one to use
find_program(SNX_FFMPEG_SH NAMES sh bash)
find_program(SNX_FFMPEG_MAKE NAMES gmake make)
if(NOT SNX_FFMPEG_SH OR NOT SNX_FFMPEG_MAKE)
    message(FATAL_ERROR "Building FFmpeg needs a POSIX shell (sh) and GNU make (gmake on the BSDs) on the PATH. On "
                        "Windows, scripts\\bootstrap.ps1 installs them with w64devkit. FFmpeg is required by the NX runtime.")
endif()

# The build's compiler launcher (ccache) wraps make's compiles only: configure's checks are one-off compiles no cache
# hits, which through it took twice as long on Windows
list(JOIN CMAKE_C_COMPILER_LAUNCHER " " snx_ffmpeg_launcher)
set(snx_ffmpeg_make_cc "")
if(snx_ffmpeg_launcher)
    set(snx_ffmpeg_make_cc "CC=${snx_ffmpeg_launcher} ${CMAKE_C_COMPILER}")
endif()
set(snx_ffmpeg_args
    --enable-static
    --disable-shared
    --enable-pic
    --disable-autodetect
    --disable-everything
    --disable-programs
    --disable-doc
    --disable-network
    --disable-avdevice
    --disable-avformat
    --disable-avfilter
    --disable-swscale
    --disable-iconv
    --enable-decoder=h264,vp8,vp9,opus
    --cc=${CMAKE_C_COMPILER}
    --cxx=${CMAKE_CXX_COMPILER}
    --ar=${CMAKE_AR}
    --ranlib=${CMAKE_RANLIB}
    --nm=${CMAKE_NM}
    # FFmpeg's own warnings stay out of the build output
    --extra-cflags=-w)
if(WIN32 OR CMAKE_CROSSCOMPILING)
    # configure guesses the target from the machine doing the build, so a cross build names it. CMake names the
    # architecture AMD64 or ARM64 on Windows, which configure doesn't know
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_ffmpeg_arch x86_64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR
           (APPLE AND NOT CMAKE_SYSTEM_PROCESSOR AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
        # an iOS build names its architecture in CMAKE_OSX_ARCHITECTURES, CMAKE_SYSTEM_PROCESSOR being optional there
        set(snx_ffmpeg_arch aarch64)
    else()
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" snx_ffmpeg_arch)
    endif()
    if(WIN32)
        set(snx_ffmpeg_os mingw32)
    elseif(APPLE)
        # configure's name for macOS and iOS alike: the SDK in the flags below says which
        set(snx_ffmpeg_os darwin)
    else()
        # configure's names for Linux, Darwin, FreeBSD, Android are CMake's in lower case
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" snx_ffmpeg_os)
    endif()
    list(APPEND snx_ffmpeg_args --target-os=${snx_ffmpeg_os} --arch=${snx_ffmpeg_arch})
endif()
if(ANDROID)
    # the NDK's Clang takes its target and sysroot as flags, which configure does not get from --cc; and the runtime is
    # a shared library there (src/nx)
    list(APPEND snx_ffmpeg_args --sysroot=${CMAKE_SYSROOT} --extra-cflags=--target=${CMAKE_C_COMPILER_TARGET}
         --extra-ldflags=--target=${CMAKE_C_COMPILER_TARGET} --enable-pic)
endif()
if(SNX_APPLE_FLAGS)
    # the architecture, SDK and oldest system version CMake gives its own compiles (cmake/apple.cmake), which configure
    # does not get from --cc; on iOS without them it would build for the Mac. Its tests link programs, so the linker
    # gets them too
    list(JOIN SNX_APPLE_FLAGS " " snx_ffmpeg_apple_flags)
    list(APPEND snx_ffmpeg_args "--extra-cflags=${snx_ffmpeg_apple_flags}" "--extra-ldflags=${snx_ffmpeg_apple_flags}")
endif()
if(CMAKE_CROSSCOMPILING)
    # configure builds a few helper programs for the machine doing the build, which needs its own C compiler
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    list(APPEND snx_ffmpeg_args --enable-cross-compile --host-cc=${SNX_HOST_CC})
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # without its assembly FFmpeg decodes movies several times slower, which a build shouldn't do without being asked
    option(SNX_FFMPEG_ASM "Build FFmpeg's x86-64 assembly (needs NASM)" ON)
    if(SNX_FFMPEG_ASM)
        find_program(SNX_NASM nasm)
        if(NOT SNX_NASM)
            message(FATAL_ERROR "NASM was not found: FFmpeg needs it for its x86-64 assembly. Install nasm, or configure "
                                "with -DSNX_FFMPEG_ASM=OFF to build FFmpeg without it (movies decode more slowly).")
        endif()
        list(APPEND snx_ffmpeg_args --x86asmexe=${SNX_NASM})
        execute_process(COMMAND "${SNX_NASM}" -v OUTPUT_VARIABLE snx_ffmpeg_nasm OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
        list(APPEND snx_ffmpeg_args --disable-x86asm)
    endif()
endif()

# Built once for every tree that builds it the same way, when SNX_DEPS_CACHE names a folder (cmake/deps_cache.cmake).
# The install prefix stays out of the key: FFmpeg only quotes it in its configuration string
snx_deps_cached(ffmpeg snx_ffmpeg_cache "source: ${snx_ffmpeg_sha256}" "patch: ${snx_ffmpeg_patch_sha256}"
                "configure: ${snx_ffmpeg_args}" "nasm: ${snx_ffmpeg_nasm}")
if(snx_ffmpeg_cache_FOUND)
    set(snx_ffmpeg_prefix "${snx_ffmpeg_cache_DIR}")
else()
    # FFmpeg's make does not see the build's -j
    cmake_host_system_information(RESULT snx_ffmpeg_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    if("$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" MATCHES "^[1-9][0-9]*$")
        set(snx_ffmpeg_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    include(ExternalProject)
    ExternalProject_Add(ffmpeg_build
        SOURCE_DIR "${snx_ffmpeg_src}"
        # configure leaves a src link to its source there: a tree that built the release's own folder before would
        # keep compiling that one
        BINARY_DIR "${CMAKE_BINARY_DIR}/ffmpeg-build-patched"
        INSTALL_DIR "${snx_ffmpeg_prefix}"
        CONFIGURE_COMMAND "${SNX_FFMPEG_SH}" "${snx_ffmpeg_src}/configure" --prefix=${snx_ffmpeg_prefix}
                          ${snx_ffmpeg_args}
        # make install is part of the build step: the libraries below are its byproducts, which Ninja checks again as
        # soon as the step ends. Installed by a later step, a rebuilt FFmpeg still looked unchanged then, and the
        # runtime kept its link to the old one until the next build
        BUILD_COMMAND "${SNX_FFMPEG_MAKE}" -j${snx_ffmpeg_jobs} ${snx_ffmpeg_make_cc}
        COMMAND "${SNX_FFMPEG_MAKE}" install
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${snx_ffmpeg_prefix}/lib/libavcodec.a" "${snx_ffmpeg_prefix}/lib/libswresample.a"
                         "${snx_ffmpeg_prefix}/lib/libavutil.a"
        # configure and make print a lot: keep it in log files, shown when a step fails
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        LOG_OUTPUT_ON_FAILURE ON)
    # a changed patch rewrites the patched file at configure time, which the build step doesn't see on its own: an
    # existing tree kept linking the FFmpeg built from the old one
    ExternalProject_Add_StepDependencies(ffmpeg_build build "${snx_ffmpeg_src}/libavcodec/vp9.c")
    snx_deps_publish(ffmpeg_build ffmpeg "${snx_ffmpeg_prefix}" "${snx_ffmpeg_cache_DIR}")
endif()

# The include folder is filled by the install step, but has to exist for the imported targets at configure time
file(MAKE_DIRECTORY "${snx_ffmpeg_prefix}/include")
add_library(FFmpeg::avutil STATIC IMPORTED GLOBAL)
# SYSTEM OFF: as a system folder (-isystem) the headers would come after every -I, and a -I that holds another
# FFmpeg's headers (/usr/local/include on the BSDs) would win over the ones these libraries were built with
set_target_properties(FFmpeg::avutil PROPERTIES IMPORTED_LOCATION "${snx_ffmpeg_prefix}/lib/libavutil.a"
                                                INTERFACE_INCLUDE_DIRECTORIES "${snx_ffmpeg_prefix}/include"
                                                SYSTEM OFF)
if(TARGET ffmpeg_build)
    add_dependencies(FFmpeg::avutil ffmpeg_build)
endif()
if(WIN32)
    # libavutil's random seed uses BCryptGenRandom
    target_link_libraries(FFmpeg::avutil INTERFACE bcrypt)
else()
    target_link_libraries(FFmpeg::avutil INTERFACE Threads::Threads m)
endif()
add_library(FFmpeg::swresample STATIC IMPORTED GLOBAL)
set_target_properties(FFmpeg::swresample PROPERTIES IMPORTED_LOCATION "${snx_ffmpeg_prefix}/lib/libswresample.a"
                                                    SYSTEM OFF)
target_link_libraries(FFmpeg::swresample INTERFACE FFmpeg::avutil)
if(TARGET ffmpeg_build)
    add_dependencies(FFmpeg::swresample ffmpeg_build)
endif()
add_library(FFmpeg::avcodec STATIC IMPORTED GLOBAL)
set_target_properties(FFmpeg::avcodec PROPERTIES IMPORTED_LOCATION "${snx_ffmpeg_prefix}/lib/libavcodec.a" SYSTEM OFF)
# the Opus decoder in libavcodec calls libswresample
target_link_libraries(FFmpeg::avcodec INTERFACE FFmpeg::swresample FFmpeg::avutil)
if(TARGET ffmpeg_build)
    add_dependencies(FFmpeg::avcodec ffmpeg_build)
endif()
