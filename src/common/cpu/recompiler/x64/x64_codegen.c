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
#include <setjmp.h>
#include <stdlib.h>
#if defined(__x86_64__) && !defined(_WIN32) && defined(SNX_HAVE_X64)
#include <elf.h>
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
static _Thread_local const char* g_fail_what; /* why, for SNX_X64_STATS */
static _Thread_local long long g_fail_a, g_fail_b;
#define SG_FAIL_JUMP g_fail
#define SG_FAIL_NOTE(what, a, b) (g_fail_what = (what), g_fail_a = (a), g_fail_b = (b))
#define SGX_FN 1

typedef struct SgXAsm SgXAsm;
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

#include "a64_x64.inc"

/* ---- the unit: its code, labels, relocations, symbols ---- */

typedef struct {
    size_t at;   /* the rel32's place */
    int label;
} Fixup;
typedef struct {
    size_t at;
    char* sym;
    long long addend;
    unsigned type; /* R_X86_64_PLT32 or R_X86_64_PC32 */
} Reloc;
typedef struct {
    char* name;
    size_t at, size;
} Sym;

typedef struct {
    SgXBuf b;
    SgXAsm a;
    SgCtx* g;
    long long* label_at; /* -1 until bound */
    int nlabels, cap_labels;
    Fixup* fixups;
    size_t nfixups, cap_fixups;
    Reloc* relocs;
    size_t nrelocs, cap_relocs;
    Sym* syms;
    size_t nsyms, cap_syms;
    size_t* frame_at; /* this function's frame patches */
    size_t nframe, cap_frame;
    const SnxCodegen* codegen;
    const SnxX64Options* options;
    const SnxFunction* f;
    int pinned;          /* the function keeps guest registers in host ones (pin): RBP, R12-R15 pushed */
    signed char pin[32]; /* each X register's host register, -1 for none */
    size_t index; /* the function's in the codegen: its snx_stale byte */
    SnxU32 word; /* the instruction being made: SNX_X64_STATS names it where the function goes to LLVM */
    int dispatch, d_label, mismatch_label;
    int nstubs, cap_stubs;
    struct Stub {
        int label;
        int kind; /* 0 a branch to target; 1 a poll then target (resume); 2 a return (leave) */
        SnxU64 target;
        SnxU64 address;
    } * stubs;
} Unit;

static void* grow(void* p, size_t* cap, size_t need, size_t each) {
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : 64;
    if (*cap < need) *cap = need;
    p = realloc(p, *cap * each);
    if (!p) {
        fprintf(stderr, "x64 back end: out of memory\n");
        abort();
    }
    return p;
}

static int new_label(Unit* u) {
    size_t cap = (size_t)u->cap_labels;
    u->label_at = (long long*)grow(u->label_at, &cap, (size_t)u->nlabels + 1, sizeof *u->label_at);
    u->cap_labels = (int)cap;
    u->label_at[u->nlabels] = -1;
    return u->nlabels++;
}
static void bind(Unit* u, int label) { u->label_at[label] = (long long)u->b.n; }

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

/* the guest registers the function keeps in host ones: to the state (store), or from it */
static void pins(Unit* u, int store) {
    int n;
    if (!u->pinned) return;
    for (n = 0; n < 32; ++n)
        if (u->pin[n] >= 0) {
            int p = u->pin[n];
            byte(u, (unsigned char)(0x48 | (p >> 3) << 2));
            byte(u, store ? 0x89 : 0x8B);
            byte(u, (unsigned char)(0x83 | (p & 7) << 3)); /* [rbx + disp32] */
            imm(u, (long long)(offsetof(CPU, x) + 8 * (size_t)n), 4);
        }
}
static void pins_flush(void* machine) { pins((Unit*)machine, 1); }

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

/* a function's symbol */
static void fname(char* out, size_t size, const char* prefix, SnxU64 address) {
    snprintf(out, size, "%s%llX", prefix, (unsigned long long)address);
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

/* call(): F_target directly where the target is one of the codegen's functions (not routed through its slot), the
   AOT module's (snx_aot_<target>) in a JIT unit where it has one, CALLDYN otherwise; tail: then return */
static void call_target(Unit* u, SnxU64 target, int tail) {
    const SnxCodegen* c = u->codegen;
    int through_slot = c->hook_direct_calls || snx_codegen_routed(c, target);
    char sym[40];
    mov_rsi_imm(u, target);
    if (!through_slot && snx_codegen_function(c, target))
        fname(sym, sizeof sym, "F_", target);
    else if (!through_slot && u->options->jit && u->options->direct &&
             u->options->direct(u->options->direct_user, target))
        snprintf(sym, sizeof sym, "snx_aot_%llx", (unsigned long long)target);
    else {
        calldyn(u, tail);
        return;
    }
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

static size_t index_of(const SnxFunction* f, SnxU64 address) {
    size_t lo = 0, hi = f->count;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (snx_function_address(f, m) < address) lo = m + 1;
        else
            hi = m;
    }
    return lo;
}

static int add_stub(Unit* u, int kind, SnxU64 target, SnxU64 address) {
    size_t cap = (size_t)u->cap_stubs;
    u->stubs = (struct Stub*)grow(u->stubs, &cap, (size_t)u->nstubs + 1, sizeof *u->stubs);
    u->cap_stubs = (int)cap;
    u->stubs[u->nstubs].label = new_label(u);
    u->stubs[u->nstubs].kind = kind;
    u->stubs[u->nstubs].target = target;
    u->stubs[u->nstubs].address = address;
    return u->stubs[u->nstubs++].label;
}

/* the label a branch to target goes to: the target's own inside the function going forward, else a stub's (the poll
   of a backward one, a tail call out) */
static int branch_label(Unit* u, SnxU64 address, SnxU64 target) {
    if (snx_function_contains(u->f, target)) {
        if (target > address) return (int)index_of(u->f, target);
        return add_stub(u, 1, target, address);
    }
    return add_stub(u, 0, target, address);
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

/* the region so far assembled, and the block's state forgotten: the next instruction starts a new one */
static void flush(Unit* u) {
    SgCtx* g = u->g;
    int k;
    if (g->x.n) sgx_assemble_region(&u->a, &g->x);
    for (k = 0; k < 32; ++k) g->gpr[k] = g->vd[k][0] = g->vd[k][1] = g->vx[k] = g->gk_known[k] = 0;
    g->fp_state[0] = g->fp_state[1] = 0, g->fp_bound[0] = g->fp_bound[1] = 0;
    g->mem = 0;
    g->origin = -1;
    g->x.flags_ins = -1;
}

/* the machine's instructions of a function's region */
enum { SGX_FN_JCC = 16, SGX_FN_CALLDYN, SGX_FN_TAILDYN };
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r) {
    Unit* u = (Unit*)a->machine;
    SgXIns* i = &a->x->ins[at];
    (void)r;
    switch (i->op) {
    case SGX_FN_JCC: jcc(u, i->cc, i->disp); break;
    case SGX_FN_CALLDYN: calldyn(u, 0); break;      /* the target in RSI (in[0]) */
    case SGX_FN_TAILDYN: calldyn(u, 1); break;
    default: sg_fail("a machine instruction of no kind", i->op, 0);
    }
}
static void machine_target(Unit* u, int op, SgD target) {
    SgX* x = &u->g->x;
    int v = sgx_reg(x, &target);
    SgXIns* i = sgx_append(x, SGX_K_MACHINE);
    i->op = (unsigned char)op;
    i->in[0] = v;
    i->fixed[0] = 6; /* RSI */
    i->calls = 1;
}

/* a branch's condition at the region's end: JCC to label on its condition code */
static void jump_if(Unit* u, SgB taken, int label) {
    SgX* x = &u->g->x;
    int from, cc = sgx_bool_cc(x, taken, &from);
    SgXIns* i = sgx_append(x, SGX_K_MACHINE);
    i->op = SGX_FN_JCC;
    i->cc = (unsigned char)cc;
    i->disp = label;
    i->flags_from = from;
}

/* ---- what the Sail roots don't have: ir_ldst.c's, ir_vec.c's and ir_int.c's, on the region's SSA ---- */

/* Xn as an address: SP for 31 */
static SgD base_of(SgCtx* g, unsigned rn) { return sg_GPR_read(g, rn); }
/* Xm as an operand: XZR for 31 */
static SgD xzr_or(SgCtx* g, unsigned rm) { return rm == 31 ? sgx_const(64, 0) : sg_GPR_read(g, rm); }

static void v_load(SgCtx* g, unsigned t, SgD address, int bytes) {
    SgD v = sg_GuestMemRead(g, address, bytes, 0);
    sg__V_write(g, t, sg_d_zext(g, v, 128));
}
static void v_store(SgCtx* g, unsigned t, SgD address, int bytes) {
    sg_GuestMemWrite(g, address, bytes, 0, sg__V_read_bits(g, t, 8 * bytes - 1, 0));
}

/* LDR, STR (immediate: unsigned offset, unscaled, pre- and post-index; register offset; literal), LDP, STP, LDNP,
   STNP of the SIMD&FP registers, B to Q */
static int fp_ldst(Unit* u, SnxU32 word, SnxU64 pc) {
    SgCtx* g = u->g;
    unsigned rt = word & 31, rn = word >> 5 & 31;
    if ((word & 0x3F000000u) == 0x1C000000u) { /* LDR (literal) */
        unsigned opc = word >> 30;
        long long imm = (long long)((word >> 5) & 0x7FFFF) << 45 >> 43; /* imm19 * 4 */
        if (opc == 3) return 0;
        v_load(g, rt, sgx_const(64, pc + (SnxU64)imm), 4 << opc);
        return 1;
    }
    if ((word & 0x3C000000u) == 0x2C000000u) { /* the pairs: opc 101 1 0 idx L imm7 Rt2 Rn Rt */
        unsigned opc = word >> 30, idx = word >> 23 & 7, l = word >> 22 & 1, rt2 = word >> 10 & 31;
        int bytes = 4 << opc;
        long long off = (long long)((word >> 15) & 0x7F) << 57 >> 57;
        SgD base, a;
        if (opc == 3 || idx > 3 || (l && rt == rt2)) return 0;
        off *= bytes;
        base = base_of(g, rn);
        a = idx == 1 ? base : sg_d_add(g, base, sgx_const(64, (SnxU64)off));
        if (l) {
            SgD a2 = sg_d_add(g, a, sgx_const(64, (SnxU64)bytes));
            SgD v1 = sg_GuestMemRead(g, a, bytes, 0), v2 = sg_GuestMemRead(g, a2, bytes, 0);
            sg__V_write(g, rt, sg_d_zext(g, v1, 128));
            sg__V_write(g, rt2, sg_d_zext(g, v2, 128));
        } else {
            SgD v1 = sg__V_read_bits(g, rt, 8 * bytes - 1, 0), v2 = sg__V_read_bits(g, rt2, 8 * bytes - 1, 0);
            sg_GuestMemWrite(g, a, bytes, 0, v1);
            sg_GuestMemWrite(g, sg_d_add(g, a, sgx_const(64, (SnxU64)bytes)), bytes, 0, v2);
        }
        if (idx == 1 || idx == 3) sg_GPR_write(g, rn, idx == 1 ? sg_d_add(g, base, sgx_const(64, (SnxU64)off)) : a);
        return 1;
    }
    if ((word & 0x3F000000u) == 0x3D000000u || (word & 0x3F000000u) == 0x3C000000u) {
        /* size 111 1 0x opc ...: the single registers */
        unsigned size = word >> 30, opc = word >> 22 & 3, scale = (opc & 2) ? 4 : size;
        int load = opc & 1, bytes;
        SgD base, a;
        if ((opc & 2) && size) return 0;
        bytes = 1 << scale;
        base = base_of(g, rn);
        if (word & 0x01000000u) /* unsigned offset */
            a = sg_d_add(g, base, sgx_const(64, (SnxU64)((word >> 10) & 0xFFF) << scale));
        else if (word & 0x00200000u) { /* register offset: 1 Rm option S 10 */
            unsigned rm = word >> 16 & 31, option = word >> 13 & 7, s = word >> 12 & 1;
            SgD m = xzr_or(g, rm);
            if ((word >> 10 & 3) != 2 || !(option & 2)) return 0;
            if (option == 2) m = sg_d_zext(g, sg_d_slice(g, m, 31, 0), 64);
            else if (option == 6)
                m = sg_d_sext(g, sg_d_slice(g, m, 31, 0), 64);
            if (s && scale) m = sg_d_lsl(g, m, scale);
            a = sg_d_add(g, base, m);
        } else { /* imm9: 00 unscaled, 01 post, 11 pre */
            unsigned idx = word >> 10 & 3;
            long long off = (long long)((word >> 12) & 0x1FF) << 55 >> 55;
            if (idx == 2) return 0;
            a = idx == 1 ? base : sg_d_add(g, base, sgx_const(64, (SnxU64)off));
            if (load) v_load(g, rt, a, bytes);
            else
                v_store(g, rt, a, bytes);
            if (idx) sg_GPR_write(g, rn, idx == 1 ? sg_d_add(g, base, sgx_const(64, (SnxU64)off)) : a);
            return 1;
        }
        if (load) v_load(g, rt, a, bytes);
        else
            v_store(g, rt, a, bytes);
        return 1;
    }
    return 0;
}

/* AdvSIMDExpandImm (the Arm ARM's shared/functions/vector) */
static int expand_imm(unsigned op, unsigned cmode, unsigned imm8, SnxU64* out) {
    SnxU64 i = imm8, r;
    unsigned k;
#define REP32(x) ((SnxU64)(SnxU32)(x) * 0x0000000100000001ull)
#define REP16(x) ((SnxU64)(unsigned short)(x) * 0x0001000100010001ull)
    switch (cmode >> 1) {
    case 0: r = REP32(i); break;
    case 1: r = REP32(i << 8); break;
    case 2: r = REP32(i << 16); break;
    case 3: r = REP32(i << 24); break;
    case 4: r = REP16(i); break;
    case 5: r = REP16(i << 8); break;
    case 6: r = cmode & 1 ? REP32(i << 16 | 0xFFFF) : REP32(i << 8 | 0xFF); break;
    default:
        if (!(cmode & 1) && !op) r = i * 0x0101010101010101ull;
        else if (!(cmode & 1)) {
            r = 0;
            for (k = 0; k < 8; ++k)
                if (i >> k & 1) r |= (SnxU64)0xFF << (8 * k);
        } else if (!op) { /* a:NOT(b):Replicate(b,5):c:d:e:f:g:h:Zeros(19) */
            SnxU32 a = i >> 7 & 1, b = i >> 6 & 1;
            SnxU32 f = a << 31 | (b ^ 1) << 30 | (b ? 0x1Fu << 25 : 0) | (SnxU32)(i & 0x3F) << 19;
            r = REP32(f);
        } else { /* a:NOT(b):Replicate(b,8):c:d:e:f:g:h:Zeros(48) */
            SnxU64 a = i >> 7 & 1, b = i >> 6 & 1;
            r = a << 63 | (b ^ 1) << 62 | (b ? 0xFFull << 54 : 0) | (i & 0x3F) << 48;
        }
        break;
    }
#undef REP32
#undef REP16
    *out = r;
    return 1;
}

/* MOVI, MVNI, ORR and BIC (vector, immediate), FMOV (vector, immediate) of singles and doubles */
static int simd_imm(Unit* u, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned rd = word & 31, q = word >> 30 & 1, op = word >> 29 & 1, cmode = word >> 12 & 15, o2 = word >> 11 & 1;
    unsigned imm8 = (word >> 16 & 7) << 5 | (word >> 5 & 31);
    SnxU64 imm;
    SgD value;
    int logic = -1; /* 1 ORR, 0 BIC */
    if ((word & 0x9FF80400u) != 0x0F000400u || o2) return 0;
    if (op && cmode == 15 && !q) return 0;
    expand_imm(op, cmode, imm8, &imm);
    if (cmode < 12 && (cmode & 1)) logic = !op; /* ORR (op 0), BIC (op 1) of 32 and 16 bits */
    else if (op && cmode < 14)
        imm = ~imm; /* MVNI */
    value = sgx_const(128, imm);
    value.kh = q ? imm : 0;
    if (logic >= 0) {
        SgD d = sg__V_read_bits(g, rd, 127, 0);
        value.kh = q ? imm : 0;
        if (logic) value = sgx_logic(g, 1, d, value);
        else {
            SgD n = value;
            n.k = ~n.k;
            n.kh = q ? ~imm : ~0ull;
            value = sgx_logic(g, 0, d, n);
        }
        if (!q) value = sg_d_zext(g, sg_d_slice(g, value, 63, 0), 128);
    }
    sg__V_write(g, rd, value);
    return 1;
}

/* SCVTF, UCVTF (vector, integer), FCVTZS, FCVTZU (vector, integer) of a scalar single or double (AdvSIMD scalar
   two-register misc): the integer in the SIMD&FP register, of the float's width, converted by the same helpers */
static int simd_scalar_cvt(Unit* u, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned rd = word & 31, rn = word >> 5 & 31, sz = word >> 22 & 1, w = sz ? 64 : 32, uns = word >> 29 & 1;
    SgD n, r;
    if ((word & 0xDFBFFC00u) == 0x5E21D800u) { /* SCVTF, UCVTF */
        n = sg__V_read_bits(g, rn, w - 1, 0);
        r = sg_FixedToFP(g, 0, n, 0, (int)uns, sg_FPCR_read(g), SG_FP_ROUNDING_FPCR, w);
    } else if ((word & 0xDFBFFC00u) == 0x5EA1B800u) { /* FCVTZS, FCVTZU: toward zero */
        n = sg__V_read_bits(g, rn, w - 1, 0);
        r = sg_FPToFixed(g, 0, n, 0, (int)uns, sg_FPCR_read(g), 3, w);
    } else
        return 0;
    sg__V_write(g, rd, sg_d_zext(g, r, 128));
    return 1;
}

/* element idx of esize bits of Vn */
static SgD elem_read(SgCtx* g, unsigned n, unsigned idx, unsigned esize) {
    unsigned lo = idx * esize, k = lo / 64, off = lo % 64;
    return sg_d_slice(g, sgx_vd_read(g, n, (int)k), off + esize - 1, off);
}
/* element idx of esize bits of Vd = value, the others kept */
static void elem_write(SgCtx* g, unsigned d, unsigned idx, unsigned esize, SgD value) {
    unsigned lo = idx * esize, k = lo / 64, off = lo % 64;
    SgD half, mask, v;
    if (esize == 64) {
        sgx_vd_write(g, d, (int)k, value);
        return;
    }
    half = sgx_vd_read(g, d, (int)k);
    mask = sgx_const(64, ~(sg_ones(esize) << off));
    v = sg_d_zext(g, sg_d_slice(g, value, esize - 1, 0), 64);
    if (off) v = sg_d_lsl(g, v, off);
    sgx_vd_write(g, d, (int)k, sg_d_or(g, sg_d_and(g, half, mask), v));
}
/* esize bits replicated over a doubleword */
static SgD replicate64(SgCtx* g, SgD value, unsigned esize) {
    SgD v = sg_d_zext(g, sg_d_slice(g, value, esize - 1, 0), 64);
    for (; esize < 64; esize *= 2) v = sg_d_or(g, v, sg_d_lsl(g, v, esize));
    return v;
}
/* imm5's element size (the lowest set bit: B, H, S, D) and index; 0 for an encoding with none */
static unsigned imm5_esize(unsigned imm5, unsigned* idx) {
    unsigned k;
    for (k = 0; k < 4; ++k)
        if (imm5 >> k & 1) {
            *idx = imm5 >> (k + 1);
            return 8u << k;
        }
    return 0;
}

/* UMOV, SMOV, DUP (element: scalar and vector), DUP (general), INS (general), INS (element): the AdvSIMD copies */
static int simd_copy(Unit* u, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned rd = word & 31, rn = word >> 5 & 31, imm5 = word >> 16 & 31, q = word >> 30 & 1, idx, esize;
    if ((word & 0x9FE08400u) != 0x0E000400u && (word & 0xDFE08400u) != 0x5E000400u) return 0;
    esize = imm5_esize(imm5, &idx);
    if (!esize) return 0;
    if ((word & 0xFFE0FC00u) == 0x5E000400u) { /* DUP (element), scalar: MOV Sd, Vn.S[i] */
        sg__V_write(g, rd, sg_d_zext(g, elem_read(g, rn, idx, esize), 128));
        return 1;
    }
    if ((word & 0xBFE0FC00u) == 0x0E003C00u) { /* UMOV: Q for D */
        SgD e;
        if ((esize == 64) != q) return 0;
        e = sg_d_zext(g, elem_read(g, rn, idx, esize), 64);
        if (rd != 31) sg_GPR_write(g, rd, e);
        return 1;
    }
    if ((word & 0xBFE0FC00u) == 0x0E002C00u) { /* SMOV: to W (Q 0) of B and H, to X (Q 1) of B, H and S */
        SgD e;
        if (esize == 64 || (!q && esize == 32)) return 0;
        e = sg_d_sext(g, elem_read(g, rn, idx, esize), q ? 64 : 32);
        if (!q) e = sg_d_zext(g, e, 64);
        if (rd != 31) sg_GPR_write(g, rd, e);
        return 1;
    }
    if ((word & 0xBFE0FC00u) == 0x0E000400u || (word & 0xBFE0FC00u) == 0x0E000C00u) { /* DUP: element, general */
        SgD e, r;
        if (esize == 64 && !q) return 0;
        e = (word & 0x800u) ? (rn == 31 ? sgx_const(64, 0) : sg_GPR_read(g, rn)) : elem_read(g, rn, idx, esize);
        r = replicate64(g, e, esize);
        sgx_vd_write(g, rd, 0, r);
        sgx_vd_write(g, rd, 1, q ? r : sgx_const(64, 0));
        return 1;
    }
    if ((word & 0xFFE0FC00u) == 0x4E001C00u) { /* INS (general) */
        elem_write(g, rd, idx, esize, rn == 31 ? sgx_const(64, 0) : sg_GPR_read(g, rn));
        return 1;
    }
    if ((word & 0xFFE08400u) == 0x6E000400u) { /* INS (element): imm4 the source's index */
        unsigned from = (word >> 11 & 15) >> (esize == 8 ? 0 : esize == 16 ? 1 : esize == 32 ? 2 : 3);
        elem_write(g, rd, idx, esize, elem_read(g, rn, from, esize));
        return 1;
    }
    return 0;
}

/* SYS, MRS and MSR as ir_int.c's sys(): what EL0 reads and writes on the Switch, the cache maintenance it does; 0
   for the rest (UNDEF there, LLVM's) */
#define SYSREG(op1, crn, crm, op2) ((1u << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
static int sys(Unit* u, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned rt = word & 31, op2 = word >> 5 & 7, crm = word >> 8 & 15, crn = word >> 12 & 15, op1 = word >> 16 & 7;
    unsigned op0 = word >> 19 & 3, l = word >> 21 & 1;
    if ((word & 0xFFC00000u) != 0xD5000000u) return 0;
    if (op0 == 3) {
        SnxU32 key = SYSREG(op1, crn, crm, op2);
        if (l) {
            SgD v;
            SgX* x = sgx(g);
            switch (key) {
            case SYSREG(3, 13, 0, 2):
            case SYSREG(3, 13, 0, 3): {
                SgXIns* i = sgx_append(x, SGX_K_LOAD);
                i->w = 64;
                i->disp = key == SYSREG(3, 13, 0, 2) ? (int)offsetof(CPU, tpidr) : (int)offsetof(CPU, tpidrro);
                i->def = sgx_vreg(x, x->n - 1, 64);
                v = sgx_val(64, i->def);
                break;
            }
            case SYSREG(3, 4, 4, 0):
            case SYSREG(3, 4, 4, 1): {
                SgXIns* i = sgx_append(x, SGX_K_LOAD);
                i->w = 32;
                i->disp = key == SYSREG(3, 4, 4, 0) ? (int)offsetof(CPU, fpcr) : (int)offsetof(CPU, fpsr);
                i->def = sgx_vreg(x, x->n - 1, 32);
                v = sgx_val(64, i->def);
                break;
            }
            case SYSREG(3, 4, 2, 0): /* NZCV from PSTATE's fields, N in bit 31 */
                v = sg_d_zext(g, sg_d_concat(g, sg_d_concat(g, sg_PSTATE_read(g, SG_PSTATE_N), sg_PSTATE_read(g, SG_PSTATE_Z)),
                                             sg_d_concat(g, sg_PSTATE_read(g, SG_PSTATE_C), sg_PSTATE_read(g, SG_PSTATE_V))), 64);
                v = sg_d_lsl(g, v, 28);
                break;
            /* the constants ir_int.c's read_sysreg gives: CTR_EL0 (SCTLR_EL1.UCT), DCZID_EL0, CNTFRQ_EL0 */
            case SYSREG(3, 0, 0, 1): v = sgx_const(64, 0x8444C004u); break;
            case SYSREG(3, 0, 0, 7): v = sgx_const(64, 4); break;
            case SYSREG(3, 14, 0, 0): v = sgx_const(64, 19200000u); break;
            /* the counters: the runtime's clock, a call (no other way) */
            case SYSREG(3, 14, 0, 1):
            case SYSREG(3, 14, 0, 2): {
                SgD k = sgx_const(32, key);
                v = sgx_call(g, "snx_mrs", 0, 1, &k, 1, 64);
                break;
            }
            default: return -1; /* UNDEF at EL0 on the Switch, as ir_int.c's read_sysreg leaves it */
            }
            if (rt != 31) sg_GPR_write(g, rt, v);
            return 1;
        }
        switch (key) {
        case SYSREG(3, 13, 0, 2): {
            SgD v = xzr_or(g, rt);
            SgX* x = sgx(g);
            int r = sgx_reg(x, &v);
            SgXIns* i = sgx_append(x, SGX_K_STORE);
            i->w = 64;
            i->in[0] = r;
            i->disp = (int)offsetof(CPU, tpidr);
            return 1;
        }
        case SYSREG(3, 4, 4, 1): /* FPSR: the bits the A57 keeps (integer.c's A57_FPSR_MASK) */
            sgx_state_store(g, (int)offsetof(CPU, fpsr), 32, sg_d_and(g, sg_d_slice(g, xzr_or(g, rt), 31, 0), sgx_const(32, 0xF800009Fu)));
            g->fp_state[0] = g->fp_state[1] = 0, g->fp_bound[0] = g->fp_bound[1] = 0;
            return 1;
        case SYSREG(3, 4, 4, 0): { /* FPCR: the runtime's hook too (snx_msr), a call */
            SgD args[2];
            args[0] = sgx_const(32, key);
            args[1] = xzr_or(g, rt);
            if (key == SYSREG(3, 4, 4, 0) && !(g->features & SNX_A64_FEAT_FP16))
                args[1] = sg_d_and(g, args[1], sgx_const(64, ~(SnxU64)(1u << 19)));
            sgx_call(g, "snx_msr", 0, 1, args, 2, 0);
            return 1;
        }
        case SYSREG(3, 4, 2, 0): /* NZCV from bits 31:28 */
            sg_PSTATE_write(g, SG_PSTATE_N, 4, sg_d_slice(g, xzr_or(g, rt), 31, 28));
            g->origin = -1;
            return 1;
        default: return -1; /* read-only at EL0, or EL1's: UNDEF, as ir_int.c's write_sysreg leaves it */
        }
    }
    if (op0 == 1 && !l && op1 == 3 && crn == 7 && op2 == 1) {
        SgD a = xzr_or(g, rt);
        switch (crm) {
        case 4: { /* DC ZVA: the 64 bytes of the block at a zeroed, four 16-byte stores */
            SgX* x = sgx(g);
            SgD block = sg_d_and(g, a, sgx_const(64, ~(SnxU64)63));
            int z = sgx_xconst(x, 0, 0), base = sgx_mem(g), index = sgx_reg(x, &block), k, in[4] = {0, z, 0, 0};
            for (k = 0; k < 4; ++k)
                sgx_vins(x, &sgx_v_movdqu_st, 0, 0, in, 0, 0, SGX_VM_GUEST, base, index, 16 * k, 0, 0);
            return 1;
        }
        case 5: sgx_call(g, "snx_ic_ivau", 0, 1, &a, 1, 0); return 1;
        case 10:
        case 11:
        case 14: return 1;
        default: return -1;
        }
    }
    return -1; /* the rest of SYS and SYSL: UNDEF, as ir_int.c's sys() */
}
#undef SYSREG

#include "common/cpu/recompiler/x64/x64_vec.c"

/* ---- the SIMD&FP structures: LD1-LD4, ST1-ST4 (multiple and single), LD1R-LD4R ---- */

/* bytes 8 or 16 of guest memory at address as an XMM register (MOVQ, MOVDQU), and the other way */
static int vload(SgCtx* g, SgD address, int bytes) {
    SgX* x = sgx(g);
    int base = sgx_mem(g), index = sgx_reg(x, &address);
    return sgx_vins(x, bytes == 16 ? &sgx_v_movdqu_ld : &sgx_v_movq_ld, 0, 0, NULL, 0, 1, SGX_VM_GUEST, base, index, 0,
                    0, 0);
}
static void vstore(SgCtx* g, SgD address, int r, int bytes) {
    SgX* x = sgx(g);
    int base = sgx_mem(g), index = sgx_reg(x, &address), in[4] = {0, r, 0, 0};
    sgx_vins(x, bytes == 16 ? &sgx_v_movdqu_st : &sgx_v_movq_st, 0, 0, in, 0, 0, SGX_VM_GUEST, base, index, 0, 0, 0);
}

/* 1 made, 0 not one of these, -1 UNDEF */
static int struct_ldst(Unit* u, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned q = word >> 30 & 1, l = word >> 22 & 1, post = word >> 23 & 1, rm = word >> 16 & 31;
    unsigned rn = word >> 5 & 31, rt = word & 31, size = word >> 10 & 3;
    SgD base, a;
    unsigned total, k;
    if ((word & 0xBF800000u) == 0x0C000000u || (word & 0xBF800000u) == 0x0C800000u) {
        /* multiple structures: opcode's registers and elements of each structure */
        unsigned opc = word >> 12 & 15, rpt, selem, len = q ? 16 : 8, eb = 1u << size, ne = len / eb, r, e, s;
        if ((!post && rm) || (word >> 21 & 1)) return 0;
        switch (opc) {
        case 0: rpt = 1, selem = 4; break;
        case 2: rpt = 4, selem = 1; break;
        case 4: rpt = 1, selem = 3; break;
        case 6: rpt = 3, selem = 1; break;
        case 7: rpt = 1, selem = 1; break;
        case 8: rpt = 1, selem = 2; break;
        case 10: rpt = 2, selem = 1; break;
        default: return -1;
        }
        if (size == 3 && !q && selem > 1) return -1;
        base = sg_GPR_read(g, rn);
        total = rpt * selem * len;
        if (selem == 1) {
            for (r = 0; r < rpt; ++r) {
                a = sg_d_add(g, base, sgx_const(64, r * len));
                if (l) vwrite(g, (rt + r) & 31, vload(g, a, (int)len), (int)q);
                else
                    vstore(g, a, vread(g, (rt + r) & 31), (int)len);
            }
        } else {
            /* interleaved: structure e's element s is V[t + s]'s element e, one at a time */
            int regs[4];
            for (s = 0; s < selem; ++s) regs[s] = l ? vzero(g) : vread(g, (rt + s) & 31);
            for (e = 0; e < ne; ++e)
                for (s = 0; s < selem; ++s) {
                    a = sg_d_add(g, base, sgx_const(64, (e * selem + s) * eb));
                    if (l) regs[s] = lane_put(g, size, regs[s], (int)e, sg_GuestMemRead(g, a, eb, 0));
                    else
                        sg_GuestMemWrite(g, a, eb, 0, lane_get(g, size, regs[s], (int)e));
                }
            if (l)
                for (s = 0; s < selem; ++s) vwrite(g, (rt + s) & 31, regs[s], (int)q);
        }
    } else if ((word & 0xBF9F0000u) == 0x0D000000u || (word & 0xBF800000u) == 0x0D800000u) {
        /* single structure: one element of selem registers (or each register's every element: LDnR) */
        unsigned opc = word >> 13 & 7, s_bit = word >> 12 & 1, rbit = word >> 21 & 1, selem = ((opc & 1) << 1 | rbit) + 1;
        unsigned scale = opc >> 1, index, eb, s;
        int replicate = 0;
        if (!post && rm) return 0;
        switch (scale) {
        case 0: index = q << 3 | s_bit << 2 | size; break;
        case 1:
            if (size & 1) return -1;
            index = q << 2 | s_bit << 1 | size >> 1;
            break;
        case 2:
            if (size & 2) return -1;
            if (!(size & 1)) index = q << 1 | s_bit;
            else {
                if (s_bit) return -1;
                index = q;
                scale = 3;
            }
            break;
        default:
            if (!l || s_bit) return -1;
            scale = size;
            replicate = 1;
            index = 0;
            break;
        }
        eb = 1u << scale;
        base = sg_GPR_read(g, rn);
        total = selem * eb;
        for (s = 0; s < selem; ++s) {
            unsigned t = (rt + s) & 31;
            a = sg_d_add(g, base, sgx_const(64, s * eb));
            if (replicate) {
                int v = lane_put(g, scale, vzero(g), 0, sg_GuestMemRead(g, a, eb, 0));
                vwrite(g, t, vdup(g, scale, v, 0), (int)q);
            } else if (l)
                vwrite(g, t, lane_put(g, scale, vread(g, t), (int)index, sg_GuestMemRead(g, a, eb, 0)), 1);
            else
                sg_GuestMemWrite(g, a, eb, 0, lane_get(g, scale, vread(g, t), (int)index));
        }
    } else
        return 0;
    if (post) /* the base moved past what was read or written, or by Xm */
        sg_GPR_write(g, rn, sg_d_add(g, sg_GPR_read(g, rn), rm == 31 ? sgx_const(64, total) : sg_GPR_read(g, rm)));
    (void)k;
    return 1;
}

/* the instruction at address, word; 0 where the function goes to LLVM */
static int instruction(Unit* u, size_t index, SnxU64 address, SnxU32 word) {
    SgCtx* g = u->g;
    SgX* x = &g->x;
    const SnxFunction* f = u->f;
    SnxA64Insn insn;
    SnxU64 next = address + 4, target;
    int terminal = 0;
    snx_a64_decode_word(word, u->codegen->features, &insn);
    /* a long block is several regions: the guest's registers are in the state at each one's end */
    if (x->n > SGX_MAXI - SGX_MAXI / 4 || x->nv > SGX_MAXV - SGX_MAXV / 4 || x->nb > SGX_MAXB - SGX_MAXB / 4) flush(u);
    g->word = word;
    g->pc = address;
    g->start = x->n;
    g->branched = g->undefined = 0;
    g->taken = 0;
    g->exclusive_count = g->exclusive_next = 0;
    g->status = 0;
    switch (insn.flow) {
    case SNX_A64_FLOW_UNDEF:
    undef:
        flush(u);
        pins(u, 1);
        set_pc(u, address);
        mov_rdi_rbx(u);
        mov_rsi_imm(u, address);
        byte(u, 0xBA), imm(u, (long long)word, 4); /* mov edx, word */
        call_rt(u, (int)offsetof(RtAPI, undef));
        leave(u);
        return 1;
    case SNX_A64_FLOW_PLAIN:
        if (insn.feature & ~(SnxU64)SNX_A64_TARGET_OUNCE) goto undef;
        if (insn.op == SNX_A64_OP_SYS) {
            int k = sys(u, word);
            if (k < 0) goto undef;
            if (!k) return 0;
            break;
        }
        if (fp_ldst(u, word, address) || simd_imm(u, word) || simd_scalar_cvt(u, word) || simd_copy(u, word)) break;
        {
            int v = struct_ldst(u, word);
            if (v < 0) goto undef;
            if (v) break;
        }
        {
            int v = vec(u, &insn, word);
            if (v < 0) goto undef;
            if (v) break;
        }
        if (!sg_decode(g) || x->unmade) return 0;
        if (g->undefined) {
            /* the Sail's UNDEF (SGX_K_UNDEF set the pc and called the runtime): the function returns */
            flush(u);
            leave(u);
            return 1;
        }
        break;
    case SNX_A64_FLOW_B:
        flush(u);
        target = snx_a64_branch_target(&insn, address);
        if (snx_function_contains(f, target)) {
            if (target <= address) poll_then(u, target);
            jmp(u, (int)index_of(f, target));
        } else
            call_target(u, target, 1);
        terminal = 1;
        break;
    case SNX_A64_FLOW_BC:
        if (!sg_decode(g) || x->unmade || !g->branched) return 0;
        target = snx_a64_branch_target(&insn, address);
        if (g->taken) jump_if(u, g->taken, branch_label(u, address, target));
        flush(u);
        if (!g->taken) jmp(u, branch_label(u, address, target));
        break;
    case SNX_A64_FLOW_BL:
        target = snx_a64_branch_target(&insn, address);
        sg_GPR_write(g, 30, sgx_const(64, next));
        flush(u);
        call_target(u, target, 0);
        check_return(u, next);
        break;
    case SNX_A64_FLOW_BLR:
        if (!sg_decode(g) || x->unmade || !g->branched) return 0; /* it writes the link register */
        machine_target(u, SGX_FN_CALLDYN, g->target);
        flush(u);
        check_return(u, next);
        break;
    case SNX_A64_FLOW_BR:
        if (!sg_decode(g) || x->unmade || !g->branched) return 0;
        if (u->dispatch) {
            SgD t = g->target;
            int v = sgx_reg(x, &t);
            SgXIns* st = sgx_append(x, SGX_K_STORE);
            st->w = 64;
            st->in[0] = v;
            st->disp = (int)offsetof(CPU, pc);
            flush(u);
            jmp(u, u->d_label);
        } else {
            machine_target(u, SGX_FN_TAILDYN, g->target);
            flush(u);
        }
        terminal = 1;
        break;
    case SNX_A64_FLOW_RET:
        if (!sg_decode(g) || x->unmade || !g->branched) return 0;
        {
            SgD t = g->target;
            int v = sgx_reg(x, &t);
            SgXIns* st = sgx_append(x, SGX_K_STORE);
            st->w = 64;
            st->in[0] = v;
            st->disp = (int)offsetof(CPU, pc);
        }
        flush(u);
        leave(u);
        terminal = 1;
        break;
    case SNX_A64_FLOW_SVC: {
        unsigned n = (word >> 5) & 0xFFFF;
        flush(u);
        pins(u, 1); /* the SVC reads and writes the guest's registers in the state */
        set_pc(u, next);
        mov_rdi_rbx(u);
        byte(u, 0xBE), imm(u, (long long)n, 4); /* mov esi, n */
        if (u->codegen->direct_svc && n < SNX_SVC_COUNT) {
            /* c->rt->svcs[n](c, n) */
            load_rax(u, (int)offsetof(CPU, rt));
            byte(u, 0x48), byte(u, 0x8B), byte(u, 0x80), imm(u, (long long)offsetof(RtAPI, svcs), 4);
            byte(u, 0xFF), byte(u, 0x90), imm(u, (long long)(8 * n), 4); /* call [rax + 8n] */
        } else
            call_rt(u, (int)offsetof(RtAPI, svc));
        pins(u, 0);
        break;
    }
    case SNX_A64_FLOW_BRK:
        flush(u);
        pins(u, 1);
        set_pc(u, address);
        mov_rdi_rbx(u);
        mov_rsi_imm(u, address);
        byte(u, 0xBA), imm(u, (long long)((word >> 5) & 0xFFFF), 4);
        call_rt(u, (int)offsetof(RtAPI, brk));
        leave(u);
        terminal = 1;
        break;
    default: break; /* ERET, an exception: none comes out of a decode */
    }
    /* off the end, or the next instruction not the function's (its addresses are sorted: next is the following
       index's where the function has it) */
    if (!terminal && !(index + 1 < f->count && snx_function_address(f, index + 1) == next)) {
        flush(u);
        call_target(u, next, 1);
    }
    return 1;
}

/* the guest registers the function keeps in host registers (RBP, R12-R15, callee-saved: no call changes them) for
   its whole run, written to the state where something else may read it (a call, SVC, BRK, UNDEF, a poll, the
   exits) and read back after: the five most used, a register field of an instruction counting 1, 8 inside a loop
   (a backward branch's span). None in a small function, or one with a call of the runtime inside a block that may
   read the state (a system register's helper, WFE, SEV) */
static void choose_pins(Unit* u, const SnxFunction* f) {
    static const signed char hosts[5] = {5, 12, 13, 14, 15};
    unsigned weight[32], k, best;
    unsigned char* loop;
    size_t i;
    memset(u->pin, -1, sizeof u->pin);
    u->pinned = 0;
    if (f->count < 8 || getenv("SNX_X64_NO_PINS")) return;
    loop = (unsigned char*)calloc(f->count + 1, 1);
    if (!loop) return;
    memset(weight, 0, sizeof weight);
    for (i = 0; i < f->count; ++i) {
        SnxU64 a = snx_function_address(f, i);
        SnxU32 w = 0;
        SnxA64Insn insn;
        (void)snx_codegen_word((SnxCodegen*)u->codegen, a, &w);
        snx_a64_decode_word(w, u->codegen->features, &insn);
        if (insn.op == SNX_A64_OP_SYS || insn.op == SNX_A64_OP_WFE || insn.op == SNX_A64_OP_SEV ||
            insn.op == SNX_A64_OP_SEVL) {
            free(loop);
            return;
        }
        if (insn.flow == SNX_A64_FLOW_B || insn.flow == SNX_A64_FLOW_BC) {
            SnxU64 t = snx_a64_branch_target(&insn, a);
            if (t <= a && snx_function_contains(f, t)) {
                size_t j;
                for (j = index_of(f, t); j <= i; ++j)
                    if (loop[j] < 3) ++loop[j]; /* the depth of loops, to 3 */
            }
        }
    }
    for (i = 0; i < f->count; ++i) {
        SnxU32 w = 0;
        unsigned wt;
        (void)snx_codegen_word((SnxCodegen*)u->codegen, snx_function_address(f, i), &w);
        wt = 1u << (3 * loop[i]);
        /* the general-purpose register fields by the encoding's group: none in SIMD and FP data processing (bits
           27-25 111); a load's or store's base, and its registers where V (bit 26) is clear (a pair's second, bits
           14-10, where bits 29-27 are 101; the register offset, bits 20-16, where bits 29-27 are 111, 24 clear and
           21 set); the others' Rd, Rn, Rm (and Ra, bits 14-10, of the three-source group, bits 28-24 11011) */
        if ((w >> 25 & 7) == 7) continue;
        if ((w >> 25 & 5) == 4) {
            weight[w >> 5 & 31] += wt;
            if (!(w >> 26 & 1)) {
                weight[w & 31] += wt;
                if ((w >> 27 & 7) == 5) weight[w >> 10 & 31] += wt;
            }
            if ((w >> 27 & 7) == 7 && !(w >> 24 & 1) && (w >> 21 & 1) && (w >> 10 & 3) == 2) weight[w >> 16 & 31] += wt;
            continue;
        }
        if ((w >> 26 & 7) == 5) { /* branches: CBZ's and TBZ's Rt, BR's and RET's Rn */
            if ((w >> 25 & 0x3F) == 0x1A || (w >> 25 & 0x3F) == 0x1B) weight[w & 31] += wt;
            else if ((w >> 25) == 0x6B)
                weight[w >> 5 & 31] += wt;
            continue;
        }
        weight[w & 31] += wt;
        weight[w >> 5 & 31] += wt;
        if ((w >> 25 & 7) == 5) { /* data processing (register) */
            weight[w >> 16 & 31] += wt;
            if ((w >> 24 & 31) == 0x1B) weight[w >> 10 & 31] += wt;
        }
    }
    free(loop);
    for (k = 0; k < 5; ++k) {
        unsigned r, top = 0;
        best = 32;
        for (r = 0; r < 32; ++r)
            if (u->pin[r] < 0 && weight[r] > top) top = weight[r], best = r;
        if (best == 32 || top < 16) break;
        u->pin[best] = hosts[k];
        u->pinned = 1;
    }
}

/* the function's code, or 0 (the unit as it was) where an instruction isn't this back end's */
static int function(Unit* u, const SnxFunction* f, size_t index) {
    size_t i, at0 = u->b.n, fix0 = u->nfixups, rel0 = u->nrelocs, sym0 = u->nsyms;
    int lab0 = u->nlabels, ok = 1;
    jmp_buf jb;
    jmp_buf* saved = g_fail;
    char sym[40];
    u->f = f;
    u->index = index;
    u->nframe = 0;
    u->a.cold.n = 0;
    u->a.ncjumps = u->a.nccalls = 0;
    u->a.npool = u->a.nrips = 0;
    u->a.slots_max = 0;
    u->nstubs = 0;
    g_fail = &jb;
    if (setjmp(jb)) {
        ok = 0;
        goto out;
    }
    /* labels: each leader's, by index */
    for (i = 0; i < f->count; ++i) new_label(u);
    u->dispatch = f->leader_count > 1;
    u->d_label = u->dispatch ? new_label(u) : -1;
    u->mismatch_label = new_label(u);
    fname(sym, sizeof sym, "F_", f->start);
    u->syms = (Sym*)grow(u->syms, &u->cap_syms, u->nsyms + 1, sizeof *u->syms);
    u->syms[u->nsyms].name = strdup(sym);
    u->syms[u->nsyms].at = u->b.n;
    u->nsyms++;
    flush(u); /* no block state of another function's */
    choose_pins(u, f);
    u->g->pinned = u->pinned;
    memcpy(u->g->pin, u->pin, sizeof u->pin);
    u->a.pin_flush = pins_flush;
    prologue(u);
    if (u->options->stale) {
        /* if (snx_stale[index]) { c->pc = e; return; } */
        int live = new_label(u);
        stale_test(u, index);
        jcc(u, 0x4, live);
        byte(u, 0x48), byte(u, 0x89), byte(u, 0xF0); /* mov rax, rsi */
        store_rax(u, (int)offsetof(CPU, pc));
        leave(u);
        bind(u, live);
    }
    /* if (e != start) { c->pc = e; goto D; } or { CALLDYN(e); return; } */
    mov_rax_imm(u, f->start);
    byte(u, 0x48), byte(u, 0x39), byte(u, 0xC6); /* cmp rsi, rax */
    jcc(u, 0x5, u->mismatch_label);
    if (f->count && snx_function_address(f, 0) != f->start) jmp(u, (int)index_of(f, f->start));
    {
        /* SNX_X64_STATS=2: every instruction the function goes to LLVM for, by op ("x64op <function> <op>"), its
           region dropped and the next one made: what would make the most functions this back end's */
        static int every = -1;
        volatile size_t vi = 0;
        volatile int missed = 0;
        if (every < 0) every = getenv("SNX_X64_STATS") && atoi(getenv("SNX_X64_STATS")) >= 2;
        if (setjmp(jb)) {
            if (!every) {
                ok = 0;
                goto out;
            }
        missed:
            {
                SnxA64Insn insn;
                snx_a64_decode_word(u->word, u->codegen->features, &insn);
                fprintf(stderr, "x64op %llX %s %08X %s (%lld, %lld)\n", (unsigned long long)f->start,
                        snx_a64_op_text(insn.op), (unsigned)u->word, g_fail_what ? g_fail_what : "not made", g_fail_a,
                        g_fail_b);
                g_fail_what = NULL;
            }
            missed = 1;
            u->g->x.n = 0;
            u->g->x.nv = 1;
            u->g->x.nb = 1;
            flush(u);
            ++vi;
        }
        for (; vi < f->count; ++vi) {
            SnxU64 address = snx_function_address(f, vi);
            SnxU32 word = 0;
            if (f->leaders[vi]) {
                flush(u);
                bind(u, lab0 + (int)vi);
            }
            (void)snx_codegen_word((SnxCodegen*)u->codegen, address, &word);
            u->word = word;
            g_fail_what = NULL;
            if (!instruction(u, vi, address, word)) {
                if (every) goto missed;
                ok = 0;
                goto out;
            }
        }
        if (missed) {
            ok = 0;
            goto out;
        }
    }
    flush(u);
    leave(u); /* off the end */
    bind(u, u->mismatch_label);
    if (u->dispatch) {
        byte(u, 0x48), byte(u, 0x89), byte(u, 0xF0); /* mov rax, rsi */
        store_rax(u, (int)offsetof(CPU, pc));
        jmp(u, u->d_label);
    } else
        calldyn(u, 1);
    if (u->dispatch) {
        /* D: the poll, then switch (c->pc) over the leaders, else CALLDYN(c->pc) */
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
    for (i = 0; i < (size_t)u->nstubs; ++i) {
        struct Stub s = u->stubs[i];
        bind(u, s.label);
        if (s.kind == 1) {
            poll_then(u, s.target);
            jmp(u, (int)index_of(f, s.target));
        } else
            call_target(u, s.target, 1);
    }
    /* the cold paths (the FP helpers' calls), then the constant pool */
    sgx_cold_emit(&u->a);
    sgx_pool_emit(&u->a);
    /* the frame's size, in the prologue and every epilogue */
    {
        /* RSP 16-aligned in the body: the return address and RBX (and RBP, R12-R15 where pinned: 8 more) */
        int size = ((u->a.slots_max * 8 + 15) & ~15) + (u->pinned ? 8 : 0);
        for (i = 0; i < u->nframe; ++i)
            if (u->frame_at[i] + 4 <= u->b.cap) {
                unsigned char* p = u->b.p + u->frame_at[i];
                p[0] = (unsigned char)size, p[1] = (unsigned char)(size >> 8), p[2] = (unsigned char)(size >> 16),
                p[3] = (unsigned char)(size >> 24);
            }
    }
    /* the labels' jumps */
    for (i = fix0; i < u->nfixups; ++i) {
        long long t = u->label_at[u->fixups[i].label];
        if (t < 0) {
            ok = 0;
            goto out;
        }
        if (u->fixups[i].at + 4 <= u->b.cap) snx_link_put32(u->b.p + u->fixups[i].at, (uint32_t)(t - (long long)(u->fixups[i].at + 4)));
    }
    u->syms[sym0].size = u->b.n - u->syms[sym0].at;
out:
    g_fail = saved;
    if (!ok && getenv("SNX_X64_STATS"))
        fprintf(stderr, "x64: F_%llX to LLVM at %08X\n", (unsigned long long)f->start, (unsigned)u->word);
    if (!ok) {
        /* as it was: LLVM makes this one */
        u->b.n = at0;
        u->nfixups = fix0;
        for (i = rel0; i < u->nrelocs; ++i) free(u->relocs[i].sym);
        u->nrelocs = rel0;
        for (i = sym0; i < u->nsyms; ++i) free(u->syms[i].name);
        u->nsyms = sym0;
        u->g->x.n = 0;
        u->g->x.nv = 1;
        u->g->x.nb = 1;
    }
    u->nfixups = ok ? u->nfixups : fix0;
    u->nlabels = lab0; /* a function's labels are its own */
    return ok;
}

/* ---- the ELF object ---- */

typedef struct {
    unsigned char* p;
    size_t n, cap;
} Out;
static void put(Out* o, const void* data, size_t n) {
    o->p = (unsigned char*)grow(o->p, &o->cap, o->n + n, 1);
    memcpy(o->p + o->n, data, n);
    o->n += n;
}
static void align(Out* o, size_t a) {
    static const unsigned char zero[16];
    while (o->n % a) put(o, zero, 1);
}
static unsigned str_add(Out* s, const char* name) {
    unsigned at = (unsigned)s->n;
    put(s, name, strlen(name) + 1);
    return at;
}

/* FNV-1a of a name */
static size_t name_hash(const char* n) {
    size_t h = (size_t)1469598103934665603ull;
    for (; *n; ++n) h = (h ^ (unsigned char)*n) * (size_t)1099511628211ull;
    return h;
}

static int write_object(Unit* u, unsigned char** object, size_t* size) {
    Out o = {0}, strtab = {0}, symtab = {0}, rela = {0}, data = {0}, rela_data = {0}, shstr = {0};
    size_t i, j, nundef = 0, first_global;
    char** undef = NULL;
    size_t cap_undef = 0, *reloc_undef = NULL;
    Elf64_Ehdr eh;
    Elf64_Shdr sh[9];
    Elf64_Sym sym;
    unsigned sh_name[9];
    size_t off_text, off_rela, off_data, off_rela_data, off_sym, off_str, off_shstr;
    int jit = u->options->jit;
    memset(&sh, 0, sizeof sh);
    str_add(&strtab, "");
    str_add(&shstr, "");
    sh_name[1] = str_add(&shstr, ".text");
    sh_name[2] = str_add(&shstr, ".rela.text");
    sh_name[3] = str_add(&shstr, ".data.rel.ro");
    sh_name[4] = str_add(&shstr, ".rela.data.rel.ro");
    sh_name[5] = str_add(&shstr, ".symtab");
    sh_name[6] = str_add(&shstr, ".strtab");
    sh_name[7] = str_add(&shstr, ".shstrtab");
    sh_name[8] = str_add(&shstr, ".note.GNU-stack");
    /* the symbols: null, .text's section symbol, the functions (local in a JIT unit), then the globals: the
       functions (the AOT module's), the table, what the code calls that isn't here */
    memset(&sym, 0, sizeof sym);
    put(&symtab, &sym, sizeof sym);
    sym.st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
    sym.st_shndx = 1;
    put(&symtab, &sym, sizeof sym);
    /* calls this object resolves itself: to one of its own functions. The names by a hash table each (the unit's
       functions, then the symbols it leaves undefined), and each relocation's undefined symbol's index kept */
    {
        size_t hcap = 64, *hsym, *hund;
        while (hcap < 2 * (u->nsyms + u->nrelocs) + 2) hcap *= 2;
        hsym = (size_t*)calloc(hcap, sizeof *hsym); /* index + 1, 0 none */
        hund = (size_t*)calloc(hcap, sizeof *hund);
        reloc_undef = (size_t*)calloc(u->nrelocs + 1, sizeof *reloc_undef);
        if (!hsym || !hund || !reloc_undef) {
            fprintf(stderr, "x64 back end: out of memory\n");
            abort();
        }
        for (j = 0; j < u->nsyms; ++j) {
            size_t h = name_hash(u->syms[j].name) & (hcap - 1);
            while (hsym[h]) h = (h + 1) & (hcap - 1);
            hsym[h] = j + 1;
        }
        for (i = 0; i < u->nrelocs; ++i) {
            size_t h0 = name_hash(u->relocs[i].sym) & (hcap - 1), h = h0;
            for (; hsym[h]; h = (h + 1) & (hcap - 1))
                if (!strcmp(u->relocs[i].sym, u->syms[hsym[h] - 1].name)) break;
            if (hsym[h]) {
                j = hsym[h] - 1;
                snx_link_put32(u->b.p + u->relocs[i].at,
                               (uint32_t)((long long)u->syms[j].at - (long long)(u->relocs[i].at + 4)));
                free(u->relocs[i].sym);
                u->relocs[i].sym = NULL;
                continue;
            }
            for (h = h0; hund[h]; h = (h + 1) & (hcap - 1))
                if (!strcmp(u->relocs[i].sym, undef[hund[h] - 1])) break;
            if (!hund[h]) {
                undef = (char**)grow(undef, &cap_undef, nundef + 1, sizeof *undef);
                undef[nundef++] = u->relocs[i].sym;
                hund[h] = nundef;
            }
            reloc_undef[i] = hund[h] - 1;
        }
        free(hsym);
        free(hund);
    }
    if (jit)
        for (i = 0; i < u->nsyms; ++i) {
            memset(&sym, 0, sizeof sym);
            sym.st_name = str_add(&strtab, u->syms[i].name);
            sym.st_info = ELF64_ST_INFO(STB_LOCAL, STT_FUNC);
            sym.st_shndx = 1;
            sym.st_value = u->syms[i].at;
            sym.st_size = u->syms[i].size;
            put(&symtab, &sym, sizeof sym);
        }
    first_global = symtab.n / sizeof sym;
    if (!jit)
        for (i = 0; i < u->nsyms; ++i) {
            memset(&sym, 0, sizeof sym);
            sym.st_name = str_add(&strtab, u->syms[i].name);
            sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
            sym.st_other = STV_HIDDEN;
            sym.st_shndx = 1;
            sym.st_value = u->syms[i].at;
            sym.st_size = u->syms[i].size;
            put(&symtab, &sym, sizeof sym);
        }
    if (jit) {
        memset(&sym, 0, sizeof sym);
        sym.st_name = str_add(&strtab, u->options->table_name);
        sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
        sym.st_other = STV_HIDDEN;
        sym.st_shndx = 3;
        sym.st_size = 8 * u->nsyms;
        put(&symtab, &sym, sizeof sym);
        /* the table: each function's address, R_X86_64_64 of its symbol */
        for (i = 0; i < u->nsyms; ++i) {
            Elf64_Rela r;
            SnxU64 zero = 0;
            put(&data, &zero, 8);
            r.r_offset = 8 * i;
            r.r_info = ELF64_R_INFO(2 + i, R_X86_64_64);
            r.r_addend = 0;
            put(&rela_data, &r, sizeof r);
        }
    }
    {
        size_t undef0 = symtab.n / sizeof sym;
        for (i = 0; i < nundef; ++i) {
            memset(&sym, 0, sizeof sym);
            sym.st_name = str_add(&strtab, undef[i]);
            sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
            sym.st_shndx = SHN_UNDEF;
            if (!strcmp(undef[i], "snx_stale")) sym.st_other = STV_HIDDEN; /* the module's own, as LLVM declares it */
            put(&symtab, &sym, sizeof sym);
        }
        for (i = 0; i < u->nrelocs; ++i) {
            Elf64_Rela r;
            if (!u->relocs[i].sym) continue;
            r.r_offset = u->relocs[i].at;
            r.r_info = ELF64_R_INFO(undef0 + reloc_undef[i], u->relocs[i].type);
            r.r_addend = u->relocs[i].addend;
            put(&rela, &r, sizeof r);
        }
    }
    /* the file: header, sections, section headers */
    memset(&eh, 0, sizeof eh);
    put(&o, &eh, sizeof eh);
    align(&o, 16);
    off_text = o.n;
    put(&o, u->b.p, u->b.n);
    align(&o, 8);
    off_rela = o.n;
    put(&o, rela.p ? rela.p : (unsigned char*)"", rela.n);
    align(&o, 8);
    off_data = o.n;
    if (data.n) put(&o, data.p, data.n);
    align(&o, 8);
    off_rela_data = o.n;
    if (rela_data.n) put(&o, rela_data.p, rela_data.n);
    align(&o, 8);
    off_sym = o.n;
    put(&o, symtab.p, symtab.n);
    off_str = o.n;
    put(&o, strtab.p, strtab.n);
    off_shstr = o.n;
    put(&o, shstr.p, shstr.n);
    align(&o, 8);
    sh[1] = (Elf64_Shdr){sh_name[1], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0, off_text, u->b.n, 0, 0, 16, 0};
    sh[2] = (Elf64_Shdr){sh_name[2], SHT_RELA, SHF_INFO_LINK, 0, off_rela, rela.n, 5, 1, 8, sizeof(Elf64_Rela)};
    sh[3] = (Elf64_Shdr){sh_name[3], SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0, off_data, data.n, 0, 0, 8, 0};
    sh[4] = (Elf64_Shdr){sh_name[4], SHT_RELA, SHF_INFO_LINK, 0, off_rela_data, rela_data.n, 5, 3, 8, sizeof(Elf64_Rela)};
    sh[5] = (Elf64_Shdr){sh_name[5], SHT_SYMTAB, 0, 0, off_sym, symtab.n, 6, (Elf64_Word)first_global, 8,
                         sizeof(Elf64_Sym)};
    sh[6] = (Elf64_Shdr){sh_name[6], SHT_STRTAB, 0, 0, off_str, strtab.n, 0, 0, 1, 0};
    sh[7] = (Elf64_Shdr){sh_name[7], SHT_STRTAB, 0, 0, off_shstr, shstr.n, 0, 0, 1, 0};
    sh[8] = (Elf64_Shdr){sh_name[8], SHT_PROGBITS, 0, 0, off_shstr, 0, 0, 0, 1, 0};
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_REL;
    eh.e_machine = EM_X86_64;
    eh.e_version = EV_CURRENT;
    eh.e_shoff = o.n;
    eh.e_ehsize = sizeof eh;
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = 9;
    eh.e_shstrndx = 7;
    put(&o, sh, sizeof sh);
    memcpy(o.p, &eh, sizeof eh);
    free(strtab.p), free(symtab.p), free(rela.p), free(data.p), free(rela_data.p), free(shstr.p), free(undef);
    free(reloc_undef);
    *object = o.p;
    *size = o.n;
    return 0;
}

static unsigned host_features(void);
/* the back end's SSE code needs SSSE3 and SSE4.1 (2008's processors), and SSE4.2, PCLMULQDQ and POPCNT */
#define X64_BASELINE (SGX_F_SSE3 | SGX_F_SSSE3 | SGX_F_SSE41 | SGX_F_SSE42 | SGX_F_PCLMUL | SGX_F_POPCNT)
int snx_x64_available(void) {
    const char* b = getenv("SNX_RECOMPILER_BACKEND");
    if (b && !strcmp(b, "llvm")) return 0;
    return (host_features() & X64_BASELINE) == X64_BASELINE;
}

/* CPUID's BMI1, BMI2 and LZCNT (ABM) */
static unsigned host_features(void) {
    unsigned a, b, c, d, f = 0;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    if (b & (1u << 3)) f |= SGX_F_BMI1;
    if (b & (1u << 8)) f |= SGX_F_BMI2;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
    if (c & (1u << 5)) f |= SGX_F_LZCNT;
    /* leaf 1's: SSE3, PCLMULQDQ, SSSE3, SSE4.1, SSE4.2, POPCNT; the VEX forms' (AVX, FMA, F16C) where the system
       keeps the YMM state (OSXSAVE, and XCR0's SSE and AVX bits) */
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & 1u) f |= SGX_F_SSE3;
    if (c & (1u << 1)) f |= SGX_F_PCLMUL;
    if (c & (1u << 9)) f |= SGX_F_SSSE3;
    if (c & (1u << 19)) f |= SGX_F_SSE41;
    if (c & (1u << 20)) f |= SGX_F_SSE42;
    if (c & (1u << 23)) f |= SGX_F_POPCNT;
    if ((c & (1u << 27)) && (c & (1u << 28))) {
        unsigned lo, hi;
        __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        if ((lo & 6) == 6) {
            f |= SGX_F_AVX;
            if (c & (1u << 12)) f |= SGX_F_FMA;
            if (c & (1u << 29)) f |= SGX_F_F16C;
        }
    }
    return f;
}

int snx_x64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options, unsigned char* made,
                   unsigned char** object, size_t* size, char* error, size_t error_size) {
    Unit u;
    size_t i, count = 0, need = 4096;
    *object = NULL;
    *size = 0;
    memset(made, 0, last - first);
    if (codegen->hook_direct_calls) return 0; /* every call through the table: LLVM's */
    memset(&u, 0, sizeof u);
    u.g = (SgCtx*)calloc(1, sizeof *u.g);
    if (!u.g) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = first; i < last; ++i) need += codegen->functions[i].count * 160 + 512;
    u.b.cap = need;
    u.b.p = (unsigned char*)malloc(u.b.cap);
    u.b.index = -1;
    u.codegen = codegen;
    u.options = options;
    u.a.b = &u.b;
    u.a.machine = &u;
    u.a.reloc = unit_reloc;
    u.g->features = snx_a64_features(codegen->features);
    u.g->origin = -1;
    u.g->x.features = host_features();
    u.g->x.flags_ins = -1;
    u.g->x.base = 3;
    u.g->x.nv = 1;
    u.g->x.nb = 1;
    for (i = first; i < last; ++i) {
        size_t before = u.b.n;
        if (u.b.cap - u.b.n < codegen->functions[i].count * 400 + 4096) {
            u.b.cap = u.b.cap * 2 + codegen->functions[i].count * 400 + 4096;
            u.b.p = (unsigned char*)realloc(u.b.p, u.b.cap);
        }
        made[i - first] = (unsigned char)function(&u, &codegen->functions[i], i);
        if (u.b.n > u.b.cap) {
            /* past the buffer: grown, and made again */
            u.b.n = before;
            u.b.cap *= 4;
            u.b.p = (unsigned char*)realloc(u.b.p, u.b.cap);
            --i;
            continue;
        }
        count += made[i - first];
        if (options->jit && !made[i - first]) break;
    }
    if (getenv("SNX_X64_STATS"))
        fprintf(stderr, "x64: %lu of %lu functions\n", (unsigned long)count, (unsigned long)(last - first));
    if (options->jit && count != last - first) count = 0;
    if (count) {
        write_object(&u, object, size);
        if (getenv("SNX_X64_DUMP")) {
            /* SNX_X64_DUMP=folder: each object as written, x64_<first function>.o, for objdump */
            char path[512];
            FILE* fo;
            snprintf(path, sizeof path, "%s/x64_%llX.o", getenv("SNX_X64_DUMP"),
                     (unsigned long long)codegen->functions[first].start);
            fo = fopen(path, "wb");
            if (fo) {
                fwrite(*object, 1, *size, fo);
                fclose(fo);
            }
        }
    }
    for (i = 0; i < u.nrelocs; ++i) free(u.relocs[i].sym);
    for (i = 0; i < u.nsyms; ++i) free(u.syms[i].name);
    free(u.relocs), free(u.syms), free(u.fixups), free(u.label_at), free(u.frame_at), free(u.stubs), free(u.b.p);
    free(u.a.cold.p), free(u.a.pool), free(u.a.rips), free(u.a.cjumps), free(u.a.ccalls);
    free(u.g);
    if (options->jit && !count) memset(made, 0, last - first);
    return 0;
}

#else
int snx_x64_available(void) { return 0; }
/* no x86-64 back end in this build */
int snx_x64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options, unsigned char* made,
                   unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
