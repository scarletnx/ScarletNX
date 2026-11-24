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
#include "common/cpu/arm/checked.h"
#include "common/cpu/arm/codegen.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"

void snx_arm_undefined(SnxArmCPU* cpu, uint64_t word, uint64_t reason) {
    if (cpu->rt && cpu->rt->undefined) {
        cpu->rt->undefined(cpu, (uint32_t)word, (uint32_t)reason);
        return;
    }
    cpu->exit = (uint32_t)reason;
    cpu->syndrome = (uint32_t)word;
}

void snx_arm_coproc(SnxArmCPU* cpu, uint64_t word) {
    if (cpu->rt && cpu->rt->coproc)
        cpu->rt->coproc(cpu, (uint32_t)word);
    else
        snx_arm_coproc_user(cpu, (uint32_t)word);
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
    uint32_t booth = snx_asr32(multiplier << 31, 31), carry = multiplicand * booth, sum = carry + accum;
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
    uint32_t booth0 = snx_asr32(multiplier << 27, 27);
    uint32_t booth1 = snx_asr32(multiplier << 29, 29);
    uint32_t booth2 = snx_asr32(multiplier << 31, 31);
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

void snx_arm_multiply_carry(SnxArmCPU* cpu, uint64_t instruction, uint64_t thumb) {
    uint32_t word = (uint32_t)instruction, lhs, rhs, accum = 0, accum_hi = 0;
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

/* ---- the native back ends' bus accesses (arm.h), as llvm.c's readmem and writemem make them ---- */

static int io_takes(uint32_t address, uint32_t regions, uint64_t size) {
    if (size & SNX_ARM_IO_ARM9) return address >> 24 == 0x04; /* the NDS's registers alone */
    return address >> 28 || (regions >> (address >> 24) & 1);
}
static uint32_t io_load(const uint8_t* p, unsigned bytes) {
    return bytes == 1 ? *p : bytes == 2 ? snx_ld_le16(p) : snx_ld_le32(p);
}
/* What a read of an I/O register with nothing to read gives on the GBA: the opcode the ARM7TDMI fetched last, as mGBA's
   GBALoadBad makes it. ARM: [$+8]. Thumb: [$+4] in both halves, but [$+4] above [$+2] from the BIOS and OAM, and from
   IWRAM [$+4] above [$+2] or below it by $+4's place in its word */
static uint32_t io_open_bus(const SnxArmCPU* cpu, uint32_t where) {
    if (where == ~0u) return 0; /* the NDS's registers answer a read with nothing to give with zero */
    uint32_t pc = where & ~1u, region = (pc + 4) >> 24, h2, h4;
    if (!(where & 1)) return snx_ld_le32(cpu->mem + (pc + 8));
    h2 = snx_ld_le16(cpu->mem + (pc + 2));
    h4 = snx_ld_le16(cpu->mem + (pc + 4));
    if (region == 0x00 || region == 0x07 || (region == 0x03 && ((pc + 4) & 2))) return h4 << 16 | h2;
    return h4 | (region == 0x03 ? h2 : h4) << 16;
}
uint64_t snx_arm_io_read(SnxArmCPU* cpu, uint64_t address, uint64_t size, uint64_t where) {
    uint32_t a = (uint32_t)address;
    unsigned bytes = (unsigned)size & 7;
    if (cpu->io && io_takes(a, SNX_ARM_IO_REGIONS, size))
        return cpu->io(cpu, a, (uint32_t)size & (SNX_ARM_IO_SIGNED | 7), io_open_bus(cpu, (uint32_t)where));
    if (size & SNX_ARM_IO_SIGNED) /* ARMv4T: a misaligned LDRSH reads a sign extended byte instead */
        return (uint32_t)(a & 1 ? (int32_t)(int8_t)cpu->mem[a] : (int32_t)(int16_t)io_load(cpu->mem + a, 2));
    return io_load(cpu->mem + (size & SNX_ARM_IO_LIST ? a : a & ~(uint32_t)(bytes - 1)), bytes);
}
uint64_t snx_arm_io_write(SnxArmCPU* cpu, uint64_t address, uint64_t size, uint64_t value) {
    uint32_t a = (uint32_t)address;
    unsigned bytes = (unsigned)size & 7;
    if (cpu->io && io_takes(a, SNX_ARM_IO_STORE_REGIONS, size)) return cpu->io(cpu, a, 0x100u | bytes, (uint32_t)value);
    uint8_t* p = cpu->mem + (size & SNX_ARM_IO_LIST ? a : a & ~(uint32_t)(bytes - 1));
    if (bytes == 1)
        *p = (uint8_t)value;
    else if (bytes == 2)
        snx_st_le16(p, (uint16_t)value);
    else
        snx_st_le32(p, (uint32_t)value);
    return 0;
}

/* ---- checked memory for the native back ends' blocks (arm.h), as llvm.c's bus_access makes it ---- */

/* a transfer's bytes are guest order for the callback, the state's words numbers: a transfer narrower than a word is
   its low bytes */
static void checked_bytes(const SnxArmCheckedCPU* c, unsigned char* bytes, unsigned n) {
    for (unsigned i = 0; i < n; ++i) bytes[i] = (unsigned char)(c->words[i / 4] >> 8 * (i % 4));
}
static void checked_words(SnxArmCheckedCPU* c, const unsigned char* bytes, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        if (!(i % 4)) c->words[i / 4] = 0;
        c->words[i / 4] |= (uint32_t)bytes[i] << 8 * (i % 4);
    }
}
uint64_t snx_arm_checked_fetch(SnxArmCPU* cpu, uint64_t pc, uint64_t word, uint64_t bytes) {
    SnxArmCheckedCPU* c = (SnxArmCheckedCPU*)cpu;
    unsigned char got[4] = {0};
    if (c->access(c->context, SNX_ARM_ACCESS_FETCH, (uint32_t)pc, (uint32_t)bytes, got)) return SNX_ARM_PREFETCH_ABORT;
    uint32_t w = bytes == 2 ? snx_ld_le16(got) : snx_ld_le32(got);
    return w == (uint32_t)word ? 0 : SNX_ARM_CODE_CHANGED;
}
uint64_t snx_arm_checked_access(SnxArmCPU* cpu, uint64_t operation, uint64_t address, uint64_t bytes) {
    SnxArmCheckedCPU* c = (SnxArmCheckedCPU*)cpu;
    unsigned char moved[sizeof c->words];
    unsigned n = (unsigned)bytes;
    int write = ((uint32_t)operation & 3) == SNX_ARM_ACCESS_WRITE;
    if (write) checked_bytes(c, moved, n);
    uint32_t status = c->access(c->context, (uint32_t)operation, (uint32_t)address, n, moved);
    if (!status && !write) checked_words(c, moved, n);
    return status;
}
uint64_t snx_arm_checked_strex(SnxArmCPU* cpu, uint64_t address, uint64_t bytes) {
    int reserved =
        cpu->exclusive_valid && cpu->exclusive_address == (uint32_t)address && cpu->exclusive_size == (uint32_t)bytes;
    cpu->exclusive_valid = 0;
    if (!reserved) return 1;
    uint32_t status =
        (uint32_t)snx_arm_checked_access(cpu, SNX_ARM_ACCESS_WRITE | SNX_ARM_ACCESS_EXCLUSIVE, address, bytes);
    return !status ? 0 : status == SNX_ARM_EXCLUSIVE_FAILED ? 1 : 2;
}
