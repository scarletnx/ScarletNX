# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The Linux kernel's threads, futexes and signals for an AArch64 Linux guest (kernel.h): a library of its own,
# which the GNU ABI runtime and the Android bridge both use. It needs only the common code's signal ABI
add_library(scarletnx-gnuabi-kernel STATIC
    sync.c thread.c futex.c signals.c syscall.c system.c host.c errno.c)
set_target_properties(scarletnx-gnuabi-kernel PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(scarletnx-gnuabi-kernel PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(scarletnx-gnuabi-kernel PUBLIC scarletnx-common)
target_compile_options(scarletnx-gnuabi-kernel PRIVATE -Wall -Wextra -Werror)
if(WIN32)
    target_link_libraries(scarletnx-gnuabi-kernel PUBLIC bcrypt) # BCryptGenRandom
else()
    find_package(Threads REQUIRED)
    target_link_libraries(scarletnx-gnuabi-kernel PUBLIC Threads::Threads)
endif()
