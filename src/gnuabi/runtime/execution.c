/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "gnuabi/runtime/execution.h"
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#endif
#include "gnuabi/runtime/arm_exec.h"
/* the process whose memory a fault on another host thread (an LLVM worker) is tested against, and the host's
   handlers before the runtime's */
static struct {
    GnuProcess* process;
    int installed;
#ifdef _WIN32
    PVOID guard;
#else
    struct sigaction previous_segv, previous_bus;
#endif
} g_execution;
/* a fault of the guest's code on its own thread: the thread runs guest code there */
static GnuThread* faulting_thread(uintptr_t address) {
    GnuThread* t = gnu_thread_self();
    uintptr_t base = t ? (uintptr_t)gnu_process_of(t)->memory.base : 0;
    return t && t->running && address >= base && address - base < gnu_process_of(t)->memory.size ? t : NULL;
}
/* the host's fault handler leaves the guest code for its run loop, which ends the process (gnu_process_fault, with
   its locks, outside the handler) */
static void guest_fault(GnuThread* t, int signal) { longjmp(t->exit_target, 16 + signal); }
#ifdef _WIN32
static LONG CALLBACK access_fault(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        exception->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t address = (uintptr_t)exception->ExceptionRecord->ExceptionInformation[1];
        GnuProcess* p = g_execution.process;
        GnuThread* t = faulting_thread(address);
        /* the JIT's dispatch table is committed as lookups reach it */
        if (p && gnu_jit_commit(&p->jit, address)) return EXCEPTION_CONTINUE_EXECUTION;
        if (t && gnu_memory_code_written(&gnu_process_of(t)->memory, address - (uintptr_t)gnu_process_of(t)->memory.base))
            return EXCEPTION_CONTINUE_EXECUTION; /* a write over compiled code: the store again, on a writable page */
        if (t) guest_fault(t, 11);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void access_fault(int signal, siginfo_t* information, void* context) {
    (void)context;
    uintptr_t address = (uintptr_t)information->si_addr;
    /* a guest address on any thread: a write of the runtime (a read() into the buffer) or of guest code */
    GnuProcess* p = g_execution.process;
    if (p && address >= (uintptr_t)p->memory.base && address - (uintptr_t)p->memory.base < p->memory.size &&
        gnu_memory_code_written(&p->memory, address - (uintptr_t)p->memory.base))
        return; /* a write over compiled code: the store again, on a writable page */
    GnuThread* t = faulting_thread(address);
    if (t) {
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, signal);
        (void)pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL);
        guest_fault(t, signal);
    }
    (void)sigaction(signal, signal == SIGSEGV ? &g_execution.previous_segv : &g_execution.previous_bus, NULL);
    (void)raise(signal);
}
#endif
/* runs the A64 program until it exits or execs, or the thread ends: a guest RET that matches no host frame returns all
   the way here, and the run goes on from cpu->pc, as after a longjmp, a vfork or an exec. The reason it stopped:
   1 the process ends, 2 an execve, 4 the thread ends */
static int run_a64(GnuThread* t) {
    int reason = setjmp(t->exit_target);
    /* 3: a syscall took the code it would return into (gnu_jit_invalidate), or a signal's handler starts or returns:
       the guest goes on at cpu->pc */
    if (!reason || reason == 3) {
        t->running = 1;
        gnu_kernel_check_stop(&t->k);
        for (;;) gnu_jit_call(&gnu_process_of(t)->jit, &t->cpu, t->cpu.pc);
    }
    t->running = 0;
    if (reason >= 16) { /* a fault in guest code, from the host's handler */
        gnu_process_fault(t, reason - 16, "Guest memory access violated mapped permissions");
        reason = gnu_process_of(t)->exited && gnu_process_of(t)->kernel.stopper == &t->k ? 1 : 4;
    }
    return reason;
}

static void install(GnuProcess* p) {
    g_execution.process = p;
    if (g_execution.installed) return;
    g_execution.installed = 1;
    gnu_thread_install_kick();
#ifdef _WIN32
    g_execution.guard = AddVectoredExceptionHandler(1, access_fault);
#else
    struct sigaction guard;
    memset(&guard, 0, sizeof guard);
    /* Specific fix for FreeBSD whom doesn't expose the sa_sigaction macro */
#if !defined(__linux__) && !defined(__NetBSD__) && !defined(__XSI_VISIBLE)
    guard.__sigaction_u.__sa_sigaction = access_fault;
#else
    guard.sa_sigaction = access_fault;
#endif
    guard.sa_flags = SA_SIGINFO;
    sigemptyset(&guard.sa_mask);
    (void)sigaction(SIGSEGV, &guard, &g_execution.previous_segv);
    (void)sigaction(SIGBUS, &guard, &g_execution.previous_bus);
#endif
}

int gnu_runtime_run(GnuThread* t) {
    GnuProcess* p = gnu_process_of(t);
    install(p);
    gnu_thread_set_self(t);
    for (;;) {
        int reason = p->elf.bits == 32 ? gnu_arm_run(t) : run_a64(t);
        if (reason != 2 || !p->exec || gnu_spawn_finish_exec(t)) break;
    }
    return p->exit_code;
}
