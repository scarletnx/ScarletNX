/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's code for Windows (its calling convention, in COFF objects) run by a System V x86-64 host, for
   the back ends' tests: host_unicorn.h's interface without Unicorn (HOSTUC_WIN64). The objects are loaded by the
   production loader (code_loader_coff.c), whose machine's code this host runs. hostuc_call enters the code as Windows'
   C would, every register its callee has to keep marked, and stops where one came back changed (RBX, RBP, RSI, RDI, R12
   to R15, XMM6 to XMM15, MXCSR, the stack past the shadow it was left). A native function the code calls goes through a
   stub that is a Windows callee: it takes the arguments where that convention has them, overwrites the shadow it is
   owed, changes every register a callee may, and refuses a stack that is not 16-aligned.

   The objects' function table is held to what Windows does with it: where a native function is called, and with
   hostuc_step at every instruction the code runs, its frames are unwound by the table as RtlVirtualUnwind does (the
   codes of the prologue run so far, an epilogue's own instructions, else all of them), back to the entry, every register
   to its mark */
#define _GNU_SOURCE /* the signal's registers */
#include "support/host_unicorn.h"
#include "common/cpu/recompiler/llvm/code_loader_int.h"
#include "common/cpu/recompiler/llvm/coff_defs.h"
#include "common/utilities/bits.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#define PAGE  4096u
#define STUB  32u
#define STUBS 256u

typedef uint64_t (*Native)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef struct {
    Native fn;
    char* name;
} Stub;
typedef struct {
    char* name;
    uint64_t at;
} Defined;
/* an entry of the loaded function table: its code, its UNWIND_INFO */
typedef struct {
    uint64_t begin, end;
    const unsigned char* info;
} Entry;
struct HostUc {
    SnxCodeLoader* loader;
    unsigned char *code, *stubs;
    size_t cap, used;
    Stub stub[STUBS];
    unsigned nstubs;
    Defined* defined;
    size_t ndefined;
    uintptr_t low; /* the lowest of the pages below the arena */
    Entry* entries;
    size_t nentries;
    size_t unwound, failures; /* the states unwound, and those that failed */
};
static HostUc* g_host; /* the one whose code runs: the traps' and the steps' */

/* what the entry below marks, reads back and checks: the registers, then the canary above the callee's shadow */
#define MARKS  8 /* RBX, RBP, RSI, RDI, R12 to R15 */
#define CANARY 8
struct Win64Regs {
    uint64_t mark[MARKS];         /* 0 */
    uint64_t xmark[10][2];        /* 64, 16-aligned */
    uint64_t got[MARKS];          /* 224 */
    uint64_t xgot[10][2];         /* 288 */
    uint64_t canary[CANARY];      /* 448 */
    uint64_t sp, sp_after, fault; /* 512, 520, 528: RSP at the call and after it; a native's call with RSP misaligned */
    uint32_t csr, csr_after;      /* 536, 540 */
    uint64_t host_sp;             /* 544 */
    uint64_t junk[2];             /* 552: not 16-aligned, loaded unaligned */
    uint64_t step;                /* 568: the code stops after each instruction (the trap flag) */
    uint64_t regs[16];            /* 576: a trap's registers, by number, as the code called it */
    uint64_t xregs[16][2];        /* 704 */
};
[[gnu::visibility("hidden")]] extern struct Win64Regs snx_test_win64;
[[gnu::aligned(16), gnu::used, gnu::visibility("hidden")]] struct Win64Regs snx_test_win64;
static_assert(offsetof(struct Win64Regs, xmark) == 64 && offsetof(struct Win64Regs, got) == 224 &&
                  offsetof(struct Win64Regs, xgot) == 288 && offsetof(struct Win64Regs, canary) == 448 &&
                  offsetof(struct Win64Regs, sp) == 512 && offsetof(struct Win64Regs, csr) == 536 &&
                  offsetof(struct Win64Regs, host_sp) == 544 && offsetof(struct Win64Regs, junk) == 552 &&
                  offsetof(struct Win64Regs, step) == 568 && offsetof(struct Win64Regs, regs) == 576 &&
                  offsetof(struct Win64Regs, xregs) == 704,
              "the entry's and the stubs' bytes reach the marks at their offsets");

/* enter(fn RDI, a0 RSI, a1 RDX): fn(a0, a1) as Windows' convention calls it */
[[gnu::naked]] static uint64_t enter(uint64_t fn, uint64_t a0, uint64_t a1) {
    __asm__("pushq %rbx\n\tpushq %rbp\n\tpushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
            "movq %rsp, snx_test_win64+544(%rip)\n\t"
            "subq $104, %rsp\n\t" /* 32 of shadow, 64 of canary, 8 to align */
            "movq snx_test_win64+448(%rip), %rax\n\tmovq %rax, 32(%rsp)\n\t"
            "movq snx_test_win64+456(%rip), %rax\n\tmovq %rax, 40(%rsp)\n\t"
            "movq snx_test_win64+464(%rip), %rax\n\tmovq %rax, 48(%rsp)\n\t"
            "movq snx_test_win64+472(%rip), %rax\n\tmovq %rax, 56(%rsp)\n\t"
            "movq snx_test_win64+480(%rip), %rax\n\tmovq %rax, 64(%rsp)\n\t"
            "movq snx_test_win64+488(%rip), %rax\n\tmovq %rax, 72(%rsp)\n\t"
            "movq snx_test_win64+496(%rip), %rax\n\tmovq %rax, 80(%rsp)\n\t"
            "movq snx_test_win64+504(%rip), %rax\n\tmovq %rax, 88(%rsp)\n\t"
            "stmxcsr snx_test_win64+536(%rip)\n\t"
            "movq %rdi, %rax\n\tmovq %rsi, %rcx\n\t" /* fn, a0; a1 is in RDX */
            "movq snx_test_win64+0(%rip), %rbx\n\tmovq snx_test_win64+8(%rip), %rbp\n\t"
            "movq snx_test_win64+16(%rip), %rsi\n\tmovq snx_test_win64+24(%rip), %rdi\n\t"
            "movq snx_test_win64+32(%rip), %r12\n\tmovq snx_test_win64+40(%rip), %r13\n\t"
            "movq snx_test_win64+48(%rip), %r14\n\tmovq snx_test_win64+56(%rip), %r15\n\t"
            "movaps snx_test_win64+64(%rip), %xmm6\n\tmovaps snx_test_win64+80(%rip), %xmm7\n\t"
            "movaps snx_test_win64+96(%rip), %xmm8\n\tmovaps snx_test_win64+112(%rip), %xmm9\n\t"
            "movaps snx_test_win64+128(%rip), %xmm10\n\tmovaps snx_test_win64+144(%rip), %xmm11\n\t"
            "movaps snx_test_win64+160(%rip), %xmm12\n\tmovaps snx_test_win64+176(%rip), %xmm13\n\t"
            "movaps snx_test_win64+192(%rip), %xmm14\n\tmovaps snx_test_win64+208(%rip), %xmm15\n\t"
            /* the registers no argument is in, as a caller may leave them */
            "movq snx_test_win64+552(%rip), %r8\n\tmovq %r8, %r9\n\tmovq %r8, %r10\n\tmovq %r8, %r11\n\t"
            "movq %rsp, snx_test_win64+512(%rip)\n\t"
            "cmpq $0, snx_test_win64+568(%rip)\n\tje 1f\n\t"
            "pushfq\n\torq $0x100, (%rsp)\n\tpopfq\n" /* the trap flag */
            "1:\n\t"
            "callq *%rax\n\t"
            "pushfq\n\tandq $-257, (%rsp)\n\tpopfq\n\t"
            "movq %rsp, snx_test_win64+520(%rip)\n\t"
            "movq %rbx, snx_test_win64+224(%rip)\n\tmovq %rbp, snx_test_win64+232(%rip)\n\t"
            "movq %rsi, snx_test_win64+240(%rip)\n\tmovq %rdi, snx_test_win64+248(%rip)\n\t"
            "movq %r12, snx_test_win64+256(%rip)\n\tmovq %r13, snx_test_win64+264(%rip)\n\t"
            "movq %r14, snx_test_win64+272(%rip)\n\tmovq %r15, snx_test_win64+280(%rip)\n\t"
            "movups %xmm6, snx_test_win64+288(%rip)\n\tmovups %xmm7, snx_test_win64+304(%rip)\n\t"
            "movups %xmm8, snx_test_win64+320(%rip)\n\tmovups %xmm9, snx_test_win64+336(%rip)\n\t"
            "movups %xmm10, snx_test_win64+352(%rip)\n\tmovups %xmm11, snx_test_win64+368(%rip)\n\t"
            "movups %xmm12, snx_test_win64+384(%rip)\n\tmovups %xmm13, snx_test_win64+400(%rip)\n\t"
            "movups %xmm14, snx_test_win64+416(%rip)\n\tmovups %xmm15, snx_test_win64+432(%rip)\n\t"
            "stmxcsr snx_test_win64+540(%rip)\n\t"
            "cld\n\t"
            "movq snx_test_win64+512(%rip), %rsp\n\t"
            /* the canary, what is left of it: 0 for all of it */
            "movq 32(%rsp), %rcx\n\txorq snx_test_win64+448(%rip), %rcx\n\t"
            "movq 40(%rsp), %rdx\n\txorq snx_test_win64+456(%rip), %rdx\n\torq %rdx, %rcx\n\t"
            "movq 48(%rsp), %rdx\n\txorq snx_test_win64+464(%rip), %rdx\n\torq %rdx, %rcx\n\t"
            "movq 56(%rsp), %rdx\n\txorq snx_test_win64+472(%rip), %rdx\n\torq %rdx, %rcx\n\t"
            "movq 64(%rsp), %rdx\n\txorq snx_test_win64+480(%rip), %rdx\n\torq %rdx, %rcx\n\t"
            "movq 72(%rsp), %rdx\n\txorq snx_test_win64+488(%rip), %rdx\n\torq %rdx, %rcx\n\t"
            "movq 80(%rsp), %rdx\n\txorq snx_test_win64+496(%rip), %rdx\n\torq %rdx, %rcx\n\t"
            "movq 88(%rsp), %rdx\n\txorq snx_test_win64+504(%rip), %rdx\n\torq %rdx, %rcx\n\t"
            "orq %rcx, snx_test_win64+528(%rip)\n\t"
            "movq snx_test_win64+544(%rip), %rsp\n\t"
            "popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\tpopq %rbp\n\tpopq %rbx\n\tretq");
}

/* ---- the unwinder: what RtlVirtualUnwind makes of a frame by its UNWIND_INFO (Microsoft's x64 exception handling) ---- */

typedef struct {
    uint64_t rip, r[16], x[16][2];
} Frame;
static uint64_t ld64(uint64_t at) {
    uint64_t v;
    memcpy(&v, (const void*)(uintptr_t)at, 8);
    return v;
}
static const Entry* entry_of(const HostUc* u, uint64_t rip) {
    size_t k;
    for (k = 0; k < u->nentries; ++k)
        if (rip >= u->entries[k].begin && rip < u->entries[k].end) return &u->entries[k];
    return NULL;
}
/* an epilogue at rip (ADD RSP, pops, then the return or a jump out of the function, or to its start): run: 1 */
static int epilogue(const Entry* e, Frame* f) {
    const unsigned char* p = (const unsigned char*)(uintptr_t)f->rip;
    Frame g = *f;
    int64_t to;
    if (!memcmp(p, "\x48\x83\xC4", 3))
        g.r[4] += (uint64_t)(int64_t)(int8_t)p[3], p += 4;
    else if (!memcmp(p, "\x48\x81\xC4", 3))
        g.r[4] += (uint64_t)(int64_t)(int32_t)snx_ld_le32(p + 3), p += 7;
    for (;;) {
        if ((p[0] & 0xF8) == 0x58)
            g.r[p[0] & 7] = ld64(g.r[4]), g.r[4] += 8, p += 1;
        else if (p[0] == 0x41 && (p[1] & 0xF8) == 0x58)
            g.r[8 | (p[1] & 7)] = ld64(g.r[4]), g.r[4] += 8, p += 2;
        else
            break;
    }
    if (p[0] == 0xE9) {
        to = (int64_t)(uintptr_t)(p + 5) + (int32_t)snx_ld_le32(p + 1);
        if ((uint64_t)to > e->begin && (uint64_t)to < e->end) return 0;
    } else if (p[0] != 0xC3 && !((p[0] & 0xF8) == 0x48 && p[1] == 0xFF && (p[2] & 0x38) == 0x20))
        return 0;
    g.rip = ld64(g.r[4]);
    g.r[4] += 8;
    *f = g;
    return 1;
}
/* one frame: the caller's state in *f. 0 where rip has no entry */
static int unwind(const HostUc* u, Frame* f) {
    const Entry* e = entry_of(u, f->rip);
    const unsigned char* info = e ? e->info : NULL;
    uint64_t offset, base;
    unsigned k = 0, count;
    if (!e) return 0;
    offset = f->rip - e->begin;
    if (offset >= info[1] && epilogue(e, f)) return 1;
    base = f->r[4];
    count = info[2];
    while (k < count) {
        const unsigned char* c = info + 4 + 2 * k;
        unsigned op = c[1] & 15, arg = c[1] >> 4;
        unsigned slots = op == 1 ? arg ? 3 : 2 : op == 8 ? 2 : 1;
        if (offset >= info[1] || c[0] <= offset) {
            switch (op) {
                case 0: f->r[arg] = ld64(f->r[4]), f->r[4] += 8; break;                         /* UWOP_PUSH_NONVOL */
                case 1: f->r[4] += arg ? snx_ld_le32(c + 2) : 8ull * snx_ld_le16(c + 2); break; /* UWOP_ALLOC_LARGE */
                case 2: f->r[4] += 8ull * arg + 8; break;                                       /* UWOP_ALLOC_SMALL */
                case 8:
                    memcpy(f->x[arg], (const void*)(uintptr_t)(base + 16ull * snx_ld_le16(c + 2)), 16);
                    break; /* XMM128 */
                default: return 0;
            }
        }
        k += slots;
    }
    f->rip = ld64(f->r[4]);
    f->r[4] += 8;
    return 1;
}
/* the state of some of the code's instruction unwound to the entry: what is wrong with where it came to, NULL for
   nothing */
static const char* unwinds(HostUc* u, Frame* f) {
    static const unsigned char kept[MARKS] = {3, 5, 6, 7, 12, 13, 14, 15};
    const struct Win64Regs* w = &snx_test_win64;
    unsigned k, frames = 0;
    ++u->unwound;
    while (entry_of(u, f->rip)) {
        if (++frames > 10000 || !unwind(u, f)) return "a frame it can't unwind";
        if (f->r[4] > w->sp || f->r[4] + (256u << 10) < w->sp) return "a stack pointer off the stack";
    }
    if (f->r[4] != w->sp || f->rip != ld64(w->sp - 8)) return "not the entry's frame";
    for (k = 0; k < MARKS; ++k)
        if (f->r[kept[k]] != w->mark[k]) return "a general-purpose register not its caller's";
    for (k = 0; k < 10; ++k)
        if (memcmp(f->x[6 + k], w->xmark[k], 16)) return "an XMM register not its caller's";
    return NULL;
}
static void failed(HostUc* u, uint64_t rip, const char* why) {
    const Entry* e = entry_of(u, rip);
    if (++u->failures > 20) return;
    fprintf(stderr, "win64: unwound from +%llx of the function at %llx: %s\n",
            (unsigned long long)(e ? rip - e->begin : 0), (unsigned long long)(e ? e->begin : rip), why);
}
/* a native function's call: from the call's return in the code */
[[gnu::used]] static void trap_unwinds(void) {
    struct Win64Regs* w = &snx_test_win64;
    Frame f;
    uint64_t rip = ld64(w->regs[4]);
    const char* why;
    if (!g_host || !entry_of(g_host, rip)) return; /* code with no table (a step's or a block's) */
    f.rip = rip;
    memcpy(f.r, w->regs, sizeof f.r);
    memcpy(f.x, w->xregs, sizeof f.x);
    f.r[4] += 8;
    if ((why = unwinds(g_host, &f))) failed(g_host, rip, why);
}
/* hostuc_step's: after an instruction, where the next is the code's */
static void on_step(int number, siginfo_t* info, void* context) {
    static const int regs[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                 REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    const ucontext_t* c = (const ucontext_t*)context;
    Frame f;
    const char* why;
    unsigned k;
    (void)number, (void)info;
    f.rip = (uint64_t)c->uc_mcontext.gregs[REG_RIP];
    if (!g_host || !entry_of(g_host, f.rip)) return;
    for (k = 0; k < 16; ++k) f.r[k] = (uint64_t)c->uc_mcontext.gregs[regs[k]];
    memcpy(f.x, c->uc_mcontext.fpregs->_xmm, sizeof f.x);
    {
        uint64_t rip = f.rip;
        if ((why = unwinds(g_host, &f))) failed(g_host, rip, why);
    }
}
void hostuc_step(HostUc* u, int on) {
    struct sigaction sa = {0};
    (void)u;
    sa.sa_sigaction = on_step;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL);
    snx_test_win64.step = (uint64_t)on;
}
size_t hostuc_unwound(const HostUc* u, size_t* failures) {
    *failures = u->failures;
    return u->unwound;
}

/* what a stub jumps to, R10 the native function: a Windows callee of six arguments (those past the native's own are
   whatever is there) calling the System V function, then every register it may change changed */
[[gnu::naked]] static void trap(void) {
    __asm__("movq %rsp, %rax\n\tandl $15, %eax\n\tcmpl $8, %eax\n\tje 1f\n\t"
            "movq $1, snx_test_win64+528(%rip)\n" /* the caller's stack was not 16-aligned */
            "1:\n\t"
            "movq %rbx, snx_test_win64+600(%rip)\n\tmovq %rsp, snx_test_win64+608(%rip)\n\t"
            "movq %rbp, snx_test_win64+616(%rip)\n\tmovq %rsi, snx_test_win64+624(%rip)\n\t"
            "movq %rdi, snx_test_win64+632(%rip)\n\tmovq %r12, snx_test_win64+672(%rip)\n\t"
            "movq %r13, snx_test_win64+680(%rip)\n\tmovq %r14, snx_test_win64+688(%rip)\n\t"
            "movq %r15, snx_test_win64+696(%rip)\n\t"
            "movaps %xmm6, snx_test_win64+800(%rip)\n\tmovaps %xmm7, snx_test_win64+816(%rip)\n\t"
            "movaps %xmm8, snx_test_win64+832(%rip)\n\tmovaps %xmm9, snx_test_win64+848(%rip)\n\t"
            "movaps %xmm10, snx_test_win64+864(%rip)\n\tmovaps %xmm11, snx_test_win64+880(%rip)\n\t"
            "movaps %xmm12, snx_test_win64+896(%rip)\n\tmovaps %xmm13, snx_test_win64+912(%rip)\n\t"
            "movaps %xmm14, snx_test_win64+928(%rip)\n\tmovaps %xmm15, snx_test_win64+944(%rip)\n\t"
            "pushq %rsi\n\tpushq %rdi\n\tpushq %rbp\n\tmovq %rsp, %rbp\n\t"
            "movq %rcx, %rdi\n\tmovq %rdx, %rsi\n\tmovq %r8, %rdx\n\tmovq %r9, %rcx\n\t"
            "movq 64(%rbp), %r8\n\tmovq 72(%rbp), %r9\n\t" /* past the pushes, the return and the shadow */
            "movq snx_test_win64+552(%rip), %rax\n\t"
            "movq %rax, 32(%rbp)\n\tmovq %rax, 40(%rbp)\n\tmovq %rax, 48(%rbp)\n\tmovq %rax, 56(%rbp)\n\t"
            "subq $176, %rsp\n\tandq $-16, %rsp\n\t"
            "movaps %xmm6, 0(%rsp)\n\tmovaps %xmm7, 16(%rsp)\n\tmovaps %xmm8, 32(%rsp)\n\tmovaps %xmm9, 48(%rsp)\n\t"
            "movaps %xmm10, 64(%rsp)\n\tmovaps %xmm11, 80(%rsp)\n\tmovaps %xmm12, 96(%rsp)\n\t"
            "movaps %xmm13, 112(%rsp)\n\tmovaps %xmm14, 128(%rsp)\n\tmovaps %xmm15, 144(%rsp)\n\t"
            "movq %rdi, 160(%rsp)\n\t"
            "pushq %r8\n\tpushq %r9\n\tpushq %rcx\n\tpushq %rdx\n\tpushq %rsi\n\tpushq %rdi\n\tpushq %r10\n\t"
            "subq $8, %rsp\n\t"
            "callq trap_unwinds\n\t"
            "addq $8, %rsp\n\t"
            "popq %r10\n\tpopq %rdi\n\tpopq %rsi\n\tpopq %rdx\n\tpopq %rcx\n\tpopq %r9\n\tpopq %r8\n\t"
            "callq *%r10\n\t"
            "movaps 0(%rsp), %xmm6\n\tmovaps 16(%rsp), %xmm7\n\tmovaps 32(%rsp), %xmm8\n\tmovaps 48(%rsp), %xmm9\n\t"
            "movaps 64(%rsp), %xmm10\n\tmovaps 80(%rsp), %xmm11\n\tmovaps 96(%rsp), %xmm12\n\t"
            "movaps 112(%rsp), %xmm13\n\tmovaps 128(%rsp), %xmm14\n\tmovaps 144(%rsp), %xmm15\n\t"
            "movq snx_test_win64+552(%rip), %rcx\n\tmovq %rcx, %rdx\n\tmovq %rcx, %r8\n\tmovq %rcx, %r9\n\t"
            "movq %rcx, %r10\n\tmovq %rcx, %r11\n\t"
            "movups snx_test_win64+552(%rip), %xmm0\n\tmovaps %xmm0, %xmm1\n\tmovaps %xmm0, %xmm2\n\t"
            "movaps %xmm0, %xmm3\n\tmovaps %xmm0, %xmm4\n\tmovaps %xmm0, %xmm5\n\t"
            "movq %rbp, %rsp\n\tpopq %rbp\n\tpopq %rdi\n\tpopq %rsi\n\tretq");
}

const char* hostuc_name(HostUcArch arch) {
    (void)arch;
    return "x86-64 (Windows)";
}
int hostuc_big(HostUcArch arch) {
    (void)arch;
    return 0;
}
/* the structures are this machine's: none is turned over */
void hostuc_swapper(HostUc* u, void (*fn)(void* user), void* user) { (void)u, (void)fn, (void)user; }
void hostuc_changed(HostUc* u, const void* p, size_t size) { (void)u, (void)p, (void)size; }

/* pages below the loader's arena, where the code's 32-bit distances reach them */
static void* near_pages(HostUc* u, size_t size) {
    uintptr_t base = (uintptr_t)u->loader->base;
    void* p;
    size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (!u->low) u->low = base;
    p = mmap((void*)(u->low - size - 16 * PAGE), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if ((uintptr_t)p < base && base - (uintptr_t)p > (uintptr_t)1 << 30) { /* a hint not taken, too far */
        munmap(p, size);
        return NULL;
    }
    if ((uintptr_t)p < u->low) u->low = (uintptr_t)p;
    return p;
}

HostUc* hostuc_new(HostUcArch arch, size_t code_cap) {
    HostUc* u;
    char error[256];
    unsigned k;
    if (arch != HOSTUC_WIN64 || !(u = (HostUc*)calloc(1, sizeof *u))) return NULL;
    u->loader = snx_code_loader_new_machine((size_t)256 << 20, SNX_CODE_COFF_X64, error, sizeof error);
    if (!u->loader) {
        fprintf(stderr, "win64 loader: %s\n", error);
        free(u);
        return NULL;
    }
    u->cap = (code_cap + PAGE - 1) & ~(size_t)(PAGE - 1);
    u->code = (unsigned char*)near_pages(u, u->cap + STUB * STUBS);
    if (!u->code || mprotect(u->code, u->cap + STUB * STUBS, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        hostuc_free(u);
        return NULL;
    }
    u->stubs = u->code + u->cap;
    for (k = 0; k < MARKS; ++k) snx_test_win64.mark[k] = 0x5151000000000000ull + k * 0x0101010101ull;
    for (k = 0; k < 10; ++k) {
        snx_test_win64.xmark[k][0] = 0x6262000000000000ull + k * 0x0303030303ull;
        snx_test_win64.xmark[k][1] = 0x7373000000000000ull + k * 0x0505050505ull;
    }
    for (k = 0; k < CANARY; ++k) snx_test_win64.canary[k] = 0x8484000000000000ull + k * 0x0707070707ull;
    snx_test_win64.junk[0] = snx_test_win64.junk[1] = 0xBAD0BAD1BAD2BAD3ull;
    return u;
}
void hostuc_free(HostUc* u) {
    size_t k;
    if (!u) return;
    snx_code_loader_free(u->loader);
    if (u->code) munmap(u->code, u->cap + STUB * STUBS);
    for (k = 0; k < u->nstubs; ++k) free(u->stub[k].name);
    for (k = 0; k < u->ndefined; ++k) free(u->defined[k].name);
    free(u->defined);
    free(u->entries);
    if (g_host == u) g_host = NULL;
    free(u);
}
int hostuc_map(HostUc* u, void* p, size_t size) {
    (void)u;
    return mprotect(p, (size + PAGE - 1) & ~(size_t)(PAGE - 1), PROT_READ | PROT_WRITE | PROT_EXEC);
}
void* hostuc_alloc(HostUc* u, size_t size) { return near_pages(u, size); }
void* hostuc_pages(size_t size) {
    size = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

uint64_t hostuc_trap_of(HostUc* u, const char* name) {
    unsigned k;
    for (k = 0; k < u->nstubs; ++k)
        if (!strcmp(u->stub[k].name, name)) return (uint64_t)(uintptr_t)(u->stubs + STUB * k);
    return 0;
}
uint64_t hostuc_native(HostUc* u, const char* name, void* fn) {
    uint64_t at = hostuc_trap_of(u, name), to = (uint64_t)(uintptr_t)trap;
    unsigned char* p;
    if (at) return at;
    if (u->nstubs == STUBS) return 0;
    memcpy(&u->stub[u->nstubs].fn, &fn, sizeof fn);
    u->stub[u->nstubs].name = strdup(name);
    p = u->stubs + STUB * u->nstubs++;
    memcpy(p, "\x49\xBA", 2), memcpy(p + 2, &fn, 8);       /* mov r10, fn */
    memcpy(p + 10, "\x49\xBB", 2), memcpy(p + 12, &to, 8); /* mov r11, trap */
    memcpy(p + 20, "\x41\xFF\xE3", 3);                     /* jmp r11 */
    return (uint64_t)(uintptr_t)p;
}
/* no wide native is x86-64's: its CASP is inline */
uint64_t hostuc_native_wide(HostUc* u, const char* name, void* fn) {
    (void)u, (void)name, (void)fn;
    return 0;
}
uint64_t hostuc_code(HostUc* u, const void* bytes, size_t size) {
    size_t at = (u->used + 15) & ~(size_t)15;
    if (at + size > u->cap) return 0;
    memcpy(u->code + at, bytes, size);
    u->used = at + size;
    return (uint64_t)(uintptr_t)(u->code + at);
}
int hostuc_define(HostUc* u, const char* name, uint64_t address) {
    Defined* grown = (Defined*)realloc(u->defined, (u->ndefined + 1) * sizeof *grown);
    if (!grown) return -1;
    u->defined = grown;
    if (!(u->defined[u->ndefined].name = strdup(name))) return -1;
    u->defined[u->ndefined++].at = address;
    return 0;
}
static SnxU64 resolve(void* user, const char* name) {
    HostUc* u = (HostUc*)user;
    size_t k;
    for (k = 0; k < u->ndefined; ++k)
        if (!strcmp(u->defined[k].name, name)) return u->defined[k].at;
    return hostuc_trap_of(u, name);
}
/* the section of the object named name: its header, NULL for none */
static const unsigned char* section(const unsigned char* object, const char* name) {
    unsigned n = snx_ld_le16(object + 2), k;
    for (k = 0; k < n; ++k)
        if (!strncmp((const char*)object + 20 + 40 * (size_t)k, name, 8)) return object + 20 + 40 * (size_t)k;
    return NULL;
}
/* the loaded object's function table: each entry as the loader relocated it (from the arena's start, its code at text),
   found in the arena's read-only data, and its UNWIND_INFO, which is the object's */
static int table(HostUc* u, const unsigned char* object, uint64_t text, char* error, size_t error_size) {
    const unsigned char *p = section(object, ".pdata"), *x = section(object, ".xdata");
    const unsigned char* ro = u->loader->base + u->loader->region[SNX_CODE_RODATA].start;
    size_t used = u->loader->region[SNX_CODE_RODATA].used, n, k, at;
    Entry* grown;
    if (!p || !x) return 0; /* functions of no frame alone: a descriptor's */
    n = snx_ld_le32(p + 16) / 12;
    if (!(grown = (Entry*)realloc(u->entries, (u->nentries + n) * sizeof *grown))) return -1;
    u->entries = grown;
    for (k = 0; k < n; ++k) {
        const unsigned char* e = object + snx_ld_le32(p + 20) + 12 * k;
        const unsigned char* info = object + snx_ld_le32(x + 20) + snx_ld_le32(e + 8);
        uint32_t rva = (uint32_t)(text - (uint64_t)(uintptr_t)u->loader->base), got = 0;
        for (at = 0; at + 12 <= used; at += 4)
            if (snx_ld_le32(ro + at) == rva + snx_ld_le32(e) && snx_ld_le32(ro + at + 4) == rva + snx_ld_le32(e + 4))
                break;
        if (at + 12 <= used) got = snx_ld_le32(ro + at + 8);
        if (!got || memcmp(u->loader->base + got, info, 4 + 2 * (size_t)info[2])) {
            snprintf(error, error_size, "entry %zu of the function table is not in the arena, or not its unwind data",
                     k);
            return -1;
        }
        u->entries[u->nentries].begin = text + snx_ld_le32(e);
        u->entries[u->nentries].end = text + snx_ld_le32(e + 4);
        u->entries[u->nentries++].info = u->loader->base + got;
    }
    return 0;
}
int hostuc_load(HostUc* u, const unsigned char* object, size_t size, char* error, size_t error_size) {
    unsigned char* copy = (unsigned char*)malloc(size);
    SnxLinkInput input = {copy, size, "win64 object"};
    uint64_t text = snx_align_up(snx_code_next_free(u->loader, SNX_CODE_TEXT), 64); /* where the load's code starts */
    uint32_t symbols, count, k;
    int rc;
    if (!copy) return -1;
    memcpy(copy, object, size);
    /* the object's own symbols made global, for hostuc_symbol (but its sections') */
    symbols = snx_ld_le32(copy + 8), count = snx_ld_le32(copy + 12);
    for (k = 0; k < count; k += 1u + copy[symbols + 18 * (size_t)k + 17]) {
        unsigned char* s = copy + symbols + 18 * (size_t)k;
        if (s[16] == SYM_STATIC && s[0] != '.') s[16] = SYM_EXTERNAL;
    }
    snx_code_set_resolver(u->loader, resolve, u);
    rc = snx_code_load(u->loader, &input, 1, NULL, 0, NULL, error, error_size);
    if (!rc) rc = table(u, copy, text, error, error_size);
    free(copy);
    return rc;
}
uint64_t hostuc_symbol(HostUc* u, const char* name) {
    SnxU64 at = 0;
    return snx_code_lookup(u->loader, name, &at) ? at : 0;
}

int hostuc_call(HostUc* u, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t* result) {
    static const char* const names[MARKS] = {"RBX", "RBP", "RSI", "RDI", "R12", "R13", "R14", "R15"};
    struct Win64Regs* w = &snx_test_win64;
    uint64_t r;
    size_t failures = u->failures;
    int bad = 0;
    unsigned k;
    w->fault = 0;
    g_host = u;
    r = enter(fn, a0, a1);
    if (result) *result = r;
    if (w->sp_after != w->sp)
        bad = 1, fprintf(stderr, "win64: RSP came back %lld off\n", (long long)(w->sp_after - w->sp));
    for (k = 0; k < MARKS; ++k)
        if (w->got[k] != w->mark[k])
            bad = 1, fprintf(stderr, "win64: %s came back %llx, was %llx\n", names[k], (unsigned long long)w->got[k],
                             (unsigned long long)w->mark[k]);
    for (k = 0; k < 10; ++k)
        if (memcmp(w->xgot[k], w->xmark[k], 16))
            bad = 1, fprintf(stderr, "win64: XMM%u came back %llx:%llx\n", 6 + k, (unsigned long long)w->xgot[k][1],
                             (unsigned long long)w->xgot[k][0]);
    if ((w->csr ^ w->csr_after) & 0xFFC0u) /* the control bits: the flags are a callee's to change */
        bad = 1, fprintf(stderr, "win64: MXCSR came back %x, was %x\n", w->csr_after, w->csr);
    if (w->fault == 1)
        bad = 1, fprintf(stderr, "win64: a call with the stack not 16-aligned\n");
    else if (w->fault)
        bad = 1, fprintf(stderr, "win64: the caller's stack past the shadow was written\n");
    if (u->failures != failures) bad = 1;
    return bad ? -1 : 0;
}
