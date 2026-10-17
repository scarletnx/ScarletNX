/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
/* Included by llvm.c. Lane-by-lane ARM semantics, deliberately without host
   vector/FP fast paths. Integer bit patterns keep LLVM from changing FP rules. */
static LLVMValueRef vk(ArmIR* a, unsigned width, uint64_t x) {
    return LLVMConstInt(LLVMIntTypeInContext(a->ctx, width), x, 0);
}
static LLVMValueRef vcast(ArmIR* a, LLVMValueRef x, unsigned width, int sign) {
    unsigned old = LLVMGetIntTypeWidth(LLVMTypeOf(x));
    LLVMTypeRef t = LLVMIntTypeInContext(a->ctx, width);
    if (old == width) return x;
    if (old > width) return LLVMBuildTrunc(a->b, x, t, "");
    return sign ? LLVMBuildSExt(a->b, x, t, "") : LLVMBuildZExt(a->b, x, t, "");
}
static LLVMValueRef vget(ArmIR* a, unsigned offset, unsigned width) {
    LLVMValueRef x = get(a, offsetof(SnxArmCPU, d) + (offset & ~7u), a->i64);
    if (offset & 7) x = LLVMBuildLShr(a->b, x, c64(a, (offset & 7) * 8), "");
    return vcast(a, x, width, 0);
}
static void vset(ArmIR* a, unsigned offset, unsigned width, LLVMValueRef x) {
    LLVMValueRef result = vcast(a, x, 64, 0);
    if (width < 64) {
        unsigned shift = (offset & 7) * 8;
        uint64_t mask = ((UINT64_C(1) << width) - 1) << shift;
        LLVMValueRef old = get(a, offsetof(SnxArmCPU, d) + (offset & ~7u), a->i64);
        result = bor(a, band(a, old, c64(a, ~mask)),
                     LLVMBuildShl(a->b, band(a, result, c64(a, mask >> shift)), c64(a, shift), ""));
    }
    put(a, offsetof(SnxArmCPU, d) + (offset & ~7u), result);
}
static LLVMValueRef fp_call(ArmIR* a, SnxArmVectorOp op, unsigned width, unsigned param, LLVMValueRef x, LLVMValueRef y,
                            LLVMValueRef z) {
    LLVMValueRef fn = LLVMGetNamedFunction(a->mod, "snx_arm_fp");
    LLVMTypeRef params[] = {a->ptr, a->i64, a->i64, a->i64, a->i32, a->i32, a->i32};
    LLVMTypeRef type = LLVMFunctionType(a->i64, params, 7, 0);
    LLVMValueRef args[] = {a->cpu,     vcast(a, x, 64, 0), vcast(a, y, 64, 0), vcast(a, z, 64, 0),
                           c32(a, op), c32(a, width),      c32(a, param)};
    if (!fn) fn = LLVMAddFunction(a->mod, "snx_arm_fp", type);
    return LLVMBuildCall2(a->b, type, fn, args, 7, "");
}
static void vector_memory(ArmIR* a, const SnxArmVector* v) {
    LLVMValueRef base = reg(a, v->n), addr, end, offset = c32(a, (uint32_t)v->imm);
    int single = (v->flags & AV_PRE) && !(v->flags & AV_WB);
    end = v->flags & AV_UP ? add(a, base, offset) : sub(a, base, offset);
    addr = single || (v->flags & AV_PRE) ? end : base;
    if (a->memory == SNX_ARM_MEMORY_CHECKED)
        transfer_begin(a, addr, v->bytes, 4, !(v->flags & AV_LOAD), 1);
    else
        bounds(a, addr, v->bytes, 4);
    for (unsigned i = 0; i < v->bytes; i += 4) {
        LLVMValueRef p = add(a, addr, c32(a, i));
        if (v->flags & AV_LOAD)
            vset(a, v->d + i, 32, readmem(a, p, 4, 0));
        else
            writemem(a, p, 4, vget(a, v->d + i, 32));
    }
    if (a->memory == SNX_ARM_MEMORY_CHECKED && !(v->flags & AV_LOAD)) transfer_commit(a);
    if (v->flags & AV_WB) setreg(a, v->n, end);
}
static void vector_transfer(ArmIR* a, const SnxArmVector* v) {
    if (v->op == AV_STATUS) {
        if (v->flags & AV_LOAD) {
            LLVMValueRef f = GET(a, fpscr);
            if (v->n == 15)
                PUT(a, cpsr, bor(a, band(a, GET(a, cpsr), c32(a, 0x0fffffff)), band(a, f, c32(a, 0xf0000000))));
            else
                setreg(a, v->n, f);
        } else {
            if (a->profile == SNX_ARM_PROFILE_ARM11)
                require(a, cmp(a, LLVMIntEQ, band(a, reg(a, v->n), c32(a, ~0xf3c0009fu)), c32(a, 0)),
                        SNX_ARM_BACKEND_UNSUPPORTED, c32(a, a->word));
            PUT(a, fpscr, band(a, reg(a, v->n), c32(a, 0xffc0009f)));
        }
    } else if (v->op == AV_CORE_PAIR) {
        LLVMValueRef x, y;
        if (v->flags & AV_LOAD) {
            x = vget(a, v->d, 32);
            y = vget(a, v->d + 4, 32);
            setreg(a, v->n, x);
            setreg(a, v->m, y);
        } else {
            x = reg(a, v->n);
            y = reg(a, v->m);
            vset(a, v->d, 32, x);
            vset(a, v->d + 4, 32, y);
        }
    } else if (v->flags & AV_LOAD) {
        setreg(a, v->n, vcast(a, vget(a, v->d, v->width), 32, !(v->flags & AV_UNSIGNED)));
    } else if (v->op == AV_DUP) {
        LLVMValueRef x = vcast(a, reg(a, v->n), v->width, 0);
        for (unsigned i = 0; i < v->bytes; i += v->width / 8) vset(a, v->d + i, v->width, x);
    } else
        vset(a, v->d, v->width, vcast(a, reg(a, v->n), v->width, 0));
}
static void vector_fp(ArmIR* a, const SnxArmVector* v) {
    LLVMValueRef x[4], y[4], z[4], r[4];
    unsigned width = v->width, outwidth = width, lanes, param = 0;
    int unary = v->op == AV_MOV || (v->op >= AV_FABS && v->op <= AV_FSQRT) || v->op == AV_FCVT || v->op == AV_FTOI ||
                v->op == AV_ITOF || v->op == AV_FROUND || v->op == AV_FRECPE || v->op == AV_FRSQRTE ||
                v->op == AV_URECPE || v->op == AV_URSQRTE;
    if (v->op == AV_FCVT)
        outwidth = v->aux;
    else if (v->op == AV_FTOI)
        outwidth = v->flags & AV_SIMD ? v->aux : v->bytes * 8;
    lanes = v->flags & AV_SIMD ? v->bytes * 8 / outwidth : 1;
    if (v->flags & AV_SIMD) param |= 1u << 30;
    if (v->flags & AV_UNSIGNED) param |= 1u << 28;
    if (v->op == AV_FCVT) param |= v->aux | ((uint32_t)v->imm << 24);
    if (v->op == AV_FTOI || v->op == AV_ITOF)
        param |= v->aux << 8 | (((uint32_t)v->imm >> 8) & 255) << 16 | ((uint32_t)v->imm & 15) << 24;
    if (v->op == AV_FROUND) param |= (uint32_t)v->imm << 24 | v->aux;
    for (unsigned i = 0; i < lanes; i++) {
        unsigned inwidth = v->op == AV_ITOF ? v->aux : width;
        x[i] = vget(a, (unary ? v->m : v->n) + i * inwidth / 8, inwidth);
        y[i] = (v->op == AV_FCMP || v->op == AV_FCMPE) && v->aux
                   ? vk(a, width, 0)
                   : vget(a, v->m + (v->flags & AV_SCALAR ? 0 : i * width / 8), width);
        if (v->flags & AV_PAIR) {
            unsigned base = i < lanes / 2 ? v->n : v->m, j = i % (lanes / 2);
            x[i] = vget(a, base + j * 2 * width / 8, width);
            y[i] = vget(a, base + (j * 2 + 1) * width / 8, width);
        }
        if (v->flags & AV_ZERO) {
            y[i] = vk(a, width, 0);
            if (v->flags & AV_REVERSE) {
                y[i] = x[i];
                x[i] = vk(a, width, 0);
            }
        }
        z[i] = vget(a, v->d + i * outwidth / 8, outwidth);
    }
    for (unsigned i = 0; i < lanes; i++) {
        switch (v->op) {
            case AV_MOV: r[i] = x[i]; break;
            case AV_FABS: r[i] = band(a, x[i], vk(a, width, (UINT64_C(1) << (width - 1)) - 1)); break;
            case AV_FNEG: r[i] = bxor(a, x[i], vk(a, width, UINT64_C(1) << (width - 1))); break;
            case AV_FSELECT: r[i] = choose(a, condition(a, (unsigned[]){0, 6, 10, 12}[v->aux]), x[i], y[i]); break;
            default: r[i] = fp_call(a, v->op, width, param, x[i], y[i], z[i]); break;
        }
    }
    if (v->op == AV_FCMP || v->op == AV_FCMPE) {
        PUT(a, fpscr, bor(a, band(a, GET(a, fpscr), c32(a, 0x0fffffff)), shl(a, vcast(a, r[0], 32, 0), 28)));
    } else
        for (unsigned i = 0; i < lanes; i++) {
            if (v->op == AV_FTOI) r[i] = vcast(a, vcast(a, r[i], v->aux, 0), outwidth, !(v->flags & AV_UNSIGNED));
            vset(a, v->d + i * outwidth / 8, outwidth, vcast(a, r[i], outwidth, 0));
        }
}
static void vector_integer(ArmIR* a, const SnxArmVector* v);
static void vector_struct(ArmIR* a, const SnxArmVector* v);
static void vector_instruction(ArmIR* a) {
    SnxArmVector v;
    if (!snx_arm_vector_decode(a->word, &v)) {
        trap(a, SNX_ARM_UNDEFINED, c32(a, a->word));
        return;
    }
    if (a->profile == SNX_ARM_PROFILE_ARM11) {
        /* Profile decoding has excluded NEON, D16..31 and post-VFPv2 forms.
           No short-vector or enabled-FP-exception semantics are substituted.
           Transfers/status do not consume LEN/STRIDE or arithmetic controls. */
        /* Odd double-register multiple counts are legacy FSTMX/FLDMX forms
           with an extra format word, not ordinary VLDM/VSTM transactions. */
        int multiple = !(v.flags & AV_PRE) || (v.flags & AV_WB);
        if (v.op == AV_MEM && (v.bytes > 64 || (multiple && v.width == 64 && (v.imm & 4)))) {
            trap(a, SNX_ARM_BACKEND_UNSUPPORTED, c32(a, a->word));
            return;
        }
        if (v.op != AV_MEM && v.op != AV_CORE && v.op != AV_CORE_PAIR && v.op != AV_STATUS)
            require(a, cmp(a, LLVMIntEQ, band(a, GET(a, fpscr), c32(a, 0x00379f00)), c32(a, 0)),
                    SNX_ARM_BACKEND_UNSUPPORTED, c32(a, a->word));
    }
    if (v.op == AV_CRYPTO) {
        LLVMTypeRef types[] = {a->ptr, a->i32, a->i32, a->i32, a->i32};
        LLVMTypeRef type = LLVMFunctionType(LLVMVoidTypeInContext(a->ctx), types, 5, 0);
        LLVMValueRef fn = LLVMGetNamedFunction(a->mod, "snx_arm_crypto");
        if (!fn) fn = LLVMAddFunction(a->mod, "snx_arm_crypto", type);
        LLVMValueRef args[] = {a->cpu, c32(a, v.d), c32(a, v.n), c32(a, v.m), c32(a, v.aux)};
        LLVMBuildCall2(a->b, type, fn, args, 5, "");
    } else if (v.op == AV_MEM)
        vector_memory(a, &v);
    else if (v.op == AV_STRUCT)
        vector_struct(a, &v);
    else if (v.op == AV_CORE || v.op == AV_CORE_PAIR || v.op == AV_STATUS || (v.op == AV_DUP && !(v.flags & AV_SIMD)))
        vector_transfer(a, &v);
    else if (v.op == AV_IMM) {
        for (unsigned i = 0; i < v.bytes; i += v.width / 8) {
            LLVMValueRef value = vk(a, v.width, v.imm);
            if (v.aux)
                value = v.aux == 1 ? bor(a, vget(a, v.d + i, v.width), value)
                                   : band(a, vget(a, v.d + i, v.width), LLVMBuildNot(a->b, value, ""));
            vset(a, v.d + i, v.width, value);
        }
    } else if (v.op == AV_MOV || (v.op >= AV_FADD && v.op <= AV_FSELECT) || v.op == AV_URECPE || v.op == AV_URSQRTE)
        vector_fp(a, &v);
    else
        vector_integer(a, &v);
}
/* Integer NEON and structure transfers are defined below. */
static LLVMValueRef vsat(ArmIR* a, LLVMValueRef x, unsigned bits, int uns) {
    unsigned wide = LLVMGetIntTypeWidth(LLVMTypeOf(x));
    uint64_t max = uns ? (bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1) : (UINT64_C(1) << (bits - 1)) - 1;
    LLVMValueRef hi = vk(a, wide, max),
                 lo = uns ? vk(a, wide, 0) : LLVMConstNeg(vk(a, wide, UINT64_C(1) << (bits - 1)));
    LLVMValueRef r = choose(a, cmp(a, LLVMIntSLT, x, lo), lo, choose(a, cmp(a, LLVMIntSGT, x, hi), hi, x));
    PUT(a, fpscr, bor(a, GET(a, fpscr), choose(a, cmp(a, LLVMIntNE, x, r), c32(a, 1u << 27), c32(a, 0))));
    return r;
}
static LLVMValueRef vshift(ArmIR* a, LLVMValueRef x, LLVMValueRef amount, unsigned bits, int uns, int round,
                           int saturate) {
    LLVMValueRef k = vcast(a, amount, 8, 0);
    k = vcast(a, k, 32, 1);
    LLVMValueRef positive = cmp(a, LLVMIntSGE, k, c32(a, 0));
    LLVMValueRef wide = vcast(a, x, 128, !uns), left, right, count = sub(a, c32(a, 0), k);
    count = choose(a, cmp(a, LLVMIntSLT, count, c32(a, 1)), c32(a, 1), count);
    count = choose(a, cmp(a, LLVMIntUGT, count, c32(a, bits + 1)), c32(a, bits + 1), count);
    right = wide;
    if (round) right = add(a, right, LLVMBuildShl(a->b, vk(a, 128, 1), vcast(a, sub(a, count, c32(a, 1)), 128, 0), ""));
    right = LLVMBuildAShr(a->b, right, vcast(a, count, 128, 0), "");
    left = LLVMBuildShl(a->b, wide, vcast(a, band(a, k, c32(a, 63)), 128, 0), "");
    LLVMValueRef big = vk(a, 128, 0);
    if (saturate) {
        LLVMValueRef outside = LLVMBuildShl(a->b, vk(a, 128, 1), vk(a, 128, bits), "");
        big = choose(a, cmp(a, LLVMIntEQ, wide, vk(a, 128, 0)), vk(a, 128, 0),
                     choose(a, cmp(a, LLVMIntSLT, wide, vk(a, 128, 0)), LLVMBuildNeg(a->b, outside, ""), outside));
    }
    left = choose(a, cmp(a, LLVMIntSGE, k, c32(a, bits)), big, left);
    LLVMValueRef r = choose(a, positive, left, right);
    return saturate ? vsat(a, r, bits, uns) : r;
}
static void vector_permute(ArmIR* a, const SnxArmVector* v) {
    LLVMValueRef x[32], y[16], z[16], r[16], t[16];
    unsigned bytes = v->bytes, width = v->width, lanes = bytes * 8 / width;
    if (v->op == AV_EXT || v->op == AV_TBL || v->op == AV_TBX) {
        unsigned table = v->op == AV_EXT ? bytes : v->aux * 8;
        for (unsigned i = 0; i < table; i++) x[i] = vget(a, v->n + i, 8);
        for (unsigned i = 0; i < bytes; i++) {
            y[i] = vget(a, v->m + i, 8);
            z[i] = vget(a, v->d + i, 8);
        }
        for (unsigned i = 0; i < bytes; i++) {
            if (v->op == AV_EXT) {
                unsigned index = (unsigned)v->imm + i;
                r[i] = index < bytes ? x[index] : y[index - bytes];
            } else {
                r[i] = v->op == AV_TBX ? z[i] : vk(a, 8, 0);
                for (unsigned j = 0; j < table; j++) r[i] = choose(a, cmp(a, LLVMIntEQ, y[i], vk(a, 8, j)), x[j], r[i]);
            }
        }
        for (unsigned i = 0; i < bytes; i++) vset(a, v->d + i, 8, r[i]);
        return;
    }
    if (v->op == AV_DUP) {
        LLVMValueRef val = vget(a, v->m, width);
        for (unsigned i = 0; i < lanes; i++) vset(a, v->d + i * width / 8, width, val);
        return;
    }
    /* ARM defines these two-destination aliases as UNKNOWN, not a trap.
       Choose a deterministic zero value rather than copying an emulator's choice. */
    if (v->op != AV_REV && v->d == v->m) {
        for (unsigned i = 0; i < bytes; i += 8) vset(a, v->d + i, 64, c64(a, 0));
        return;
    }
    for (unsigned i = 0; i < lanes; i++) {
        x[i] = vget(a, v->d + i * width / 8, width);
        y[i] = vget(a, v->m + i * width / 8, width);
    }
    for (unsigned i = 0; i < lanes; i++) {
        if (v->op == AV_REV) {
            unsigned group = v->aux * 8 / width;
            r[i] = y[i / group * group + group - 1 - i % group];
        } else if (v->op == AV_SWP) {
            r[i] = y[i];
            t[i] = x[i];
        } else if (v->op == AV_TRN) {
            r[i] = (i & 1) ? y[i - 1] : x[i];
            t[i] = (i & 1) ? y[i] : x[i + 1];
        } else if (v->op == AV_ZIP) {
            r[i] = (i & 1) ? y[i / 2] : x[i / 2];
            t[i] = (i & 1) ? y[(i + lanes) / 2] : x[(i + lanes) / 2];
        } else {
            unsigned j = (i % (lanes / 2)) * 2;
            r[i] = i < lanes / 2 ? x[j] : y[j];
            t[i] = i < lanes / 2 ? x[j + 1] : y[j + 1];
        }
    }
    for (unsigned i = 0; i < lanes; i++) {
        vset(a, v->d + i * width / 8, width, r[i]);
        if (v->op != AV_REV) vset(a, v->m + i * width / 8, width, t[i]);
    }
}
static void vector_integer(ArmIR* a, const SnxArmVector* v) {
    SnxArmVectorOp op = v->op;
    if (op >= AV_REV && op <= AV_SWP) {
        vector_permute(a, v);
        return;
    }
    unsigned width = v->width, out = width, nx = width, my = width;
    int uns = !!(v->flags & AV_UNSIGNED), narrow = op == AV_MOVN || op == AV_QMOVN || op == AV_QMOVUN ||
                                                   op == AV_SHRN || op == AV_QSHRN || op == AV_QSHRUN ||
                                                   op == AV_ADDHN || op == AV_SUBHN;
    int wide = (op >= AV_ADDL && op <= AV_QDMLSL) || op == AV_PADDL || op == AV_PADAL || op == AV_SHLL;
    if (narrow) nx = my = width * 2;
    if (wide) out = width * 2;
    if (op == AV_ADDW || op == AV_SUBW) nx = out;
    unsigned lanes = v->bytes * 8 / out;
    LLVMValueRef x[16], y[16], z[16], r[16];
    int unary = (op >= AV_ABS && op <= AV_NOT) || op == AV_MOVN || op == AV_QMOVN || op == AV_QMOVUN || op == AV_SHRN ||
                op == AV_QSHRN || op == AV_QSHRUN || op == AV_SHLL || op == AV_SHR || op == AV_RSHR || op == AV_SRA ||
                op == AV_RSRA || op == AV_SLI || op == AV_SRI ||
                (v->flags & AV_SCALAR && (op == AV_SHL || op == AV_QSHL || op == AV_QSHLU));
    int pair = op == AV_PADD || op == AV_PMIN || op == AV_PMAX || op == AV_PADDL || op == AV_PADAL;
    for (unsigned i = 0; i < lanes; i++) {
        if (pair) {
            unsigned base = (op == AV_PADDL || op == AV_PADAL) ? v->m : i < lanes / 2 ? v->n : v->m;
            unsigned j = (op == AV_PADDL || op == AV_PADAL) ? i : i % (lanes / 2);
            x[i] = vget(a, base + j * 2 * width / 8, width);
            y[i] = vget(a, base + (j * 2 + 1) * width / 8, width);
        } else {
            x[i] = vget(a, (unary ? v->m : v->n) + i * nx / 8, nx);
            y[i] = vget(a, v->m + (v->flags & AV_SCALAR ? 0 : i * my / 8), my);
        }
        if (v->flags & AV_ZERO) {
            y[i] = vk(a, width, 0);
            if (v->flags & AV_REVERSE) {
                y[i] = x[i];
                x[i] = vk(a, width, 0);
            }
        }
        z[i] = out == 128 ? vk(a, 128, 0) : vget(a, v->d + i * out / 8, out);
    }
    for (unsigned i = 0; i < lanes; i++) {
        LLVMValueRef xx = vcast(a, x[i], 128, !uns), yy = vcast(a, y[i], 128, !uns), rr = NULL;
        LLVMValueRef zero = vk(a, 128, 0), one = vk(a, 128, 1);
        switch (op) {
            case AV_AND: rr = band(a, x[i], y[i]); break;
            case AV_BIC: rr = band(a, x[i], LLVMBuildNot(a->b, y[i], "")); break;
            case AV_ORR: rr = bor(a, x[i], y[i]); break;
            case AV_ORN: rr = bor(a, x[i], LLVMBuildNot(a->b, y[i], "")); break;
            case AV_EOR: rr = bxor(a, x[i], y[i]); break;
            case AV_BSL: rr = bor(a, band(a, z[i], x[i]), band(a, LLVMBuildNot(a->b, z[i], ""), y[i])); break;
            case AV_BIT: rr = bor(a, band(a, y[i], x[i]), band(a, LLVMBuildNot(a->b, y[i], ""), z[i])); break;
            case AV_BIF: rr = bor(a, band(a, LLVMBuildNot(a->b, y[i], ""), x[i]), band(a, y[i], z[i])); break;
            case AV_ADD:
            case AV_ADDL:
            case AV_ADDW:
            case AV_PADD:
            case AV_PADDL:
            case AV_PADAL:
                rr = add(a, xx, yy);
                if (op == AV_PADAL) rr = add(a, rr, vcast(a, z[i], 128, 0));
                break;
            case AV_SUB:
            case AV_SUBL:
            case AV_SUBW: rr = sub(a, xx, yy); break;
            case AV_HADD:
            case AV_RHADD:
            case AV_HSUB:
                rr = op == AV_HSUB ? sub(a, xx, yy) : add(a, xx, yy);
                if (op == AV_RHADD) rr = add(a, rr, one);
                rr = LLVMBuildAShr(a->b, rr, one, "");
                break;
            case AV_QADD:
            case AV_QSUB: rr = vsat(a, op == AV_QADD ? add(a, xx, yy) : sub(a, xx, yy), width, uns); break;
            case AV_CGT:
            case AV_CGE:
            case AV_CEQ:
            case AV_TST: {
                LLVMValueRef c = op == AV_CGT   ? cmp(a, uns ? LLVMIntUGT : LLVMIntSGT, x[i], y[i])
                                 : op == AV_CGE ? cmp(a, uns ? LLVMIntUGE : LLVMIntSGE, x[i], y[i])
                                 : op == AV_CEQ ? cmp(a, LLVMIntEQ, x[i], y[i])
                                                : cmp(a, LLVMIntNE, band(a, x[i], y[i]), vk(a, width, 0));
                rr = LLVMBuildSExt(a->b, c, LLVMTypeOf(x[i]), "");
                break;
            }
            case AV_MIN:
            case AV_MAX:
            case AV_PMIN:
            case AV_PMAX:
                rr = choose(a,
                            cmp(a,
                                (op == AV_MIN || op == AV_PMIN) ? (uns ? LLVMIntULT : LLVMIntSLT)
                                                                : (uns ? LLVMIntUGT : LLVMIntSGT),
                                x[i], y[i]),
                            x[i], y[i]);
                break;
            case AV_ABD:
            case AV_ABA:
            case AV_ABDL:
            case AV_ABAL:
                rr = choose(a, cmp(a, LLVMIntSGT, xx, yy), sub(a, xx, yy), sub(a, yy, xx));
                if (op == AV_ABA || op == AV_ABAL) rr = add(a, rr, vcast(a, z[i], 128, 0));
                break;
            case AV_MUL:
            case AV_MLA:
            case AV_MLS:
            case AV_MULL:
            case AV_MLAL:
            case AV_MLSL:
                rr = LLVMBuildMul(a->b, xx, yy, "");
                if (op == AV_MLA || op == AV_MLAL) rr = add(a, vcast(a, z[i], 128, 0), rr);
                if (op == AV_MLS || op == AV_MLSL) rr = sub(a, vcast(a, z[i], 128, 0), rr);
                break;
            case AV_PMUL:
            case AV_PMULL:
                xx = vcast(a, x[i], 128, 0);
                yy = vcast(a, y[i], 128, 0);
                rr = zero;
                for (unsigned j = 0; j < width; j++)
                    rr =
                        bxor(a, rr,
                             choose(a, cmp(a, LLVMIntNE, band(a, yy, LLVMBuildShl(a->b, one, vk(a, 128, j), "")), zero),
                                    LLVMBuildShl(a->b, xx, vk(a, 128, j), ""), zero));
                break;
            case AV_QDMULL:
            case AV_QDMLAL:
            case AV_QDMLSL:
            case AV_QDMULH:
            case AV_QRDMULH:
                xx = vcast(a, x[i], 128, 1);
                yy = vcast(a, y[i], 128, 1);
                rr = LLVMBuildShl(a->b, LLVMBuildMul(a->b, xx, yy, ""), one, "");
                if (op == AV_QRDMULH) rr = add(a, rr, LLVMBuildShl(a->b, one, vk(a, 128, width - 1), ""));
                if (op == AV_QDMULH || op == AV_QRDMULH) rr = LLVMBuildAShr(a->b, rr, vk(a, 128, width), "");
                rr = vsat(a, rr, out, 0);
                if (op == AV_QDMLAL || op == AV_QDMLSL)
                    rr = vsat(a,
                              op == AV_QDMLAL ? add(a, vcast(a, z[i], 128, 1), rr) : sub(a, vcast(a, z[i], 128, 1), rr),
                              out, 0);
                break;
            case AV_ADDHN:
            case AV_SUBHN:
                rr = op == AV_ADDHN ? add(a, x[i], y[i]) : sub(a, x[i], y[i]);
                if (v->flags & AV_ROUND) rr = add(a, rr, vk(a, nx, UINT64_C(1) << (width - 1)));
                rr = LLVMBuildLShr(a->b, rr, vk(a, nx, width), "");
                break;
            case AV_ABS:
            case AV_NEG:
            case AV_QABS:
            case AV_QNEG:
                xx = vcast(a, x[i], 128, 1);
                rr = LLVMBuildNeg(a->b, xx, "");
                if (op == AV_ABS || op == AV_QABS) rr = choose(a, cmp(a, LLVMIntSLT, xx, zero), rr, xx);
                if (op == AV_QABS || op == AV_QNEG) rr = vsat(a, rr, width, 0);
                break;
            case AV_NOT: rr = LLVMBuildNot(a->b, x[i], ""); break;
            case AV_CLZ:
            case AV_CLS:
            case AV_CNT: {
                LLVMValueRef arg = x[i];
                if (op == AV_CLS) arg = bxor(a, arg, LLVMBuildAShr(a->b, arg, vk(a, width, width - 1), ""));
                LLVMValueRef args[] = {arg, LLVMConstInt(a->i1, 0, 0)};
                rr = intrinsic(a, op == AV_CNT ? "llvm.ctpop" : "llvm.ctlz", op == AV_CNT ? 10 : 9, LLVMTypeOf(arg),
                               args, op == AV_CNT ? 1 : 2);
                if (op == AV_CLS) rr = sub(a, rr, vk(a, width, 1));
                break;
            }
            case AV_SHL:
            case AV_RSHL:
            case AV_QSHL:
            case AV_QRSHL:
            case AV_QSHLU:
                if (v->flags & AV_SCALAR) {
                    if (op == AV_QSHLU) {
                        rr = LLVMBuildShl(a->b, vcast(a, x[i], 128, 1), vk(a, 128, v->imm), "");
                        rr = vsat(a, rr, width, 1);
                    } else
                        rr = vshift(a, x[i], c32(a, (uint32_t)v->imm), width, uns, 0, op == AV_QSHL);
                } else
                    rr = vshift(a, x[i], y[i], width, uns, op == AV_RSHL || op == AV_QRSHL,
                                op == AV_QSHL || op == AV_QRSHL);
                break;
            case AV_SHR:
            case AV_RSHR:
            case AV_SRA:
            case AV_RSRA:
                rr = xx;
                if (op == AV_RSHR || op == AV_RSRA)
                    rr = add(a, rr, LLVMBuildShl(a->b, one, vk(a, 128, v->imm - 1), ""));
                rr = LLVMBuildAShr(a->b, rr, vk(a, 128, v->imm), "");
                if (op == AV_SRA || op == AV_RSRA) rr = add(a, rr, vcast(a, z[i], 128, 0));
                break;
            case AV_SLI:
            case AV_SRI: {
                unsigned shift = (unsigned)v->imm;
                if (op == AV_SRI && shift == width)
                    rr = z[i];
                else {
                    uint64_t mask = op == AV_SLI ? (UINT64_C(1) << shift) - 1 : ~(UINT64_MAX >> (64 - width + shift));
                    rr = op == AV_SLI ? LLVMBuildShl(a->b, x[i], vk(a, width, shift), "")
                                      : LLVMBuildLShr(a->b, x[i], vk(a, width, shift), "");
                    rr = bor(a, rr, band(a, z[i], vk(a, width, mask)));
                }
                break;
            }
            case AV_SHLL: rr = LLVMBuildShl(a->b, xx, vk(a, 128, v->imm), ""); break;
            case AV_MOVN: rr = x[i]; break;
            case AV_QMOVN:
            case AV_QMOVUN:
            case AV_SHRN:
            case AV_QSHRN:
            case AV_QSHRUN:
                if (op == AV_QMOVUN || op == AV_QSHRUN) xx = vcast(a, x[i], 128, 1);
                rr = xx;
                if (op == AV_SHRN || op == AV_QSHRN || op == AV_QSHRUN) {
                    if (v->flags & AV_ROUND) rr = add(a, rr, LLVMBuildShl(a->b, one, vk(a, 128, v->imm - 1), ""));
                    rr = LLVMBuildAShr(a->b, rr, vk(a, 128, v->imm), "");
                }
                if (op != AV_SHRN) rr = vsat(a, rr, width, uns || op == AV_QMOVUN || op == AV_QSHRUN);
                break;
            default: trap(a, SNX_ARM_UNDEFINED, c32(a, a->word)); return;
        }
        r[i] = vcast(a, rr, out, 0);
    }
    for (unsigned i = 0; i < lanes; i++) {
        if (out == 128) {
            vset(a, v->d, 64, vcast(a, r[i], 64, 0));
            vset(a, v->d + 8, 64, vcast(a, LLVMBuildLShr(a->b, r[i], vk(a, 128, 64), ""), 64, 0));
        } else
            vset(a, v->d + i * out / 8, out, r[i]);
    }
}
static void vector_struct(ArmIR* a, const SnxArmVector* v) {
    unsigned eb = v->width / 8, lanes = 8 / eb;
    LLVMValueRef base = reg(a, v->n), offset = v->m == 13 ? c32(a, v->bytes) : reg(a, v->m);
    bounds(a, base, v->bytes, v->alignment);
    unsigned address = 0;
    for (unsigned g = 0; g < v->groups; g++) {
        unsigned elements = v->flags & (AV_LANE | AV_REPLICATE) ? 1 : lanes;
        for (unsigned e = 0; e < elements; e++)
            for (unsigned s = 0; s < v->count; s++) {
                unsigned d = v->d + (g + s * v->stride) * 8 + (v->flags & AV_LANE ? v->lane : e) * eb;
                unsigned at = v->flags & AV_REPLICATE ? s * eb : address;
                LLVMValueRef p = add(a, base, c32(a, at));
                if (v->flags & AV_LOAD) {
                    LLVMValueRef x = readmem(a, p, eb > 4 ? 4 : eb, 0);
                    if (eb == 8)
                        x = bor(a, z64(a, x),
                                LLVMBuildShl(a->b, z64(a, readmem(a, add(a, p, c32(a, 4)), 4, 0)), c64(a, 32), ""));
                    else
                        x = vcast(a, x, v->width, 0);
                    if (v->flags & AV_REPLICATE)
                        for (unsigned k = 0; k < lanes; k++) vset(a, d + k * eb, v->width, x);
                    else
                        vset(a, d, v->width, x);
                } else {
                    LLVMValueRef x = vget(a, d, v->width);
                    writemem(a, p, eb > 4 ? 4 : eb, vcast(a, x, 32, 0));
                    if (eb == 8)
                        writemem(a, add(a, p, c32(a, 4)), 4, vcast(a, LLVMBuildLShr(a->b, x, c64(a, 32), ""), 32, 0));
                }
                address += eb;
            }
    }
    if (v->flags & AV_WB) setreg(a, v->n, add(a, base, offset));
}
