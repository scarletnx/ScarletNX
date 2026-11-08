/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/arm/successors.h"
#include "common/cpu/arm/arm.h"
#include "common/utilities/endian.h"

/* whether control never reaches the instruction after the A32 instruction w when it runs: a branch without link, a
   write to the PC, BKPT. A call returns there, as does an instruction the block compiler leaves to the backend
   (SnxArmRt.undefined), which may run it */
static int a32_ends(SnxArmProfile profile, uint32_t w) {
    unsigned rd = (w >> 12) & 15;
    switch (snx_a32_decode_profile(profile, w)) {
        case SNX_A32_BRANCH: return !(w & (1u << 24)); /* B; BL returns */
        case SNX_A32_BX: return !(w & 0x20);           /* BX; BLX returns */
        case SNX_A32_DATA: return rd == 15;
        case SNX_A32_SINGLE:
        case SNX_A32_EXTRA: return rd == 15 && (w & (1u << 20));
        case SNX_A32_MULTIPLE: return (w & (1u << 15)) && (w & (1u << 20));
        case SNX_A32_BKPT: return 1;
        default: return 0;
    }
}
static void a32_next(SnxArmProfile profile, uint32_t pc, uint32_t w, void (*found)(void*, uint32_t, int),
                     void* context) {
    SnxA32Kind kind = snx_a32_decode_profile(profile, w);
    unsigned cond = w >> 28;
    if (kind == SNX_A32_BRANCH) found(context, pc + 8 + ((uint32_t)snx_sext32(w & 0xFFFFFF, 24) << 2), 0);
    if (kind == SNX_A32_BLX_IMM)
        found(context, pc + 8 + ((uint32_t)snx_sext32(w & 0xFFFFFF, 24) << 2) + ((w >> 23) & 2), 1);
    if (!(a32_ends(profile, w) && (cond == 14 || cond == 15))) found(context, pc + 4, 0);
}
/* Its instructions are walked as the block compiler walks them, IT state included, to its last one */
static void thumb_next(SnxArmProfile profile, uint32_t pc, const unsigned char* code, size_t units, size_t count,
                       void (*found)(void*, uint32_t, int), void* context) {
    size_t used = 0, len = 1;
    unsigned it_len = 0, it_idx = 0, it_conds[4], cond = 14;
    SnxThumb t = {0};
    while (used < units) {
        unsigned hw1 = code[used * 2] | (unsigned)code[used * 2 + 1] << 8;
        unsigned hw2 = used + 1 < count ? code[used * 2 + 2] | (unsigned)code[used * 2 + 3] << 8 : 0;
        len = snx_thumb_decode_profile(profile, (uint16_t)hw1, (uint16_t)hw2, &t);
        if (!len || (len == TH_F32 && used + 1 >= count)) { /* as the block compiler steps over it */
            t.shape = TH_UNDEF;
            len = (hw1 >> 11) >= 0x1D && used + 1 < count ? 2 : 1;
        }
        cond = it_idx < it_len ? it_conds[it_idx++] : 14;
        if (used + len >= units) break;
        used += len;
        if (t.shape == TH_IT) { /* as llvm_thumb.inc reads it: 4 - ctz(mask) instructions */
            unsigned mask = t.imm & 15, first = (t.imm >> 4) & 15;
            for (it_len = 4; !(mask & 1); mask >>= 1) --it_len;
            mask = t.imm & 15;
            it_idx = 0;
            for (unsigned i = 0; i < it_len; ++i)
                it_conds[i] = i == 0 ? first : ((first & 0xE) | ((mask >> (4 - i)) & 1));
        }
    }
    pc += (uint32_t)used * 2; /* the last instruction */
    uint32_t next = pc + (uint32_t)len * 2, base = pc + 4;
    int always = cond == 14, ends = 0;
    switch (t.shape) {
        case TH_BRANCH: {
            unsigned hw1 = t.rn, hw2 = t.rm, s = (hw1 >> 10) & 1;
            unsigned i1 = !(((hw2 >> 13) & 1) ^ s), i2 = !(((hw2 >> 11) & 1) ^ s);
            switch (t.op) {
                case BR_B:
                    found(context, base + ((uint32_t)snx_sext32(t.imm, 11) << 1), 1);
                    ends = 1;
                    break;
                case BR_BCOND:
                    found(context, base + ((uint32_t)snx_sext32(t.imm, 8) << 1), 1);
                    always = 0;
                    break;
                case BR_CBZ:
                case BR_CBNZ:
                    found(context, base + t.imm, 1);
                    always = 0;
                    break;
                case BR_BL:
                case BR_BW:
                    found(context,
                          base + ((uint32_t)snx_sext32(
                                      (s << 23) | (i1 << 22) | (i2 << 21) | ((hw1 & 0x3FF) << 11) | (hw2 & 0x7FF), 24)
                                  << 1),
                          1);
                    ends = t.op == BR_BW;
                    break;
                case BR_BLX_IMM:
                    found(context,
                          (base & ~3u) +
                              ((uint32_t)snx_sext32((s << 23) | (i1 << 22) | (i2 << 21) | ((hw1 & 0x3FF) << 11) |
                                                        (((hw2 >> 1) & 0x3FF) << 1),
                                                    24)
                               << 1),
                          0);
                    break;
                case BR_BCOND_W:
                    found(context,
                          base +
                              ((uint32_t)snx_sext32((s << 19) | (((hw2 >> 11) & 1) << 18) | (((hw2 >> 13) & 1) << 17) |
                                                        ((hw1 & 0x3F) << 11) | (hw2 & 0x7FF),
                                                    20)
                               << 1),
                          1);
                    always = 0;
                    break;
                case BR_BX: ends = 1; break;
                case BR_BKPT: ends = 1; break;
                default: break; /* BLX register and SVC return */
            }
            break;
        }
        case TH_TABLE: ends = 1; break;
        case TH_HI: ends = t.rd == 15 && t.op != 1; break; /* MOV or ADD to the PC; not CMP */
        case TH_LOAD: ends = t.rd == 15; break;
        case TH_PUSHPOP: ends = t.op == 1 && t.ra; break; /* POP with the PC */
        case TH_A32: ends = a32_ends(profile, t.imm); break;
        default: break;
    }
    if (!(ends && always)) found(context, next, 1);
}
void snx_arm_successors(SnxArmProfile profile, uint32_t pc, const unsigned char* code, size_t units, size_t count,
                        int thumb, void (*found)(void* context, uint32_t address, int thumb), void* context) {
    if (!units) return;
    if (thumb)
        thumb_next(profile, pc, code, units, count, found, context);
    else
        a32_next(profile, pc + (uint32_t)(units - 1) * 4, snx_ld_le32(code + (units - 1) * 4), found, context);
}
