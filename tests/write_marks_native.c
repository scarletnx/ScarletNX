/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The write marks (the setting write_marks) as the native back end makes them (mach.h's sgx_mark): functions of a few
   words, each made with the marks and run on guest memory with the marks' bytes below it. Each page a store wrote has
   its mark and no other page has, but that the stores off the stack pointer mark with write_marks=2 alone; the stores'
   own bytes, the registers and the flags a branch reads after a store are what they were. Where the host has no native
   back end LLVM makes the functions, whose marks are the same (llvm_write_marks.c).
     snx-test-write-marks-native */
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#ifndef _WIN32
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#endif
#include <stdatomic.h>
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/runtime/interface.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void snx_ir_write_marks(int on);

#define PAGES 128 /* guest memory: [0, PAGES * 4 KiB) */
#define CODE  0x40000u
#define STOP  0u

static int g_failures, g_level;
static void fail(const char* what, int level, const char* why) {
    fprintf(stderr, "write_marks_native: %s at write_marks=%d: %s\n", what, level, why);
    ++g_failures;
}

typedef struct {
    const char* name;
    u32 words[40];
    unsigned nwords;
    unsigned pages[24]; /* those marked with write_marks=1 */
    unsigned npages;
    unsigned more[8]; /* and those with 2 besides: the stack's */
    unsigned nmore;
} Program;

/* add x9, x9, #0x123: a word the native back end is made to leave to LLVM (snx_native_without) where a program has it */
#define LEFT_WORD 0x91048d29u

/* the registers' values: a program reads what it needs of them */
static void start(CPU* c, u8* mem) {
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->x[0] = 0x1000;
    c->x[1] = 0x1122334455667788ull;
    c->x[2] = 0x99aabbccddeeff00ull;
    c->x[3] = 0x5555;
    c->x[4] = 0x8000;
    c->x[5] = 0x9000;
    c->x[6] = 0xb000;
    c->x[7] = 0x30000;
    c->x[10] = 0xc000;
    c->x[12] = 0xd000 + 5;
    c->x[13] = 0x5004;
    c->x[14] = 0x3000;
    c->x[15] = 0xa000;
    c->x[16] = 0x10;
    c->x[20] = 0xe000;
    c->x[21] = 0xf000;
    c->x[22] = 0x10000;
    c->x[23] = 0x11000;
    c->x[24] = 0x4444;
    c->x[25] = 0x5555aaaa;
    c->x[30] = STOP;
    c->x[31] = 0x3f000;
    memcpy(mem + 0xe000, &c->x[1], 8); /* CAS and CASP find what they compare, and store */
    memcpy(mem + 0xf000, &c->x[6], 8);
    memcpy(mem + 0xf008, &c->x[7], 8);
    c->v[0].d[0] = 0x0123456789abcdefull;
    c->v[0].d[1] = 0xfedcba9876543210ull;
    memset(mem + 0xd000, 0xee, 64);
}

#define RET 0xd65f03c0u
static const Program g_programs[] = {
    {"every kind of store",
     {
         0xa9bf7bfd, /* stp x29, x30, [sp, #-16]!  the stack: no mark */
         0xd10083ff, /* sub sp, sp, #32 */
         0xf90007e3, /* str x3, [sp, #8]           the stack: no mark */
         0xf9000001, /* str x1, [x0]               page 1 */
         0xa93f89c1, /* stp x1, x2, [x14, #-8]     pages 2 and 3 */
         0xf907fda1, /* str x1, [x13, #4088]       pages 5 and 6 */
         0x39000081, /* strb w1, [x4]              page 8 */
         0x3d8000a0, /* str q0, [x5]               page 9 */
         0xb83069e1, /* str w1, [x15, x16]         page 10 */
         0xc89ffcc1, /* stlr x1, [x6]              page 11 */
         0xc85f7d49, /* ldxr x9, [x10] */
         0xc80b7d41, /* stxr w11, x1, [x10]        page 12 */
         0xd50b742c, /* dc zva, x12                page 13 */
         0xc8a17e82, /* cas x1, x2, [x20]          page 14 */
         0x48267eb8, /* casp x6, x7, x24, x25, [x21] page 15 */
         0xf82182c9, /* swp x1, x9, [x22]          page 16 */
         0xf82102e9, /* ldadd x1, x9, [x23]        page 17 */
         0x910083ff, /* add sp, sp, #32 */
         0xa8c17bfd, /* ldp x29, x30, [sp], #16 */
         RET,
     },
     20,
     {1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
     15,
     {0x3e},
     1},
    /* a span of 4,096 bytes across two pages, then one of 4,104 (too wide for a group) and more bases than a region keeps */
    {"groups",
     {
         0xf9000001, /* str x1, [x0]                page 1 */
         0xf9080001, /* str x1, [x0, #4096]         page 2 */
         0xf90000e1, /* str x1, [x7]                page 0x30 */
         0xf907fce1, /* str x1, [x7, #4088]         page 0x30, to its end */
         0xf9080ce1, /* str x1, [x7, #4120]         page 0x31 */
         0xf9000081, /* str x1, [x4]                page 8 */
         0xf90000a1, /* str x1, [x5]                page 9 */
         0xf90000c1, /* str x1, [x6]                page 11 */
         0xf9000141, /* str x1, [x10]               page 12 */
         0xf90001c1, /* str x1, [x14]               page 3 */
         0xf9000281, /* str x1, [x20]               page 14 */
         0xf90002a1, /* str x1, [x21]               page 15 */
         0xf90002c1, /* str x1, [x22]               page 16 */
         0xf90002e1, /* str x1, [x23]               page 17 */
         RET,
     },
     15,
     {1, 2, 3, 8, 9, 11, 12, 14, 15, 16, 17, 0x30, 0x31},
     13,
     {0},
     0},
    /* one byte of each of three pages from one base: a group of 4,098 bytes is too wide, as its middle page has no mark of
       its first or its last byte */
    {"bytes of three pages",
     {
         0x913ffc09, /* add x9, x0, #0xfff          0x1fff */
         0x9140080b, /* add x11, x0, #2, lsl #12    0x3000 */
         0x39000121, /* strb w1, [x9]               page 1 */
         0x39000521, /* strb w1, [x9, #1]           page 2 */
         0x39000161, /* strb w1, [x11]              page 3 */
         RET,
     },
     6,
     {1, 2, 3},
     3,
     {0},
     0},
    /* the last byte of a page and the first of the next: one byte on each */
    {"a halfword across two pages",
     {
         0x913ffc09, /* add x9, x0, #0xfff          0x1fff */
         0x79000121, /* strh w1, [x9]               pages 1 and 2 */
         RET,
     },
     3,
     {1, 2},
     2,
     {0},
     0},
    /* a store past the end of a page, and one at its last bytes */
    {"page boundaries",
     {
         0xf803f0e1, /* stur x1, [x7, #0x3f]        page 0x30 */
         0x91040529, /* (unused) add x9, x9, #0x100 */
         0xd1000400, /* sub x0, x0, #1: 0xfff */
         0x39000001, /* strb w1, [x0]               page 0 */
         0x91000400, /* add x0, x0, #1: 0x1000 */
         0x39000001, /* strb w1, [x0]               page 1 */
         0xd1001000, /* sub x0, x0, #4: 0xffc */
         0xf9000001, /* str x1, [x0]                pages 0 and 1 */
         RET,
     },
     9,
     {0, 1, 0x30},
     3,
     {0},
     0},
    /* a store after a branch that skips it: the pages of what runs alone */
    {"a skipped store",
     {
         0xb4000042, /* cbz x2, +8: x2 is not 0 */
         0xf9000001, /* str x1, [x0]                page 1 */
         0xb5000041, /* cbnz x1, +8: taken */
         0xf9000081, /* str x1, [x4]                not run */
         0xf90000a1, /* str x1, [x5]                page 9 */
         RET,
     },
     6,
     {1, 9},
     2,
     {0},
     0},
    /* a compare kept across a store (the marks' shifts may write the host's flags): the branch takes what the compare
       said */
    {"a compare across a store",
     {
         0xeb02003f, /* cmp x1, x2: not equal */
         0xf9000001, /* str x1, [x0]                page 1 */
         0x54000040, /* b.eq +8: not taken */
         0xf9000081, /* str x1, [x4]                page 8 */
         0xeb00003f, /* cmp x1, x0 */
         0xa90004a1, /* stp x1, x1, [x5]            page 9 */
         0x54000048, /* b.hi +8 (unsigned higher): taken */
         0xf90000c1, /* str x1, [x6]                not run */
         0xaa0103e9, /* mov x9, x1 */
         RET,
     },
     10,
     {1, 8, 9},
     3,
     {0},
     0},
    /* a store before a branch that is taken: its mark is made on the way the branch goes, not only on the other */
    {"a store before a taken branch",
     {
         0xf9000001, /* str x1, [x0]                page 1 */
         0xb4000041, /* cbz x1, +8: x1 is not 0: not taken */
         0xf9000081, /* str x1, [x4]                page 8 */
         0xb5000041, /* cbnz x1, +8: taken */
         0xf90000a1, /* str x1, [x5]                not run */
         RET,
     },
     6,
     {1, 8},
     2,
     {0},
     0},
    /* the marks before a call (an SVC, whose handler sees them) and before a BLR */
    {"a call sees the marks",
     {
         0xf9000001, /* str x1, [x0]                page 1 */
         0xd4000021, /* svc #1 */
         0xf9000081, /* str x1, [x4]                page 8 */
         0xd4000101, /* svc #8 */
         0xf90000a1, /* str x1, [x5]                page 9 */
         0xd4000121, /* svc #9 */
         RET,
     },
     7,
     {1, 8, 9},
     3,
     {0},
     0},
    /* a function the native back end leaves to LLVM halfway, with a store before: what it noted is not the next one's */
    {"a function left to LLVM after a store",
     {
         0xaa1e03fd, /* mov x29, x30 */
         0x94000005, /* bl the next one */
         0xf9000001, /* str x1, [x0]                page 1 */
         LEFT_WORD,
         0xaa1d03fe, /* mov x30, x29 */
         RET,
         0xf9000081, /* str x1, [x4]                page 8 */
         RET,
     },
     8,
     {1, 8},
     2,
     {0},
     0},
    /* the marks before an undefined instruction, which the runtime's handler sees */
    {"an undefined instruction sees the marks",
     {
         0xf9000001, /* str x1, [x0]                page 1 */
         0x00000001, /* udf #1 */
         RET,
     },
     3,
     {1},
     1,
     {0},
     0},
    /* a loop over 4 pages, the store post-indexed */
    {"a loop",
     {
         0xd2820000, /* mov x0, #0x1000 */
         0xf80086e1, /* str x1, [x23], #8           pages 0x11 to 0x18 */
         0xd1000400, /* sub x0, x0, #1 */
         0xb5ffffc0, /* cbnz x0, -8 */
         RET,
     },
     5,
     {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18},
     8,
     {0},
     0},
};

/* CPU.disp and the rest for the recompiler's run, as snx_recompiled_run wants them */
static const u8* g_marks;
static int g_udfs;
static void undef(CPU* c, u64 pc, u32 word) {
    if (word >> 16 || (word & 0xFFFF) >= PAGES) {
        fprintf(stderr, "write_marks_native: undefined %08x at %llx\n", word, (unsigned long long)pc);
        ++g_failures;
    } else {
        /* UDF #page: the stores before it are marked by then: what the runtime does with it sees them */
        ++g_udfs;
        if (g_level && !g_marks[word & 0xFFFF]) {
            fprintf(stderr, "write_marks_native: at UDF #%u the mark of that page is not set yet\n", word & 0xFFFFu);
            ++g_failures;
        }
    }
    c->pc = STOP;
}
/* SVC #page: the stores before it are marked by then (what a call may make visible comes after their marks) */
static int g_svcs;
static const u32 g_left_word = LEFT_WORD;
static void svc(CPU* c, u32 imm) {
    (void)c;
    ++g_svcs;
    if (g_level && imm < PAGES && !g_marks[imm]) {
        fprintf(stderr, "write_marks_native: at SVC #%u the mark of that page is not set yet\n", imm);
        ++g_failures;
    }
}
static void brk(CPU* c, u64 pc, u32 imm) { (void)c, (void)pc, (void)imm; }

static void run(const Program* p, int level, RtAPI* rt) {
    struct snx_recompiler* r;
    struct snx_recompiled* m;
    u8 *block, *mem, *marks;
    u8 code[sizeof p->words];
    CPU c;
    unsigned k, pg, svcs = 0, udfs = 0;
    int left = 0;
    char what[160];
    snx_ir_write_marks(level);
    r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!r) {
        fail(p->name, level, "no recompiler");
        return;
    }
    (void)snx_recompiler_set_a64_target(r, SNX_RECOMPILER_A64_ANDROID); /* LSE */
    for (k = 0; k < p->nwords; ++k) {
        snx_st_le32(code + 4 * k, p->words[k]);
        svcs += (p->words[k] & 0xFFE0001Fu) == 0xD4000001u;
        udfs += p->words[k] < PAGES; /* UDF #page: a word of its own */
        if (p->words[k] == LEFT_WORD) left = 1, snx_native_without(r, &g_left_word, 1);
    }
    m = snx_recompile(r, CODE, code, 4 * p->nwords);
    block = (u8*)calloc(1, SNX_WMARK_BELOW + PAGES * 4096);
    if (!m || !block) {
        fail(p->name, level, m ? "no memory" : snx_recompiler_error(r));
        goto done;
    }
    if (!left && snx_x64_available() && snx_recompiled_x64_functions(m) != snx_recompiled_functions(m))
        fail(p->name, level, "the native back end did not make the function");
    mem = block + SNX_WMARK_BELOW, marks = block;
    g_marks = marks, g_level = level, g_svcs = 0, g_udfs = 0;
    start(&c, mem);
    c.rt = rt;
    if (snx_recompiled_bind(m, &c)) {
        fail(p->name, level, "bind");
        goto done;
    }
    snx_recompiled_run(m, &c, CODE, STOP);
    if (svcs && !g_svcs) fail(p->name, level, "the SVC did not run");
    if (udfs != (unsigned)g_udfs) fail(p->name, level, "the UDF did not run once");
    for (pg = 0; pg < PAGES; ++pg) {
        int want = 0;
        for (k = 0; level && k < p->npages; ++k) want |= p->pages[k] == pg;
        for (k = 0; level > 1 && k < p->nmore; ++k) want |= p->more[k] == pg;
        if (marks[pg] != want) {
            snprintf(what, sizeof what, "page %#x's mark is %u, expected %d", pg, marks[pg], want);
            fail(p->name, level, what);
        }
    }
    for (u64 i = PAGES; i < SNX_WMARK_BELOW; ++i)
        if (marks[i]) {
            snprintf(what, sizeof what, "the mark %llu, past guest memory, is set", (unsigned long long)i);
            fail(p->name, level, what);
            break;
        }
    if (c.x[31] != 0x3f000) fail(p->name, level, "the stack pointer at the end");
done:
    free(block);
    snx_recompiled_free(m);
    snx_recompiler_free(r);
}

/* ---- publication: the marks are made before what makes the stores visible ----
   A guest loop stores to a page and then, by each kind of store that publishes, sets a flag in another page; a host thread
   waits for the flag (the guest waits for it to be cleared) and looks at once for the page's mark: a mark made after the
   flag would be missed. The thread takes the mark and clears the flag, and the loop goes on */
#define PUB_ROUNDS 20000
typedef struct {
    const char* name;
    u32 pre;      /* a word before the store to page 1, 0 for none */
    u32 words[3]; /* those that set the flag (x2, 1) at x3: x8 is 0, x9 and w10 are theirs */
    unsigned nwords;
} Publication;
static const Publication g_publications[] = {
    {"stlr x2, [x3]", 0, {0xc89ffc62}, 1},
    {"swp x2, x9, [x3]", 0, {0xf8228069}, 1},
    {"ldadd x2, x9, [x3]", 0, {0xf8220069}, 1},
    {"cas x8, x2, [x3]", 0, {0xc8a87c62}, 1},
    {"dmb ish; str x2, [x3]", 0, {0xd5033bbf, 0xf9000062}, 2},
    {"ldxr, stxr", 0, {0xc85f7c69, 0xc80a7c62, 0x35ffffca}, 3},
    {"ldxr before the store, stxr", 0xc85f7c69, {0xc80a7c62, 0x35ffffaa}, 2},
};
typedef struct {
    _Atomic u64* flag;
    _Atomic u8* mark;
    int lost, rounds;
    _Atomic int done;
} Pub;
static _Atomic int g_pub_stop;
static void pub_watch(void* arg) {
    Pub* p = (Pub*)arg;
    for (; p->rounds < PUB_ROUNDS; ++p->rounds) {
        unsigned spin = 0;
        while (!atomic_load_explicit(p->flag, memory_order_acquire)) {
            if (++spin % 1000 == 0) plat_yield();
            if (atomic_load_explicit(&g_pub_stop, memory_order_relaxed)) goto out;
        }
        if (!atomic_load_explicit(p->mark, memory_order_relaxed)) ++p->lost;
        atomic_store_explicit(p->mark, 0, memory_order_relaxed);
        atomic_store_explicit(p->flag, 0, memory_order_release);
    }
out:
    atomic_store_explicit(&p->done, 1, memory_order_release);
}
static void publication(const Publication* k, RtAPI* rt) {
    struct snx_recompiler* r;
    struct snx_recompiled* m = NULL;
    u8 code[16 * 4], *block;
    u32 words[16];
    unsigned n = 0, at, i;
    Pub watch = {0};
    CPU c;
    char what[96];
    if (k->pre) words[n++] = k->pre;
    words[n++] = 0xf9000001; /* str x1, [x0]: page 1 */
    for (i = 0; i < k->nwords; ++i) words[n++] = k->words[i];
    words[n++] = 0xc8dffc65;                                 /* ldar x5, [x3] */
    words[n++] = 0xb5ffffe5;                                 /* cbnz x5, -4 */
    words[n++] = 0xd1000484;                                 /* sub x4, x4, #1 */
    words[n] = 0xb5000004 | (u32)((0u - n) & 0x7FFFFu) << 5; /* cbnz x4, the first word */
    words[++n] = RET;
    ++n;
    for (i = 0; i < n; ++i) snx_st_le32(code + 4 * i, words[i]);
    snx_ir_write_marks(1);
    r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    block = (u8*)calloc(1, SNX_WMARK_BELOW + PAGES * 4096);
    if (!r || !block) {
        fail(k->name, 1, "no recompiler or memory");
        goto done;
    }
    (void)snx_recompiler_set_a64_target(r, SNX_RECOMPILER_A64_ANDROID);
    m = snx_recompile(r, CODE, code, 4 * n);
    if (!m) {
        fail(k->name, 1, snx_recompiler_error(r));
        goto done;
    }
    memset(&c, 0, sizeof c);
    c.mem = block + SNX_WMARK_BELOW;
    c.rt = rt;
    c.x[0] = 0x1000;
    c.x[1] = 0x1122334455667788ull;
    c.x[2] = 1;
    c.x[3] = 0x2000;
    c.x[4] = PUB_ROUNDS;
    c.x[30] = STOP;
    watch.flag = (_Atomic u64*)(void*)(c.mem + 0x2000);
    watch.mark = (_Atomic u8*)(block + 1);
    if (snx_recompiled_bind(m, &c)) {
        fail(k->name, 1, "bind");
        goto done;
    }
    if (plat_thread(pub_watch, &watch, 1 << 18)) {
        fail(k->name, 1, "no thread");
        goto done;
    }
    snx_recompiled_run(m, &c, CODE, STOP);
    for (at = 0; at < 100000 && !atomic_load_explicit(&watch.done, memory_order_acquire); ++at) plat_yield();
    atomic_store_explicit(&g_pub_stop, 1, memory_order_relaxed);
    while (!atomic_load_explicit(&watch.done, memory_order_acquire)) plat_yield();
    atomic_store_explicit(&g_pub_stop, 0, memory_order_relaxed);
    if (watch.rounds != PUB_ROUNDS) {
        snprintf(what, sizeof what, "%d of %d rounds seen", watch.rounds, PUB_ROUNDS);
        fail(k->name, 1, what);
    }
    if (watch.lost) {
        snprintf(what, sizeof what, "the mark was not set at %d of %d flags", watch.lost, PUB_ROUNDS);
        fail(k->name, 1, what);
    }
done:
    free(block);
    snx_recompiled_free(m);
    snx_recompiler_free(r);
}

#ifndef _WIN32
/* ---- the marks before the store that faults ----
   The publishing store of each of the programs above writes a page that cannot be written, so it faults: by then the mark
   of the store before it is set, or the fault's thread sees the store without its mark */
static sigjmp_buf g_fault_jump;
static const u8* g_fault_mark;
static volatile sig_atomic_t g_fault_marked, g_faulted;
static void on_fault(int sig, siginfo_t* si, void* context) {
    (void)sig, (void)si, (void)context;
    g_faulted = 1;
    g_fault_marked = *g_fault_mark != 0;
    siglongjmp(g_fault_jump, 1);
}
static void faults(const Publication* k, RtAPI* rt) {
    const size_t size = SNX_WMARK_BELOW + 0x40000;
    struct snx_recompiler* r = NULL;
    struct snx_recompiled* m = NULL;
    struct sigaction act, old[2];
    u8 code[16 * 4];
    u32 words[16];
    unsigned n = 0, i;
    CPU c;
    u8* block = (u8*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) {
        fail(k->name, 1, "no memory");
        return;
    }
    if (k->pre) words[n++] = k->pre;
    words[n++] = 0xf9000001; /* str x1, [x0]: page 1 */
    for (i = 0; i < k->nwords; ++i) words[n++] = k->words[i];
    words[n++] = RET;
    for (i = 0; i < n; ++i) snx_st_le32(code + 4 * i, words[i]);
    snx_ir_write_marks(1);
    r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!r) goto done;
    (void)snx_recompiler_set_a64_target(r, SNX_RECOMPILER_A64_ANDROID);
    m = snx_recompile(r, CODE, code, 4 * n);
    if (!m) goto done;
    memset(&c, 0, sizeof c);
    c.mem = block + SNX_WMARK_BELOW;
    c.rt = rt;
    c.x[0] = 0x1000;
    c.x[1] = 0x1122334455667788ull;
    c.x[2] = 1;
    c.x[3] = 0x20000; /* a multiple of any host's page */
    c.x[30] = STOP;
    g_fault_mark = block + 1;
    g_faulted = g_fault_marked = 0;
    if (mprotect(c.mem + 0x20000, 0x20000, PROT_READ) || snx_recompiled_bind(m, &c)) goto done;
    memset(&act, 0, sizeof act);
    act.sa_sigaction = on_fault;
    act.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGSEGV, &act, &old[0]) || sigaction(SIGBUS, &act, &old[1])) goto done; /* macOS' is SIGBUS */
    if (!sigsetjmp(g_fault_jump, 1)) snx_recompiled_run(m, &c, CODE, STOP);
    sigaction(SIGSEGV, &old[0], NULL);
    sigaction(SIGBUS, &old[1], NULL);
    if (!g_faulted)
        fail(k->name, 1, "the store that publishes did not fault");
    else if (!g_fault_marked)
        fail(k->name, 1, "the store that publishes faulted before the mark of the store before it was set");
    munmap(block, size);
    snx_recompiled_free(m);
    snx_recompiler_free(r);
    return;
done:
    fail(k->name, 1, "no recompiler, module or protection");
    munmap(block, size);
    snx_recompiled_free(m);
    snx_recompiler_free(r);
}
#endif

int main(void) {
    RtAPI rt;
    snx_runtime_defaults(&rt);
    rt.undef = undef, rt.svc = svc, rt.brk = brk;
    for (int level = 0; level <= 2; ++level)
        for (size_t i = 0; i < sizeof g_programs / sizeof *g_programs; ++i) run(&g_programs[i], level, &rt);
    for (size_t i = 0; i < sizeof g_publications / sizeof *g_publications; ++i) publication(&g_publications[i], &rt);
#ifndef _WIN32
    for (size_t i = 0; i < sizeof g_publications / sizeof *g_publications; ++i) faults(&g_publications[i], &rt);
#endif
    if (g_failures) {
        fprintf(stderr, "write_marks_native: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("write_marks_native: the %s back end marked the pages its stores wrote and no others\n",
           snx_x64_available() ? "x86-64" : "host's");
    return 0;
}
