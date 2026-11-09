# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A compiler cache is optional. When ccache is on PATH, the configure names it as the C and C++ compiler launcher, so a
# cold build after a clean reuses compiled output. sccache is used when ccache is not found: it suits a shared or remote
# cache, while ccache is faster for local C and C++. With neither installed the build is the same build, compiled from
# scratch. An explicit CMAKE_C_COMPILER_LAUNCHER or CMAKE_CXX_COMPILER_LAUNCHER (as CI gives ccache) is used as it is.
include_guard(GLOBAL)

set(SNX_COMPILER_CACHE "auto" CACHE STRING
    "Compiler cache for the C and C++ compiles: auto (ccache, then sccache), ccache, sccache, or off")
set_property(CACHE SNX_COMPILER_CACHE PROPERTY STRINGS auto ccache sccache off)

# CI names the launcher on each cmake line. A configure that names one keeps it, including a name this file would not
# choose.
if(CMAKE_C_COMPILER_LAUNCHER OR CMAKE_CXX_COMPILER_LAUNCHER)
    set(snx_launcher "${CMAKE_C_COMPILER_LAUNCHER}")
    if(NOT snx_launcher)
        set(snx_launcher "${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()
    message(STATUS "Compiler cache: ${snx_launcher}, set by the configure")
    return()
endif()

string(TOLOWER "${SNX_COMPILER_CACHE}" snx_compiler_cache)
if(snx_compiler_cache STREQUAL "off")
    message(STATUS "Compiler cache: off (SNX_COMPILER_CACHE=off)")
    return()
endif()

if(snx_compiler_cache STREQUAL "auto")
    # NO_CACHE: the search runs on every configure, so a tool that was removed is not named from a stale cache entry
    find_program(snx_ccache NAMES ccache NO_CACHE NO_CMAKE_FIND_ROOT_PATH)
    find_program(snx_sccache NAMES sccache NO_CACHE NO_CMAKE_FIND_ROOT_PATH)
    if(snx_ccache)
        set(snx_compiler_cache ccache)
        set(snx_compiler_cache_path "${snx_ccache}")
    elseif(snx_sccache)
        set(snx_compiler_cache sccache)
        set(snx_compiler_cache_path "${snx_sccache}")
    else()
        message(STATUS "Compiler cache: none found. ccache and sccache are optional, and the build compiles as it does "
                       "without one")
        return()
    endif()
elseif(snx_compiler_cache STREQUAL "ccache" OR snx_compiler_cache STREQUAL "sccache")
    find_program(snx_compiler_cache_path NAMES "${snx_compiler_cache}" NO_CACHE NO_CMAKE_FIND_ROOT_PATH)
else()
    message(WARNING "SNX_COMPILER_CACHE=${SNX_COMPILER_CACHE} is not auto, ccache, sccache or off. The build uses no "
                    "compiler cache")
    return()
endif()

if(NOT snx_compiler_cache_path)
    message(WARNING "SNX_COMPILER_CACHE=${snx_compiler_cache}, but ${snx_compiler_cache} was not found. The build "
                    "uses no compiler cache")
    return()
endif()

# The bare name, not the path find_program returned: FFmpeg, OpenSSL and jemalloc take the launcher through make, which
# finds it on PATH, and a path with a space would split there.
set(CMAKE_C_COMPILER_LAUNCHER "${snx_compiler_cache}")
set(CMAKE_CXX_COMPILER_LAUNCHER "${snx_compiler_cache}")
message(STATUS "Compiler cache: ${snx_compiler_cache}. It is optional, and SNX_COMPILER_CACHE=off disables it")
