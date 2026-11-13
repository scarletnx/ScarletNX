/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The machine loop, after the GBA's (agb/cpu/cpu.c). The BIOS's interrupt entry and return, and the calls of it that
   wait, leave on the stacks and in the registers what the console's BIOS leaves there: the SDK's interrupt handler
   switches threads by rewriting the frame the BIOS pushed, and a thread that waits inside the BIOS is resumed there. */
#include "nds/cpu/nds_cpu.h"
#include "nds/bios/bios.h"
#include "nds/cpu/recomp.h"
#include <stdio.h>
#include <string.h>

/* bus cycles a compiled instruction takes: the ARM9 runs at twice the bus clock, an instruction a cycle of its own
   where its code and data are in the caches */
#define INSTRUCTIONS_PER_CYCLE 2u

static int bank_index(uint32_t cpsr) {
    switch (cpsr & 0x1F) {
        case 0x11: return 1; /* fiq */
        case 0x12: return 2; /* irq */
        case 0x13: return 3; /* svc */
        case 0x17: return 4; /* abt */
        case 0x1B: return 5; /* und */
        default: return 0;   /* usr and sys */
    }
}

static void switch_mode(NdsCpu* cpu, uint32_t mode) {
    int old_mode = bank_index(cpu->arm.cpsr), new_mode = bank_index(mode);
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)0x1F) | (mode & 0x1F);
    if (old_mode == new_mode) return;
    cpu->banked_r13[old_mode] = cpu->arm.r[13];
    cpu->banked_r14[old_mode] = cpu->arm.r[14];
    if (old_mode == 1 || new_mode == 1) { /* r8 to r12 change banks only in FIQ mode */
        uint32_t* saved = old_mode == 1 ? cpu->fiq_r8 : cpu->usr_r8;
        for (int i = 0; i < 5; ++i) saved[i] = cpu->arm.r[8 + i];
        for (int i = 0; i < 5; ++i) cpu->arm.r[8 + i] = new_mode == 1 ? cpu->fiq_r8[i] : cpu->usr_r8[i];
    }
    cpu->arm.r[13] = cpu->banked_r13[new_mode];
    cpu->arm.r[14] = cpu->banked_r14[new_mode];
}

/* the CPSR whole, its mode's registers banked in */
static void set_cpsr(NdsCpu* cpu, uint32_t value) {
    switch_mode(cpu, value & 0x1F);
    cpu->arm.cpsr = value;
}

static bool irq_ready(const NdsCpu* cpu) { return nds_irq_pending(cpu->board) && !(cpu->arm.cpsr & 0x80); }

/* ---- the instructions the block compiler leaves to the system ---- */

static uint32_t expanded(uint32_t word) {
    unsigned rotate = (word >> 8 & 15) * 2;
    uint32_t value = word & 0xFF;
    return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

static uint32_t shifted(uint32_t value, unsigned type, uint32_t amount, int by_register, uint32_t* carry) {
    if (by_register) {
        amount &= 0xFF;
        if (!amount) return value;
    } else if (!amount) { /* an immediate of zero: LSL by none, LSR and ASR by 32, RRX */
        if (type == 0) return value;
        if (type == 3) {
            uint32_t in = *carry;
            *carry = value & 1;
            return in << 31 | value >> 1;
        }
        amount = 32;
    }
    switch (type) {
        case 0:
            if (amount > 32) return *carry = 0, 0;
            *carry = value >> (32 - amount) & 1;
            return amount == 32 ? 0 : value << amount;
        case 1:
            if (amount > 32) return *carry = 0, 0;
            *carry = value >> (amount - 1) & 1;
            return amount == 32 ? 0 : value >> amount;
        case 2:
            if (amount >= 32) {
                *carry = value >> 31;
                return value >> 31 ? 0xFFFFFFFFu : 0;
            }
            *carry = value >> (amount - 1) & 1;
            return (uint32_t)((int32_t)value >> amount);
        default:
            amount &= 31;
            if (amount) value = value >> amount | value << (32 - amount);
            *carry = value >> 31;
            return value;
    }
}

/* a register as an instruction at pc reads it */
static uint32_t operand(const NdsCpu* cpu, unsigned index, uint32_t bias) {
    return index == 15 ? cpu->arm.r[15] + bias : cpu->arm.r[index];
}

/* the block ends after this instruction, the machine going on at target */
static void jump(NdsCpu* cpu, uint32_t target) {
    cpu->arm.r[15] = target & (cpu->arm.cpsr & SNX_ARM_T ? ~1u : ~3u);
    cpu->arm.exit = NDS_EXIT_YIELD;
}

/* An exception return: the mode's SPSR into the CPSR. User and system mode have none, and the flags stay */
static void exception_return(NdsCpu* cpu) {
    int bank = bank_index(cpu->arm.cpsr);
    if (bank) set_cpsr(cpu, cpu->spsr[bank]);
}

/* a data processing instruction the compiler refused: one that writes the PC with S set, an exception return */
static bool data_processing(NdsCpu* cpu, uint32_t word) {
    unsigned opcode = word >> 21 & 15, set = word >> 20 & 1, rn = word >> 16 & 15, rd = word >> 12 & 15;
    uint32_t carry = cpu->arm.cpsr >> 29 & 1, carry_in = carry, overflow = cpu->arm.cpsr >> 28 & 1, result, b;
    uint32_t bias = (word & 0x02000010) == 0x10 ? 12 : 8;
    uint32_t a = operand(cpu, rn, bias);
    if (word & (1u << 25)) {
        b = expanded(word);
        if (word >> 8 & 15) carry = b >> 31;
    } else if (word & 0x10)
        b = shifted(operand(cpu, word & 15, bias), word >> 5 & 3, operand(cpu, word >> 8 & 15, bias), 1, &carry);
    else
        b = shifted(operand(cpu, word & 15, bias), word >> 5 & 3, word >> 7 & 31, 0, &carry);
    uint64_t wide = 0;
    int arithmetic = 1;
    switch (opcode) {
        case 2:
        case 10: wide = (uint64_t)a + (uint32_t)~b + 1; overflow = ((a ^ b) & (a ^ (uint32_t)wide)) >> 31; break;
        case 3: wide = (uint64_t)b + (uint32_t)~a + 1; overflow = ((b ^ a) & (b ^ (uint32_t)wide)) >> 31; break;
        case 4:
        case 11: wide = (uint64_t)a + b; overflow = (~(a ^ b) & (a ^ (uint32_t)wide)) >> 31; break;
        case 5: wide = (uint64_t)a + b + carry_in; overflow = (~(a ^ b) & (a ^ (uint32_t)wide)) >> 31; break;
        case 6: wide = (uint64_t)a + (uint32_t)~b + carry_in; overflow = ((a ^ b) & (a ^ (uint32_t)wide)) >> 31; break;
        case 7: wide = (uint64_t)b + (uint32_t)~a + carry_in; overflow = ((b ^ a) & (b ^ (uint32_t)wide)) >> 31; break;
        default: arithmetic = 0; break;
    }
    if (arithmetic) {
        result = (uint32_t)wide;
        carry = (uint32_t)(wide >> 32) & 1;
    } else
        result = opcode == 0 || opcode == 8    ? a & b
                 : opcode == 1 || opcode == 9 ? a ^ b
                 : opcode == 12               ? a | b
                 : opcode == 13               ? b
                 : opcode == 14               ? a & ~b
                                              : ~b;
    int test = opcode >= 8 && opcode <= 11;
    if (set && rd == 15) {
        exception_return(cpu);
        if (test) return true;
        jump(cpu, result);
        return true;
    }
    if (set)
        cpu->arm.cpsr = (cpu->arm.cpsr & 0x0FFFFFFFu) | (result & 0x80000000u) | (uint32_t)!result << 30 | carry << 29 |
                        overflow << 28;
    if (test) return true;
    if (rd == 15)
        jump(cpu, result & ~3u);
    else
        cpu->arm.r[rd] = result;
    return true;
}

/* the user bank's register while another mode runs */
static uint32_t* user_register(NdsCpu* cpu, unsigned i) {
    int bank = bank_index(cpu->arm.cpsr);
    if (bank && (i == 13 || i == 14)) return &(i == 13 ? cpu->banked_r13 : cpu->banked_r14)[0];
    if (bank == 1 && i >= 8 && i < 13) return &cpu->usr_r8[i - 8];
    return &cpu->arm.r[i];
}

/* LDM and STM as the compiler refuses them: the S bit's forms (the user bank's registers, or with the PC loaded an
   exception return) and the lists ARMv7 leaves unpredictable, as the ARM946E-S runs them */
static bool block_transfer(NdsCpu* cpu, uint32_t word) {
    NdsBoard* board = cpu->board;
    unsigned n = word >> 16 & 15, list = word & 0xFFFF, count = 0, index = 0;
    int load = word >> 20 & 1, pre = word >> 24 & 1, up = word >> 23 & 1, wb = word >> 21 & 1, sbit = word >> 22 & 1;
    uint32_t base = operand(cpu, n, 8), pc = 0;
    if (!list) return false;
    int user = sbit && !(load && (list >> 15 & 1));
    for (unsigned i = 0; i < 16; ++i) count += list >> i & 1;
    uint32_t adjusted = up ? base + count * 4 : base - count * 4;
    uint32_t start = (up ? base + (pre ? 4 : 0) : adjusted + (pre ? 0 : 4)) & ~3u;
    for (unsigned i = 0; i < 16; ++i) {
        if (!(list >> i & 1)) continue;
        uint32_t address = start + index++ * 4;
        if (load) {
            uint32_t value = nds_read32(board, address);
            if (i == 15)
                pc = value;
            else
                *(user ? user_register(cpu, i) : &cpu->arm.r[i]) = value;
        } else
            nds_write32(board, address,
                        i == 15 ? cpu->arm.r[15] + 12 : *(user ? user_register(cpu, i) : &cpu->arm.r[i]));
    }
    /* the base written back; a load that has it in its list keeps the loaded value unless the base is the list's only
       register or not its last (the ARM946E-S's rule, as melonDS has it) */
    if (wb) {
        bool listed = load && (list >> n & 1);
        if (!listed || !(list & ~(1u << n)) || (list & ~((2u << n) - 1))) cpu->arm.r[n] = adjusted;
    }
    if (load && (list >> 15 & 1)) {
        if (sbit)
            exception_return(cpu);
        else
            cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | (pc & 1 ? SNX_ARM_T : 0);
        jump(cpu, pc);
    }
    return true;
}

/* single transfers the compiler refuses: a base the instruction also loads or writes back */
static bool single_transfer(NdsCpu* cpu, uint32_t word) {
    NdsBoard* board = cpu->board;
    unsigned n = word >> 16 & 15, d = word >> 12 & 15;
    int pre = word >> 24 & 1, up = word >> 23 & 1, wb = !pre || (word >> 21 & 1), load = word >> 20 & 1;
    unsigned bytes;
    int sign = 0;
    uint32_t offset, carry = 0;
    if ((word & 0x0C000000) == 0x04000000) {
        bytes = word & (1u << 22) ? 1 : 4;
        offset = word & (1u << 25) ? shifted(cpu->arm.r[word & 15], word >> 5 & 3, word >> 7 & 31, 0, &carry)
                                   : word & 0xFFF;
    } else {
        unsigned op = word >> 5 & 3;
        if (!load && op != 1) return false; /* LDRD and STRD */
        bytes = op == 2 ? 1 : 2;
        sign = op >= 2;
        offset = word & (1u << 22) ? (word >> 4 & 0xF0) | (word & 15) : cpu->arm.r[word & 15];
    }
    uint32_t base = operand(cpu, n, 8), adjusted = up ? base + offset : base - offset, address = pre ? adjusted : base;
    if (load) {
        uint32_t value;
        if (bytes == 4) {
            unsigned turn = (address & 3) * 8;
            value = nds_read32(board, address);
            if (turn) value = value >> turn | value << (32 - turn);
        } else if (bytes == 2)
            value = sign ? (uint32_t)(int32_t)(int16_t)nds_read16(board, address) : nds_read16(board, address);
        else
            value = sign ? (uint32_t)(int32_t)(int8_t)nds_read8(board, address) : nds_read8(board, address);
        if (wb && n != 15) cpu->arm.r[n] = adjusted;
        if (d == 15) {
            cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | (value & 1 ? SNX_ARM_T : 0);
            jump(cpu, value);
        } else
            cpu->arm.r[d] = value;
    } else {
        uint32_t value = d == 15 ? cpu->arm.r[15] + 12 : cpu->arm.r[d];
        if (bytes == 4)
            nds_write32(board, address, value);
        else if (bytes == 2)
            nds_write16(board, address, (uint16_t)value);
        else
            nds_write8(board, address, (uint8_t)value);
        if (wb && n != 15) cpu->arm.r[n] = adjusted;
    }
    return true;
}

static void undefined(SnxArmCPU* arm, uint32_t word, uint32_t reason) {
    NdsCpu* cpu = (NdsCpu*)arm;
    NdsBoard* board = cpu->board;
    bool ran = false;
    if (arm->cpsr & SNX_ARM_T) goto stop;
    if ((word >> 28) == 15) goto stop;
    if ((word & 0x0FBF0FFF) == 0x010F0000) { /* MRS */
        arm->r[word >> 12 & 15] = word & (1u << 22) ? cpu->spsr[bank_index(arm->cpsr)] : arm->cpsr;
        return;
    }
    if ((word & 0x0FB0FFF0) == 0x0120F000 || (word & 0x0FB0F000) == 0x0320F000) { /* MSR */
        unsigned fields = word >> 16 & 15;
        uint32_t value = word & (1u << 25) ? expanded(word) : arm->r[word & 15], bits = 0;
        if (fields & 8) bits |= 0xFF000000u;
        if (fields & 4) bits |= 0x00FF0000u;
        if (fields & 2) bits |= 0x0000FF00u;
        if (fields & 1) bits |= 0x000000FFu;
        if (word & (1u << 22)) {
            uint32_t* spsr = &cpu->spsr[bank_index(arm->cpsr)];
            *spsr = (*spsr & ~bits) | (value & bits);
            return;
        }
        bits &= ~(uint32_t)SNX_ARM_T; /* the state is the branches' to change */
        if (!(arm->cpsr & 0x1F & 0x0F)) bits &= 0xFF000000u; /* user mode writes the flags alone */
        uint32_t masked = arm->cpsr & 0x80;
        set_cpsr(cpu, (arm->cpsr & ~bits) | (value & bits));
        /* interrupts on again with one waiting: the block ends here, and the machine takes it */
        if (masked && irq_ready(cpu)) {
            arm->r[15] += 4;
            arm->exit = NDS_EXIT_YIELD;
        }
        return;
    }
    if ((word & 0x0FB00FF0) == 0x01000090) { /* SWP and SWPB */
        uint32_t address = arm->r[word >> 16 & 15], held = arm->r[word & 15];
        unsigned rd = word >> 12 & 15;
        if (word & (1u << 22)) {
            arm->r[rd] = nds_read8(board, address);
            nds_write8(board, address, (uint8_t)held);
        } else {
            uint32_t value = nds_read32(board, address);
            unsigned turn = (address & 3) * 8;
            arm->r[rd] = turn ? value >> turn | value << (32 - turn) : value;
            nds_write32(board, address, held);
        }
        return;
    }
    if ((word & 0x0E000000) == 0x08000000)
        ran = block_transfer(cpu, word);
    else if (((word & 0x0C000000) == 0x04000000 && (word & 0x0E000010) != 0x06000010) ||
             ((word & 0x0E000090) == 0x00000090 && (word & 0x60)))
        ran = single_transfer(cpu, word);
    else if ((word & 0x0C000000) == 0 && (word & 0x0E000090) != 0x00000090 &&
             ((word & 0x01900000) != 0x01000000)) /* not the miscellaneous instructions among the tests */
        ran = data_processing(cpu, word);
    if (ran) return;
stop:
    arm->exit = reason;
    arm->syndrome = word;
}

/* CP15, the system control coprocessor: the registers the SDK's start code sets (the caches, the protection unit's
   regions, the two TCMs) and the wait for an interrupt. Their numbers and IDs are the ARM946E-S's (GBATEK's "ARM
   CP15 System Control Coprocessor") */
static void coproc(SnxArmCPU* arm, uint32_t word) {
    NdsCpu* cpu = (NdsCpu*)arm;
    NdsBoard* board = cpu->board;
    unsigned rt = word >> 12 & 15, load = word >> 20 & 1, crn = word >> 16 & 15, crm = word & 15, op2 = word >> 5 & 7;
    unsigned id = crn << 8 | crm << 4 | op2;
    if ((word & 0x0F000F10) != 0x0E000F10 || (word >> 21 & 7)) {
        arm->exit = SNX_ARM_UNDEFINED;
        arm->syndrome = word;
        return;
    }
    if (load) {
        uint32_t value = 0;
        switch (id) {
            case 0x000:
            case 0x003:
            case 0x004:
            case 0x005:
            case 0x006:
            case 0x007: value = 0x41059461; break;
            case 0x001: value = 0x0F0D2112; break;
            case 0x002: value = 0x00140180; break;
            case 0x100: value = board->cp15_control; break;
            case 0x910: value = board->cp15_dtcm; break;
            case 0x911: value = board->cp15_itcm; break;
            default:
                if (crn == 6 && op2 < 2)
                    value = board->cp15_regions[crm & 7];
                else if (crn == 2 || crn == 3 || crn == 5)
                    value = board->cp15_misc[(crn == 5 ? 4 : crn == 3 ? 2 : 0) + (op2 & 3) % (crn == 5 ? 4 : 2)];
                break;
        }
        if (rt == 15) /* the flags from its top bits */
            arm->cpsr = (arm->cpsr & 0x0FFFFFFFu) | (value & 0xF0000000u);
        else
            arm->r[rt] = value;
        return;
    }
    uint32_t value = arm->r[rt];
    switch (id) {
        case 0x100: {
            uint32_t old = board->cp15_control;
            board->cp15_control = (old & ~0x000FF085u) | (value & 0x000FF085u);
            if ((old ^ board->cp15_control) & (1u << 16)) nds_dtcm_remap(board);
            break;
        }
        case 0x910:
            board->cp15_dtcm = value & 0xFFFFF03Eu;
            nds_dtcm_remap(board);
            break;
        case 0x911: board->cp15_itcm = value & 0x0000003Eu; break;
        case 0x704:
        case 0x782: /* wait for an interrupt */
            board->halted = 1;
            arm->r[15] += 4;
            arm->exit = NDS_EXIT_YIELD;
            break;
        default:
            if (crn == 6 && op2 < 2)
                board->cp15_regions[crm & 7] = value;
            else if (crn == 2 || crn == 3 || crn == 5)
                board->cp15_misc[(crn == 5 ? 4 : crn == 3 ? 2 : 0) + (op2 & 3) % (crn == 5 ? 4 : 2)] = value;
            /* the caches' flushes (register 7) need nothing: a block runs only while memory holds its bytes */
            break;
    }
}

/* The bus hook: the registers at 0x04000000. A write that leaves an interrupt to take, or halts, ends the block */
static uint32_t bus(SnxArmCPU* arm, uint32_t address, uint32_t size_write, uint32_t value) {
    NdsCpu* cpu = (NdsCpu*)arm;
    unsigned bytes = size_write & 0xFF;
    if (!(size_write & 0x100)) return nds_io_read(cpu->board, address, bytes);
    nds_io_write(cpu->board, address, bytes, value);
    return irq_ready(cpu) || cpu->board->frame_done;
}

void nds_cpu_init(NdsCpu* cpu, NdsBoard* board, struct NdsRecomp* recomp) {
    memset(cpu, 0, sizeof *cpu);
    cpu->board = board;
    cpu->recomp = recomp;
    cpu->rt.undefined = undefined;
    cpu->rt.coproc = coproc;
    cpu->arm.rt = &cpu->rt;
    cpu->arm.mem = board->mem.base;
    cpu->arm.mem_size = NDS_MAP_SIZE + NDS_MAP_GUARD;
    cpu->arm.io = bus;
    /* as the firmware leaves a game it starts: system mode, the stacks at the DTCM's end */
    cpu->arm.cpsr = 0x1F;
    cpu->arm.r[13] = 0x03002F7C;
    for (int i = 0; i < 6; ++i) cpu->banked_r13[i] = 0x03002F7C;
    cpu->banked_r13[2] = 0x03003F80;
    cpu->banked_r13[3] = 0x03003FC0;
    cpu->arm.r[12] = cpu->arm.r[14] = cpu->arm.r[15] = board->rom->arm9_entry;
}

/* ---- the BIOS's interrupt entry and its calls that wait ---- */

static uint32_t dtcm_end(const NdsBoard* board) { return (board->cp15_dtcm & 0xFFFFF000u) + 0x4000; }

/* the entry the BIOS performs: r0 to r3, r12 and the return on the IRQ stack, then the handler the game left at the
   DTCM's last word, which returns to NDS_BIOS_IRQ_RETURN. False while the game has left none */
static bool irq_enter(NdsCpu* cpu) {
    NdsBoard* board = cpu->board;
    uint32_t handler = nds_read32(board, dtcm_end(board) - 4), saved = cpu->arm.cpsr, next = cpu->arm.r[15];
    if (!handler) return false;
    if (board->trace > 1)
        fprintf(stderr, "nds irq: IE %08X IF %08X pc %08X handler %08X line %u\n", board->ie, board->irf, next, handler,
                board->vcount);
    switch_mode(cpu, 0x12);
    cpu->spsr[2] = saved;
    uint32_t sp = cpu->arm.r[13] - 24;
    for (unsigned i = 0; i < 4; ++i) nds_write32(board, sp + i * 4, cpu->arm.r[i]);
    nds_write32(board, sp + 16, cpu->arm.r[12]);
    nds_write32(board, sp + 20, next + 4); /* the interrupted instruction's address and four */
    cpu->arm.r[13] = sp;
    cpu->arm.r[0] = dtcm_end(board);
    cpu->arm.r[14] = NDS_BIOS_IRQ_RETURN;
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | 0x80 | (handler & 1 ? SNX_ARM_T : 0);
    cpu->arm.r[15] = handler & ~1u;
    return true;
}

static void irq_return(NdsCpu* cpu) {
    NdsBoard* board = cpu->board;
    uint32_t sp = cpu->arm.r[13], back;
    for (unsigned i = 0; i < 4; ++i) cpu->arm.r[i] = nds_read32(board, sp + i * 4);
    cpu->arm.r[12] = nds_read32(board, sp + 16);
    back = nds_read32(board, sp + 20);
    cpu->arm.r[13] = sp + 24;
    cpu->arm.r[14] = back;
    set_cpsr(cpu, cpu->spsr[2]);
    cpu->arm.r[15] = (back - 4) & (cpu->arm.cpsr & SNX_ARM_T ? ~1u : ~3u);
}

/* The BIOS's entry to a call that waits: the caller's CPSR, r11, r12 and the return on the supervisor stack, then
   system mode in ARM state with interrupts as the caller had them, r2 and lr on the caller's stack */
static void swi_enter(NdsCpu* cpu) {
    NdsBoard* board = cpu->board;
    uint32_t caller = cpu->arm.cpsr, back = cpu->arm.r[15], sp;
    switch_mode(cpu, 0x13);
    cpu->spsr[3] = caller;
    cpu->arm.r[14] = back;
    sp = cpu->arm.r[13] - 16;
    nds_write32(board, sp, caller);
    nds_write32(board, sp + 4, cpu->arm.r[11]);
    nds_write32(board, sp + 8, cpu->arm.r[12]);
    nds_write32(board, sp + 12, back);
    cpu->arm.r[13] = sp;
    switch_mode(cpu, 0x1F);
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)(SNX_ARM_T | 0x80)) | (caller & 0x80);
    sp = cpu->arm.r[13] - 8;
    nds_write32(board, sp, cpu->arm.r[2]);
    nds_write32(board, sp + 4, cpu->arm.r[14]);
    cpu->arm.r[13] = sp;
    cpu->arm.r[14] = NDS_BIOS_SWI_RETURN;
}

static void swi_return(NdsCpu* cpu) {
    NdsBoard* board = cpu->board;
    uint32_t sp = cpu->arm.r[13], caller, back;
    cpu->arm.r[2] = nds_read32(board, sp);
    cpu->arm.r[14] = nds_read32(board, sp + 4);
    cpu->arm.r[13] = sp + 8;
    switch_mode(cpu, 0x13);
    sp = cpu->arm.r[13];
    caller = nds_read32(board, sp);
    cpu->arm.r[11] = nds_read32(board, sp + 4);
    cpu->arm.r[12] = nds_read32(board, sp + 8);
    back = nds_read32(board, sp + 12);
    cpu->arm.r[13] = sp + 16;
    cpu->arm.r[14] = back;
    set_cpsr(cpu, caller);
    cpu->arm.r[15] = back;
}

/* IntrWait's look at the flags the game's handler keeps below its address: the ones r1 names come out of them, and
   IME is on after it */
static uint32_t intr_check(NdsCpu* cpu) {
    NdsBoard* board = cpu->board;
    uint32_t at = dtcm_end(board) - 8, flags = nds_read32(board, at), found = flags & cpu->arm.r[1];
    if (found) nds_write32(board, at, flags ^ found);
    board->ime = 1;
    return found;
}

static void handle_swi(NdsCpu* cpu, uint32_t syndrome, int thumb) {
    NdsBoard* board = cpu->board;
    uint32_t number = thumb ? syndrome & 0xFF : syndrome >> 16 & 0xFF;
    if (number == NDS_SWI_HALT) {
        swi_enter(cpu);
        cpu->arm.r[15] = NDS_BIOS_HALT;
        board->halted = 1;
        return;
    }
    if (number == NDS_SWI_INTR_WAIT || number == NDS_SWI_VBLANK_INTR_WAIT) {
        /* the flags asked for are dropped first when r0 says so (VBlankIntrWait always), then the ARM9's BIOS halts
           before it looks at them, whatever r0 says */
        if (number == NDS_SWI_VBLANK_INTR_WAIT) cpu->arm.r[0] = cpu->arm.r[1] = 1;
        int discard = cpu->arm.r[0] != 0;
        swi_enter(cpu);
        if (discard) (void)intr_check(cpu);
        board->ime = 1;
        cpu->arm.r[15] = NDS_BIOS_INTR_WAIT;
        board->halted = 1;
        return;
    }
    uint64_t cycles = 0;
    if (nds_bios_call(board, number, cpu->arm.r, &cycles)) {
        cpu->unknown_swi = number;
        cpu->unknown_swi_at = cpu->arm.r[15] - (thumb ? 2u : 4u);
        ++cpu->unknown_swi_count;
    }
    board->cycles += cycles;
}

int nds_cpu_run_frame(NdsCpu* cpu, char* error, size_t error_size) {
    NdsBoard* board = cpu->board;
    SnxArmCPU* arm = &cpu->arm;
    board->frame_done = 0;
    while (!board->frame_done) {
        if (board->cycles >= board->next_event) {
            nds_run_events(board);
            continue;
        }
        uint32_t pc = arm->r[15];
        /* IntrWait's loop: over once the handler flagged an interrupt it waits for, else it halts again */
        if (pc == NDS_BIOS_INTR_WAIT && !board->halted) {
            if (intr_check(cpu))
                arm->r[15] = pc = NDS_BIOS_SWI_RETURN;
            else
                board->halted = 1;
        }
        if (board->halted) {
            if (board->ie & board->irf)
                board->halted = 0;
            else {
                board->cycles = board->next_event; /* nothing runs until the next event */
                continue;
            }
        }
        if (irq_ready(cpu)) {
            if (!irq_enter(cpu)) board->cycles += 64;
            continue;
        }
        if (pc >= 0xFFFF0000u) {
            if (pc == NDS_BIOS_IRQ_RETURN)
                irq_return(cpu);
            else if (pc == NDS_BIOS_HALT || pc == NDS_BIOS_SWI_RETURN)
                swi_return(cpu);
            else if (pc == NDS_BIOS_INTR_WAIT)
                board->cycles += 64; /* awake with no interrupt it may take: the loop goes round */
            else {
                snprintf(error, error_size, "the game jumped into the BIOS at 0x%08X (lr 0x%08X)", pc, arm->r[14]);
                return -1;
            }
            continue;
        }
        int thumb = (arm->cpsr & SNX_ARM_T) != 0;
        unsigned instructions = 0;
        SnxArmBlock block = nds_recomp_block(cpu->recomp, board, pc, thumb, &instructions, error, error_size);
        if (!block) return -1;
        arm->exit = SNX_ARM_OK;
        cpu->trail[cpu->blocks & 31] = pc | (uint32_t)thumb;
        block(arm);
        ++cpu->blocks;
        cpu->instructions += instructions;
        board->cycles += (instructions + INSTRUCTIONS_PER_CYCLE - 1) / INSTRUCTIONS_PER_CYCLE;
        if (arm->exit == SNX_ARM_OK || arm->exit == NDS_EXIT_YIELD) continue;
        if (arm->exit == SNX_ARM_SVC)
            handle_swi(cpu, arm->syndrome, thumb);
        else {
            snprintf(error, error_size,
                     "the game stopped at 0x%08X (%s): exit %u, syndrome 0x%08X, lr 0x%08X, cpsr 0x%08X", arm->r[15],
                     thumb ? "Thumb" : "ARM", arm->exit, arm->syndrome, arm->r[14], arm->cpsr);
            return -1;
        }
    }
    return 0;
}
