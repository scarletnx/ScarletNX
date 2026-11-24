/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's guest functions: what ir_codegen.c's snx_ir_emit makes of a function, without LLVM. Each
   basic block's instructions are sailgen's x86-64 emitters' (a64_x64.inc), appended to one SSA region (registers read
   once, flags reused, constants folded) and assembled when the block ends; what joins the blocks is written here: the
   entry's check, the branches (a backward one polling first), the direct and dynamic calls (the dispatch table read
   inline, as snx_lookup reads it), the returns' checks, SVC, BRK and UNDEF, the dispatch block of a function of several
   leaders. The guest's registers are in the CPU structure at every block's start and end, as a block's stores keep
   them (sailgen's x64_a64.h), so a block needs nothing of another's and a call nothing of its caller's.

   The function's ABI is GuestFn's, System V x86-64: the state in RDI, kept in RBX (pushed), the entry in RSI; RSP
   16-aligned after the prologue (the push and a frame of spill slots, its size patched in at the end). Control flow
   is bytes written here (jumps, calls, the frame): the instruction selection derives what computes values.

   The object is ELF x86-64, as LLVM's are: F_<address> global and hidden in the AOT module's units (library.c), so
   an LLVM unit's function calls one of these and the other way; in a JIT unit local, with the unit's table. A call
   is a PLT32 relocation, resolved here where the callee is this object's own */
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "common/utilities/bits.h"
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#if defined(__x86_64__) && !defined(_WIN32) && defined(SNX_HAVE_X64)
#include "common/cpu/recompiler/llvm/elf_object.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && !defined(_WIN32) && defined(SNX_HAVE_X64)
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* a word the emitters can't make (bits.h's sg_fail) takes the function back to LLVM */
static _Thread_local jmp_buf* g_fail;
#define SG_FAIL_JUMP g_fail
#define SGX_FN 1

typedef struct SgXAsm SgXAsm;
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

#include "a64_x64.inc"

/* ---- what mach_codegen.inc asks of the machine ----
   x86-64's ELF; a call's target and a function's entry in RSI; the pins in RBP, R12-R15 (pushed: the callee's) and
   R10, R11 (caller-saved, values' otherwise: reserved, and an exclusive's, an atomic's or CRC32's fixed registers);
   the vector pins in XMM15 down to XMM8 */
#define MG_EM EM_X86_64
#define MG_R_ABS64 R_X86_64_64
#define SGX_FN_TARGET 6
static const signed char mg_pin_hosts[7] = {5, 12, 13, 14, 15, 11, 10};
#define MG_NPINS 7
#define MG_NPINS_FIXED 5
#define MG_PIN_RESERVED(h) ((h) < 12 && (h) != 5)
#define MG_VPIN(k) (15 - (k))
#define MG_NVPINS 8
#define MG_XRESERVED 0u
#define MG_BOUND 1
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend);

#include "common/cpu/recompiler/mach/mach_codegen.inc"

/* ---- the glue: bytes of the ABI and the flow ---- */

static void byte(Unit* u, unsigned char x) { sgx_byte(&u->b, x); }
static void imm(Unit* u, long long v, int bytes) { sgx_imm(&u->b, v, bytes); }
static void rel_label(Unit* u, int label) {
    u->fixups = (Fixup*)grow(u->fixups, &u->cap_fixups, u->nfixups + 1, sizeof *u->fixups);
    u->fixups[u->nfixups].at = u->b.n;
    u->fixups[u->nfixups++].label = label;
    imm(u, 0, 4);
}
static void rel_sym(Unit* u, const char* sym) {
    size_t n = strlen(sym);
    u->relocs = (Reloc*)grow(u->relocs, &u->cap_relocs, u->nrelocs + 1, sizeof *u->relocs);
    u->relocs[u->nrelocs].at = u->b.n;
    u->relocs[u->nrelocs].sym = (char*)malloc(n + 1);
    memcpy(u->relocs[u->nrelocs].sym, sym, n + 1);
    u->relocs[u->nrelocs].type = R_X86_64_PLT32;
    u->relocs[u->nrelocs++].addend = -4;
    imm(u, 0, 4);
}
/* cmp byte [rip + snx_stale + index], 0: the guest changed the code the function at index was made from */
static void stale_test(Unit* u, size_t index) {
    byte(u, 0x80);
    byte(u, 0x3D);
    u->relocs = (Reloc*)grow(u->relocs, &u->cap_relocs, u->nrelocs + 1, sizeof *u->relocs);
    u->relocs[u->nrelocs].at = u->b.n;
    u->relocs[u->nrelocs].sym = strdup("snx_stale");
    u->relocs[u->nrelocs].type = R_X86_64_PC32;
    u->relocs[u->nrelocs++].addend = (long long)index - 5; /* from the end of the instruction, past the imm8 */
    imm(u, 0, 4);
    byte(u, 0);
}
/* the helpers' calls the emitters make (sgx_call): relocations of the unit */
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend) {
    Unit* u = (Unit*)machine;
    size_t n = strlen(sym);
    (void)addend;
    u->relocs = (Reloc*)grow(u->relocs, &u->cap_relocs, u->nrelocs + 1, sizeof *u->relocs);
    u->relocs[u->nrelocs].at = at;
    u->relocs[u->nrelocs].sym = (char*)malloc(n + 1);
    memcpy(u->relocs[u->nrelocs].sym, sym, n + 1);
    u->relocs[u->nrelocs].type = R_X86_64_PLT32;
    u->relocs[u->nrelocs++].addend = -4;
}

/* ---- the glue: bytes of the ABI and the flow ---- */

static void mov_rax_imm(Unit* u, SnxU64 v) {
    if (v <= 0xFFFFFFFFu) {
        byte(u, 0xB8); /* mov eax, imm32 */
        imm(u, (long long)v, 4);
    } else {
        byte(u, 0x48); /* mov rax, imm64 */
        byte(u, 0xB8);
        imm(u, (long long)v, 8);
    }
}
static void mov_rsi_imm(Unit* u, SnxU64 v) {
    if (v <= 0xFFFFFFFFu) {
        byte(u, 0xBE); /* mov esi, imm32 */
        imm(u, (long long)v, 4);
    } else {
        byte(u, 0x48); /* mov rsi, imm64 */
        byte(u, 0xBE);
        imm(u, (long long)v, 8);
    }
}
static void mov_rdi_rbx(Unit* u) {
    byte(u, 0x48);
    byte(u, 0x89);
    byte(u, 0xDF);
}
/* the state's field at off = RAX, and RAX = it */
static void store_rax(Unit* u, int off) {
    byte(u, 0x48);
    byte(u, 0x89);
    byte(u, 0x83);
    imm(u, off, 4);
}
static void load_rax(Unit* u, int off) {
    byte(u, 0x48);
    byte(u, 0x8B);
    byte(u, 0x83);
    imm(u, off, 4);
}
static void set_pc(Unit* u, SnxU64 pc) {
    mov_rax_imm(u, pc);
    store_rax(u, (int)offsetof(CPU, pc));
}
/* call [state->rt + off] */
static void call_rt(Unit* u, int off) {
    load_rax(u, (int)offsetof(CPU, rt));
    byte(u, 0xFF);
    byte(u, 0x90);
    imm(u, off, 4);
}
static void jcc(Unit* u, int cc, int label) {
    byte(u, 0x0F);
    byte(u, (unsigned char)(0x80 | cc));
    rel_label(u, label);
}
static void jmp(Unit* u, int label) {
    byte(u, 0xE9);
    rel_label(u, label);
}
/* what a bound PLT stub's slot is compared with (mach_codegen.inc's MG_BOUND) */
static void mg_if_stale(Unit* u, size_t index, int label) {
    stale_test(u, index);
    jcc(u, 0x5, label);
}
static void mg_if_x_not(Unit* u, unsigned r, SnxU64 value, int label) {
    int p = u->pinned ? u->pin[r] : -1;
    mov_rax_imm(u, value);
    if (p >= 0) {
        /* cmp p, rax */
        byte(u, (unsigned char)(0x48 | (p >> 3))), byte(u, 0x39), byte(u, (unsigned char)(0xC0 | (p & 7)));
    } else {
        /* cmp [rbx + x<r>], rax */
        byte(u, 0x48), byte(u, 0x39), byte(u, 0x83), imm(u, (long long)(offsetof(CPU, x) + 8 * (size_t)r), 4);
    }
    jcc(u, 0x5, label);
}
static void mg_if_pc_not(Unit* u, SnxU64 value, int label) {
    mov_rax_imm(u, value);
    /* cmp [rbx + pc], rax */
    byte(u, 0x48), byte(u, 0x39), byte(u, 0x83), imm(u, (long long)offsetof(CPU, pc), 4);
    jcc(u, 0x5, label);
}
static void mg_target_pc(Unit* u) {
    /* mov rsi, [rbx + pc] */
    byte(u, 0x48), byte(u, 0x8B), byte(u, 0xB3), imm(u, (long long)offsetof(CPU, pc), 4);
}

/* the guest registers the function keeps in host ones: to the state (store), or from it */
/* the pins to the state (store) or from it: every one, or (caller) those in caller-saved registers, which a call
   changes (R10, R11, the XMM registers) */
static void pins_of(Unit* u, SgXBuf* b, int store, int caller) {
    int n;
    if (!u->pinned) return;
    if (u->mempin >= 0 && !store && (!caller || u->mempin == 10 || u->mempin == 11)) {
        int p = u->mempin; /* mov p, [rbx + mem]: read-only, never stored */
        sgx_byte(b, (unsigned char)(0x48 | (p >> 3) << 2));
        sgx_byte(b, 0x8B);
        sgx_byte(b, (unsigned char)(0x83 | (p & 7) << 3));
        sgx_imm(b, (long long)offsetof(CPU, mem), 4);
    }
    for (n = 0; n < 32; ++n) {
        if (u->pin[n] >= 0 && (!caller || u->pin[n] == 10 || u->pin[n] == 11)) {
            int p = u->pin[n];
            sgx_byte(b, (unsigned char)(0x48 | (p >> 3) << 2));
            sgx_byte(b, store ? 0x89 : 0x8B);
            sgx_byte(b, (unsigned char)(0x83 | (p & 7) << 3)); /* [rbx + disp32] */
            sgx_imm(b, (long long)(offsetof(CPU, x) + 8 * (size_t)n), 4);
        }
        if (u->vpin[n] >= 0) {
            int p = u->vpin[n];
            sgx_byte(b, 0xF3); /* movdqu [rbx + disp32], xmm / xmm, [rbx + disp32] */
            if (p >= 8) sgx_byte(b, 0x44);
            sgx_byte(b, 0x0F);
            sgx_byte(b, store ? 0x7F : 0x6F);
            sgx_byte(b, (unsigned char)(0x83 | (p & 7) << 3));
            sgx_imm(b, (long long)offsetof(CPU, v) + 16 * n, 4);
        }
    }
}

static void prologue(Unit* u) {
    byte(u, 0x53); /* push rbx */
    if (u->pinned) /* push rbp, r12, r13, r14, r15 */
        byte(u, 0x55), byte(u, 0x41), byte(u, 0x54), byte(u, 0x41), byte(u, 0x55), byte(u, 0x41), byte(u, 0x56),
            byte(u, 0x41), byte(u, 0x57);
    mov_rdi_rbx(u);
    u->b.p[u->b.n - 2] = 0x89, u->b.p[u->b.n - 1] = 0xFB; /* mov rbx, rdi (the other way) */
    byte(u, 0x48); /* sub rsp, imm32 */
    byte(u, 0x81);
    byte(u, 0xEC);
    u->frame_at = (size_t*)grow(u->frame_at, &u->cap_frame, u->nframe + 1, sizeof *u->frame_at);
    u->frame_at[u->nframe++] = u->b.n;
    imm(u, 0, 4);
    pins(u, 0);
}
/* the frame and the registers the prologue pushed given back (a pinned guest register to the state before: pins) */
static void epilogue(Unit* u) {
    byte(u, 0x48); /* add rsp, imm32 */
    byte(u, 0x81);
    byte(u, 0xC4);
    u->frame_at = (size_t*)grow(u->frame_at, &u->cap_frame, u->nframe + 1, sizeof *u->frame_at);
    u->frame_at[u->nframe++] = u->b.n;
    imm(u, 0, 4);
    if (u->pinned) /* pop r15, r14, r13, r12, rbp */
        byte(u, 0x41), byte(u, 0x5F), byte(u, 0x41), byte(u, 0x5E), byte(u, 0x41), byte(u, 0x5D), byte(u, 0x41),
            byte(u, 0x5C), byte(u, 0x5D);
    byte(u, 0x5B); /* pop rbx */
}
static void leave(Unit* u) {
    pins(u, 1);
    epilogue(u);
    byte(u, 0xC3); /* ret */
}


/* CALLDYN(RSI): the dispatch table's function for the target (CPU.disp, inside [code_lo, code_hi) and aligned), else
   the runtime's miss, called with the state and the target: snx_lookup's, inline. tail: the epilogue and a jump */
static void calldyn(Unit* u, int tail) {
    int miss = new_label(u), found = new_label(u);
    byte(u, 0x48), byte(u, 0x89), byte(u, 0xF0);                                     /* mov rax, rsi */
    byte(u, 0x48), byte(u, 0x2B), byte(u, 0x83), imm(u, (long long)offsetof(CPU, code_lo), 4); /* sub rax, lo */
    byte(u, 0x48), byte(u, 0x8B), byte(u, 0x93), imm(u, (long long)offsetof(CPU, code_hi), 4); /* mov rdx, hi */
    byte(u, 0x48), byte(u, 0x2B), byte(u, 0x93), imm(u, (long long)offsetof(CPU, code_lo), 4); /* sub rdx, lo */
    byte(u, 0x48), byte(u, 0x39), byte(u, 0xD0);                                     /* cmp rax, rdx */
    jcc(u, 0x3, miss);                                                               /* jae */
    byte(u, 0x40), byte(u, 0xF6), byte(u, 0xC6), byte(u, 0x03);                      /* test sil, 3 */
    jcc(u, 0x5, miss);                                                               /* jnz */
    byte(u, 0x48), byte(u, 0xC1), byte(u, 0xE8), byte(u, 0x02);                      /* shr rax, 2 */
    byte(u, 0x48), byte(u, 0x8B), byte(u, 0x93), imm(u, (long long)offsetof(CPU, disp), 4); /* mov rdx, disp */
    byte(u, 0x48), byte(u, 0x8B), byte(u, 0x04), byte(u, 0xC2);                      /* mov rax, [rdx + rax*8] */
    byte(u, 0x48), byte(u, 0x85), byte(u, 0xC0);                                     /* test rax, rax */
    jcc(u, 0x5, found);                                                              /* jnz */
    bind(u, miss);
    load_rax(u, (int)offsetof(CPU, rt));
    byte(u, 0x48), byte(u, 0x8B), byte(u, 0x80), imm(u, (long long)offsetof(RtAPI, miss), 4); /* mov rax, [rax+miss] */
    bind(u, found);
    pins(u, 1); /* the callee reads the guest's registers from the state */
    mov_rdi_rbx(u); /* before the epilogue: its pop gives RBX back to the caller */
    if (tail) epilogue(u);
    byte(u, 0xFF);
    byte(u, tail ? 0xE0 : 0xD0); /* jmp rax / call rax */
    if (!tail) pins(u, 0);
}

static void set_target(Unit* u, SnxU64 target) { mov_rsi_imm(u, target); }
/* call F_target / jmp: the pins stored first, the state in RDI, the target in RSI already */
static void call_sym(Unit* u, const char* sym, int tail) {
    pins(u, 1);
    mov_rdi_rbx(u);
    if (tail) epilogue(u);
    byte(u, tail ? 0xE9 : 0xE8);
    rel_sym(u, sym);
    if (!tail) pins(u, 0);
}

/* if (c->pc != next) return: the callee changed the code it returns to, or left for another pc */
static void check_return(Unit* u, SnxU64 next) {
    int stay = new_label(u), go = new_label(u);
    mov_rax_imm(u, next);
    byte(u, 0x48), byte(u, 0x39), byte(u, 0x83), imm(u, (long long)offsetof(CPU, pc), 4); /* cmp [rbx+pc], rax */
    jcc(u, 0x5, go);
    jmp(u, stay);
    bind(u, go);
    leave(u);
    bind(u, stay);
    if (u->options->stale) {
        /* this function's code changed while the call ran: c->pc is next already */
        stale_test(u, u->index);
        jcc(u, 0x5, go);
    }
}

/* the poll of a backward branch: if (c->poll) { c->pc = resume; c->rt->poll(c); F_start(c, resume); return; } */
static void poll_then(Unit* u, SnxU64 resume) {
    int run = new_label(u);
    char sym[40];
    byte(u, 0x83), byte(u, 0xBB), imm(u, (long long)offsetof(CPU, poll), 4), byte(u, 0x00); /* cmp dword [rbx+poll], 0 */
    jcc(u, 0x4, run);
    pins(u, 1);
    set_pc(u, resume);
    mov_rdi_rbx(u);
    call_rt(u, (int)offsetof(RtAPI, poll));
    mov_rdi_rbx(u);
    epilogue(u);
    mov_rsi_imm(u, resume);
    fname(sym, sizeof sym, "F_", u->f->start);
    byte(u, 0xE9);
    rel_sym(u, sym);
    bind(u, run);
}


/* UNDEF: c->pc = address; c->rt->undef(c, address, word) */
static void mg_undef(Unit* u, SnxU64 address, SnxU32 word) {
    pins(u, 1);
    set_pc(u, address);
    mov_rdi_rbx(u);
    mov_rsi_imm(u, address);
    byte(u, 0xBA), imm(u, (long long)word, 4); /* mov edx, word */
    call_rt(u, (int)offsetof(RtAPI, undef));
}
/* SVC: c->pc = next; c->rt->svcs[n](c, n) or c->rt->svc(c, n), the pins around it */
static void mg_svc(Unit* u, SnxU64 next, unsigned n, int direct) {
    pins(u, 1); /* the SVC reads and writes the guest's registers in the state */
    set_pc(u, next);
    mov_rdi_rbx(u);
    byte(u, 0xBE), imm(u, (long long)n, 4); /* mov esi, n */
    if (direct) {
        load_rax(u, (int)offsetof(CPU, rt));
        byte(u, 0x48), byte(u, 0x8B), byte(u, 0x80), imm(u, (long long)offsetof(RtAPI, svcs), 4);
        byte(u, 0xFF), byte(u, 0x90), imm(u, (long long)(8 * n), 4); /* call [rax + 8n] */
    } else
        call_rt(u, (int)offsetof(RtAPI, svc));
    pins(u, 0);
}
/* BRK: c->pc = address; c->rt->brk(c, address, imm) */
static void mg_brk(Unit* u, SnxU64 address, unsigned imm16) {
    pins(u, 1);
    set_pc(u, address);
    mov_rdi_rbx(u);
    mov_rsi_imm(u, address);
    byte(u, 0xBA), imm(u, (long long)imm16, 4);
    call_rt(u, (int)offsetof(RtAPI, brk));
}
static void mg_entry(Unit* u, size_t index, SnxU64 start) {
    if (u->options->stale) {
        int live = new_label(u);
        stale_test(u, index);
        jcc(u, 0x4, live);
        byte(u, 0x48), byte(u, 0x89), byte(u, 0xF0); /* mov rax, rsi */
        store_rax(u, (int)offsetof(CPU, pc));
        leave(u);
        bind(u, live);
    }
    mov_rax_imm(u, start);
    byte(u, 0x48), byte(u, 0x39), byte(u, 0xC6); /* cmp rsi, rax */
    jcc(u, 0x5, u->mismatch_label);
}
static void mg_dispatch(Unit* u, int lab0) {
    const SnxFunction* f = u->f;
    size_t i;
    if (u->dispatch) {
        byte(u, 0x48), byte(u, 0x89), byte(u, 0xF0); /* mov rax, rsi */
        store_rax(u, (int)offsetof(CPU, pc));
        jmp(u, u->d_label);
    } else
        calldyn(u, 1);
    if (u->dispatch) {
        int run = new_label(u);
        bind(u, u->d_label);
        byte(u, 0x83), byte(u, 0xBB), imm(u, (long long)offsetof(CPU, poll), 4), byte(u, 0x00);
        jcc(u, 0x4, run);
        pins(u, 1);
        mov_rdi_rbx(u);
        call_rt(u, (int)offsetof(RtAPI, poll));
        pins(u, 0);
        bind(u, run);
        load_rax(u, (int)offsetof(CPU, pc));
        for (i = 0; i < f->count; ++i)
            if (f->leaders[i]) {
                SnxU64 a = snx_function_address(f, i);
                byte(u, 0x48), byte(u, 0xBA), imm(u, (long long)a, 8); /* mov rdx, imm64 */
                byte(u, 0x48), byte(u, 0x39), byte(u, 0xD0);        /* cmp rax, rdx */
                jcc(u, 0x4, lab0 + (int)i);
            }
        byte(u, 0x48), byte(u, 0x89), byte(u, 0xC6); /* mov rsi, rax */
        calldyn(u, 1);
    }
}
static int mg_finish(Unit* u, size_t fix0) {
    size_t i;
    /* RSP 16-aligned in the body: the return address and RBX (and RBP, R12-R15 where pinned: 8 more) */
    int size = ((u->a.slots_max * 8 + 15) & ~15) + (u->pinned ? 8 : 0);
    for (i = 0; i < u->nframe; ++i)
        if (u->frame_at[i] + 4 <= u->b.cap) {
            unsigned char* p = u->b.p + u->frame_at[i];
            p[0] = (unsigned char)size, p[1] = (unsigned char)(size >> 8), p[2] = (unsigned char)(size >> 16),
            p[3] = (unsigned char)(size >> 24);
        }
    for (i = fix0; i < u->nfixups; ++i) {
        long long t = u->label_at[u->fixups[i].label];
        if (t < 0) return 0;
        if (u->fixups[i].at + 4 <= u->b.cap)
            snx_link_put32(u->b.p + u->fixups[i].at, (uint32_t)(t - (long long)(u->fixups[i].at + 4)));
    }
    return 1;
}
/* a rel32 of the unit's own function */
static int mg_resolve_local(Unit* u, const Reloc* r, size_t target_at) {
    if (r->type != R_X86_64_PLT32) return 0;
    snx_link_put32(u->b.p + r->at, (uint32_t)((long long)target_at - (long long)(r->at + 4)));
    return 1;
}

#include "common/cpu/recompiler/x64/x64_vec.c"

static unsigned host_features(void);
/* the host's features the back end goes without (snx_x64_without) */
static struct {
    _Atomic unsigned without;
} g_x64_codegen;
/* the back end's SSE code needs SSSE3 and SSE4.1 (2008's processors), and SSE4.2, PCLMULQDQ and POPCNT */
#define X64_BASELINE (SGX_F_SSE3 | SGX_F_SSSE3 | SGX_F_SSE41 | SGX_F_SSE42 | SGX_F_PCLMUL | SGX_F_POPCNT)
int snx_x64_available(void) {
    return (host_features() & X64_BASELINE) == X64_BASELINE;
}
unsigned snx_x64_features(void) { return host_features(); }
void snx_x64_without(unsigned features) {
    atomic_store_explicit(&g_x64_codegen.without, features & ~(unsigned)X64_BASELINE, memory_order_relaxed);
}

/* CPUID's BMI1, BMI2, SHA and LZCNT (ABM) */
static unsigned host_features(void) {
    unsigned a, b, c, d, f = 0;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    if (b & (1u << 3)) f |= SGX_F_BMI1;
    if (b & (1u << 8)) f |= SGX_F_BMI2;
    if (b & (1u << 29)) f |= SGX_F_SHA;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
    if (c & (1u << 5)) f |= SGX_F_LZCNT;
    /* leaf 1's: SSE3, PCLMULQDQ, SSSE3, SSE4.1, SSE4.2, POPCNT, AES; the VEX forms' (AVX, FMA, F16C) where the system
       keeps the YMM state (OSXSAVE, and XCR0's SSE and AVX bits) */
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & 1u) f |= SGX_F_SSE3;
    if (c & (1u << 1)) f |= SGX_F_PCLMUL;
    if (c & (1u << 9)) f |= SGX_F_SSSE3;
    if (c & (1u << 19)) f |= SGX_F_SSE41;
    if (c & (1u << 20)) f |= SGX_F_SSE42;
    if (c & (1u << 23)) f |= SGX_F_POPCNT;
    if (c & (1u << 25)) f |= SGX_F_AES;
    if ((c & (1u << 27)) && (c & (1u << 28))) {
        unsigned lo, hi;
        __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        if ((lo & 6) == 6) {
            f |= SGX_F_AVX;
            if (c & (1u << 12)) f |= SGX_F_FMA;
            if (c & (1u << 29)) f |= SGX_F_F16C;
        }
    }
    return f & ~atomic_load_explicit(&g_x64_codegen.without, memory_order_relaxed);
}

int snx_x64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options, unsigned char* made,
                   unsigned char** object, size_t* size, char* error, size_t error_size) {
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, host_features());
}

typedef void (*FactPut)(void* user, const void* data, size_t size);
static void fact64(FactPut put, void* user, SnxU64 v) {
    unsigned char b[8];
    for (unsigned i = 0; i < 8; ++i) b[i] = (unsigned char)(v >> (8 * i));
    put(user, b, sizeof b);
}
/* what call_or_stub and call_target ask of target: its function (start and index), routed, imported, and a bound PLT
   stub's words, binding and guard, with what the call of its function asks in turn */
static void target_facts(Unit* u, SnxU64 target, FactPut put, void* user) {
    const SnxCodegen* c = u->codegen;
    const SnxFunction* f = snx_codegen_function(c, target);
    SnxU32 w[3] = {0, 0, 0};
    int guard = 0;
    SnxU64 bound = stub_function(u, target, w, &guard);
    fact64(put, user, target);
    fact64(put, user, f ? (SnxU64)(f - c->functions) : ~(SnxU64)0);
    fact64(put, user, (SnxU64)snx_codegen_routed(c, target));
    fact64(put, user, u->options->direct ? (SnxU64)u->options->direct(u->options->direct_user, target) : 0);
    fact64(put, user, bound);
    if (!bound) return;
    fact64(put, user, (SnxU64)guard);
    for (unsigned i = 0; i < 3; ++i) fact64(put, user, w[i]);
    fact64(put, user, bound);
    f = snx_codegen_function(c, bound);
    fact64(put, user, f ? (SnxU64)(f - c->functions) : ~(SnxU64)0);
    fact64(put, user, u->options->direct ? (SnxU64)u->options->direct(u->options->direct_user, bound) : 0);
}

void snx_x64_unit_facts(const SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options,
                        void (*put)(void* user, const void* data, size_t size), void* user) {
    Unit u = {0};
    size_t i, k;
    u.codegen = codegen;
    u.options = options;
    fact64(put, user, host_features());
    fact64(put, user, codegen->features);
    fact64(put, user, (SnxU64)codegen->direct_svc);
    fact64(put, user, (SnxU64)codegen->hook_direct_calls);
    fact64(put, user, (SnxU64)options->jit);
    if (options->jit && options->table_name) put(user, options->table_name, strlen(options->table_name) + 1);
    fact64(put, user, (SnxU64)options->stale);
    fact64(put, user, options->llvm_at);
    fact64(put, user, first);
    fact64(put, user, last);
    for (i = first; i < last; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        fact64(put, user, f->start);
        fact64(put, user, f->count);
        fact64(put, user, f->leader_count);
        for (k = 0; k < f->count; ++k) {
            SnxU64 address = snx_function_address(f, k), next = address + 4;
            SnxU32 word = 0;
            SnxA64Insn insn;
            int guard = 0;
            (void)snx_codegen_word((SnxCodegen*)codegen, address, &word);
            fact64(put, user, address);
            fact64(put, user, word);
            fact64(put, user, (SnxU64)f->leaders[k]);
            snx_a64_decode_word(word, codegen->features, &insn);
            switch (insn.flow) {
                case SNX_A64_FLOW_B:
                case SNX_A64_FLOW_BC:
                case SNX_A64_FLOW_BL: target_facts(&u, snx_a64_branch_target(&insn, address), put, user); break;
                case SNX_A64_FLOW_BR:
                    fact64(put, user, bound_function(&u, address, &guard));
                    fact64(put, user, (SnxU64)guard);
                    break;
                default: break;
            }
            if (!(k + 1 < f->count && snx_function_address(f, k + 1) == next)) target_facts(&u, next, put, user);
        }
    }
}

#else
int snx_x64_available(void) { return 0; }
unsigned snx_x64_features(void) { return 0; }
void snx_x64_without(unsigned features) { (void)features; }
/* no x86-64 back end in this build */
int snx_x64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options, unsigned char* made,
                   unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
void snx_x64_unit_facts(const SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options,
                        void (*put)(void* user, const void* data, size_t size), void* user) {
    (void)codegen, (void)first, (void)last, (void)options, (void)put, (void)user;
}
#endif
