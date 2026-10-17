/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/process/process.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/runtime/jit.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif
static void syscall_entry(CPU* c, u32 immediate) {
    GnuProcess* p = c->thread;
    if (immediate) {
        gnu_process_fault(p, 4, "Linux syscall entry requires SVC #0");
        return;
    }
    (void)gnu_jit_changed(&p->jit, 0);
    c->x[0] = (u64)gnu_syscall(p, c->x[8], c->x);
    /* an mprotect or munmap may have taken the next instruction's page: fetching it faults, as on Linux */
    if (p->elf.bits != 32 && !gnu_memory_access(&p->memory, c->pc, 4, GNU_PROT_EXEC))
        gnu_process_fault(p, 11, "Guest instruction fetch violated mapped execution permissions");
    /* or changed its code, whose host code this function is: the rest runs from the code as it is now */
    if (gnu_jit_changed(&p->jit, c->pc) && p->running) longjmp(p->exit_target, 3);
}
static void undefined(CPU* c, u64 pc, u32 instruction) {
    char reason[160];
    snprintf(reason, sizeof reason, "Unsupported instruction %08x at 0x%llx", instruction, (unsigned long long)pc);
    c->pc = pc;
    gnu_process_fault(c->thread, 4, reason);
}
static void breakpoint(CPU* c, u64 pc, u32 immediate) {
    char reason[160];
    snprintf(reason, sizeof reason, "Guest BRK #%u at 0x%llx", immediate, (unsigned long long)pc);
    c->pc = pc;
    gnu_process_fault(c->thread, 5, reason);
}
static void hint(u64 address) { (void)address; }
static u64 ticks(void) {
    int64_t sec, ns;
    if (gnu_clock_gettime(1, &sec, &ns)) return 0;
    return (u64)sec * 19200000 + (u64)ns * 192 / 10000;
}
/* FP helpers consume c->fpcr directly without changing the host environment. */
static void fpcr(CPU* c) { (void)c; }
static void event(void) {}
static void wait_event(CPU* c) { (void)c; }
/* one guest thread: nothing asks for the core back. SNX_GNU_SAMPLE's watchdog (main.c) asks, to see where a loop that
   makes no syscall runs */
static void poll(CPU* c) {
    GnuProcess* p = c->thread;
    __atomic_store_n(&c->poll, 0, __ATOMIC_RELAXED);
    fprintf(stderr, "[%u] sample: pc 0x%llx lr 0x%llx sp 0x%llx x0 0x%llx x1 0x%llx\n", p->pid,
            (unsigned long long)c->pc, (unsigned long long)c->x[30], (unsigned long long)c->x[31],
            (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
}
const RtAPI gnu_rtapi = {syscall_entry, undefined, hint,       ticks, breakpoint,
                         fpcr,          event,     wait_event, poll,  gnu_jit_miss};

/* SNX_GNU_SAMPLE=1: once a second, the guest's pc on stderr (poll above), for a loop that makes no syscall. A forked
   child has no watchdog thread, so it starts its own at its next exec */
#ifndef _WIN32
static void* sampler(void* process) {
    GnuProcess* p = process;
    for (;;) {
        gnu_sleep(1, 0);
        __atomic_store_n(&p->cpu.poll, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
void gnu_start_sampler(GnuProcess* p) {
    static uint32_t started_for;
    pthread_t thread;
    if (!getenv("SNX_GNU_SAMPLE") || started_for == (uint32_t)getpid()) return;
    started_for = (uint32_t)getpid();
    if (!pthread_create(&thread, NULL, sampler, p)) pthread_detach(thread);
}
#else
void gnu_start_sampler(GnuProcess* p) { (void)p; }
#endif
