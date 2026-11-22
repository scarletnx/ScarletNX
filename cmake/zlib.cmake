# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# zlib (zlib license) as the static library snx_zlib, from the release source in cmake/dependencies.cmake
# (zlib_SOURCE_DIR): FFmpeg's PNG decoder and encoder call its inflate and deflate, and the Android driver's libz.so is
# the NDK's zlib that an app's native code links (src/android/bridge/hle_zlib.c). libdeflate has no stream interface,
# so they get zlib itself: static, as libz.a in a folder of its own that FFmpeg's configure finds with -lz
# (cmake/ffmpeg.cmake). This file is included into the top-level scope: its variables carry the snx_zlib_ prefix.
add_library(snx_zlib STATIC
    "${zlib_SOURCE_DIR}/adler32.c" "${zlib_SOURCE_DIR}/compress.c" "${zlib_SOURCE_DIR}/crc32.c"
    "${zlib_SOURCE_DIR}/deflate.c" "${zlib_SOURCE_DIR}/infback.c" "${zlib_SOURCE_DIR}/inffast.c"
    "${zlib_SOURCE_DIR}/inflate.c" "${zlib_SOURCE_DIR}/inftrees.c" "${zlib_SOURCE_DIR}/trees.c"
    "${zlib_SOURCE_DIR}/uncompr.c" "${zlib_SOURCE_DIR}/zutil.c" "${zlib_SOURCE_DIR}/gzclose.c"
    "${zlib_SOURCE_DIR}/gzlib.c" "${zlib_SOURCE_DIR}/gzread.c" "${zlib_SOURCE_DIR}/gzwrite.c")
target_include_directories(snx_zlib SYSTEM PUBLIC "${zlib_SOURCE_DIR}")
if(NOT WIN32)
    # what zlib's own configure finds: gz*.c's read, write and close
    target_compile_definitions(snx_zlib PRIVATE Z_HAVE_UNISTD_H)
endif()
# position independent: FFmpeg is built with --enable-pic, and the Android runtime is a shared library. The folder is a
# generator expression so that a multi-config generator (Xcode) adds no folder for the configuration to it
set(snx_zlib_lib_dir "${PROJECT_BINARY_DIR}/zlib/lib")
set_target_properties(snx_zlib PROPERTIES OUTPUT_NAME z POSITION_INDEPENDENT_CODE ON
                                          ARCHIVE_OUTPUT_DIRECTORY "$<1:${snx_zlib_lib_dir}>")
# machine code with SNX_LTO too: FFmpeg's configure links its zlib check with -lz and no -flto, and GNU ld without
# LLVM's plugin reads no bitcode, so configure stopped with "zlib requested but not found" (tests/zlib_lto_test.cmake)
snx_no_lto(snx_zlib)
snx_quiet(snx_zlib)
