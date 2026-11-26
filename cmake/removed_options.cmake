# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Options the build had and has no more: one given (-D, or an older tree's cache) is said with its value and what holds
# now, and taken out of the cache, so a tree says it once. tests/removed_options_test.cmake runs it
if(DEFINED SNX_A64_DISASM)
    message(STATUS "SNX_A64_DISASM=${SNX_A64_DISASM} is no longer an option: scarletnx --mcp lists A64 code through the "
                   "A64 decoder in every build")
    unset(SNX_A64_DISASM CACHE)
endif()
# the LLVM back end, and what found it: the native back ends make all of the code
foreach(option SNX_WITH_LLVM SNX_LLVM_CONFIG)
    if(DEFINED ${option})
        message(STATUS "${option}=${${option}} is no longer an option: the native back ends make all of the code, and "
                       "the build has no LLVM")
        unset(${option} CACHE)
    endif()
endforeach()
