# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DZLIB=<zlib source> -DWORK=<scratch dir> -DGENERATOR=<generator> -DMAKE=<make program>
#         -DCC=<C compiler> -DCXX=<C++ compiler> -P tests/zlib_lto_test.cmake
#
# FFmpeg's configure finds the build's zlib by linking a program with -lz and no -flto (check_lib zlib zlib.h
# zlibVersion -lz; cmake/ffmpeg.cmake), so libz.a has to be machine code with SNX_LTO on too. A small project with
# SNX_LTO=ON builds snx_zlib from cmake/zlib.cmake as RelWithDebInfo; the archive must hold no LLVM bitcode, and the
# program configure writes for that check must link against it as configure links it. Where cmake/lto.cmake leaves
# ThinLTO off for this toolchain there is nothing to check: the test says so, with lto.cmake's reason, and ctest counts
# it skipped.
foreach(var SRC ZLIB WORK GENERATOR MAKE CC CXX)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "zlib_lto_test.cmake needs -D${var}=...")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.28)
project(zlib_lto_test LANGUAGES C CXX)
include(\"${SRC}/cmake/lto.cmake\")
# cmake/dependencies.cmake's
function(snx_quiet)
    foreach(target IN LISTS ARGN)
        target_compile_options(\${target} PRIVATE -w)
    endforeach()
endfunction()
set(zlib_SOURCE_DIR \"${ZLIB}\")
include(\"${SRC}/cmake/zlib.cmake\")
file(WRITE \"\${CMAKE_BINARY_DIR}/lto.txt\" \"\${SNX_LTO_GENEX}\")
file(WRITE \"\${CMAKE_BINARY_DIR}/libdir.txt\" \"\${snx_zlib_lib_dir}\")
")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/build" -G "${GENERATOR}"
                        "-DCMAKE_MAKE_PROGRAM=${MAKE}" "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_CXX_COMPILER=${CXX}"
                        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSNX_LTO=ON
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the project with cmake/zlib.cmake did not configure (exit ${result}):\n${out}")
endif()
file(READ "${WORK}/build/lto.txt" lto)
if(lto STREQUAL "0")
    string(REGEX MATCH "ThinLTO: off[^\n]*" why "${out}")
    message("zlib_lto skipped: cmake/lto.cmake left ThinLTO off for ${CC}, so no target is bitcode here (${why})")
    return()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK}/build" --config RelWithDebInfo --target snx_zlib
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "snx_zlib did not build with SNX_LTO=ON (exit ${result}):\n${out}")
endif()
file(READ "${WORK}/build/libdir.txt" libdir)
set(archive "${libdir}/libz.a")
if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "snx_zlib built no ${archive}, where cmake/ffmpeg.cmake's -L${libdir} looks for -lz")
endif()

# a member that is LLVM bitcode: the end of its ar header ('`' '\n') and the bitcode's start ('B' 'C' 0xC0 0xDE), at
# a whole byte of the hex text
file(READ "${archive}" archive_hex HEX)
set(rest "${archive_hex}")
set(skipped 0)
set(bitcode_at -1)
while(TRUE)
    string(FIND "${rest}" "600a4243c0de" at)
    if(at EQUAL -1)
        break()
    endif()
    math(EXPR nibble "${skipped} + ${at}")
    math(EXPR odd "${nibble} % 2")
    if(odd EQUAL 0)
        math(EXPR bitcode_at "${nibble} / 2 + 2")
        break()
    endif()
    math(EXPR next "${at} + 1")
    string(SUBSTRING "${rest}" ${next} -1 rest)
    math(EXPR skipped "${skipped} + ${next}")
endwhile()
if(bitcode_at GREATER -1)
    message(SEND_ERROR "${archive} holds LLVM bitcode with SNX_LTO=ON (a member from byte ${bitcode_at}): snx_zlib "
                       "got ThinLTO, which FFmpeg's configure cannot link")
endif()

# check_func_headers' program for zlib.h and zlibVersion, compiled and linked as configure does it: the compiler alone,
# the include and the library folder cmake/ffmpeg.cmake gives it, then -lz
file(WRITE "${WORK}/check.c" "#include <zlib.h>
#include <stdint.h>
long check_zlibVersion(void) { return (long) zlibVersion; }
int main(void) { int ret = 0;
 ret |= ((intptr_t)check_zlibVersion) & 0xFFFF;
return ret; }
")
execute_process(COMMAND "${CC}" -w "-I${ZLIB}" -c "${WORK}/check.c" -o "${WORK}/check.o"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "configure's zlib check did not compile against ${ZLIB}/zlib.h (exit ${result}):\n${out}")
endif()
execute_process(COMMAND "${CC}" "-L${libdir}" "${WORK}/check.o" -o "${WORK}/check" -lz
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT result EQUAL 0)
    message(SEND_ERROR "configure's zlib check did not link against ${archive} with -lz and no -flto (exit "
                       "${result}), so FFmpeg's configure would stop with \"zlib requested but not found\":\n${out}")
endif()
if(bitcode_at GREATER -1 OR NOT result EQUAL 0)
    message(FATAL_ERROR "libz.a is not what FFmpeg's configure can link with SNX_LTO=ON")
endif()
message("with SNX_LTO=ON, ${archive} is machine code and configure's zlib check links against it with -lz")
