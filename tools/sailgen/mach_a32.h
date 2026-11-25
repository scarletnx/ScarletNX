/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's machine back ends, AArch32's part any machine's shares (after the machine's assembly and before its own
   AArch32 part, x64_a32.h or arm64_a32.h), as llvm_arm.h is LLVM's: the registers and CPSR where SnxArmCPU keeps them
   (arm.h), its address in the machine's SGX_BASE register. A register read once is the region's value of it after (a
   load, or what was written: every write is stored at once, and the stores a later one overwrites are dropped as the
   region ends). PSTATE's fields written are kept for one store of CPSR (sg_flags_flush), as llvm.c's nz wrote them, but
   past the instruction's end: NZCV from AddWithCarry stays the flags of the instruction that made them, which the next
   instruction's ConditionHolds reads as the condition code the selection derived (sg_condition_origin), until
   something needs CPSR in memory (a branch out of the region: a condition's skip, an exit, a call, the region's end).

   The guest's CPU and its memory are the block's options (codegen.h): the profile's ArchVersion for the Sail, and how
   an access reaches guest memory, as llvm.c's transfer_begin, readmem, writemem and transfer_commit have it.
   - Flat memory: an access whose bytes pass min(mem_size, 2^32) is a DATA_ABORT exit, R15 the instruction's, the
     syndrome its address, as llvm.c's bounds() makes it; a store clears the exclusive monitor where it overlaps the
     reservation (llvm.c's store_monitor), on a cold path.
   - Hosted memory: no range check (a guard faults on the host), the rest as flat memory.
   - Checked memory: each transfer is one call of the runtime library (rt_helpers.c's snx_arm_checked_access), its bytes
     in the state's words (checked.h): a load's read from there, a store's written there and then sent.
   - The ARM7TDMI's and the ARM946E's bus (flat memory): an access the bus hook takes (SnxArmCPU.io) leaves the hot code
     for a call of snx_arm_io_read or snx_arm_io_write (sgx_a32_bus), the rest is direct; a misaligned load rotates, a
     store aligns, and a write whose hook says so ends the block after its instruction (sgx_a32_yield_check). The
   condition of
   an instruction (ConditionPassed) is a jump past it, made where it first changes something (sg_when's deferred
   skip), so a clause that hands its word on (sg_see) has emitted nothing but what nothing reads. The machine's
   AArch32 part writes the jumps to labels, the machine instructions (SGX_A32_*) and the exits' and the monitor's
   stubs; the block compiler (mach/mach_arm.inc and the machine's glue) the prologue and its entry checks, the
   instructions the Sail has no clause for, the block's end and its chaining */

#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/checked.h"
#include "common/cpu/arm/codegen.h"
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
/* a way out of the block the machine's AArch32 part does not write itself (the block compiler's, mach_arm.inc): where
   undefined, the backend's (snx_arm_undefined of the word, for the reason) and R15 = next where it set no exit; else
   R15 = next alone (a yield) */
typedef struct {
    int label, undefined;
    unsigned reason;
    SnxU32 word, pc, next;
} SgA32Out;
/* an access that left the hot code for the bus hook (sgx_a32_bus): its label, where the hot code goes on, the runtime
   library's function and its arguments but the address and a store's value (already in their registers), and the
   caller-saved registers with values read after it, kept in the frame's slots from slot */
typedef struct {
    int label, back;
    const char* sym;
    SnxU32 size, key, pc;
    int load;
    unsigned live;
    int slot;
} SgA32Bus;
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
    SgB taken;     /* where it holds, the next instruction where it does not (CBZ); 0 for always */
    int gpr[15];   /* the region's value of each register, 0 for none */
    int cpsr;      /* the region's CPSR, 0 for none */
    SgD field[32]; /* PSTATE's fields written and not stored yet (pending's bits) */
    unsigned pending;
    int origin; /* the instruction whose host flags NZCV was last written from, -1 for none */
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
    /* the labels, by number: each one's offset in the code once bound (-1 before); the jumps to them (the machine's
       field of kind at at) */
    long long* label_at;
    int nlabels, cap_labels;
    struct SgA32Fix {
        size_t at;
        int label;
        int kind;
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
    /* the block's options (codegen.h), and its address, which a trace slot counts from */
    SnxArmProfile profile;
    SnxArmMemoryMode memory;
    SnxU32 trace, skipped, block_pc;
    /* what the bus hook's writes of the instruction returned, ORed (yields: there was one): nonzero ends the block */
    SgD yield;
    int yields;
    /* a checked transfer's address, size and operation (checked.h), a store's sent once (sent) */
    SgD transfer;
    unsigned transfer_bytes, transfer_op;
    int sent;
    unsigned group_at; /* the bytes of a multiple transfer already moved */
    SgA32Out* outs;
    int nouts, cap_outs;
    SgA32Bus* buses;
    int nbuses, cap_buses;
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
/* x64.h's: the state nothing reads past a region's end, which no liveness knows here */
static inline int sgx_dead_words(SnxU64 dead, SnxU32 vdead, unsigned short* words) {
    (void)dead, (void)vdead, (void)words;
    return 0;
}

/* ---- what the block compiler decides ---- */

/* the guest's architecture version (arm.sail's ArchVersion): the block's profile's */
static inline long long sg_ArchVersion(SgCtx* g) {
    switch (g->profile) {
        case SNX_ARM_PROFILE_ARM7TDMI: return 4;
        case SNX_ARM_PROFILE_ARM946E: return 5;
        case SNX_ARM_PROFILE_ARM11: return 6;
        case SNX_ARM_PROFILE_A9: return 7;
        default: return 8;
    }
}
static inline int sgx_a32_hosted(const SgCtx* g) { return g->memory == SNX_ARM_MEMORY_HOSTED; }
static inline int sgx_a32_checked(const SgCtx* g) { return g->memory == SNX_ARM_MEMORY_CHECKED; }
static inline int sgx_a32_arm7(const SgCtx* g) { return g->profile == SNX_ARM_PROFILE_ARM7TDMI; }
static inline int sgx_a32_arm9(const SgCtx* g) { return g->profile == SNX_ARM_PROFILE_ARM946E; }
/* the two whose bus has the hook, loads that rotate, stores that align and a PC stored as PC + 12 */
static inline int sgx_a32_legacy(const SgCtx* g) { return sgx_a32_arm7(g) || sgx_a32_arm9(g); }
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
/* ConditionHolds(cond) on the flags NZCV was last written from, while the host's flags are still them: the condition
   code
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
static inline void sg_transfer_commit(SgCtx* g);
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
    sg_transfer_commit(g);
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
        if (x->flags_ins != from)
            sg_fail("NZCV written after the host's flags that are them were overwritten", from, 0);
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
    SGX_A32_CALL,    /* sym(state, in[0..]) (SGX_A32_ARG's registers); the result in SGX_RET where def */
    SGX_A32_STREX,   /* the exclusive store: address, value, memory's base in SGX_A32_STREX_REGS', imm bytes;
                        the status in SGX_RET */
    SGX_A32_UNDEF_END, /* after snx_arm_undefined: R15 = imm where it set no exit, then the block's return */
    SGX_A32_CHECK,     /* after a call of the backend's: the block's return where it set an exit */
    /* the block compiler's own (mach_arm.inc's sgx_machine_op), on every machine: */
    SGX_A32_OUT, /* to a new way out (SgA32Out) on cc: kv[0] the word, imm its reason + 1, 0 for a yield; kv[1] the
                    instruction's address, and the next one's from bit 32 */
    SGX_A32_BUS, /* on cc, out of the hot code to sym(state, in[0] the address, imm its size, kv[0]'s low word or in[3]),
                    R15 kv[0]'s high word, else sel's access of [in[1] + in[2]]: a load's into SGX_RET (kv[1] 1), or a
                    store of in[3], SGX_RET then 0 */
    SGX_A32_FIELD, /* SGX_RET = the host's number of w bits at [in[0] + disp]: no guest memory */
    SGX_A32_TAIL   /* the block's end: a tail call of the block whose address is in[0] */
};

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
    sg_transfer_commit(g);
    g->branched = 1;
    g->target = target;
    g->taken = when;
}
/* R15 = v */
static inline void sgx_a32_pc(SgCtx* g, SgD v) { sgx_a32_put(g, offsetof(SnxArmCPU, r) + 60, 32, v); }
/* UNDEFINED, or an instruction the block cannot run (reason): the backend's to run or stop at (llvm.c's trap):
   snx_arm_undefined(state, word, reason) with R15 the instruction's; its exit returns, else the block ends with R15 the
   next instruction's */
static inline void sgx_a32_call(SgCtx* g, const char* sym, SgD* args, int nargs, int result);
static inline void sgx_a32_trap(SgCtx* g, unsigned reason) {
    sgx_a32_commit(g);
    sgx_a32_pc(g, sgx_const(32, (SnxU32)g->pc));
    SgD args[2] = {sgx_const(32, g->word), sgx_const(32, reason)};
    sgx_a32_call(g, "snx_arm_undefined", args, 2, 0);
    sgx_a32_machine(g, SGX_A32_UNDEF_END, 1)->imm = g->next;
    g->undefined = 1;
}
static inline void sg_undefined(SgCtx* g) { sgx_a32_trap(g, SNX_ARM_UNDEFINED); }
/* sgx_a32_trap where bad holds (llvm.c's require of an exit of the backend's), the block going on otherwise */
static inline void sgx_a32_trap_on(SgCtx* g, SgB bad, unsigned reason) {
    SgX* x = sgx(g);
    if (x->b[bad].kind == SGX_B_CONST && !x->b[bad].v) return;
    SgXIns* i = sgx_a32_on(g, SGX_A32_OUT, bad);
    i->imm = reason + 1;
    i->kv[0] = g->word;
    i->kv[1] = (SnxU32)g->pc | (SnxU64)g->next << 32;
}
/* the instruction's end: the block's where a write of it through the bus hook returned nonzero, R15 the next
   instruction's (llvm.c's yield_check) */
static inline void sgx_a32_yield_check(SgCtx* g) {
    if (!g->yields) return;
    g->yields = 0;
    SgXIns* i = sgx_a32_on(g, SGX_A32_OUT, sg_b_not(g, sg_d_is_zero(g, g->yield)));
    i->imm = 0;
    i->kv[1] = (SnxU32)g->pc | (SnxU64)g->next << 32;
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
/* the block's stop at the instruction (llvm.c's trap of a code the backend does not take): R15 pc, the
   exit, its syndrome */
static inline void sgx_a32_stop(SgCtx* g, unsigned code, SgD syndrome, SnxU32 pc) {
    sgx_a32_commit(g);
    sg_flags_flush(g);
    sgx_a32_pc(g, sgx_const(32, pc));
    sgx_a32_put(g, offsetof(SnxArmCPU, syndrome), 32, syndrome);
    sgx_a32_put(g, offsetof(SnxArmCPU, exit), 32, sgx_const(32, code));
    sgx_a32_machine(g, SGX_A32_JMP, 0)->disp = g->ret;
    g->undefined = 1; /* the instruction's way leaves the block */
}
static inline void sg_AArch32_CallSupervisor(SgCtx* g, SgD imm) { sg_exception(g, SNX_ARM_SVC, imm); }
static inline void sg_AArch32_SoftwareBreakpoint(SgCtx* g, SgD imm) { sg_exception(g, SNX_ARM_BREAKPOINT, imm); }
/* the ARM7TDMI's multiply carry, the runtime library's, from the operands before the result lands
   (llvm.c's multiply) */
static inline void sg_MultiplyCarry(SgCtx* g) {
    SgD args[2] = {sgx_const(32, g->word), sgx_const(32, (SnxU64)g->thumb)};
    sgx_a32_commit(g);
    sgx_a32_call(g, "snx_arm_multiply_carry", args, 2, 0);
}
/* the instruction's trace slot (codegen.h's SnxArmCodegenOptions) gets v */
static inline void sgx_a32_trace(SgCtx* g, SgD v) {
    if (!g->trace) return;
    sgx_a32_commit(g);
    sgx_a32_put(g, g->trace + (((SnxU32)g->pc - g->block_pc) >> (g->thumb ? 1 : 2)) * 4, 32, v);
}
static inline void sg_TraceMultiplier(SgCtx* g, SgD multiplier) { sgx_a32_trace(g, multiplier); }

/* ---- calls ---- */

/* sym(state, args...): every value in a caller-saved register spilled, the arguments in SGX_A32_ARG's; the call
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
        i->fixed[k] = SGX_A32_ARG(x, k);
    }
    if (result) i->def = sgx_vreg(x, x->n - 1, 64);
    for (int k = 0; k < 15; ++k) g->gpr[k] = 0;
    g->cpsr = 0;
    g->origin = -1;
    g->vfp_ok[0] = g->vfp_ok[1] = 0;
}

/* ---- guest memory ---- */

/* the region's min(mem_size, 2^32): an access whose last byte is past it aborts (llvm.c's bounds). The ARM7TDMI's and
   the ARM946E's window runs past 4 GiB by a guard (agb/cpu/memmap.c), where an access the console's bus wraps lands:
   its size as it is */
static inline SgD sgx_a32_limit(SgCtx* g) {
    SgX* x = sgx(g);
    if (!g->limit) {
        SgXIns* i = sgx_append(x, SGX_K_LOAD);
        i->w = 64;
        i->disp = (int)offsetof(SnxArmCPU, mem_size);
        i->def = sgx_vreg(x, x->n - 1, 64);
        SgD size = sgx_val(64, i->def), top = sgx_const(64, 1ull << 32);
        SgD l = sgx_a32_legacy(g) ? size : sg_d_select(g, sgx_d_ult(g, top, size), top, size);
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
   ALIGNMENT exit, past the memory a DATA_ABORT one (not on hosted memory, whose guard faults on the host), both with
   the address as the syndrome (llvm.c's bounds) */
static inline void sgx_a32_bounds(SgCtx* g, SgD address, unsigned bytes, unsigned alignment) {
    SgX* x = sgx(g);
    SgD a = address;
    sgx_reg(x, &a);
    if (alignment > 1)
        sgx_a32_exit(g, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, a, sgx_const(32, alignment - 1)))), SNX_ARM_ALIGNMENT,
                     a, 0);
    if (sgx_a32_hosted(g)) return;
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
/* ---- checked memory (checked.h): the runtime library's calls, a transfer's bytes in the state's words ---- */

/* the state's word k of a checked transfer, as a number of bits */
static inline SgD sgx_a32_word(SgCtx* g, unsigned at, unsigned bits) {
    SgX* x = sgx(g);
    if (at % 4) sg_fail("a checked transfer's byte past a word's first", at, bits);
    SgXIns* i = sgx_append(x, SGX_K_LOAD);
    i->w = 32;
    i->disp = (int)(offsetof(SnxArmCheckedCPU, words) + at);
    i->def = sgx_vreg(x, x->n - 1, 32);
    return sgx_val(bits, i->def);
}
/* the runtime library's sym(state, a, b, bytes), R15 the instruction's first: the callback may read it */
static inline SgD sgx_a32_checked_call(SgCtx* g, const char* sym, SgD a, SgD b, unsigned bytes) {
    SgX* x = sgx(g);
    SgD args[3] = {sg_d_zext(g, a, 64), sg_d_zext(g, b, 64), sgx_const(64, bytes)};
    for (int k = 0; k < 3; ++k) sgx_reg(x, &args[k]);
    sgx_a32_pc(g, sgx_const(32, (SnxU32)g->pc));
    sgx_a32_call(g, sym, args, 3, 1);
    return sg_d_slice(g, sgx_val(64, x->ins[x->n - 1].def), 31, 0);
}
/* one transfer through the runtime's bus, a fault a DATA_ABORT exit with its address (llvm.c's bus_access) */
static inline void sgx_a32_checked_access(SgCtx* g, unsigned operation, SgD address, unsigned bytes) {
    sgx_reg(sgx(g), &address);
    SgD status = sgx_a32_checked_call(g, "snx_arm_checked_access", sgx_const(32, operation), address, bytes);
    sgx_a32_exit(g, sg_b_not(g, sg_d_is_zero(g, status)), SNX_ARM_DATA_ABORT, address, 0);
}
/* the instruction at g->pc fetched through the bus before it runs (llvm.c's instruction_fetch): word as the block has
   it, of bytes */
static inline void sgx_a32_fetch(SgCtx* g, SnxU32 word, unsigned bytes) {
    if (!sgx_a32_checked(g)) return;
    SgD pc = sgx_const(32, (SnxU32)g->pc);
    SgD status = sgx_a32_checked_call(g, "snx_arm_checked_fetch", pc, sgx_const(32, word), bytes);
    sgx_reg(sgx(g), &status);
    sgx_a32_exit(g, sg_d_eq(g, status, sgx_const(32, SNX_ARM_PREFETCH_ABORT)), SNX_ARM_PREFETCH_ABORT, pc, 0);
    sgx_a32_exit(g, sg_d_eq(g, status, sgx_const(32, SNX_ARM_CODE_CHANGED)), SNX_ARM_CODE_CHANGED, pc, 0);
}

/* ---- the ARM7TDMI's and the ARM946E's bus: the hook's regions, and the access that leaves the hot code for it ---- */

/* where the hook may take an access at address: the regions' top bytes, and everything past the console's 28 bits; on
   the ARM946E its registers' window alone (llvm.c's io_route, but for the hook's own presence, the
   runtime library's) */
static inline SgB sgx_a32_route(SgCtx* g, SgD address, int store) {
    sg_flags_flush(g); /* the condition made here stays the flags of the jump on it */
    SgD top = sg_d_lsr(g, address, 24);
    if (sgx_a32_arm9(g)) return sg_d_eq(g, top, sgx_const(32, 4));
    SnxU32 regions = store ? SNX_ARM_IO_STORE_REGIONS : SNX_ARM_IO_REGIONS;
    SgD bit = sg_d_and(g, sg_d_shift_dyn(g, sgx_const(32, regions), sg_d_and(g, top, sgx_const(32, 31)), 1),
                       sgx_const(32, 1));
    return sg_b_not(g, sg_d_is_zero(g, sg_d_or(g, bit, sg_d_lsr(g, address, 28))));
}
/* the access of bytes at [CPU.mem + address], or where out holds the runtime library's with hook (the address the hook
   gets) and size: what it gives, a load's value zero extended to 32 bits, a store's of value 0 or what the hook
   returned */
static inline SgD sgx_a32_bus(SgCtx* g, SgB out, SgD hook, SgD address, int bytes, unsigned size, const SgD* value) {
    SgX* x = sgx(g);
    const SgXSel* s;
    if (x->b[out].kind == SGX_B_CONST && !x->b[out].v) { /* an address known to be none of the hook's */
        if (!value) return sg_d_zext(g, sgx_a32_load(g, address, bytes, 0), 32);
        sgx_a32_store(g, address, bytes, 0, bytes < 4 ? sg_d_slice(g, *value, 8 * bytes - 1, 0) : *value);
        return sgx_const(32, 0);
    }
    if (value)
        s = bytes == 1   ? sgx_a32_mem_form(sgx_sel_mov_8, sgx_sel_mov_8_n, 1)
            : bytes == 2 ? sgx_a32_mem_form(sgx_sel_mov_16, sgx_sel_mov_16_n, 1)
                         : sgx_a32_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 1);
    else
        s = bytes == 1   ? sgx_a32_mem_form(sgx_sel_zext8_32, sgx_sel_zext8_32_n, 0)
            : bytes == 2 ? sgx_a32_mem_form(sgx_sel_zext16_32, sgx_sel_zext16_32_n, 0)
                         : sgx_a32_mem_form(sgx_sel_mov_32, sgx_sel_mov_32_n, 0);
    /* an argument of 64 bits each, so no ABI's rule for a narrower one matters */
    SgD h = sgx_zext(x, hook, 64), at = sgx_zext(x, address, 64),
        v = value ? sg_d_zext(g, *value, 64) : sgx_const(64, 0);
    int in[4] = {0};
    in[0] = sgx_reg(x, &h);
    in[1] = sgx_a32_mem(g);
    in[2] = sgx_reg(x, &at);
    if (value) in[3] = sgx_reg(x, &v);
    int from = 0, cc = sgx_bool_cc(x, out, &from);
    SgXIns* i = sgx_append(x, SGX_K_MACHINE);
    i->op = SGX_A32_BUS;
    i->cc = (unsigned char)cc;
    i->flags_from = from;
    i->sel = s;
    i->sym = value ? "snx_arm_io_write" : "snx_arm_io_read";
    i->imm = size;
    i->kv[0] = sgx_a32_arm9(g) ? ~0u : (SnxU32)g->pc | (SnxU32)g->thumb; /* where the open bus is read from */
    i->kv[0] |= (SnxU64)(SnxU32)g->pc << 32;
    i->kv[1] = !value;
    for (int k = 0; k < 4; ++k) i->in[k] = in[k];
    i->fixed[0] = SGX_A32_ARG(x, 0), i->fixed[1] = SGX_A32_BUS_MEM(x), i->fixed[2] = SGX_A32_ARG(x, 1);
    i->fixed[3] = SGX_A32_ARG(x, 2);
    i->side = 1;
    i->def = sgx_vreg(x, x->n - 1, 64);
    SgD r = sg_d_slice(g, sgx_val(64, i->def), 31, 0);
    /* the hook may change any of the state: the region's registers and CPSR are read again after it */
    x->flags_ins = -1;
    for (int k = 0; k < 15; ++k) g->gpr[k] = 0;
    g->cpsr = g->limit = g->mem = 0;
    g->origin = -1;
    return r;
}
/* a single transfer's load of bytes (1, 2, 4) at address: the aligned unit, a misaligned one rotated through 32 bits,
   an ARMv4T LDRSH at an odd address the byte there (llvm.c's readmem on the two) */
static inline SgD sgx_a32_bus_read(SgCtx* g, SgD address, int bytes, int sign) {
    SgX* x = sgx(g);
    int odd = sign && bytes == 2 && sgx_a32_arm7(g);
    sgx_reg(x, &address);
    SgD aligned = bytes > 1 ? sg_d_and(g, address, sgx_const(32, ~(SnxU32)(bytes - 1))) : address;
    unsigned size = (unsigned)bytes | (odd ? SNX_ARM_IO_SIGNED : 0) | (sgx_a32_arm9(g) ? SNX_ARM_IO_ARM9 : 0);
    sgx_reg(x, &aligned);
    SgB out = sgx_a32_route(g, address, 0);
    if (odd) /* the byte's, and its sign, are the runtime library's */
        out = sg_b_or(g, out, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, address, sgx_const(32, 1)))));
    SgD v = sgx_a32_bus(g, out, sgx_a32_arm7(g) ? address : aligned, aligned, bytes, size, NULL);
    if (odd) return sg_d_sext(g, sg_d_slice(g, v, 15, 0), 32);
    /* the ARM946E-S rotates a word alone: its halfword loads read the aligned unit as it is */
    if (bytes > 1 && (sgx_a32_arm7(g) || bytes == 4))
        v = sg_d_shift_dyn(g, v, sg_d_lsl(g, sg_d_and(g, address, sgx_const(32, (SnxU32)(bytes - 1))), 3), 3);
    return sign && bytes < 4 ? sg_d_sext(g, sg_d_slice(g, v, 8 * bytes - 1, 0), 32) : v;
}
/* a multiple transfer's word at address: below the save memory its word's, on the ARM946E everywhere (llvm.c's
   list_address) */
static inline SgD sgx_a32_list_address(SgCtx* g, SgD address) {
    SgD word = sg_d_and(g, address, sgx_const(32, ~3u));
    if (sgx_a32_arm9(g)) return word;
    return sg_d_select(g, sgx_d_ult(g, address, sgx_const(32, SNX_ARM_SAVE_MEMORY)), word, address);
}
static inline SgD sgx_a32_list_read(SgCtx* g, SgD address) {
    SgD at = sgx_a32_list_address(g, address);
    sgx_reg(sgx(g), &at);
    SgB out = sgx_a32_route(g, at, 0);
    return sgx_a32_bus(g, out, at, at, 4, 4u | SNX_ARM_IO_LIST | (sgx_a32_arm9(g) ? SNX_ARM_IO_ARM9 : 0), NULL);
}
/* a store of bytes through the bus (list: a multiple transfer's word): what the hook returned joins the instruction's
   yields (llvm.c's writemem, writemem_list) */
static inline void sgx_a32_bus_write(SgCtx* g, SgD address, int bytes, SgD value, int list) {
    SgX* x = sgx(g);
    SgD at = list        ? sgx_a32_list_address(g, address)
             : bytes > 1 ? sg_d_and(g, address, sgx_const(32, ~(SnxU32)(bytes - 1)))
                         : address;
    unsigned size = 0x100u | (unsigned)bytes | (list ? SNX_ARM_IO_LIST : 0) | (sgx_a32_arm9(g) ? SNX_ARM_IO_ARM9 : 0);
    value.w = 32; /* the register, whose low bits are stored: the hook sees it whole */
    sgx_reg(x, &value);
    sgx_reg(x, &at);
    if (!list) {
        sgx_reg(x, &address);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_valid), 32, sgx_const(32, 0));
    }
    SgD hook = list ? at : address; /* the raw address: the save memory's 8 bit bus picks the lane from it */
    SgB out = sgx_a32_route(g, hook, 1);
    SgD r = sgx_a32_bus(g, out, hook, at, bytes, size, &value);
    g->yield = g->yields ? sg_d_or(g, g->yield, r) : r;
    g->yields = 1;
}

/* ---- an instruction's transfer, of any profile and mode: begun (its range and alignment checked, a checked load's
   bytes read), moved, ended (a checked store's bytes sent) ---- */

/* llvm.c's transfer_begin: bytes at address, of one instruction (multi: more than one register's).
   alignment 1 for none */
static inline void sgx_a32_transfer(SgCtx* g, SgD address, unsigned bytes, unsigned alignment, int store, int multi) {
    SgX* x = sgx(g);
    sgx_reg(x, &address);
    if (!sgx_a32_checked(g)) {
        sgx_a32_bounds(g, address, bytes, alignment);
        sgx_a32_trace(g, address);
        return;
    }
    if (alignment > 1)
        sgx_a32_exit(g, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, address, sgx_const(32, alignment - 1)))),
                     SNX_ARM_ALIGNMENT, address, 0);
    /* past the top of the 4 GiB: address + bytes > 2^32 */
    sgx_a32_exit(g, sgx_d_ult(g, sgx_const(64, (1ull << 32) - bytes), sgx_zext(x, address, 64)), SNX_ARM_DATA_ABORT,
                 address, 0);
    g->transfer = address;
    g->transfer_bytes = bytes;
    g->transfer_op = (store ? SNX_ARM_ACCESS_WRITE : SNX_ARM_ACCESS_READ) | (multi ? SNX_ARM_ACCESS_MULTI : 0);
    if (!store) sgx_a32_checked_access(g, g->transfer_op, address, bytes);
}
/* a single load of bytes (1, 2 or 4) of the transfer, disp bytes into it, to 32 bits */
static inline SgD sgx_a32_read(SgCtx* g, SgD address, int bytes, int disp, int sign) {
    SgD v;
    if (sgx_a32_legacy(g))
        return sgx_a32_bus_read(g, disp ? sg_d_add(g, address, sgx_const(32, (SnxU32)disp)) : address, bytes, sign);
    v = sgx_a32_checked(g) ? sgx_a32_word(g, (unsigned)disp, (unsigned)(8 * bytes))
                           : sgx_a32_load(g, address, bytes, disp);
    if (bytes == 4) return v;
    return sign ? sg_d_sext(g, v, 32) : sg_d_zext(g, v, 32);
}
/* a multiple transfer's word, disp bytes into it */
static inline SgD sgx_a32_read_list(SgCtx* g, SgD address, int disp) {
    if (sgx_a32_legacy(g))
        return sgx_a32_list_read(g, disp ? sg_d_add(g, address, sgx_const(32, (SnxU32)disp)) : address);
    return sgx_a32_checked(g) ? sgx_a32_word(g, (unsigned)disp, 32) : sgx_a32_load(g, address, 4, disp);
}
/* the low bytes of value (32 bits) stored, disp bytes into the transfer; the exclusive monitor's check is the caller's
   (sgx_a32_written), but the two profiles' with the hook, whose single stores clear it and whose
   multiple ones do not */
static inline void sgx_a32_write(SgCtx* g, SgD address, int bytes, int disp, SgD value, int list) {
    if (sgx_a32_legacy(g)) {
        sgx_a32_bus_write(g, disp ? sg_d_add(g, address, sgx_const(32, (SnxU32)disp)) : address, bytes, value, list);
        return;
    }
    if (sgx_a32_checked(g)) {
        if (disp % 4) sg_fail("a checked transfer's byte past a word's first", disp, bytes);
        sgx_a32_put(g, offsetof(SnxArmCheckedCPU, words) + (size_t)disp, 32, value);
        return;
    }
    sgx_a32_store(g, address, bytes, disp, bytes < 4 ? sg_d_slice(g, value, 8 * bytes - 1, 0) : value);
}
/* a store's end: the monitor's one check for its bytes (their range overlaps the reservation exactly where one of its
   words does), then a checked one's bytes sent */
static inline void sgx_a32_written(SgCtx* g, SgD address, unsigned bytes) {
    if (!sgx_a32_legacy(g)) sgx_a32_monitor(g, address, bytes);
    if (sgx_a32_checked(g)) sgx_a32_checked_access(g, g->transfer_op, g->transfer, g->transfer_bytes);
}

/* a multiple transfer's range, checked before its first access (arm.sail's GuestTransfer): word aligned, but on the
   ARM7TDMI and the ARM946E, whose buses take an unaligned base */
static inline void sg_GuestTransfer(SgCtx* g, SgD address, long long bytes, int write) {
    sgx_a32_commit(g);
    if (g->group) sg_fail("a second multiple transfer in one instruction", (long long)g->word, 0);
    sgx_reg(sgx(g), &address);
    sgx_a32_transfer(g, address, (unsigned)bytes, sgx_a32_legacy(g) ? 1 : 4, write, 1);
    g->group = 1;
    g->group_write = write;
    g->group_start = address;
    g->group_at = 0;
    g->sent = 0;
}
/* flat and hosted memory take a transfer of no bytes: its address checked, nothing moved; checked memory's bus has no
   transaction of none */
static inline int sg_GuestTransferEmpty(SgCtx* g) { return !sgx_a32_checked(g); }
/* a multiple store's bytes go out before anything after them is written (checked memory's one transaction) */
static inline void sg_transfer_commit(SgCtx* g) {
    if (!g->group || !g->group_write || g->sent || !sgx_a32_checked(g)) return;
    g->sent = 1;
    sgx_a32_checked_access(g, g->transfer_op, g->transfer, g->transfer_bytes);
}
static inline SgD sg_GuestRead(SgCtx* g, SgD address, long long size, int aligned) {
    (void)aligned;
    sgx_a32_commit(g);
    if (size != 1 && size != 2 && size != 4) sg_fail("a guest read of this size", size, 0);
    if (g->group) {
        unsigned at = g->group_at;
        g->group_at += (unsigned)size;
        if (sgx_a32_legacy(g)) return sgx_a32_list_read(g, address);
        return sgx_a32_checked(g) ? sgx_a32_word(g, at, 32) : sgx_a32_load(g, address, (int)size, 0);
    }
    sgx_a32_transfer(g, address, (unsigned)size, 1, 0, 0);
    if (sgx_a32_legacy(g) || sgx_a32_checked(g))
        return sg_d_slice(g, sgx_a32_read(g, address, (int)size, 0, 0), 8 * size - 1, 0);
    return sgx_a32_load(g, address, (int)size, 0);
}
/* an ARMv4T halfword load's 32 bits as its bus gives them (arm.sail's GuestReadHalfwordV4) */
static inline SgD sg_GuestReadHalfwordV4(SgCtx* g, SgD address, int sign) {
    sgx_a32_commit(g);
    if (g->group) sg_fail("a halfword load in a multiple transfer", (long long)g->word, 0);
    sgx_reg(sgx(g), &address);
    sgx_a32_transfer(g, address, 2, 1, 0, 0);
    return sgx_a32_read(g, address, 2, 0, sign);
}
static inline void sg_GuestWrite(SgCtx* g, SgD address, long long size, int aligned, SgD value) {
    (void)aligned;
    sgx_a32_commit(g);
    if (size != 1 && size != 2 && size != 4) sg_fail("a guest write of this size", size, 0);
    sgx_reg(sgx(g), &address);
    if (g->group) {
        unsigned at = g->group_at;
        g->group_at += (unsigned)size;
        if (sgx_a32_legacy(g))
            sgx_a32_bus_write(g, address, (int)size, value, 1);
        else if (sgx_a32_checked(g)) {
            /* a word of the transfer, sent as its last one is in (sg_transfer_commit) */
            sgx_a32_put(g, offsetof(SnxArmCheckedCPU, words) + at, 32, value);
            sgx_a32_monitor(g, address, (unsigned)size);
        } else {
            sgx_a32_store(g, address, (int)size, 0, value);
            sgx_a32_monitor(g, address, (unsigned)size);
        }
        return;
    }
    sgx_a32_transfer(g, address, (unsigned)size, 1, 1, 0);
    value.w = 32; /* the low bytes of the register they are in */
    sgx_a32_write(g, address, (int)size, 0, value, 0);
    sgx_a32_written(g, address, (unsigned)size);
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

/* the dispatchers sailgen generates after this file (a32.roots' decoders) */
static int sg_decode_t16(SgCtx* g);
static int sg_decode_a32(SgCtx* g);

/* the instruction's end: what it wrote stored where it ran under a condition (its skip then closes the region,
   x64_arm.c's), else kept for the next; a multiple load's registers in their order */
static inline void sgx_a32_finish(SgCtx* g) {
    sg_registers_flush(g);
    sg_transfer_commit(g);
    g->group = 0;
    g->when = 0;
    if (g->skip >= 0) sg_flags_flush(g);
}
