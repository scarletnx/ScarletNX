/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"

/* VFPv2 recognition. Emission stops a short vector or an enabled exception
   before arithmetic (SNX_ARM_BACKEND_UNSUPPORTED). Never silently execute NEON,
   D16..D31, VFPv3 immediates or fused operations. */
static int vfp2(uint32_t w) {
    SnxArmVector v;
    if ((w >> 28) == 15 || !snx_arm_vector_decode(w, &v) || (v.flags & AV_SIMD)) return 0;
    switch (v.op) {
        case AV_CORE: return (w & 0x0fe00f7f) == 0x0e000a10 && v.d < 128;
        case AV_CORE_PAIR: return v.d + 8 <= 128;
        case AV_STATUS: return 1;
        case AV_MEM: return v.d + v.bytes <= 128;
        case AV_FADD:
        case AV_FSUB:
        case AV_FMUL:
        case AV_FDIV:
        case AV_FMLA:
        case AV_FMLS:
        case AV_FNMLA:
        case AV_FNMLS:
        case AV_FNMUL: return v.d + v.bytes <= 128 && v.n + v.bytes <= 128 && v.m + v.bytes <= 128;
        case AV_MOV:
        case AV_FABS:
        case AV_FNEG:
        case AV_FSQRT: return v.d + v.bytes <= 128 && v.m + v.bytes <= 128;
        case AV_FCMP:
        case AV_FCMPE: return v.n + v.bytes <= 128 && (v.aux || v.m + v.bytes <= 128);
        case AV_FCVT:
            return (v.width == 32 || v.width == 64) && (v.aux == 32 || v.aux == 64) && v.d + v.bytes <= 128 &&
                   v.m + v.width / 8 <= 128;
        case AV_ITOF: return v.aux == 32 && !(v.imm >> 8) && v.d + v.bytes <= 128 && v.m + 4 <= 128;
        case AV_FTOI: return v.aux == 32 && !(v.imm >> 8) && v.d + v.bytes <= 128 && v.m + v.width / 8 <= 128;
        default: return 0;
    }
}
/* The Cortex-A9's: a store with writeback into the register it stores (STR r0, [r0, #28]!) is UNPREDICTABLE in
   ARMv7, and A57's decoders reject it. The A9 stores the register as it was, then writes the base back, which a Vita
   game's code relies on (one stores a structure's address in its field and passes the field to a callback): decoded
   as the same instruction with another base, which the emitters run so (they read the stored register before they
   write the base back). A load's writeback into the register it loads stays rejected, as do the doubleword stores */
static unsigned other_register(unsigned a, unsigned b) {
    unsigned r = 0;
    while (r == a || r == b) ++r;
    return r;
}
static SnxA32Kind a9_a32(uint32_t w) {
    SnxA32Kind kind = snx_a32_decode(w);
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15;
    int store_writeback = !(w & (1u << 20)) && (!(w & (1u << 24)) || (w & (1u << 21)));
    if (kind != SNX_A32_UNDEFINED || n != d || n == 15 || !store_writeback || (w >> 28) == 15) return kind;
    SnxA32Kind twin = snx_a32_decode((w & ~(15u << 16)) | other_register(d, w & 15) << 16);
    if (twin == SNX_A32_SINGLE || (twin == SNX_A32_EXTRA && ((w >> 5) & 3) == 1)) return twin; /* STR, STRB, STRH */
    return kind;
}
static unsigned a9_thumb(uint16_t h, uint16_t h2, SnxThumb* o) {
    unsigned len = snx_thumb_decode(h, h2, o), n = h & 15, t = h2 >> 12;
    /* F8xx/F9xx stores (hw1 bit 4 clear) of the imm8 form (hw2 bit 11) with writeback (hw2 bit 8) */
    if (len || (h & 0xfe90) != 0xf800 || (h2 & 0x900) != 0x900 || n != t || n == 15) return len;
    len = snx_thumb_decode((uint16_t)((h & ~15u) | other_register(t, 15)), h2, o);
    if (len != TH_F32 || o->shape != TH_STORE || !(o->flags & TH_WB)) {
        *o = (SnxThumb){0};
        return 0;
    }
    o->rn = n;
    return len;
}
SnxA32Kind snx_a32_decode_profile(SnxArmProfile profile, uint32_t w) {
    if (profile == SNX_ARM_PROFILE_A57) return snx_a32_decode(w);
    if (profile == SNX_ARM_PROFILE_A9) return a9_a32(w);
    if (profile == SNX_ARM_PROFILE_ARM7TDMI) {
        /* An MSR that writes the control or extension byte switches modes on the AGB board, which banks the
           registers: the board runs it. A57's EL0 MSR would drop the mode bits */
        SnxA32Kind kind = snx_a32_decode(w);
        return kind == SNX_A32_MSR && ((w >> 16) & 3) ? SNX_A32_UNDEFINED : kind;
    }
    if (profile != SNX_ARM_PROFILE_ARM11) return SNX_A32_UNDEFINED;
    /* ARM11 CP15 barrier operations, not ARMv7's unconditional encodings.
       Only these user-accessible maintenance operations are admitted. */
    uint32_t cp = w & 0x0fff0fff;
    if ((w >> 28) != 15 && ((w >> 12) & 15) != 15 && (cp == 0x0e070fba || cp == 0x0e070f9a || cp == 0x0e070f95))
        return SNX_A32_BARRIER;
    /* ARMv6 packed-byte operations used by libc. Keep the default A57
       decoder/emitter contract unchanged; admit only these exact ARM11 forms. */
    uint32_t packed = w & 0x0ff00ff0;
    if ((packed == 0x06500f90 || packed == 0x06800fb0) && (w >> 28) != 15) {
        if (((w >> 16) & 15) == 15 || ((w >> 12) & 15) == 15 || (w & 15) == 15) return SNX_A32_UNDEFINED;
        return packed == 0x06500f90 ? SNX_A32_UADD8 : SNX_A32_SEL;
    }
    /* ARMv5TE signed halfword multiplies, which ARM11 MPCore has: SMLAxy (op 0), SMLAWy/SMULWy (op 1, bit 5 set
       for SMULWy), SMLALxy (op 2), SMULxy (op 3). PC operands, an accumulator field that should be zero but is not
       (SMULxy, SMULWy) and SMLALxy with RdHi == RdLo are UNPREDICTABLE and rejected */
    if ((w & 0x0f900090) == 0x01000080 && (w >> 28) != 15) {
        unsigned op = (w >> 21) & 3, d = (w >> 16) & 15, n = (w >> 12) & 15;
        int accumulates = op == 0 || op == 2 || (op == 1 && !(w & 0x20));
        if (d == 15 || ((w >> 8) & 15) == 15 || (w & 15) == 15 || (accumulates ? n == 15 : n != 0) ||
            (op == 2 && d == n))
            return SNX_A32_UNDEFINED;
        return SNX_A32_MULH;
    }
    SnxA32Kind kind = snx_a32_decode(w);
    switch (kind) {
        case SNX_A32_MOVW:
        case SNX_A32_MOVT:
        case SNX_A32_MLS:
        case SNX_A32_DIV:
        case SNX_A32_BITFIELD:
        case SNX_A32_ACQUIRE_RELEASE:
        case SNX_A32_BARRIER:
        case SNX_A32_CRC32: return SNX_A32_UNDEFINED;
        case SNX_A32_REV: return (w & 0x0fff0ff0) == 0x06ff0f30 ? SNX_A32_UNDEFINED : kind; /* RBIT */
        case SNX_A32_EXCLUSIVE: return ((w >> 8) & 15) == 15 ? kind : SNX_A32_UNDEFINED; /* no ARMv8 acquire/release */
        case SNX_A32_HINT:
            if ((w >> 28) == 15) return (w & 0xfd70f000) == 0xf550f000 ? kind : SNX_A32_UNDEFINED; /* PLD */
            return kind; /* NOP, YIELD, WFE, WFI, SEV; the unallocated hints (SEVL among them) are NOP */
        case SNX_A32_VECTOR: return vfp2(w) ? kind : SNX_A32_UNDEFINED;
        default: return kind;
    }
}
unsigned snx_thumb_decode_profile(SnxArmProfile profile, uint16_t h, uint16_t h2, SnxThumb* o) {
    if (profile == SNX_ARM_PROFILE_A57) return snx_thumb_decode(h, h2, o);
    if (profile == SNX_ARM_PROFILE_A9) return a9_thumb(h, h2, o);
    *o = (SnxThumb){0};
    if (profile != SNX_ARM_PROFILE_ARM11 && profile != SNX_ARM_PROFILE_ARM7TDMI) return 0;
    int v6 = profile == SNX_ARM_PROFILE_ARM11; /* the ARM7TDMI's ARMv4T has none of ARMv5's and ARMv6's additions */
    /* ARMv6K's and ARMv4T's Thumb BL is two separately executable 16-bit instructions. Its
       signed high offset is NOT Thumb-2's S/J1/J2 encoding. LR is observable
       after the prefix, and execution may enter directly at a suffix (a GBA game's call through LR). BLX's suffix is
       ARMv5's */
    unsigned top = h & 0xf800;
    if (top == 0xf000 || top == 0xf800 || (v6 && top == 0xe800 && !(h & 1))) {
        o->shape = TH_LEGACY_BL;
        o->len = TH_F16;
        o->op = top == 0xf000 ? 0 : top == 0xf800 ? 1 : 2;
        o->imm = h & 0x7ff;
        return TH_F16;
    }
    /* ARMv4T's 1011 group is ADD/SUB SP, PUSH and POP alone: CBZ, the extends, CPS, REV, BKPT, IT and the hints came
       later */
    if (!v6 && (h & 0xf000) == 0xb000 && (h & 0xff00) != 0xb000 && (h & 0xfe00) != 0xb400 && (h & 0xfe00) != 0xbc00)
        goto invalid;
    if (top == 0x4800) {
        o->shape = TH_LOAD;
        o->len = TH_F16;
        o->rd = (h >> 8) & 7;
        o->rn = 15;
        o->imm = (h & 255) * 4;
        o->flags = TH_PCB | TH_PRE | TH_UP;
        return TH_F16;
    }
    if ((h & 0xff00) == 0xba00 && ((h >> 6) & 3) != 2) {
        o->shape = TH_REV;
        o->len = TH_F16;
        o->op = (h >> 6) & 3;
        o->rd = h & 7;
        o->rm = (h >> 3) & 7;
        return TH_F16;
    }
    if ((h & 0xff00) == 0xbe00) {
        o->shape = TH_BRANCH;
        o->len = TH_F16;
        o->op = BR_BKPT;
        o->imm = h & 255;
        return TH_F16;
    }
    if (top >= 0xe800 || !snx_thumb_decode(h, h2, o)) goto invalid;
    if (o->shape == TH_IT || o->shape == TH_HINT ||
        (o->shape == TH_BRANCH &&
         (o->op == BR_CBZ || o->op == BR_CBNZ || (o->op == BR_BLX_REG && (!v6 || o->rm == 15))))) /* BLX is v5's */
        goto invalid;
    /* the forms ARMv6 makes UNPREDICTABLE (a base in an LDM or STM's list, an empty list, CMP of two low registers)
       run on the ARM7TDMI as it runs them, which its profile's emitter follows */
    if (!v6) return TH_F16;
    if (o->shape == TH_HI && o->op == 1 && (o->rd == 15 || o->rm == 15 || (o->rd < 8 && o->rm < 8))) goto invalid;
    if (o->shape == TH_PUSHPOP && !o->imm && !o->ra) goto invalid;
    if (o->shape == TH_LDMSTM) {
        if (!o->imm) goto invalid;
        if (o->imm & (1u << o->rn)) {
            if (o->op == 1)
                o->flags &= ~TH_WB; /* LDM base in list: load it, no writeback */
            else if (o->imm & ((1u << o->rn) - 1))
                goto invalid;
        }
    }
    return TH_F16;
invalid:
    *o = (SnxThumb){0};
    return 0;
}
