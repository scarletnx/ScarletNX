/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"

SnxArmExit snx_arm_gccjit_stub(void) { return SNX_ARM_BACKEND_UNSUPPORTED; }

SnxA32Kind snx_a32_decode(uint32_t w) {
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, m = w & 15;
    unsigned s = (w >> 8) & 15, op, load, wb;
    SnxArmVector vector;
    if (snx_arm_vector_decode(w, &vector)) return SNX_A32_VECTOR;
    if ((w >> 28) == 15) {
        if ((w & 0x0e000000) == 0x0a000000) return SNX_A32_BLX_IMM;
        if (w == 0xf57ff01f) return SNX_A32_CLREX;
        if ((w & 0xfffffdff) == 0xf1010000) return SNX_A32_SETEND;
        /* PLD, PLDW and PLI, immediate/literal or shifted register. These are
           hints, not loads: even an unmapped address must not data-abort. */
        op = w & 0xfd70f000;
        if (op == 0xf550f000 || op == 0xf510f000 || op == 0xf450f000) {
            if ((w & (1u << 25)) && ((w & 16) || m == 15)) return SNX_A32_UNDEFINED;
            if (op == 0xf510f000 && n == 15) return SNX_A32_UNDEFINED;
            return SNX_A32_HINT;
        }
        if ((w & 0xffffff00) == 0xf57ff000) {
            op = (w >> 4) & 15;
            /* ISB SY only. DMB/DSB: valid shareability/type options. */
            if ((op == 6 && (w & 15) == 15) || ((op == 4 || op == 5) && (w & 3))) return SNX_A32_BARRIER;
        }
        return SNX_A32_UNDEFINED;
    }
    if ((w & 0x0ffffff0) == 0x012fff10 || (w & 0x0ffffff0) == 0x012fff30)
        return m != 15 ? SNX_A32_BX : SNX_A32_UNDEFINED;
    if ((w & 0x0ff000f0) == 0x01200070) return (w >> 28) == 14 ? SNX_A32_BKPT : SNX_A32_UNDEFINED;
    /* MRC/MCR and MRRC/MCRR to the system coprocessors: which registers exist, and what they hold, is the backend's
       (SnxArmRt.coproc). Coprocessors 10 and 11 are VFP's, decoded above */
    if (((w & 0x0f000010) == 0x0e000010 || (w & 0x0fe00000) == 0x0c400000) && (s == 14 || s == 15))
        return SNX_A32_COPROC;
    if ((w & 0x0f000000) == 0x0f000000) return SNX_A32_SVC;
    if ((w & 0x0e000000) == 0x0a000000) return SNX_A32_BRANCH;
    /* NOP, YIELD, WFE, WFI, SEV, SEVL, and ARMv8 runs the unallocated hints (CSDB, ESB, DBG...) as NOP */
    if ((w & 0x0fffff00) == 0x0320f000) return SNX_A32_HINT;
    if ((w & 0x0fbf0fff) == 0x010f0000) return !(w & (1u << 22)) && d != 15 ? SNX_A32_MRS : SNX_A32_UNDEFINED;
    /* CPSR: at EL0 the f and s fields write NZCVQ and GE, x writes only E and c nothing. SPSR needs privilege */
    if ((w & 0x0ff0fff0) == 0x0120f000 || (w & 0x0ff0f000) == 0x0320f000) {
        if (n && ((w & (1u << 25)) || m != 15)) return SNX_A32_MSR;
        return SNX_A32_UNDEFINED;
    }
    if ((w & 0x0ff00000) == 0x03000000) return d != 15 ? SNX_A32_MOVW : SNX_A32_UNDEFINED;
    if ((w & 0x0ff00000) == 0x03400000) return d != 15 ? SNX_A32_MOVT : SNX_A32_UNDEFINED;
    if ((w & 0x0fff0ff0) == 0x016f0f10) return d != 15 && m != 15 ? SNX_A32_CLZ : SNX_A32_UNDEFINED;
    if ((w & 0x0fff0ff0) == 0x06bf0f30 || (w & 0x0fff0ff0) == 0x06bf0fb0 || (w & 0x0fff0ff0) == 0x06ff0fb0 ||
        (w & 0x0fff0ff0) == 0x06ff0f30)
        return d != 15 && m != 15 ? SNX_A32_REV : SNX_A32_UNDEFINED;
    if ((w & 0x0f8000f0) == 0x06800070) {
        op = (w >> 20) & 7;
        if (op != 1 && op != 5 && !(w & 0x300) && d != 15 && m != 15) return SNX_A32_EXTEND; /* op 0, 4: XTB16 */
        return SNX_A32_UNDEFINED;
    }
    if ((w & 0x0fe00070) == 0x07a00050 || (w & 0x0fe00070) == 0x07e00050) {
        if (d != 15 && m != 15 && ((w >> 7) & 31) + ((w >> 16) & 31) < 32) return SNX_A32_BITFIELD;
        return SNX_A32_UNDEFINED;
    }
    if ((w & 0x0fe00070) == 0x07c00010) {
        if (d != 15 && ((w >> 16) & 31) >= ((w >> 7) & 31)) return SNX_A32_BITFIELD;
        return SNX_A32_UNDEFINED;
    }
    /* ARMv6 media instructions. Their should-be-one or should-be-zero fields and PC operands are UNPREDICTABLE */
    if ((w & 0x0f800010) == 0x06000010) {
        op = (w >> 5) & 7;
        if (!(w & (3u << 20)) || op == 5 || op == 6 || s != 15 || n == 15 || d == 15 || m == 15)
            return SNX_A32_UNDEFINED;
        return SNX_A32_PARALLEL;
    }
    if ((w & 0x0ff00030) == 0x06800010) return n != 15 && d != 15 && m != 15 ? SNX_A32_PACK : SNX_A32_UNDEFINED;
    if ((w & 0x0ff00ff0) == 0x06800fb0) return n != 15 && d != 15 && m != 15 ? SNX_A32_SELECT : SNX_A32_UNDEFINED;
    if ((w & 0x0fa00030) == 0x06a00010 || (w & 0x0fb00ff0) == 0x06a00f30)
        return d != 15 && m != 15 ? SNX_A32_SATURATE : SNX_A32_UNDEFINED;
    if ((w & 0x0f800010) == 0x07000010 && ((w >> 20) & 7) != 1 && ((w >> 20) & 7) != 3) {
        /* Rd (RdHi) 19:16, Ra (RdLo) 15:12, Rm 11:8, Rn 3:0 */
        op = (w >> 5) & 7;
        if (n == 15 || s == 15 || m == 15) return SNX_A32_UNDEFINED;
        switch ((w >> 20) & 7) {
            case 0: return op < 4 ? SNX_A32_DUAL_MUL : SNX_A32_UNDEFINED;        /* SMLAD, SMUAD, SMLSD, SMUSD */
            case 4: return op < 4 && d != 15 && d != n ? SNX_A32_DUAL_MUL : SNX_A32_UNDEFINED; /* SMLALD, SMLSLD */
            case 5: /* SMMLA, SMMUL; SMMLS */
                return op < 2 || (op >= 6 && d != 15) ? SNX_A32_DUAL_MUL : SNX_A32_UNDEFINED;
            default: return SNX_A32_UNDEFINED;
        }
    }
    if ((w & 0x0ff000f0) == 0x07800010) return n != 15 && s != 15 && m != 15 ? SNX_A32_ABS_DIFF : SNX_A32_UNDEFINED;
    /* QADD, QSUB, QDADD, QDSUB: Rn 19:16, Rd 15:12, Rm 3:0 */
    if ((w & 0x0f9000f0) == 0x01000050) return !s && n != 15 && d != 15 && m != 15 ? SNX_A32_QARITH : SNX_A32_UNDEFINED;
    /* CRC32{C}: unconditional only, sizes B, H and W */
    if ((w & 0x0f9000f0) == 0x01000040) {
        if ((w >> 28) != 14 || ((w >> 21) & 3) == 3 || (w & 0xd00) || n == 15 || d == 15 || m == 15)
            return SNX_A32_UNDEFINED;
        return SNX_A32_CRC32;
    }
    /* the halfword multiplies: Rd (RdHi) 19:16, Ra (RdLo) 15:12, Rm 11:8, Rn 3:0 */
    if ((w & 0x0f900090) == 0x01000080) {
        op = (w >> 21) & 3;
        if (n == 15 || s == 15 || m == 15) return SNX_A32_UNDEFINED;
        if (op == 3 || (op == 1 && (w & 0x20))) return !d ? SNX_A32_HALF_MUL : SNX_A32_UNDEFINED; /* SMUL, SMULW */
        return d != 15 && (op != 2 || d != n) ? SNX_A32_HALF_MUL : SNX_A32_UNDEFINED;
    }
    /* UMAAL: RdHi 19:16, RdLo 15:12 */
    if ((w & 0x0ff000f0) == 0x00400090)
        return n != 15 && d != 15 && m != 15 && s != 15 && n != d ? SNX_A32_MULL : SNX_A32_UNDEFINED;
    if ((w & 0x0fd0f0f0) == 0x0710f010) return n != 15 && m != 15 && s != 15 ? SNX_A32_DIV : SNX_A32_UNDEFINED;
    if ((w & 0x0f8000f0) == 0x00800090)
        return n != 15 && d != 15 && m != 15 && s != 15 && n != d ? SNX_A32_MULL : SNX_A32_UNDEFINED;
    if ((w & 0x0fc000f0) == 0x00000090)
        return n != 15 && m != 15 && s != 15 && ((w & (1u << 21)) ? d != 15 : d == 0) ? SNX_A32_MUL : SNX_A32_UNDEFINED;
    if ((w & 0x0ff000f0) == 0x00600090)
        return n != 15 && d != 15 && m != 15 && s != 15 ? SNX_A32_MLS : SNX_A32_UNDEFINED;
    if ((w & 0x0f8000f0) == 0x01800090) {
        op = (w >> 20) & 7;
        load = op & 1;
        if (s == 12) { /* ARMv8 LDA/STL, without an exclusive monitor */
            if (n == 15 || (op >> 1) == 1 || (load ? d == 15 || m != 15 : d != 15 || m == 15)) return SNX_A32_UNDEFINED;
            return SNX_A32_ACQUIRE_RELEASE;
        }
        if (n == 15 || d == 15 || (s != 15 && s != 14) || (load && m != 15)) return SNX_A32_UNDEFINED;
        if (!load && (m == 15 || d == n || d == m)) return SNX_A32_UNDEFINED;
        if ((op >> 1) == 1) { /* doubleword register pair */
            unsigned t = load ? d : m;
            if ((t & 1) || t >= 14 || (!load && d == t + 1)) return SNX_A32_UNDEFINED;
        }
        return SNX_A32_EXCLUSIVE;
    }
    if ((w & 0x0e000090) == 0x00000090 && (w & 0x60)) {
        op = (w >> 5) & 3;
        load = (w >> 20) & 1;
        wb = !(w & (1u << 24)) || (w & (1u << 21));
        if (!(w & (1u << 22)) && (s || m == 15)) return SNX_A32_UNDEFINED;
        /* a PC base is UNPREDICTABLE only with writeback, as for the word and byte forms */
        if ((wb && (n == 15 || n == d)) || d == 15) return SNX_A32_UNDEFINED;
        if (!load && op >= 2) { /* LDRD / STRD */
            if ((d & 1) || d >= 14 || (wb && n == d + 1) || (!(w & (1u << 24)) && (w & (1u << 21))))
                return SNX_A32_UNDEFINED;
        }
        return SNX_A32_EXTRA;
    }
    if ((w & 0x0c000000) == 0x04000000) {
        if ((w & (1u << 25)) && ((w & 0x10) || m == 15)) return SNX_A32_UNDEFINED;
        load = (w >> 20) & 1;
        wb = !(w & (1u << 24)) || (w & (1u << 21));
        /* A PC base is UNPREDICTABLE only with writeback, for stores as for loads: STR r1, [pc, r0] reads PC+8 */
        if ((wb && (n == 15 || n == d)) || ((w & (1u << 22)) && d == 15)) return SNX_A32_UNDEFINED;
        return SNX_A32_SINGLE;
    }
    if ((w & 0x0e000000) == 0x08000000) {
        /* Base-in-list with writeback is UNPREDICTABLE in the original ARMv6
           text, but ARM11 MPCore executes it (and guest code relies on it):
           STM stores the old base, LDM's loaded base wins over writeback.
           Both semantics are checked against Unicorn's ARM11 MPCore model. */
        if (!((uint16_t)w) || n == 15 || (w & (1u << 22))) return SNX_A32_UNDEFINED;
        return SNX_A32_MULTIPLE;
    }
    if ((w & 0x0c000000) == 0) {
        op = (w >> 21) & 15;
        if (!(w & (1u << 25)) && (w & 0x10) && ((w & 0x80) || s == 15 || m == 15 || n == 15)) return SNX_A32_UNDEFINED;
        if (op >= 8 && op <= 11) {
            if (!(w & (1u << 20)) || d) return SNX_A32_UNDEFINED;
        } else if (d == 15 && (w & (1u << 20)))
            return SNX_A32_UNDEFINED; /* exception return */
        if ((op == 13 || op == 15) && n) return SNX_A32_UNDEFINED;
        return SNX_A32_DATA;
    }
    return SNX_A32_UNDEFINED;
}
