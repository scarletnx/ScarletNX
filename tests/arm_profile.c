/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/arm/arm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const uint32_t forbidden[] = {0xe3013234, 0xe3453678, 0xe0634291, 0xe713f211, 0xe733f211,
                                         0xe6ff3f32, 0xe7e73051, 0xf57ff05f, 0xf57ff04f, 0xf57ff06f,
                                         0xe1910e9f, 0xe1810e92, 0xe1910c9f, 0xe181fc92, 0xe1013042,
                                         0xf4d1f000, 0xf591f000, 0xf2010802, 0xeef00a00, 0xeea00a00};
    for (size_t i = 0; i < sizeof forbidden / sizeof *forbidden; ++i) {
        assert(snx_a32_decode(forbidden[i]) != SNX_A32_UNDEFINED);
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, forbidden[i]) == SNX_A32_UNDEFINED);
    }
    static const uint32_t valid[] = {0xe1a00000, 0xe6bf0f31, 0xe6af0071, 0xe16f0f11, 0xe1910f9f,
                                     0xe1810f92, 0xf57ff01f, 0xe320f004, 0xf5d0f000, 0xee1d0f70,
                                     0xee070fba, 0xee070f9a, 0xee070f95, 0xee300a01, 0xe320f005, 0xe320f014};
    for (size_t i = 0; i < sizeof valid / sizeof *valid; ++i)
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, valid[i]) != SNX_A32_UNDEFINED);
    for (size_t i = 10; i < 13; ++i) assert(snx_a32_decode(valid[i]) == SNX_A32_COPROC); /* CP15 DMB, DSB, ISB */
    const uint32_t packed[] = {0xe6510f92, 0xe6810fb2};
    for (unsigned i = 0; i < 2; ++i) {
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, packed[i]) == (i ? SNX_A32_SEL : SNX_A32_UADD8));
        const unsigned shifts[] = {0, 12, 16};
        for (unsigned j = 0; j < 3; ++j)
            assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, packed[i] | (15u << shifts[j])) == SNX_A32_UNDEFINED);
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, packed[i] | 0xf0000000u) == SNX_A32_UNDEFINED);
    }
    /* ARMv5TE halfword multiplies (llvm-mc -mcpu=mpcore): smulbb r3,r1,r3; smultt r3,r1,r2; smlabb r3,r1,r2,r9;
       smlawt r3,r1,r2,r9; smulwb r3,r1,r2; smlalbt r4,r5,r1,r2. The A57 contract keeps its own decoder */
    const uint32_t mulh[] = {0xe1630381, 0xe16302e1, 0xe1039281, 0xe12392c1, 0xe12302a1, 0xe14542c1};
    for (unsigned i = 0; i < sizeof mulh / sizeof *mulh; ++i) {
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, mulh[i]) == SNX_A32_MULH);
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, mulh[i] | 15u) == SNX_A32_UNDEFINED); /* Rm = PC */
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, mulh[i] | 0xf0000000u) == SNX_A32_UNDEFINED);
    }
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, 0xe1631381) == SNX_A32_UNDEFINED); /* SMULBB, Rn not 0 */
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM11, 0xe14442c1) == SNX_A32_UNDEFINED); /* SMLAL RdHi == RdLo */
    /* A57: a CP15 barrier is a coprocessor access, which the backend allows or refuses (SnxArmRt.coproc) */
    assert(snx_a32_decode(0xee070fba) == SNX_A32_COPROC);
    assert(snx_a32_decode_profile((SnxArmProfile)99, 0xe1a00000) == SNX_A32_UNDEFINED);
    /* ARM7TDMI: A57's decoder, but for an MSR that writes the control or extension byte, a mode switch the AGB board
       runs (msr cpsr_c, #0xd2; msr cpsr_fc, r0), the flags alone staying an MSR (msr cpsr_f, r0); and the NV condition,
       which never holds on ARMv4: those words run as nothing (later cores' CPS, ARMv5's BLX) */
    assert(snx_a32_decode(0xe321f0d2) == SNX_A32_MSR && snx_a32_decode(0xe129f000) == SNX_A32_MSR);
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xe321f0d2) == SNX_A32_UNDEFINED);
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xe129f000) == SNX_A32_UNDEFINED);
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xe128f000) == SNX_A32_MSR);
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xf1000000) == SNX_A32_HINT);
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xfa000003) == SNX_A32_HINT);
    uint32_t state = 1234;
    for (unsigned i = 0; i < 100000; ++i) {
        state = state * 1664525u + 1013904223u;
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_A57, state) == snx_a32_decode(state));
        if ((state >> 28) == 15)
            assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, state) == SNX_A32_HINT);
        else if (snx_a32_decode(state) != SNX_A32_MSR || !((state >> 16) & 3))
            assert(snx_a32_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, state) == snx_a32_decode(state));
    }
    for (unsigned i = 0; i <= 65535; ++i) {
        SnxThumb a = {0}, b = {0};
        unsigned x = snx_thumb_decode((uint16_t)i, 0xf800, &a);
        unsigned y = snx_thumb_decode_profile(SNX_ARM_PROFILE_A57, (uint16_t)i, 0xf800, &b);
        assert(x == y && !memcmp(&a, &b, sizeof a));
        y = snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, (uint16_t)i, 0xf800, &b);
        assert(y <= TH_F16); /* never accidentally decode Thumb-2 */
        if ((i & 0xff00) == 0xbf00 || ((i & 0xf800) == 0xe800 && (i & 1))) assert(!y);
        /* ARMv4T, the ARM7TDMI: nothing ARMv5 and ARMv6 added (BLX, its suffix and by register, and the 1011 group
           past ADD/SUB SP, PUSH and POP); BL's halves apart and the PC-relative load as ARM11's; the rest A57's
           16-bit Thumb, the forms ARMv6 makes UNPREDICTABLE included, which the ARM7TDMI runs */
        {
            SnxThumb c = {0};
            int later = ((i & 0xf000) == 0xb000 && (i & 0xff00) != 0xb000 && (i & 0xfe00) != 0xb400 &&
                         (i & 0xfe00) != 0xbc00) ||
                        (i & 0xf800) == 0xe800 || (i & 0xff80) == 0x4780;
            unsigned z = snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, (uint16_t)i, 0xf800, &c);
            if (later)
                assert(!z);
            else if (i >= 0xf000)
                assert(z == 1 && c.shape == TH_LEGACY_BL);
            else if ((i & 0xf800) == 0x4800)
                assert(z == 1 && c.shape == TH_LOAD && c.rn == 15);
            else if ((i & 0xff80) == 0x4700) /* BX takes no notice of bits 2 to 0 */
                assert(z == 1 && c.shape == TH_BRANCH && c.op == BR_BX && c.rm == ((i >> 3) & 15));
            else
                assert(z == x && (!z || !memcmp(&a, &c, sizeof a))); /* a refusal leaves A57's partly filled */
        }
    }
    SnxThumb t;
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xf400, 0xd000, &t) == 1 && t.shape == TH_LEGACY_BL);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xf800, 0, &t) == 1 && t.op == 1);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xe800, 0, &t) == 1 && t.op == 2);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM11, 0xc903, 0, &t) == 1 && !(t.flags & TH_WB));
    /* the ARM7TDMI: BL's halves apart; BLX, BKPT, CPS, IT and the hints undefined; BX and PUSH as ever */
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xf400, 0xd000, &t) == 1 && t.shape == TH_LEGACY_BL &&
           !t.op);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xf800, 0, &t) == 1 && t.op == 1);
    assert(!snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xe800, 0, &t));
    assert(!snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0x4788, 0, &t));     /* BLX r1 */
    assert(!snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xbe00, 0, &t));     /* BKPT */
    assert(!snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xb662, 0, &t));     /* CPSIE i */
    assert(!snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xbf08, 0, &t));     /* IT EQ */
    assert(!snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xbf00, 0, &t));     /* NOP */
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0x4708, 0, &t) == 1); /* BX r1 */
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0x4775, 0, &t) == 1 && t.op == BR_BX && t.rm == 14);
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_ARM7TDMI, 0xb510, 0, &t) == 1 && t.shape == TH_PUSHPOP);
    puts("ARM11 and ARM7TDMI profiles and unchanged default decoder witnesses passed");
    /* The Cortex-A9's stores with writeback into the register they store (llvm-mc -mcpu=cortex-a9, which warns that
       they are UNPREDICTABLE): str r0, [r0, #28]!; strb r1, [r1], #4; strh r2, [r2, #-2]!; str r3, [r3, r4]!. A57
       rejects them; a load into its base with writeback, and STRD, stay rejected */
    const uint32_t stores[] = {0xe5a0001c, 0xe4c11004, 0xe16220b2, 0xe7a33004};
    for (unsigned i = 0; i < sizeof stores / sizeof *stores; ++i) {
        SnxA32Kind kind = i == 2 ? SNX_A32_EXTRA : SNX_A32_SINGLE;
        assert(snx_a32_decode(stores[i]) == SNX_A32_UNDEFINED);
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_A9, stores[i]) == kind);
        assert(snx_a32_decode_profile(SNX_ARM_PROFILE_A9, stores[i] | (1u << 20)) == SNX_A32_UNDEFINED); /* loads */
    }
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_A9, 0xe1e000f8) == SNX_A32_UNDEFINED); /* strd r0, r1, [r0, #8]! */
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_A9, 0xe5af001c) == SNX_A32_UNDEFINED); /* str r0, [pc, #28]! */
    assert(snx_a32_decode_profile(SNX_ARM_PROFILE_A9, 0xe1a00000) == SNX_A32_DATA);
    /* Thumb: str.w r0, [r0, #28]! (f840 0f1c, a Vita game's), strb r1, [r1], #1 (f801 1b01), strh r2, [r2, #-2]!
       (f822 2d02); ldr.w r0, [r0, #28]! (f850 0f1c) stays rejected, and str.w r0, [r1, #28]! decodes everywhere */
    const uint16_t thumb[][2] = {{0xf840, 0x0f1c}, {0xf801, 0x1b01}, {0xf822, 0x2d02}};
    for (unsigned i = 0; i < sizeof thumb / sizeof *thumb; ++i) {
        SnxThumb t;
        assert(!snx_thumb_decode(thumb[i][0], thumb[i][1], &t));
        assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_A9, thumb[i][0], thumb[i][1], &t) == TH_F32);
        assert(t.shape == TH_STORE && (t.flags & TH_WB) && t.rn == i && t.rd == i);
        assert(!snx_thumb_decode_profile(SNX_ARM_PROFILE_A9, thumb[i][0] | 0x10, thumb[i][1], &t)); /* loads */
    }
    SnxThumb other;
    assert(snx_thumb_decode_profile(SNX_ARM_PROFILE_A9, 0xf841, 0x0f1c, &other) == TH_F32 && other.rn == 1);
    return 0;
}
