# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# dav1d's libdav1d as a static library (target dav1d::dav1d), for FFmpeg's libdav1d decoder: the AV1 movies of the
# Ounce's games, which the native video service decodes (src/nx/hle/services/native_video.c). FFmpeg's own AV1 decoder
# only parses the stream for a hardware decoder (Vulkan Video's, with the vulkan_video setting: cmake/ffmpeg.cmake), so
# a software decode needs dav1d. It builds from the release in
# cmake/dependencies.cmake with meson and ninja, which it has instead of a CMake build: only the library, at 8 and 16
# bits a sample (8- and 10-bit streams), with its assembly (NASM's on x86-64). Built on Linux, the BSDs and Windows (MSYS2's
# meson, as its CI builds), not in a cross build yet (meson would need a cross file of its own); a runtime without it plays
# an AV1 movie black. This file is
# included into the top-level scope before cmake/ffmpeg.cmake, which reads snx_dav1d_ready: its variables carry the
# snx_dav1d_ prefix.
option(SNX_DAV1D "Build dav1d, for the Ounce's AV1 movies (needs meson and ninja)" ON)
set(snx_dav1d_ready OFF)
if(SNX_DAV1D)
    find_program(SNX_MESON meson)
    find_program(SNX_DAV1D_NINJA NAMES ninja ninja-build)
    if(CMAKE_CROSSCOMPILING OR APPLE OR ANDROID)
        message(STATUS "dav1d: not built for this target yet: AV1 movies play black")
    elseif(NOT SNX_MESON OR NOT SNX_DAV1D_NINJA)
        message(STATUS "dav1d: meson or ninja not found: AV1 movies play black. Install them, or configure with "
                       "-DSNX_DAV1D=OFF to silence this")
    else()
        set(snx_dav1d_ready ON)
    endif()
endif()

if(snx_dav1d_ready)
    set(snx_dav1d_prefix "${PROJECT_BINARY_DIR}/dav1d")
    set(snx_dav1d_build "${PROJECT_BINARY_DIR}/dav1d-build/meson")
    # where its dav1d.pc goes, which FFmpeg's configure reads (cmake/ffmpeg.cmake): meson's pkgconfig module puts it
    # in libdata/pkgconfig on FreeBSD, whatever --libdir says, and in the libdir's pkgconfig elsewhere
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "FreeBSD")
        set(snx_dav1d_pkgconfig "${snx_dav1d_prefix}/libdata/pkgconfig")
    else()
        set(snx_dav1d_pkgconfig "${snx_dav1d_prefix}/lib/pkgconfig")
    endif()
    # its x86-64 assembly needs NASM, as FFmpeg's does; other architectures' is assembled by the compiler
    set(snx_dav1d_asm true)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        find_program(SNX_NASM nasm)
        if(NOT SNX_NASM)
            set(snx_dav1d_asm false)
            message(STATUS "dav1d: NASM not found, built without its x86-64 assembly (AV1 movies decode more slowly)")
        endif()
    endif()
    include(ExternalProject)
    ExternalProject_Add(dav1d_build
        SOURCE_DIR "${dav1d_SOURCE_DIR}"
        BINARY_DIR "${PROJECT_BINARY_DIR}/dav1d-build"
        INSTALL_DIR "${snx_dav1d_prefix}"
        # meson refuses to set up a folder it set up before: the configure step starts from an empty one, and only runs
        # again when the source or these arguments change
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E rm -rf "${snx_dav1d_build}"
        COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "${SNX_MESON}" setup "${snx_dav1d_build}"
                "${dav1d_SOURCE_DIR}" --prefix=${snx_dav1d_prefix} --libdir=lib --buildtype=release
                --default-library=static -Db_staticpic=true -Dbitdepths=8,16 -Denable_asm=${snx_dav1d_asm}
                -Denable_tools=false -Denable_examples=false -Denable_tests=false -Denable_docs=false
        # installed in the build step, as FFmpeg's is (cmake/ffmpeg.cmake): Ninja checks the library as soon as it ends
        BUILD_COMMAND "${SNX_DAV1D_NINJA}" -C "${snx_dav1d_build}"
        COMMAND "${SNX_DAV1D_NINJA}" -C "${snx_dav1d_build}" install
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${snx_dav1d_prefix}/lib/libdav1d.a"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_OUTPUT_ON_FAILURE ON)
    file(MAKE_DIRECTORY "${snx_dav1d_prefix}/include")
    add_library(dav1d::dav1d STATIC IMPORTED GLOBAL)
    set_target_properties(dav1d::dav1d PROPERTIES IMPORTED_LOCATION "${snx_dav1d_prefix}/lib/libdav1d.a"
                                                  INTERFACE_INCLUDE_DIRECTORIES "${snx_dav1d_prefix}/include")
    # its threads, dlsym (for the C library's thread stack size) and libm; on Windows its threads are the system's own
    if(NOT WIN32)
        target_link_libraries(dav1d::dav1d INTERFACE Threads::Threads m ${CMAKE_DL_LIBS})
    endif()
    add_dependencies(dav1d::dav1d dav1d_build)
endif()
