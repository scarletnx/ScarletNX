# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# jemalloc as the malloc of the scarletnx program (SNX_JEMALLOC, cmake/dependencies.cmake): the static library built
# from the release source in cmake/dependencies.cmake with jemalloc's own configure and make, as OpenSSL is, and
# linked whole into the program (target snx_jemalloc). On Linux and FreeBSD it defines malloc, free and the rest, and
# the dynamic linker binds every library's calls to the program's definitions, LLVM's and the GPU driver's among them;
# on macOS, where its functions carry a je_ prefix, it puts its zone in front of the system's default one at startup.
# Only the library is built: no programs, docs or C++ operators (the C++ library's new calls malloc, which is
# jemalloc's). jemalloc is BSD-2-Clause. It needs GNU make (gmake on the BSDs).
# This file is included into the top-level scope: its variables carry the snx_jemalloc_ prefix.
set(snx_jemalloc_src "${jemalloc_SOURCE_DIR}")
set(snx_jemalloc_prefix "${PROJECT_BINARY_DIR}/jemalloc")
find_program(SNX_JEMALLOC_MAKE NAMES gmake make)
if(NOT SNX_JEMALLOC_MAKE)
    message(FATAL_ERROR "Building jemalloc (SNX_JEMALLOC=ON) needs GNU make (gmake on the BSDs) on the PATH. Configure "
                        "with -DSNX_JEMALLOC=OFF to keep the C library's malloc.")
endif()

# The largest page the target's kernels use, as a power of two: a jemalloc made for smaller pages stops at startup on
# a kernel with bigger ones ("Unsupported system page size"), and one made for bigger pages runs on smaller ones.
# AArch64 and PowerPC64 kernels come with 4, 16 or 64 KiB pages and LoongArch's with 16 KiB by default; Apple's arm64
# Macs have 16 KiB, the most a Mac has
if(APPLE)
    set(snx_jemalloc_lg_page 14)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|riscv64)$")
    set(snx_jemalloc_lg_page 12)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ppc64|ppc64le|powerpc64|powerpc64le|loongarch64)$")
    set(snx_jemalloc_lg_page 16)
else()
    message(FATAL_ERROR "SNX_JEMALLOC=ON for ${CMAKE_SYSTEM_PROCESSOR}: cmake/jemalloc.cmake knows no page size for "
                        "it. Configure with -DSNX_JEMALLOC=OFF to keep the C library's malloc.")
endif()
# The huge page is named too, 2 MiB: configure would read it from the /proc/meminfo of the machine doing the build
set(snx_jemalloc_args --with-lg-page=${snx_jemalloc_lg_page} --with-lg-hugepage=21 --disable-cxx)
if(CMAKE_CROSSCOMPILING)
    # a Linux cross build (cmake/cross.cmake): configure takes the target from --host and compiles for it with CC below
    list(APPEND snx_jemalloc_args --host=${CMAKE_C_COMPILER_TARGET})
endif()
# The compiler with what CMake's own compiles get beyond CMAKE_C_FLAGS: Apple's architecture, SDK and oldest system
# version (cmake/apple.cmake), a Linux cross build's target, sysroot and CPU baseline (cmake/cross.cmake). Configure's
# tests link programs, so a cross build's linker (LLD) goes in LDFLAGS
set(snx_jemalloc_cc "${CMAKE_C_COMPILER}")
foreach(flags IN LISTS SNX_APPLE_FLAGS SNX_TARGET_FLAGS)
    string(APPEND snx_jemalloc_cc " ${flags}")
endforeach()
list(JOIN SNX_TARGET_LDFLAGS " " snx_jemalloc_ldflags)
list(APPEND snx_jemalloc_args "CC=${snx_jemalloc_cc}" "LDFLAGS=${snx_jemalloc_ldflags}" "AR=${CMAKE_AR}"
     "NM=${CMAKE_NM}" "RANLIB=${CMAKE_RANLIB}")

# The build's compiler launcher (ccache) wraps make's compiles only, as for OpenSSL
set(snx_jemalloc_make_cc "")
list(JOIN CMAKE_C_COMPILER_LAUNCHER " " snx_jemalloc_launcher)
if(snx_jemalloc_launcher)
    set(snx_jemalloc_make_cc "CC=${snx_jemalloc_launcher} ${snx_jemalloc_cc}")
endif()

# Built once for every tree that builds it the same way, when SNX_DEPS_CACHE names a folder (cmake/deps_cache.cmake)
snx_deps_cached(jemalloc snx_jemalloc_cache "source: ${snx_jemalloc_sha256}" "configure: ${snx_jemalloc_args}")
if(snx_jemalloc_cache_FOUND)
    set(snx_jemalloc_prefix "${snx_jemalloc_cache_DIR}")
else()
    # jemalloc's make does not see the build's -j
    cmake_host_system_information(RESULT snx_jemalloc_jobs QUERY NUMBER_OF_LOGICAL_CORES)
    if("$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" MATCHES "^[1-9][0-9]*$")
        set(snx_jemalloc_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    include(ExternalProject)
    ExternalProject_Add(jemalloc_build
        SOURCE_DIR "${snx_jemalloc_src}"
        BINARY_DIR "${PROJECT_BINARY_DIR}/jemalloc-build"
        INSTALL_DIR "${snx_jemalloc_prefix}"
        CONFIGURE_COMMAND "${snx_jemalloc_src}/configure" --prefix=${snx_jemalloc_prefix} ${snx_jemalloc_args}
        # the install is part of the build step, as for OpenSSL: the library below is its byproduct. The _pic archive
        # is the one made of position-independent objects, which a PIE program links
        BUILD_COMMAND "${SNX_JEMALLOC_MAKE}" -j${snx_jemalloc_jobs} build_lib_static ${snx_jemalloc_make_cc}
        COMMAND "${SNX_JEMALLOC_MAKE}" install_lib_static
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${snx_jemalloc_prefix}/lib/libjemalloc_pic.a"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        LOG_OUTPUT_ON_FAILURE ON)
    snx_deps_publish(jemalloc_build jemalloc "${snx_jemalloc_prefix}" "${snx_jemalloc_cache_DIR}")
endif()

add_library(snx_jemalloc STATIC IMPORTED GLOBAL)
set_target_properties(snx_jemalloc PROPERTIES IMPORTED_LOCATION "${snx_jemalloc_prefix}/lib/libjemalloc_pic.a")
if(TARGET jemalloc_build)
    add_dependencies(snx_jemalloc jemalloc_build)
endif()
target_link_libraries(snx_jemalloc INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
