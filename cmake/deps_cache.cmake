# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# FFmpeg and OpenSSL, which build with their own configure and make (cmake/ffmpeg.cmake, cmake/openssl.cmake), built
# once for every build tree that would build them the same way. With SNX_DEPS_CACHE naming a folder, each build of
# them lives there in a folder named after a hash of all that goes into it: the source's SHA-256, the configure line,
# the compiler and its version, the text of the system's C headers as the compiler reads them, and the cmake file
# that builds it. A tree whose hash finds a finished folder uses it and builds neither; one that finds none builds as
# without the cache and then publishes what it installed. A change to any of those inputs is another folder, so a
# stale build is never picked up, and folders of older inputs are only left behind. Without SNX_DEPS_CACHE, each tree
# builds its own. The environment's SNX_DEPS_CACHE is the default, as CI sets it.
set(SNX_DEPS_CACHE "$ENV{SNX_DEPS_CACHE}" CACHE PATH
    "Folder where FFmpeg and OpenSSL, once built, are kept for every build tree to reuse")

# The system's C headers as <compiler> reads them with the flags after it: a toolchain update that changes them changes
# every key
function(snx_deps_headers_hash out compiler)
    set(probe "${PROJECT_BINARY_DIR}/snx-deps/headers.c")
    file(WRITE "${probe}" "#include <errno.h>\n#include <limits.h>\n#include <math.h>\n#include <stddef.h>\n"
                          "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                          "#include <time.h>\n#ifdef _WIN32\n#include <windows.h>\n#else\n#include <pthread.h>\n"
                          "#include <unistd.h>\n#endif\n")
    # -P: the text without line markers, which would put this tree's path in it
    execute_process(COMMAND "${compiler}" ${ARGN} -E -P "${probe}" OUTPUT_VARIABLE text ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "SNX_DEPS_CACHE: ${compiler} could not read the system's C headers:\n${err}")
    endif()
    string(SHA256 hash "${text}")
    set(${out} "${hash}" PARENT_SCOPE)
endfunction()

# snx_deps_cached(<name> <out> [HOST <C compiler>] <key text>...): where this build of <name> lives. <out>_FOUND is
# TRUE when a finished build is in SNX_DEPS_CACHE (use <out>_DIR, build nothing), else FALSE (build it in the tree;
# <out>_DIR, when set, is where snx_deps_publish puts it). The cmake file calling this is part of the key, with the
# compiler and the headers: the target's, or with HOST, for a program the build runs, the build machine's compiler's
# without the target's flags, so every target built on one machine shares it. The tree's snx-deps/<name>.dir names
# the folder, for a CI cache to keep the ones a run used
function(snx_deps_cached name out)
    set(${out}_FOUND FALSE PARENT_SCOPE)
    set(${out}_DIR "" PARENT_SCOPE)
    if(NOT SNX_DEPS_CACHE)
        return()
    endif()
    set(texts ${ARGN})
    list(LENGTH texts count)
    if(count GREATER 1)
        list(GET texts 0 first)
    else()
        set(first "")
    endif()
    if(first STREQUAL "HOST")
        list(GET texts 1 compiler)
        list(REMOVE_AT texts 0 1)
        execute_process(COMMAND "${compiler}" --version OUTPUT_VARIABLE identity ERROR_QUIET)
        string(REGEX REPLACE "\n.*" "" identity "${identity}")
        set(flags "")
        set(property "snx_deps_headers:${compiler}")
    else()
        set(compiler "${CMAKE_C_COMPILER}")
        set(identity "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
        set(flags "")
        if(CMAKE_C_COMPILER_TARGET)
            list(APPEND flags "--target=${CMAKE_C_COMPILER_TARGET}")
        endif()
        if(CMAKE_SYSROOT)
            list(APPEND flags "--sysroot=${CMAKE_SYSROOT}")
        endif()
        # the CPU and ABI the dependencies are built for (cmake/cross.cmake): big-endian PowerPC64 glibc is ELFv1 to
        # Clang without -mabi=elfv2, and an ELFv2 root (Arch POWER's) has no ELFv1 headers
        list(APPEND flags ${SNX_ARCH_FLAGS})
        set(property snx_deps_headers)
    endif()
    get_property(headers GLOBAL PROPERTY "${property}")
    if(NOT headers)
        snx_deps_headers_hash(headers "${compiler}" ${flags})
        set_property(GLOBAL PROPERTY "${property}" "${headers}")
    endif()
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" recipe)
    string(JOIN "\n" key "${name}" ${texts} "compiler: ${identity}" "headers: ${headers}" "recipe: ${recipe}")
    string(SHA256 hash "${key}")
    string(SUBSTRING "${hash}" 0 16 hash)
    set(dir "${SNX_DEPS_CACHE}/${name}-${hash}")
    file(WRITE "${PROJECT_BINARY_DIR}/snx-deps/${name}.key" "${key}\n")
    file(WRITE "${PROJECT_BINARY_DIR}/snx-deps/${name}.dir" "${name}-${hash}\n")
    set(${out}_DIR "${dir}" PARENT_SCOPE)
    if(EXISTS "${dir}/snx-complete")
        message(STATUS "${name}: the build in ${dir}")
        set(${out}_FOUND TRUE PARENT_SCOPE)
    endif()
endfunction()

# snx_deps_publish(<ExternalProject> <name> <installed prefix> <dir>): after the install step, a copy of the prefix
# goes to <dir> (cmake/deps_publish.cmake). Nothing when <dir> is empty (no SNX_DEPS_CACHE)
function(snx_deps_publish project name prefix dir)
    if(NOT dir)
        return()
    endif()
    ExternalProject_Add_Step(${project} publish
        COMMAND "${CMAKE_COMMAND}" "-DFROM=${prefix}" "-DTO=${dir}" "-DKEY=${PROJECT_BINARY_DIR}/snx-deps/${name}.key"
                -P "${PROJECT_SOURCE_DIR}/cmake/deps_publish.cmake"
        COMMENT "Keeping ${name} in ${dir}"
        DEPENDEES install)
endfunction()
