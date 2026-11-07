/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hand-written A32 and Thumb decoders that the decoders from QEMU's .decode files (src/common/cpu/arm/a32.c,
   thumb.c) replaced: tests/arm_decode.c checks the new ones against them, word by word */
#ifndef SNX_TESTS_ARM_DECODE_REF_H
#define SNX_TESTS_ARM_DECODE_REF_H

#include "common/cpu/arm/arm.h"

SnxA32Kind snx_a32_decode_ref(uint32_t word);
unsigned snx_thumb_decode_ref(uint16_t hw1, uint16_t hw2, SnxThumb* out);

#endif
