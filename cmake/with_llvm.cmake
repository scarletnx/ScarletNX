# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# SNX_WITH_LLVM: the LLVM backend, which makes the code the recompiler's own back ends leave (cmake/llvm.cmake finds
# LLVM). Every driver calls it, and the recompiler's library: a build without it is the recompiler's core
# (snx_recompiler_core) and its tests, and one that asks for a driver stops, naming it. Included after the drivers'
# options; tests/with_llvm_test.cmake runs it
option(SNX_WITH_LLVM "Build the LLVM backend, which makes the code the recompiler's own back ends leave" ON)
if(NOT SNX_WITH_LLVM)
    set(snx_calls_llvm "")
    foreach(driver NX GNUABI 3DS VITA AGB NDS ANDROID)
        if(SNX_BUILD_${driver})
            list(APPEND snx_calls_llvm "SNX_BUILD_${driver}=${SNX_BUILD_${driver}}")
        endif()
    endforeach()
    if(snx_calls_llvm)
        list(JOIN snx_calls_llvm ", " snx_calls_llvm)
        message(FATAL_ERROR "SNX_WITH_LLVM=${SNX_WITH_LLVM} builds the recompiler's core alone, and these need LLVM: "
                            "${snx_calls_llvm}. Turn them off, or SNX_WITH_LLVM on.")
    endif()
endif()
