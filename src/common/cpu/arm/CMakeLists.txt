# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Several runtime configurations can request this shared backend.
if(TARGET snx_arm_llvm)
    return()
endif()
# Kept separate from the A64 state ABI and emitters. Host-target LLVM modules can
# contain either backend, but their guest function signatures are not interchangeable.
add_library(snx_arm_rt STATIC fp_helpers.c crypto_helpers.c rt_helpers.c)
set_target_properties(snx_arm_rt PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(snx_arm_rt PRIVATE -Wall -Wextra -Werror=missing-prototypes)
# linked into the libraries made from emitted ARM code (tests/arm_test.py loads one into Python), which have no
# sanitizer runtime: SNX_TEST_SANITIZERS (tests/CMakeLists.txt) builds it without
if(SNX_TEST_SANITIZERS)
    target_compile_options(snx_arm_rt PRIVATE -fno-sanitize=address,undefined)
endif()
# the decoders and the profile rules, which need no LLVM: a runtime that executes a word itself links these alone
add_library(snx_arm_decode STATIC a32.c thumb_decode.c vector_decode.c profile.c)
set_target_properties(snx_arm_decode PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_arm_decode PRIVATE -Wall -Wextra -Werror=missing-prototypes)
add_library(snx_arm_llvm STATIC llvm.c successors.c)
set_target_properties(snx_arm_llvm PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(snx_arm_llvm SYSTEM PUBLIC "${SNX_LLVM_INCLUDE_DIR}")
target_compile_options(snx_arm_llvm PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_directories(snx_arm_llvm PUBLIC "${SNX_LLVM_LIBRARY_DIR}")
target_link_libraries(snx_arm_llvm PUBLIC ${SNX_LLVM_LIBS} ${SNX_LLVM_SYSTEM_LIBS} Threads::Threads snx_arm_rt
                      snx_arm_decode)
