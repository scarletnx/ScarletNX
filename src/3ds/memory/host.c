/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* sigaltstack, SA_ONSTACK */
#endif
#include "3ds/memory/host.h"
#include "common/utilities/game_log.h"
#include "common/memory/host.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* the fault handler's: the space it watches and the pc it reports */
static struct {
    unsigned char* base;
    const uint32_t* pc;
    bool (*resolve)(void* context, uint32_t va);
    void* context;
#ifdef _WIN32
    void* handler;
#else
    struct sigaction segv, bus; /* the handlers before, which a fault elsewhere goes on to */
    stack_t stack, before_stack; /* the guest thread's alternate signal stack, and the one it had */
#endif
} g_host;

bool ctr_host_flat(void) { return snx_host_page() == 4096; }

#ifdef _WIN32
static LONG CALLBACK fault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || !g_host.base)
        return EXCEPTION_CONTINUE_SEARCH;
    unsigned char* at = (unsigned char*)r->ExceptionInformation[1];
    if (at < g_host.base || at >= g_host.base + CTR_HOST_SPACE + CTR_HOST_GUARD) return EXCEPTION_CONTINUE_SEARCH;
    if (at < g_host.base + CTR_HOST_SPACE && g_host.resolve && g_host.resolve(g_host.context, (uint32_t)(at - g_host.base)))
        return EXCEPTION_CONTINUE_EXECUTION;
    fprintf(stderr, "3ds: guest data abort: %s 0x%08llx in the block at 0x%08x\n",
            r->ExceptionInformation[0] ? "write to" : "read of", (unsigned long long)(at - g_host.base),
            g_host.pc ? (unsigned)*g_host.pc : 0u);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
    return EXCEPTION_CONTINUE_SEARCH;
}
void ctr_host_faults(unsigned char* base, const uint32_t* pc, bool (*resolve)(void* context, uint32_t va),
                     void* context) {
    if (base && !g_host.handler) g_host.handler = AddVectoredExceptionHandler(1, fault);
    if (!base && g_host.handler) {
        RemoveVectoredExceptionHandler(g_host.handler);
        g_host.handler = NULL;
    }
    g_host.base = base;
    g_host.pc = pc;
    g_host.resolve = base ? resolve : NULL;
    g_host.context = context;
}
void ctr_host_say(const char* text, size_t size) {
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)size, &written, NULL);
}
#else
/* hexadecimal for a signal handler, which may not call stdio */
static void hex(char* out, uint64_t value, unsigned digits) {
    for (unsigned i = 0; i < digits; ++i) out[i] = "0123456789abcdef"[(value >> ((digits - 1 - i) * 4)) & 15];
}
static void fault(int signal, siginfo_t* info, void* context) {
    unsigned char* at = info->si_addr;
    /* a tracked page (SIGSEGV on Linux and FreeBSD, SIGBUS where a protection fault is one): resolved, the access is
       made again on return */
    if (g_host.base && at >= g_host.base && at < g_host.base + CTR_HOST_SPACE && g_host.resolve &&
        g_host.resolve(g_host.context, (uint32_t)(at - g_host.base)))
        return;
    if (g_host.base && at >= g_host.base && at < g_host.base + CTR_HOST_SPACE + CTR_HOST_GUARD) {
        char line[] = "3ds: guest data abort at 0x000000000 in the block at 0x00000000\n";
        hex(line + 27, (uint64_t)(at - g_host.base), 9);
        hex(line + 55, g_host.pc ? *g_host.pc : 0, 8);
        snx_game_log_crash(line, sizeof line - 1); /* in the game log too, before the pipe's reader is gone */
        _exit(1);
    }
    /* someone else's fault: the handler before, or the default (the signal again, without this handler) */
    const struct sigaction* before = signal == SIGBUS ? &g_host.bus : &g_host.segv;
    if (before->sa_flags & SA_SIGINFO && before->sa_sigaction)
        before->sa_sigaction(signal, info, context);
    else if (before->sa_handler != SIG_DFL && before->sa_handler != SIG_IGN && before->sa_handler)
        before->sa_handler(signal);
    else {
        sigaction(signal, before, NULL);
        raise(signal);
    }
}
/* the alternate stack a resolution runs on: it may submit to the GPU and wait for it, which the few kilobytes of a
   usual signal stack would not hold */
#define FAULT_STACK (1u << 20)
void ctr_host_faults(unsigned char* base, const uint32_t* pc, bool (*resolve)(void* context, uint32_t va),
                     void* context) {
    if (base && !g_host.base) {
        void* stack = mmap(NULL, FAULT_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack != MAP_FAILED) {
            g_host.stack = (stack_t){.ss_sp = stack, .ss_size = FAULT_STACK};
            if (sigaltstack(&g_host.stack, &g_host.before_stack)) {
                munmap(stack, FAULT_STACK);
                g_host.stack.ss_sp = NULL;
            }
        }
        struct sigaction action = {0};
        action.sa_sigaction = fault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &g_host.segv);
        sigaction(SIGBUS, &action, &g_host.bus);
    } else if (!base && g_host.base) {
        sigaction(SIGSEGV, &g_host.segv, NULL);
        sigaction(SIGBUS, &g_host.bus, NULL);
        if (g_host.stack.ss_sp) {
            sigaltstack(&g_host.before_stack, NULL);
            munmap(g_host.stack.ss_sp, FAULT_STACK);
            g_host.stack.ss_sp = NULL;
        }
    }
    g_host.base = base;
    g_host.pc = pc;
    g_host.resolve = base ? resolve : NULL;
    g_host.context = context;
}
void ctr_host_say(const char* text, size_t size) {
    ssize_t written = write(2, text, size);
    (void)written;
}
#endif
