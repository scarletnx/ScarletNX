# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The Android app driver (docs/ANDROID_APPS.md): an APK's DEX code translated to JVM classes, run by a JVM beside
# ScarletNX's own android.* and the HLE of Android's userland, which scarletnx runs as scarletnx-android
# (src/multicall).
#
#   snx_android_dex       the DEX reader and its translator to class files (dex/)
#   snx_android_program   the command line (runtime/main.c)
add_library(snx_android_dex STATIC dex/dex.c dex/classfile.c dex/translate.c)
target_link_libraries(snx_android_dex PUBLIC scarletnx-common)
set_target_properties(snx_android_dex PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_android_dex PRIVATE -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes
                                               -Werror=missing-variable-declarations)

add_library(snx_android_program STATIC runtime/main.c)
target_link_libraries(snx_android_program PUBLIC snx_android_dex snx_zip)
set_target_properties(snx_android_program PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_android_program PRIVATE -pedantic-errors -Wall -Wextra -Werror
                                                   -Werror=missing-prototypes -Werror=missing-variable-declarations)
