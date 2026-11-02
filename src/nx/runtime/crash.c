/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The crash report: what failed, where (the game's recompiled code or the runtime itself), on which thread and with
   which registers, then the end of the run. The crash handler calls it with the log in crash mode */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dladdr, before any header */
#endif
#include "nx/runtime/main.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#include <signal.h>
#include <string.h>
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#endif

#if defined(__ELF__)
/* the start of the runtime's own image, for addr2line: the linker defines it for an executable and a library alike */
extern const char __ehdr_start[] __attribute__((weak, visibility("hidden")));
#endif

#define NEAR_FUNCTION ((u64)1 << 20)   /* a host pc this far past a recompiled function's start is still in it */
#define RUNTIME_WINDOW ((u64)256 << 20) /* the runtime's image is smaller than this */

static const char* crash_kind(void* fault, int sig) {
#if defined(_WIN32)
    switch ((unsigned long)sig) {
        case 0xC0000005ul: /* EXCEPTION_ACCESS_VIOLATION */
        case 0xC0000006ul: return "invalid memory access"; /* EXCEPTION_IN_PAGE_ERROR */
        case 0xC000001Dul: return "illegal instruction";
        case 0xC000008Eul: /* the float exceptions, then integer division by zero and overflow */
        case 0xC0000090ul:
        case 0xC0000091ul:
        case 0xC0000094ul:
        case 0xC0000095ul: return "arithmetic error";
        default: return "exception";
    }
#else
    if (fault || sig == SIGSEGV || sig == SIGBUS) return "invalid memory access";
    return sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "arithmetic error" : "crash";
#endif
}

/* the recompiled function whose host code starts closest below pc: 1 with its guest address and pc's offset in it */
static int game_function_at(u64 pc, u64* guest, u64* offset) {
    u64 best = 0;
    u32 i;
    if (!g_mod || !g_mod->entry_fn) return 0;
    for (i = 0; i < g_mod->nentries; i++) {
        u64 fn = (u64)(size_t)g_mod->entry_fn[i];
        if (fn <= pc && fn > best) {
            best = fn;
            *guest = g_mod->entry_addr[i];
        }
    }
    if (!best || pc - best >= NEAR_FUNCTION) return 0;
    *offset = pc - best;
    return 1;
}

/* where host_pc is: the game's code, or the runtime's own with its offset for addr2line */
static void report_place(void* host_pc) {
    u64 pc = (u64)(size_t)host_pc, guest = 0, offset = 0;
    u32 block;
    if (game_function_at(pc, &guest, &offset)) {
        WARN("  inside the recompiled code of game function %llx (+%llx on the host)\n", (unsigned long long)guest,
             (unsigned long long)offset);
        return;
    }
    block = arm_block_at(host_pc, &offset);
    if (block) {
        WARN("  inside the recompiled %s block %x (+%llx on the host)\n", block & 1 ? "Thumb" : "A32", block & ~1u,
             (unsigned long long)offset);
        return;
    }
#if defined(_WIN32)
    {
        u64 base = (u64)(size_t)GetModuleHandleW(NULL);
        if (pc >= base && pc - base < RUNTIME_WINDOW) {
            WARN("  in the runtime itself (exe offset +%llx), on %s\n", (unsigned long long)(pc - base),
                 kthread_cur() ? "a game thread" : "one of the runtime's threads");
            return;
        }
    }
#elif defined(__ELF__)
    if (__ehdr_start) {
        u64 base = (u64)(size_t)__ehdr_start;
        if (pc >= base && pc - base < RUNTIME_WINDOW) {
            WARN("  in the runtime itself (exe offset +%llx)\n", (unsigned long long)(pc - base));
            return;
        }
    }
#endif
#if !defined(_WIN32)
    /* a library's: which one, at an offset addr2line takes, and the host frames that led there */
    {
        Dl_info di;
        int found = dladdr(host_pc, &di) && di.dli_fbase;
        if (found) {
            const char* name =
                di.dli_fname && strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname;
            WARN("  in %s +%llx (%s), on %s\n", name ? name : "?",
                 (unsigned long long)((const u8*)host_pc - (const u8*)di.dli_fbase), di.dli_sname ? di.dli_sname : "?",
                 kthread_cur() ? "a game thread" : "one of the runtime's threads");
        }
#if !defined(__ANDROID__) && !defined(__FreeBSD__) /* Android's libc has dladdr but not backtrace */
        {
            void* bt[24];
            int n = backtrace(bt, 24);
            logf_(0, "  host frames:");
            for (int i = 0; i < n; i++)
                if (dladdr(bt[i], &di) && di.dli_fbase)
                    logf_(0, " %s+%llx",
                          di.dli_fname && strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : "?",
                          (unsigned long long)((const u8*)bt[i] - (const u8*)di.dli_fbase));
                else
                    logf_(0, " %p", bt[i]);
            logf_(0, "\n");
        }
#endif
        if (found) return;
    }
#endif
    WARN("  outside the game's code and the runtime (a library's)\n");
}

void crash_report(void* host_pc, void* fault, int sig) {
    const char* kind = crash_kind(fault, sig);
    u8* f = (u8*)fault;
    KThread* t = kthread_cur();
    char memory[256];
    int in_guest = g_mem && f >= g_mem && (u64)(f - g_mem) < AS_SIZE;
    u64 guest = in_guest ? (u64)(f - g_mem) : 0;
#if defined(_WIN32)
    WARN("\n*** Crash (exception 0x%08lx) at host pc=%p", (unsigned long)sig, host_pc);
#else
    WARN("\n*** Crash (signal %d) at host pc=%p", sig, host_pc);
#endif
    if (fault && in_guest)
        WARN(", access to %p (game address %llx) ***\n", fault, (unsigned long long)guest);
    else if (fault)
        WARN(", access to %p ***\n", fault);
    else
        WARN(" ***\n");
    WARN("  %s\n", kind);
    report_place(host_pc);
    /* the low pages are never mapped: a pointer the game failed to get (an allocation it ran out of memory for) plus
       a field's offset lands there */
    if (fault && in_guest && guest < ((u64)16 << 20) && g_settings.memory_gb < 6)
        WARN("  a null pointer plus an offset: a game short of memory does this, memory_gb=6 in its .ini may help\n");
    if (t) {
        WARN("  thread %llu (%s)\n", (unsigned long long)t->id, t->name);
        kernel_dump_regs(&t->cpu);
    } else
        WARN("  on one of the runtime's threads, not the game's\n");
    if (mem_reserved()) {
        mem_report(memory, sizeof memory);
        WARN("  memory: %s\n", memory);
    }
    fatal("The game crashed (%s). See the .log for details.", kind);
}
