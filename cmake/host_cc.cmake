# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A cross build also builds programs that run during the build (FFmpeg's configure helpers, swipc), for the machine
# doing it: SNX_HOST_CC is that machine's C compiler, clang or else cc (any compiler will do for these tools)
include_guard(GLOBAL)
# On a Mac, Xcode's: another clang first on PATH, as the NDK's, finds no macOS SDK, and FFmpeg's host checks then find
# no libc headers. /usr/bin/clang finds it through xcrun
if(CMAKE_HOST_APPLE)
    find_program(SNX_HOST_CC NAMES clang PATHS /usr/bin NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
find_program(SNX_HOST_CC NAMES clang cc)
if(NOT SNX_HOST_CC)
    message(FATAL_ERROR "A cross build builds programs for this machine too (FFmpeg's helpers, swipc), which needs a C "
                        "compiler for it (clang or cc). Install one or set SNX_HOST_CC to its path.")
endif()
