/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's code on Windows unwinds: functions it made (a leaf, one that calls, one whose registers are
   pinned with the vector ones, a frame of more than a page, an undefined word, a loop's poll, a jump and calls through
   the dispatch table, one of them no function's) are loaded as the runtime JIT's are, their function table registered
   with RtlAddFunctionTable, and run one instruction at a time (the trap flag). At each instruction of theirs
   RtlLookupFunctionEntry and RtlVirtualUnwind, the system's own, unwind the stack to the test's entry, where every
   register a callee keeps must be what the entry marked, in the middle of a prologue and of an epilogue too; from the
   runtime's functions they call the stack unwinds the same way, and a longjmp out of one crosses their frames.

   On another x86-64 host it is the back end's code for Windows that runs, and support/host_win64.c's unwinder, which does
   what RtlVirtualUnwind does with the same table; there is no longjmp */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/runtime/interface.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "support/host_unicorn.h"
#endif

#define BASE  0x10000u
#define WORDS 2048u
#define BIG   1400u /* the long function's additions: each value is read again far on, so it is kept in the frame */
#define STOP  0u
#define GUEST 0x40000u /* the guest's memory, its stack in the middle */

/* the functions, by their first word */
enum {
    F_LEAF = 0,
    F_CALLS = 4,
    F_DIRECT = 12, /* its one call is another function of the unit's */
    F_PINNED = 16,
    F_UNDEF = 36,
    F_POLL = 40,
    F_JUMP = 48,
    F_FLUSHED = 52,
    F_FAR = 56, /* a leaf whose place in the dispatch table is not the first */
    F_MISS = 58,
    F_BIG = 64
};

static struct {
    uint32_t words[WORDS];
    unsigned char* guest;
    CPU* cpu;
    RtAPI* rt;
    SNX_ATOMIC(GuestFn) * disp;
    unsigned svcs, undefs, polls, misses;
    size_t failures;
#if defined(_WIN32)
    size_t unwound, bad;
    unsigned function;      /* the one that runs */
    unsigned char *lo, *hi; /* the loaded code's bounds, by the functions' addresses */
    jmp_buf out;
    int jump;
#endif
} g_test;

static void failed(const char* what, unsigned function) {
    ++g_test.failures;
    fprintf(stderr, "FAIL: the function at word %u: %s\n", function, what);
}

/* the helpers the functions' cold paths call (x64_helpers.c) */
#define HELPERS(X) X(snx_x64_fp_binary) X(snx_x64_fp_fma) X(snx_x64_fp_unary) X(snx_x64_fp_minmax) X(snx_x64_fp_compare)
u64 snx_x64_fp_binary(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_fma(CPU* c, u64 add, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_unary(CPU* c, u64 a, u64 fpcr, u32 desc);
u64 snx_x64_fp_minmax(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);
u64 snx_x64_fp_compare(CPU* c, u64 a, u64 b, u64 fpcr, u32 desc);

static void program(void) {
    uint32_t* w = g_test.words;
    unsigned k;
    for (k = 0; k < WORDS; ++k) w[k] = 0xD65F03C0u; /* RET */
    w[F_LEAF] = 0x91000400u;                        /* ADD X0, X0, #1 */
    {
        /* STP X29, X30, [SP, #-16]!; BL leaf; ADR X9, the far leaf; BLR X9; SVC #1; LDP X29, X30, [SP], #16; RET */
        const uint32_t calls[] = {0xA9BF7BFDu,
                                  0x94000000u | ((unsigned)(F_LEAF - (F_CALLS + 1)) & 0x3FFFFFFu),
                                  0x10000000u | (unsigned)(F_FAR - (F_CALLS + 2)) << 5 | 9u,
                                  0xD63F0120u,
                                  0xD4000021u,
                                  0xA8C17BFDu,
                                  0xD65F03C0u};
        memcpy(w + F_CALLS, calls, sizeof calls);
    }
    {
        /* STP X29, X30, [SP, #-16]!; BL leaf; LDP X29, X30, [SP], #16; RET */
        const uint32_t direct[] = {0xA9BF7BFDu, 0x94000000u | ((unsigned)(F_LEAF - (F_DIRECT + 1)) & 0x3FFFFFFu),
                                   0xA8C17BFDu, 0xD65F03C0u};
        memcpy(w + F_DIRECT, direct, sizeof direct);
    }
    {
        /* MOVZ X16, #3; a loop of scalar floating point, whose registers it keeps in host ones; SVC #2 in it; RET */
        const uint32_t pinned[] = {0xD2800070u, 0x1E210800u, 0x1F010802u, 0x1E232843u, 0x1E653884u,
                                   0x1E61C086u, 0x1E274867u, 0x1E2020E0u, 0x9A83B041u, 0xD4000041u,
                                   0x91000442u, 0x8B020063u, 0xF1000610u, 0x54FFFE81u, 0xD65F03C0u};
        memcpy(w + F_PINNED, pinned, sizeof pinned);
    }
    w[F_UNDEF] = 0x91000400u, w[F_UNDEF + 1] = 0x00000000u; /* ADD, UDF */
    /* MOVZ X0, #3; SUBS X0, X0, #1; B.NE back to it; RET: the poll at the branch */
    w[F_POLL] = 0xD2800060u, w[F_POLL + 1] = 0xF1000400u, w[F_POLL + 2] = 0x54FFFFE1u;
    /* ADR X9, the far leaf; BR X9 */
    w[F_JUMP] = 0x10000000u | (unsigned)(F_FAR - F_JUMP) << 5 | 9u, w[F_JUMP + 1] = 0xD61F0120u;
    w[F_FAR] = 0x91000400u; /* ADD X0, X0, #1 */
    /* ADR X9, two bytes into the far leaf; BLR X9, its one call: no function's, the runtime's miss */
    w[F_MISS] = 0x50000000u | ((unsigned)(F_FAR - F_MISS) & 0x7FFFFu) << 5 | 9u, w[F_MISS + 1] = 0xD63F0120u;
    /* FADD S0, S1, S2, FMUL S3, S0, S1 of denormals FPCR.FZ flushes: the cold paths and their helper */
    w[F_FLUSHED] = 0x1E222820u, w[F_FLUSHED + 1] = 0x1E210803u;
    /* ADD Xd, Xd, Xn, d a step of 29 registers and n one far from it, then SVC #3 with the frame up */
    for (k = 0; k < BIG; ++k) {
        unsigned d = (k * 7) % 29, n = (k * 11 + 13) % 29;
        w[F_BIG + k] = 0x8B000000u | n << 16 | d << 5 | d;
    }
    w[F_BIG + BIG] = 0xD4000061u;
}

/* ---- the host: how the code is entered, watched and what it calls ---- */

static void on_svc(CPU* c, u32 number);
static void on_undef(CPU* c, u64 pc, u32 word) {
    (void)word;
    ++g_test.undefs;
    c->pc = pc + 4;
}
static void on_poll(CPU* c) {
    ++g_test.polls;
    c->poll = 0;
}
static void on_miss(CPU* c, u64 pc) {
    ++g_test.misses;
    c->pc = pc;
}

#if defined(_WIN32)
/* what entry() marks before the call and the unwinder must find at it: RBX, RBP, RSI, RDI, R12 to R15, XMM6 to XMM15;
   the call's stack pointer and return address */
struct Marks {
    uint64_t mark[8];      /* 0 */
    uint64_t xmark[10][2]; /* 64 */
    uint64_t sp, ret;      /* 224, 232 */
    uint64_t step;         /* 240: the trap flag through the call */
};
struct Marks snx_test_marks;
static_assert(offsetof(struct Marks, xmark) == 64 && offsetof(struct Marks, sp) == 224 &&
                  offsetof(struct Marks, step) == 240,
              "entry's bytes reach the marks at their offsets");
/* entry(fn RCX, state RDX, pc R8): fn(state, pc) */
[[gnu::naked]] static void entry(GuestFn fn, CPU* c, u64 pc) {
    __asm__(
        "pushq %rbx\n\tpushq %rbp\n\tpushq %rsi\n\tpushq %rdi\n\tpushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
        "subq $200, %rsp\n\t"
        "movups %xmm6, 32(%rsp)\n\tmovups %xmm7, 48(%rsp)\n\tmovups %xmm8, 64(%rsp)\n\tmovups %xmm9, 80(%rsp)\n\t"
        "movups %xmm10, 96(%rsp)\n\tmovups %xmm11, 112(%rsp)\n\tmovups %xmm12, 128(%rsp)\n\t"
        "movups %xmm13, 144(%rsp)\n\tmovups %xmm14, 160(%rsp)\n\tmovups %xmm15, 176(%rsp)\n\t"
        "movq %rcx, %rax\n\tmovq %rdx, %rcx\n\tmovq %r8, %rdx\n\t"
        "movq snx_test_marks+0(%rip), %rbx\n\tmovq snx_test_marks+8(%rip), %rbp\n\t"
        "movq snx_test_marks+16(%rip), %rsi\n\tmovq snx_test_marks+24(%rip), %rdi\n\t"
        "movq snx_test_marks+32(%rip), %r12\n\tmovq snx_test_marks+40(%rip), %r13\n\t"
        "movq snx_test_marks+48(%rip), %r14\n\tmovq snx_test_marks+56(%rip), %r15\n\t"
        "movups snx_test_marks+64(%rip), %xmm6\n\tmovups snx_test_marks+80(%rip), %xmm7\n\t"
        "movups snx_test_marks+96(%rip), %xmm8\n\tmovups snx_test_marks+112(%rip), %xmm9\n\t"
        "movups snx_test_marks+128(%rip), %xmm10\n\tmovups snx_test_marks+144(%rip), %xmm11\n\t"
        "movups snx_test_marks+160(%rip), %xmm12\n\tmovups snx_test_marks+176(%rip), %xmm13\n\t"
        "movups snx_test_marks+192(%rip), %xmm14\n\tmovups snx_test_marks+208(%rip), %xmm15\n\t"
        "movq %rsp, snx_test_marks+224(%rip)\n\t"
        "leaq 2f(%rip), %r8\n\tmovq %r8, snx_test_marks+232(%rip)\n\t"
        "cmpq $0, snx_test_marks+240(%rip)\n\tje 1f\n\t"
        "pushfq\n\torq $0x100, (%rsp)\n\tpopfq\n"
        "1:\n\tcallq *%rax\n"
        "2:\n\tmovq $0, snx_test_marks+240(%rip)\n\t" /* the next step is the last */
        "movups 32(%rsp), %xmm6\n\tmovups 48(%rsp), %xmm7\n\tmovups 64(%rsp), %xmm8\n\tmovups 80(%rsp), %xmm9\n\t"
        "movups 96(%rsp), %xmm10\n\tmovups 112(%rsp), %xmm11\n\tmovups 128(%rsp), %xmm12\n\t"
        "movups 144(%rsp), %xmm13\n\tmovups 160(%rsp), %xmm14\n\tmovups 176(%rsp), %xmm15\n\t"
        "addq $200, %rsp\n\t"
        "popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\tpopq %rdi\n\tpopq %rsi\n\tpopq %rbp\n\tpopq %rbx\n\tretq");
}
/* the state c unwound by the system to entry's frame: what is wrong with where it came to, NULL for nothing */
static const char* unwinds(CONTEXT c) {
    const struct Marks* m = &snx_test_marks;
    unsigned frames = 0, k;
    ++g_test.unwound;
    while (c.Rip != m->ret) {
        DWORD64 image = 0, frame = 0;
        void* data;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, NULL);
        if (++frames > 64) return "never reached the entry";
        if (c.Rsp > m->sp) return "a stack pointer past the entry's";
        if (f)
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &data, &frame, NULL);
        else { /* a leaf's */
            c.Rip = *(DWORD64*)(uintptr_t)c.Rsp;
            c.Rsp += 8;
        }
    }
    if (c.Rsp != m->sp) return "a stack pointer not the entry's";
    {
        const uint64_t got[8] = {c.Rbx, c.Rbp, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15};
        for (k = 0; k < 8; ++k)
            if (got[k] != m->mark[k]) return "a general-purpose register not its caller's";
    }
    for (k = 0; k < 10; ++k)
        if (memcmp(&(&c.Xmm6)[k], m->xmark[k], 16)) return "an XMM register not its caller's";
    return NULL;
}
static void unwound(const CONTEXT* c, const char* where) {
    const char* why = unwinds(*c);
    if (!why) return;
    if (++g_test.bad <= 20)
        fprintf(stderr, "unwound from %s, +%llx of the code (the function at word %u): %s\n", where,
                (unsigned long long)(c->Rip - (DWORD64)(uintptr_t)g_test.lo), g_test.function, why);
}
static LONG WINAPI on_step(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (e->ContextRecord->Rip >= (DWORD64)(uintptr_t)g_test.lo && e->ContextRecord->Rip < (DWORD64)(uintptr_t)g_test.hi)
        unwound(e->ContextRecord, "an instruction");
    if (snx_test_marks.step) e->ContextRecord->EFlags |= 0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}
/* a function the code calls: the stack from here, through the code's frames, to the entry; or out, across them */
static void on_svc(CPU* c, u32 number) {
    ++g_test.svcs;
    (void)c, (void)number;
    if (g_test.jump) longjmp(g_test.out, 7);
    CONTEXT here;
    RtlCaptureContext(&here);
    unwound(&here, "a call");
}
#else
static void on_svc(CPU* c, u32 number) {
    (void)c, (void)number;
    ++g_test.svcs;
}
#endif

/* ---- the runs ---- */

static void start(unsigned function) {
    CPU* c = g_test.cpu;
    unsigned k;
    memset(c, 0, sizeof *c);
    for (k = 0; k < 31; ++k) c->x[k] = 0x0101010101010101ull * k;
    for (k = 0; k < 32; ++k) c->v[k].d[0] = 0x3F8000003F800000ull + k, c->v[k].d[1] = k;
    if (function == F_FLUSHED) {
        c->fpcr = 1u << 24;                                     /* FZ */
        c->v[1].d[0] = 0x00000001u, c->v[2].d[0] = 0x00000002u; /* denormals */
    }
    c->x[31] = GUEST / 2; /* SP */
    c->x[30] = STOP;
    c->mem = g_test.guest;
    c->rt = g_test.rt;
    c->disp = g_test.disp;
    c->code_lo = BASE;
    c->code_hi = BASE + sizeof g_test.words;
    c->poll = function == F_POLL;
    c->pc = BASE + 4 * function;
}
/* what the function at word function must have done: its state's, as the words say */
static void check(unsigned function) {
    const CPU* c = g_test.cpu;
    switch (function) {
        case F_LEAF:
        case F_FAR:
        case F_DIRECT:
        case F_JUMP:
            if (c->x[0] != 1) failed("X0 not the leaf's", function);
            break;
        case F_MISS:
            if (g_test.misses != 1 || c->pc != BASE + 4 * F_FAR + 2 || c->x[0])
                failed("its call into an instruction not the runtime's miss", function);
            break;
        case F_CALLS:
            if (c->x[0] != 2 || !g_test.svcs) failed("its calls not made", function);
            break;
        case F_PINNED:
            if (g_test.svcs != 3 || c->x[16]) failed("its loop not run three times", function);
            break;
        case F_UNDEF:
            if (g_test.undefs != 1) failed("the undefined word not raised once", function);
            break;
        case F_POLL:
            if (g_test.polls != 1 || c->x[0]) failed("its loop or its poll not run", function);
            break;
        case F_FLUSHED:
            if (!(c->fpsr & 0x80)) failed("the denormals not flushed (FPSR.IDC)", function);
            break;
        default: {
            uint64_t x[29];
            unsigned k, bad = 0;
            for (k = 0; k < 29; ++k) x[k] = 0x0101010101010101ull * k;
            for (k = 0; k < BIG; ++k) x[(k * 7) % 29] += x[(k * 11 + 13) % 29];
            for (k = 0; k < 29; ++k) bad += c->x[k] != x[k];
            if (bad || g_test.svcs != 1) failed("its sums", function);
        }
    }
}

int main(void) {
    static const unsigned functions[] = {F_LEAF,    F_CALLS, F_PINNED, F_UNDEF,  F_POLL, F_JUMP,
                                         F_FLUSHED, F_FAR,   F_MISS,   F_DIRECT, F_BIG};
    enum { COUNT = sizeof functions / sizeof *functions };
    SnxCodegen codegen;
    SnxCodeImage image = {0};
    SnxNativeOptions options = {.jit = 1, .table_name = "UNWIND_TABLE"};
    SnxU64 hints[COUNT];
    unsigned char *made, *object = NULL;
    const uint64_t* table;
    size_t size = 0, unwound_count = 0, bad = 0, k;
    char error[512] = "";
    if (!snx_x64_available()) {
        printf("x64 unwind: no x86-64 back end on this host\n");
        return 77;
    }
    program();
#if !defined(_WIN32)
    snx_x64_set_win64(1);
    HostUc* uc = hostuc_new(HOSTUC_WIN64, 1u << 16);
    if (!uc) {
        fprintf(stderr, "FAIL: no loader for the code\n");
        return 1;
    }
    g_test.cpu = (CPU*)hostuc_alloc(uc, sizeof(CPU));
    g_test.rt = (RtAPI*)hostuc_alloc(uc, sizeof(RtAPI));
#define NATIVE(f) hostuc_native(uc, #f, (void*)f)
#else
    SnxCodeLoader* loader = snx_code_loader_new(0, error, sizeof error);
    if (!loader) {
        fprintf(stderr, "FAIL: no loader for the code: %s\n", error);
        return 1;
    }
    g_test.cpu = (CPU*)calloc(1, sizeof(CPU));
    g_test.rt = (RtAPI*)calloc(1, sizeof(RtAPI));
#define NATIVE(f) ((uint64_t)(uintptr_t)f)
#endif
    g_test.guest = (unsigned char*)calloc(1, GUEST);
    g_test.disp = (SNX_ATOMIC(GuestFn)*)calloc(WORDS, sizeof *g_test.disp);
    if (!g_test.cpu || !g_test.rt || !g_test.guest || !g_test.disp) {
        fprintf(stderr, "FAIL: out of memory\n");
        return 1;
    }
    g_test.rt->svc = (void (*)(CPU*, u32))(uintptr_t)NATIVE(on_svc);
    g_test.rt->undef = (void (*)(CPU*, u64, u32))(uintptr_t)NATIVE(on_undef);
    g_test.rt->poll = (void (*)(CPU*))(uintptr_t)NATIVE(on_poll);
    g_test.rt->miss = (GuestFn)(uintptr_t)NATIVE(on_miss);
    image.image = (unsigned char*)g_test.words;
    image.image_size = image.text_size = sizeof g_test.words;
    image.base = BASE;
    for (k = 0; k < COUNT; ++k) hints[k] = BASE + 4 * functions[k];
    if (snx_codegen_analyze(&codegen, &image, 1, hints, COUNT, 1, SNX_A64_TARGET_SWITCH)) {
        fprintf(stderr, "FAIL: no functions found\n");
        return 1;
    }
    made = (unsigned char*)calloc(codegen.function_count + 1, 1);
    if (!made ||
        snx_x64_object(&codegen, 0, codegen.function_count, &options, made, &object, &size, error, sizeof error) < 0 ||
        !object) {
        fprintf(stderr, "FAIL: the back end made no unit of the functions: %s\n", error);
        return 1;
    }
#if defined(_WIN32)
    {
        SnxLinkInput input = {object, size, "unwind"};
        const char* name = "UNWIND_TABLE";
        SnxU64 at = 0;
#define DEFINE(f) snx_code_define(loader, #f, NATIVE(f));
        HELPERS(DEFINE)
        if (snx_code_load(loader, &input, 1, &name, 1, &at, error, sizeof error)) {
            fprintf(stderr, "FAIL: loading the unit: %s\n", error);
            return 1;
        }
        table = (const uint64_t*)(uintptr_t)at;
    }
#else
#define DEFINE(f) NATIVE(f);
    HELPERS(DEFINE)
    if (hostuc_load(uc, object, size, error, sizeof error)) {
        fprintf(stderr, "FAIL: loading the unit: %s\n", error);
        return 1;
    }
    table = (const uint64_t*)(uintptr_t)hostuc_symbol(uc, "UNWIND_TABLE");
#endif
    for (k = 0; k < codegen.function_count; ++k) {
        GuestFn fn = (GuestFn)(uintptr_t)table[k];
        atomic_store_explicit(&g_test.disp[(codegen.functions[k].start - BASE) / 4], fn, memory_order_relaxed);
#if defined(_WIN32)
        if (!g_test.lo || (unsigned char*)(uintptr_t)table[k] < g_test.lo)
            g_test.lo = (unsigned char*)(uintptr_t)table[k];
        {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(table[k], &base, NULL);
            if (!f) {
                fprintf(stderr, "FAIL: RtlLookupFunctionEntry finds no entry for the function at word %llu\n",
                        (unsigned long long)(codegen.functions[k].start - BASE) / 4);
                return 1;
            }
            if (g_test.hi < (unsigned char*)(uintptr_t)(base + f->EndAddress))
                g_test.hi = (unsigned char*)(uintptr_t)(base + f->EndAddress);
        }
#endif
    }
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, on_step);
    for (k = 0; k < 8; ++k) snx_test_marks.mark[k] = 0x5151000000000000ull + k * 0x0101010101ull;
    for (k = 0; k < 10; ++k)
        snx_test_marks.xmark[k][0] = 0x6262000000000000ull + k * 0x0303030303ull,
        snx_test_marks.xmark[k][1] = 0x7373000000000000ull + k * 0x0505050505ull;
#else
    hostuc_step(uc, 1);
#endif
    for (k = 0; k < COUNT; ++k) {
        unsigned f = functions[k];
        GuestFn fn = atomic_load_explicit(&g_test.disp[f], memory_order_relaxed);
        size_t before = g_test.failures;
        if (!fn) {
            failed("no code of it", f);
            continue;
        }
        g_test.svcs = g_test.undefs = g_test.polls = g_test.misses = 0;
        start(f);
#if defined(_WIN32)
        g_test.function = f;
        snx_test_marks.step = 1;
        entry(fn, g_test.cpu, g_test.cpu->pc);
        snx_test_marks.step = 0;
#else
        if (hostuc_call(uc, (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)g_test.cpu, g_test.cpu->pc, NULL))
            failed("its registers or its unwinding", f);
#endif
        if (g_test.failures == before) check(f);
    }
#if defined(_WIN32)
    unwound_count = g_test.unwound, bad = g_test.bad;
    /* out of a call the code made, across its frames: the one that calls, the pinned one, the long one */
    {
        static const unsigned out[] = {F_CALLS, F_PINNED, F_BIG};
        volatile unsigned n;
        g_test.jump = 1;
        for (n = 0; n < sizeof out / sizeof *out; ++n) switch (setjmp(g_test.out)) {
                case 0:
                    start(out[n]);
                    atomic_load_explicit(&g_test.disp[out[n]], memory_order_relaxed)(g_test.cpu, g_test.cpu->pc);
                    failed("returned rather than jumped out of", out[n]);
                    break;
                case 7: break;
                default: failed("the longjmp out of it came back wrong", out[n]);
            }
        g_test.jump = 0;
    }
#else
    unwound_count = hostuc_unwound(uc, &bad);
#endif
    if (bad) ++g_test.failures;
    {
        /* a callee of the object's may be another compiler's, which has 32 bytes there to write (in an AOT module LLVM's):
           push rbx; mov rbx, rcx; sub rsp, imm32 */
        const unsigned char* p =
            (const unsigned char*)(uintptr_t)atomic_load_explicit(&g_test.disp[F_DIRECT], memory_order_relaxed);
        if (!p || memcmp(p, "\x53\x48\x89\xCB\x48\x81\xEC", 7) || p[7] < 32)
            failed("its frame has no room for its callee's", F_DIRECT);
    }
    {
        /* the long function's frame is past a page: its prologue reads each page first (mov r9, rsp; sub r9, 4096) */
        static const unsigned char probe[] = {0x49, 0x89, 0xE1, 0x49, 0x81, 0xE9, 0x00, 0x10, 0x00, 0x00};
        size_t probes = 0;
        for (k = 0; k + sizeof probe <= size; ++k) probes += !memcmp(object + k, probe, sizeof probe);
        if (probes != 1)
            ++g_test.failures, fprintf(stderr, "FAIL: %zu frames of a page or more, not the long one's\n", probes);
    }
    if (unwound_count < BIG) ++g_test.failures, fprintf(stderr, "FAIL: only %zu states unwound\n", unwound_count);
    printf("x64 unwind: %zu functions, %zu bytes; %zu states unwound, %zu wrongly\n", codegen.function_count, size,
           unwound_count, bad);
    free(made), free(object);
    snx_codegen_free(&codegen);
#if !defined(_WIN32)
    hostuc_free(uc);
#endif
    return g_test.failures != 0;
}
