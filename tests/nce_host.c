/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE's machine layer (src/nx/runtime/nce/nce_host.c) on an AArch64 Linux host, or under qemu-aarch64: guest code in
   a page of its own, patched with the patcher's stubs (nce_patch.c), runs natively. Its outcalls keep each register,
   the flags and FPCR while the host's code changes them. A hook runs its handler, or the function's own code from
   its moved first word. A handler runs guest code again (a nested run). A kick stops a loop of guest code that makes
   no outcall, and the loop goes on with its registers. A BRK stops in the runtime, which steps past it. The counter's
   stubs read the guest clock as guest_ticks does (runtime/clock.c), across a pause and a resume */
/* the native build defines it (tests/CMakeLists.txt), the qemu one does not */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/nce/nce_host.h"
#include "nx/runtime/nce/nce_patch.h"
#include "nx/runtime/platform/plat.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static int failures;
static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/* the guest's page: code from 0, a pool of stubs from 0x2000, the guest's stack below 0x10000 */
#define GUEST_SIZE 0x10000u
#define POOL_AT    0x2000u
static u8* g_guest;
static NcePool g_pool;
static NceEnv g_env;
static u64 g_trap;

static u64 at(u32 offset) { return (u64)(uintptr_t)g_guest + offset; }

/* ---- the guest's code ---- */

/* f, at 0: x19, d8, v31 and Z set, SVC #0x26 (x0 += 100), then x2 = Z, MRS TPIDRRO_EL0 into x1, x0 += x1, BL to g
   (the hook), x3 = x0, RET with x0 restored from x20 */
static const u32 f_code[] = {
    0xD2824693u, /* MOVZ X19, #0x1234 */
    0x1E6E1008u, /* FMOV D8, #1.0 */
    0x4F0767FFu, /* MOVI V31.4S, #0xFF, LSL #24 */
    0xEB00001Fu, /* CMP X0, X0: Z */
    0xD40004C1u, /* SVC #0x26 */
    0x9A9F17E2u, /* CSET X2, EQ */
    0xD53BD061u, /* MRS X1, TPIDRRO_EL0 */
    0x8B010000u, /* ADD X0, X0, X1 */
    0xAA1E03F5u, /* MOV X21, X30 */
    0xAA0003F4u, /* MOV X20, X0 */
    0x94000000u, /* BL g (filled in) */
    0xAA0003E3u, /* MOV X3, X0 */
    0xAA1403E0u, /* MOV X0, X20 */
    0xAA1503FEu, /* MOV X30, X21 */
    0xD65F03C0u, /* RET */
};
#define F_BL 10
/* g, at 0x100: MOVZ X0, #9; RET. Its first word moves to the hook's stub */
static const u32 g_code[] = {0xD2800120u, 0xD65F03C0u};
#define G_AT 0x100u
/* h, at 0x200: SVC #0x27 (a nested run of n), then RET */
static const u32 h_code[] = {0xD40004E1u, 0xD65F03C0u};
#define H_AT 0x200u
/* n, at 0x300: MOVZ X0, #77; RET */
static const u32 n_code[] = {0xD28009A0u, 0xD65F03C0u};
#define N_AT 0x300u
/* l, at 0x400: x16 and x17 set, then a loop on the word at x2 with no outcall, then BRK #5, then RET */
static const u32 l_code[] = {
    0xD2800AB0u, /* MOVZ X16, #0x55 */
    0xD2800CD1u, /* MOVZ X17, #0x66 */
    0xB9400041u, /* loop: LDR W1, [X2] */
    0x34FFFFE1u, /* CBZ W1, loop */
    0xD42000A0u, /* BRK #5 */
    0xD65F03C0u, /* RET */
};
#define L_AT 0x400u
/* e, at 0x500: SVC #0x29, which ends its host thread */
static const u32 e_code[] = {0xD4000521u, 0xD65F03C0u};
#define E_AT 0x500u
/* k, at 0x600: three reads of the system counter, into x5, x6 and x1 (whose stub keeps x2 and x3, not x0 and x1) */
static const u32 k_code[] = {
    0xD53BE045u, /* MRS X5, CNTVCT_EL0 */
    0xD53BE026u, /* MRS X6, CNTPCT_EL0 */
    0xD53BE041u, /* MRS X1, CNTVCT_EL0 */
    0xD65F03C0u, /* RET */
};
#define K_AT   0x600u
/* the same at 0x700, whose stubs read a clock of a 19.2 MHz counter (a phone's), whose factor has a fraction */
#define K19_AT 0x700u

/* the clock that plat_ns is (plat_sdl.c's SDL_GetTicksNS), which the guest clock follows until the counter's stubs
   take it to the host's counter. Twice as fast as CLOCK_MONOTONIC: qemu-user's counter keeps the host clock's pace, so
   only a guest clock that left plat_ns for the counter reads as the stubs do */
u64 plat_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return 2u * ((u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec);
}

static _Alignas(16) u8 g_exit_stack[0x2000];
static int g_exit_after;
static u64 g_exit_reads;
static void* exit_thread(void* p) {
    CPU* c = (CPU*)p;
    char why[240];
    if (nce_host_attach(c, &g_exit_reads, why, sizeof why)) return NULL;
    c->x[31] = (u64)(uintptr_t)(g_exit_stack + sizeof g_exit_stack);
    c->x[30] = 0;
    c->pc = (u64)(uintptr_t)g_guest + E_AT;
    nce_host_run(c);
    g_exit_after = 1; /* never: the SVC ended the thread */
    return NULL;
}

static void put(u32 offset, const u32* words, size_t count) {
    for (size_t i = 0; i < count; i++) snx_st_le32(g_guest + offset + i * 4, words[i]);
}

/* each word of the encodings that the patcher changes, from offset to offset + 4 * count, with env's stubs */
static void patch_with(u32 offset, size_t count, const NceEnv* env) {
    for (size_t i = 0; i < count; i++) {
        u64 a = at(offset + (u32)i * 4);
        u32 w = snx_ld_le32(g_guest + offset + i * 4), word;
        unsigned reg, imm;
        NceKind kind = nce_kind(w, &reg, &imm);
        NceSite s;
        if (kind == NCE_NONE) continue;
        s.addr = a;
        s.word = w;
        s.kind = (unsigned char)kind;
        s.reg = (unsigned char)reg;
        s.proven = 1;
        s.imm = imm;
        check(!nce_emit(&g_pool, 1, env, &s, &word), "a stub for each site");
        snx_st_le32(g_guest + offset + i * 4, word);
    }
}
static void patch(u32 offset, size_t count) { patch_with(offset, count, &g_env); }

/* ---- the runtime's side ---- */

static int g_hook_handles = 1; /* 1: the hook's handler answers, 0: the function's own code runs */
static u64 g_reads;            /* the main thread's count of counter reads, which the stubs add to */
static u64 g_original;
static unsigned g_preempts, g_traps;
static volatile u32 g_flag;

static void run_from(CPU* c, u64 pc) {
    u64 back = c->x[30];
    c->x[30] = g_trap;
    c->pc = pc;
    nce_host_run(c);
    c->x[30] = back;
}

static void dispatch(CPU* c, u64 code) {
    unsigned kind = (unsigned)(code >> 16), imm = (unsigned)(code & 0xFFFFu);
    /* the host's registers change here, as the runtime's code may change them */
    __asm__ volatile("mov x19, #0\n movi v31.16b, #0\n fmov d8, xzr\n cmn x19, #1\n msr fpcr, xzr" ::
                         : "x19", "v8", "v31", "cc", "memory");
    if (kind == NCE_HOOK && imm == NCE_TRAP) nce_host_leave();
    if (kind == NCE_HOOK && imm == 7) {
        if (g_hook_handles) {
            c->x[0] = 42;
            c->pc = c->x[30];
        } else
            c->pc = g_original;
        return;
    }
    if (kind == NCE_SVC && imm == 0x26) c->x[0] += 100;
    if (kind == NCE_SVC && imm == 0x29) pthread_exit(NULL); /* svcExitThread's end of the host thread */
    if (kind == NCE_SVC && imm == 0x27) {
        CPU saved = *c;
        run_from(c, at(N_AT));
        saved.x[0] = c->x[0];
        *c = saved;
    }
}
static void preempted(CPU* c) {
    g_preempts++;
    check(c->x[16] == 0x55 && c->x[17] == 0x66, "a kick keeps x16 and x17");
    g_flag = 1;
}
static void trapped(CPU* c, int sig) {
    u32 w = snx_ld_le32((const u8*)(uintptr_t)c->pc);
    check(sig == SIGTRAP && w == 0xD42000A0u, "BRK #5 stops in the runtime at its word");
    g_traps++;
    c->pc += 4;
}

/* the indirect form (Android's TLS_SLOT_APP): a thread whose word another library holds refuses NCE, saying the value
   it found, and takes it once the word is 0 */
typedef struct {
    int held, attach, freed;
    char why[240];
} Held;
static void* held_thread(void* p) {
    Held* h = (Held*)p;
    void** word = (void**)(void*)((u8*)__builtin_thread_pointer() + g_env.tls);
    char why[240];
    CPU c;
    memset(&c, 0, sizeof c);
    *word = (void*)(uintptr_t)0x5A5A5A5A0ull;
    h->held = nce_host_check(h->why, sizeof h->why);
    h->attach = nce_host_attach(&c, NULL, why, sizeof why);
    *word = NULL;
    h->freed = nce_host_check(why, sizeof why);
    return NULL;
}

typedef struct {
    int tid;
    volatile int done;
} Kicker;
static void* kicker(void* p) {
    Kicker* k = (Kicker*)p;
    struct timespec ts = {0, 20 * 1000 * 1000};
    for (int i = 0; i < 200 && !k->done && !g_flag; i++) {
        nanosleep(&ts, NULL);
        nce_host_kick(k->tid);
    }
    return NULL;
}

/* the host's counter, read as clock.c reads it */
static u64 counter_now(void) {
    u64 v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
}
/* the clock of a 19.2 MHz counter that k19's stubs read: frozen, then start */
static u64 g_clock19[2];
#define F19 19200000u
static u64 ticks19(u64 cnt) {
    __extension__ typedef unsigned __int128 u128;
    u64 hi = 1000000000u / F19, lo = (u64)(((u128)(1000000000u % F19) << 64) / F19);
    u64 ns = cnt * hi + (u64)(((u128)cnt * lo) >> 64) - g_clock19[1];
    return ns / 10000u * 192u + ns % 10000u * 192u / 10000u;
}

/* the three reads of k against guest_ticks before and after them */
static void counter_test(CPU* c, u64 reads) {
    u64 before, after, frozen;
    c->x[0] = 0x1111;
    c->x[2] = 0x2222;
    c->x[3] = 0x3333;
    before = guest_ticks();
    run_from(c, at(K_AT));
    after = guest_ticks();
    check(before <= c->x[5] && c->x[5] <= c->x[6] && c->x[6] <= c->x[1] && c->x[1] <= after,
          "the stubs' reads lie between guest_ticks before and after, in order");
    check(c->x[0] == 0x1111 && c->x[2] == 0x2222 && c->x[3] == 0x3333, "the stubs keep their scratch registers");
    check(c->x[31] == at(GUEST_SIZE), "the stubs leave sp as it was");
    check(g_reads == reads + 3, "each read counted in the thread's count");

    clock_pause();
    frozen = guest_ticks();
    {
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    run_from(c, at(K_AT));
    check(c->x[5] == frozen && c->x[6] == frozen && c->x[1] == frozen,
          "a paused clock reads where the pause stopped it");
    clock_resume();
    run_from(c, at(K_AT));
    after = guest_ticks();
    check(c->x[5] >= frozen && c->x[1] <= after, "after the resume, on from the pause, behind guest_ticks");
    check(c->x[5] - frozen < 100u * 19200u, "the 200 ms paused are not counted");

    /* a counter of 19.2 MHz: its nanoseconds take the factor's fraction (UMULH), which this host's may not have */
    before = counter_now();
    run_from(c, at(K19_AT));
    after = counter_now();
    check(ticks19(before) <= c->x[5] && c->x[5] <= c->x[6] && c->x[6] <= c->x[1] && c->x[1] <= ticks19(after),
          "a 19.2 MHz counter's reads lie between its ticks before and after");
}

int main(void) {
    NceHostConfig config = {dispatch, preempted, trapped, 0, 0};
    ClockCounter counter;
    char why[160];
    CPU c;
    u32 bl;
    memset(&c, 0, sizeof c);
    g_guest = (u8*)mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_guest == (u8*)MAP_FAILED) {
        puts("nce_host: no RWX memory, skipped");
        return 77;
    }
    config.lo = at(0);
    config.hi = at(GUEST_SIZE);
    if (nce_host_init(&config, &g_env.tls, &g_env.indirect, &g_env.outcall, why, sizeof why)) {
        fprintf(stderr, "FAIL: nce_host_init: %s\n", why);
        return 1;
    }
    clock_start();
    if (clock_from_counter(&counter)) {
        fprintf(stderr, "FAIL: the host's counter runs at %llu Hz\n", (unsigned long long)counter.freq);
        return 1;
    }
    g_env.clock = counter.words;
    g_env.ns_lo = counter.mul_lo;
    g_env.ns_hi = counter.mul_hi;
    g_env.ns_origin = counter.origin;
    g_pool.buf = g_guest + POOL_AT;
    g_pool.addr = at(POOL_AT);
    g_pool.size = 0x4000;
    put(0, f_code, sizeof f_code / 4);
    bl = 0x94000000u | (u32)(((G_AT - F_BL * 4) >> 2) & 0x3FFFFFFu);
    snx_st_le32(g_guest + F_BL * 4, bl);
    put(G_AT, g_code, 2);
    put(H_AT, h_code, 2);
    put(N_AT, n_code, 2);
    put(L_AT, l_code, sizeof l_code / 4);
    patch(0, sizeof f_code / 4);
    patch(H_AT, 2);
    put(E_AT, e_code, 2);
    patch(E_AT, 2);
    put(K_AT, k_code, sizeof k_code / 4);
    patch(K_AT, sizeof k_code / 4);
    {
        __extension__ typedef unsigned __int128 u128;
        NceEnv env = g_env;
        env.clock = (u64)(uintptr_t)g_clock19;
        env.ns_hi = 1000000000u / F19;
        env.ns_lo = (u64)(((u128)(1000000000u % F19) << 64) / F19);
        env.ns_origin = 0;
        g_clock19[1] = counter_now() * env.ns_hi - 1000000000u; /* about a second of guest time */
        put(K19_AT, k_code, sizeof k_code / 4);
        patch_with(K19_AT, sizeof k_code / 4, &env);
    }
    {
        u32 word;
        check(!nce_divert(&g_pool, 1, &g_env, at(G_AT), g_code[0], 7, &word, &g_original), "g's hook");
        snx_st_le32(g_guest + G_AT, word);
    }
    check(!nce_trap(&g_pool, 1, &g_env, NCE_TRAP, &g_trap), "the trap");
    __builtin___clear_cache((char*)g_guest, (char*)g_guest + GUEST_SIZE);

    check(!nce_host_attach(&c, &g_reads, why, sizeof why), "the main thread attaches");
    c.tpidrro = 0x1000;
    c.x[31] = at(GUEST_SIZE);
    c.fpcr = 1u << 24; /* FZ: the guest's, which the host's code doesn't run with */
    run_from(&c, at(0));
    check(c.x[19] == 0x1234, "x19 kept across the outcall");
    check(c.x[2] == 1, "Z kept across the outcall");
    check(c.v[8].d[0] == 0x3FF0000000000000ull, "d8 kept across the outcall");
    check(c.v[31].s[0] == 0xFF000000u && c.v[31].s[3] == 0xFF000000u, "v31 kept across the outcall");
    check(c.fpcr == 1u << 24, "the guest's FPCR kept across the outcall");
    check(c.x[0] == 100 + 0x1000, "the SVC's result, and TPIDRRO_EL0 from the CPU");
    check(c.x[3] == 42, "the hook's handler answers the BL");
    check(c.x[31] == at(GUEST_SIZE), "sp back as it was");

    g_hook_handles = 0;
    c.x[0] = 0;
    run_from(&c, at(0));
    check(c.x[3] == 9, "the hooked function's own code, from its moved first word");

    c.x[0] = 1;
    run_from(&c, at(H_AT));
    check(c.x[0] == 77, "a nested run from an outcall");
    counter_test(&c, g_reads);

    {
        pthread_t th;
        Kicker k = {nce_host_tid(), 0};
        c.x[2] = (u64)(uintptr_t)&g_flag;
        pthread_create(&th, NULL, kicker, &k);
        run_from(&c, at(L_AT));
        k.done = 1;
        pthread_join(th, NULL);
        check(g_preempts >= 1 && g_flag, "a kick stops a loop that makes no outcall");
        check(g_traps == 1, "BRK stops in the runtime once, and the code goes on past it");
        check(c.x[16] == 0x55 && c.x[17] == 0x66, "x16 and x17 after the loop");
    }
    {
        pthread_t th;
        CPU e;
        memset(&e, 0, sizeof e);
        pthread_create(&th, NULL, exit_thread, &e);
        pthread_join(th, NULL);
        check(!g_exit_after, "an outcall ends its host thread with pthread_exit");
    }
    if (g_env.indirect) {
        pthread_t th;
        Held h;
        memset(&h, 0, sizeof h);
        pthread_create(&th, NULL, held_thread, &h);
        pthread_join(th, NULL);
        check(h.held == -1 && strstr(h.why, "0x5a5a5a5a0") != NULL, "a word another library holds, with its value");
        check(h.attach == -1, "no thread attaches over another library's word");
        check(h.freed == 0, "a word of 0 is free");
    }
    if (failures) return 1;
    printf("nce_host: ok (%s form, the CPU pointer %s TPIDR_EL0 + %llu; the counter at %llu Hz)\n",
           g_env.indirect ? "indirect" : "direct", g_env.indirect ? "through" : "at", (unsigned long long)g_env.tls,
           (unsigned long long)counter.freq);
    return 0;
}
