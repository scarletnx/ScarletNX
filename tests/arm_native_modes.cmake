# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The native back ends' AArch32 blocks of the other profiles and memory modes (cpu/arm/codegen.h), against LLVM's of the
# same options: arm_x64.c's --profile, --memory and --trace. The Cortex-A57 on flat memory is arm_<machine> itself

# snx_arm_native_modes(<machine> [<argument>...]): arm_<machine>_<mode> for each mode of snx-test-arm-<machine>, with
# the arguments (how many blocks and states), and each again with three host registers (four of x86-64's on Windows,
# whose first are a shift count's and a pair's), so that values spill
function(snx_arm_native_modes machine)
    set(pool 3)
    if(machine STREQUAL "win64" OR WIN32)
        set(pool 4)
    endif()
    foreach(mode a9 arm11 arm946e arm7tdmi arm7tdmi_trace a57_hosted a9_hosted arm11_hosted a57_checked a9_checked
                 arm11_checked)
        string(REGEX MATCH "^[^_]+" profile "${mode}")
        set(options --profile ${profile})
        if(mode MATCHES "_(hosted|checked)$")
            list(APPEND options --memory ${CMAKE_MATCH_1})
        elseif(mode MATCHES "_trace$")
            list(APPEND options --trace)
        endif()
        add_test(NAME arm_${machine}_${mode} COMMAND snx-test-arm-${machine} ${options} ${ARGN})
        add_test(NAME arm_${machine}_${mode}_spill COMMAND snx-test-arm-${machine} ${options} ${ARGN} --pool ${pool} --seed 2)
        set_tests_properties(arm_${machine}_${mode} arm_${machine}_${mode}_spill
                             PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    endforeach()
    # a Thumb store with writeback into the register it stores, which the Cortex-A9 takes and the A57 does not (profile.c)
    foreach(halves f841,1d04 f801,1d04 f821,1d04 f841,1b04)
        string(REPLACE "," "_" name "${halves}")
        add_test(NAME arm_${machine}_a9_own_base_${name}
                 COMMAND snx-test-arm-${machine} --profile a9 --states 64 --halves ${halves})
        set_tests_properties(arm_${machine}_a9_own_base_${name} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    # what random words hardly are: the ARM946E's LDM with writeback of its base alone, of the base first and of it last
    add_test(NAME arm_${machine}_arm946e_ldm_base
             COMMAND snx-test-arm-${machine} --profile arm946e --states 64 --word e8b10002 --word e9b10002
                     --word e8310002 --word e9310002 --word e8b10006 --word e8b10003)
    set_tests_properties(arm_${machine}_arm946e_ldm_base PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    # a block longer than the trace's skipped bits have in a word: MULEQ and MULNE, one of each pair not run
    # a list at a misaligned address of the save memory's bus, which keeps it as it is, where no hook takes it
    add_test(NAME arm_${machine}_arm7tdmi_list_save
             COMMAND snx-test-arm-${machine} --profile arm7tdmi --states 256 --word e3a0140e,e2811002,e891000c
                     --word e3a0140e,e2811002,e881000c)
    set_tests_properties(arm_${machine}_arm7tdmi_list_save PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    string(REPEAT "00000291,10000291," 23 long)
    add_test(NAME arm_${machine}_arm7tdmi_trace_long
             COMMAND snx-test-arm-${machine} --profile arm7tdmi --trace --states 64 --word ${long}00000291)
    set_tests_properties(arm_${machine}_arm7tdmi_trace_long PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    # an empty Thumb PUSH and POP, which checked memory has no transaction of (UNDEFINED there)
    foreach(halfword b400 bc00)
        add_test(NAME arm_${machine}_a57_checked_empty_${halfword}
                 COMMAND snx-test-arm-${machine} --memory checked --states 64 --halves ${halfword})
        set_tests_properties(arm_${machine}_a57_checked_empty_${halfword} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endfunction()
