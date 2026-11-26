# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The AArch32 blocks that the native back ends make, run from the Python tests' states (arm_blocks.c and arm_blocks.py):
# this host's back end as arm_native_<test>, and the other machines' as arm_<machine>_<test>, under Unicorn where its C
# library is there. The witnesses need no Unicorn, the comparisons with its ARM core take Python's, SNX_TEST_ARM_DIFF
option(SNX_TEST_ARM_DIFF "Compare the native back ends' A32 and Thumb blocks with Python unicorn 2.1.4" OFF)
if(CMAKE_CROSSCOMPILING OR NOT UNIX)
    return()
endif()

# snx_arm_blocks(<machine> [<source>...]): snx-test-arm-blocks-<machine>, of arm_blocks.c and the sources
function(snx_arm_blocks machine)
    add_executable(snx-test-arm-blocks-${machine} arm_blocks.c ${ARGN})
    snx_test_target(snx-test-arm-blocks-${machine} gnu23)
    target_include_directories(snx-test-arm-blocks-${machine} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(snx-test-arm-blocks-${machine} PRIVATE snx_recompiler_core snx_arm_decode snx_arm_rt
                                                                 ${CMAKE_DL_LIBS})
endfunction()

# snx_arm_blocks_test(<machine> <test> <script> [<argument>...]): the script, on the machine's blocks
function(snx_arm_blocks_test machine test script)
    # -X faulthandler names where a crash in Unicorn was
    add_test(NAME arm_${machine}_${test} COMMAND "${Python3_EXECUTABLE}" -X faulthandler
             "${CMAKE_CURRENT_SOURCE_DIR}/${script}" --blocks $<TARGET_FILE:snx-test-arm-blocks-${machine}> ${ARGN})
    set_tests_properties(arm_${machine}_${test} PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77)
endfunction()

snx_arm_blocks(native)
if(Python3_Interpreter_FOUND)
    snx_arm_blocks_test(native witnesses arm_test.py)
    snx_arm_blocks_test(native 7tdmi_witnesses arm7tdmi_test.py)
    if(SNX_TEST_ARM_DIFF)
        snx_arm_blocks_test(native differential arm_test.py --unicorn)
        snx_arm_blocks_test(native vector arm_vector_test.py)
        snx_arm_blocks_test(native thumb thumb_test.py)
        # random encodings of every class, from random FP and flag state
        snx_arm_blocks_test(native fuzz arm_fuzz.py --count 1500)
    endif()
    if(SNX_TEST_ARM_AOT) # the checked memory and the ARM11
        snx_arm_blocks_test(native checked_matrix arm_checked_matrix.py)
        if(SNX_TEST_ARM_DIFF)
            snx_arm_blocks_test(native arm11_differential arm11_test.py)
            snx_arm_blocks_test(native arm11_vfp arm11_vfp.py)
        endif()
    endif()
endif()

# llvm-mc's verdict on the tests' words (arm_mc_words.py): the toolchain's, which it skips where there is none
if(Python3_Interpreter_FOUND)
    get_filename_component(snx_cc_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    find_program(SNX_LLVM_MC NAMES llvm-mc HINTS "${snx_cc_bin}")
    add_test(NAME arm_mc_words COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_mc_words.py"
             --mc "${SNX_LLVM_MC}")
    set_tests_properties(arm_mc_words PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()

if(SNX_UNICORN_INCLUDE_DIR AND SNX_UNICORN_LIBRARY)
    foreach(m arm64 arm64win rv64 ppc64 mips64 la64 ppc64be mips64be s390x)
        string(TOUPPER "${m}" upper)
        snx_arm_blocks(${m} support/host_unicorn.c support/la64_interp.c)
        target_compile_definitions(snx-test-arm-blocks-${m} PRIVATE SNX_TEST_${upper}=1)
        if(m STREQUAL "arm64win")
            target_compile_definitions(snx-test-arm-blocks-${m} PRIVATE SNX_TEST_ARM64=1)
        endif()
        target_include_directories(snx-test-arm-blocks-${m} PRIVATE "${SNX_UNICORN_INCLUDE_DIR}")
        target_link_libraries(snx-test-arm-blocks-${m} PRIVATE "${SNX_UNICORN_LIBRARY}")
        if(Python3_Interpreter_FOUND)
            snx_arm_blocks_test(${m} witnesses arm_test.py)
            snx_arm_blocks_test(${m} 7tdmi_witnesses arm7tdmi_test.py)
            if(SNX_TEST_ARM_DIFF)
                # the same states, each word's from its own place on
                snx_arm_blocks_test(${m} differential arm_test.py --unicorn --every 4)
                snx_arm_blocks_test(${m} vector arm_vector_test.py --every 5)
                snx_arm_blocks_test(${m} thumb thumb_test.py)
                snx_arm_blocks_test(${m} fuzz arm_fuzz.py --count 500 --states 3)
            endif()
            if(SNX_TEST_ARM_AOT)
                snx_arm_blocks_test(${m} checked_matrix arm_checked_matrix.py --every 4)
                if(SNX_TEST_ARM_DIFF)
                    snx_arm_blocks_test(${m} arm11_differential arm11_test.py --every 4)
                    snx_arm_blocks_test(${m} arm11_vfp arm11_vfp.py --every 5)
                endif()
            endif()
        endif()
    endforeach()
endif()
