/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's crash report (fault.h): a host fault in generated code, with the guest's registers and the libraries
   their addresses are in, before the JVM's handler writes its own report, which knows nothing of the guest */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "android/bridge/fault.h"
#include "android/bridge/bridge.h"
#include "android/bridge/hle_signal.h"
#include "android/bridge/host.h"
#include "android/bridge/linker.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

/* stderr, unbuffered: the process ends after it */
static void say(const char* format, ...) {
    char line[512];
    va_list a;
    va_start(a, format);
    int n = vsnprintf(line, sizeof line, format, a);
    va_end(a);
    if (n <= 0) return;
    size_t size = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
#ifdef _WIN32
    fwrite(line, 1, size, stderr);
#else
    ssize_t written = write(2, line, size);
    (void)written;
#endif
}

/* an address with the library and the symbol it is in, when it is guest code */
static void where(const char* name, uint64_t address) {
    const char *library = NULL, *symbol = NULL;
    uint64_t base = 0, at = 0;
    if (linker_address(address, &library, &base, &symbol, &at)) {
        if (symbol)
            say("  %-4s 0x%016llx  %s+0x%llx (%s+0x%llx)\n", name, (unsigned long long)address, library,
                (unsigned long long)(address - base), symbol, (unsigned long long)(address - at));
        else
            say("  %-4s 0x%016llx  %s+0x%llx\n", name, (unsigned long long)address, library,
                (unsigned long long)(address - base));
    } else {
        say("  %-4s 0x%016llx\n", name, (unsigned long long)address);
    }
}

/* the report of a fault at host pc on a thread whose CPU is c */
static void report(const CPU* c, const char* what, uint64_t host_pc, uint64_t address) {
    say("\nscarletnx-android: %s in the guest's generated code at host 0x%llx (address 0x%llx)\n", what,
        (unsigned long long)host_pc, (unsigned long long)address);
    say("  code is the guest function or block whose compiled code holds the faulting host instruction. pc and the\n"
        "  registers are the ones the code stored last (at a call, a branch out of its function or a poll)\n");
    /* the guest code that the host pc is in: the start of its function or block, by the compiled units */
    uint64_t guest = host_pc ? bridge_guest_of_host((uintptr_t)host_pc) : 0;
    if (guest) where("code", guest);
    where("pc", c->pc);
    where("x30", c->x[30]);
    where("sp", c->x[31]);
    for (int i = 0; i < 30; i += 2)
        say("  x%-2d  0x%016llx   x%-2d  0x%016llx\n", i, (unsigned long long)c->x[i], i + 1,
            (unsigned long long)c->x[i + 1]);
    say("  nzcv %u%u%u%u  tpidr 0x%llx\n", c->n, c->z, c->cf, c->vf, (unsigned long long)c->tpidr);
    /* the frame records the guest's x29 chain names: a backtrace, as far as it reads */
    uint64_t fp = c->x[29];
    for (int depth = 0; depth < 16 && fp && !(fp & 15); ++depth) {
        uint64_t record[2];
        if (!host_copy_checked(record, (const void*)(uintptr_t)fp, sizeof record) || !record[1]) break;
        char name[16];
        snprintf(name, sizeof name, "#%d", depth);
        where(name, record[1]);
        if (record[0] <= fp) break;
        fp = record[0];
    }
    say("\n");
}

#ifdef _WIN32
static LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    const CPU* c = signals_guest_code_cpu();
    if (c) {
        uint64_t address = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
        report(c, "an exception", (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, address);
    }
    return EXCEPTION_CONTINUE_SEARCH; /* the JVM's handler, and its report, follow */
}
void fault_install(void) {
    static LONG done;
    if (InterlockedExchange(&done, 1)) return;
    AddVectoredExceptionHandler(1, on_exception);
}
#else
static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
static struct sigaction previous[4];

static void on_fault(int sig, siginfo_t* info, void* context) {
    const CPU* c = signals_guest_code_cpu();
    size_t i = 0;
    while (i < 3 && signals[i] != sig) ++i;
    if (c) {
        static int reported;
        if (!__atomic_exchange_n(&reported, 1, __ATOMIC_RELAXED)) {
            uint64_t pc = 0;
#if defined(__linux__) && defined(__x86_64__)
            pc = (uint64_t)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__FreeBSD__) && defined(__x86_64__)
            pc = (uint64_t)((ucontext_t*)context)->uc_mcontext.mc_rip;
#elif defined(__APPLE__) && defined(__x86_64__)
            pc = (uint64_t)((ucontext_t*)context)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(__aarch64__)
            pc = (uint64_t)((ucontext_t*)context)->uc_mcontext->__ss.__pc;
#elif defined(__linux__) && defined(__aarch64__)
            pc = (uint64_t)((ucontext_t*)context)->uc_mcontext.pc;
#endif
            report(c, strsignal(sig), pc, (uint64_t)(uintptr_t)info->si_addr);
        }
    }
    /* the JVM's handler (its null checks fault on purpose in Java code), as if it had been called first */
    const struct sigaction* p = &previous[i];
    if (p->sa_flags & SA_SIGINFO) {
        if (p->sa_sigaction) p->sa_sigaction(sig, info, context);
    } else if (p->sa_handler == SIG_DFL || p->sa_handler == SIG_IGN) {
        struct sigaction d = {0};
        d.sa_handler = SIG_DFL;
        sigemptyset(&d.sa_mask);
        sigaction(sig, &d, NULL);
        raise(sig);
    } else {
        p->sa_handler(sig);
    }
}

void fault_install(void) {
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_RELAXED)) return;
    for (size_t i = 0; i < sizeof signals / sizeof *signals; ++i) {
        struct sigaction a = {0};
        a.sa_sigaction = on_fault;
        a.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sigemptyset(&a.sa_mask);
        sigaction(signals[i], &a, &previous[i]);
    }
}
#endif
