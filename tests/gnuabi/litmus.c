/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* The Arm memory model as guest threads: the litmus tests M1 of docs/layers/cpu/plan.md asks for. Each build is one
   test (-DLITMUS_TEST), built and run by tests/gnuabi_litmus.py.
 *
 * The threads are persistent: spawning a guest thread costs a clone per iteration, so instead every thread runs the
 * same loop and the loop's two barriers of its own (sync_point) separate one attempt from the next. Between them a
 * thread runs the test's accesses, the plain loads and stores the translator lowers to ordinary host loads and stores
 * and the barrier, acquire and release it lowers to fences and to LDAR and STLR. The barriers and the counter use
 * sequentially consistent atomics, so the harness itself can never be the reason an outcome is missing.
 *
 * The parent counts the values its reads saw. Every outcome it reports must be one the model allows: a fence, an
 * acquire or a release the translator drops, or a plain access the back end moves across one, shows up as an outcome
 * no Arm core produces. The model is the AArch64 application level memory model of the Arm ARM, whose authoritative
 * form is aarch64.cat in the herd7 tool suite (Jade Alglave and Luc Maranget), and the expected sets are the ones
 * herd7 gives for the same tests written in its format. tests/litmus/ holds those files, so that the expectations can
 * be checked with herd7, or with Arm's copy of it at developer.arm.com/herd7, rather than taken on trust.
 *
 * The images are freestanding (no libc, no libgcc) for aarch64-linux-gnu, as tests/gnuabi/threads.c is. */
#include "threading.h"
#include <stdatomic.h>

#define T_SB     1 /* store buffering, DMB ISH between each store and the other thread's load */
#define T_MP     2 /* message passing, DMB ISH between the stores and between the loads */
#define T_MP_REL 3 /* message passing, STLR and LDAR instead of plain accesses and DMB */
#define T_WRC    4 /* weak read coherence on three threads, DMB ISH after each load that publishes */
#define T_SPIN   5 /* a plain load in a spin loop the other thread must satisfy */
#define T_MP_HALF                                                                                                      \
    6 /* message passing with DMB ISHST for the stores and DMB ISHLD for the loads, which each host lowers of its own */

#if !defined(LITMUS_TEST)
#error "build with -DLITMUS_TEST=1 through 6"
#endif

#if LITMUS_TEST < T_SB || LITMUS_TEST > T_MP_HALF
#error "LITMUS_TEST is 1 through 6"
#endif

#if LITMUS_TEST == T_WRC
#define THREADS 3
#else
#define THREADS 2
#endif

#if !defined(LITMUS_ITERATIONS)
#define LITMUS_ITERATIONS 200000
#endif

#if LITMUS_TEST == T_SPIN
#define ITERATIONS 1 /* the spin loop does not repeat: one attempt is the test */
#else
#define ITERATIONS LITMUS_ITERATIONS
#endif

#if LITMUS_TEST == T_WRC
#define READERS 3
#elif LITMUS_TEST == T_SPIN
#define READERS 1
#else
#define READERS 2
#endif

static u32 x, y;
/* each thread's reads, in the slot its body writes, collected in the order the model's test names them */
static u32 seen[THREADS][2];
static u32 values[READERS];
static u32 histogram[1u << READERS];
static u32 bad_values;
static _Atomic u32 arrive, sense;
static u32 phase[THREADS];
static u32 ptid[THREADS], ctid[THREADS];
alignas(16) static u8 stacks[THREADS][65536];

/* the test's own accesses: volatile keeps the fixture's compiler from caching or removing them, and leaves them
   ordinary guest loads and stores (not exclusive or atomic) for the translator. A build uses only the accessors its
   test names, so each is marked to keep -Wunused-function quiet. */
#define RETAIN [[maybe_unused]]
RETAIN static inline u32 plain_load(u32* p) { return *(volatile u32*)p; }
RETAIN static inline void plain_store(u32* p, u32 v) { *(volatile u32*)p = v; }
RETAIN static inline void dmb_ish(void) { __asm__ volatile("dmb ish" ::: "memory"); }
RETAIN static inline void dmb_ishst(void) { __asm__ volatile("dmb ishst" ::: "memory"); }
RETAIN static inline void dmb_ishld(void) { __asm__ volatile("dmb ishld" ::: "memory"); }
RETAIN static inline u32 acquire_load(u32* p) {
    u32 v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}
RETAIN static inline void release_store(u32* p, u32 v) {
    __asm__ volatile("stlr %w1, [%0]" : : "r"(p), "r"(v) : "memory");
}

/* a sense reversing barrier: every thread flips its own sense and the last one to arrive releases the others. Unlike
   a barrier that reads a shared generation before it arrives, a thread that the host delays cannot join the phase
   after. */
static void sync_point(u32 tid) {
    u32 mine = phase[tid] ^ 1u;
    phase[tid] = mine;
    if ((atomic_fetch_add_explicit(&arrive, 1, memory_order_seq_cst) + 1) == THREADS) {
        atomic_store_explicit(&arrive, 0, memory_order_relaxed);
        atomic_store_explicit(&sense, mine, memory_order_seq_cst);
    } else {
        while (atomic_load_explicit(&sense, memory_order_seq_cst) != mine) {}
    }
}

static void body(u32 tid) {
#if LITMUS_TEST == T_SB
    if (tid == 0) {
        plain_store(&x, 1);
        dmb_ish();
        seen[0][0] = plain_load(&y);
    } else {
        plain_store(&y, 1);
        dmb_ish();
        seen[1][0] = plain_load(&x);
    }
#elif LITMUS_TEST == T_MP
    if (tid == 0) {
        plain_store(&x, 1);
        dmb_ish();
        plain_store(&y, 1);
    } else {
        seen[1][0] = plain_load(&y);
        dmb_ish();
        seen[1][1] = plain_load(&x);
    }
#elif LITMUS_TEST == T_MP_REL
    if (tid == 0) {
        release_store(&x, 1);
        release_store(&y, 1);
    } else {
        seen[1][0] = acquire_load(&y);
        seen[1][1] = acquire_load(&x);
    }
#elif LITMUS_TEST == T_WRC
    if (tid == 0) {
        plain_store(&x, 1);
    } else if (tid == 1) {
        seen[1][0] = plain_load(&x);
        dmb_ish();
        plain_store(&y, 1);
    } else {
        seen[2][0] = plain_load(&y);
        dmb_ish();
        seen[2][1] = plain_load(&x);
    }
#elif LITMUS_TEST == T_SPIN
    if (tid == 0) {
        for (volatile u32 delay = 0; delay < 2000; delay++) {}
        plain_store(&x, 1); /* x is the flag here */
        dmb_ish();
    } else {
        while (plain_load(&x) == 0) {}
        seen[1][0] = 1;
    }
#else
    if (tid == 0) {
        plain_store(&x, 1);
        dmb_ishst();
        plain_store(&y, 1);
    } else {
        seen[1][0] = plain_load(&y);
        dmb_ishld();
        seen[1][1] = plain_load(&x);
    }
#endif
}

/* what the test's reads are, in the model's order: the names of the load lines of tests/litmus/ */
static void collect(void) {
#if LITMUS_TEST == T_SB
    values[0] = seen[0][0]; /* P0's load of y */
    values[1] = seen[1][0]; /* P1's load of x */
#elif LITMUS_TEST == T_MP || LITMUS_TEST == T_MP_REL
    values[0] = seen[1][0]; /* P1's load of y, the flag */
    values[1] = seen[1][1]; /* P1's load of x, the message */
#elif LITMUS_TEST == T_WRC
    values[0] = seen[1][0]; /* P1's load of x */
    values[1] = seen[2][0]; /* P2's load of y */
    values[2] = seen[2][1]; /* P2's load of x */
#elif LITMUS_TEST == T_SPIN
    values[0] = seen[1][0]; /* P1 left the spin loop */
#else
    values[0] = seen[1][0]; /* P1's load of y, the flag */
    values[1] = seen[1][1]; /* P1's load of x, the message */
#endif
}

static void tally(void) {
    u32 bits = 0;
    for (u32 i = 0; i < READERS; i++) {
        if (values[i] > 1) { /* a read the instruction cannot produce: counted apart, not folded into a bit */
            bad_values++;
            return; /* the attempt is counted once, in bad_values, so the counts still add up to the attempts */
        }
        bits |= (values[i] & 1u) << i;
    }
    histogram[bits]++;
}

static void reset(void) {
    plain_store(&x, 0);
    plain_store(&y, 0);
}

static u32 text_len;
static char text[96];
static void text_reset(const char* name) {
    text_len = 0;
    while (*name) text[text_len++] = *name++;
}
static void text_lit(const char* s) {
    text[text_len++] = ' ';
    while (*s) text[text_len++] = *s++;
}
static void text_u32(u32 v) {
    char digits[10];
    u32 n = 0;
    text[text_len++] = ' ';
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) text[text_len++] = digits[--n];
}
static void text_hex(u32 v) {
    static const char hex[] = "0123456789abcdef";
    text[text_len++] = ' ';
    for (int i = 28; i >= 0; i -= 4) text[text_len++] = hex[(v >> i) & 15];
}
static void text_flush(void) {
    text[text_len++] = '\n';
    call(64, 1, (u64)text, text_len);
}

static const char* const names[] = {"", "sb", "mp", "mp_rel_acq", "wrc", "spin", "mp_half"};

static int worker(u64 index) {
    u32 tid = (u32)index;
    for (u32 i = 0; i < ITERATIONS; i++) {
        sync_point(tid);
        body(tid);
        sync_point(tid);
        if (tid == 0) {
            collect();
            tally();
            reset();
        }
    }
    return 0;
}

int guest_main(u64* stack) {
    (void)stack;
    for (u32 i = 1; i < THREADS; i++) {
        ctid[i] = 0xffffffff;
        long tid = spawn_thread(THREAD_FLAGS, stacks[i] + sizeof stacks[i], &ptid[i], 0x7000 + i, &ctid[i], worker, i);
        CHECK(tid > 0 && (u32)tid == ptid[i]);
    }
    worker(0);
    for (u32 i = 1; i < THREADS; i++) join(&ctid[i]);
    /* one line per outcome the reads produced, then the count of attempts, then the reads that were not 0 or 1 */
    for (u32 bits = 0; bits < (1u << READERS); bits++) {
        if (!histogram[bits]) continue;
        text_reset("litmus");
        text_lit(names[LITMUS_TEST]);
        text_hex(bits);
        text_u32(histogram[bits]);
        text_flush();
    }
    text_reset("litmus");
    text_lit(names[LITMUS_TEST]);
    text_lit("done");
    text_u32(ITERATIONS);
    text_flush();
    if (bad_values) {
        text_reset("litmus");
        text_lit(names[LITMUS_TEST]);
        text_lit("bad");
        text_u32(bad_values);
        text_flush();
    }
    return 0;
}
