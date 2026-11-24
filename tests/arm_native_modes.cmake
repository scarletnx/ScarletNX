# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The native back ends' AArch32 blocks of the other profiles and memory modes (cpu/arm/codegen.h), against LLVM's of the
# same options: arm_x64.c's --profile, --memory and --trace. The Cortex-A57 on flat memory is arm_<machine> itself

# snx_arm_native_modes(<machine> [<argument>...]): arm_<machine>_<mode> for each mode of snx-test-arm-<machine>, with
# the arguments (how many blocks and states), and each again with three host registers, so that values spill
function(snx_arm_native_modes machine)
    foreach(mode a9 arm11 a57_hosted a9_hosted arm11_hosted a57_checked a9_checked arm11_checked)
        string(REGEX MATCH "^[^_]+" profile "${mode}")
        set(options --profile ${profile})
        if(mode MATCHES "_(hosted|checked)$")
            list(APPEND options --memory ${CMAKE_MATCH_1})
        elseif(mode MATCHES "_trace$")
            list(APPEND options --trace)
        endif()
        add_test(NAME arm_${machine}_${mode} COMMAND snx-test-arm-${machine} ${options} ${ARGN})
        add_test(NAME arm_${machine}_${mode}_spill COMMAND snx-test-arm-${machine} ${options} ${ARGN} --pool 3 --seed 2)
        set_tests_properties(arm_${machine}_${mode} arm_${machine}_${mode}_spill
                             PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    endforeach()
    # a store with writeback into the register it stores, which the Cortex-A9 takes and the A57 does not (profile.c);
    # Thumb is x86-64's
    if(machine STREQUAL "x64")
        foreach(halves f841,1d04 f801,1d04 f821,1d04 f841,1b04)
            string(REPLACE "," "_" name "${halves}")
            add_test(NAME arm_x64_a9_own_base_${name}
                     COMMAND snx-test-arm-x64 --profile a9 --states 64 --halves ${halves})
        endforeach()
    endif()
    # an empty Thumb PUSH and POP, which checked memory has no transaction of (UNDEFINED there); Thumb is x86-64's
    if(machine STREQUAL "x64")
        foreach(halfword b400 bc00)
            add_test(NAME arm_x64_a57_checked_empty_${halfword}
                     COMMAND snx-test-arm-x64 --memory checked --states 64 --halves ${halfword})
        endforeach()
    endif()
endfunction()
