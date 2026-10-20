# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
if(CMAKE_CROSSCOMPILING OR NOT TARGET snx_ctr_cpu)
    message(FATAL_ERROR "SNX_TEST_ARM_AOT requires a native build with SNX_BUILD_3DS=ON")
endif()
if(NOT SNX_BUILD_NX AND NOT SNX_BUILD_GNUABI)
    include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" arm-aot-backend)
endif()
add_executable(snx-test-arm-checked-emit arm_checked_emit.c)
snx_test_target(snx-test-arm-checked-emit 11)
target_link_libraries(snx-test-arm-checked-emit PRIVATE snx_arm_llvm)
foreach(opt 0 2)
    set(object "${CMAKE_CURRENT_BINARY_DIR}/arm-checked-O${opt}${CMAKE_C_OUTPUT_EXTENSION}")
    add_custom_command(OUTPUT "${object}"
        COMMAND $<TARGET_FILE:snx-test-arm-checked-emit> "${object}" "${opt}"
        DEPENDS snx-test-arm-checked-emit arm_checked_cases.h VERBATIM)
    set_source_files_properties("${object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(snx-test-arm-checked-O${opt} arm_checked.c "${object}")
    snx_test_target(snx-test-arm-checked-O${opt} 11)
    target_compile_options(snx-test-arm-checked-O${opt} PRIVATE -UNDEBUG)
    target_link_libraries(snx-test-arm-checked-O${opt} PRIVATE snx_ctr_cpu snx_arm_rt)
    add_test(NAME arm_checked_static_O${opt} COMMAND snx-test-arm-checked-O${opt})
endforeach()
# Exercise the unchanged A57 entry points without configuring the NX runtime.
if(NOT SNX_BUILD_NX AND NOT SNX_BUILD_GNUABI)
    add_executable(snx-test-arm-emit arm_emit.c)
    snx_test_target(snx-test-arm-emit 11)
    target_link_libraries(snx-test-arm-emit PRIVATE snx_arm_llvm)
    add_executable(snx-test-thumb-emit thumb_emit.c)
    snx_test_target(snx-test-thumb-emit 11)
    target_link_libraries(snx-test-thumb-emit PRIVATE snx_arm_llvm)
    add_executable(snx-test-arm-decode arm_decode.c)
    snx_test_target(snx-test-arm-decode 11)
    target_link_libraries(snx-test-arm-decode PRIVATE snx_arm_llvm)
    add_test(NAME arm_decode COMMAND snx-test-arm-decode)
    if(Python3_Interpreter_FOUND AND UNIX)
        add_test(NAME arm_llvm_witnesses COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
            --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}")
        add_test(NAME arm7tdmi_witnesses COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm7tdmi_test.py"
            --arm-emitter $<TARGET_FILE:snx-test-arm-emit> --thumb-emitter $<TARGET_FILE:snx-test-thumb-emit>
            --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}")
        add_test(NAME arm_checked_matrix COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_checked_matrix.py"
            --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}")
        set_tests_properties(arm_checked_matrix PROPERTIES TIMEOUT 300)
        option(SNX_TEST_ARM_DIFF "Compare A32 LLVM execution with Python unicorn 2.1.4" OFF)
        if(SNX_TEST_ARM_DIFF)
            add_test(NAME arm_llvm_differential COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
                --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}" --unicorn)
            add_test(NAME arm11_llvm_differential COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm11_test.py"
                --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}")
            set_tests_properties(arm_llvm_differential arm11_llvm_differential PROPERTIES TIMEOUT 900)
            get_filename_component(snx_llvm_config_real "${SNX_LLVM_CONFIG}" REALPATH)
            get_filename_component(snx_llvm_bin "${snx_llvm_config_real}" DIRECTORY)
            find_program(SNX_LLVM_MC llvm-mc HINTS "${snx_llvm_bin}" NO_DEFAULT_PATH)
            if(SNX_LLVM_MC)
                add_test(NAME arm11_vfp_differential COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm11_vfp.py"
                    --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt>
                    --mc "${SNX_LLVM_MC}" --cc "${CMAKE_C_COMPILER}")
                set_tests_properties(arm11_vfp_differential PROPERTIES TIMEOUT 900)
                add_test(NAME arm_llvm_vector COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_vector_test.py"
                    --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt>
                    --mc "${SNX_LLVM_MC}" --cc "${CMAKE_C_COMPILER}")
                add_test(NAME thumb_llvm COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/thumb_test.py"
                    --emitter $<TARGET_FILE:snx-test-thumb-emit> --runtime $<TARGET_FILE:snx_arm_rt> --mc "${SNX_LLVM_MC}" --cc "${CMAKE_C_COMPILER}")
                set_tests_properties(arm_llvm_vector thumb_llvm PROPERTIES TIMEOUT 1800)
            endif()
        endif()
    endif()
endif()
