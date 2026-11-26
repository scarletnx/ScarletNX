# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Options the build had and has no more: one given (-D, or an older tree's cache) is said with its value and what holds
# now, and taken out of the cache, so a tree says it once. tests/removed_options_test.cmake runs it
if(DEFINED SNX_A64_DISASM)
    message(STATUS "SNX_A64_DISASM=${SNX_A64_DISASM} is no longer an option: scarletnx --mcp lists A64 code through the "
                   "A64 decoder in every build")
    unset(SNX_A64_DISASM CACHE)
endif()
# the QEMU differential's sweep, which was LLVM's and is the native back end's: its option is named for what it runs
if(DEFINED SNX_TEST_LLVM_SWEEP)
    if(DEFINED SNX_TEST_QEMU_SWEEP)
        message(STATUS "SNX_TEST_LLVM_SWEEP=${SNX_TEST_LLVM_SWEEP} is now SNX_TEST_QEMU_SWEEP, which is given too: "
                       "SNX_TEST_QEMU_SWEEP=${SNX_TEST_QEMU_SWEEP} holds")
    else()
        message(STATUS "SNX_TEST_LLVM_SWEEP=${SNX_TEST_LLVM_SWEEP} is now SNX_TEST_QEMU_SWEEP: "
                       "SNX_TEST_QEMU_SWEEP=${SNX_TEST_LLVM_SWEEP} holds")
        set(SNX_TEST_QEMU_SWEEP "${SNX_TEST_LLVM_SWEEP}" CACHE BOOL "Run the whole-encoding QEMU differential sweep (slow)")
    endif()
    unset(SNX_TEST_LLVM_SWEEP CACHE)
endif()
# the LLVM back end, and what found it: the native back ends make all of the code
foreach(option SNX_WITH_LLVM SNX_LLVM_CONFIG)
    if(DEFINED ${option})
        message(STATUS "${option}=${${option}} is no longer an option: the native back ends make all of the code, and "
                       "the build has no LLVM")
        unset(${option} CACHE)
    endif()
endforeach()
