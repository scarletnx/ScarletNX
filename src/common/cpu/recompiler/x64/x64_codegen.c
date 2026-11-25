/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's guest functions: what ir_codegen.c's snx_ir_emit makes of a function, without LLVM. Each
   basic block's instructions are sailgen's x86-64 emitters' (a64_x64.inc), appended to one SSA region (registers read
   once, flags reused, constants folded) and assembled when the block ends; what joins the blocks is written here: the
   entry's check, the branches (a backward one polling first), the direct and dynamic calls (the dispatch table read
   inline, as snx_lookup reads it), the returns' checks, SVC, BRK and UNDEF, the dispatch block of a function of several
   leaders. The guest's registers are in the CPU structure at every block's start and end, as a block's stores keep
   them (sailgen's x64_a64.h), so a block needs nothing of another's and a call nothing of its caller's.

   The function's ABI is GuestFn's, the host's C: System V's x86-64, the state in RDI, kept in RBX (pushed), the entry
   in RSI; RSP 16-aligned after the prologue (the push and a frame of spill slots, its size patched in at the end).
   Windows' (win64, the features' SGX_F_WIN64): the state in RCX, the entry in RDX, and the frame's lowest 48 bytes the
   callees' (four arguments' shadow, a fifth and a sixth). Values are in the registers a call changes in both, so a
   function without pins saves RBX alone; the pins take RDI and RSI, which are its to save there, where System V's take
   R11 and R10, and the vector pins' XMM registers are saved above the callees' bytes, XMM6 and XMM7 with them for
   values. A C call keeps every pin there, so none is stored around it. Control flow is bytes written here (jumps,
   calls, the frame): the instruction selection derives what computes values.

   The object is ELF x86-64, or COFF of Windows' ABI (x64_elf.h), as LLVM's are: F_<address> global and hidden in the AOT
   module's units (library.c), so an LLVM unit's function calls one of these and the other way; in a JIT unit local,
   with the unit's table. A call is a PLT32 relocation, resolved here where the callee is this object's own */
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "common/utilities/bits.h"
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "common/cpu/recompiler/x64/x64_elf.h"
#if defined(__x86_64__) && defined(SNX_HAVE_X64)
#if defined(__APPLE__) || defined(_WIN32)
#include "common/cpu/recompiler/llvm/elf_defs.h"
#else
#include <elf.h>
#endif
/* elf_object.c's, for mach_codegen.inc's ELF objects (its header clashes with x64_elf.h's) */
unsigned char snx_elf_host_data(void);
int snx_elf_byteswap(unsigned char* object, size_t size, int to_foreign);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && defined(SNX_HAVE_X64)
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
static _Thread_local const char* g_why; /* what it was */
#define SG_FAIL_JUMP g_fail
#define SG_FAIL_NOTE(what, a, b) (g_why = (what), (void)(a), (void)(b))
#define SGX_FN 1

typedef struct SgXAsm SgXAsm;
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

#include "a64_x64.inc"

/* ---- what mach_codegen.inc asks of the machine ----
   x86-64's ELF; a call's target and a function's entry in RSI (Windows' RDX); the pins in RBP, R12-R15 (pushed: the
   callee's) and R11, R10 (caller-saved, values' otherwise: reserved, and an exclusive's, an atomic's or CRC32's fixed
   registers), on Windows RDI and RSI (the callee's: no value's and no sequence's); the vector pins in XMM15 down to
   XMM8 */
#define MG_EM EM_X86_64
#define MG_R_ABS64 R_X86_64_64
#define MG_WIN64(u) SGX_WIN64(&(u)->g->x)
#define SGX_FN_TARGET (MG_WIN64(u) ? 2 : 6)
static const signed char g_pin_hosts[2][7] = {{5, 12, 13, 14, 15, 11, 10}, {5, 12, 13, 14, 15, 7, 6}};
#define mg_pin_hosts (g_pin_hosts[MG_WIN64(u)])
#define MG_NPINS 7
#define MG_NPINS_FIXED (MG_WIN64(u) ? 7 : 5)
#define MG_PIN_RESERVED(h) ((h) == 10 || (h) == 11)
#define MG_VPIN(k) (15 - (k))
#define MG_NVPINS 8
#define MG_XRESERVED 0u
#define MG_BOUND 1
#define MG_COFF win64
#define MG_POLL_INLINE 1
#define MG_JTAB 1
#define MG_JTAB_CLOBBER (1u << 0 | 1u << (MG_WIN64(u) ? 8 : 2)) /* RAX, RDX (Windows' R8) */
#define MG_JTAB_LEADERS 24 /* fewer: the dispatch block (measured on an i5-9400: 12 cases, 14 leaders, 17% quicker) */
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend);

#include "common/cpu/recompiler/mach/mach_codegen.inc"

static int win64(const Unit* u) { return MG_WIN64(u); }
/* the callee-saved host registers the pins take (RBP, R12-R15, on Windows RDI and RSI: bits), which the prologue
   pushes */
static unsigned pushed_pins(const Unit* u) {
    unsigned mask = 0;
    int n;
    if (!u->pinned) return 0;
    for (n = 0; n < 32; ++n)
        if (u->pin[n] >= 0) mask |= 1u << u->pin[n];
    if (u->mempin >= 0) mask |= 1u << u->mempin;
    return mask & ~(1u << 10 | 1u << 11);
}
/* where the function's saved XMM register k (xsaved's bits) is from RSP: above the callees' bytes, in order */
static int xsaved_at(const Unit* u, int k) {
    return 8 * SGX_WIN64_ARGS + 16 * (int)snx_popcount64(u->a.xsaved & ((1u << k) - 1));
}

/* ---- the glue: bytes of the ABI and the flow ---- */

static void byte(Unit* u, unsigned char x) { sgx_byte(&u->b, x); }
static void imm(Unit* u, long long v, int bytes) { sgx_imm(&u->b, v, bytes); }
static void rel_label(Unit* u, int label) {
    u->fixups = (Fixup*)grow(u->fixups, &u->cap_fixups, u->nfixups + 1, sizeof *u->fixups);
    u->fixups[u->nfixups].at = u->b.n;
    u->fixups[u->nfixups].kind = 0;
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
/* the second argument, a call's target: RSI, or RDX */
static void mov_arg1_imm(Unit* u, SnxU64 v) {
    unsigned char op = win64(u) ? 0xBA : 0xBE;
    if (v <= 0xFFFFFFFFu) {
        byte(u, op); /* mov esi, imm32 */
        imm(u, (long long)v, 4);
    } else {
        byte(u, 0x48); /* mov rsi, imm64 */
        byte(u, op);
        imm(u, (long long)v, 8);
    }
}
/* the third, 32 bits: EDX, or R8D */
static void mov_arg2_imm(Unit* u, SnxU32 v) {
    if (win64(u))
        byte(u, 0x41), byte(u, 0xB8);
    else
        byte(u, 0xBA);
    imm(u, (long long)v, 4);
}
/* the state, the first: RDI, or RCX */
static void state_arg(Unit* u) { sgx_state_arg(&u->a, &u->b); }
/* RAX = the target (RSI, RDX), and the target = RAX */
static void mov_rax_target(Unit* u) { byte(u, 0x48), byte(u, 0x89), byte(u, win64(u) ? 0xD0 : 0xF0); }
static void mov_target_rax(Unit* u) { byte(u, 0x48), byte(u, 0x89), byte(u, win64(u) ? 0xC2 : 0xC6); }
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
    sgx_call_room(&u->a);
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
    byte(u, 0x48), byte(u, 0x8B), byte(u, win64(u) ? 0x93 : 0xB3), imm(u, (long long)offsetof(CPU, pc), 4);
}

/* the guest registers the function keeps in host ones: to the state (store), or from it */
/* the pins to the state (store) or from it: every one, or (caller) those in caller-saved registers, which a call
   changes (R10, R11, the XMM registers; none of a Windows function's) */
static void pins_of(Unit* u, SgXBuf* b, int store, int caller) {
    int n;
    if (!u->pinned || (caller && win64(u))) return;
    if (u->mempin >= 0 && !store && (!caller || u->mempin == 10 || u->mempin == 11)) {
        int p = u->mempin; /* mov p, [rbx + mem]: read-only, never stored */
        sgx_byte(b, (unsigned char)(0x48 | (p >> 3) << 2));
        sgx_byte(b, 0x8B);
        sgx_byte(b, (unsigned char)(0x83 | (p & 7) << 3));
        sgx_imm(b, (long long)offsetof(CPU, mem), 4);
    }
    for (n = 0; n < 32; ++n) {
        if (u->pin[n] >= 0 && (!caller || u->pin[n] == 10 || u->pin[n] == 11) &&
            (store ? (u->pin_wr & u->cur_dirty) >> n & 1 : u->pin_sel >> n & 1)) {
            int p = u->pin[n];
            sgx_byte(b, (unsigned char)(0x48 | (p >> 3) << 2));
            sgx_byte(b, store ? 0x89 : 0x8B);
            sgx_byte(b, (unsigned char)(0x83 | (p & 7) << 3)); /* [rbx + disp32] */
            sgx_imm(b, (long long)(offsetof(CPU, x) + 8 * (size_t)n), 4);
        }
        if (u->vpin[n] >= 0 && (store ? (u->vpin_wr & u->vcur_dirty) >> n & 1 : u->vpin_sel >> n & 1)) {
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

/* an XMM register of the callee's saved above the callees' bytes, or back: movaps [rsp + disp32] */
static void xsaved(Unit* u, int restore) {
    int k;
    for (k = 6; k < 16; ++k) {
        if (!(u->a.xsaved >> k & 1)) continue;
        if (k >= 8) byte(u, 0x44);
        byte(u, 0x0F), byte(u, restore ? 0x28 : 0x29);
        byte(u, (unsigned char)(0x84 | (k & 7) << 3)), byte(u, 0x24);
        imm(u, xsaved_at(u, k), 4);
    }
}
/* what x64_elf.c's unwind_info reads back: the pushes, the frame, the XMM registers saved */
static void prologue(Unit* u) {
    unsigned pushed = pushed_pins(u);
    int n;
    u->a.x = &u->g->x;
    u->a.xsaved = 0;
    if (win64(u)) {
        for (n = 0; n < 32; ++n)
            if (u->vpin[n] >= 0) u->a.xsaved |= 1u << u->vpin[n];
        if (u->a.xsaved) u->a.xsaved |= 1u << 6 | 1u << 7, sgx_call_room(&u->a);
    }
    byte(u, 0x53); /* push rbx */
    if (pushed >> 5 & 1) byte(u, 0x55);                            /* push rbp */
    for (n = 12; n < 16; ++n)
        if (pushed >> n & 1) byte(u, 0x41), byte(u, (unsigned char)(0x50 | (n & 7))); /* push r12 to r15 */
    if (pushed >> 7 & 1) byte(u, 0x57);                            /* push rdi */
    if (pushed >> 6 & 1) byte(u, 0x56);                            /* push rsi */
    byte(u, 0x48), byte(u, 0x89), byte(u, win64(u) ? 0xCB : 0xFB); /* mov rbx, rdi (rcx) */
    if (u->again) {
        /* a frame of a page or more: each page read first, nearest first (sgx_stack_probe) */
        sgx_stack_probe(&u->b);
        u->frame_at = (size_t*)grow(u->frame_at, &u->cap_frame, u->nframe + 1, sizeof *u->frame_at);
        u->frame_at[u->nframe++] = u->b.n - SGX_STACK_PROBE_SIZE + 1;
    }
    byte(u, 0x48); /* sub rsp, imm32 */
    byte(u, 0x81);
    byte(u, 0xEC);
    u->frame_at = (size_t*)grow(u->frame_at, &u->cap_frame, u->nframe + 1, sizeof *u->frame_at);
    u->frame_at[u->nframe++] = u->b.n;
    imm(u, 0, 4);
    xsaved(u, 0);
    pins(u, 0);
}
/* the frame and the registers the prologue pushed given back (a pinned guest register to the state before: pins). From
   the ADD on the bytes are an epilogue as Windows' unwinder knows one, up to the return or the jump after it */
static void epilogue(Unit* u) {
    unsigned pushed = pushed_pins(u);
    int n;
    xsaved(u, 1);
    byte(u, 0x48); /* add rsp, imm32 */
    byte(u, 0x81);
    byte(u, 0xC4);
    u->frame_at = (size_t*)grow(u->frame_at, &u->cap_frame, u->nframe + 1, sizeof *u->frame_at);
    u->frame_at[u->nframe++] = u->b.n;
    imm(u, 0, 4);
    if (pushed >> 6 & 1) byte(u, 0x5E); /* pop rsi */
    if (pushed >> 7 & 1) byte(u, 0x5F); /* pop rdi */
    for (n = 15; n >= 12; --n)
        if (pushed >> n & 1) byte(u, 0x41), byte(u, (unsigned char)(0x58 | (n & 7))); /* pop r15 to r12 */
    if (pushed >> 5 & 1) byte(u, 0x5D); /* pop rbp */
    byte(u, 0x5B); /* pop rbx */
}
static void leave(Unit* u) {
    pins(u, 1);
    epilogue(u);
    byte(u, 0xC3); /* ret */
}

/* a field of the state with the register the dispatch below takes as scratch, RDX or R8 */
static void scratch_op(Unit* u, int op, size_t off) {
    byte(u, win64(u) ? 0x4C : 0x48), byte(u, (unsigned char)op), byte(u, win64(u) ? 0x83 : 0x93);
    imm(u, (long long)off, 4);
}
/* CALLDYN(RSI): the dispatch table's function for the target (CPU.disp, inside [code_lo, code_hi) and aligned), else
   the runtime's miss, called with the state and the target: snx_lookup's, inline. tail: the epilogue and a jump */
static void calldyn(Unit* u, int tail) {
    int miss = new_label(u), found = new_label(u);
    mov_rax_target(u);
    byte(u, 0x48), byte(u, 0x2B), byte(u, 0x83), imm(u, (long long)offsetof(CPU, code_lo), 4); /* sub rax, lo */
    scratch_op(u, 0x8B, offsetof(CPU, code_hi));                                               /* mov rdx, hi */
    scratch_op(u, 0x2B, offsetof(CPU, code_lo));                                               /* sub rdx, lo */
    byte(u, win64(u) ? 0x4C : 0x48), byte(u, 0x39), byte(u, win64(u) ? 0xC0 : 0xD0);           /* cmp rax, rdx */
    jcc(u, 0x3, miss);                                                               /* jae */
    if (win64(u))
        byte(u, 0xF6), byte(u, 0xC2), byte(u, 0x03); /* test dl, 3 */
    else
        byte(u, 0x40), byte(u, 0xF6), byte(u, 0xC6), byte(u, 0x03);                  /* test sil, 3 */
    jcc(u, 0x5, miss);                                                               /* jnz */
    byte(u, 0x48), byte(u, 0xC1), byte(u, 0xE8), byte(u, 0x02);                      /* shr rax, 2 */
    scratch_op(u, 0x8B, offsetof(CPU, disp));                                        /* mov rdx, disp */
    byte(u, win64(u) ? 0x49 : 0x48), byte(u, 0x8B), byte(u, 0x04),
        byte(u, win64(u) ? 0xC0 : 0xC2);                                             /* mov rax, [rdx + rax*8] */
    byte(u, 0x48), byte(u, 0x85), byte(u, 0xC0);                                     /* test rax, rax */
    jcc(u, 0x5, found);                                                              /* jnz */
    bind(u, miss);
    load_rax(u, (int)offsetof(CPU, rt));
    byte(u, 0x48), byte(u, 0x8B), byte(u, 0x80), imm(u, (long long)offsetof(RtAPI, miss), 4); /* mov rax, [rax+miss] */
    bind(u, found);
    pins(u, 1); /* the callee reads the guest's registers from the state */
    state_arg(u); /* before the epilogue: its pop gives RBX back to the caller */
    if (tail) epilogue(u);
    else
        sgx_call_room(&u->a);
    if (tail && win64(u)) byte(u, 0x48); /* the jump the unwinder takes for an epilogue's has REX.W */
    byte(u, 0xFF);
    byte(u, tail ? 0xE0 : 0xD0); /* jmp rax / call rax */
    if (!tail) pins(u, 0);
}

static void set_target(Unit* u, SnxU64 target) { mov_arg1_imm(u, target); }
/* call F_target / jmp: the pins stored first, the state in RDI (RCX), the target in RSI (RDX) already */
static void call_sym(Unit* u, const char* sym, int tail) {
    pins(u, 1);
    state_arg(u);
    if (tail) epilogue(u);
    else
        sgx_call_room(&u->a);
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
static void poll_jz(Unit* u, int label) {
    byte(u, 0x83), byte(u, 0xBB), imm(u, (long long)offsetof(CPU, poll), 4), byte(u, 0x00); /* cmp dword [rbx+poll], 0 */
    jcc(u, 0x4, label);
}
static void poll_slow(Unit* u, SnxU64 resume) {
    char sym[40];
    pins(u, 1);
    set_pc(u, resume);
    state_arg(u);
    call_rt(u, (int)offsetof(RtAPI, poll));
    state_arg(u);
    if (win64(u)) mov_arg1_imm(u, resume); /* nothing between its epilogue and the jump */
    epilogue(u);
    if (!win64(u)) mov_arg1_imm(u, resume);
    fname(sym, sizeof sym, "F_", u->f->start);
    byte(u, 0xE9);
    rel_sym(u, sym);
}
/* a switch's jump, T the target (SGX_FN_TARGET): if (!c->poll && T - lo <= span && !(T & 3)) goto *(table +
   table[T - lo]), else the dispatch block (CPU.pc holds the target). In RAX and RDX, on Windows, where T is in RDX, R8 */
static void jtab_jump(Unit* u, int k) {
    const struct JTab* t = &u->jtabs[k];
    SnxU64 span = (SnxU64)(t->n - 1) * 4;
    int win = win64(u);
    byte(u, 0x83), byte(u, 0xBB), imm(u, (long long)offsetof(CPU, poll), 4), byte(u, 0x00); /* cmp dword [rbx+poll], 0 */
    jcc(u, 0x5, u->d_label);
    if (t->lo < 0x80000000u) {
        byte(u, 0x48), byte(u, 0x8D), byte(u, win ? 0x82 : 0x86), imm(u, -(long long)t->lo, 4); /* lea rax, [T - lo] */
    } else {
        byte(u, win ? 0x49 : 0x48), byte(u, win ? 0xB8 : 0xBA), imm(u, (long long)t->lo, 8); /* mov rdx (r8), lo */
        mov_rax_target(u);
        byte(u, win ? 0x4C : 0x48), byte(u, 0x29), byte(u, win ? 0xC0 : 0xD0); /* sub rax, rdx (r8) */
    }
    byte(u, 0x48), byte(u, 0x3D), imm(u, (long long)span, 4); /* cmp rax, span */
    jcc(u, 0x7, u->d_label);                                  /* ja */
    byte(u, 0xA8), byte(u, 0x03);                             /* test al, 3 */
    jcc(u, 0x5, u->d_label);
    byte(u, win ? 0x4C : 0x48), byte(u, 0x8D), byte(u, win ? 0x05 : 0x15); /* lea rdx (r8), [rip + table] */
    rel_label(u, t->label);
    byte(u, win ? 0x49 : 0x48), byte(u, 0x63), byte(u, 0x04), byte(u, win ? 0x00 : 0x02); /* movsxd rax, [rdx + rax] */
    byte(u, win ? 0x4C : 0x48), byte(u, 0x01), byte(u, win ? 0xC0 : 0xD0);                /* add rax, rdx (r8) */
    byte(u, 0xFF), byte(u, 0xE0); /* jmp rax (no REX.W: no epilogue's to Windows' unwinder) */
}
/* each table: a word a slot, its label's distance from the table (mg_finish's fixups of kind table's label + 1) */
static void jtab_emit(Unit* u) {
    int k;
    for (k = 0; k < u->njtabs; ++k) {
        const struct JTab* t = &u->jtabs[k];
        unsigned s;
        while (u->b.n & 3) byte(u, 0xCC);
        bind(u, t->label);
        for (s = 0; s < t->n; ++s) {
            u->fixups = (Fixup*)grow(u->fixups, &u->cap_fixups, u->nfixups + 1, sizeof *u->fixups);
            u->fixups[u->nfixups].at = u->b.n;
            u->fixups[u->nfixups].kind = t->label + 1;
            u->fixups[u->nfixups++].label = u->jslots[t->first + s];
            imm(u, 0, 4);
        }
    }
}
static void poll_then(Unit* u, SnxU64 resume) {
    int run = new_label(u);
    poll_jz(u, run);
    poll_slow(u, resume);
    bind(u, run);
}


/* UNDEF: c->pc = address; c->rt->undef(c, address, word) */
static void mg_undef(Unit* u, SnxU64 address, SnxU32 word) {
    pins(u, 1);
    set_pc(u, address);
    state_arg(u);
    mov_arg1_imm(u, address);
    mov_arg2_imm(u, word);
    call_rt(u, (int)offsetof(RtAPI, undef));
}
/* SVC: c->pc = next; c->rt->svcs[n](c, n) or c->rt->svc(c, n), the pins around it */
static void mg_svc(Unit* u, SnxU64 next, unsigned n, int direct) {
    pins(u, 1); /* the SVC reads and writes the guest's registers in the state */
    set_pc(u, next);
    state_arg(u);
    mov_arg1_imm(u, n);
    if (direct) {
        sgx_call_room(&u->a);
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
    state_arg(u);
    mov_arg1_imm(u, address);
    mov_arg2_imm(u, imm16);
    call_rt(u, (int)offsetof(RtAPI, brk));
}
static void mg_entry(Unit* u, size_t index, SnxU64 start) {
    if (u->options->stale) {
        int live = new_label(u);
        stale_test(u, index);
        jcc(u, 0x4, live);
        mov_rax_target(u);
        store_rax(u, (int)offsetof(CPU, pc));
        epilogue(u); /* no pin changed yet (and not every one loaded): none stored */
        byte(u, 0xC3);
        bind(u, live);
    }
    mov_rax_imm(u, start);
    byte(u, 0x48), byte(u, 0x39), byte(u, win64(u) ? 0xC2 : 0xC6); /* cmp rsi, rax */
    jcc(u, 0x5, u->mismatch_label);
}
/* cmp rax, value (RDX the value where it is no imm32) */
static void cmp_rax(Unit* u, SnxU64 value) {
    if (value < 0x80000000u) {
        byte(u, 0x48), byte(u, 0x3D), imm(u, (long long)value, 4); /* cmp rax, imm32 */
    } else {
        byte(u, 0x48), byte(u, 0xBA), imm(u, (long long)value, 8); /* mov rdx, imm64 */
        byte(u, 0x48), byte(u, 0x39), byte(u, 0xD0);               /* cmp rax, rdx */
    }
}
/* RAX among the leaders at[0..n) (ascending): a jump to the leader's label (lab0 + its index), else to miss. A tree
   of compares, the middle one first, a few in a row at its leaves */
static void dispatch_tree(Unit* u, int lab0, const SnxU64* at, const size_t* index, size_t n, int miss) {
    size_t m, k;
    int left;
    if (n <= 3) {
        for (k = 0; k < n; ++k) {
            cmp_rax(u, at[k]);
            jcc(u, 0x4, lab0 + (int)index[k]); /* je */
        }
        jmp(u, miss);
        return;
    }
    m = n / 2;
    left = new_label(u);
    cmp_rax(u, at[m]);
    jcc(u, 0x4, lab0 + (int)index[m]); /* je */
    jcc(u, 0x2, left);                  /* jb */
    dispatch_tree(u, lab0, at + m + 1, index + m + 1, n - m - 1, miss);
    bind(u, left);
    dispatch_tree(u, lab0, at, index, m, miss);
}
static void mg_dispatch(Unit* u, int lab0) {
    const SnxFunction* f = u->f;
    size_t i;
    if (u->dispatch) {
        mov_rax_target(u);
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
        state_arg(u);
        call_rt(u, (int)offsetof(RtAPI, poll));
        pins(u, 0);
        bind(u, run);
        load_rax(u, (int)offsetof(CPU, pc));
        {
            /* a target outside the leaders' span (a tail call through a pointer: most BRs that aren't a switch's) goes
               on at once; inside it, a tree of compares, not one a leader */
            size_t n = 0;
            SnxU64* at = (SnxU64*)malloc(f->leader_count * sizeof *at);
            size_t* index = (size_t*)malloc(f->leader_count * sizeof *index);
            int miss = new_label(u);
            if (!at || !index) sg_fail("out of memory for the dispatch block", 0, 0);
            for (i = 0; i < f->count && n < f->leader_count; ++i)
                if (f->leaders[i]) at[n] = snx_function_address(f, i), index[n++] = i;
            if (n > 3) {
                cmp_rax(u, at[0]);
                jcc(u, 0x2, miss); /* jb */
                cmp_rax(u, at[n - 1]);
                jcc(u, 0x7, miss); /* ja */
            }
            dispatch_tree(u, lab0, at, index, n, miss);
            free(at), free(index);
            bind(u, miss);
        }
        mov_target_rax(u);
        calldyn(u, 1);
    }
}
static int mg_finish(Unit* u, size_t fix0) {
    size_t i;
    /* RSP 16-aligned in the body: the return address and RBX, and the pushed pins' registers: 8 more where those are
       an odd number of doublewords */
    int size = ((u->a.slots_max * 8 + 15) & ~15) + (int)snx_popcount64(pushed_pins(u)) % 2 * 8;
    if (win64(u) && size >= 4096 && !u->again) {
        u->again = 1; /* a page or more, whose prologue reads each page first (the guard page): made again with it */
        return 0;
    }
    for (i = 0; i < u->nframe; ++i)
        if (u->frame_at[i] + 4 <= u->b.cap) {
            unsigned char* p = u->b.p + u->frame_at[i];
            p[0] = (unsigned char)size, p[1] = (unsigned char)(size >> 8), p[2] = (unsigned char)(size >> 16),
            p[3] = (unsigned char)(size >> 24);
        }
    for (i = fix0; i < u->nfixups; ++i) {
        long long t = u->label_at[u->fixups[i].label];
        if (t < 0) return 0;
        if (u->fixups[i].at + 4 <= u->b.cap) {
            /* a jump table's word (kind: its table's label + 1), else a rel32 */
            long long from = u->fixups[i].kind ? u->label_at[u->fixups[i].kind - 1] : (long long)(u->fixups[i].at + 4);
            snx_link_put32(u->b.p + u->fixups[i].at, (uint32_t)(t - from));
        }
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
/* the host's features the back end goes without (snx_x64_without), and the ABI it writes (snx_x64_set_win64) */
static struct {
    _Atomic unsigned without;
    _Atomic int win64;
} g_x64_codegen;
/* the back end's SSE code needs SSSE3 and SSE4.1 (2008's processors), and SSE4.2, PCLMULQDQ and POPCNT */
#define X64_BASELINE (SGX_F_SSE3 | SGX_F_SSSE3 | SGX_F_SSE41 | SGX_F_SSE42 | SGX_F_PCLMUL | SGX_F_POPCNT)
int snx_x64_available(void) {
    return (host_features() & X64_BASELINE) == X64_BASELINE;
}
unsigned snx_x64_features(void) { return host_features(); }
int snx_x64_win64(void) { return snx_x64_available() && (host_features() & SGX_F_WIN64); }
void snx_x64_without(unsigned features) {
    atomic_store_explicit(&g_x64_codegen.without, features & ~(unsigned)(X64_BASELINE | SGX_F_WIN64),
                          memory_order_relaxed);
}
void snx_x64_set_win64(int on) { atomic_store_explicit(&g_x64_codegen.win64, on, memory_order_relaxed); }

/* CPUID's BMI1, BMI2, SHA and LZCNT (ABM), and Windows' ABI */
static unsigned host_features(void) {
    unsigned a, b, c, d, f = 0;
#if defined(_WIN32)
    f |= SGX_F_WIN64;
#else
    if (atomic_load_explicit(&g_x64_codegen.win64, memory_order_relaxed)) f |= SGX_F_WIN64;
#endif
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
    fact64(put, user, (SnxU64)options->alone);
    fact64(put, user, options->without_count);
    if (options->without_count) put(user, options->without, options->without_count * sizeof *options->without);
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
int snx_x64_win64(void) { return 0; }
void snx_x64_without(unsigned features) { (void)features; }
void snx_x64_set_win64(int on) { (void)on; }
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
