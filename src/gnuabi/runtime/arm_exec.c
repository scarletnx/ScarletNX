/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/runtime/arm_exec.h"
#include "common/cpu/arm/exec.h"
#include "gnuabi/abi/syscalls/syscalls.h"
#include "gnuabi/abi/errno/errno.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ELF's code ranges are the executor's: the same fields, so the blocks read the program's ranges as they are */
static_assert(sizeof(GnuElfRange) == sizeof(SnxArmExecRange), "GnuElfRange is SnxArmExecRange");
static_assert(offsetof(GnuElfRange, address) == offsetof(SnxArmExecRange, address), "GnuElfRange.address");
static_assert(offsetof(GnuElfRange, data) == offsetof(SnxArmExecRange, data), "GnuElfRange.data");
static_assert(offsetof(GnuElfRange, size) == offsetof(SnxArmExecRange, size), "GnuElfRange.size");

/* AArch32 (ARM EABI) syscall numbers differ from AArch64 asm-generic. Map the
   set the GNU ABI implements; anything else reports ENOSYS. */
static uint64_t arm_syscall_number(uint32_t nr) {
    switch (nr) {
        case 1: return GNU_NR_exit;
        case 248: return GNU_NR_exit_group;
        case 256: return GNU_NR_set_tid_address;
        case 240: return GNU_NR_futex;
        case 263: return GNU_NR_clock_gettime;
        case 174: return GNU_NR_rt_sigaction;
        case 175: return GNU_NR_rt_sigprocmask;
        case 122: return GNU_NR_uname;
        case 20: return GNU_NR_getpid;
        case 224: return GNU_NR_gettid;
        case 45: return GNU_NR_brk;
        case 91: return GNU_NR_munmap;
        case 192: return GNU_NR_mmap; /* mmap2: offset is in pages */
        case 125: return GNU_NR_mprotect;
        case 384: return GNU_NR_getrandom;
        default: return (uint64_t)-1;
    }
}
static void arm_syscall(GnuProcess* p) {
    uint64_t number = arm_syscall_number(p->arm.r[7]);
    if (number == (uint64_t)-1) {
        p->arm.r[0] = (uint32_t)-GNU_ENOSYS;
        return;
    }
    uint64_t arguments[6];
    for (unsigned i = 0; i < 6; i++) arguments[i] = p->arm.r[i];
    int64_t result = gnu_syscall(p, number, arguments);
    p->arm.r[0] = (uint32_t)result;
}

int gnu_arm_run(GnuProcess* p) {
    char error[512];
    SnxArmExec* exec = snx_arm_exec_new(error, sizeof error);
    if (!exec) {
        fprintf(stderr, "gnuabi: cannot compile ARM code: %s\n", error);
        return 4;
    }
    if (!setjmp(p->exit_target)) {
        p->running = 1;
        while (!p->exited) {
            uint32_t pc = p->arm.r[15];
            int thumb = (p->arm.cpsr & SNX_ARM_T) != 0;
            SnxArmBlock block = snx_arm_exec_block(exec, (const SnxArmExecRange*)p->elf.code, p->elf.code_count, pc,
                                                   thumb, p->memory.base);
            if (!block) {
                gnu_process_fault(p, 4, "Cannot compile an ARM guest block");
                break;
            }
            p->arm.exit = SNX_ARM_OK;
            block(&p->arm);
            switch (p->arm.exit) {
                case SNX_ARM_OK: break;
                case SNX_ARM_SVC: arm_syscall(p); break;
                case SNX_ARM_BREAKPOINT: gnu_process_fault(p, 5, "Guest executed BKPT"); break;
                case SNX_ARM_UNDEFINED: gnu_process_fault(p, 4, "Guest executed an undefined instruction"); break;
                case SNX_ARM_DATA_ABORT:
                    gnu_process_fault(p, 11, "Guest data access violated mapped permissions");
                    break;
                case SNX_ARM_ALIGNMENT: gnu_process_fault(p, 7, "Guest performed an unaligned access"); break;
                case SNX_ARM_ENDIAN_UNSUPPORTED:
                    gnu_process_fault(p, 4, "Big-endian guest state is unsupported");
                    break;
                default: gnu_process_fault(p, 4, "Guest entered an unsupported instruction state"); break;
            }
        }
        if (!p->exited) gnu_process_fault(p, 4, "Guest entry returned without exit/exit_group");
    }
    p->running = 0;
    snx_arm_exec_free(exec);
    return p->exit_code;
}
