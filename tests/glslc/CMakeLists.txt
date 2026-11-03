# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# glslc (src/glslc): each pass on its own (the C tests read the passes' arrays)
foreach(pass lex parse)
    add_executable(snx-test-glslc-${pass} ${pass}_test.c)
    snx_test_target(snx-test-glslc-${pass} 99)
    target_link_libraries(snx-test-glslc-${pass} PRIVATE snx_glslc)
    add_test(NAME glslc_${pass} COMMAND snx-test-glslc-${pass})
endforeach()
