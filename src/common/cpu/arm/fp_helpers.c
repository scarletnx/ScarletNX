/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
#include "common/cpu/arm/simd.h"
/* Share the audited integer implementation of ARM's FP pseudocode, not the
   A64 register ABI. Namespace this copy so both runtime libraries can coexist.
   The included implementation retains its SwitchRecomp/Apache notices. */
#define snx_fp_binary   snx_arm_soft_binary
#define snx_fp_fma      snx_arm_soft_fma
#define snx_fp_unary    snx_arm_soft_unary
#define snx_fp_convert  snx_arm_soft_convert
#define snx_fp_to_int   snx_arm_soft_to_int
#define snx_fp_from_int snx_arm_soft_from_int
#define snx_fp_compare  snx_arm_soft_compare
#define snx_fp_minmax   snx_arm_soft_minmax
#define snx_fp_round    snx_arm_soft_round
#define urecpe32        snx_arm_soft_urecpe32
#define ursqrte32       snx_arm_soft_ursqrte32
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/helpers/fp.c"

/* AArch32 FPRecipStep/FPRSqrtStep round the product BEFORE subtraction.
   A64's fused step helpers are not interchangeable with these operations. */
static u64 step(CPU* c, u64 x, u64 y, int half) {
    FpValue a = fp_unpack(c, x, 32), b = fp_unpack(c, y, 32);
    if (a.kind >= FPV_QNAN || b.kind >= FPV_QNAN) return fp_nan(c, fp_nan2(a, b), 32);
    int special = (a.kind == FPV_INF && b.kind == FPV_ZERO) || (b.kind == FPV_INF && a.kind == FPV_ZERO);
    u64 product = special ? 0 : snx_fp_binary(c, x, y, 32, SNX_FP_MUL);
    if (!half) return snx_fp_binary(c, 0x40000000, product, 32, SNX_FP_SUB);
    /* FPHalvedSub(3, product): a single rounding of the exact difference / 2. */
    return fp_fused(c, 0x40400000, product ^ 0x80000000u, 0x3f800000, 32, -1);
}
uint64_t snx_arm_fp(SnxArmCPU* cpu, uint64_t x, uint64_t y, uint64_t add, uint32_t op, uint32_t width, uint32_t param) {
    CPU c; /* only fpcr/fpsr are consumed by the raw-bit primitives */
    c.fpcr = param & (1u << 30) ? (cpu->fpscr & (1u << 26)) | 0x03000000 : cpu->fpscr;
    c.fpsr = 0;
    unsigned rm = (param >> 24) & 15, iw = (param >> 8) & 255, fb = (param >> 16) & 255;
    int uns = !!(param & (1u << 28));
    uint64_t sign = UINT64_C(1) << (width - 1), result = 0, product;
    if (rm == 8) rm = (c.fpcr >> 22) & 3;
    switch (op) {
        case AV_FADD: result = snx_fp_binary(&c, x, y, width, SNX_FP_ADD); break;
        case AV_FSUB: result = snx_fp_binary(&c, x, y, width, SNX_FP_SUB); break;
        case AV_FMUL: result = snx_fp_binary(&c, x, y, width, SNX_FP_MUL); break;
        case AV_FDIV: result = snx_fp_binary(&c, x, y, width, SNX_FP_DIV); break;
        case AV_FABD: result = snx_fp_binary(&c, x, y, width, SNX_FP_ABD); break;
        case AV_FMLA:
        case AV_FMLS:
        case AV_FNMLA:
        case AV_FNMLS:
        case AV_FNMUL:
            product = snx_fp_binary(&c, x, y, width, SNX_FP_MUL);
            if (op == AV_FMLS || op == AV_FNMLA || op == AV_FNMUL) product ^= sign;
            if (op == AV_FNMLA || op == AV_FNMLS) add ^= sign;
            result = op == AV_FNMUL ? product : snx_fp_binary(&c, add, product, width, SNX_FP_ADD);
            break;
        case AV_FMA:
        case AV_FMS:
        case AV_FNMA:
        case AV_FNMS:
            if (op == AV_FMS || op == AV_FNMA) x ^= sign;
            if (op == AV_FNMA || op == AV_FNMS) add ^= sign;
            result = snx_fp_fma(&c, add, x, y, width);
            break;
        case AV_FSQRT: result = snx_fp_unary(&c, x, width, SNX_FP_SQRT); break;
        case AV_FRECPE: result = snx_fp_unary(&c, x, width, SNX_FP_RECPE); break;
        case AV_FRSQRTE: result = snx_fp_unary(&c, x, width, SNX_FP_RSQRTE); break;
        case AV_URECPE: result = urecpe32((u32)x); break;
        case AV_URSQRTE: result = ursqrte32((u32)x); break;
        case AV_FRECPS:
        case AV_FRSQRTS: result = step(&c, x, y, op == AV_FRSQRTS); break;
        case AV_FCMP:
        case AV_FCMPE: result = snx_fp_compare(&c, x, y, width, op == AV_FCMPE); break;
        case AV_FCEQ:
        case AV_FCGE:
        case AV_FCGT:
        case AV_FACGE:
        case AV_FACGT: {
            if (op == AV_FACGE || op == AV_FACGT) {
                x &= sign - 1;
                y &= sign - 1;
            }
            unsigned f = snx_fp_compare(&c, x, y, width, op != AV_FCEQ);
            int ok = op == AV_FCEQ ? f == 6 : (op == AV_FCGE || op == AV_FACGE) ? f == 6 || f == 2 : f == 2;
            result = ok ? UINT32_MAX : 0;
            break;
        }
        case AV_FCVT: result = snx_fp_convert(&c, x, width, param & 255, rm); break;
        case AV_ITOF: result = snx_fp_from_int(&c, x, iw, width, uns, fb); break;
        case AV_FTOI: result = snx_fp_to_int(&c, x, width, iw, uns, fb, rm); break;
        case AV_FROUND: result = snx_fp_round(&c, x, width, rm, param & 1); break;
        case AV_FMIN:
        case AV_FMAX:
        case AV_FMINNM:
        case AV_FMAXNM:
            result =
                snx_fp_minmax(&c, x, y, width, op == AV_FMIN || op == AV_FMINNM, op == AV_FMINNM || op == AV_FMAXNM);
            break;
        default: break; /* emitter only calls this API for the above operations */
    }
    cpu->fpscr |= c.fpsr & 0x9f;
    return result;
}
