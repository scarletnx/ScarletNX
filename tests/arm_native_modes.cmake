# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The native back ends' AArch32 blocks of the other profiles and memory modes (cpu/arm/codegen.h), against LLVM's of the
# same options as test-references' arm/ keeps them (arm_x64.c's --frozen; arm_frozen.py records them), arm_x64.c's
# --profile, --memory and --trace there. The Cortex-A57 on flat memory is arm_<machine> itself

# snx_arm_frozen_program(<target>): a program of arm_x64.c with the frozen files' reader: each block's results held to
# LLVM's as the file keeps them
function(snx_arm_frozen_program target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support/frozen.c")
endfunction()

# snx_arm_native_modes(<machine> [<argument>...]): arm_<machine>_<mode> for each mode of snx-test-arm-<machine>, the
# blocks of its file (the arguments: the first --blocks N A32 ones and --thumb N Thumb ones, where it is not all), and
# each again with three host registers (four of x86-64's on Windows, whose first are a shift count's and a pair's), so
# that values spill
function(snx_arm_native_modes machine)
    set(pool 3)
    if(machine STREQUAL "win64" OR WIN32)
        set(pool 4)
    endif()
    set(frozen "${test-references_SOURCE_DIR}/arm")
    foreach(mode a9 arm11 arm946e arm7tdmi arm7tdmi_trace a57_hosted a9_hosted arm11_hosted a57_checked a9_checked
                 arm11_checked)
        add_test(NAME arm_${machine}_${mode} COMMAND snx-test-arm-${machine} --frozen ${frozen}/${mode}.txt ${ARGN})
        add_test(NAME arm_${machine}_${mode}_spill
                 COMMAND snx-test-arm-${machine} --frozen ${frozen}/${mode}.txt ${ARGN} --pool ${pool})
        set_tests_properties(arm_${machine}_${mode} arm_${machine}_${mode}_spill
                             PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    endforeach()
    # a Thumb store with writeback into the register it stores, which the Cortex-A9 takes and the A57 does not
    # (profile.c); the ARM946E's LDM with writeback of its base alone, of the base first and of it last; a list at a
    # misaligned address of the save memory's bus, which keeps it as it is, where no hook takes it; a block longer than
    # the trace's skipped bits have in a word (MULEQ and MULNE, one of each pair not run); an empty Thumb PUSH and POP,
    # which checked memory has no transaction of (UNDEFINED there): what random words hardly are
    foreach(case a9_own_base_f841_1d04 a9_own_base_f801_1d04 a9_own_base_f821_1d04 a9_own_base_f841_1b04
                 arm946e_ldm_base arm7tdmi_list_save arm7tdmi_trace_long a57_checked_empty_b400 a57_checked_empty_bc00)
        add_test(NAME arm_${machine}_${case} COMMAND snx-test-arm-${machine} --frozen ${frozen}/${case}.txt)
        set_tests_properties(arm_${machine}_${case} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    endforeach()
endfunction()
