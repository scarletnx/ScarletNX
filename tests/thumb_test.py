#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Thumb16/Thumb32 blocks of a native back end vs ARMv8 AArch32 (Unicorn 2.1.4).

A Thumb block for each instruction or sequence, its halfwords beside its text, made by this host's back end or (run
under Unicorn) another machine's, with all its host registers and with three, so that values spill, or by LLVM at
each --opt level. Each is run from
random state, every GPR, CPSR, D register, FPSCR and guest memory compared. Exact raw-bit comparisons only. No game
assets.
"""

import argparse
import ctypes as C
import random

from arm_blocks import CPU, PC, SIZE, Blocks, add_variants, thumb, variants


def catalogue():
    single = [
        # shift by immediate
        ('lsl r0, r1, #1', 0xEA4F, 0x0041),
        ('lsl r3, r4, #0', 0xEA4F, 0x0304),
        ('lsr r0, r1, #32', 0xEA4F, 0x0011),
        ('asr r2, r5, #31', 0xEA4F, 0x72E5),
        # add/sub register and imm3
        ('add r0, r1, r2', 0xEB01, 0x0002),
        ('sub r3, r4, r5', 0xEBA4, 0x0305),
        ('add r0, r1, #7', 0xF101, 0x0007),
        ('sub r2, r3, #0', 0xF1A3, 0x0200),
        # mov/cmp/add/sub imm8
        ('mov r0, #0', 0xF04F, 0x0000),
        ('mov r7, #255', 0xF04F, 0x07FF),
        ('cmp r1, #128', 0x2980),
        ('add r2, #10', 0xF102, 0x020A),
        ('sub r3, #10', 0xF1A3, 0x030A),
        # data-processing register
        ('and r0, r1', 0xEA00, 0x0001),
        ('eor r0, r1', 0xEA80, 0x0001),
        ('lsl r0, r1', 0xFA00, 0xF001),
        ('lsr r0, r1', 0xFA20, 0xF001),
        ('asr r0, r1', 0xFA40, 0xF001),
        ('adc r0, r1', 0xEB40, 0x0001),
        ('sbc r0, r1', 0xEB60, 0x0001),
        ('ror r0, r1', 0xFA60, 0xF001),
        ('tst r0, r1', 0x4208),
        ('neg r0, r1', 0xF1C1, 0x0000),
        ('cmp r0, r1', 0x4288),
        ('cmn r0, r1', 0x42C8),
        ('orr r0, r1', 0xEA40, 0x0001),
        ('mul r0, r1', 0xFB01, 0xF000),
        ('bic r0, r1', 0xEA20, 0x0001),
        ('mvn r0, r1', 0xEA6F, 0x0001),
        # their 16-bit forms, which set the flags, as the plain mnemonics above are the wide ones
        ('lsls r0, r1, #1', 0x0048),
        ('movs r3, r4', 0x0023),
        ('lsrs r0, r1, #32', 0x0808),
        ('asrs r2, r5, #31', 0x17EA),
        ('adds r0, r1, r2', 0x1888),
        ('subs r3, r4, r5', 0x1B63),
        ('adds r0, r1, #7', 0x1DC8),
        ('subs r2, r3, #0', 0x1E1A),
        ('movs r0, #0', 0x2000),
        ('movs r7, #255', 0x27FF),
        ('adds r2, #10', 0x320A),
        ('subs r3, #10', 0x3B0A),
        ('ands r0, r1', 0x4008),
        ('eors r0, r1', 0x4048),
        ('lsls r0, r1', 0x4088),
        ('lsrs r0, r1', 0x40C8),
        ('asrs r0, r1', 0x4108),
        ('adcs r0, r1', 0x4148),
        ('sbcs r0, r1', 0x4188),
        ('rors r0, r1', 0x41C8),
        ('rsbs r0, r1, #0', 0x4248),
        ('orrs r0, r1', 0x4308),
        ('muls r0, r1, r0', 0x4348),
        ('bics r0, r1', 0x4388),
        ('mvns r0, r1', 0x43C8),
        # high registers / branch exchange
        ('add r0, r8', 0x4440),
        ('add r8, r1', 0x4488),
        ('add r9, r10', 0x44D1),
        ('cmp r1, r9', 0x4549),
        ('mov r8, r2', 0x4690),
        ('mov r0, r11', 0x4658),
        ('bx r3', 0x4718),
        ('blx r4', 0x47A0),
        ('bx lr', 0x4770),
        ('mov pc, r2', 0x4697),
        # load/store register offset
        ('str r0, [r1, r2]', 0x5088),
        ('strh r0, [r1, r2]', 0x5288),
        ('strb r0, [r1, r2]', 0x5488),
        ('ldrsb r0, [r1, r2]', 0x5688),
        ('ldr r0, [r1, r2]', 0x5888),
        ('ldrh r0, [r1, r2]', 0x5A88),
        ('ldrb r0, [r1, r2]', 0x5C88),
        ('ldrsh r0, [r1, r2]', 0x5E88),
        # load/store immediate
        ('str r0, [r1, #0]', 0x6008),
        ('str r0, [r1, #124]', 0x67C8),
        ('ldr r0, [r1, #124]', 0x6FC8),
        ('ldr r0, [r1, #0]', 0x6808),
        ('strb r0, [r1, #31]', 0x77C8),
        ('ldrb r0, [r1, #31]', 0x7FC8),
        ('strh r0, [r1, #62]', 0x87C8),
        ('ldrh r0, [r1, #62]', 0x8FC8),
        # sp-relative
        ('str r0, [sp, #252]', 0x903F),
        ('ldr r0, [sp, #252]', 0x983F),
        ('ldr r7, [sp, #0]', 0x9F00),
        # load address
        ('add r0, pc, #1020', 0xF20F, 0x30FC),
        ('add r0, sp, #1020', 0xA8FF),
        ('add r3, sp, #0', 0xAB00),
        # add/sub sp
        ('add sp, #128', 0xB020),
        ('sub sp, #128', 0xB0A0),
        ('add sp, #0', 0xB000),
        # extend
        ('sxtb r0, r1', 0xB248),
        ('sxth r0, r1', 0xB208),
        ('uxtb r0, r1', 0xB2C8),
        ('uxth r0, r1', 0xB288),
        # push/pop
        ('push {r0, r1}', 0xB403),
        ('push {lr}', 0xB500),
        ('push {r0-r7, lr}', 0xB5FF),
        ('pop {r0, r1}', 0xBC03),
        ('pop {r0-r7, pc}', 0xBDFF),
        # ldm/stm
        ('stmia r0!, {r1, r2}', 0xC006),
        ('ldmia r0!, {r1, r2}', 0xC806),
        ('ldmia r0!, {r1-r7}', 0xC8FE),
        # branches and svc
        ('beq #254', 0xD07F),
        ('bne #0', 0xD100),
        ('b #2046', 0xE3FF),
        ('b #-2048', 0xE400),
        ('svc #255', 0xDFFF),
        ('cbz r0, #4', 0xB110),
        ('cbnz r1, #64', 0xBB01),
        # Thumb32 data-processing / bitfield / wide immediate
        ('movw r0, #0', 0xF240, 0x0000),
        ('movw r3, #65535', 0xF64F, 0x73FF),
        ('movt r0, #0', 0xF2C0, 0x0000),
        ('movt r3, #65535', 0xF6CF, 0x73FF),
        ('addw r0, r1, #0', 0xF201, 0x0000),
        ('addw r3, r4, #4095', 0xF604, 0x73FF),
        ('subw r0, r1, #4095', 0xF6A1, 0x70FF),
        ('subw r3, r4, #0', 0xF2A4, 0x0300),
        ('sbfx r0, r1, #0, #1', 0xF341, 0x0000),
        ('sbfx r3, r4, #31, #1', 0xF344, 0x73C0),
        ('ubfx r0, r1, #4, #8', 0xF3C1, 0x1007),
        ('ubfx r3, r4, #0, #32', 0xF3C4, 0x031F),
        ('bfi r0, r1, #4, #8', 0xF361, 0x100B),
        ('bfc r0, #4, #8', 0xF36F, 0x100B),
        ('bfc r3, #0, #32', 0xF36F, 0x031F),
        ('sxtb.w r0, r1', 0xFA4F, 0xF081),
        ('uxth.w r3, r4', 0xFA1F, 0xF384),
        # Thumb32 wide data-processing (shifted register)
        ('and.w r0, r1, r2', 0xEA01, 0x0002),
        ('orr.w r0, r1, r2', 0xEA41, 0x0002),
        ('eor.w r0, r1, r2', 0xEA81, 0x0002),
        ('bic.w r0, r1, r2', 0xEA21, 0x0002),
        ('orn r0, r1, r2', 0xEA61, 0x0002),
        ('mvn.w r0, r1', 0xEA6F, 0x0001),
        ('add.w r0, r1, r2', 0xEB01, 0x0002),
        ('adc.w r0, r1, r2', 0xEB41, 0x0002),
        ('sbc.w r0, r1, r2', 0xEB61, 0x0002),
        ('sub.w r0, r1, r2', 0xEBA1, 0x0002),
        ('rsb.w r0, r1, #0', 0xF1C1, 0x0000),
        ('mov.w r0, r1', 0xEA4F, 0x0001),
        ('lsl.w r0, r1, r2', 0xFA01, 0xF002),
        ('lsr.w r0, r1, r2', 0xFA21, 0xF002),
        ('asr.w r0, r1, r2', 0xFA41, 0xF002),
        ('ror.w r0, r1, r2', 0xFA61, 0xF002),
        ('tst.w r0, r1', 0xEA10, 0x0F01),
        ('teq r0, r1', 0xEA90, 0x0F01),
        ('cmn.w r0, r1', 0xEB10, 0x0F01),
        ('cmp.w r0, r1', 0xEBB0, 0x0F01),
        # Thumb32 multiply
        ('mul r0, r1', 0xFB01, 0xF000),
        ('mla r0, r1, r2, r3', 0xFB01, 0x3002),
        ('mls r0, r1, r2, r3', 0xFB01, 0x3012),
        # Thumb32 wide load/store
        ('ldr.w r0, [r1, #255]', 0xF8D1, 0x00FF),
        ('ldr.w r0, [r1, #2304]', 0xF8D1, 0x0900),  # imm12 with bits 11 and 8 set: not the imm8 form
        ('ldr r0, [r1, #-8]', 0xF851, 0x0C08),
        ('ldrb r0, [r1, #-1]', 0xF811, 0x0C01),
        ('ldrsh r0, [r1, #-2]', 0xF931, 0x0C02),
        ('strb r0, [r1, #-1]!', 0xF801, 0x0D01),
        ('ldrt r0, [r1, #4]', 0xF851, 0x0E04),
        ('ldr.w r0, [pc, #16]', 0xF8DF, 0x0010),
        ('ldr.w r0, [pc, #-16]', 0xF85F, 0x0010),
        ('pld [r1, #64]', 0xF891, 0xF040),
        ('ldrd r0, r1, [r2, #8]', 0xE9D2, 0x0102),
        ('ldrd r3, r10, [r2, #-1020]', 0xE952, 0x3AFF),
        ('ldrd r4, r5, [r2], #-8', 0xE872, 0x4502),
        ('ldrd r0, r1, [pc, #8]', 0xE9DF, 0x0102),
        ('strd r3, r9, [r2, #16]!', 0xE9E2, 0x3904),
        ('strd r0, r1, [r2]', 0xE9C2, 0x0100),
        # Thumb32 LDM/STM (A32 twins)
        ('ldm.w r0!, {r1, r2, r8}', 0xE8B0, 0x0106),
        ('ldmdb r0, {r1, r9}', 0xE910, 0x0202),
        ('stmdb sp!, {r1, r2, lr}', 0xE92D, 0x4006),
        ('stm.w r0, {r1, r10}', 0xE880, 0x0402),
        ('push.w {r4-r11, lr}', 0xE92D, 0x4FF0),
        ('pop.w {r4-r11}', 0xE8BD, 0x0FF0),
        ('pop.w {r4, r5, pc}', 0xE8BD, 0x8030),
        # Thumb32 branches and miscellaneous control
        ('beq.w #4096', 0xF001, 0x8000),
        ('blt.w #-4092', 0xF6FF, 0xA802),
        ('b.w #-2', 0xF7FF, 0xBFFF),
        ('dmb ish', 0xF3BF, 0x8F5B),
        ('dsb sy', 0xF3BF, 0x8F4F),
        ('isb sy', 0xF3BF, 0x8F6F),
        ('clrex', 0xF3BF, 0x8F2F),
        ('nop.w', 0xF3AF, 0x8000),
        ('mrs r0, apsr', 0xF3EF, 0x8000),
        ('msr apsr_nzcvq, r1', 0xF381, 0x8800),
        # Thumb32 register data processing (REV, CLZ, extends with add: A32 twins)
        ('clz r0, r1', 0xFAB1, 0xF081),
        ('rbit r0, r1', 0xFA91, 0xF0A1),
        ('rev.w r0, r1', 0xFA91, 0xF081),
        ('rev16.w r0, r1', 0xFA91, 0xF091),
        ('revsh.w r0, r1', 0xFA91, 0xF0B1),
        ('rev r0, r1', 0xBA08),
        ('rev16 r0, r1', 0xBA48),
        ('revsh r0, r1', 0xBAC8),
        ('sxtab r0, r1, r2', 0xFA41, 0xF082),
        ('uxtah r0, r1, r2, ror #8', 0xFA11, 0xF092),
        ('uxtb.w r0, r1, ror #16', 0xFA5F, 0xF0A1),
        ('sxth.w r3, r4, ror #24', 0xFA0F, 0xF3B4),
        ('lsls.w r0, r1, r2', 0xFA11, 0xF002),
        ('ands.w r0, r1, #0xff000000', 0xF011, 0x407F),  # a rotated immediate: C is its bit 31
        ('orrs.w r0, r1, #255', 0xF051, 0x00FF),  # an unrotated one: C unchanged
        ('tst.w r0, #0x80000000', 0xF010, 0x4F00),
        ('movs.w r0, r1, lsr #1', 0xEA5F, 0x0051),
        ('eors.w r0, r1, r2, ror #4', 0xEA91, 0x1032),
        ('mvns.w r0, r1, asr #32', 0xEA7F, 0x0021),
        ('rors.w r0, r1, r2', 0xFA71, 0xF002),
        # Thumb32 long multiply and divide (A32 twins)
        ('smull r0, r1, r2, r3', 0xFB82, 0x0103),
        ('umull r0, r1, r2, r3', 0xFBA2, 0x0103),
        ('smlal r0, r1, r2, r3', 0xFBC2, 0x0103),
        ('umlal r0, r1, r2, r3', 0xFBE2, 0x0103),
        ('sdiv r0, r1, r2', 0xFB91, 0xF0F2),
        ('udiv r0, r1, r2', 0xFBB1, 0xF0F2),
        # Thumb32 multiplies past MUL/MLA/MLS whose hw2 bits 7:4 are 0000 or 0001, as MUL's and MLS's are
        ('smlabb r0, r1, r2, r3', 0xFB11, 0x3002),
        ('smlabt r0, r1, r2, r3', 0xFB11, 0x3012),
        ('smulbb r0, r1, r2', 0xFB11, 0xF002),
        ('smlad r0, r1, r2, r3', 0xFB21, 0x3002),
        ('smuad r0, r1, r2', 0xFB21, 0xF002),
        ('smlawb r0, r1, r2, r3', 0xFB31, 0x3002),
        ('smlawt r0, r1, r2, r3', 0xFB31, 0x3012),
        ('smlsd r0, r1, r2, r3', 0xFB41, 0x3002),
        ('smmla r0, r1, r2, r3', 0xFB51, 0x3002),
        ('smmul r0, r1, r2', 0xFB51, 0xF002),
        ('smmls r0, r1, r2, r3', 0xFB61, 0x3002),
        ('usad8 r0, r1, r2', 0xFB71, 0xF002),
        ('usada8 r0, r1, r2, r3', 0xFB71, 0x3002),
        ('pkhbt r0, r1, r2, lsl #20', 0xEAC1, 0x5002),
        ('pkhtb r0, r1, r2, asr #6', 0xEAC1, 0x10A2),
        # Thumb32 exclusives and acquire/release (A32 twins, a doubleword one's pair any two registers)
        ('ldrex r0, [r1]', 0xE851, 0x0F00),
        ('strex r2, r3, [r1]', 0xE841, 0x3200),
        ('lda r0, [r1]', 0xE8D1, 0x0FAF),
        ('stl r0, [r1]', 0xE8C1, 0x0FAF),
        ('ldaexd r0, r1, [r12]', 0xE8DC, 0x01FF),
        ('ldrex r0, [r1, #4]', 0xE851, 0x0F01),
        ('ldrex r0, [r1, #1020]', 0xE851, 0x0FFF),
        ('strex r2, r3, [r1, #8]', 0xE841, 0x3202),
        ('hint #9', 0xBF90),
        ('sevl', 0xBF50),
        ('cpsid i', 0xB672),
        # Thumb32 coprocessor, VFP and Advanced SIMD (A32 twins)
        ('vldr d0, [r1, #8]', 0xED91, 0x0B02),
        ('vldr d1, [pc, #8]', 0xED9F, 0x1B02),
        ('vstr s2, [r1, #4]', 0xED81, 0x1A01),
        ('vadd.f64 d0, d1, d2', 0xEE31, 0x0B02),
        ('vmul.f32 s0, s1, s2', 0xEE20, 0x0A81),
        ('vadd.i32 q8, q8, q9', 0xEF60, 0x08E2),
        ('vld1.32 {d16, d17}, [r0]', 0xF960, 0x0A8F),
        ('vst1.8 {d0}, [r1]!', 0xF901, 0x070D),
        ('vmov r0, s1', 0xEE10, 0x0A90),
        ('vmov s3, r2', 0xEE01, 0x2A90),
        ('vmrs r0, fpscr', 0xEEF1, 0x0A10),
        ('vpush {d8, d9}', 0xED2D, 0x8B04),
        ('vpop {d8}', 0xECBD, 0x8B02),
        ('vcvt.f32.s32 s0, s1', 0xEEB8, 0x0AE0),
        ('vselgt.f32 s0, s1, s2', 0xFE30, 0x0A81),
        ('vrintm.f64 d0, d1', 0xFEBB, 0x0B41),
        ('aese.8 q0, q1', 0xFFB0, 0x0302),
        ('str.w r0, [r1, #255]', 0xF8C1, 0x00FF),
        ('ldr.w r0, [r1, r2]', 0xF851, 0x0002),
        ('str.w r0, [r1, r2]', 0xF841, 0x0002),
        ('ldrb.w r0, [r1, #255]', 0xF891, 0x00FF),
        ('ldrh.w r0, [r1, #255]', 0xF8B1, 0x00FF),
        ('ldrsb.w r0, [r1, r2]', 0xF911, 0x0002),
        ('ldrsh.w r0, [r1, r2]', 0xF931, 0x0002),
        ('ldr.w r0, [r1, #4]!', 0xF851, 0x0F04),
        ('ldr.w r0, [r1], #4', 0xF851, 0x0B04),
        ('str.w r0, [r1, #4]!', 0xF841, 0x0F04),
    ]
    # short IT-guarded sequences (branches are unpredictable inside IT blocks)
    it_seq = [
        # Guards use hi-reg MOV/ADD, which do not set flags, keeping the IT
        # condition stable.
        ('it eq; moveq r8, r1', 0xBF08, 0x4688),
        ('itt ne; movne r8, r1; addne r0, r8', 0xBF1C, 0x4688, 0x4440),
        ('ittt eq; moveq r8, r1; addeq r0, r8; moveq r9, r2', 0xBF02, 0x4688, 0x4440, 0x4691),
        ('itttt ne; movne r8, r1; addne r0, r8; movne r9, r2; addne r3, r8', 0xBF1F, 0x4688, 0x4440, 0x4691, 0x4443),
        ('ite eq; moveq r8, r1; movne r8, r2', 0xBF0C, 0x4688, 0x4690),
        ('itte eq; moveq r8, r1; addeq r0, r8; movne r8, r3', 0xBF06, 0x4688, 0x4440, 0x4698),
        ('itee eq; moveq r8, r1; movne r8, r2; movne r8, r3', 0xBF0E, 0x4688, 0x4690, 0x4698),
        # PC reads at an address 2 mod 4: ADD from the PC is PC + 4, literals and ADR use Align(PC, 4)
        ('nop; add r0, pc', 0xBF00, 0x4478),
        ('nop; mov r0, pc', 0xBF00, 0x4678),
        ('nop; ldr.w r0, [pc, #12]', 0xBF00, 0xF8DF, 0x000C),
        ('nop; adr.w r0, #12', 0xBF00, 0xF20F, 0x000C),
        ('nop; vldr d1, [pc, #8]', 0xBF00, 0xED9F, 0x1B02),
        # an exclusive pair, and the doubleword ones with any second register
        ('ldrex r0, [r1]; strex r2, r3, [r1]', 0xE851, 0x0F00, 0xE841, 0x3200),
        ('ldrex r0, [r1, #12]; strex r2, r3, [r1, #12]', 0xE851, 0x0F03, 0xE841, 0x3203),
        ('ldrexb r0, [r1]; strexb r2, r3, [r1]', 0xE8D1, 0x0F4F, 0xE8C1, 0x3F42),
        ('ldrexh r0, [r1]; strexh r2, r3, [r1]', 0xE8D1, 0x0F5F, 0xE8C1, 0x3F52),
        ('ldrexd r4, r5, [r1]; strexd r2, r6, r7, [r1]', 0xE8D1, 0x457F, 0xE8C1, 0x6772),
        ('ldrexd r0, r8, [r2]; strexd r3, r9, r4, [r2]', 0xE8D2, 0x087F, 0xE8C2, 0x9473),
        ('ldaex r0, [r1]; stlex r2, r3, [r1]', 0xE8D1, 0x0FEF, 0xE8C1, 0x3FE2),
        ('ldaexd r0, r1, [r12]; stlexd r3, lr, r2, [r12]', 0xE8DC, 0x01FF, 0xE8CC, 0xE2F3),
    ]
    return [(text, text.count(';') + 1, list(halfwords)) for text, *halfwords in single + it_seq]


EDGE = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0x0000FFFF, 0xFFFF0000)


def initial(seed, n, is_mem):
    rng = random.Random(seed)
    cpu = CPU()
    mem = (C.c_uint8 * SIZE).from_buffer_copy(rng.randbytes(SIZE))
    cpu.mem = mem
    cpu.mem_size = SIZE
    cpu.cpsr = ((seed & 15) << 28) | (1 << 5) | 0x10  # Thumb state (T bit)
    for i in range(16):
        if is_mem:
            cpu.r[i] = 0x1000 + (rng.getrandbits(32) % 0x7000 & ~15)
        elif seed < len(EDGE):
            cpu.r[i] = EDGE[(seed + i) % len(EDGE)]
        else:
            cpu.r[i] = rng.getrandbits(32)
    cpu.r[15] = PC
    for i in range(32):
        cpu.d[i] = rng.getrandbits(64)
    mem[PC : PC + 4 * n] = bytes(4 * n)
    return cpu, mem


def compare(libraries, entries, states):
    import unicorn as U
    from unicorn import arm_const as A

    regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]
    total = 0
    for index, (text, n, hws) in enumerate(entries):
        u = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_THUMB)
        u.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
        u.mem_map(0, SIZE)
        is_mem = '[' in text or any(op in text for op in ('push', 'pop', 'ldm', 'stm', 'vst1', 'vld1'))
        for seed in range(states):
            cpu, mem = initial(seed, len(hws) // 2, is_mem)
            for i, h in enumerate(hws):
                mem[PC + 2 * i : PC + 2 * i + 2] = h.to_bytes(2, 'little')
            u.mem_write(0, bytes(mem))
            u.ctl_remove_cache(0, SIZE)
            u.reg_write(A.UC_ARM_REG_CPSR, cpu.cpsr)
            u.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
            u.reg_write(A.UC_ARM_REG_FPSCR, cpu.fpscr)
            for r, value in zip(regs, cpu.r):
                u.reg_write(r, value)
            for i, value in enumerate(cpu.d):
                u.reg_write(A.UC_ARM_REG_D0 + i, value)
            try:
                u.emu_start(PC | 1, PC + 2 * len(hws), count=n)
                fetched = True
            except U.UcError as exc:
                # Register-indirect branches jump to a random target; Unicorn
                # fetch-faults there. The branch itself completed (PC is set).
                if 'FETCH_UNMAPPED' in str(exc):
                    fetched = False
                elif 'EXCEPTION' not in str(exc):
                    raise AssertionError(f'Unicorn rejected {text}: {exc}') from exc
                fetched = False
            expected = (
                [u.reg_read(r) for r in regs]
                + [u.reg_read(A.UC_ARM_REG_CPSR)]
                + [u.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)]
                + [u.reg_read(A.UC_ARM_REG_FPSCR)]
            )
            expected_mem = bytes(u.mem_read(0, SIZE))
            start, guest = bytes(cpu), bytes(mem)
            for blocks in libraries:
                C.memmove(C.byref(cpu), start, len(start))
                C.memmove(mem, guest, SIZE)
                blocks.functions[index](cpu)
                label = f'{blocks.name}: {text} seed={seed}'
                if not fetched and text.startswith('svc'):
                    # SVC raises on real hardware too; check the reported exit.
                    assert cpu.exit == 4, f'{label} expected SVC exit, got {cpu.exit}'
                    total += 1
                    continue
                actual = list(cpu.r) + [cpu.cpsr] + list(cpu.d) + [cpu.fpscr]
                assert cpu.exit == 0, f'{label} exit={cpu.exit} syndrome={cpu.syndrome:x}'
                assert actual == expected, (
                    label
                    + ': '
                    + ', '.join(f'r{i}: {x:08x} != {y:08x}' for i, (x, y) in enumerate(zip(actual, expected)) if x != y)
                )
                if fetched:
                    assert bytes(mem) == expected_mem, label + ': memory differs'
                total += 1
    print(f'  {total} exact comparisons passed ({len(entries)} sequences)')


IT_BITS = 0x0600FC00
# Blocks that start or end around an IT block, as halfwords: the conditions are a block's, decided when it compiles
IT_WITNESSES = (
    ('it-alone', [0xBF08]),  # IT EQ the block can't hold the instruction of: ITSTATE 0x08 left in CPSR
    ('it-entry', [0x4688]),  # MOV r8, r1 entered with ITSTATE in CPSR: it stops, saying the ITSTATE
    ('it-end', [0x2000, 0xBF08]),  # MOVS r0, #0; IT EQ, the code ending before its instruction
    ('it-skip', [0xBF1C, 0xEE10, 0x0F10, 0x4688]),  # ITT NE; MRCNE p15 (undefined here); MOVNE r8, r1
)


def it_witnesses(functions):
    def run(index, cpsr):
        cpu, _ = initial(1, 4, False)
        cpu.cpsr = cpsr
        cpu.r[1], cpu.r[8] = 0x1111, 0x8888
        functions[index](cpu)
        return cpu

    thumb_state = (1 << 5) | 0x10
    cpu = run(0, thumb_state)
    assert cpu.exit == 0 and cpu.r[15] == PC + 2 and cpu.cpsr & IT_BITS == 0x0800, f'it-alone: {cpu.cpsr:x}'
    cpu = run(1, thumb_state | 0x0800)
    assert cpu.exit == 13 and cpu.syndrome == 0x08 and cpu.r[8] == 0x8888, f'it-entry: exit {cpu.exit} {cpu.syndrome:x}'
    cpu = run(1, thumb_state | 1 << 25 | 0x0C00)  # ITSTATE 0x0D: its bits 1:0 in 26:25, 7:2 in 15:10
    assert cpu.exit == 13 and cpu.syndrome == 0x0D, f'it-entry 0d: exit {cpu.exit} {cpu.syndrome:x}'
    cpu = run(2, thumb_state)
    assert cpu.exit == 0 and cpu.r[15] == PC + 4 and cpu.cpsr & IT_BITS == 0x0800, f'it-end: {cpu.r[15]:x}'
    cpu = run(3, thumb_state | 1 << 30)  # Z: NE fails, the MRC is skipped and so is the MOV after it
    assert cpu.exit == 0 and cpu.r[15] == PC + 8 and cpu.r[8] == 0x8888, f'it-skip: {cpu.r[15]:x} {cpu.r[8]:x}'
    cpu = run(3, thumb_state)  # NE holds: the MRC is undefined without a backend
    assert cpu.exit == 1 and cpu.r[15] == PC + 2, f'it-skip taken: exit {cpu.exit}'
    print(f'  {len(IT_WITNESSES)} IT block witnesses passed')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--blocks', required=True, help='snx-test-arm-blocks, for this host or another machine')
    p.add_argument('--states', type=int, default=24)
    add_variants(p)
    args = p.parse_args()
    entries = catalogue()
    halfwords = [hws for _, _, hws in entries] + [hws for _, hws in IT_WITNESSES]
    libraries = [Blocks(args.blocks, thumb(halfwords), **variant).refuse() for variant in variants(p, args)]
    for blocks in libraries:
        print(f'Thumb16/32: {blocks.name}', flush=True)
        it_witnesses(blocks.functions[len(entries) :])
    compare(libraries, entries, args.states)
    for blocks in libraries:
        blocks.close()


if __name__ == '__main__':
    main()
