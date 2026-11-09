/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's threads on one host thread, as the 3DS driver runs its (3ds/aot/dispatch.c): the scheduler gives the core
   to a ready thread, which runs block after block until it waits or its slice ends. The Vita gives a game three cores,
   so a thread spinning on another is common: each slice goes to the ready thread that has waited longest, whatever
   the priorities, which keeps every thread running as three cores would rather than starving the lower ones */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* sigaltstack */
#endif
#include "vita/runtime/process.h"
#include "vita/runtime/jit.h"
#include "vita/runtime/lockstep.h"
#include "vita/runtime/watch.h"
#include "vita/hle/hle.h"
#include "vita/gpu/render.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#endif

/* blocks between the scheduler's looks: a few milliseconds of a game's code */
#define SLICE_BLOCKS 20000u
/* the backward edges the blocks a slice chains through may take, which ends it when they run out */
#define SLICE_EDGES  2048
#define VBLANK_NS    16683333u /* 59.94 Hz */
#define CACHE_SLOTS  65536u

typedef struct {
    uint32_t key;
    SnxArmBlock block;
} Cached;
static Cached cache[CACHE_SLOTS];

/* the block at pc: the module's, else one compiled now */
static SnxArmBlock block_at(VitaProcess* p, uint32_t pc, bool thumb) {
    uint32_t key = pc | thumb;
    Cached* c = &cache[(key >> 1) & (CACHE_SLOTS - 1)];
    if (c->key == key && c->block) return c->block;
    SnxArmBlock block = vita_module_block(p->module, key);
    if (!block && p->jit) block = vita_jit_block(p->jit, pc, thumb);
    if (block) *c = (Cached){key, block};
    return block;
}

/* ---- host faults in the guest's memory: the guest's data abort, which ends the run ---- */
#ifndef _WIN32
static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_armed;
static unsigned char* fault_base;
static volatile uintptr_t fault_address;
static pthread_t fault_thread; /* the guest's: another thread's fault is never the guest's data abort */
static void on_fault(int signal, siginfo_t* info, void* context) {
    (void)context;
    uintptr_t at = (uintptr_t)info->si_addr;
    if (vita_watch_fault(at)) return; /* a watched page's first write (watch.h): it goes on */
    if (fault_armed && fault_base && pthread_equal(pthread_self(), fault_thread) && at >= (uintptr_t)fault_base &&
        at < (uintptr_t)fault_base + VITA_SPACE + 0x10000) {
        fault_address = at - (uintptr_t)fault_base;
        fault_armed = 0;
        siglongjmp(fault_jump, 1);
    }
    /* the runtime's own: the default action */
    struct sigaction def;
    memset(&def, 0, sizeof def);
    def.sa_handler = SIG_DFL;
    sigaction(signal, &def, NULL);
}
static void faults_install(unsigned char* base) {
    static unsigned char alternate[1 << 16];
    stack_t ss = {.ss_sp = alternate, .ss_size = sizeof alternate};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    fault_base = base;
    fault_thread = pthread_self();
}
#endif

static const char* wait_name(uint32_t kind) {
    static const char* const names[] = {"a delay", "a semaphore", "a mutex", "a thread's end", "an event flag",
                                        "a vblank", "audio output", "the display queue", "the GPU", "a condition",
                                        "a reader-writer lock", "a signal"};
    return kind < sizeof names / sizeof *names ? names[kind] : "?";
}
static void report_threads(VitaProcess* p) {
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        const VitaThread* t = &p->threads[i];
        if (t->state == VITA_THREAD_FREE || t->state == VITA_THREAD_DEAD) continue;
        printf("  thread 0x%08" PRIx32 " \"%s\": pc 0x%08" PRIx32 "%s lr 0x%08" PRIx32 " sp 0x%08" PRIx32, t->uid,
               t->name, t->cpu.r[15], t->cpu.cpsr & SNX_ARM_T ? " (Thumb)" : "", t->cpu.r[14], t->cpu.r[13]);
        if (t->state == VITA_THREAD_WAITING)
            printf(", waits on %s 0x%08" PRIx32, wait_name(t->wait.kind), t->wait.object);
        else if (t->state == VITA_THREAD_DORMANT)
            printf(", not started");
        putchar('\n');
    }
}
/* the blocks entered last, oldest first from g_history_at: where a bad branch came from */
#define HISTORY 160u
static uint32_t g_history[HISTORY];
static unsigned g_history_at;
/* The words on t's stack that are return addresses into the image's code (just past a BL or BLX), nearest first */
static void report_stack(VitaProcess* p, const VitaThread* t) {
    unsigned shown = 0;
    printf("  stack:");
    for (uint32_t at = t->cpu.r[13] & ~3u; shown < 12 && at - (t->cpu.r[13] & ~3u) < 0x1000; at += 4) {
        if (!vita_mapped(p, at, 4)) break;
        uint32_t w = vita_read32(p, at), ret = w & ~1u;
        const VitaSegment* s = vita_image_segment(&p->image, ret - 4, 4);
        if (!s || !(s->flags & 1)) continue;
        bool call;
        if (w & 1) { /* Thumb: BL or BLX's second halfword, or BLX register */
            uint32_t h = vita_read16(p, ret - 2);
            call = (h & 0xd000) == 0xd000 || (h & 0xd000) == 0xc000 || (h & 0xff87) == 0x4780;
        } else {
            uint32_t i = vita_read32(p, ret - 4);
            call = (i & 0x0f000000) == 0x0b000000 || (i & 0xfe000000) == 0xfa000000 || (i & 0x0ffffff0) == 0x012fff30;
        }
        if (!call) continue;
        printf(" 0x%08" PRIx32, w);
        ++shown;
    }
    putchar('\n');
}
/* The code of the last distinct blocks entered (halfwords, to decode: a spin's loop, a bad branch's source), and the
   words the registers point to (what the loop reads) */
static void report_code(VitaProcess* p, const VitaThread* t) {
    uint32_t seen[12], words[13];
    unsigned count = 0, n = 0;
    for (unsigned i = HISTORY; i-- > 0 && count < 12;) {
        uint32_t b = g_history[(g_history_at + i) % HISTORY], at = b & ~1u;
        bool dup = !b;
        for (unsigned j = 0; j < count; ++j) dup |= seen[j] == b;
        if (dup) continue;
        seen[count++] = b;
        printf("  code %08" PRIx32 "%s:", at, b & 1 ? " T" : "");
        for (uint32_t o = 0; o < 64 && vita_mapped(p, at + o, 2); o += 2) printf(" %04" PRIx32, vita_read16(p, at + o));
        putchar('\n');
    }
    for (unsigned r = 0; r < 13; ++r) {
        uint32_t a = t->cpu.r[r] & ~3u;
        bool dup = a < 0x70000000u || !vita_mapped(p, a, 32);
        for (unsigned j = 0; j < n; ++j) dup |= words[j] == a;
        if (dup) continue;
        words[n++] = a;
        printf("  mem %08" PRIx32 " (r%u):", a, r);
        for (unsigned k = 0; k < 8; ++k) printf(" %08" PRIx32, vita_read32(p, a + k * 4));
        putchar('\n');
    }
}
/* The loaded export at or below address, in the same segment: a function's start, so a crash inside a library's code
   says which function it was (look the NID up in the vita-headers database). address of a library without exports
   (the game's own code) finds nothing */
static void report_export(const VitaProcess* p, const char* what, uint32_t address) {
    const VitaExport* best = NULL;
    for (uint32_t i = 0; i < p->image.export_count; ++i) {
        const VitaExport* e = &p->image.exports[i];
        if (e->variable || (e->address & ~1u) > address) continue;
        if (!best || (e->address & ~1u) > (best->address & ~1u)) best = e;
    }
    const VitaSegment* s = best ? vita_image_segment(&p->image, address, 1) : NULL;
    const VitaSegment* b = best ? vita_image_segment(&p->image, best->address & ~1u, 1) : NULL;
    if (!s || s != b) return;
    printf("  %s 0x%08" PRIx32 " is %u bytes into the export NID 0x%08" PRIx32 " (library 0x%08" PRIx32 ") at 0x%08" PRIx32
           "\n", what, address, (unsigned)(address - (best->address & ~1u)), best->nid, best->library_nid, best->address);
}
static void report_state(VitaProcess* p, VitaThread* t);
static void report_fault(VitaProcess* p, VitaThread* t, const char* what, uint32_t detail) {
    if (!t) {
        printf("*** Crash: %s 0x%08" PRIx32 " outside the game's threads\n", what, detail);
        return;
    }
    const SnxArmCPU* c = &t->cpu;
    printf("*** Crash: %s 0x%08" PRIx32 " in thread \"%s\" at pc 0x%08" PRIx32 "%s\n", what, detail, t->name, c->r[15],
           c->cpsr & SNX_ARM_T ? " (Thumb)" : "");
    report_export(p, "pc", c->r[15]);
    report_export(p, "lr", c->r[14]);
    report_state(p, t);
}
static void report_state(VitaProcess* p, VitaThread* t) {
    const SnxArmCPU* c = &t->cpu;
    printf("  blocks before:");
    for (unsigned i = 0; i < HISTORY; ++i)
        if (g_history[(g_history_at + i) % HISTORY]) printf(" %08" PRIx32, g_history[(g_history_at + i) % HISTORY]);
    putchar('\n');
    for (unsigned i = 0; i < 16; i += 4)
        printf("  r%-2u 0x%08" PRIx32 "  r%-2u 0x%08" PRIx32 "  r%-2u 0x%08" PRIx32 "  r%-2u 0x%08" PRIx32 "\n", i,
               c->r[i], i + 1, c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    printf("  cpsr 0x%08" PRIx32 " fpscr 0x%08" PRIx32 "\n", c->cpsr, c->fpscr);
    report_stack(p, t);
    report_code(p, t);
}

#ifdef _WIN32
static VitaProcess* volatile crash_process;
static volatile LONG crash_said;
static void crash_report(EXCEPTION_POINTERS* e) {
    VitaProcess* p = crash_process;
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (!p || InterlockedExchange(&crash_said, 1)) return;
    uintptr_t at = r->NumberParameters > 1 ? (uintptr_t)r->ExceptionInformation[1] : 0;
    uintptr_t base = (uintptr_t)p->mem;
    printf("*** Host exception 0x%08lx at host pc %p, address %p (%s)\n", (unsigned long)r->ExceptionCode,
           r->ExceptionAddress, (void*)at,
           r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters > 1
               ? (r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "execute" : "read")
               : "no address");
    if (at >= base && at < base + VITA_SPACE + 0x10000) report_fault(p, p->current, "data abort at", (uint32_t)(at - base));
    else report_fault(p, p->current, "host fault, guest address unknown, last pc", 0);
    report_threads(p);
    fflush(stdout);
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    crash_report(e);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI crash_guest_access(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    VitaProcess* p = crash_process;
    if (p && r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters > 1) {
        uintptr_t at = (uintptr_t)r->ExceptionInformation[1], base = (uintptr_t)p->mem;
        if (at >= base && at < base + VITA_SPACE + 0x10000) crash_report(e);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* whether t's wait is met (its result set) or timed out, now */
static bool wait_try(VitaProcess* p, VitaThread* t, uint64_t now) {
    bool met;
    switch (t->wait.kind) {
        case VITA_WAIT_DELAY:
        case VITA_WAIT_AUDIO: /* a set: the result is the one the call made before waiting */
            met = now >= t->wait.deadline;
            if (met && !t->wait.a) t->cpu.r[0] = 0;
            return met;
        case VITA_WAIT_DISPLAY:
        case VITA_WAIT_GXM: met = vita_gxm_wait_try(p, t, now); break;
        default: met = vita_kernel_wait_try(p, t, now); break;
    }
    if (met) return true;
    if (t->wait.deadline && now >= t->wait.deadline) {
        t->cpu.r[0] = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        if (t->wait.timeout) vita_write32(p, t->wait.timeout, 0);
        return true;
    }
    return false;
}
/* how long a ready thread waits behind the others before it goes first */
#define STARVE_NS 24000000u
/* the thread to run next, or NULL when none can now. *wake is then when one may (UINT64_MAX for never by time). The
   highest priority goes first (a smaller number, and a relative one such as 0x10000100 counts as the default's 160).
   Among equals the thread that paces the frames goes first, whose work ends them, and then the one that waited
   longest, as the others run on cores of their own on the console. A thread ready for STARVE_NS goes before all */
static VitaThread* schedule(VitaProcess* p, uint64_t now, uint64_t* wake) {
    VitaThread *top = NULL, *starved = NULL;
    uint32_t top_rank = 0;
    *wake = UINT64_MAX;
    for (unsigned i = 0; i < VITA_THREADS; ++i) {
        VitaThread* t = &p->threads[i];
        if (t->state == VITA_THREAD_WAITING && wait_try(p, t, now)) {
            if (t->wait.timeout && t->cpu.r[0] != SCE_KERNEL_ERROR_WAIT_TIMEOUT) {
                uint64_t left = t->wait.deadline > now ? (t->wait.deadline - now) / 1000u : 0;
                vita_write32(p, t->wait.timeout, (uint32_t)left);
            }
            t->state = VITA_THREAD_READY;
        }
        if (t->state == VITA_THREAD_WAITING && t->wait.deadline && t->wait.deadline < *wake) *wake = t->wait.deadline;
        if (t->state != VITA_THREAD_READY) {
            t->ready_ns = 0;
            continue;
        }
        if (!t->ready_ns) t->ready_ns = now;
        if (now - t->ready_ns > STARVE_NS && (!starved || t->ready_ns < starved->ready_ns)) starved = t;
        uint32_t rank = t->priority >= 0x10000000u ? 160u : t->priority;
        if (!top || rank < top_rank || (rank == top_rank && (t == p->pacer || (top != p->pacer && t->seq < top->seq))))
            top = t, top_rank = rank;
    }
    return starved ? starved : top;
}
#define GPU_POLL_NS 200000u /* the longest sleep with a submission on the GPU */
static void sleep_until(uint64_t when) {
    uint64_t now = plat_ns();
    if (when > now) plat_sleep_ns((int64_t)(when - now));
}
static bool live(const VitaProcess* p) {
    for (unsigned i = 0; i < VITA_THREADS; ++i)
        if (p->threads[i].state == VITA_THREAD_READY || p->threads[i].state == VITA_THREAD_WAITING) return true;
    return false;
}
/* the imports by SVC immediate, as chains through svc_next in import order */
static uint32_t* svc_head;
static uint32_t* svc_next;
static uint32_t svc_heads;
static bool svc_index_make(const VitaProcess* p) {
    uint32_t count = p->image.import_count, max = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (p->image.imports[i].svc > max) max = p->image.imports[i].svc;
    svc_heads = count ? max + 1 : 0;
    svc_head = malloc((svc_heads ? svc_heads : 1) * sizeof *svc_head);
    svc_next = malloc((count ? count : 1) * sizeof *svc_next);
    if (!svc_head || !svc_next) return false;
    for (uint32_t i = 0; i < svc_heads; ++i) svc_head[i] = UINT32_MAX;
    for (uint32_t i = count; i-- > 0;) {
        uint32_t svc_imm = p->image.imports[i].svc;
        svc_next[i] = svc_head[svc_imm];
        svc_head[svc_imm] = i;
    }
    return true;
}
static void svc_index_free(void) {
    free(svc_head);
    free(svc_next);
    svc_head = svc_next = NULL;
    svc_heads = 0;
}
/* an SVC of the current thread: an import, or a trampoline's. False when the run cannot go on */
static bool svc(VitaProcess* p, VitaThread* t, uint32_t imm) {
    if (imm < VITA_SVC_RUNTIME) {
        uint32_t pc = t->cpu.r[15], address = 0;
        bool thumb = !!(t->cpu.cpsr & SNX_ARM_T);
        for (uint32_t i = imm < svc_heads ? svc_head[imm] : UINT32_MAX; i != UINT32_MAX; i = svc_next[i]) {
            const VitaImport* import = &p->image.imports[i];
            if (!!(import->stub & 1) != thumb) continue;
            address = import->stub & ~1u;
            if (pc >= address && pc <= address + 8) {
                vita_hle_call(p, t, i);
                return true;
            }
        }
        report_fault(p, t, "SVC does not match an imported function", imm);
        return false;
    }
    switch (imm) {
        case VITA_SVC_THREAD_RETURN: vita_thread_end(p, t, (int32_t)t->cpu.r[0]); return true;
        case VITA_SVC_DISPLAY_NEXT: vita_gxm_display_next(p, t); return true;
        case VITA_SVC_DISPLAY_DONE: vita_gxm_display_done(p, t); return true;
        case VITA_SVC_FIBER_RETURN: vita_fiber_ended(p, t); return true;
        case VITA_SVC_CALL_RETURN: vita_hle_call_returned(p, t); return true;
        default: report_fault(p, t, "unknown SVC", imm); return false;
    }
}
int vita_run(VitaProcess* p, double seconds) {
    char why[256];
    if (!(p->jit = vita_jit_new(p, why, sizeof why))) printf("No run-time compiler (%s): only the module's blocks run\n", why);
    uint64_t end = seconds > 0 ? plat_ns() + (uint64_t)(seconds * 1e9) : 0;
    int result = 0;
    bool closed = false;
    p->display.next_ns = plat_ns() + VBLANK_NS;
    VitaThread* volatile running = NULL;
    VitaThread* last = NULL; /* the thread that ran the last slice */
    if (!svc_index_make(p)) {
        result = 1;
        goto done;
    }
#ifndef _WIN32
    faults_install(p->mem);
    if (sigsetjmp(fault_jump, 1)) {
        report_fault(p, running, "data abort at", (uint32_t)fault_address);
        result = 1;
        goto done;
    }
#else
    crash_process = p;
    AddVectoredExceptionHandler(1, crash_guest_access);
    SetUnhandledExceptionFilter(crash_filter);
#endif
    if (!vita_watch_init(p)) printf("vita: guest memory's writes aren't watched: the GPU copies its memory each time\n");
    while (!p->exited) {
        uint64_t now = plat_ns(), wake;
        if (now >= p->display.next_ns) {
            ++p->display.vblanks;
            p->display.next_ns += VBLANK_NS;
            if (p->display.next_ns < now) p->display.next_ns = now + VBLANK_NS;
            if (!vita_display_vblank(p)) {
                closed = true;
                break;
            }
            if (end && now >= end) break;
        }
        VitaThread* t = schedule(p, now, &wake);
        if (!t) {
            if (!live(p)) break; /* every thread ended */
            uint64_t until = wake < p->display.next_ns ? wake : p->display.next_ns;
            /* a thread may be waiting for the GPU, which nothing here announces: look again soon, not at the vblank */
            if (vita_render_pending(p) && now + GPU_POLL_NS < until) until = now + GPU_POLL_NS;
            sleep_until(until);
            continue;
        }
        /* a thread resumed after another ran: its exclusive monitor cleared, as the kernel's context switch clears it
           (CLREX). Its STREX then fails and its loop reads again, where comparing the value LDREX read would let the
           store pass over the other threads' writes in between (a lock-free list's head popped and pushed back) */
        if (t != last) t->cpu.exclusive_valid = 0;
        p->current = running = last = t;
        t->seq = ++p->seq;
        t->ready_ns = 0;
#ifndef _WIN32
        fault_armed = 1;
#endif
        t->poll = SLICE_EDGES;
        for (unsigned n = 0; n < SLICE_BLOCKS && p->current == t && t->poll > 0; ++n) {
            uint32_t pc = t->cpu.r[15];
            bool thumb = !!(t->cpu.cpsr & SNX_ARM_T);
            VitaLockstep checked = vita_lockstep_step(p, t);
            if (checked == VITA_LOCKSTEP_MISMATCH) {
                result = 1;
                goto done;
            }
            if (checked == VITA_LOCKSTEP_RAN) continue;
            SnxArmBlock block = block_at(p, pc, thumb);
            if (!block) {
                report_fault(p, t, "no code at", pc);
                result = 1;
                goto done;
            }
            block(&t->cpu);
            g_history[g_history_at++ % HISTORY] = pc | thumb;
            if (t->cpu.exit == SNX_ARM_OK) continue;
            if (t->cpu.exit == SNX_ARM_SVC) {
                t->cpu.exit = SNX_ARM_OK;
                if (!svc(p, t, t->cpu.syndrome)) {
                    result = 1;
                    goto done;
                }
                if (p->exited) break;
                continue;
            }
            report_fault(p, t,
                         t->cpu.exit == SNX_ARM_UNDEFINED    ? "undefined instruction"
                         : t->cpu.exit == SNX_ARM_BREAKPOINT ? "breakpoint"
                         : t->cpu.exit == SNX_ARM_IT_ENTRY   ? "Thumb block entered inside an IT block, ITSTATE"
                                                             : "unsupported instruction (exit reason in cpsr) word",
                         t->cpu.syndrome);
            result = 1;
            goto done;
        }
#ifndef _WIN32
        fault_armed = 0;
#endif
        p->current = NULL;
    }
done:
#ifndef _WIN32
    fault_armed = 0;
#endif
    if (p->exited)
        printf("The game exited (%" PRId32 ")\n", p->exit_code);
    else if (closed)
        printf("The window was closed\n");
    else if (end && plat_ns() >= end)
        printf("Stopped after %.0f seconds\n", seconds);
    else if (!result)
        printf("Every thread ended\n");
    if (result || !p->exited) report_threads(p);
    if (!p->exited && (closed || (end && plat_ns() >= end)))
        for (unsigned i = 0; i < VITA_THREADS; ++i) {
            VitaThread* t = &p->threads[i];
            if (t->state != VITA_THREAD_READY) continue;
            printf("  running thread \"%s\":\n", t->name);
            report_state(p, t);
        }
    printf("Imports called: %" PRIu64 "\n", p->hle_calls);
    size_t hints = vita_jit_save_hints(p->jit, p->folder);
    if (hints)
        printf("%zu blocks compiled at run time were added to the hints, which a recompile (-r) starts from\n",
               hints);
    vita_jit_free(p->jit);
    p->jit = NULL;
    vita_lockstep_free();
    svc_index_free();
    return result;
}