/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_BLEND_IMAGE_PROGRAM_H
#define SNX_TEST_BLEND_IMAGE_PROGRAM_H
#include "common/cpu/aarch64/cpu.h"

/* Every covered fragment copies its own R32UI texel, then outputs a fixed premultiplied color. Two-dimensional
   guest image storage occupies IMG, while the programmable blend attachment must use IMG+nimg. */
static const u64 g_blend_image_instructions[] = {0xe000000000070000ull | 0x1cull << 30, /* IPA R0, fragment x */
                                                 0xe000000000070001ull | 0x1dull << 30, /* IPA R1, fragment y */
                                                 0x5cb0018000070a00ull,                 /* F2I.U32.F32.TRUNC R0, R0 */
                                                 0x5cb0018000170a01ull,                 /* F2I.U32.F32.TRUNC R1, R1 */
                                                 0x4c98078804a70004ull,                 /* MOV R4, c[2][0x128] */
                                                 0x4c98078804b70006ull,                 /* MOV R6, c[2][0x12c] */
                                                 0xeb10020600470008ull,                 /* SULD.D.32 R8, [R0,R1], R4 */
                                                 0xeb20030600170008ull,                 /* SUST.P.R [R0,R1], R8, R6 */
                                                 0x0100000000071000ull | 0x3e000000ull << 20,
                                                 0x0100000000071001ull | 0x3d800000ull << 20,
                                                 0x0100000000071002ull | 0x3e400000ull << 20,
                                                 0x0100000000071003ull | 0x3e800000ull << 20,
                                                 0xe30000000007000full};
#endif
