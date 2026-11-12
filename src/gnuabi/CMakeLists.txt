# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# the kernel's threads, futexes and signals (kernel/CMakeLists.txt), which the Android bridge uses too
if(NOT TARGET scarletnx-gnuabi-kernel)
    add_subdirectory(kernel)
endif()

add_library(scarletnx-gnuabi STATIC
    abi/syscalls/syscalls.c loader/elf/elf.c memory/memory.c process/process.c
    process/threads.c process/host_signals.c runtime/runtime.c runtime/execution.c runtime/arm_exec.c runtime/jit.c
    process/spawn.c filesystem/vfs.c filesystem/devfs/devfs.c filesystem/tmpfs/tmpfs.c filesystem/procfs/procfs.c
    filesystem/hostfs/hostfs.c
    network/socket.c network/poll.c network/etc.c)
set_target_properties(scarletnx-gnuabi PROPERTIES OUTPUT_NAME scarletnx-gnuabi-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
# AArch32 execution needs the ARM LLVM block backend. Build it here when the NX
# runtime (which normally owns it) is not part of this configuration.
if(NOT TARGET snx_arm_llvm AND NOT SNX_BUILD_NX)
    include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" gnuabi-arm-backend)
endif()
# The JIT compiles through the recompiler library (snx_recompiler.h), which the NX driver's tree configures. Without
# the NX runtime, configure it here
if(NOT TARGET scarletnx-recompiler)
    include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm" gnuabi-llvm-backend)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler" gnuabi-recompiler-library)
endif()
target_link_libraries(scarletnx-gnuabi PUBLIC scarletnx-gnuabi-kernel scarletnx-common scarletnx-net snx_arm_llvm
                      scarletnx-recompiler)
target_link_libraries(scarletnx-gnuabi PUBLIC OpenSSL::SSL)
target_compile_options(scarletnx-gnuabi PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(scarletnx-gnuabi PRIVATE _CRT_SECURE_NO_WARNINGS)
if(WIN32)
    target_link_libraries(scarletnx-gnuabi PRIVATE bcrypt)
endif()
# The command line (runtime/main.c's snx_gnuabi_main), which scarletnx runs as scarletnx-gnuabi (src/multicall). Its
# checkout, where an older version kept the default sysroot, linux/ (snx_checkout)
target_sources(scarletnx-gnuabi PRIVATE runtime/main.c)
target_compile_definitions(scarletnx-gnuabi PRIVATE SNX_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                           SNX_BUILD_DIR="${PROJECT_BINARY_DIR}")
include("${PROJECT_SOURCE_DIR}/tests/cmake/gnuabi.cmake") # its tests
