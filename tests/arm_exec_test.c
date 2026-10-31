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
    if (!failures) puts("A32 executor and DRY zero-extending SVC passed");
    return failures ? 1 : 0;
}
