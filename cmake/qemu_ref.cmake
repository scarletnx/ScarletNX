# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The QEMU reference (tests/qemu_ref/): its AArch64 runner, built freestanding by the build's Clang for
# aarch64-linux-gnu and linked by ld.lld, runs under qemu-aarch64. On a Linux host that has all three,
# SNX_QEMU_REF is ON with SNX_QEMU_AARCH64 and SNX_QEMU_REF_RUNNER (the runner's path); otherwise it is OFF and
# SNX_QEMU_REF_MISSING says what is missing, for the tests to skip with
include_guard(GLOBAL)
set(SNX_QEMU_REF OFF)
set(SNX_QEMU_REF_MISSING "")
set(snx_qemu_ref_flags --target=aarch64-linux-gnu -fuse-ld=lld -static -ffreestanding -nostdlib -fno-stack-protector
    -fno-pie -O2 -march=armv8-a -std=c23 -Wall -Wextra -pedantic -Werror -Wl,-e,_start
    "-Wl,--image-base=0x40000000000")
if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    set(SNX_QEMU_REF_MISSING "the QEMU reference runs on a Linux host's own build only")
elseif(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(SNX_QEMU_REF_MISSING "the runner needs Clang")
else()
    find_program(SNX_QEMU_AARCH64 qemu-aarch64)
    find_program(SNX_LD_LLD NAMES ld.lld)
    if(NOT SNX_QEMU_AARCH64)
        set(SNX_QEMU_REF_MISSING "no qemu-aarch64 on PATH (qemu-user)")
    elseif(NOT SNX_LD_LLD)
        set(SNX_QEMU_REF_MISSING "no ld.lld on PATH (lld)")
    else()
        # Clang has to target AArch64 and link with LLD: a program with the runner's flags
        set(probe "${PROJECT_BINARY_DIR}/qemu_ref_probe")
        file(MAKE_DIRECTORY "${probe}")
        file(WRITE "${probe}/probe.c" "void _start(void);\nvoid _start(void) { for (;;) { } }\n")
        execute_process(COMMAND "${CMAKE_C_COMPILER}" ${snx_qemu_ref_flags} "${probe}/probe.c" -o "${probe}/probe"
                        RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
        if(status EQUAL 0)
            set(SNX_QEMU_REF ON)
        else()
            set(SNX_QEMU_REF_MISSING "${CMAKE_C_COMPILER} cannot build a static aarch64-linux-gnu program with LLD")
        endif()
    endif()
endif()
if(SNX_QEMU_REF)
    message(STATUS "QEMU reference: ${SNX_QEMU_AARCH64}")
else()
    message(STATUS "QEMU reference tests skipped: ${SNX_QEMU_REF_MISSING}")
endif()

# snx_qemu_ref_runner(): builds the runner into the build folder, as the target snx_qemu_ref_runner
function(snx_qemu_ref_runner)
    if(NOT SNX_QEMU_REF OR TARGET snx_qemu_ref_runner)
        return()
    endif()
    set(dir "${PROJECT_SOURCE_DIR}/tests/qemu_ref")
    set(runner "${PROJECT_BINARY_DIR}/qemu_ref_runner")
    add_custom_command(OUTPUT "${runner}"
        COMMAND "${CMAKE_C_COMPILER}" ${snx_qemu_ref_flags} "${dir}/runner_start.S" "${dir}/runner.c" -o "${runner}"
        DEPENDS "${dir}/runner_start.S" "${dir}/runner.c" "${dir}/qemu_ref_proto.h"
        COMMENT "Building the QEMU reference's AArch64 runner" VERBATIM)
    add_custom_target(snx_qemu_ref_runner ALL DEPENDS "${runner}")
    set(SNX_QEMU_REF_RUNNER "${runner}" PARENT_SCOPE)
endfunction()
