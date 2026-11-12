/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shared AArch32 executor: runs A32 blocks and checks the DRY SVC surface
   zero-extends r0-r7 from 32-bit to 64-bit for the common HLE handler. */
#include "common/cpu/arm/exec.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t number, arguments[6];
    int seen;
} Svc;

static int syscall(void* context, uint32_t imm, uint64_t number, const uint64_t arguments[6]) {
    Svc* s = (Svc*)context;
    (void)imm;
    s->number = number;
    for (unsigned i = 0; i < 6; i++) s->arguments[i] = arguments[i];
    s->seen = 1;
    return 1; /* stop */
}

/* Blocks forgotten in a range compile again from what memory holds there now, as when ldr:ro maps an NRO where it
   unmapped another, and every other block stays cached. 500 blocks at scattered addresses fill 1024 slots to just
   under the half the table grows at, so some share a home slot and sit further on, and every other block from
   FORGET_LO to FORGET_HI goes, one range each, from among them (blocks in a row would each have a home of their own:
   the hash spreads a run of addresses evenly) */
#define FORGET_BLOCKS 500
#define FORGET_SPAN   8192 /* 8-byte places for a block */
#define FORGET_LO     100
#define FORGET_HI     350
static void block_code(unsigned char* mem, uint32_t pc, unsigned value) {
    const uint32_t words[2] = {0xE3A00000u | (value & 0xFFu), 0xEF000000u}; /* mov r0, #value; svc #0 */
    for (unsigned k = 0; k < 8; k++) mem[pc + k] = (unsigned char)(words[k / 4] >> (k % 4 * 8));
}
static int forget_test(void) {
    static unsigned char mem[0x1000 + FORGET_SPAN * 8];
    static unsigned char taken[FORGET_SPAN];
    static uint32_t at[FORGET_BLOCKS];
    static SnxArmBlock first[FORGET_BLOCKS];
    SnxArmExecRange range = {0x1000, mem + 0x1000, FORGET_SPAN * 8};
    SnxArmExec* exec = snx_arm_exec_new(NULL, 0);
    uint32_t x = 12345;
    int failures = 0;
    if (!exec) return 1;
    for (int i = 0; i < FORGET_BLOCKS;) { /* the places, from a fixed sequence (an LCG) */
        x = x * 1103515245u + 12345u;
        uint32_t place = ((x & 0x7FFFFFFFu) >> 8) % FORGET_SPAN;
        if (taken[place]) continue;
        taken[place] = 1;
        at[i] = 0x1000u + place * 8;
        block_code(mem, at[i], (unsigned)i);
        i++;
    }
    for (int i = 0; i < FORGET_BLOCKS; i++) {
        first[i] = snx_arm_exec_block(exec, &range, 1, at[i], 0, mem);
        if (!first[i]) {
            fprintf(stderr, "FAIL: block %d does not compile\n", i);
            snx_arm_exec_free(exec);
            return 1;
        }
    }
    /* the odd blocks from FORGET_LO to FORGET_HI are another program's now */
    for (int i = FORGET_LO | 1; i < FORGET_HI; i += 2) {
        block_code(mem, at[i], (unsigned)i + 7);
        snx_arm_exec_forget(exec, at[i], at[i] + 8);
    }
    /* the blocks kept first, as nothing is compiled for them: a new block takes the first free slot of its probe, which
       would fill a hole that hid a kept block from its lookup */
    for (int k = 0; k < 2 * FORGET_BLOCKS; k++) {
        int i = k % FORGET_BLOCKS, moved = i >= FORGET_LO && i < FORGET_HI && (i & 1);
        uint32_t pc = at[i];
        if (moved != (k >= FORGET_BLOCKS)) continue;
        SnxArmBlock now = snx_arm_exec_block(exec, &range, 1, pc, 0, mem);
        if (!moved && now != first[i]) {
            fprintf(stderr, "FAIL: block %d, outside the range forgotten, was compiled again\n", i);
            failures++;
        }
        if (moved && now == first[i]) {
            fprintf(stderr, "FAIL: block %d, in the range forgotten, is still the one compiled before\n", i);
            failures++;
        }
        SnxArmCPU cpu;
        Svc svc = {0};
        memset(&cpu, 0, sizeof cpu);
        cpu.mem = mem;
        cpu.mem_size = sizeof mem;
        cpu.cpsr = 0x10;
        cpu.r[15] = pc;
        (void)snx_arm_exec_run(exec, &cpu, &range, 1, syscall, &svc);
        unsigned want = (unsigned)(moved ? i + 7 : i) & 0xFFu;
        if (!svc.seen || cpu.r[0] != want) {
            fprintf(stderr, "FAIL: block %d ran r0 = %u, %u wanted\n", i, (unsigned)cpu.r[0], want);
            failures++;
        }
    }
    snx_arm_exec_free(exec);
    return failures;
}

int main(void) {
    /* mov r0,#0x80000000; mov r1,#0xFFFFFFFF; mov r7,#7; svc #0 */
    static const uint32_t code[] = {0xE3A00102, 0xE3E01000, 0xE3A07007, 0xEF000000};
    unsigned char mem[0x10000];
    memset(mem, 0, sizeof mem);
    /* the guest's code is little-endian whatever the host's byte order */
    for (unsigned i = 0; i < sizeof code; i++) mem[0x1000 + i] = (unsigned char)(code[i / 4] >> (i % 4 * 8));
    SnxArmCPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = mem;
    cpu.mem_size = sizeof mem;
    cpu.cpsr = 0x10; /* USR, A32 */
    cpu.r[15] = 0x1000;
    SnxArmExecRange range = {0x1000, mem + 0x1000, sizeof code};
    SnxArmExec* exec = snx_arm_exec_new(NULL, 0);
    if (!exec) {
        fprintf(stderr, "FAIL: cannot create executor\n");
        return 1;
    }
    Svc svc = {0};
    int exit = snx_arm_exec_run(exec, &cpu, &range, 1, syscall, &svc);
    int failures = 0;
    /* a checked block loads without the compiler runtime's stack probe, which the JIT loader does not define */
    static const uint32_t checked_code[] = {0xE1A00000, 0xEAFFFFFE}; /* mov r0, r0; b . */
    size_t consumed = 0;
    if (!snx_arm_exec_compile(exec, checked_code, 2, 0x1000, 0, SNX_ARM_PROFILE_ARM11, 1, &consumed)) {
        fprintf(stderr, "FAIL: a checked block does not compile and load\n");
        failures++;
    }
    snx_arm_exec_free(exec);
    if (!svc.seen) {
        fprintf(stderr, "FAIL: syscall not reached (exit %d)\n", exit);
        failures++;
    } else {
        /* r0=0x80000000 and r1=0xFFFFFFFF must arrive ZERO-extended, r7 as number. */
        if (svc.arguments[0] != 0x80000000ull) {
            fprintf(stderr, "FAIL: args[0]=%llx expected 0x80000000 (zero-extend)\n",
                    (unsigned long long)svc.arguments[0]);
            failures++;
        }
        if (svc.arguments[1] != 0xFFFFFFFFull) {
            fprintf(stderr, "FAIL: args[1]=%llx expected 0xffffffff (zero-extend)\n",
                    (unsigned long long)svc.arguments[1]);
            failures++;
        }
        if (svc.number != 7) {
            fprintf(stderr, "FAIL: number=%llu expected 7\n", (unsigned long long)svc.number);
            failures++;
        }
    }
    /* The Vita's A9 profile: a store with writeback into the register it stores (a Vita game's Thumb
       str r0, [r0, #28]!, then A32's) stores the register as it was, then writes the base back */
    static const unsigned char thumb_store[] = {0x40, 0xf8, 0x1c, 0x0f, 0x00, 0xdf};     /* str.w; svc #0 */
    static const unsigned char a32_store[] = {0x1c, 0x00, 0xa0, 0xe5, 0x00, 0x00, 0x00, 0xef}; /* str; svc #0 */
    for (int thumb = 1; thumb >= 0; --thumb) {
        const unsigned char* bytes = thumb ? thumb_store : a32_store;
        size_t size = thumb ? sizeof thumb_store : sizeof a32_store;
        memset(mem, 0, sizeof mem);
        memcpy(mem + 0x1000, bytes, size);
        memset(&cpu, 0, sizeof cpu);
        cpu.mem = mem;
        cpu.mem_size = sizeof mem;
        cpu.cpsr = thumb ? 0x30 : 0x10;
        cpu.r[15] = 0x1000;
        cpu.r[0] = 0x2000;
        SnxArmExecRange store_range = {0x1000, mem + 0x1000, size};
        SnxArmExec* a9 = snx_arm_exec_new(NULL, 0);
        if (!a9) return 1;
        snx_arm_exec_set_profile(a9, SNX_ARM_PROFILE_A9);
        Svc stop = {0};
        exit = snx_arm_exec_run(a9, &cpu, &store_range, 1, syscall, &stop);
        snx_arm_exec_free(a9);
        uint32_t stored = mem[0x201c] | (uint32_t)mem[0x201d] << 8 | (uint32_t)mem[0x201e] << 16 |
                          (uint32_t)mem[0x201f] << 24;
        if (!stop.seen || stored != 0x2000 || cpu.r[0] != 0x201c) {
            fprintf(stderr, "FAIL: A9 %s store with writeback: exit %d, stored %08x, r0 %08x\n", thumb ? "Thumb" : "A32",
                    exit, (unsigned)stored, (unsigned)cpu.r[0]);
            failures++;
        }
    }
    failures += forget_test();
    if (!failures) puts("A32 executor and DRY zero-extending SVC passed");
    return failures ? 1 : 0;
}
