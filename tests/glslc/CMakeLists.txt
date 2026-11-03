# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# glslc (src/glslc): its preprocessor and tokens on their own (the test reads the pass's arrays)
foreach(pass lex)
    add_executable(snx-test-glslc-${pass} ${pass}_test.c)
    snx_test_target(snx-test-glslc-${pass} 99)
    target_link_libraries(snx-test-glslc-${pass} PRIVATE snx_glslc)
    add_test(NAME glslc_${pass} COMMAND snx-test-glslc-${pass})
endforeach()
