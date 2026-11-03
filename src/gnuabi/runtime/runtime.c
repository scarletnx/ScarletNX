/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gnuabi/process/process.h"
#include "gnuabi/abi/errno/errno.h"
#include "gnuabi/runtime/jit.h"
#include "gnuabi/kernel/signals.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif
/* the calls that a signal stops but never restarts after its handler (signal(7)): they fail with EINTR */
static int never_restarts(uint64_t number) {
    switch (number) {
        case 22:  /* epoll_pwait */
        case 72:  /* pselect6 */
        case 73:  /* ppoll */
        case 441: /* epoll_pwait2 */
            return 1;
    }
    return 0;
}

/* the guest goes on at cpu->pc through the dispatch table: its host frames go, as after a code change */
static void resume(GnuThread* t) {
    if (t->running) longjmp(t->exit_target, 3);
}

static void syscall_entry(CPU* c, u32 immediate) {
    GnuThread* t = c->thread;
    GnuProcess* p = gnu_process_of(t);
    if (immediate) {
        gnu_process_fault(t, 4, "Linux syscall entry requires SVC #0");
        return;
    }
    (void)gnu_jit_changed(&p->jit, 0);
    uint64_t number = (uint32_t)c->x[8], args[6];
    for (int i = 0; i < 6; i++) args[i] = c->x[i];
    if (number == 139) { /* rt_sigreturn: the registers are the frame's, x0 too */
        gnu_signal_return(&t->k);
        (void)gnu_signal_deliver(&t->k, NULL, 0); /* a signal the restored mask unblocks enters its handler first */
        resume(t);
        return;
    }
    for (;;) {
        gnu_kernel_enter_call(&t->k);
        int64_t result = gnu_syscall(t, number, args);
        gnu_kernel_leave_call(&t->k);
        /* a host call that a host signal stopped: Linux's own result for it, which the signal's action turns into
           EINTR or a restart */
        if (result == -GNU_EINTR) result = never_restarts(number) ? -GNU_ERESTARTNOHAND : -GNU_ERESTARTSYS;
        if (__atomic_load_n(&t->k.interrupt, __ATOMIC_SEQ_CST)) gnu_thread_drain(t); /* before the kernel looks */
        int delivered = gnu_signal_deliver(&t->k, &result, args[0]);
        if (delivered == GNU_DELIVER_RESTART) continue;
        if (delivered == GNU_DELIVER_HANDLER) {
            resume(t);
            return;
        }
        c->x[0] = (u64)result;
        break;
    }
    /* an mprotect or munmap may have taken the next instruction's page: fetching it faults, as on Linux */
    if (p->elf.bits != 32 && !gnu_memory_access(&p->memory, c->pc, 4, GNU_PROT_EXEC))
        gnu_process_fault(t, 11, "Guest instruction fetch violated mapped execution permissions");
    /* or changed its code, whose host code this function is: the rest runs from the code as it is now */
    if (gnu_jit_changed(&p->jit, c->pc)) resume(t);
}
static void undefined(CPU* c, u64 pc, u32 instruction) {
    char reason[160];
    snprintf(reason, sizeof reason, "Unsupported instruction %08x at 0x%llx", instruction, (unsigned long long)pc);
    c->pc = pc;
    gnu_process_fault((GnuThread*)c->thread, 4, reason);
}
static void breakpoint(CPU* c, u64 pc, u32 immediate) {
    char reason[160];
    snprintf(reason, sizeof reason, "Guest BRK #%u at 0x%llx", immediate, (unsigned long long)pc);
    c->pc = pc;
    gnu_process_fault((GnuThread*)c->thread, 5, reason);
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
/* RtAPI.poll, on a backward branch or a dispatch of generated code, with every guest register in the CPU: the pc
   where --sample-pc's watchdog (below) found the thread, and a signal or the end of the process that another thread
   sent it */
static void poll(CPU* c) {
    GnuThread* t = c->thread;
    __atomic_store_n(&c->poll, 0, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&t->sample, 0, __ATOMIC_RELAXED))
        fprintf(stderr, "[%u:%u] sample: pc 0x%llx lr 0x%llx sp 0x%llx x0 0x%llx x1 0x%llx\n",
                gnu_process_of(t)->kernel.pid, t->k.tid, (unsigned long long)c->pc, (unsigned long long)c->x[30], (unsigned long long)c->x[31],
                (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
    if (__atomic_load_n(&t->k.interrupt, __ATOMIC_SEQ_CST)) gnu_thread_drain(t);
    if (gnu_signal_deliver(&t->k, NULL, 0) == GNU_DELIVER_HANDLER) resume(t);
}
const RtAPI gnu_rtapi = {syscall_entry, undefined,  hint, ticks, breakpoint,   fpcr,
                         event,         wait_event, poll, NULL,  gnu_jit_miss, snx_runtime_cas128, NULL};

/* --sample-pc: once a second, the guest's pc on stderr (poll above), for a loop that makes no syscall. A forked child
   has no watchdog thread, so it starts its own at its next exec */
#ifndef _WIN32
static struct {
    int sample; /* gnu_sample_pc's */
} g_runtime;
void gnu_sample_pc(int on) { g_runtime.sample = on; }
static void* sampler(void* process) {
    GnuProcess* p = process;
    for (;;) {
        gnu_sleep(1, 0);
        gnu_mutex_lock(&p->kernel.lock);
        for (size_t i = 0; i < p->kernel.thread_count; i++) {
            GnuThread* t = p->kernel.threads[i]->runtime;
            __atomic_store_n(&t->sample, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&t->cpu.poll, 1, __ATOMIC_RELAXED);
        }
        gnu_mutex_unlock(&p->kernel.lock);
    }
    return NULL;
}
void gnu_start_sampler(GnuProcess* p) {
    static uint32_t started_for;
    pthread_t thread;
    if (!g_runtime.sample || started_for == (uint32_t)getpid()) return;
    started_for = (uint32_t)getpid();
    if (!pthread_create(&thread, NULL, sampler, p)) pthread_detach(thread);
}
#else
void gnu_start_sampler(GnuProcess* p) { (void)p; }
#endif
