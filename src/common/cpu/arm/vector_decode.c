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
#include <string.h>

static unsigned vd(uint32_t w) { return (((w >> 12) & 15) | ((w >> 18) & 16)) * 8; }
static unsigned vn(uint32_t w) { return (((w >> 16) & 15) | ((w >> 3) & 16)) * 8; }
static unsigned vm(uint32_t w) { return ((w & 15) | ((w >> 1) & 16)) * 8; }
static unsigned sd(uint32_t w) { return (((w >> 11) & 30) | ((w >> 22) & 1)) * 4; }
static unsigned sn(uint32_t w) { return (((w >> 15) & 30) | ((w >> 7) & 1)) * 4; }
static unsigned sm(uint32_t w) { return (((w << 1) & 30) | ((w >> 5) & 1)) * 4; }
static uint64_t fpimm(unsigned imm, unsigned width) {
    unsigned frac = width == 64 ? 52 : 23, bias = width == 64 ? 1023 : 127;
    return ((uint64_t)(imm >> 7) << (width - 1)) | ((uint64_t)(bias - 3 + (((imm >> 4) & 7) ^ 4)) << frac) |
           ((uint64_t)(imm & 15) << (frac - 4));
}
/* VFP register transfers and loads/stores precede the arithmetic space. */
static int vfp(uint32_t w, SnxArmVector* v) {
    unsigned sz = (w >> 8) & 1, op, k;
    if ((w & 0x0fe00f7f) == 0x0e000a10) {
        v->op = AV_CORE;
        v->d = sn(w);
        v->n = (w >> 12) & 15;
        v->width = 32;
        v->flags = w & (1u << 20) ? AV_LOAD : 0;
        return v->n != 15;
    }
    if ((w & 0x0fe00fd0) == 0x0c400a10 || (w & 0x0fe00fd0) == 0x0c400b10) {
        v->op = AV_CORE_PAIR;
        v->d = sz ? vm(w) : sm(w);
        v->n = (w >> 12) & 15;
        v->m = (w >> 16) & 15;
        v->flags = w & (1u << 20) ? AV_LOAD : 0;
        return v->n != 15 && v->m != 15 && (!(v->flags & AV_LOAD) || v->n != v->m) && (sz || v->d < 124);
    }
    if ((w & 0x0fef0fff) == 0x0ee10a10) {
        v->op = AV_STATUS;
        v->n = (w >> 12) & 15;
        v->flags = w & (1u << 20) ? AV_LOAD : 0;
        return v->n != 15 || (v->flags & AV_LOAD);
    }
    if ((w & 0x0e000e00) == 0x0c000a00) {
        unsigned p = (w >> 24) & 1, u = (w >> 23) & 1, wb = (w >> 21) & 1;
        v->op = AV_MEM;
        v->d = sz ? vd(w) : sd(w);
        v->n = (w >> 16) & 15;
        v->flags = ((w >> 20) & 1 ? AV_LOAD : 0) | (u ? AV_UP : 0) | (p ? AV_PRE : 0) | (wb ? AV_WB : 0);
        v->width = sz ? 64 : 32;
        v->imm = (w & 255) * 4;
        /* A32 allows a PC base without writeback for stores and VLDM too. Thumb doesn't (thumb_decode.c) */
        if (p && !wb) { /* VLDR/VSTR (including literals) */
            v->bytes = v->width / 8;
            return 1;
        }
        if ((!p && !u) || (p && (u || !wb)) || (v->n == 15 && wb)) return 0;
        v->bytes = (w & 255) * 4;
        if (sz) {
            v->bytes &= ~7u;
            if (v->bytes > 128 || ((w & 1) && v->d + v->bytes > 128)) return 0;
        }
        if (!v->bytes || v->d + v->bytes > (sz ? 256 : 128)) return 0;
        return 1;
    }
    if ((w & 0x0f000e10) != 0x0e000a00) return 0;
    v->width = sz ? 64 : 32;
    v->bytes = v->width / 8;
    v->d = sz ? vd(w) : sd(w);
    v->n = sz ? vn(w) : sn(w);
    v->m = sz ? vm(w) : sm(w);
    op = (w >> 20) & 11; /* D bit is not an opcode */
    k = (w >> 6) & 1;
    switch (op) {
        case 0: v->op = k ? AV_FMLS : AV_FMLA; return 1;
        case 1: v->op = k ? AV_FNMLA : AV_FNMLS; return 1;
        case 2: v->op = k ? AV_FNMUL : AV_FMUL; return 1;
        case 3: v->op = k ? AV_FSUB : AV_FADD; return 1;
        case 8:
            if (k) return 0;
            v->op = AV_FDIV;
            return 1;
        case 9: v->op = k ? AV_FNMA : AV_FNMS; return 1;
        case 10: v->op = k ? AV_FMS : AV_FMA; return 1;
        default: break;
    }
    if (!k) {
        if (w & 0xa0) return 0;
        v->op = AV_IMM;
        v->imm = fpimm(((w >> 12) & 0xf0) | (w & 15), v->width);
        return 1;
    }
    op = (w >> 16) & 15;
    k = (w >> 7) & 1;
    switch (op) {
        case 0: v->op = k ? AV_FABS : AV_MOV; break;
        case 1: v->op = k ? AV_FSQRT : AV_FNEG; break;
        case 2:
        case 3: /* half conversions preserve the other half of Sd */
            v->op = AV_FCVT;
            v->aux = op == 2 ? v->width : 16;
            if (op == 2) {
                v->m = sm(w) + k * 2;
                v->width = 16;
            } else {
                v->d = sd(w) + k * 2;
                v->bytes = 2;
            }
            v->imm = 8;
            break;
        case 4:
        case 5:
            if (op == 5 && (w & 0x2f)) return 0;
            v->op = k ? AV_FCMPE : AV_FCMP;
            v->n = v->d;
            v->aux = op == 5;
            break;
        case 6:
            v->op = AV_FROUND;
            v->imm = k ? 3 : 8;
            break;
        case 7:
            if (!k) {
                v->op = AV_FROUND;
                v->imm = 8;
                v->aux = 1;
            } else {
                v->op = AV_FCVT;
                v->aux = sz ? 32 : 64;
                v->d = sz ? sd(w) : vd(w);
                v->bytes = v->aux / 8;
                v->imm = 8;
            }
            break;
        case 8:
            v->op = AV_ITOF;
            v->m = sm(w);
            v->aux = 32;
            v->imm = 0;
            if (!k) v->flags |= AV_UNSIGNED;
            break;
        case 12:
        case 13:
            v->op = AV_FTOI;
            v->d = sd(w);
            v->bytes = 4;
            v->aux = 32;
            v->imm = k ? 3 : 8;
            if (op == 12) v->flags |= AV_UNSIGNED;
            break;
        case 10:
        case 11:
        case 14:
        case 15: {
            unsigned iw = k ? 32 : 16, fbits = iw - (((w & 15) << 1) | ((w >> 5) & 1));
            if (fbits > iw) return 0;
            v->op = op & 4 ? AV_FTOI : AV_ITOF;
            v->m = v->d;
            v->aux = iw;
            v->imm = (fbits << 8) | (op & 4 ? 3 : 8);
            if (op & 1) v->flags |= AV_UNSIGNED;
            break;
        }
        default: return 0;
    }
    return 1;
}
static int vfp_v8(uint32_t w, SnxArmVector* v) {
    unsigned sz = (w >> 8) & 1, op = (w >> 20) & 11, k = (w >> 6) & 1;
    if ((w & 0xff000e10) != 0xfe000a00) return 0;
    v->width = sz ? 64 : 32;
    v->bytes = v->width / 8;
    v->d = sz ? vd(w) : sd(w);
    v->n = sz ? vn(w) : sn(w);
    v->m = sz ? vm(w) : sm(w);
    if (op <= 3 && !k) {
        v->op = AV_FSELECT;
        v->aux = op;
        return 1;
    }
    if (op == 8) {
        v->op = k ? AV_FMINNM : AV_FMAXNM;
        return 1;
    }
    if (op != 11 || !k) return 0;
    op = (w >> 16) & 15;
    if (op < 8) return 0;
    v->imm = (unsigned[]){4, 0, 1, 2}[op & 3];
    if (op < 12) {
        if (w & 128) return 0;
        v->op = AV_FROUND;
    } else {
        v->op = AV_FTOI;
        v->d = sd(w);
        v->bytes = 4;
        v->aux = 32;
        if (!(w & 128)) v->flags |= AV_UNSIGNED;
    }
    return 1;
}

/* Advanced SIMD decoders below use the same register bank as VFP. */
static int neon(uint32_t w, SnxArmVector* v);
static int structures(uint32_t w, SnxArmVector* v);
static int core_lane(uint32_t w, SnxArmVector* v);
int snx_arm_vector_decode(uint32_t w, SnxArmVector* v) {
    memset(v, 0, sizeof(*v));
    if ((w >> 28) != 15) return vfp(w, v) || core_lane(w, v);
    return vfp_v8(w, v) || neon(w, v) || structures(w, v);
}

/* Filled separately from the scalar VFP space, which is predicated. */
static int core_lane(uint32_t w, SnxArmVector* v) {
    if ((w & 0x0f900f5f) == 0x0e800b10) {
        if ((w & (1u << 22)) && (w & 32)) return 0;
        v->op = AV_DUP;
        v->d = vn(w);
        v->n = (w >> 12) & 15;
        v->width = w & (1u << 22) ? 8 : w & 32 ? 16 : 32;
        v->bytes = w & (1u << 21) ? 16 : 8;
        return v->n != 15 && (v->bytes != 16 || !(v->d & 8));
    }
    if ((w & 0x0f000f1f) != 0x0e000b10) return 0;
    unsigned a = (w >> 21) & 3, b = (w >> 5) & 3, lane;
    if (a & 2) {
        v->width = 8;
        lane = (a & 1) * 4 + b;
    } else if (b & 1) {
        v->width = 16;
        lane = (a & 1) * 2 + (b >> 1);
    } else if (!b) {
        v->width = 32;
        lane = a & 1;
    } else
        return 0;
    v->op = AV_CORE;
    v->d = vn(w) + lane * v->width / 8;
    v->n = (w >> 12) & 15;
    v->flags = (w & (1u << 20) ? AV_LOAD : 0) | (w & (1u << 23) ? AV_UNSIGNED : 0);
    if ((v->flags & AV_UNSIGNED) && (!(v->flags & AV_LOAD) || v->width == 32)) return 0;
    return v->n != 15;
}
static int same_length(uint32_t w, SnxArmVector* v) {
    unsigned op = (w >> 8) & 15, b = (w >> 4) & 1, u = (w >> 24) & 1, size = (w >> 20) & 3;
    v->width = 8u << size;
    v->flags = AV_SIMD | (u ? AV_UNSIGNED : 0);
    switch (op) {
        case 0: v->op = b ? AV_QADD : AV_HADD; break;
        case 1:
            if (!b)
                v->op = AV_RHADD;
            else {
                v->op =
                    (SnxArmVectorOp[]){AV_AND, AV_BIC, AV_ORR, AV_ORN, AV_EOR, AV_BSL, AV_BIT, AV_BIF}[u * 4 + size];
                v->width = 8;
                return 1;
            }
            break;
        case 2: v->op = b ? AV_QSUB : AV_HSUB; break;
        case 3: v->op = b ? AV_CGE : AV_CGT; break;
        case 4:
        case 5:
            v->op = op == 4 ? (b ? AV_QSHL : AV_SHL) : (b ? AV_QRSHL : AV_RSHL);
            v->n = vm(w);
            v->m = vn(w);
            return 1;
        case 6: v->op = b ? AV_MIN : AV_MAX; break;
        case 7: v->op = b ? AV_ABA : AV_ABD; break;
        case 8: v->op = b ? (u ? AV_CEQ : AV_TST) : (u ? AV_SUB : AV_ADD); break;
        case 9:
            v->op = b ? (u ? AV_PMUL : AV_MUL) : (u ? AV_MLS : AV_MLA);
            if (b && u && size) return 0;
            break;
        case 10:
            v->op = b ? AV_PMIN : AV_PMAX;
            if (v->bytes != 8) return 0;
            break;
        case 11:
            if (b) {
                if (u || v->bytes != 8) return 0;
                v->op = AV_PADD;
            } else {
                if (size != 1 && size != 2) return 0;
                v->op = u ? AV_QRDMULH : AV_QDMULH;
                v->flags = AV_SIMD;
            }
            break;
        default:
            if (size & 1) return 0; /* no FP16 arithmetic on Cortex-A57 */
            v->width = 32;
            v->flags = AV_SIMD;
            if (op == 12) {
                if (u || !b) return 0;
                v->op = size ? AV_FMS : AV_FMA;
            }
            if (op == 13) {
                if (!b) {
                    v->op = u ? (size ? AV_FABD : AV_FADD) : (size ? AV_FSUB : AV_FADD);
                    if (u && !size) v->flags |= AV_PAIR;
                } else {
                    if (u && size) return 0;
                    v->op = u ? AV_FMUL : size ? AV_FMLS : AV_FMLA;
                }
            }
            if (op == 14) {
                if (!u) {
                    if (size || b) return 0;
                    v->op = AV_FCEQ;
                } else
                    v->op = b ? (size ? AV_FACGT : AV_FACGE) : (size ? AV_FCGT : AV_FCGE);
            }
            if (op == 15) {
                if (b)
                    v->op = u ? (size ? AV_FMINNM : AV_FMAXNM) : (size ? AV_FRSQRTS : AV_FRECPS);
                else {
                    v->op = size ? AV_FMIN : AV_FMAX;
                    if (u) v->flags |= AV_PAIR;
                }
            }
            return !(v->flags & AV_PAIR) || v->bytes == 8;
    }
    return size != 3 || v->op == AV_ADD || v->op == AV_SUB || v->op == AV_QADD || v->op == AV_QSUB;
}
static int misc_neon(uint32_t w, SnxArmVector* v) {
    unsigned size = (w >> 18) & 3, group = (w >> 16) & 3, b = (w >> 7) & 31, q = (w >> 6) & 1;
    v->width = 8u << size;
    v->flags = AV_SIMD;
    if (b >= 16) return 0;
    if (group == 0) {
        if (b <= 2) {
            v->op = AV_REV;
            v->aux = 8u >> b;
            if (v->width >= v->aux * 8) return 0;
        } else if (b == 4 || b == 5 || b == 12 || b == 13) {
            v->op = b < 8 ? AV_PADDL : AV_PADAL;
            if (b & 1) v->flags |= AV_UNSIGNED;
        } else if (b == 8 || b == 9)
            v->op = b == 8 ? AV_CLS : AV_CLZ;
        else if (b == 10 || b == 11) {
            if (size) return 0;
            v->op = b == 10 ? AV_CNT : AV_NOT;
        } else if (b == 14 || b == 15)
            v->op = b == 14 ? AV_QABS : AV_QNEG;
        else
            return 0;
    } else if (group == 1) {
        if (b >= 8) {
            if (size != 2) return 0;
            v->width = 32;
        }
        unsigned k = b & 7;
        if (k <= 4) {
            v->op = b < 8 ? (k == 2               ? AV_CEQ
                             : (k == 0 || k == 4) ? AV_CGT
                                                  : AV_CGE)
                          : (k == 2               ? AV_FCEQ
                             : (k == 0 || k == 4) ? AV_FCGT
                                                  : AV_FCGE);
            v->n = v->m;
            v->flags |= AV_ZERO;
            if (k == 3 || k == 4) v->flags |= AV_REVERSE;
        } else if (k == 6 || k == 7)
            v->op = b < 8 ? (k == 6 ? AV_ABS : AV_NEG) : (k == 6 ? AV_FABS : AV_FNEG);
        else
            return 0;
    } else if (group == 2) {
        if (b < 4) {
            v->op = (SnxArmVectorOp[]){AV_SWP, AV_TRN, AV_UZP, AV_ZIP}[b];
            if (!b && size) return 0;
            if (size == 2 && !q && b >= 2) return 0;
        } else if (b == 4 || b == 5) {
            v->op = b == 4 ? (q ? AV_QMOVUN : AV_MOVN) : AV_QMOVN;
            if (b == 5 && q) v->flags |= AV_UNSIGNED;
            v->bytes = 8;
            if (v->m & 8) return 0;
        } else if (b == 6) {
            if (q || v->d & 8) return 0;
            v->op = AV_SHLL;
            v->bytes = 16;
            v->imm = v->width;
            v->flags |= AV_UNSIGNED;
        } else if ((b == 12 || b == 14) && size == 1) {
            if (q) return 0;
            v->op = AV_FCVT;
            v->width = b == 12 ? 32 : 16;
            v->aux = b == 12 ? 16 : 32;
            v->bytes = b == 12 ? 8 : 16;
            v->imm = 0;
            return b == 12 ? !(v->m & 8) : !(v->d & 8);
        } else if (size == 2 && (b == 8 || b == 9 || b == 10 || b == 11 || b == 13 || b == 15)) {
            v->op = AV_FROUND;
            v->imm = b == 8 || b == 9 ? 0 : b == 10 ? 4 : b == 11 ? 3 : b == 13 ? 2 : 1;
            v->aux = b == 9;
        } else
            return 0;
    } else {
        if (size != 2) return 0;
        if (b < 8) {
            v->op = AV_FTOI;
            v->aux = 32;
            v->imm = (unsigned[]){4, 0, 1, 2}[b >> 1];
            if (b & 1) v->flags |= AV_UNSIGNED;
        } else if (b < 12)
            v->op = (SnxArmVectorOp[]){AV_URECPE, AV_URSQRTE, AV_FRECPE, AV_FRSQRTE}[b - 8];
        else {
            v->op = b < 14 ? AV_ITOF : AV_FTOI;
            v->aux = 32;
            v->imm = b < 14 ? 0 : 3;
            if (b & 1) v->flags |= AV_UNSIGNED;
        }
    }
    if (size == 3) return 0;
    if (v->op == AV_MOVN || v->op == AV_QMOVN || v->op == AV_QMOVUN || v->op == AV_SHLL) return 1;
    return !q || !((v->d | v->m) & 8);
}
static int neon(uint32_t w, SnxArmVector* v) {
    unsigned q = (w >> 6) & 1, u = (w >> 24) & 1, size = (w >> 20) & 3, op = (w >> 8) & 15;
    v->d = vd(w);
    v->n = vn(w);
    v->m = vm(w);
    v->bytes = q ? 16 : 8;
    v->flags = AV_SIMD;
    if ((w & 0xffbf0f10) == 0xf3b00300) {
        v->op = AV_CRYPTO;
        v->aux = (w >> 6) & 3;
        return !((v->d | v->m) & 8);
    }
    if ((w & 0xffbf0fd0) == 0xf3b902c0 || (w & 0xffbf0f90) == 0xf3ba0380) {
        v->op = AV_CRYPTO;
        v->aux = (w & 0x00010000) ? 11 : (w & 64) ? 13 : 12;
        return !((v->d | v->m) & 8);
    }
    if ((w & 0xfe800f50) == 0xf2000c40) {
        v->op = AV_CRYPTO;
        v->aux = u ? 8 + size : 4 + size;
        return (!u || size < 3) && !((v->d | v->n | v->m) & 8);
    }
    if ((w & 0xfe800000) == 0xf2000000) {
        if (q && ((v->d | v->n | v->m) & 8)) return 0;
        return same_length(w, v);
    }
    if ((w & 0xffb00010) == 0xf2b00000) {
        v->op = AV_EXT;
        v->width = 8;
        v->imm = op;
        return op < v->bytes && (!q || !((v->d | v->n | v->m) & 8));
    }
    if ((w & 0xffb00c10) == 0xf3b00800) {
        v->op = q ? AV_TBX : AV_TBL;
        v->bytes = 8;
        v->width = 8;
        v->aux = ((w >> 8) & 3) + 1;
        return v->n + v->aux * 8 <= 256;
    }
    if ((w & 0xffb00f90) == 0xf3b00c00) {
        unsigned imm = (w >> 16) & 15, shift = 0;
        if (!imm || (imm & 7) == 0) return 0;
        while (!(imm & (1u << shift))) ++shift;
        v->op = AV_DUP;
        v->width = 8u << shift;
        v->m += (imm >> (shift + 1)) * (v->width / 8);
        return !q || !(v->d & 8);
    }
    if ((w & 0xffb00810) == 0xf3b00000) return misc_neon(w, v);
    if ((w & 0xfeb80090) == 0xf2800010) {
        unsigned imm = ((w >> 17) & 128) | ((w >> 12) & 112) | (w & 15), invert = (w >> 5) & 1;
        if (q && (v->d & 8)) return 0;
        v->op = AV_IMM;
        v->width = 32;
        if (op < 8)
            v->imm = (uint64_t)imm << ((op >> 1) * 8);
        else if (op < 12) {
            v->width = 16;
            v->imm = imm << (((op >> 1) & 1) * 8);
        } else if (op == 12)
            v->imm = imm << 8 | 255;
        else if (op == 13)
            v->imm = imm << 16 | 65535;
        else if (op == 14) {
            if (!invert) {
                v->width = 8;
                v->imm = imm;
            } else {
                v->width = 64;
                v->imm = 0;
                for (unsigned i = 0; i < 8; i++)
                    if (imm & (1u << i)) v->imm |= UINT64_C(255) << (i * 8);
            }
            return 1;
        } else {
            if (invert) return 0;
            v->imm = fpimm(imm, 32);
            return 1;
        }
        if ((op & 1) && op < 12)
            v->aux = invert ? 2 : 1;
        else if (invert)
            v->imm = ~v->imm;
        return 1;
    }
    if ((w & 0xfe800010) == 0xf2800010) {
        unsigned imm = ((w >> 16) & 63) | ((w >> 1) & 64), width = 64;
        while (width && !(imm & width)) width >>= 1;
        if (width < 8) return 0;
        v->width = width;
        v->flags |= u ? AV_UNSIGNED : 0;
        if (op <= 3) {
            v->op = (SnxArmVectorOp[]){AV_SHR, AV_SRA, AV_RSHR, AV_RSRA}[op];
            v->imm = 2 * width - imm;
        } else if (op == 4) {
            if (!u) return 0;
            v->op = AV_SRI;
            v->imm = 2 * width - imm;
        } else if (op == 5) {
            v->op = u ? AV_SLI : AV_SHL;
            v->flags |= AV_SCALAR;
            v->imm = imm - width;
        } else if (op == 6 || op == 7) {
            if (op == 6 && !u) return 0;
            v->op = op == 6 ? AV_QSHLU : AV_QSHL;
            v->flags |= AV_SCALAR;
            v->imm = imm - width;
        } else if (op == 8 || op == 9) {
            if (width > 32 || v->m & 8) return 0;
            v->op = op == 8 ? (u ? AV_QSHRUN : AV_SHRN) : AV_QSHRN;
            v->bytes = 8;
            v->imm = 2 * width - imm;
            if (q) v->flags |= AV_ROUND;
            return 1;
        } else if (op == 10) {
            if (width > 32 || q || v->d & 8) return 0;
            v->op = AV_SHLL;
            v->bytes = 16;
            v->imm = imm - width;
            return 1;
        } else if (op == 14 || op == 15) {
            if (width != 32) return 0;
            v->op = op == 14 ? AV_ITOF : AV_FTOI;
            v->aux = 32;
            v->imm = ((64 - imm) << 8) | (op == 15 ? 3 : 0);
        } else
            return 0;
        return !q || !((v->d | v->m) & 8);
    }
    if ((w & 0xfe800050) == 0xf2800040) { /* by-element */
        if (size != 1 && size != 2) return 0;
        v->width = 8u << size;
        v->flags |= AV_SCALAR;
        v->m = size == 1 ? (w & 7) * 8 + (((w >> 5) & 1) * 2 + ((w >> 3) & 1)) * 2 : (w & 15) * 8 + ((w >> 5) & 1) * 4;
        if (op == 0 || op == 4 || op == 8 || op == 1 || op == 5 || op == 9 || op == 12 || op == 13) {
            v->bytes = u ? 16 : 8;
            if (u && ((v->d | v->n) & 8)) return 0;
            v->op = op == 0    ? AV_MLA
                    : op == 4  ? AV_MLS
                    : op == 8  ? AV_MUL
                    : op == 12 ? AV_QDMULH
                    : op == 13 ? AV_QRDMULH
                    : op == 1  ? AV_FMLA
                    : op == 5  ? AV_FMLS
                               : AV_FMUL;
            return !(op & 1) || op == 13 || size == 2;
        }
        v->bytes = 16;
        if (v->d & 8) return 0;
        if (op == 2 || op == 6 || op == 10) {
            v->op = op == 2 ? AV_MLAL : op == 6 ? AV_MLSL : AV_MULL;
            if (u) v->flags |= AV_UNSIGNED;
        } else if (op == 3 || op == 7 || op == 11) {
            if (u) return 0;
            v->op = op == 3 ? AV_QDMLAL : op == 7 ? AV_QDMLSL : AV_QDMULL;
        } else
            return 0;
        return 1;
    }
    if ((w & 0xfe800050) == 0xf2800000) {
        if (size == 3) return 0;
        v->width = 8u << size;
        v->bytes = 16;
        v->flags |= u ? AV_UNSIGNED : 0;
        switch (op) {
            case 0: v->op = AV_ADDL; break;
            case 1: v->op = AV_ADDW; break;
            case 2: v->op = AV_SUBL; break;
            case 3: v->op = AV_SUBW; break;
            case 4:
            case 6:
                v->op = op == 4 ? AV_ADDHN : AV_SUBHN;
                v->bytes = 8;
                if (u) v->flags |= AV_ROUND;
                return !((v->n | v->m) & 8);
            case 5: v->op = AV_ABAL; break;
            case 7: v->op = AV_ABDL; break;
            case 8: v->op = AV_MLAL; break;
            case 10: v->op = AV_MLSL; break;
            case 12: v->op = AV_MULL; break;
            case 9:
            case 11:
            case 13:
                if (u || !size) return 0;
                v->op = op == 9 ? AV_QDMLAL : op == 11 ? AV_QDMLSL : AV_QDMULL;
                break;
            case 14:
                if (u || size == 1) return 0;
                v->op = AV_PMULL;
                if (size == 2) v->width = 64;
                break;
            default: return 0;
        }
        if ((v->op == AV_ADDW || v->op == AV_SUBW) && (v->n & 8)) return 0;
        return !(v->d & 8);
    }
    return 0;
}
static int structures(uint32_t w, SnxArmVector* v) {
    if ((w & 0xff100000) != 0xf4000000) return 0;
    unsigned type = (w >> 8) & 15, size = (w >> 6) & 3, align = (w >> 4) & 3;
    v->op = AV_STRUCT;
    v->d = vd(w);
    v->n = (w >> 16) & 15;
    v->m = w & 15;
    v->flags = AV_SIMD | (w & (1u << 21) ? AV_LOAD : 0) | (v->m != 15 ? AV_WB : 0);
    v->groups = v->stride = 1;
    v->alignment = 1;
    if (v->n == 15) return 0;
    if (!(w & (1u << 23))) {
        switch (type) {
            case 0:
            case 1:
                v->count = 4;
                v->stride = type + 1;
                break;
            case 2:
                v->count = 1;
                v->groups = 4;
                break;
            case 3:
                v->count = 2;
                v->groups = 2;
                v->stride = 2;
                break;
            case 4:
            case 5:
                v->count = 3;
                v->stride = type - 3;
                if (align > 1) return 0;
                break;
            case 6:
                v->count = 1;
                v->groups = 3;
                if (align > 1) return 0;
                break;
            case 7:
                v->count = 1;
                if (align > 1) return 0;
                break;
            case 8:
            case 9:
                v->count = 2;
                v->stride = type - 7;
                if (align == 3) return 0;
                break;
            case 10:
                v->count = 1;
                v->groups = 2;
                if (align == 3) return 0;
                break;
            default: return 0;
        }
        if (v->count != 1 && size == 3) return 0;
        v->width = 8u << size;
        v->bytes = v->groups * v->count * 8;
        v->alignment = align ? 4u << align : 1;
    } else if (type >= 12) {
        unsigned t = (w >> 5) & 1, a = (w >> 4) & 1;
        if (!(v->flags & AV_LOAD)) return 0;
        v->flags |= AV_REPLICATE;
        v->count = (type & 3) + 1;
        if (v->count == 4 && size == 3) {
            if (!a) return 0;
            size = 2;
            v->alignment = 16;
        } else {
            if (size == 3 || (v->count == 3 && a) || (v->count == 1 && !size && a)) return 0;
            if (a) v->alignment = v->count * (1u << size);
            if (v->count == 4 && size == 2 && a) v->alignment = 8;
        }
        v->width = 8u << size;
        if (v->count == 1)
            v->groups = t + 1;
        else
            v->stride = t + 1;
        v->bytes = v->count * (v->width / 8);
    } else {
        size = type >> 2;
        v->count = (type & 3) + 1;
        unsigned index = (w >> 4) & 15;
        v->flags |= AV_LANE;
        v->width = 8u << size;
        v->lane = index >> (size + 1);
        if (size && v->count != 1) v->stride = index & (1u << size) ? 2 : 1;
        unsigned a = index & ((1u << size) - 1);
        if (!size) a = index & 1;
        if (v->count == 1) {
            if (index & (1u << size)) return 0;
            if (size == 2 && a != 0 && a != 3) return 0;
            if (a) v->alignment = 1u << size;
        } else if (v->count == 2) {
            if (size == 2 && (index & 2)) return 0;
            if (index & 1) v->alignment = 2u << size;
        } else if (v->count == 3) {
            if (a) return 0;
        } else {
            if (size == 2 && a == 3) return 0;
            if (size == 2)
                v->alignment = a ? 4u << a : 1;
            else if (index & 1)
                v->alignment = 4u << size;
        }
        v->bytes = v->count * (v->width / 8);
    }
    return v->d + (v->groups - 1 + (v->count - 1) * v->stride) * 8 + 8 <= 256;
}
