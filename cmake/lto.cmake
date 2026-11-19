# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# ThinLTO (SNX_LTO): the targets compile to LLVM bitcode, and the link optimizes across files. On a Steam Deck it cut
# the GPU thread's time for each frame by 6.3% (37.9 ms to 35.5 ms over 20 race frames, every run faster).
#
# It is off by default: the links take longer, and the builds of development and of pull requests run many times a
# day. -DSNX_LTO=ON turns it on in the optimized configurations (Release, RelWithDebInfo, MinSizeRel), never in Debug,
# and only where CheckIPOSupported links a test project with it. CI's nightlies and releases turn it on for Linux
# x86-64, Windows and Android's arm64-v8a (.github/workflows/build.yml). Included before the packages, so the CPM
# packages that build through their CMake (SDL3, zstd, lz4 and the others) get it too, as in the build that was
# measured. FFmpeg, OpenSSL, jemalloc and curl build with their own configure, and do not.
#
# Where SNX_LTO=ON does not work, or the check cannot tell:
#   - Apple (the macos and ios presets): CMake has no IPO for Swift, which the apps link, so an IPO configuration
#     stops at the generate step. A dSYM of LTO code also needs the linker's -object_path_lto.
#   - Linux and NetBSD cross builds (the linux-*-cross and netbsd presets): the check builds its test project without
#     the preset's -fuse-ld, so it does not test the linker that the build uses. Those presets with -fuse-ld=bfd also
#     need an LLVMgold.so of the cross Clang's version.
#   - SNX_TEST_SANITIZERS with a linker other than lld (below).
#
# Two kinds of output must stay machine code, so they never get it:
#   - the installed archive scarletnx-recompiler and the objects it is made of (snx_no_lto in
#     src/common/cpu/recompiler/CMakeLists.txt): a consumer links it with a linker that may not read this Clang's
#     bitcode.
#   - the runtime library of the game modules (rtlib and armlib in src/common/cpu/recompiler/llvm/): custom commands
#     that compile with their own flags, which this file does not change.
include_guard(GLOBAL)

# SNX_LTO_GENEX: true in a configuration whose targets compile to bitcode, set below where ThinLTO is on. A test that
# reads an object's machine code reads the linked program there (tests/CMakeLists.txt, arm_dispatch_frame)
set(SNX_LTO_GENEX 0)

option(SNX_LTO "Link the optimized configurations with ThinLTO where the toolchain supports it (never Debug)" OFF)

# snx_no_lto(<target>...): keep the targets machine code in every configuration
function(snx_no_lto)
    foreach(target IN LISTS ARGN)
        set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)
    endforeach()
endfunction()

if(NOT SNX_LTO)
    message(STATUS "ThinLTO: off (SNX_LTO=OFF, -DSNX_LTO=ON turns it on)")
    return()
endif()
if(APPLE)
    message(FATAL_ERROR "SNX_LTO=ON: CMake has no IPO for Swift, which the Apple apps link. Configure with "
                        "-DSNX_LTO=OFF")
endif()
# ASan gives each instrumented object a common ___asan_globals_registered. GNU ld's LTO plugin then takes from an
# archive every bitcode member that has one, needed or not: snx-test-3ds-board linked the sections of the Switch
# runtime's settings, and their setters were undefined. lld takes only the members that a reference needs
if(SNX_TEST_SANITIZERS AND NOT CMAKE_C_COMPILER_LINKER_ID STREQUAL "LLD")
    message(STATUS "ThinLTO: off with SNX_TEST_SANITIZERS and this linker (${CMAKE_C_COMPILER_LINKER_ID}): it pulls "
                   "every archive member with ASan's common symbol. lld does not")
    return()
endif()
include(CheckIPOSupported)
check_ipo_supported(RESULT snx_lto_ok OUTPUT snx_lto_output LANGUAGES C CXX)
if(NOT snx_lto_ok)
    message(STATUS "ThinLTO: off, this toolchain did not link CheckIPOSupported's test project with it. Its output:\n"
                   "${snx_lto_output}")
    return()
endif()
foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${config} ON)
endforeach()
set(SNX_LTO_GENEX "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
# The link keeps the machine code of each bitcode module in a cache, by its contents and imports: a link after an edit
# compiles again only the modules that it changed. An edit to one file of the renderer took 50 s to build without it,
# 18 s with it, and 4 s without ThinLTO. A whole build fills about 420 MB, and the cache keeps 2 GB at most. The linker
# ID is CMake 3.29's. GNU ld runs LLVM's gold plugin, which takes the cache's options as plugin options, and stops on
# them in a link without the plugin: only the links of LTO targets get them (not snx_no_lto's)
set(snx_lto_cache "${PROJECT_BINARY_DIR}/thinlto-cache")
set(snx_lto_target "$<BOOL:$<TARGET_PROPERTY:INTERPROCEDURAL_OPTIMIZATION_$<UPPER_CASE:$<CONFIG>>>>")
if(CMAKE_C_COMPILER_LINKER_ID STREQUAL "LLD")
    add_link_options("$<${snx_lto_target}:LINKER:--thinlto-cache-dir=${snx_lto_cache}>"
                     "$<${snx_lto_target}:LINKER:--thinlto-cache-policy=cache_size_bytes=2g>")
elseif(CMAKE_C_COMPILER_LINKER_ID MATCHES "^GNU(gold)?$")
    add_link_options("$<${snx_lto_target}:LINKER:-plugin-opt,cache-dir=${snx_lto_cache}>"
                     "$<${snx_lto_target}:LINKER:-plugin-opt,cache-policy=cache_size_bytes=2g>")
endif()
message(STATUS "ThinLTO: on for Release, RelWithDebInfo and MinSizeRel (SNX_LTO)")
