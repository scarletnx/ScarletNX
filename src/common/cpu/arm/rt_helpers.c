/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
/* The runtime library's calls into the backend (arm.h's SnxArmRt): what generated code can't do itself is decided by
   the runtime running it, as A64 code calls RtAPI's undef. Linked into snx_arm_rt and every 32-bit game module */
#include "common/cpu/arm/arm.h"

void snx_arm_undefined(SnxArmCPU* cpu, uint32_t word, uint32_t reason) {
    if (cpu->rt && cpu->rt->undefined) {
        cpu->rt->undefined(cpu, word, reason);
        return;
    }
    cpu->exit = reason;
    cpu->syndrome = word;
}

void snx_arm_coproc(SnxArmCPU* cpu, uint32_t word) {
    if (cpu->rt && cpu->rt->coproc)
        cpu->rt->coproc(cpu, word);
    else
        snx_arm_coproc_user(cpu, word);
}

void snx_arm_coproc_user(SnxArmCPU* cpu, uint32_t word) {
    /* MRC/MCR p15, 0, Rt, c13, c0, 2 (TPIDRURW) or 3 (TPIDRURO, read only) */
    unsigned rt = (word >> 12) & 15, load = (word >> 20) & 1, opc1 = (word >> 21) & 7, crn = (word >> 16) & 15;
    unsigned coproc = (word >> 8) & 15, opc2 = (word >> 5) & 7, crm = word & 15;
    if ((word & 0x0F000010) == 0x0E000010 && coproc == 15 && !opc1 && crn == 13 && !crm && rt != 15 &&
        (opc2 == 2 || (opc2 == 3 && load))) {
        if (!load)
            cpu->tpidrurw = cpu->r[rt];
        else
            cpu->r[rt] = opc2 == 3 ? cpu->tpidruro : cpu->tpidrurw;
        return;
    }
    cpu->exit = SNX_ARM_UNDEFINED;
    cpu->syndrome = word;
}

/* The ARM7TDMI's multiply carry: C after MULS, MLAS, the long ones with S and Thumb's MULS is the carry out of its Booth
   multiplier's last adder stage. The rule below is zaydlang's and calc84maniac's, as NanoBoyAdvance has it
   (MultiplyCarrySimple, MultiplyCarryLo, MultiplyCarryHi in ARM7TDMI.hh), altered from its original form into C:

   Copyright (C) 2024 zaydlang, calc84maniac

   This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
   liable for any damages arising from the use of this software.

   Permission is granted to anyone to use this software for any purpose, including commercial applications, and to
   alter it and redistribute it freely, subject to the following restrictions:

     1. The origin of this software must not be misrepresented; you must not claim that you wrote the original
        software. If you use this software in a product, an acknowledgment in the product documentation would be
        appreciated but is not required.
     2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
        software.
     3. This notice may not be removed or altered from any source distribution. */

/* the multiplier ends early, a byte of its multiplier at a time: 1 when it took all four steps */
static int multiply_full(uint32_t multiplier, int is_signed) {
    uint32_t mask = 0xFFFFFF00u;
    for (;;) {
        multiplier &= mask;
        if (!multiplier || (is_signed && multiplier == mask)) break;
        mask <<= 8;
    }
    return mask == 0;
}

/* all four steps: the last Booth addend is negative only when the multiplier's top two bits are 10 */
static int multiply_carry_simple(uint32_t multiplier) { return multiplier >> 30 == 2; }

/* fewer steps: the carry-save adder's carry out of bit 31 after the last one */
static int multiply_carry_lo(uint32_t multiplicand, uint32_t multiplier, uint32_t accum) {
    multiplicand |= 1; /* negating then inverts the upper bits; this bit never reaches the carry */
    uint32_t booth = (uint32_t)((int32_t)(multiplier << 31) >> 31), carry = multiplicand * booth, sum = carry + accum;
    int shift = 29;
    do {
        for (int i = 0; i < 4; ++i, shift -= 2) { /* 8 multiplier bits in 4 Booth steps */
            uint32_t next = (uint32_t)((int32_t)(multiplier << shift) >> shift), addend = multiplicand * (next - booth);
            booth = next;
            accum ^= carry ^ addend;
            sum += addend;
            carry = sum - accum;
        }
    } while (booth != multiplier);
    return (int)(carry >> 31);
}

/* a long multiply's four steps: the last three Booth addends' upper bits decide bit 63's carry */
static int multiply_carry_hi(uint32_t multiplicand, uint32_t multiplier, uint32_t accum_hi, int sign_extend) {
    if (sign_extend) {
        multiplicand = (uint32_t)((int32_t)multiplicand >> 6);
        multiplier = (uint32_t)((int32_t)multiplier >> 26);
    } else {
        multiplicand >>= 6;
        multiplier >>= 26;
    }
    multiplicand |= 1;
    uint32_t carry = ~accum_hi & 0x20000000u, accum = accum_hi - 0x08000000u; /* the magic bits 61 and 63-60 */
    uint32_t booth0 = (uint32_t)((int32_t)(multiplier << 27) >> 27);
    uint32_t booth1 = (uint32_t)((int32_t)(multiplier << 29) >> 29);
    uint32_t booth2 = (uint32_t)((int32_t)(multiplier << 31) >> 31);
    uint32_t addend = multiplicand * (booth1 - booth2); /* the third-last addend */
    accum -= addend & 0x10000000u;
    addend = multiplicand * (booth0 - booth1); /* the second-last */
    accum -= addend & 0x40000000u;
    uint32_t sum = accum + (addend & 0x20000000u);
    accum -= carry;
    addend = multiplicand * (multiplier - booth0); /* the last */
    sum += addend & 0x40000000u;
    return (int)((sum ^ accum) >> 31);
}

void snx_arm_multiply_carry(SnxArmCPU* cpu, uint32_t word, uint32_t thumb) {
    uint32_t lhs, rhs, accum = 0, accum_hi = 0;
    int full, carry, wide = 0, is_signed = 1;
    if (thumb) { /* MULS Rd, Rm: Rd is the multiplier */
        lhs = cpu->r[(word >> 3) & 7];
        rhs = cpu->r[word & 7];
    } else {
        lhs = cpu->r[word & 15];
        rhs = cpu->r[(word >> 8) & 15];
        wide = (word >> 23) & 1;
        if (wide) {
            is_signed = (word >> 22) & 1;
            if (word & (1u << 21)) {
                accum = cpu->r[(word >> 12) & 15];
                accum_hi = cpu->r[(word >> 16) & 15];
            }
        } else if (word & (1u << 21))
            accum = cpu->r[(word >> 12) & 15];
    }
    full = multiply_full(rhs, is_signed);
    if (!full)
        carry = multiply_carry_lo(lhs, rhs, accum);
    else
        carry = wide ? multiply_carry_hi(lhs, rhs, accum_hi, is_signed) : multiply_carry_simple(rhs);
    cpu->cpsr = (cpu->cpsr & ~0x20000000u) | (carry ? 0x20000000u : 0); /* C, bit 29 */
}
