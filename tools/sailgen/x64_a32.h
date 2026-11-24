/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's x86-64 back end, AArch32's part (after x64.h), as llvm_arm.h is LLVM's: the registers and CPSR where
   SnxArmCPU keeps them (arm.h), its address in RBX. A register read once is the region's value of it after (a load, or
   what was written: every write is stored at once, and the stores a later one overwrites are dropped as the region
   ends). PSTATE's fields written are kept for one store of CPSR (sg_flags_flush), as llvm.c's nz wrote them, but past
   the instruction's end: NZCV from AddWithCarry stays the flags of the instruction that made them, which the next
   instruction's ConditionHolds reads as the condition code the selection derived (sg_condition_origin), until
   something needs CPSR in memory (a branch out of the region: a condition's skip, an exit, a call, the region's end).

   The flat guest memory of the block compiler's FLAT mode (llvm.h): an access whose bytes pass min(mem_size, 2^32) is
   a DATA_ABORT exit, R15 the instruction's, the syndrome its address, as llvm.c's bounds() makes it; a store clears
   the exclusive monitor where it overlaps the reservation (llvm.c's store_monitor), on a cold path. The condition of
   an instruction (ConditionPassed) is a jump past it, made where it first changes something (sg_when's deferred
   skip), so a clause that hands its word on (sg_see) has emitted nothing but what nothing reads. The block compiler
   (src/common/cpu/recompiler/x64/x64_arm.c) writes the rest: the prologue and its entry checks, the instructions the
   Sail has no clause for, the block's end and its chaining, and it binds the labels and writes the exits' stubs this
   file records */

#include "common/cpu/arm/arm.h"
#include <stddef.h>

static_assert(offsetof(SnxArmCPU, syndrome) == offsetof(SnxArmCPU, exit) + 4, "exit and syndrome cleared by one store");

/* an exit (sgx_a32_exit's) the block's code jumps to: its label, the exit, the syndrome (a host register, or the
   constant where reg is -1), and R15 = pc first unless pc_kept */
typedef struct {
    int label;
    unsigned exit;
    signed char reg, pc_kept;
    SnxU32 syndrome, pc;
} SgA32Exit;
/* a store's check of the exclusive monitor that found it armed: its cold path, the address's register and the bytes,
   and where the hot code goes on */
typedef struct {
    int label;
    signed char reg;
    unsigned bytes;
    size_t back;
} SgA32Monitor;

struct SgCtx {
    SgX x;
    SnxU32 word;
    SnxU64 pc; /* the instruction's address (gen.c's g->pc) */
    SnxU32 next;
    int thumb; /* the block is Thumb: 1 where its instructions read Thumb's PC (the A32 twins of T32's among them) */
    int iset;  /* SelectInstrSet's, 0 for the block's */
    int branched, undefined;
    SgD target;    /* BranchTo's, 32 bits */
    int gpr[15];   /* the region's value of each register, 0 for none */
    int cpsr;      /* the region's CPSR, 0 for none */
    SgD field[32]; /* PSTATE's fields written and not stored yet (pending's bits) */
    unsigned pending;
    int origin; /* the instruction whose x86 flags NZCV was last written from, -1 for none */
    SgB when;   /* the instruction's condition, its skip not made yet (0: none) */
    int skip;   /* the label past the instruction its condition jumps to, -1 for none */
    int limit;  /* min(mem_size, 2^32), once a region (0: none yet) */
    int mem;    /* CPU.mem, once a region */
    SgB vfp_ok
        [2]; /* FPSCR lets the VFP's hot paths run (x64_arm.c's fp_state): a compare's, arithmetic's; 0 none yet */
    int group, group_write; /* a multiple transfer's range checked (GuestTransfer) */
    SgD group_start;
    unsigned deferred; /* the registers a multiple load wrote, stored as it ends (sg_registers_flush) */
    SgD dvalue[15];
    int start; /* the instructions before this word's */
    /* the labels, by number: each one's offset in the code once bound (-1 before); the rel32s that jump to them */
    long long* label_at;
    int nlabels, cap_labels;
    struct SgA32Fix {
        size_t at;
        int label;
    }* fix;
    int nfix, cap_fix;
    SgA32Exit* exits;
    int nexits, cap_exits;
    SgA32Monitor* monitors;
    int nmonitors, cap_monitors;
    int ret;       /* the block's return: the epilogue and RET, bound at its end */
    int e_written; /* an MSR of the block may have set CPSR.E, which the next block's checked entry looks at */
    /* a Thumb block's: ITSTATE before the instruction and after it (an SVC's or BKPT's, left in CPSR: sg_exception), and
       the one an IT set for the instructions after it */
    unsigned itstate, it_after, it_next;
    int it_set;
    /* a Thumb-2 exclusive's: its second register + 1 (a doubleword's, 0 for the one after the first), and what it adds
       to its base register */
    unsigned pair;
    SnxU32 offset;
};

static inline SgX* sgx(SgCtx* g) { return &g->x; }

/* ---- labels ---- */

static inline void* sgx_a32_grow(void* p, int* cap, int need, size_t each) {
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : 32;
    if (*cap < need) *cap = need;
    p = realloc(p, (size_t)*cap * each);
    if (!p) sg_fail("out of memory", need, 0);
    return p;
}
static inline int sgx_a32_label(SgCtx* g) {
    g->label_at = (long long*)sgx_a32_grow(g->label_at, &g->cap_labels, g->nlabels + 1, sizeof *g->label_at);
    g->label_at[g->nlabels] = -1;
    return g->nlabels++;
}
static inline void sgx_a32_bind(SgCtx* g, SgXBuf* b, int label) { g->label_at[label] = (long long)b->n; }
/* a rel32 at b's end to label, patched when the block is done (sgx_a32_patch) */
static inline void sgx_a32_rel(SgCtx* g, SgXBuf* b, int label) {
    g->fix = (struct SgA32Fix*)sgx_a32_grow(g->fix, &g->cap_fix, g->nfix + 1, sizeof *g->fix);
    g->fix[g->nfix].at = b->n;
    g->fix[g->nfix++].label = label;
    sgx_imm(b, 0, 4);
}
static inline void sgx_a32_jcc(SgCtx* g, SgXBuf* b, int cc, int label) {
    sgx_byte(b, 0x0F);
    sgx_byte(b, (unsigned char)(0x80 | (cc & 15)));
    sgx_a32_rel(g, b, label);
}
static inline void sgx_a32_jmp(SgCtx* g, SgXBuf* b, int label) {
    sgx_byte(b, 0xE9);
    sgx_a32_rel(g, b, label);
}
/* every jump to a label patched: 0, or -1 where one was never bound */
static inline int sgx_a32_patch(SgCtx* g, SgXBuf* b) {
    for (int k = 0; k < g->nfix; ++k) {
        long long to = g->label_at[g->fix[k].label];
        size_t at = g->fix[k].at;
        if (to < 0) return -1;
        long long d = to - (long long)(at + 4);
        if (at + 4 <= b->cap) {
            b->p[at] = (unsigned char)d;
            b->p[at + 1] = (unsigned char)(d >> 8);
            b->p[at + 2] = (unsigned char)(d >> 16);
            b->p[at + 3] = (unsigned char)(d >> 24);
        }
    }
    return 0;
}

/* x64.h's: the state nothing reads past a region's end, which no liveness knows here */
static inline int sgx_dead_words(SnxU64 dead, SnxU32 vdead, unsigned short* words) {
    (void)dead, (void)vdead, (void)words;
    return 0;
}

/* ---- what the block compiler decides ---- */

/* the guest's architecture version (arm.sail's ArchVersion): the Cortex-A57's, the one profile this back end makes */
static inline long long sg_ArchVersion(SgCtx* g) {
    (void)g;
    return 8;
}
/* InstrSet's member (InstrSet_A32 1, InstrSet_T32 2): the block's until the instruction selects the other */
static inline int sg_CurrentInstrSet(SgCtx* g) { return g->iset ? g->iset : g->thumb ? 2 : 1; }
static inline void sg_SetInstrSet(SgCtx* g, int iset) { g->iset = iset; }
/* ITSTATE before the instruction (0 in an A32 block, or where no IT block is open) */
static inline SgS sg_ITSTATE(SgCtx* g) { return sg_s(g->itstate, 8); }
/* IT's new ITSTATE, which the block compiler takes for the instructions after it: known when it recompiles */
static inline void sg_ITSTATE_set(SgCtx* g, SgD v) {
    if (!g->thumb || !sgx_is_const(&v)) sg_fail("an ITSTATE known only at run time", (long long)g->word, g->thumb);
    g->it_set = 1;
    g->it_next = (unsigned)v.k;
}
/* CurrentCond (the Arm ARM's shared AArch32.CurrentCond): an A32 word's condition; in Thumb the T1 and T3 B<c>'s own,
   else the IT block's while one is open, else AL */
static inline SgS sg_AArch32_CurrentCond(SgCtx* g) {
    if (!g->thumb) return sg_s(g->word >> 28, 4);
    if (g->word < 0x10000 && (g->word & 0xF000) == 0xD000 && ((g->word >> 9) & 7) != 7) /* T1: 1101 cccc, not 111x */
        return sg_s(g->word >> 8, 4);
    if (g->word >= 0x10000 && (g->word & 0xF800D000u) == 0xF0008000u && ((g->word >> 23) & 7) != 7) /* T3 */
        return sg_s(g->word >> 22, 4);
    return sg_s(g->itstate & 15 ? g->itstate >> 4 : 14, 4);
}
/* ConditionHolds(cond) on the flags NZCV was last written from, while the x86 flags are still them: the condition code
   the selection gave Arm's cond on that form's flags. 0 otherwise: the Sail then reads PSTATE */
static inline SgB sg_condition_origin(SgCtx* g, SgS cond) {
    SgX* x = sgx(g);
    if (g->origin < 0 || x->flags_ins != g->origin || cond.v >= 14) return 0;
    int cc = x->ins[g->origin].fsel->cc_of[cond.v];
    if (cc < 0) return 0;
    return sgx_bool(x, SGX_B_CC, g->origin, cc);
}

/* ---- the state ---- */

static inline SgD sg_GPR_read(SgCtx* g, long long n) {
    SgX* x = sgx(g);
    if (n < 0 || n > 14) sg_fail("a register past R14 read", n, 14);
    if (!g->gpr[n]) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 32;
        i->disp = (int)(offsetof(SnxArmCPU, r) + 4 * (size_t)n);
        i->def = sgx_vreg(x, x->n - 1, 32);
        g->gpr[n] = i->def;
    }
    return sgx_val(32, g->gpr[n]);
}
static inline void sgx_a32_commit(SgCtx* g);
/* the state's field at disp (w bits: 32 or 64) = v: the register v is in */
static inline int sgx_a32_put(SgCtx* g, size_t disp, unsigned w, SgD v) {
    SgX* x = sgx(g);
    int r = sgx_reg(x, &v);
    SgXIns* i = sgx_append(x, SGX_K_STORE);
    i->w = (unsigned char)w;
    i->in[0] = r;
    i->disp = (int)disp;
    return r;
}
/* R[n] = v, stored at once (not where a multiple load defers it: sg_registers_flush) */
static inline void sgx_a32_set(SgCtx* g, long long n, SgD v) {
    if (n < 0 || n > 14) sg_fail("a register past R14 written", n, 14);
    if (v.w != 32) sg_fail("a register written from a value of another width", n, v.w);
    g->gpr[n] = sgx_a32_put(g, offsetof(SnxArmCPU, r) + 4 * (size_t)n, 32, v);
}
static inline void sg_GPR_write(SgCtx* g, long long n, SgD v) {
    sgx_a32_commit(g);
    if (g->group && !g->group_write) {
        /* a multiple load's: every word read before a register is written, as the block compilers read them */
        if (n < 0 || n > 14) sg_fail("a register past R14 written", n, 14);
        g->deferred |= 1u << n;
        g->dvalue[n] = v;
        return;
    }
    sgx_a32_set(g, n, v);
}
static inline void sg_registers_flush(SgCtx* g) {
    for (unsigned n = 0; n < 15; ++n)
        if (g->deferred >> n & 1) sgx_a32_set(g, n, g->dvalue[n]);
    g->deferred = 0;
}
/* CPSR as the region has it */
static inline SgD sgx_a32_cpsr(SgCtx* g) {
    SgX* x = sgx(g);
    if (!g->cpsr) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 32;
        i->disp = (int)offsetof(SnxArmCPU, cpsr);
        i->def = sgx_vreg(x, x->n - 1, 32);
        g->cpsr = i->def;
    }
    return sgx_val(32, g->cpsr);
}

/* PSTATE's fields this back end keeps, numbered as ProcState's, and their bits in CPSR */
enum { SG_PSTATE_N = 0, SG_PSTATE_Z = 1, SG_PSTATE_C = 2, SG_PSTATE_V = 3, SG_PSTATE_T = 29, SG_PSTATE_E = 30 };
static inline unsigned sg_pstate_bit(long long field) {
    switch (field) {
        case SG_PSTATE_N: return 31;
        case SG_PSTATE_Z: return 30;
        case SG_PSTATE_C: return 29;
        case SG_PSTATE_V: return 28;
        case SG_PSTATE_T: return 5;
        case SG_PSTATE_E: return 9;
        default: sg_fail("a field of PSTATE this back end does not keep", field, 0);
    }
}
static inline SgD sg_PSTATE_read(SgCtx* g, long long field) {
    unsigned bit = sg_pstate_bit(field);
    if (g->pending >> field & 1) return g->field[field];
    /* T and E: as the block's entry checked them (a Thumb block's T set), until an instruction of the block writes them */
    if (field == SG_PSTATE_T) return sgx_const(1, (SnxU64)g->thumb);
    if (field == SG_PSTATE_E) return sgx_const(1, 0);
    return sg_d_slice(g, sgx_a32_cpsr(g), bit, bit);
}
/* PSTATE's fields first to first + count - 1 = v, v's top bit the first, kept for sg_flags_flush. NZCV from AddWithCarry
   is the four condition codes on the flags of the instruction that made them (the selection's map), which stay its
   condition codes until those flags end (sgx_flags_clobbered makes them 0 or 1 in registers then) */
static inline void sg_PSTATE_write(SgCtx* g, long long first, long long count, SgD v) {
    SgX* x = sgx(g);
    sgx_a32_commit(g);
    if (first < 0 || count < 1 || first + count > 32)
        sg_fail("fields of PSTATE this back end does not keep", first, count);
    if (v.w != (unsigned)count) sg_fail("fields written from a value of another width", count, v.w);
    if (count == 4 && first == SG_PSTATE_N && v.nzcv) {
        int from = v.nzcv - 1;
        const SgXSel* fs = x->ins[from].fsel;
        if (x->flags_ins != from) sg_fail("NZCV written after the x86 flags that are them were overwritten", from, 0);
        for (int f = 0; f < 4; ++f) {
            int cc = sgx_arm_flag_cc(fs, f);
            g->field[f] = cc < 0 ? sgx_const(1, 0) : sgx_bool_bits(x, sgx_bool(x, SGX_B_CC, from, cc));
            g->pending |= 1u << f;
        }
        g->origin = from;
        return;
    }
    if (v.nzcv) sgx_reg(x, &v);
    if (first == SG_PSTATE_T && count == 1 && !(g->pending >> SG_PSTATE_T & 1) && sgx_is_const(&v) &&
        (v.k & 1) == (SnxU64)g->thumb)
        return; /* the block's own instruction set (BL's SelectInstrSet): CPSR keeps it */
    for (long long f = 0; f < count; ++f) {
        (void)sg_pstate_bit(first + f);
        g->field[first + f] = sg_d_slice(g, v, count - 1 - f, count - 1 - f);
        g->pending |= 1u << (first + f);
        if (first + f <= SG_PSTATE_V) g->origin = -1;
    }
}
/* the fields written, in one store of CPSR: its other bits, each field ORed in at its bit */
static inline void sg_flags_flush(SgCtx* g) {
    if (!g->pending) return;
    SnxU32 mask = 0;
    for (unsigned k = 0; k < 32; ++k)
        if (g->pending >> k & 1) mask |= 1u << sg_pstate_bit(k);
    SgD c = sg_d_and(g, sgx_a32_cpsr(g), sgx_const(32, ~mask));
    for (unsigned k = 0; k < 32; ++k)
        if (g->pending >> k & 1) c = sg_d_or(g, c, sg_d_lsl(g, sg_d_zext(g, g->field[k], 32), sg_pstate_bit(k)));
    g->cpsr = sgx_a32_put(g, offsetof(SnxArmCPU, cpsr), 32, c);
    g->pending = 0;
}

/* AddWithCarry is x64.h's: its NZCV the flags of the instruction that made it */

/* ---- the machine's instructions ---- */

enum {
    SGX_A32_JCC = 1, /* to label disp on cc (flags_from): a condition's skip */
    SGX_A32_JMP,     /* to label disp */
    SGX_A32_EXIT,    /* to a new exit on cc: exit imm, R15 kv[1] unless kv[0] bit 1, syndrome in[0] (or kv[0] >> 32) */
    SGX_A32_MONITOR, /* a store's check of the monitor: the address in[0], imm bytes */
    SGX_A32_FENCE,   /* MFENCE */
    SGX_A32_CALL,    /* sym(state, in[0..]): RSI, RDX, RCX, R8; the result in RAX where def */
    SGX_A32_STREX,   /* the exclusive store: address RSI, value RDX, memory's base R10, imm bytes; status in RAX */
    SGX_A32_UNDEF_END, /* after snx_arm_undefined: R15 = imm where it set no exit, then the block's return */
    SGX_A32_CHECK      /* after a call of the backend's: the block's return where it set an exit */
};
static const signed char sgx_a32_args[4] = {6, 2, 1, 8};

/* a machine instruction, after the fields written are stored: whatever it does may leave the region (a jump, an exit)
   or read the state (a call). One that changes the flags (clobbers) ends the conditions on them first */
static inline SgXIns* sgx_a32_machine(SgCtx* g, int op, int clobbers) {
    SgX* x = sgx(g);
    sg_flags_flush(g);
    if (clobbers) sgx_flags_clobbered(x);
    SgXIns* i = sgx_append(x, SGX_K_MACHINE);
    i->op = (unsigned char)op;
    if (clobbers) x->flags_ins = -1;
    return i;
}
/* a jump on c (its condition code on the flags as they are now) */
static inline SgXIns* sgx_a32_on(SgCtx* g, int op, SgB c) {
    SgX* x = sgx(g);
    sg_flags_flush(g);
    int from = 0, cc = sgx_bool_cc(x, c, &from);
    SgXIns* i = sgx_append(x, SGX_K_MACHINE);
    i->op = (unsigned char)op;
    i->cc = (unsigned char)cc;
    i->flags_from = from;
    return i;
}
/* where c holds: exit code (R15 the instruction's unless pc_kept), the syndrome the value s */
static inline void sgx_a32_exit(SgCtx* g, SgB c, unsigned code, SgD s, int pc_kept) {
    SgX* x = sgx(g);
    SgXBool b = x->b[c];
    if (b.kind == SGX_B_CONST && !b.v) return;
    if (!sgx_is_const(&s)) sgx_reg(x, &s);
    SgXIns* i = sgx_a32_on(g, SGX_A32_EXIT, c);
    i->imm = code;
    i->kv[1] = (SnxU32)g->pc;
    i->kv[0] = (SnxU64)(pc_kept ? 2 : 0) | (sgx_is_const(&s) ? 1 | (SnxU64)(SnxU32)s.k << 32 : 0);
    if (!sgx_is_const(&s)) i->in[0] = s.v;
}
/* x64.h's: AddWithCarry's flags, which the instruction writes to PSTATE after its result's register, made past the
   condition's skip (whose jump would end them) */
static inline void sgx_flags_begin(SgCtx* g) { sgx_a32_commit(g); }
/* the condition's skip, where the instruction first changes something: a jump past it where the condition fails */
static inline void sgx_a32_commit(SgCtx* g) {
    SgB c = g->when;
    if (!c) return;
    g->when = 0;
    g->skip = sgx_a32_label(g);
    sgx_a32_on(g, SGX_A32_JCC, sg_b_not(g, c))->disp = g->skip;
}

/* ---- the flow ---- */

/* the rest of the instruction runs where c holds (sailgen's decode body: `if ConditionPassed() then ...`) */
static inline void sg_when(SgCtx* g, SgB c) {
    SgX* x = sgx(g);
    if (x->b[c].kind == SGX_B_CONST && x->b[c].v) return;
    if (g->when || g->skip >= 0) sg_fail("an instruction under two conditions", (long long)g->word, 0);
    g->when = c;
}
/* a clause that hands its word on to the next clause: it read the word's fields, and maybe registers and flags for its
   condition, which nothing reads; its condition, not made yet, goes with it */
static inline int sg_see(SgCtx* g) {
    if (g->skip >= 0 || g->branched || g->undefined)
        sg_fail("a word handed on to the next clause after its own code", g->word, 0);
    g->when = 0;
    return 0;
}
/* BranchTo(target, type, conditional): R15 the target where the instruction ends the block, under when (the next
   instruction where it does not hold) */
static inline void sg_branch_to(SgCtx* g, SgD target, SgB when) {
    sgx_a32_commit(g);
    if (g->branched) sg_fail("an instruction that branches twice", (long long)g->word, 0);
    if (target.w != 32) sg_fail("a branch to a target of other than 32 bits", target.w, 32);
    g->branched = 1;
    g->target = when ? sg_d_select(g, when, target, sgx_const(32, g->next)) : target;
}
/* R15 = v */
static inline void sgx_a32_pc(SgCtx* g, SgD v) { sgx_a32_put(g, offsetof(SnxArmCPU, r) + 60, 32, v); }
/* UNDEFINED: the backend's to run or stop at (llvm.c's trap): snx_arm_undefined(state, word, SNX_ARM_UNDEFINED) with
   R15 the instruction's; its exit returns, else the block goes on at the next instruction (x64_arm.c ends it there) */
static inline void sgx_a32_call(SgCtx* g, const char* sym, SgD* args, int nargs, int result);
static inline void sg_undefined(SgCtx* g) {
    sgx_a32_commit(g);
    sgx_a32_pc(g, sgx_const(32, (SnxU32)g->pc));
    SgD args[2] = {sgx_const(32, g->word), sgx_const(32, SNX_ARM_UNDEFINED)};
    sgx_a32_call(g, "snx_arm_undefined", args, 2, 0);
    sgx_a32_machine(g, SGX_A32_UNDEF_END, 1)->imm = g->next;
    g->undefined = 1;
}
/* a Thumb ITSTATE in CPSR: [1:0] in bits 26:25, [7:2] in 15:10 (SG_IT_BITS) */
#define SG_IT_BITS 0x0600FC00u
static inline SnxU32 sg_it_bits(unsigned itstate) { return (SnxU32)((itstate & 3u) << 25 | (itstate >> 2) << 10); }
/* SVC and BKPT: R15 past the instruction and the exit, the immediate its syndrome (llvm_arm.h's sg_exception); in Thumb
   an IT block's ITSTATE after it left in CPSR */
static inline void sg_exception(SgCtx* g, unsigned code, SgD imm) {
    sgx_a32_commit(g);
    sg_flags_flush(g);
    sgx_a32_pc(g, sgx_const(32, g->next));
    if (g->it_after) { /* Thumb: the ITSTATE after it in CPSR's IT bits (llvm_thumb.inc's it_leave) */
        SgD c = sg_d_or(g, sgx_a32_cpsr(g), sgx_const(32, sg_it_bits(g->it_after)));
        g->cpsr = sgx_a32_put(g, offsetof(SnxArmCPU, cpsr), 32, c);
    }
    sgx_a32_put(g, offsetof(SnxArmCPU, syndrome), 32, sg_d_zext(g, imm, 32));
    sgx_a32_put(g, offsetof(SnxArmCPU, exit), 32, sgx_const(32, code));
    sgx_a32_machine(g, SGX_A32_JMP, 0)->disp = g->ret;
    g->undefined = 1; /* the instruction's way leaves the block */
}
static inline void sg_AArch32_CallSupervisor(SgCtx* g, SgD imm) { sg_exception(g, SNX_ARM_SVC, imm); }
static inline void sg_AArch32_SoftwareBreakpoint(SgCtx* g, SgD imm) { sg_exception(g, SNX_ARM_BREAKPOINT, imm); }
/* the ARM7TDMI's MULS's carry and its trace: neither is the Cortex-A57's */
static inline void sg_MultiplyCarry(SgCtx* g) { sg_fail("the ARM7TDMI's multiply carry", (long long)g->word, 0); }
static inline void sg_TraceMultiplier(SgCtx* g, SgD multiplier) {
    (void)g;
    (void)multiplier;
}

/* ---- calls ---- */

/* sym(state, args...): every value in a caller-saved register spilled, the arguments in RSI, RDX, RCX, R8; the call
   may change any of the state, so the region's registers and CPSR are read again after it */
static inline void sgx_a32_call(SgCtx* g, const char* sym, SgD* args, int nargs, int result) {
    SgX* x = sgx(g);
    int regs[4];
    if (nargs > 4) sg_fail("a call of more than four arguments", nargs, 4);
    for (int k = 0; k < nargs; ++k) regs[k] = sgx_reg(x, &args[k]);
    SgXIns* i = sgx_a32_machine(g, SGX_A32_CALL, 1);
    i->sym = sym;
    i->calls = 1;
    for (int k = 0; k < nargs; ++k) {
        i->in[k] = regs[k];
        i->fixed[k] = sgx_a32_args[k];
    }
    if (result) i->def = sgx_vreg(x, x->n - 1, 64);
    for (int k = 0; k < 15; ++k) g->gpr[k] = 0;
    g->cpsr = 0;
    g->origin = -1;
    g->vfp_ok[0] = g->vfp_ok[1] = 0;
}

/* ---- guest memory ---- */

/* the region's min(mem_size, 2^32): an access whose last byte is past it aborts (llvm.c's bounds) */
static inline SgD sgx_a32_limit(SgCtx* g) {
    SgX* x = sgx(g);
    if (!g->limit) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 64;
        i->disp = (int)offsetof(SnxArmCPU, mem_size);
        i->def = sgx_vreg(x, x->n - 1, 64);
        SgD size = sgx_val(64, i->def), top = sgx_const(64, 1ull << 32);
        SgD l = sg_d_select(g, sgx_d_ult(g, top, size), top, size);
        g->limit = sgx_reg(x, &l);
    }
    return sgx_val(64, g->limit);
}
static inline int sgx_a32_mem(SgCtx* g) {
    SgX* x = sgx(g);
    if (!g->mem) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 64;
        i->disp = (int)offsetof(SnxArmCPU, mem);
        i->def = sgx_vreg(x, x->n - 1, 64);
        g->mem = i->def;
    }
    return g->mem;
}
/* an access of bytes at address (32 bits) checked: misaligned for alignment (a power of two, 1 for none) an
   ALIGNMENT exit, past the memory a DATA_ABORT one, both with the address as the syndrome (llvm.c's bounds) */
static inline void sgx_a32_bounds(SgCtx* g, SgD address, unsigned bytes, unsigned alignment) {
    SgX* x = sgx(g);
    SgD a = address;
    sgx_reg(x, &a);
    if (alignment > 1)
        sgx_a32_exit(g, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, a, sgx_const(32, alignment - 1)))), SNX_ARM_ALIGNMENT,
                     a, 0);
    SgD end = sg_d_add(g, sgx_zext(x, a, 64), sgx_const(64, bytes));
    sgx_a32_exit(g, sgx_d_ult(g, sgx_a32_limit(g), end), SNX_ARM_DATA_ABORT, a, 0);
}
/* the candidate of table whose r/m operand is its input (a load: rm_out 0) or its result (a store) */
static inline const SgXSel* sgx_a32_mem_form(const SgXSel* t, unsigned n, int rm_out) {
    for (unsigned k = 0; k < n; ++k)
        if (t[k].rm >= 0 && !t[k].feature && !t[k].zero && (rm_out ? t[k].out == t[k].rm : t[k].in[0] == t[k].rm))
            return &t[k];
    sg_fail("no form of a load or a store", (long long)n, rm_out);
}
/* [CPU.mem + address + disp] of bytes 1, 2, 4 or 8, zero extended (the address 32 bits, checked already) */
static inline SgD sgx_a32_load(SgCtx* g, SgD address, int bytes, int disp) {
    SgX* x = sgx(g);
    const SgXSel* s = bytes == 1   ? sgx_a32_mem_form(sgx_sel_zext8_32, sgx_sel_zext8_32_n, 0)
                      : bytes == 2 ? sgx_a32_mem_form(sgx_sel_zext16_32, sgx_sel_zext16_32_n, 0)
                      : bytes == 4 ? sgx_a32_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 0)
                                   : sgx_a32_mem_form(sgx_sel_mov_64, sgx_sel_mov_64_n, 0);
    SgD a = sgx_zext(x, address, 64);
    int m = sgx_a32_mem(g), r = sgx_reg(x, &a);
    SgXIns* i = sgx_append(x, SGX_K_SEL);
    i->sel = s;
    i->fsel = s;
    i->w = (unsigned char)(bytes == 8 ? 64 : 32);
    i->mplace = s->in[0];
    i->in[5] = m;
    i->in[6] = r;
    i->mdisp = disp;
    i->def = sgx_vreg(x, x->n - 1, (unsigned)(8 * bytes));
    return sgx_val((unsigned)(8 * bytes), i->def);
}
static inline void sgx_a32_store(SgCtx* g, SgD address, int bytes, int disp, SgD value) {
    SgX* x = sgx(g);
    const SgXSel* s = bytes == 1   ? sgx_a32_mem_form(sgx_sel_mov_8, sgx_sel_mov_8_n, 1)
                      : bytes == 2 ? sgx_a32_mem_form(sgx_sel_mov_16, sgx_sel_mov_16_n, 1)
                      : bytes == 4 ? sgx_a32_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 1)
                                   : sgx_a32_mem_form(sgx_sel_mov_64, sgx_sel_mov_64_n, 1);
    SgD a = sgx_zext(x, address, 64);
    int v = sgx_reg(x, &value), m = sgx_a32_mem(g), r = sgx_reg(x, &a);
    SgXIns* i = sgx_append(x, SGX_K_SEL);
    i->sel = s;
    i->fsel = s;
    i->w = (unsigned char)(bytes == 8 ? 64 : 32);
    i->mplace = s->out;
    i->in[0] = v;
    i->in[5] = m;
    i->in[6] = r;
    i->mdisp = disp;
    i->side = 1;
}
/* after a store of bytes at address: the exclusive monitor cleared where the store overlaps its reservation */
static inline void sgx_a32_monitor(SgCtx* g, SgD address, unsigned bytes) {
    SgX* x = sgx(g);
    int r = sgx_reg(x, &address);
    SgXIns* i = sgx_a32_machine(g, SGX_A32_MONITOR, 1);
    i->in[0] = r;
    i->imm = bytes;
}
/* a multiple transfer's range, checked before its first access (arm.sail's GuestTransfer): word aligned on the
   Cortex-A57 */
static inline void sg_GuestTransfer(SgCtx* g, SgD address, long long bytes, int write) {
    sgx_a32_commit(g);
    if (g->group) sg_fail("a second multiple transfer in one instruction", (long long)g->word, 0);
    sgx_reg(sgx(g), &address);
    sgx_a32_bounds(g, address, (unsigned)bytes, 4);
    g->group = 1;
    g->group_write = write;
    g->group_start = address;
}
/* flat memory takes a transfer of no bytes: its address checked, nothing moved */
static inline int sg_GuestTransferEmpty(SgCtx* g) {
    (void)g;
    return 1;
}
static inline void sg_transfer_commit(SgCtx* g) { (void)g; }
static inline SgD sg_GuestRead(SgCtx* g, SgD address, long long size, int aligned) {
    (void)aligned;
    sgx_a32_commit(g);
    if (size != 1 && size != 2 && size != 4) sg_fail("a guest read of this size", size, 0);
    if (!g->group) sgx_a32_bounds(g, address, (unsigned)size, 1);
    return sgx_a32_load(g, address, (int)size, 0);
}
static inline SgD sg_GuestReadHalfwordV4(SgCtx* g, SgD address, int sign) {
    (void)address;
    (void)sign;
    sg_fail("an ARMv4T halfword load", (long long)g->word, 0);
}
static inline void sg_GuestWrite(SgCtx* g, SgD address, long long size, int aligned, SgD value) {
    (void)aligned;
    sgx_a32_commit(g);
    if (size != 1 && size != 2 && size != 4) sg_fail("a guest write of this size", size, 0);
    if (!g->group) sgx_a32_bounds(g, address, (unsigned)size, 1);
    sgx_reg(sgx(g), &address);
    sgx_a32_store(g, address, (int)size, 0, value);
    sgx_a32_monitor(g, address, (unsigned)size);
}

/* ---- conditions for the instructions the Sail has no clause for (ConditionPassed, as the Sail's) ---- */

static inline SgB sgx_a32_condition(SgCtx* g, unsigned cond) {
    if (cond >= 14) return sg_bool(g, 1);
    SgB origin = sg_condition_origin(g, sg_s(cond, 4)), r;
    if (origin) return origin;
    SgD one = sgx_const(1, 1), zero = sgx_const(1, 0);
    switch (cond >> 1) {
        case 0: r = sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_Z), one); break;
        case 1: r = sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_C), one); break;
        case 2: r = sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_N), one); break;
        case 3: r = sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_V), one); break;
        case 4: {
            SgB c = sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_C), one);
            r = sg_b_and(g, c, sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_Z), zero));
            break;
        }
        case 5: r = sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_N), sg_PSTATE_read(g, SG_PSTATE_V)); break;
        default: {
            SgB ge = sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_N), sg_PSTATE_read(g, SG_PSTATE_V));
            r = sg_b_and(g, ge, sg_d_eq(g, sg_PSTATE_read(g, SG_PSTATE_Z), zero));
            break;
        }
    }
    return cond & 1 ? sg_b_not(g, r) : r;
}

/* ---- the machine's bytes ---- */

/* the rel32 at at, to b's end */
static inline void sgx_a32_patch32(SgXBuf* b, size_t at) {
    long long d = (long long)b->n - (long long)(at + 4);
    if (at + 4 <= b->cap) {
        b->p[at] = (unsigned char)d;
        b->p[at + 1] = (unsigned char)(d >> 8);
        b->p[at + 2] = (unsigned char)(d >> 16);
        b->p[at + 3] = (unsigned char)(d >> 24);
    }
}
/* the rel8 at at, to b's end (a short jump of the cold paths) */
static inline void sgx_a32_patch8(SgXBuf* b, size_t at) {
    if (at < b->cap) b->p[at] = (unsigned char)(b->n - (at + 1));
}
/* an instruction [rbx + disp32] of the state: op8 (and REX.W where w64), ModRM's reg field reg */
static inline void sgx_a32_state(SgXBuf* b, int w64, unsigned op8, int reg, size_t disp) {
    if (w64 || reg >= 8) sgx_byte(b, (unsigned char)(0x40 | (w64 ? 8 : 0) | (reg >> 3) << 2));
    sgx_byte(b, (unsigned char)op8);
    if (disp < 128) { /* [rbx + disp8] */
        sgx_byte(b, (unsigned char)(0x43 | (reg & 7) << 3));
        sgx_byte(b, (unsigned char)disp);
        return;
    }
    sgx_byte(b, (unsigned char)(0x83 | (reg & 7) << 3)); /* [rbx + disp32] */
    sgx_imm(b, (long long)disp, 4);
}
/* mov dword [rbx + disp], imm32 */
static inline void sgx_a32_state_imm(SgXBuf* b, size_t disp, SnxU32 v) {
    sgx_a32_state(b, 0, 0xC7, 0, disp);
    sgx_imm(b, (long long)v, 4);
}
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r);
static inline void sgx_machine_ins(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgCtx* g = (SgCtx*)a->machine;
    SgXBuf* b = a->b;
    if (i->kind != SGX_K_MACHINE) sg_fail("a machine instruction of another back end", i->kind, 0);
    switch (i->op) {
        case SGX_A32_JCC: sgx_a32_jcc(g, b, i->cc, i->disp); return;
        case SGX_A32_JMP: sgx_a32_jmp(g, b, i->disp); return;
        case SGX_A32_EXIT: {
            SgA32Exit e = {
                .label = sgx_a32_label(g),
                .exit = (unsigned)i->imm,
                .reg = i->kv[0] & 1 ? (signed char)-1 : (signed char)r[0],
                .pc_kept = (signed char)(i->kv[0] >> 1 & 1),
                .syndrome = (SnxU32)(i->kv[0] >> 32),
                .pc = (SnxU32)i->kv[1],
            };
            g->exits = (SgA32Exit*)sgx_a32_grow(g->exits, &g->cap_exits, g->nexits + 1, sizeof *g->exits);
            g->exits[g->nexits++] = e;
            sgx_a32_jcc(g, b, i->cc, e.label);
            return;
        }
        case SGX_A32_MONITOR: {
            sgx_a32_state(b, 0, 0x83, 7, offsetof(SnxArmCPU, exclusive_valid)); /* cmp dword [rbx + valid], imm8 */
            sgx_byte(b, 0);
            SgA32Monitor m = {.label = sgx_a32_label(g), .reg = (signed char)r[0], .bytes = (unsigned)i->imm};
            sgx_a32_jcc(g, b, 5, m.label); /* jne: armed */
            m.back = b->n;
            g->monitors =
                (SgA32Monitor*)sgx_a32_grow(g->monitors, &g->cap_monitors, g->nmonitors + 1, sizeof *g->monitors);
            g->monitors[g->nmonitors++] = m;
            return;
        }
        case SGX_A32_FENCE: sgx_bytes(b, "0F AE F0"); return; /* mfence */
        case SGX_A32_CALL:
            sgx_bytes(b, "48 89 DF"); /* mov rdi, rbx */
            if (a->reloc) {
                sgx_byte(b, 0xE8); /* call rel32, which the object's relocation fills */
                a->reloc(a->machine, b->n, i->sym, -4);
                sgx_imm(b, 0, 4);
            } else {
                SnxU64 to = a->resolve ? a->resolve(i->sym) : 0;
                if (!to) sg_fail("a helper nothing resolves", 0, 0);
                sgx_bytes(b, "48 B8"); /* mov rax, imm64; call rax */
                sgx_imm(b, (long long)to, 8);
                sgx_bytes(b, "FF D0");
            }
            return;
        case SGX_A32_STREX: {
            /* llvm.c's exclusive(): the reservation's address and size checked and the monitor cleared, then a
           compare-and-swap of the value the load-exclusive read: status 0 where it took, 1 otherwise */
            unsigned bytes = (unsigned)i->imm;
            size_t fail[3];
            sgx_a32_state(b, 0, 0x8B, 1, offsetof(SnxArmCPU, exclusive_valid)); /* mov ecx, [rbx + valid] */
            sgx_a32_state_imm(b, offsetof(SnxArmCPU, exclusive_valid), 0);
            sgx_bytes(b, "85 C9 0F 84"), fail[0] = b->n, sgx_imm(b, 0, 4);        /* test ecx, ecx; jz */
            sgx_a32_state(b, 0, 0x3B, 6, offsetof(SnxArmCPU, exclusive_address)); /* cmp esi, [rbx + address] */
            sgx_bytes(b, "0F 85"), fail[1] = b->n, sgx_imm(b, 0, 4);              /* jne */
            sgx_a32_state(b, 0, 0x83, 7, offsetof(SnxArmCPU, exclusive_size));    /* cmp dword [rbx + size], bytes */
            sgx_byte(b, (unsigned char)bytes);
            sgx_bytes(b, "0F 85"), fail[2] = b->n, sgx_imm(b, 0, 4);            /* jne */
            sgx_a32_state(b, 1, 0x8B, 0, offsetof(SnxArmCPU, exclusive_value)); /* mov rax, [rbx + value] */
            /* lock cmpxchg [r10 + rsi], dl / dx / edx / rdx */
            sgx_byte(b, 0xF0);
            if (bytes == 2) sgx_byte(b, 0x66);
            sgx_byte(b, (unsigned char)(0x41 | (bytes == 8 ? 8 : 0)));
            sgx_bytes(b, bytes == 1 ? "0F B0" : "0F B1");
            sgx_bytes(b, "14 32");
            sgx_bytes(b, "0F 95 C0 0F B6 C0 E9"); /* setne al; movzx eax, al; jmp done */
            size_t done = b->n;
            sgx_imm(b, 0, 4);
            sgx_a32_patch32(b, fail[0]), sgx_a32_patch32(b, fail[1]), sgx_a32_patch32(b, fail[2]);
            sgx_bytes(b, "B8 01 00 00 00"); /* mov eax, 1 */
            sgx_a32_patch32(b, done);
            return;
        }
        case SGX_A32_CHECK:
            sgx_a32_state(b, 0, 0x83, 7, offsetof(SnxArmCPU, exit)); /* cmp dword [rbx + exit], 0 */
            sgx_byte(b, 0);
            sgx_a32_jcc(g, b, 5, g->ret);
            return;
        case SGX_A32_UNDEF_END:
            sgx_a32_state(b, 0, 0x83, 7, offsetof(SnxArmCPU, exit)); /* cmp dword [rbx + exit], 0 */
            sgx_byte(b, 0);
            sgx_a32_jcc(g, b, 5, g->ret);                                      /* jne: the backend stopped there */
            sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, (SnxU32)i->imm); /* else R15 = the next instruction */
            sgx_a32_jmp(g, b, g->ret);
            return;
        default: sgx_machine_op(a, at, r); return;
    }
}

/* the block's exits and the monitor's cold paths, after its code: each exit's fields stored, then the block's return */
static inline void sgx_a32_stubs(SgCtx* g, SgXBuf* b) {
    for (int k = 0; k < g->nexits; ++k) {
        SgA32Exit* e = &g->exits[k];
        sgx_a32_bind(g, b, e->label);
        if (!e->pc_kept) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, e->pc);
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, exit), e->exit);
        if (e->reg < 0)
            sgx_a32_state_imm(b, offsetof(SnxArmCPU, syndrome), e->syndrome);
        else
            sgx_a32_state(b, 0, 0x89, e->reg, offsetof(SnxArmCPU, syndrome)); /* mov [rbx + syndrome], reg32 */
        sgx_a32_jmp(g, b, g->ret);
    }
    for (int k = 0; k < g->nmonitors; ++k) {
        SgA32Monitor* m = &g->monitors[k];
        sgx_a32_bind(g, b, m->label);
        sgx_bytes(b, "50"); /* push rax */
        if (m->reg != 0) {  /* mov eax, reg */
            if (m->reg >= 8) sgx_byte(b, 0x44);
            sgx_byte(b, 0x89);
            sgx_byte(b, (unsigned char)(0xC0 | (m->reg & 7) << 3));
        }
        /* (address - reserved) < size, or (reserved - address) < bytes: the two ranges overlap, 32-bit wrapping */
        sgx_a32_state(b, 0, 0x2B, 0, offsetof(SnxArmCPU, exclusive_address)); /* sub eax, [rbx + address] */
        sgx_a32_state(b, 0, 0x3B, 0, offsetof(SnxArmCPU, exclusive_size));    /* cmp eax, [rbx + size] */
        sgx_bytes(b, "72");                                                   /* jb clear */
        size_t clear = b->n;
        sgx_byte(b, 0);
        sgx_a32_state(b, 0, 0x8B, 0, offsetof(SnxArmCPU, exclusive_address)); /* mov eax, [rbx + address] */
        if (m->reg == 0)
            sgx_bytes(b, "2B 04 24"); /* sub eax, [rsp]: the address, pushed */
        else {
            if (m->reg >= 8) sgx_byte(b, 0x44);
            sgx_byte(b, 0x29); /* sub eax, reg */
            sgx_byte(b, (unsigned char)(0xC0 | (m->reg & 7) << 3));
        }
        sgx_byte(b, 0x3D), sgx_imm(b, m->bytes, 4); /* cmp eax, bytes */
        sgx_bytes(b, "73");                         /* jae keep */
        size_t keep = b->n;
        sgx_byte(b, 0);
        sgx_a32_patch8(b, clear);
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, exclusive_valid), 0);
        sgx_a32_patch8(b, keep);
        sgx_bytes(b, "58 E9"); /* pop rax; jmp back */
        sgx_imm(b, (long long)m->back - (long long)(b->n + 4), 4);
    }
    g->nexits = g->nmonitors = 0;
}

/* the dispatchers sailgen generates after this file (a32.roots' decoders) */
static int sg_decode_t16(SgCtx* g);
static int sg_decode_a32(SgCtx* g);

/* the instruction's end: what it wrote stored where it ran under a condition (its skip then closes the region,
   x64_arm.c's), else kept for the next; a multiple load's registers in their order */
static inline void sgx_a32_finish(SgCtx* g) {
    sg_registers_flush(g);
    g->group = 0;
    g->when = 0;
    if (g->skip >= 0) sg_flags_flush(g);
}
