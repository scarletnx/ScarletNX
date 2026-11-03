# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# glslc (src/glslc): each pass on its own (the C tests read the passes' arrays), the API a backend calls, a short
# deterministic fuzz, then whole shaders beside the host's tools: SPIRV-Tools' spirv-val on every module, and, where
# the build has glslangValidator, the same shaders, errors and words taken and refused, and the same block layouts
add_executable(snx-glslc-cli glslc_cli.c)
snx_test_target(snx-glslc-cli 99)
target_link_libraries(snx-glslc-cli PRIVATE snx_glslc)
foreach(pass lex parse emit)
    add_executable(snx-test-glslc-${pass} ${pass}_test.c)
    snx_test_target(snx-test-glslc-${pass} 99)
    target_link_libraries(snx-test-glslc-${pass} PRIVATE snx_glslc)
    add_test(NAME glslc_${pass} COMMAND snx-test-glslc-${pass})
endforeach()
add_executable(snx-test-glslc-fuzz glslc_fuzz.c)
snx_test_target(snx-test-glslc-fuzz 99)
target_link_libraries(snx-test-glslc-fuzz PRIVATE snx_glslc)
file(GLOB snx_glslc_shaders CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert"
     "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp")
add_test(NAME glslc_fuzz COMMAND snx-test-glslc-fuzz --iterations 3000 ${snx_glslc_shaders})
if(Python3_Interpreter_FOUND) # tests/CMakeLists.txt looked it up
    # the build's glslangValidator when cmake/glsl.cmake found one, else the host's, if any
    set(snx_glslc_glslang "${SNX_GLSLANG}")
    if(NOT snx_glslc_glslang)
        find_program(SNX_GLSLC_GLSLANG NAMES glslangValidator glslang)
        set(snx_glslc_glslang "${SNX_GLSLC_GLSLANG}")
    endif()
    set(snx_glslc_compare "")
    if(snx_glslc_glslang)
        set(snx_glslc_compare --glslang "${snx_glslc_glslang}")
    else()
        message(STATUS "glslc's tests: no glslangValidator, so they do not compare glslc with it")
    endif()
    set(snx_glslc_test "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/glslc_test.py")
    add_test(NAME glslc_errors COMMAND ${snx_glslc_test} errors --cli $<TARGET_FILE:snx-glslc-cli> ${snx_glslc_compare})
    if(snx_glslc_glslang)
        add_test(NAME glslc_keywords COMMAND ${snx_glslc_test} keywords --cli $<TARGET_FILE:snx-glslc-cli>
                                             ${snx_glslc_compare})
    endif()
    find_program(SNX_SPIRV_VAL spirv-val)
    find_program(SNX_SPIRV_DIS spirv-dis)
    if(SNX_SPIRV_VAL AND SNX_SPIRV_DIS)
        add_test(NAME glslc_shaders COMMAND ${snx_glslc_test} shaders --cli $<TARGET_FILE:snx-glslc-cli>
                                            --spirv-val "${SNX_SPIRV_VAL}" --spirv-dis "${SNX_SPIRV_DIS}" ${snx_glslc_compare})
    else()
        message(STATUS "Skipping glslc_shaders: spirv-val and spirv-dis are required")
    endif()
    find_program(SNX_SPIRV_OPT spirv-opt)
    if(snx_glslc_glslang AND SNX_SPIRV_OPT AND SNX_SPIRV_DIS)
        add_test(NAME glslc_semantics COMMAND ${snx_glslc_test} semantics --cli $<TARGET_FILE:snx-glslc-cli>
                                              --spirv-opt "${SNX_SPIRV_OPT}" --spirv-dis "${SNX_SPIRV_DIS}" ${snx_glslc_compare})
    else()
        message(STATUS "Skipping glslc_semantics: glslangValidator, spirv-opt and spirv-dis are required")
    endif()
endif()
