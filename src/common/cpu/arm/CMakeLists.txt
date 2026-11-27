# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Several runtime configurations can request this shared backend.
if(TARGET snx_arm_rt)
    return()
endif()
# Kept separate from the A64 state ABI and emitters: a module can hold either's code, but their guest function
# signatures are not interchangeable.
add_library(snx_arm_rt STATIC fp_helpers.c crypto_helpers.c rt_helpers.c)
set_target_properties(snx_arm_rt PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(snx_arm_rt PRIVATE -Wall -Wextra -Werror=missing-prototypes)
# the decoders, the profile rules and the blocks' successors: a runtime that executes a word itself
# links these alone.
# a32.c and thumb.c include the decoders tools/decodetree makes from QEMU's .decode files (decode/), as QEMU's
# translate.c includes them: t32.decode and t16.decode name a32.decode's and a32-uncond.decode's argument sets
# !extern, and thumb.c takes those from --argsets without the A32 decoders
add_library(snx_arm_decode STATIC a32.c thumb.c vector_decode.c profile.c successors.c)
set_target_properties(snx_arm_decode PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_compile_options(snx_arm_decode PRIVATE -Wall -Wextra -Werror=missing-prototypes)
include("${PROJECT_SOURCE_DIR}/cmake/decodetree.cmake")
set(snx_arm_decode_dir "${CMAKE_CURRENT_SOURCE_DIR}/decode")
snx_decodetree(snx_arm_decode OUTPUT decode-a32.inc DECODE "${snx_arm_decode_dir}/a32.decode"
               OPTIONS --static-decode=disas_a32)
snx_decodetree(snx_arm_decode OUTPUT decode-a32-uncond.inc DECODE "${snx_arm_decode_dir}/a32-uncond.decode"
               OPTIONS --static-decode=disas_a32_uncond)
snx_decodetree(snx_arm_decode OUTPUT decode-a32-args.inc DECODE "${snx_arm_decode_dir}/a32.decode" OPTIONS --argsets)
snx_decodetree(snx_arm_decode OUTPUT decode-a32-uncond-args.inc DECODE "${snx_arm_decode_dir}/a32-uncond.decode"
               OPTIONS --argsets)
snx_decodetree(snx_arm_decode OUTPUT decode-t32.inc DECODE "${snx_arm_decode_dir}/t32.decode"
               OPTIONS --static-decode=disas_t32)
snx_decodetree(snx_arm_decode OUTPUT decode-t16.inc DECODE "${snx_arm_decode_dir}/t16.decode"
               OPTIONS -w 16 --static-decode=disas_t16)
