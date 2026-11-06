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
static GnuProcess* active_process;
static int guest_address(uintptr_t address) {
    GnuProcess* p = active_process;
    uintptr_t base = p ? (uintptr_t)p->memory.base : 0;
    return p && p->running && address >= base && address - base < p->memory.size;
}
static void guest_fault(int signal) {
    GnuProcess* p = active_process;
    p->faulted = signal;
    p->exited = 1;
    p->exit_code = 128 + signal;
    static const char reason[] = "Guest memory access violated mapped permissions";
    /* Avoid allocation or stdio inside the host fault handler. */
    for (size_t i = 0; i < sizeof reason; i++) p->diagnostic[i] = reason[i];
    longjmp(p->exit_target, 1);
}
#ifdef _WIN32
static LONG CALLBACK access_fault(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        exception->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t address = (uintptr_t)exception->ExceptionRecord->ExceptionInformation[1];
        /* the JIT's dispatch table is committed as lookups reach it */
        if (active_process && gnu_jit_commit(&active_process->jit, address)) return EXCEPTION_CONTINUE_EXECUTION;
        if (guest_address(address) &&
            gnu_memory_code_written(&active_process->memory, address - (uintptr_t)active_process->memory.base))
            return EXCEPTION_CONTINUE_EXECUTION; /* a write over compiled code: the store again, on a writable page */
        if (guest_address(address)) guest_fault(11);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static struct sigaction previous_segv, previous_bus;
static void access_fault(int signal, siginfo_t* information, void* context) {
    (void)context;
    if (guest_address((uintptr_t)information->si_addr) &&
        gnu_memory_code_written(&active_process->memory,
                                (uintptr_t)information->si_addr - (uintptr_t)active_process->memory.base))
        return; /* a write over compiled code: the store again, on a writable page */
    if (guest_address((uintptr_t)information->si_addr)) {
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, signal);
        (void)sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
        guest_fault(signal);
    }
    (void)sigaction(signal, signal == SIGSEGV ? &previous_segv : &previous_bus, NULL);
    (void)raise(signal);
}
#endif
/* runs the A64 program until it exits or execs: a guest RET that matches no host frame returns all the way here,
   and the run goes on from cpu->pc, as after a longjmp, a vfork or an exec */
static void run_a64(GnuProcess* p) {
    int reason = setjmp(p->exit_target);
    /* 3: a syscall took the code it would return into (gnu_jit_invalidate), which is compiled again from cpu->pc */
    if (!reason || reason == 3) {
        p->running = 1;
        for (;;) gnu_jit_call(&p->jit, &p->cpu, p->cpu.pc);
    }
    p->running = 0;
}

int gnu_runtime_run(GnuProcess* p) {
    active_process = p;
#ifdef _WIN32
    PVOID guard = AddVectoredExceptionHandler(1, access_fault);
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
    int have_segv = !sigaction(SIGSEGV, &guard, &previous_segv);
    int have_bus = !sigaction(SIGBUS, &guard, &previous_bus);
#endif
    for (;;) {
        if (p->elf.bits == 32)
            (void)gnu_arm_run(p);
        else
            run_a64(p);
        if (!p->exec || gnu_spawn_finish_exec(p)) break;
    }
#ifdef _WIN32
    if (guard) RemoveVectoredExceptionHandler(guard);
#else
    if (have_segv) (void)sigaction(SIGSEGV, &previous_segv, NULL);
    if (have_bus) (void)sigaction(SIGBUS, &previous_bus, NULL);
#endif
    active_process = NULL;
    return p->exit_code;
}
