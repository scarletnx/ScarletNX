/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The arm64 back end's guest functions: mach/mach_codegen.inc over sailgen's arm64 emitters (a64_arm64.inc), with
   the glue of AAPCS64 written here, every word an encoder's of the A64 Sail (a64_enc.inc).

   The function's ABI is GuestFn's: the state in X0, kept in X19 (SGX_BASE_BIAS past it), the entry in X1
   (SGX_FN_TARGET, as a call's target is), compared with the start before the frame, so that a call of a function of
   the unit at its start comes in past that check (with nothing in X1 where the callee was made before the call);
   X29 and X30 saved, then X19 and X20, and X21 to X28 where the function pins guest registers there (the callee's: no
   call changes them), and the spill slots, whose size is patched in at the end, above them (below them on Windows),
   SP 16-aligned at the frame's bottom.
   The guest's vector registers are pinned in V31 down to V24 (caller-saved: stored around a call); V8 to V15,
   whose low halves are the callee's, hold no value, nor does V7, a by-element form's Rm (arm64_vec.c). X16 and X17 are the glue's and the assembler's own.

   The object is ELF AArch64: F_<address> global and hidden in the AOT module's units, so another back end's
   function calls one of these and the other way; in a JIT unit local, with the unit's table (ABS64). A call is a
   CALL26 relocation (a tail call JUMP26), resolved here where the callee is this object's own; snx_stale is read by
   ADRP and LDRB (ADR_PREL_PG_HI21, LDST8_ABS_LO12_NC). For Windows (SNX_ARM64_F_WIN) the code is the same but for the
   entry's check, after the prologue there, which the unwinder reads at the function's first word; AAPCS64's registers
   are Windows' too (X18, the system's, is never touched), in a COFF object whose function table unwinds it by the
   prologue and the epilogues (x64_elf.c), a frame of a page or more read page by page first (the guard page's) */
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include "common/utilities/bits.h"
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* not on Windows on another machine, whose tests run none of its code */
#if !defined(_WIN32) || defined(__aarch64__)
#if defined(__APPLE__) || defined(_WIN32)
#include "common/cpu/recompiler/object/elf_defs.h"
#else
#include <elf.h>
#endif
/* elf_object.c's, for mach_codegen.inc's ELF objects (its header clashes with x64_elf.h's) */
unsigned char snx_elf_host_data(void);
int snx_elf_byteswap(unsigned char* object, size_t size, int to_foreign);

/* AArch64's machine and relocations, by number: not every <elf.h> has them (ELF for the Arm 64-bit Architecture, 5.7) */
#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
enum { A64_ABS64 = 257, A64_ADR_PREL_PG_HI21 = 275, A64_LDST8_ABS_LO12_NC = 278, A64_JUMP26 = 282, A64_CALL26 = 283 };

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* a word the emitters can't make (bits.h's sg_fail) leaves the function to another back end */
static _Thread_local jmp_buf* g_fail;
static _Thread_local const char* g_why; /* what it was */
#define SG_FAIL_JUMP g_fail
#define SG_FAIL_NOTE(what, a, b) (g_why = (what), (void)(a), (void)(b))
#define SGX_FN 1

typedef struct SgXAsm SgXAsm;
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);

#include "a64_arm64.inc"

static_assert(SNX_ARM64_STATE_BIAS == SGX_BASE_BIAS, "arm64_codegen.h's bias of X19 is sailgen's");

/* ---- what mach_codegen.inc asks of the machine ---- */
#define MG_EM EM_AARCH64
#define MG_COFF windows
#define MG_R_ABS64 A64_ABS64
#define SGX_FN_TARGET 1
static const signed char mg_pin_hosts[9] = {20, 21, 22, 23, 24, 25, 26, 27, 28};
#define MG_NPINS 9
#define MG_GPR_NAME(r, buf) (snprintf((buf), sizeof(buf), "X%d", (r)), (buf))
#define MG_VREG_PREFIX      "V"
#define MG_NPINS_FIXED 9
#define MG_PIN_RESERVED(h) 0
#define MG_BOUND 1
#define MG_FAR_JUMPS 1             /* past B.cond's 1 MiB: the opposite over a B */
#define MG_POOL_REACH (512u << 10) /* LDR (literal)'s is 1 MiB */
#define MG_DIRECT_ENTRY    (64u << 20)  /* half of BL's 128 MiB: the call's glue comes before it */
#define MG_JTAB            1
#define MG_JTAB_CLOBBER    0u /* X16 and X17 alone */
#define MG_JTAB_LEADERS    24 /* fewer: the dispatch block (x86-64's measure; an arm64 device's is to come) */
#define MG_POLL_INLINE     1  /* a loop's turn: LDR and CBZ of the poll back to its header, the call out of line */
#define MG_VPIN(k) (31 - (k))
#define MG_NVPINS 8
#define MG_XRESERVED 0xFF80u /* V8-V15 (the callee's low halves), V7 (arm64_vec.c's scratch) */
#define MG_RBIT mg_rbit
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend);

#include "common/cpu/recompiler/mach/mach_codegen.inc"

/* ---- the glue ---- */

/* the code is for Windows */
static int windows(const Unit* u) { return (u->g->x.features & SNX_ARM64_F_WIN) != 0; }

/* a fixup's word: B's imm26, B.cond's and CBZ's imm19, ADR's immhi:immlo; FIX_TAB + a table's label: a jump table's
   word, the label's distance from the table */
enum { FIX_B, FIX_BCOND, FIX_CBZ, FIX_ADR, FIX_TAB };
/* the frame: the ten registers the prologue saves, then the slots; off Windows, slots of up to FRAME_FOLDED bytes are
   in STP X29, X30's pre-index and LDP's post-index (504 bytes at most), larger ones a SUB SP first */
enum { FRAME_SAVED = 96, FRAME_FOLDED = 504 - FRAME_SAVED };

static void w32(Unit* u, SnxU32 w) { sga_w(&u->b, w); }
static void fixup(Unit* u, int label, int kind) {
    u->fixups = (Fixup*)grow(u->fixups, &u->cap_fixups, u->nfixups + 1, sizeof *u->fixups);
    u->fixups[u->nfixups].at = u->b.n;
    u->fixups[u->nfixups].kind = kind;
    u->fixups[u->nfixups++].label = label;
}
static void reloc(Unit* u, size_t at, const char* sym, unsigned type, long long addend) {
    size_t n = strlen(sym);
    u->relocs = (Reloc*)grow(u->relocs, &u->cap_relocs, u->nrelocs + 1, sizeof *u->relocs);
    u->relocs[u->nrelocs].at = at;
    u->relocs[u->nrelocs].sym = (char*)malloc(n + 1);
    memcpy(u->relocs[u->nrelocs].sym, sym, n + 1);
    u->relocs[u->nrelocs].type = type;
    u->relocs[u->nrelocs++].addend = addend;
}
/* the helpers' calls the emitters make (arm64_a64.h's BL): CALL26 */
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend) {
    reloc((Unit*)machine, at, sym, A64_CALL26, addend);
}

static void jmp(Unit* u, int label) {
    fixup(u, label, FIX_B);
    w32(u, sge_a64_b_uncond(0));
}
static void jcc(Unit* u, int cc, int label) {
    if (u->far) {
        w32(u, sge_a64_b_cond((SnxU32)cc ^ 1, 2));
        jmp(u, label);
        return;
    }
    fixup(u, label, FIX_BCOND);
    w32(u, sge_a64_b_cond((SnxU32)cc, 0));
}
/* the state's field at off, of bytes, to or from Xt */
static void state_ldst(Unit* u, int load, int bytes, int rt, size_t off) { sga_ldst(&u->b, load, bytes, rt, SGX_BASE, -1, (int)off); }
static void movi(Unit* u, int rd, SnxU64 v) { sga_movi(&u->b, rd, v, 1); }
static void mov_x0_state(Unit* u) { sga_state_out(&u->b); }
static void set_pc(Unit* u, SnxU64 pc) {
    movi(u, SGA_X16, pc);
    state_ldst(u, 0, 8, SGA_X16, offsetof(CPU, pc));
}
/* BLR to c->rt's function at off */
static void call_rt(Unit* u, int off) {
    state_ldst(u, 1, 8, SGA_X16, offsetof(CPU, rt));
    sga_ldst(&u->b, 1, 8, SGA_X16, SGA_X16, -1, off);
    w32(u, sge_a64_blr(SGA_X16));
}

/* the pins to the state (store: those written there since it was in step, cur_dirty) or from it (those pin_sel
   names, the ones live there): every one, or (caller) the vector ones, which a call may change */
static void pins_of(Unit* u, SgXBuf* b, int store, int caller) {
    int n;
    if (!u->pinned) return;
    if (u->mempin >= 0 && !store && !caller) sga_ldst(b, 1, 8, u->mempin, SGX_BASE, -1, (int)offsetof(CPU, mem));
    for (n = 0; n < 32; ++n) {
        if (u->pin[n] >= 0 && !caller && (store ? (u->pin_wr & u->cur_dirty) >> n & 1 : u->pin_sel >> n & 1))
            sga_ldst(b, !store, 8, u->pin[n], SGX_BASE, -1, (int)(offsetof(CPU, x) + 8 * (size_t)n));
        if (u->vpin[n] >= 0 && (store ? (u->vpin_wr & u->vcur_dirty) >> n & 1 : u->vpin_sel >> n & 1))
            sga_fp_ldst(b, !store, 16, u->vpin[n], SGX_BASE, -1, (int)(offsetof(CPU, v) + 16 * (size_t)n));
    }
}

/* an ADD or SUB of SP to come, of the frame's size, patched in at the end */
static void frame_here(Unit* u, size_t at) {
    u->frame_at = (size_t*)grow(u->frame_at, &u->cap_frame, u->nframe + 1, sizeof *u->frame_at);
    u->frame_at[u->nframe++] = at;
}
/* the slots in STP X29, X30's pre-index: off Windows, but where mg_finish found them too large and made the function
   again (Unit.again) */
static int folded(const Unit* u) { return !windows(u) && !u->again; }
/* STP X29, X30, [SP, #-96]!; MOV X29, SP; STP X19, X20, [SP, #16] (and X21-X28 where pinned); ADD X19, X0, #8
   (sga_state_in); the pins loaded. The slots: off Windows above the saved registers, folded into the STP's pre-index
   (patched) or, larger, a SUB SP, SP, #frame (patched) before it; on Windows below them, a SUB SP after the ADD, past
   the probe of a page or more, as x64_elf.c's a64_prologue reads it back (and knows the epilogue by it) */
static void prologue(Unit* u) {
    u->a.frame_off = windows(u) ? 0 : FRAME_SAVED;
    if (!windows(u)) {
        frame_here(u, u->b.n);
        if (!folded(u)) w32(u, sge_a64_sub_addsub_imm(31, 31, 0, 0, 1));
    }
    w32(u, sge_a64_stp_gen_pre_idx(29, 31, 30, (SnxU32)-12 & 0x7F, 2));
    w32(u, sge_a64_add_addsub_imm(29, 31, 0, 0, 1));
    w32(u, sge_a64_stp_gen_general_offset(19, 31, 20, 2, 2));
    if (u->pinned) {
        int k;
        for (k = 0; k < 4; ++k) w32(u, sge_a64_stp_gen_general_offset((SnxU32)(21 + 2 * k), 31, (SnxU32)(22 + 2 * k), (SnxU32)(4 + 2 * k), 2));
    }
    sga_state_in(&u->b);
    if (windows(u)) {
        if (u->again) frame_here(u, sga_stack_probe(&u->b));
        frame_here(u, u->b.n);
        w32(u, sge_a64_sub_addsub_imm(31, 31, 0, 0, 1));
    }
    pins(u, 0);
}
/* the prologue's undone: the saved registers back, LDP X29, X30, [SP], #96, the slots in its post-index (patched) or an
   ADD SP, SP, #frame (patched) after it; on Windows that ADD first. The return or the jump is next: Windows' unwinder
   has an epilogue end at one */
static void epilogue(Unit* u) {
    if (windows(u)) {
        frame_here(u, u->b.n);
        w32(u, sge_a64_add_addsub_imm(31, 31, 0, 0, 1));
    }
    if (u->pinned) {
        int k;
        for (k = 0; k < 4; ++k) w32(u, sge_a64_ldp_gen_general_offset((SnxU32)(21 + 2 * k), 31, (SnxU32)(22 + 2 * k), (SnxU32)(4 + 2 * k), 2));
    }
    w32(u, sge_a64_ldp_gen_general_offset(19, 31, 20, 2, 2));
    if (folded(u)) frame_here(u, u->b.n);
    w32(u, sge_a64_ldp_gen_post_idx(29, 31, 30, 12, 2));
    if (!windows(u) && !folded(u)) {
        frame_here(u, u->b.n);
        w32(u, sge_a64_add_addsub_imm(31, 31, 0, 0, 1));
    }
}
static void leave(Unit* u) {
    pins(u, 1);
    epilogue(u);
    w32(u, sge_a64_ret(30));
}

/* CALLDYN(X1): the dispatch table's function for the target (CPU.disp, inside [code_lo, code_hi) and aligned), else
   the runtime's miss, called with the state and the target: snx_lookup's, inline. tail: the epilogue and a jump */
static void calldyn(Unit* u, int tail) {
    int miss = new_label(u), found = new_label(u);
    pins(u, 1); /* the callee reads the guest's registers from the state; X16, the target's, is the stores' scratch */
    state_ldst(u, 1, 8, SGA_X16, offsetof(CPU, code_lo));
    state_ldst(u, 1, 8, SGA_X17, offsetof(CPU, code_hi));
    w32(u, sge_a64_sub_addsub_shift(SGA_X17, SGA_X17, 0, SGA_X16, 0, 1)); /* hi - lo */
    w32(u, sge_a64_sub_addsub_shift(SGA_X16, 1, 0, SGA_X16, 0, 1));       /* target - lo */
    w32(u, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X16, 0, SGA_X17, 0, 1)); /* CMP */
    jcc(u, 2, miss);                                                       /* B.HS */
    w32(u, sge_a64_ands_log_imm(SGA_ZR, 1, 1, 0, 1, 1));                  /* TST X1, #3 */
    jcc(u, 1, miss);                                                       /* B.NE */
    w32(u, sge_a64_ubfm(SGA_X16, SGA_X16, 63, 2, 1, 1));                  /* LSR X16, X16, #2 */
    state_ldst(u, 1, 8, SGA_X17, offsetof(CPU, disp));
    w32(u, sge_a64_ldr_reg_gen(SGA_X16, SGA_X17, 1, 3, SGA_X16, 3));      /* LDR X16, [X17, X16, LSL #3] */
    fixup(u, found, FIX_CBZ);
    w32(u, sge_a64_cbnz(SGA_X16, 0, 1));
    bind(u, miss);
    state_ldst(u, 1, 8, SGA_X16, offsetof(CPU, rt));
    sga_ldst(&u->b, 1, 8, SGA_X16, SGA_X16, -1, (int)offsetof(RtAPI, miss));
    bind(u, found);
    mov_x0_state(u);
    if (tail) {
        epilogue(u);
        w32(u, sge_a64_br(SGA_X16));
    } else {
        w32(u, sge_a64_blr(SGA_X16));
        pins(u, 0);
    }
}
static void set_target(Unit* u, SnxU64 target) { movi(u, 1, target); }
/* BL F_target (CALL26), or the epilogue and B (JUMP26): the pins stored first, the state in X0, the target in X1 */
static void call_sym(Unit* u, const char* sym, int tail) {
    pins(u, 1);
    mov_x0_state(u);
    if (tail) epilogue(u);
    reloc(u, u->b.n, sym, tail ? A64_JUMP26 : A64_CALL26, 0);
    w32(u, tail ? sge_a64_b_uncond(0) : sge_a64_bl(0));
    if (!tail) pins(u, 0);
}
/* W16 = snx_stale[index]: ADRP X16 and LDRB of the page's offset, then CMP W16, #0. COFF's ADRP holds less than 1 MiB
   past the symbol: the rest is whole pages, an ADD */
static void stale_test(Unit* u, size_t index) {
    const size_t far = windows(u) ? index >> 20 : 0;
    reloc(u, u->b.n, "snx_stale", A64_ADR_PREL_PG_HI21, (long long)(index - (far << 20)));
    w32(u, sge_a64_adrp(SGA_X16, 0, 0));
    if (far) w32(u, sge_a64_add_addsub_imm(SGA_X16, SGA_X16, (SnxU32)(far << 8), 1, 1)); /* 1 MiB: 256 pages */
    reloc(u, u->b.n, "snx_stale", A64_LDST8_ABS_LO12_NC, (long long)(index - (far << 20)));
    w32(u, sge_a64_ldrb_imm_immediate_unsigned(SGA_X16, SGA_X16, 0));
    w32(u, sge_a64_subs_addsub_imm(SGA_ZR, SGA_X16, 0, 0, 0));
}
/* what a bound PLT stub's slot is compared with (mach_codegen.inc's MG_BOUND): the flags are no guest's here, as its
   registers and flags are in the state (and the pins) */
static void mg_if_stale(Unit* u, size_t index, int label) {
    stale_test(u, index);
    jcc(u, 1, label); /* B.NE */
}
static void mg_if_x_not(Unit* u, unsigned r, SnxU64 value, int label) {
    int p = u->pinned ? u->pin[r] : -1;
    movi(u, SGA_X16, value);
    if (p < 0) {
        state_ldst(u, 1, 8, SGA_X17, offsetof(CPU, x) + 8 * (size_t)r);
        p = SGA_X17;
    }
    w32(u, sge_a64_subs_addsub_shift(SGA_ZR, (SnxU32)p, 0, SGA_X16, 0, 1)); /* CMP Xp, X16 */
    jcc(u, 1, label);
}
static void mg_if_pc_not(Unit* u, SnxU64 value, int label) {
    movi(u, SGA_X16, value);
    state_ldst(u, 1, 8, SGA_X17, offsetof(CPU, pc));
    w32(u, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X17, 0, SGA_X16, 0, 1));
    jcc(u, 1, label);
}
static void mg_target_pc(Unit* u) { state_ldst(u, 1, 8, 1, offsetof(CPU, pc)); }
/* if (c->pc != next) return: the callee changed the code it returns to, or left for another pc */
static void check_return(Unit* u, SnxU64 next) {
    int stay = new_label(u), go = new_label(u);
    movi(u, SGA_X16, next);
    state_ldst(u, 1, 8, SGA_X17, offsetof(CPU, pc));
    w32(u, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X17, 0, SGA_X16, 0, 1));
    jcc(u, 1, go);
    jmp(u, stay);
    bind(u, go);
    leave(u);
    bind(u, stay);
    if (u->options->stale) {
        stale_test(u, u->index);
        jcc(u, 1, go);
    }
}
/* the poll of a backward branch: if (c->poll) { c->pc = resume; c->rt->poll(c); F_start(c, resume); return; } */
static void poll_jz(Unit* u, int label) {
    state_ldst(u, 1, 4, SGA_X16, offsetof(CPU, poll));
    if (u->far) { /* past CBZ's 1 MiB: CBNZ over a B */
        w32(u, sge_a64_cbnz(SGA_X16, 2, 0));
        jmp(u, label);
        return;
    }
    fixup(u, label, FIX_CBZ);
    w32(u, sge_a64_cbz(SGA_X16, 0, 0));
}
static void poll_slow(Unit* u, SnxU64 resume) {
    pins(u, 1);
    set_pc(u, resume);
    mov_x0_state(u);
    call_rt(u, (int)offsetof(RtAPI, poll));
    mov_x0_state(u);
    movi(u, 1, resume);
    epilogue(u);
    jmp(u, u->start_label); /* the entry of any target: past the check is the start's */
}
static void poll_then(Unit* u, SnxU64 resume) {
    int run = new_label(u);
    state_ldst(u, 1, 4, SGA_X16, offsetof(CPU, poll));
    fixup(u, run, FIX_CBZ); /* the slow path's few words: always in reach */
    w32(u, sge_a64_cbz(SGA_X16, 0, 0));
    poll_slow(u, resume);
    bind(u, run);
}
/* a switch's jump, X1 the target (SGX_FN_TARGET): if (!c->poll && X1 - lo <= span && !(X1 & 3)) goto table +
   table[X1 - lo] (ADR, LDRSW, ADD, BR), else the dispatch block (CPU.pc holds the target). A function of more than ADR's
   1 MiB (far) takes the dispatch block */
static void jtab_jump(Unit* u, int k) {
    const struct JTab* t = &u->jtabs[k];
    if (u->far) {
        jmp(u, u->d_label);
        return;
    }
    state_ldst(u, 1, 4, SGA_X16, offsetof(CPU, poll));
    fixup(u, u->d_label, FIX_CBZ);
    w32(u, sge_a64_cbnz(SGA_X16, 0, 0));
    movi(u, SGA_X16, t->lo);
    w32(u, sge_a64_sub_addsub_shift(SGA_X16, 1, 0, SGA_X16, 0, 1));         /* X16 = X1 - lo */
    w32(u, sge_a64_subs_addsub_imm(SGA_ZR, SGA_X16, (t->n - 1) * 4, 0, 1)); /* CMP X16, #span (under 4 KiB) */
    jcc(u, 8, u->d_label);                                                  /* B.HI */
    w32(u, sge_a64_ands_log_imm(SGA_ZR, SGA_X16, 1, 0, 1, 1));              /* TST X16, #3 */
    jcc(u, 1, u->d_label);                                                  /* B.NE */
    fixup(u, t->label, FIX_ADR);
    w32(u, sge_a64_adr(SGA_X17, 0, 0));
    w32(u, sge_a64_ldrsw_reg(SGA_X16, SGA_X17, 0, 3, SGA_X16)); /* LDRSW X16, [X17, X16] */
    w32(u, sge_a64_add_addsub_shift(SGA_X16, SGA_X17, 0, SGA_X16, 0, 1));
    w32(u, sge_a64_br(SGA_X16));
}
/* each table: a word a slot, its label's distance from the table (FIX_TAB) */
static void jtab_emit(Unit* u) {
    int k;
    for (k = 0; k < u->njtabs; ++k) {
        const struct JTab* t = &u->jtabs[k];
        unsigned s;
        bind(u, t->label);
        for (s = 0; s < t->n; ++s) {
            fixup(u, u->jslots[t->first + s], FIX_TAB + t->label);
            w32(u, 0);
        }
    }
}
static void mg_undef(Unit* u, SnxU64 address, SnxU32 word) {
    pins(u, 1);
    set_pc(u, address);
    mov_x0_state(u);
    movi(u, 1, address);
    movi(u, 2, word);
    call_rt(u, (int)offsetof(RtAPI, undef));
}
static void mg_svc(Unit* u, SnxU64 next, unsigned n, int direct) {
    pins(u, 1);
    set_pc(u, next);
    mov_x0_state(u);
    movi(u, 1, n);
    if (direct) {
        state_ldst(u, 1, 8, SGA_X16, offsetof(CPU, rt));
        sga_ldst(&u->b, 1, 8, SGA_X16, SGA_X16, -1, (int)offsetof(RtAPI, svcs));
        sga_ldst(&u->b, 1, 8, SGA_X16, SGA_X16, -1, (int)(8 * n));
        w32(u, sge_a64_blr(SGA_X16));
    } else
        call_rt(u, (int)offsetof(RtAPI, svc));
    pins(u, 0);
}
static void mg_brk(Unit* u, SnxU64 address, unsigned imm16) {
    pins(u, 1);
    set_pc(u, address);
    mov_x0_state(u);
    movi(u, 1, address);
    movi(u, 2, imm16);
    call_rt(u, (int)offsetof(RtAPI, brk));
}
static int mg_direct(const Unit* u) { return !windows(u); }
/* MOVZ/MOVK X16, start; CMP X1, X16; B.NE label */
static void mg_check(Unit* u, SnxU64 start, int label) {
    movi(u, SGA_X16, start);
    w32(u, sge_a64_subs_addsub_shift(SGA_ZR, 1, 0, SGA_X16, 0, 1));
    jcc(u, 1, label);
}
static void mg_stale(Unit* u, size_t index, int given, SnxU64 start) {
    int live;
    if (!u->options->stale) return;
    live = new_label(u);
    stale_test(u, index);
    jcc(u, 0, live); /* B.EQ */
    if (given)
        state_ldst(u, 0, 8, 1, offsetof(CPU, pc));
    else
        set_pc(u, start);
    epilogue(u); /* no pin changed yet (and not every one loaded): none stored */
    w32(u, sge_a64_ret(30));
    bind(u, live);
}
static void mg_entry(Unit* u, size_t index, SnxU64 start) {
    mg_stale(u, index, 1, start);
    mg_check(u, start, u->mismatch_label);
}
static void mg_dispatch(Unit* u, int lab0) {
    const SnxFunction* f = u->f;
    size_t i;
    if (!u->dispatch) {
        calldyn(u, 1);
        return;
    }
    state_ldst(u, 0, 8, 1, offsetof(CPU, pc));
    jmp(u, u->d_label);
    {
        int run = new_label(u);
        bind(u, u->d_label);
        state_ldst(u, 1, 4, SGA_X16, offsetof(CPU, poll));
        fixup(u, run, FIX_CBZ);
        w32(u, sge_a64_cbz(SGA_X16, 0, 0));
        pins(u, 1);
        mov_x0_state(u);
        call_rt(u, (int)offsetof(RtAPI, poll));
        pins(u, 0);
        bind(u, run);
        state_ldst(u, 1, 8, SGA_X17, offsetof(CPU, pc));
        for (i = 0; i < f->count; ++i)
            if (f->leaders[i]) {
                movi(u, SGA_X16, snx_function_address(f, i));
                w32(u, sge_a64_subs_addsub_shift(SGA_ZR, SGA_X17, 0, SGA_X16, 0, 1));
                jcc(u, 0, lab0 + (int)i);
            }
        w32(u, sge_a64_orr_log_shift(1, SGA_ZR, 0, SGA_X17, 0, 1)); /* MOV X1, X17 */
        calldyn(u, 1);
    }
}
/* a word's offset field of d words (imm26 at bit 0, imm19 at bit 5): 1, 0 out of its reach */
static int put_offset(Unit* u, size_t at, long long d, int kind) {
    SnxU32 w;
    if (at + 4 > u->b.cap) return 1;
    w = (SnxU32)u->b.p[at] | (SnxU32)u->b.p[at + 1] << 8 | (SnxU32)u->b.p[at + 2] << 16 | (SnxU32)u->b.p[at + 3] << 24;
    if (kind == FIX_B) {
        if (d < -(1ll << 25) || d >= (1ll << 25)) return 0;
        w = (w & ~0x3FFFFFFu) | ((SnxU32)d & 0x3FFFFFFu);
    } else if (kind == FIX_ADR) {
        if (d < -(1ll << 18) || d >= (1ll << 18)) return 0; /* d words: immhi, immlo 0 */
        w = (w & ~(3u << 29 | 0x7FFFFu << 5)) | ((SnxU32)d & 0x7FFFFu) << 5;
    } else {
        if (d < -(1ll << 18) || d >= (1ll << 18)) return 0;
        w = (w & ~(0x7FFFFu << 5)) | ((SnxU32)d & 0x7FFFFu) << 5;
    }
    u->b.p[at] = (unsigned char)w, u->b.p[at + 1] = (unsigned char)(w >> 8), u->b.p[at + 2] = (unsigned char)(w >> 16);
    u->b.p[at + 3] = (unsigned char)(w >> 24);
    return 1;
}
static void mg_pool_island(Unit* u) {
    size_t at = u->b.n;
    w32(u, sge_a64_b_uncond(0));
    sgx_pool_emit(&u->a);
    put_offset(u, at, (long long)(u->b.n - at) / 4, FIX_B);
}
/* STP X29, X30's pre-index or LDP's post-index at at: the saved registers' and size bytes of slots, in 8s */
static void frame_pair(Unit* u, size_t at, unsigned size) {
    SnxU32 w, imm7;
    if (at + 4 > u->b.cap) return;
    w = (SnxU32)u->b.p[at] | (SnxU32)u->b.p[at + 1] << 8 | (SnxU32)u->b.p[at + 2] << 16 | (SnxU32)u->b.p[at + 3] << 24;
    imm7 = (w >> 22 & 1 ? (FRAME_SAVED + size) / 8 : 0u - (FRAME_SAVED + size) / 8) & 0x7Fu; /* LDP: L, up */
    w = (w & ~(0x7Fu << 15)) | imm7 << 15;
    u->b.p[at] = (unsigned char)w, u->b.p[at + 1] = (unsigned char)(w >> 8), u->b.p[at + 2] = (unsigned char)(w >> 16);
    u->b.p[at + 3] = (unsigned char)(w >> 24);
}
static int mg_finish(Unit* u, size_t fix0) {
    size_t i;
    unsigned size = sga_frame_size(&u->a), sh = size > 4095, i12 = sh ? (size + 4095) >> 12 : size;
    if (i12 > 4095) return 0;
    if (windows(u) && SGA_FRAME_PROBED(size) && !u->again) {
        u->again = 1; /* a page or more, read page by page first (the guard page): made again with it */
        return 0;
    }
    if (folded(u) && size > FRAME_FOLDED) {
        u->again = 1; /* past the pre-index's reach: made again with a SUB and ADD of SP */
        return 0;
    }
    for (i = 0; i < u->nframe; ++i) {
        if (folded(u))
            frame_pair(u, u->frame_at[i], size);
        else
            sga_frame_imm(&u->b, u->frame_at[i], i12, sh);
    }
    for (i = fix0; i < u->nfixups; ++i) {
        long long t = u->label_at[u->fixups[i].label];
        if (t < 0) return 0;
        if (u->fixups[i].kind >= FIX_TAB) {
            if (u->fixups[i].at + 4 <= u->b.cap)
                snx_link_put32(u->b.p + u->fixups[i].at, (uint32_t)(t - u->label_at[u->fixups[i].kind - FIX_TAB]));
        } else if (!put_offset(u, u->fixups[i].at, (t - (long long)u->fixups[i].at) / 4, u->fixups[i].kind))
            return 0;
    }
    return 1;
}
/* BL or B of the unit's own function, within B's reach */
static int mg_resolve_local(Unit* u, const Reloc* r, size_t target_at) {
    if (r->type != A64_CALL26 && r->type != A64_JUMP26) return 0;
    return put_offset(u, r->at, ((long long)target_at - (long long)r->at) / 4, FIX_B);
}

/* the guest's A64 features (SNX_A64_FEAT_*) the host runs as its own: the base architecture's, and those of its
   features (SNX_ARM64_F_*) */
static _Thread_local SnxU64 g_host_a64;
static SnxU64 host_a64(unsigned host) {
    static const struct {
        unsigned host;
        SnxU64 guest;
    } map[] = {{SNX_ARM64_F_CRC32, SNX_A64_FEAT_CRC32},     {SNX_ARM64_F_AES, SNX_A64_FEAT_AES},
               {SNX_ARM64_F_PMULL, SNX_A64_FEAT_PMULL},     {SNX_ARM64_F_SHA1, SNX_A64_FEAT_SHA1},
               {SNX_ARM64_F_SHA256, SNX_A64_FEAT_SHA256},   {SNX_ARM64_F_LSE, SNX_A64_FEAT_LSE},
               {SNX_ARM64_F_RDM, SNX_A64_FEAT_RDM},         {SNX_ARM64_F_FP16, SNX_A64_FEAT_FP16},
               {SNX_ARM64_F_DOTPROD, SNX_A64_FEAT_DOTPROD}, {SNX_ARM64_F_FHM, SNX_A64_FEAT_FHM},
               {SNX_ARM64_F_SHA512, SNX_A64_FEAT_SHA512},   {SNX_ARM64_F_SHA3, SNX_A64_FEAT_SHA3},
               {SNX_ARM64_F_SM, SNX_A64_FEAT_SM3 | SNX_A64_FEAT_SM4}, {SNX_ARM64_F_JSCVT, SNX_A64_FEAT_JSCVT},
               {SNX_ARM64_F_FCMA, SNX_A64_FEAT_FCMA},       {SNX_ARM64_F_FRINTTS, SNX_A64_FEAT_FRINTTS},
               {SNX_ARM64_F_BF16, SNX_A64_FEAT_BF16},       {SNX_ARM64_F_I8MM, SNX_A64_FEAT_I8MM}};
    SnxU64 f = SNX_A64_FEAT_V8_0;
    size_t k;
    for (k = 0; k < sizeof map / sizeof *map; ++k)
        if (host & map[k].host) f |= map[k].guest;
    return f;
}

#include "common/cpu/recompiler/arm64/arm64_vec.c"

/* ---- the host ---- */

#if defined(__aarch64__) && defined(__ELF__)
#include <sys/auxv.h>
#elif defined(__aarch64__) && defined(__APPLE__)
#include <sys/sysctl.h>
/* one of XNU's flags (Apple's "Determining instruction set characteristics"), 0 where the system has none so named */
static int apple_has(const char* name) {
    int value = 0;
    size_t size = sizeof value;
    return !sysctlbyname(name, &value, &size, NULL, 0) && value;
}
/* the features Linux's hwcaps give above, by their sysctl. Read once: a unit asks for them, and each is a system call */
static unsigned apple_features(void) {
    static const struct {
        const char* name;
        unsigned f;
    } caps[] = {
        {"hw.optional.arm.FEAT_AES", SNX_ARM64_F_AES},         {"hw.optional.arm.FEAT_PMULL", SNX_ARM64_F_PMULL},
        {"hw.optional.arm.FEAT_SHA1", SNX_ARM64_F_SHA1},       {"hw.optional.arm.FEAT_SHA256", SNX_ARM64_F_SHA256},
        {"hw.optional.arm.FEAT_CRC32", SNX_ARM64_F_CRC32},     {"hw.optional.armv8_crc32", SNX_ARM64_F_CRC32},
        {"hw.optional.arm.FEAT_LSE", SNX_ARM64_F_LSE},         {"hw.optional.arm.FEAT_FP16", SNX_ARM64_F_FP16},
        {"hw.optional.arm.FEAT_RDM", SNX_ARM64_F_RDM},         {"hw.optional.arm.FEAT_JSCVT", SNX_ARM64_F_JSCVT},
        {"hw.optional.arm.FEAT_FCMA", SNX_ARM64_F_FCMA},       {"hw.optional.arm.FEAT_SHA3", SNX_ARM64_F_SHA3},
        {"hw.optional.arm.FEAT_DotProd", SNX_ARM64_F_DOTPROD}, {"hw.optional.arm.FEAT_SHA512", SNX_ARM64_F_SHA512},
        {"hw.optional.arm.FEAT_FHM", SNX_ARM64_F_FHM}};
    static _Atomic unsigned known; /* the features, and bit 31 once they are read */
    unsigned f = atomic_load_explicit(&known, memory_order_acquire);
    if (f) return f & ~(1u << 31);
    for (size_t k = 0; k < sizeof caps / sizeof *caps; ++k)
        if (apple_has(caps[k].name)) f |= caps[k].f;
    atomic_store_explicit(&known, f | 1u << 31, memory_order_release);
    return f;
}
#endif
#if defined(__aarch64__) && defined(_WIN32)
/* here, past the code above: its macros take names of that (far) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* what IsProcessorFeaturePresent says, its features by number, as not every SDK's winnt.h names them (PF_ARM_*: V8_CRYPTO
   30, V8_CRC32 31, V81_ATOMIC 34, V82_DP 43, V83_JSCVT 44, SHA3 64, SHA512 65, V82_I8MM 66, V82_FP16 67, V86_BF16 68);
   one the system has no number for is one the code goes without */
static unsigned windows_features(void) {
    static const struct {
        DWORD number;
        unsigned f;
    } list[] = {{30, SNX_ARM64_F_AES | SNX_ARM64_F_PMULL | SNX_ARM64_F_SHA1 | SNX_ARM64_F_SHA256},
                {31, SNX_ARM64_F_CRC32},
                {34, SNX_ARM64_F_LSE},
                {43, SNX_ARM64_F_DOTPROD},
                {44, SNX_ARM64_F_JSCVT},
                {64, SNX_ARM64_F_SHA3},
                {65, SNX_ARM64_F_SHA512},
                {66, SNX_ARM64_F_I8MM},
                {67, SNX_ARM64_F_FP16},
                {68, SNX_ARM64_F_BF16}};
    unsigned f = 0;
    size_t k;
    for (k = 0; k < sizeof list / sizeof *list; ++k)
        if (IsProcessorFeaturePresent(list[k].number)) f |= list[k].f;
    return f;
}
#endif
/* what a test asked for: the AArch32 blocks and the descriptor's system, and the features to go without */
static struct {
    _Atomic int windows;
    _Atomic unsigned without;
} g_arm64_codegen;
void snx_arm64_without(unsigned features) {
    atomic_store_explicit(&g_arm64_codegen.without, features & ~(unsigned)SNX_ARM64_F_WIN, memory_order_relaxed);
}
void snx_arm64_set_windows(int on) { atomic_store_explicit(&g_arm64_codegen.windows, on, memory_order_relaxed); }
int snx_arm64_windows(void) {
#if defined(_WIN32)
    return snx_arm64_available();
#else
    return atomic_load_explicit(&g_arm64_codegen.windows, memory_order_relaxed);
#endif
}
/* what the system says this processor has */
static unsigned host_features(void) {
#if defined(__aarch64__) && defined(_WIN32)
    return windows_features() | SNX_ARM64_F_WIN;
#elif defined(__aarch64__) && defined(__APPLE__)
    return apple_features();
#elif defined(__aarch64__) && defined(__ELF__)
    unsigned long hw = 0;
    unsigned f = 0;
#if defined(__linux__)
    hw = getauxval(AT_HWCAP);
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    if (elf_aux_info(AT_HWCAP, &hw, sizeof hw)) hw = 0;
#endif
    /* Linux's and FreeBSD's HWCAP bits (the kernel's arch/arm64 hwcap.h) */
    static const struct {
        unsigned bit, f;
    } caps[] = {{3, SNX_ARM64_F_AES},      {4, SNX_ARM64_F_PMULL},     {5, SNX_ARM64_F_SHA1},
                {6, SNX_ARM64_F_SHA256},   {7, SNX_ARM64_F_CRC32},     {8, SNX_ARM64_F_LSE},
                {9 | 10 << 8, SNX_ARM64_F_FP16}, {12, SNX_ARM64_F_RDM}, {13, SNX_ARM64_F_JSCVT},
                {14, SNX_ARM64_F_FCMA},    {17, SNX_ARM64_F_SHA3},     {18, SNX_ARM64_F_SM},
                {20, SNX_ARM64_F_DOTPROD}, {21, SNX_ARM64_F_SHA512},   {23, SNX_ARM64_F_FHM}};
    size_t k;
    for (k = 0; k < sizeof caps / sizeof *caps; ++k) {
        unsigned b0 = caps[k].bit & 0xFF, b1 = caps[k].bit >> 8;
        if ((hw >> b0 & 1) && (!b1 || (hw >> b1 & 1))) f |= caps[k].f;
    }
    return f;
#else
    return 0;
#endif
}
unsigned snx_arm64_features(void) {
    return host_features() & ~atomic_load_explicit(&g_arm64_codegen.without, memory_order_relaxed);
}
int snx_arm64_available(void) {
#if defined(__aarch64__) && (defined(__ELF__) || defined(_WIN32) || defined(__APPLE__))
    return 1;
#else
    return 0;
#endif
}

int snx_arm64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options, unsigned host,
                     unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    g_host_a64 = host_a64(host);
    return mach_object(codegen, first, last, options, made, object, size, error, error_size, host);
}

#else
int snx_arm64_available(void) { return 0; }
unsigned snx_arm64_features(void) { return 0; }
void snx_arm64_without(unsigned features) { (void)features; }
int snx_arm64_windows(void) { return 0; }
void snx_arm64_set_windows(int on) { (void)on; }
int snx_arm64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options, unsigned host,
                     unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size) {
    (void)codegen, (void)options, (void)host, (void)error, (void)error_size;
    memset(made, 0, last - first);
    *object = NULL;
    *size = 0;
    return 0;
}
#endif
