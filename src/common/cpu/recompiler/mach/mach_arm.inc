/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native back ends' AArch32 blocks, what any machine's shares: what cpu/arm/llvm.c's arm_block makes of an A32
   block, for any profile and memory mode of its options (cpu/arm/codegen.h), without LLVM. Included by a machine's
   block compiler (x64/x64_arm.c,
   arm64/arm64_arm.c), and followed by the machine's glue, which it declares: the block's frame, prologue and entry
   checks, its return and its chaining to the next block, the entry checks' exit function, an entry stub for another
   back end's block, and the VFP's fp_call. The machine names its emitters' file (MG_ARM_INC: a32_<machine>.inc), its
   compiler's type and functions (MG_ARM, MG_ARM_NEXT, MG_ARM_API(name)), whether it is this host's (MG_ARM_AVAILABLE),
   whether it makes code here (MG_ARM_MAKES: arm64's anywhere, for its tests) and for what features (MG_ARM_FEATURES),
   its object's machine (MG_ARM_EM) and call relocation (MG_ARM_CALL_RELOC), whether its loads need a barrier to be
   acquires (MG_ARM_LOAD_FENCE) and whether the VFP's arithmetic has a hot path of its own (MG_ARM_FP_HOT). The glue's
   prologue and entry exit check what SgCtx.thumb says: an A32 block's state, or a Thumb one's.

   Each word is sailgen's emitter of the AArch32 Sail where its clauses take it (the data processing and branches),
   else one written here over the same SSA region, as llvm.c's emitters are written over LLVM's builder (the loads
   and stores, the multiplies, the exclusives...). The region is assembled where a condition's skip ends it, where it
   grows long, and at the block's end; the guest's registers are in the state at each region's end, as each write
   stores them. Its entry checks are llvm.c's block_begin's (exit and syndrome cleared, R15 the block's address,
   CPSR.T and E clear; a hosted block has none), and it ends where arm_block ends: after an instruction that branches,
   one that leaves for the
   backend, or the last word. Where the block goes on to a known address in A32 or Thumb state and the caller names
   that block a successor, the code jumps to it as chain_block's tail call does (reading the thread's poll first where
   it is at or before the block's start); anywhere else it returns, with R15 the next instruction's */

#ifndef MG_ARM_ELF_FLAGS
#define MG_ARM_ELF_FLAGS 0 /* the objects' e_flags (mach_codegen.inc's MG_ELF_FLAGS) */
#endif
#ifndef MG_ARM_EXIT_LAST
#define MG_ARM_EXIT_LAST(c) 0 /* the failed entry check's exit is the block's last code, past what has its frame up */
#endif
#ifndef MG_ARM_ABI
#define MG_ARM_ABI 0u /* the features that are no processor's, which a test's limit leaves (x86-64's SGX_F_WIN64) */
#endif

/* the thread's: where a word the emitters can't make (bits.h's sg_fail) refuses the block, which LLVM makes, and why,
   for the caller */
static _Thread_local struct {
    jmp_buf* fail;
    const char* why;
} t_mach_arm;
#define SG_FAIL_JUMP             t_mach_arm.fail
#define SG_FAIL_NOTE(what, a, b) (t_mach_arm.why = (what), (void)(a), (void)(b))

#include MG_ARM_INC

/* a relocation of the block being made, at its code's offset */
typedef struct {
    size_t at;
    char sym[24];
    unsigned type;
    long long addend;
} Rel;

/* a block made, kept until the object: its name and address, its code in text, its relocations in rels */
typedef struct {
    SnxU32 pc;
    char name[24];
    size_t at, size, rel0, nrel;
    size_t own; /* its entry past the checks (X and its name), from its start; (size_t)-1 for none (entry's) */
} Made;

struct MG_ARM {
    SgCtx g;
    SnxArmCodegenOptions options; /* the next blocks' */
    SgXAsm a;
    SgXBuf b;
    Rel* rel; /* the block being made's */
    int nrel, cap_rel;
    /* the blocks made since the last object, which it writes in their addresses' order */
    unsigned char* text;
    size_t ntext, cap_text;
    Rel* rels;
    size_t nrels, cap_rels;
    Made* made;
    size_t nmade, cap_made;
    unsigned features;
    int pool;
    const char* why;
    int big;    /* the block being made has 32-bit frame sizes (frame) */
    size_t own; /* its entry past the checks, from its start (prologue) */
};

int MG_ARM_API(available)(void) { return MG_ARM_AVAILABLE(); }

MG_ARM* MG_ARM_API(new)(void) {
    if (!MG_ARM_MAKES()) return NULL;
    MG_ARM* c = (MG_ARM*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->b.cap = 1u << 16;
    c->b.p = (unsigned char*)malloc(c->b.cap);
    if (!c->b.p) {
        MG_ARM_API(free)(c);
        return NULL;
    }
    c->features = MG_ARM_FEATURES();
    c->options.abi = SNX_ARM_CODEGEN_OPTIONS_ABI;
    return c;
}
int MG_ARM_API(options)(MG_ARM* c, const SnxArmCodegenOptions* options) {
    if (!snx_arm_codegen_options_valid(options)) return -1;
    c->options = options ? *options : (SnxArmCodegenOptions){.abi = SNX_ARM_CODEGEN_OPTIONS_ABI};
    return 0;
}
void MG_ARM_API(free)(MG_ARM* c) {
    if (!c) return;
    free(c->b.p);
    free(c->rel);
    free(c->text), free(c->rels), free(c->made);
    free(c->g.label_at), free(c->g.fix), free(c->g.exits), free(c->g.monitors), free(c->g.outs), free(c->g.buses);
    free(c->a.cold.p), free(c->a.pool), free(c->a.rips), free(c->a.cjumps), free(c->a.ccalls);
    free(c);
}
void MG_ARM_API(limit)(MG_ARM* c, int pool, unsigned features) {
    c->pool = pool;
    c->features = MG_ARM_FEATURES() & (features | MG_ARM_ABI);
}
const char* MG_ARM_API(why)(const MG_ARM* c) { return c->why ? c->why : ""; }

/* a call of a helper (sgx_a32_call's), the chain's jumps and the poll's load: relocations of the block */
static void add_rel(MG_ARM* c, size_t at, const char* sym, unsigned type, long long addend) {
    size_t n = strlen(sym);
    if (n >= sizeof c->rel[0].sym) sg_fail("a symbol's name too long", (long long)n, 0);
    c->rel = (Rel*)sgx_a32_grow(c->rel, &c->cap_rel, c->nrel + 1, sizeof *c->rel);
    c->rel[c->nrel].at = at;
    memcpy(c->rel[c->nrel].sym, sym, n + 1);
    c->rel[c->nrel].type = type;
    c->rel[c->nrel++].addend = addend;
}
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend) {
    MG_ARM* c = (MG_ARM*)((char*)machine - offsetof(MG_ARM, g));
    add_rel(c, at, sym, MG_ARM_CALL_RELOC, addend);
}

/* the machine's instructions besides its AArch32 part's own (x64_a32.h, arm64_a32.h, risc_a32.h): SGX_A32_OUT, a jump
   to a way out of the block, and SGX_A32_BUS, an access of the ARM7TDMI or the ARM946E the bus hook may take. Their
   cold ways are written after the block's code (cold_ways) */
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r) {
    SgXIns* i = &a->x->ins[at];
    SgCtx* g = (SgCtx*)a->machine;
    SgXBuf* b = a->b;
    if (i->op == SGX_A32_OUT) {
        SgA32Out o = {.label = sgx_a32_label(g),
                      .undefined = i->imm != 0,
                      .reason = (unsigned)(i->imm ? i->imm - 1 : 0),
                      .word = (SnxU32)i->kv[0],
                      .pc = (SnxU32)i->kv[1],
                      .next = (SnxU32)(i->kv[1] >> 32)};
        g->outs = (SgA32Out*)sgx_a32_grow(g->outs, &g->cap_outs, g->nouts + 1, sizeof *g->outs);
        g->outs[g->nouts++] = o;
        sgx_a32_jcc(g, b, i->cc, o.label);
        return;
    }
    if (i->op != SGX_A32_BUS) sg_fail("a machine instruction of no kind", i->op, at);
    SgA32Bus m = {.label = sgx_a32_label(g),
                  .back = sgx_a32_label(g),
                  .sym = i->sym,
                  .size = (SnxU32)i->imm,
                  .key = (SnxU32)i->kv[0],
                  .pc = (SnxU32)(i->kv[0] >> 32),
                  .load = i->kv[1] != 0};
    int kept = 0;
    /* a vector read after it goes to its slot now: every vector register is the call's to change */
    for (int k = 0; k < SGX_NXREG; ++k) sgx_xspill(a, k, at);
    m.slot = a->slots;
    for (int k = 0; k < SGX_NREG; ++k)
        if ((SGX_CALLER_SAVED >> k & 1) && k != SGX_RET && a->owner[k] > 0 && a->x->v[a->owner[k]].last > at) {
            m.live |= 1u << k;
            ++kept;
        }
    if (a->slots + kept > a->slots_max) a->slots_max = a->slots + kept;
    g->buses = (SgA32Bus*)sgx_a32_grow(g->buses, &g->cap_buses, g->nbuses + 1, sizeof *g->buses);
    g->buses[g->nbuses++] = m;
    sgx_a32_jcc(g, b, i->cc, m.label);
    /* the access itself, through the form the selection gave it */
    unsigned char o[4] = {0};
    const SgXSel* s = i->sel;
    if (m.load) {
        o[(int)s->out] = SGX_RET;
        o[(int)s->in[0]] = (unsigned char)SGX_MEMR(r[1]);
    } else {
        o[(int)s->in[0]] = r[3];
        o[(int)s->out] = (unsigned char)SGX_MEMR(r[1]);
    }
    b->index = r[2];
    b->disp = 0;
    s->enc(b, o, 0, 0);
    b->index = -1;
    if (!m.load) sgx_a32_movi(b, SGX_RET, 0);
    sgx_a32_bind(g, b, m.back);
}
/* the ways out of the block and the bus's calls, after the block's code */
static void cold_ways(MG_ARM* c) {
    SgCtx* g = &c->g;
    SgXBuf* b = &c->b;
    for (int k = 0; k < g->nouts; ++k) {
        const SgA32Out* o = &g->outs[k];
        sgx_a32_bind(g, b, o->label);
        if (o->undefined) { /* llvm.c's trap: the backend's to run or stop at */
            sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, o->pc);
            sgx_a32_movi(b, SGX_A32_ARG(&g->x, 0), o->word);
            sgx_a32_movi(b, SGX_A32_ARG(&g->x, 1), o->reason);
            sgx_a32_call_out(&c->a, b, "snx_arm_undefined");
            sgx_a32_leave(g, b);
        }
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, o->next);
        sgx_a32_jmp(g, b, g->ret);
    }
    for (int k = 0; k < g->nbuses; ++k) {
        const SgA32Bus* m = &g->buses[k];
        int slot = m->slot;
        sgx_a32_bind(g, b, m->label);
        for (int j = 0; j < SGX_NREG; ++j)
            if (m->live >> j & 1) sgx_mov_mem(&c->a, 0, 64, j, SGX_FRAME, 8 * slot++);
        sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, m->pc); /* the hook may read it */
        sgx_a32_movi(b, SGX_A32_ARG(&g->x, 1), m->size);
        if (m->load) sgx_a32_movi(b, SGX_A32_ARG(&g->x, 2), m->key);
        sgx_a32_call_out(&c->a, b, m->sym);
        slot = m->slot;
        for (int j = 0; j < SGX_NREG; ++j)
            if (m->live >> j & 1) sgx_mov_mem(&c->a, 1, 64, j, SGX_FRAME, 8 * slot++);
        sgx_a32_jmp(g, b, m->back);
    }
    g->nouts = g->nbuses = 0;
}

/* ---- the machine's glue (defined after this file) ---- */
static void mg_prologue(MG_ARM* c, SnxU32 pc, int* bad);
static void mg_entry_exit(MG_ARM* c, SnxU32 pc, SnxU32 first, int bad);
static void mg_return(MG_ARM* c);
static void mg_go_on(MG_ARM* c, SnxU32 key, SnxU32 block, const SnxU32* next, size_t nnext, int public);
static int mg_patch_frame(MG_ARM* c);
static void mg_entry_exit_fn(SnxXElf* o);

/* ---- regions ---- */

/* the region so far assembled (what its fields wrote stored first), and what the block knows forgotten */
static void flush(MG_ARM* c) {
    SgCtx* g = &c->g;
    sg_flags_flush(g);
    if (g->x.n) sgx_assemble_region(&c->a, &g->x);
    for (int k = 0; k < 15; ++k) g->gpr[k] = 0;
    g->cpsr = 0;
    g->limit = g->mem = 0;
    g->origin = -1;
    g->vfp_ok[0] = g->vfp_ok[1] = 0;
    g->x.flags_ins = -1;
}

/* ---- the instructions the Sail has no clause for: llvm.c's, on the region's SSA ---- */

/* R[r] as an operand: the PC reads as the instruction's address plus 8; in a Thumb block, where a T32 instruction that
   is an A32 one in another encoding runs its emitter, Align(PC, 4) of it plus 4 */
static SgD reg(SgCtx* g, unsigned r) {
    if (r != 15) return sg_GPR_read(g, r);
    return sgx_const(32, g->thumb ? ((SnxU32)g->pc + 4) & ~3u : (SnxU32)g->pc + 8);
}
/* a branch to value with the state from its bit 0 (llvm.c's branch_exchange): CPSR.T from it, R15 the rest */
static void branch_exchange(SgCtx* g, SgD value) {
    sg_PSTATE_write(g, SG_PSTATE_T, 1, sg_d_slice(g, value, 0, 0));
    g->branched = 1;
    g->target = sg_d_and(g, value, sgx_const(32, ~1u));
}
/* a load written to the PC (llvm.c's arm_write_pc). The ARM7TDMI (ARMv4T) does not interwork there: bits 1 and 0 are
   dropped and the state stays ARM; only BX changes the state. Later cores branch with the state from bit 0 */
static void write_pc(SgCtx* g, SgD value) {
    if (!sgx_a32_arm7(g)) {
        branch_exchange(g, value);
        return;
    }
    g->branched = 1;
    g->target = sg_d_and(g, value, sgx_const(32, ~3u));
}
/* a shift by an immediate (llvm.c's shift with immediate set), its carry not taken */
static SgD shift_imm(SgCtx* g, SgD v, unsigned type, unsigned imm) {
    if (!imm && type == 0) return v;
    if (!imm && type == 3) /* RRX */
        return sg_d_or(g, sg_d_lsl(g, sg_d_zext(g, sg_PSTATE_read(g, SG_PSTATE_C), 32), 31), sg_d_lsr(g, v, 1));
    if (!imm) imm = 32;
    switch (type) {
        case 0: return sg_d_lsl(g, v, imm);
        case 1: return imm == 32 ? sgx_const(32, 0) : sg_d_lsr(g, v, imm);
        case 2: return sg_d_asr(g, v, imm == 32 ? 31 : imm);
        default: return sg_d_ror(g, v, imm);
    }
}
/* the instruction's condition, its skip made: what follows runs where it holds */
static void condition(SgCtx* g) {
    sg_when(g, sgx_a32_condition(g, g->word >> 28));
    sgx_a32_commit(g);
}

/* LDR, STR, LDRB, STRB and their T forms, LDRH, STRH, LDRSB, LDRSH, LDRD, STRD (llvm.c's memory) */
static int memory(SgCtx* g, SnxA32Kind kind) {
    SnxU32 w = g->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, op = (w >> 5) & 3, bytes = 4;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23));
    int wb = !pre || !!(w & (1u << 21)), load = !!(w & (1u << 20)), sign = 0, pair = 0;
    condition(g);
    SgD base = reg(g, n), offset;
    if (kind == SNX_A32_SINGLE) {
        bytes = w & (1u << 22) ? 1 : 4;
        offset = w & (1u << 25) ? shift_imm(g, reg(g, w & 15), (w >> 5) & 3, (w >> 7) & 31) : sgx_const(32, w & 4095);
    } else {
        offset = w & (1u << 22) ? sgx_const(32, ((w >> 4) & 0xf0) | (w & 15)) : reg(g, w & 15);
        if (!load && op >= 2) {
            pair = 1;
            load = op == 2;
        } else {
            bytes = op == 2 ? 1 : 2;
            sign = op >= 2;
        }
    }
    SgD adjusted = up ? sg_d_add(g, base, offset) : sg_d_sub(g, base, offset), addr = pre ? adjusted : base;
    /* LDRD and STRD, and a load of the PC, word aligned (llvm.c's: unaligned support on, as ARMv6's SCTLR.U), but on
       the ARM7TDMI, which rotates a misaligned PC, and the ARM946E-S, neither of which faults on a
       misaligned doubleword */
    sgx_reg(sgx(g), &addr);
    sgx_a32_transfer(g, addr, pair ? 8 : bytes, (pair || (load && d == 15)) && !sgx_a32_legacy(g) ? 4 : 1, !load, pair);
    if (load) {
        /* both words of a pair read before a register is written, which the bus hook's code sees */
        SgD value = sgx_a32_read(g, addr, (int)bytes, 0, sign);
        SgD second = pair && sgx_a32_legacy(g) ? sgx_a32_read(g, addr, 4, 4, 0) : value;
        if (d == 15)
            write_pc(g, value);
        else
            sg_GPR_write(g, d, value);
        if (pair) sg_GPR_write(g, d + 1, sgx_a32_legacy(g) ? second : sgx_a32_read(g, addr, 4, 4, 0));
    } else {
        /* PCStoreValue: the address plus 8, the ARM7TDMI's and the ARM946E's plus 12 */
        SgD value = d == 15 && sgx_a32_legacy(g) ? sgx_const(32, (SnxU32)g->pc + 12) : reg(g, d);
        SgD second = pair ? reg(g, d + 1) : value;
        sgx_a32_write(g, addr, (int)bytes, 0, value, 0);
        if (pair) sgx_a32_write(g, addr, 4, 4, second, 0);
        sgx_a32_written(g, addr, pair ? 8 : bytes);
    }
    if (wb) sg_GPR_write(g, n, adjusted);
    return d == 15 && load;
}

/* LDM, STM and their stack forms (llvm.c's multiple) */
static int multiple(SgCtx* g) {
    SnxU32 w = g->word, list = w & 0xFFFF;
    unsigned n = (w >> 16) & 15, count = 0, index = 0;
    int pre = !!(w & (1u << 24)), up = !!(w & (1u << 23)), load = !!(w & (1u << 20));
    SgD values[16];
    condition(g);
    for (unsigned i = 0; i < 16; ++i) count += (list >> i) & 1;
    SgD base = reg(g, n);
    SgD adjusted = up ? sg_d_add(g, base, sgx_const(32, count * 4)) : sg_d_sub(g, base, sgx_const(32, count * 4));
    SgD start = up ? sg_d_add(g, base, sgx_const(32, pre ? 4 : 0)) : sg_d_add(g, adjusted, sgx_const(32, pre ? 0 : 4));
    sgx_reg(sgx(g), &start);
    /* the ARM7TDMI and the ARM946E-S transfer at an unaligned base instead of faulting */
    sgx_a32_transfer(g, start, count * 4, sgx_a32_legacy(g) ? 1 : 4, !load, 1);
    /* every source read before any update, the base and the PC among them */
    for (unsigned i = 0; i < 16; ++i)
        if (list >> i & 1) {
            /* the ARM7TDMI stores the written back base where the base is not the first in the list */
            int written_back = sgx_a32_arm7(g) && i == n && (w & (1u << 21)) && (list & ((1u << i) - 1));
            values[i] = load                           ? sgx_a32_read_list(g, start, (int)(index * 4))
                        : i == 15 && sgx_a32_legacy(g) ? sgx_const(32, (SnxU32)g->pc + 12)
                        : written_back                 ? adjusted
                                                       : reg(g, i);
            ++index;
        }
    if (load && (list >> 15 & 1)) write_pc(g, values[15]);
    index = 0;
    for (unsigned i = 0; i < 16; ++i)
        if (list >> i & 1) {
            if (load) {
                if (i != 15) sg_GPR_write(g, i, values[i]);
            } else
                sgx_a32_write(g, start, 4, (int)(index * 4), values[i], 1);
            ++index;
        }
    if (!load) sgx_a32_written(g, start, count * 4);
    /* writeback after the stores (STM stored the old base), but not over a loaded base. The ARM946E-S (ARMv5) writes
       back where the base is the list's only register or not its last */
    int arm9_writes = sgx_a32_arm9(g) && (!(list & ~(1u << n)) || (list & ~((2u << n) - 1)));
    if ((w & (1u << 21)) && (!(load && (list >> n & 1)) || arm9_writes)) sg_GPR_write(g, n, adjusted);
    return load && (list >> 15 & 1);
}

/* N and Z of a result (llvm.c's nz without a carry or an overflow), w its width */
static void nz(SgCtx* g, SgD r, unsigned w) {
    sg_PSTATE_write(g, SG_PSTATE_N, 1, sg_d_slice(g, r, w - 1, w - 1));
    sg_PSTATE_write(g, SG_PSTATE_Z, 1, sg_d_is_zero_bit(g, r));
}

/* MUL, MLA, MLS, UMULL, UMLAL, SMULL, SMLAL, UMAAL (llvm.c's multiply) */
static void multiply(SgCtx* g, SnxA32Kind kind) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15;
    condition(g);
    if (g->trace) sg_TraceMultiplier(g, reg(g, (w >> 8) & 15)); /* whose bytes time it */
    /* the ARM7TDMI's C: the runtime library reads the operands before the results land (snx_arm_multiply_carry) */
    if (sgx_a32_arm7(g) && (w & (1u << 20)) && (kind == SNX_A32_MUL || kind == SNX_A32_MULL)) sg_MultiplyCarry(g);
    SgD a = reg(g, w & 15), b = reg(g, (w >> 8) & 15);
    if (kind == SNX_A32_MULL) {
        int umaal = (w & 0x00e00000) == 0x00400000, sign = !umaal && (w & (1u << 22));
        a = sign ? sg_d_sext(g, a, 64) : sg_d_zext(g, a, 64);
        b = sign ? sg_d_sext(g, b, 64) : sg_d_zext(g, b, 64);
        SgD r = SGX_OP(x, mul, 64, a, b);
        if (umaal) /* Rn * Rm + RdHi + RdLo, unsigned: never past 64 bits */
            r = sg_d_add(g, r, sg_d_add(g, sg_d_zext(g, reg(g, d), 64), sg_d_zext(g, reg(g, n), 64)));
        else if (w & (1u << 21))
            r = sg_d_add(g, r, sg_d_concat(g, reg(g, n), reg(g, d)));
        sg_GPR_write(g, d, sg_d_slice(g, r, 31, 0));
        sg_GPR_write(g, n, sg_d_slice(g, r, 63, 32));
        if (!umaal && (w & (1u << 20))) nz(g, r, 64);
        return;
    }
    SgD r = SGX_OP(x, mul, 32, a, b);
    if (kind == SNX_A32_MLS)
        r = sg_d_sub(g, reg(g, d), r);
    else if (w & (1u << 21))
        r = sg_d_add(g, r, reg(g, d));
    if (w & (1u << 20)) nz(g, r, 32);
    sg_GPR_write(g, n, r);
}

/* SDIV, UDIV: Rd 19:16, Rm 11:8, Rn 3:0; by 0 they give 0 (llvm.c's miscellaneous) */
static void divide(SgCtx* g) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    int u = !!(w & (1u << 21));
    condition(g);
    /* 64 bits: the most negative over -1 needs no care there, and its quotient's low word is the Arm ARM's */
    SgD n = u ? sg_d_zext(g, reg(g, w & 15), 64) : sg_d_sext(g, reg(g, w & 15), 64);
    SgD m = u ? sg_d_zext(g, reg(g, (w >> 8) & 15), 64) : sg_d_sext(g, reg(g, (w >> 8) & 15), 64);
#if SGX_DIV_ARM
    /* the machine's division is Arm's own: 0 for a divisor of 0 */
    SgD q = u ? SGX_OP(x, udiv, 64, n, m) : SGX_OP(x, sdiv, 64, n, m);
    sg_GPR_write(g, (w >> 16) & 15, sg_d_slice(g, q, 31, 0));
#else
    SgB zero = sg_d_is_zero(g, m);
    m = sg_d_select(g, zero, sgx_const(64, 1), m);
#if SGX_DIV_2OP
    /* the divisor 1 where it is 0 (the result 0 below); the most negative over -1 needs no care at 64 bits */
    SgD q = u ? SGX_OP(x, udiv, 64, n, m) : SGX_OP(x, sdiv, 64, n, m);
#else
    SgD q = u ? SGX_OP(x, udiv, 64, n, m) : SGX_OP(x, sdiv, 64, n, sg_d_asr(g, n, 63), m);
#endif
    sg_GPR_write(g, (w >> 16) & 15, sg_d_select(g, zero, sgx_const(32, 0), sg_d_slice(g, q, 31, 0)));
#endif
}

/* CLZ, REV, REV16, REVSH, RBIT, the extends, the bitfields, MRS and MSR (llvm.c's miscellaneous) */
static int miscellaneous(SgCtx* g, SnxA32Kind kind) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned d = (w >> 12) & 15, n = (w >> 16) & 15;
    condition(g);
    if (kind == SNX_A32_MOVW ||
        kind == SNX_A32_MOVT) { /* a profile the Sail has none for (the ARM7TDMI, which decodes it) */
        SnxU32 imm = ((w >> 4) & 0xf000) | (w & 0xfff);
        sg_GPR_write(g, d,
                     kind == SNX_A32_MOVW
                         ? sgx_const(32, imm)
                         : sg_d_or(g, sg_d_and(g, reg(g, d), sgx_const(32, 65535)), sgx_const(32, imm << 16)));
        return 0;
    }
    SgD v = reg(g, w & 15), r;
    switch (kind) {
        case SNX_A32_CLZ: r = sg_i_slice(g, sg_i_clz(g, v), 31, 0); break;
        case SNX_A32_REV:
            if ((w & 0x00400080) == 0x00400000)
                r = sg_d_bitreverse(g, v);
            else if (!(w & 0x80))
                r = SGX_OP(x, rev, 32, v);
            else {
                r = sg_d_or(g, sg_d_and(g, sg_d_lsr(g, v, 8), sgx_const(32, 0x00ff00ff)),
                            sg_d_lsl(g, sg_d_and(g, v, sgx_const(32, 0x00ff00ff)), 8));
                if (w & (1u << 22)) r = sg_d_sext(g, sg_d_slice(g, r, 15, 0), 32); /* REVSH */
            }
            break;
        case SNX_A32_EXTEND: {
            SgD t = sg_d_ror(g, v, ((w >> 10) & 3) * 8);
            int sign = !(w & (1u << 22));
            if (!(w & (3u << 20))) { /* SXTB16, UXTB16, with add: bytes 0 and 2 */
                SgD lo = sign ? sg_d_sext(g, sg_d_slice(g, t, 7, 0), 32) : sg_d_zext(g, sg_d_slice(g, t, 7, 0), 32);
                SgD hi = sign ? sg_d_sext(g, sg_d_slice(g, t, 23, 16), 32) : sg_d_zext(g, sg_d_slice(g, t, 23, 16), 32);
                if (n != 15) {
                    SgD rn = reg(g, n);
                    lo = sg_d_add(g, lo, rn);
                    hi = sg_d_add(g, hi, sg_d_lsr(g, rn, 16));
                }
                r = sg_d_or(g, sg_d_and(g, lo, sgx_const(32, 0xffff)), sg_d_lsl(g, hi, 16));
            } else {
                unsigned bits = w & (1u << 20) ? 16 : 8;
                SgD e = sg_d_slice(g, t, bits - 1, 0);
                r = sign ? sg_d_sext(g, e, 32) : sg_d_zext(g, e, 32);
                if (n != 15) r = sg_d_add(g, r, reg(g, n));
            }
            break;
        }
        case SNX_A32_BITFIELD: {
            unsigned lsb = (w >> 7) & 31;
            if ((w & 0x0fe00070) == 0x07c00010) { /* BFI, BFC */
                SnxU32 mask = UINT32_MAX >> (32 - (((w >> 16) & 31) - lsb + 1));
                r = (w & 15) == 15 ? sgx_const(32, 0) : sg_d_lsl(g, sg_d_and(g, v, sgx_const(32, mask)), lsb);
                r = sg_d_or(g, sg_d_and(g, reg(g, d), sgx_const(32, ~(mask << lsb))), r);
            } else { /* SBFX, UBFX */
                r = sg_d_slice(g, v, lsb + ((w >> 16) & 31), lsb);
                r = w & (1u << 22) ? sg_d_zext(g, r, 32) : sg_d_sext(g, r, 32);
            }
            break;
        }
        case SNX_A32_MRS: /* the execution state bits (IT, J, T) read as zero */
            sg_flags_flush(g);
            r = sg_d_and(g, sgx_a32_cpsr(g), sgx_const(32, ~(0x0600FC00u | (1u << 24) | SNX_ARM_T)));
            break;
        case SNX_A32_MSR: {
            SnxU32 mask = (n & 8 ? 0xf8000000u : 0) | (n & 4 ? 0x000f0000u : 0) | (n & 2 ? SNX_ARM_E : 0);
            if (w & (1u << 25)) {
                unsigned rot = ((w >> 8) & 15) * 2;
                SnxU32 imm = w & 255;
                if (rot) imm = (imm >> rot) | (imm << (32 - rot));
                v = sgx_const(32, imm);
            }
            sg_flags_flush(g);
            if (sgx_a32_hosted(g) && (mask & SNX_ARM_E)) /* a hosted block never sets E */
                sgx_a32_exit(g, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, v, sgx_const(32, SNX_ARM_E)))),
                             SNX_ARM_ENDIAN_UNSUPPORTED, sgx_const(32, w), 0);
            SgD c = sg_d_or(g, sg_d_and(g, sgx_a32_cpsr(g), sgx_const(32, ~mask)), sg_d_and(g, v, sgx_const(32, mask)));
            g->cpsr = sgx_a32_put(g, offsetof(SnxArmCPU, cpsr), 32, c);
            g->origin = -1;
            if (sgx_a32_hosted(g)) return 0; /* so no entry looks at it and no block has to end to be entered with it */
            /* a block reads E only on entry: one that may have changed it ends here, the next one's entry checked */
            g->e_written = !!(w & (1u << 17));
            return g->e_written;
        }
        default: sg_fail("an A32 instruction this back end does not make", kind, 0);
    }
    sg_GPR_write(g, d, r);
    return 0;
}

/* LDREX, STREX and their byte, halfword and doubleword forms, with acquire or not (llvm.c's exclusive): the CPU's
   value-based monitor, the store a compare-and-swap of the value the load read. A Thumb-2 one adds g->offset to its base
   and takes the second register of a doubleword pair from g->pair (its number + 1) */
static void exclusive(SgCtx* g) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned op = (w >> 20) & 7, n = (w >> 16) & 15, d = (w >> 12) & 15, t = w & 15;
    static const unsigned widths[] = {4, 8, 1, 2};
    unsigned bytes = widths[op >> 1];
    condition(g);
    SgD addr = reg(g, n);
    if (g->offset) addr = sg_d_add(g, addr, sgx_const(32, g->offset));
    sgx_reg(x, &addr);
    sgx_a32_bounds(g, addr, bytes, bytes);
    if (op & 1) {
        SgD value = sg_d_zext(g, sgx_a32_load(g, addr, (int)bytes, 0), 64);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_value), 64, value);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_address), 32, addr);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_size), 32, sgx_const(32, bytes));
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_valid), 32, sgx_const(32, 1));
        sg_GPR_write(g, d, sg_d_slice(g, value, 31, 0));
        if (bytes == 8) sg_GPR_write(g, g->pair ? g->pair - 1 : d + 1, sg_d_slice(g, value, 63, 32));
    } else {
        const signed char* fix = SGX_A32_STREX_REGS(x); /* the machine's: the address, the value and the base */
        SgD value = reg(g, t);
        if (bytes == 8) value = sg_d_concat(g, reg(g, g->pair ? g->pair - 1 : t + 1), value);
        SgD in[3];
        int regs[3];
        in[0] = sgx_zext(x, addr, 64);
        in[1] = value;
        in[2] = sgx_val(64, sgx_a32_mem(g));
        for (int k = 0; k < 3; ++k) regs[k] = sgx_reg(x, &in[k]);
        SgXIns* i = sgx_a32_machine(g, SGX_A32_STREX, 1);
        for (int k = 0; k < 3; ++k) i->in[k] = regs[k], i->fixed[k] = fix[k];
        i->imm = bytes;
        i->mclobber = SGX_A32_STREX_CLOBBER;
        i->side = 1;
        i->def = sgx_vreg(x, x->n - 1, 32);
        sg_GPR_write(g, d, sgx_val(32, i->def));
    }
}

/* llvm.c's checked_exclusive (the ARM11's on checked memory): the bus owns the reservation (checked.h), the CPU-local
   address, size and valid are additional guards */
static void checked_exclusive(SgCtx* g) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned op = (w >> 20) & 7, n = (w >> 16) & 15, d = (w >> 12) & 15, t = w & 15;
    static const unsigned widths[] = {4, 8, 1, 2};
    unsigned bytes = widths[op >> 1];
    condition(g);
    SgD addr = reg(g, n);
    sgx_reg(x, &addr);
    if (bytes > 1)
        sgx_a32_exit(g, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, addr, sgx_const(32, bytes - 1)))), SNX_ARM_ALIGNMENT,
                     addr, 0);
    if (op & 1) {
        sgx_a32_checked_access(g, SNX_ARM_ACCESS_READ | SNX_ARM_ACCESS_EXCLUSIVE, addr, bytes);
        SgD low = sgx_a32_word(g, 0, 32), value = sg_d_zext(g, low, 64);
        if (bytes == 8) value = sg_d_concat(g, sgx_a32_word(g, 4, 32), low);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_value), 64, value);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_address), 32, addr);
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_size), 32, sgx_const(32, bytes));
        sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_valid), 32, sgx_const(32, 1));
        sg_GPR_write(g, d, low);
        if (bytes == 8) sg_GPR_write(g, d + 1, sg_d_slice(g, value, 63, 32));
        return;
    }
    sgx_a32_put(g, offsetof(SnxArmCheckedCPU, words), 32, reg(g, t));
    if (bytes == 8) sgx_a32_put(g, offsetof(SnxArmCheckedCPU, words) + 4, 32, reg(g, t + 1));
    SgD status = sgx_a32_checked_call(g, "snx_arm_checked_strex", addr, sgx_const(32, bytes), 0);
    sgx_reg(x, &status);
    sgx_a32_exit(g, sg_d_eq(g, status, sgx_const(32, 2)), SNX_ARM_DATA_ABORT, addr, 0);
    sg_GPR_write(g, d, status);
}

/* LDA, STL and their byte and halfword forms: a sequentially consistent access (a store then a full barrier, as the
   store-release the LLVM back end makes; a load then one where the machine's loads are no acquires) */
static void acquire_release(SgCtx* g) {
    SgX* x = sgx(g);
    SnxU32 w = g->word;
    unsigned op = (w >> 20) & 7, bytes = op >= 6 ? 2 : op >= 4 ? 1 : 4;
    condition(g);
    SgD addr = reg(g, (w >> 16) & 15);
    sgx_reg(x, &addr);
    sgx_a32_bounds(g, addr, bytes, bytes);
    if (op & 1) {
        SgD v = sgx_a32_load(g, addr, (int)bytes, 0);
        if (MG_ARM_LOAD_FENCE) sgx_a32_machine(g, SGX_A32_FENCE, 0);
        sg_GPR_write(g, (w >> 12) & 15, bytes < 4 ? sg_d_zext(g, v, 32) : v);
    } else {
        SgD v = reg(g, w & 15);
        sgx_a32_store(g, addr, (int)bytes, 0, bytes < 4 ? sg_d_slice(g, v, 8 * bytes - 1, 0) : v);
        sgx_a32_machine(g, SGX_A32_FENCE, 0);
        sgx_a32_monitor(g, addr, bytes);
    }
}

/* ---- ARMv6's and ARMv7's DSP and media instructions: llvm.c's parallel and media, in 64-bit lanes ---- */

/* the bits lo..lo+bits-1 of x, sign- or zero-extended to 64 bits */
static SgD lane(SgCtx* g, SgD x, unsigned lo, unsigned bits, int sign) {
    SgD f = sg_d_slice(g, x, lo + bits - 1, lo);
    return sign ? sg_d_sext(g, f, 64) : sg_d_zext(g, f, 64);
}
/* a < b, a > b and a >= b of 64-bit values, signed (sg_i_cmp's 2, 4 and 5) */
static SgB less(SgCtx* g, SgD a, SgD b) { return sg_i_cmp(g, 2, sgx_int(a, 1), sgx_int(b, 1)); }
static SgB greater(SgCtx* g, SgD a, SgD b) { return sg_i_cmp(g, 4, sgx_int(a, 1), sgx_int(b, 1)); }
static SgB at_least(SgCtx* g, SgD a, SgD b) { return sg_i_cmp(g, 5, sgx_int(a, 1), sgx_int(b, 1)); }
/* the signed 64-bit v clamped to lo..hi, and whether it was outside */
static SgD clamp(SgCtx* g, SgD v, long long lo, long long hi, SgB* saturated) {
    SgD low = sgx_const(64, (SnxU64)lo), high = sgx_const(64, (SnxU64)hi);
    SgB under = less(g, v, low), over = greater(g, v, high);
    *saturated = sg_b_or(g, under, over);
    return sg_d_select(g, under, low, sg_d_select(g, over, high, v));
}
/* the signed 64-bit v's low word, and whether v didn't fit in it */
static SgD narrow(SgCtx* g, SgD v, SgB* overflow) {
    SgD r = sg_d_slice(g, v, 31, 0);
    *overflow = sg_b_not(g, sg_d_eq(g, sg_d_sext(g, r, 64), v));
    return r;
}
/* CPSR's bits under mask = bits, the fields NZCV the region keeps written first */
static void cpsr_bits(SgCtx* g, SnxU32 mask, SgD bits) {
    sg_flags_flush(g);
    SgD c = sg_d_or(g, sg_d_and(g, sgx_a32_cpsr(g), sgx_const(32, ~mask)), bits);
    g->cpsr = sgx_a32_put(g, offsetof(SnxArmCPU, cpsr), 32, c);
}
/* Q is sticky: an instruction that saturates or overflows sets it, none clears it but MSR */
static void sticky_q(SgCtx* g, SgB set) {
    SgD q = sg_d_lsl(g, sg_d_zext(g, sgx_bool_bits(sgx(g), set), 32), 27);
    cpsr_bits(g, 0, q);
}
/* RdHi:RdLo, and RdHi:RdLo = v */
static SgD rdhi_rdlo(SgCtx* g, unsigned hi, unsigned lo) { return sg_d_concat(g, reg(g, hi), reg(g, lo)); }
static void set_rdhi_rdlo(SgCtx* g, unsigned hi, unsigned lo, SgD v) {
    sg_GPR_write(g, lo, sg_d_slice(g, v, 31, 0));
    sg_GPR_write(g, hi, sg_d_slice(g, v, 63, 32));
}
static SgD mul64(SgCtx* g, SgD a, SgD b) { return SGX_OP(sgx(g), mul, 64, a, b); }

/* {S,Q,SH,U,UQ,UH}{ADD16,ASX,SAX,SUB16,ADD8,SUB8}, lane by lane, where no sum or difference overflows 64 bits. The
   modular forms set GE: a lane's sum or difference >= 0, an unsigned sum's carry out */
static void parallel(SgCtx* g) {
    SnxU32 w = g->word;
    unsigned form = (w >> 20) & 3, u = (w >> 22) & 1, op = (w >> 5) & 7, bits = op >= 4 ? 8 : 16, lanes = 32 / bits;
    condition(g);
    SgD x = reg(g, (w >> 16) & 15), y = reg(g, w & 15), r = sgx_const(32, 0), ge = sgx_const(32, 0);
    for (unsigned i = 0; i < lanes; ++i) {
        /* ASX: the low lane subtracts Rm's high halfword, the high lane adds its low one. SAX the other way */
        unsigned from = op == 1 || op == 2 ? lanes - 1 - i : i;
        int subtract = op == 3 || op == 7 || (op == 1 && i == 0) || (op == 2 && i == 1);
        SgD p = lane(g, x, i * bits, bits, !u), q = lane(g, y, from * bits, bits, !u);
        SgD v = subtract ? sg_d_sub(g, p, q) : sg_d_add(g, p, q);
        if (form == 1) {
            SgB set = at_least(g, v, sgx_const(64, u && !subtract ? 1ull << bits : 0));
            ge = sg_d_or(g, ge,
                         sg_d_select(g, set, sgx_const(32, bits == 16 ? 3u << (2 * i) : 1u << i), sgx_const(32, 0)));
        } else if (form == 2) {
            SgB unused;
            v = u ? clamp(g, v, 0, (1 << bits) - 1, &unused)
                  : clamp(g, v, -(1 << (bits - 1)), (1 << (bits - 1)) - 1, &unused);
        } else
            v = sg_d_asr(g, v, 1);
        r = sg_d_or(g, r, sg_d_lsl(g, sg_d_zext(g, sg_d_slice(g, v, bits - 1, 0), 32), i * bits));
    }
    if (form == 1) cpsr_bits(g, 0x000f0000u, sg_d_lsl(g, ge, 16));
    sg_GPR_write(g, (w >> 12) & 15, r);
}

/* SSAT, USAT and their 16-bit forms, PKHBT and PKHTB, SEL, QADD and its kin, the halfword and dual multiplies,
   USAD8 and USADA8, CRC32 and CRC32C */
static void media(SgCtx* g, SnxA32Kind kind) {
    SnxU32 w = g->word;
    unsigned n = (w >> 16) & 15, d = (w >> 12) & 15, s = (w >> 8) & 15, op;
    condition(g);
    SgD x = reg(g, w & 15), r, v;
    SgB saturated;
    switch (kind) {
        case SNX_A32_SATURATE: {
            int u = !!(w & (1u << 22));
            if ((w & 0x0fb00ff0) == 0x06a00f30) { /* SSAT16, USAT16: to 1..16 or 0..15 bits */
                unsigned bits = ((w >> 16) & 15) + !u;
                long long lo = u ? 0 : -(1ll << (bits - 1)), hi = u ? (1ll << bits) - 1 : (1ll << (bits - 1)) - 1;
                SgB any = sg_bool(g, 0);
                r = sgx_const(32, 0);
                for (unsigned i = 0; i < 2; ++i) {
                    v = clamp(g, lane(g, x, i * 16, 16, 1), lo, hi, &saturated);
                    any = sg_b_or(g, any, saturated);
                    r = sg_d_or(g, r, sg_d_lsl(g, sg_d_zext(g, sg_d_slice(g, v, 15, 0), 32), i * 16));
                }
                saturated = any;
            } else { /* SSAT, USAT: Rm LSL or ASR (ASR #32 encoded as 0), to 1..32 or 0..31 bits */
                unsigned bits = ((w >> 16) & 31) + !u;
                long long lo = u ? 0 : -(1ll << (bits - 1)), hi = u ? (1ll << bits) - 1 : (1ll << (bits - 1)) - 1;
                v = shift_imm(g, x, (w >> 5) & 2, (w >> 7) & 31);
                r = sg_d_slice(g, clamp(g, sg_d_sext(g, v, 64), lo, hi, &saturated), 31, 0);
            }
            sticky_q(g, saturated);
            sg_GPR_write(g, d, r);
            return;
        }
        case SNX_A32_PACK: /* PKHBT: Rn's bottom, Rm LSL's top. PKHTB: Rn's top, Rm ASR's bottom */
            v = shift_imm(g, x, w & 0x40 ? 2 : 0, (w >> 7) & 31);
            r = w & 0x40 ? sg_d_or(g, sg_d_and(g, reg(g, n), sgx_const(32, 0xffff0000u)),
                                   sg_d_and(g, v, sgx_const(32, 0xffff)))
                         : sg_d_or(g, sg_d_and(g, reg(g, n), sgx_const(32, 0xffff)),
                                   sg_d_and(g, v, sgx_const(32, 0xffff0000u)));
            sg_GPR_write(g, d, r);
            return;
        case SNX_A32_SELECT: { /* each byte from Rn where its GE bit is set, else from Rm: a GE bit spread to a byte */
            SgD c = sgx_a32_cpsr(g), mask = sg_d_sext(g, sg_d_slice(g, c, 19, 19), 8);
            for (unsigned i = 3; i-- > 0;)
                mask = sg_d_concat(g, mask, sg_d_sext(g, sg_d_slice(g, c, 16 + i, 16 + i), 8));
            sg_GPR_write(g, d, sg_d_or(g, sg_d_and(g, reg(g, n), mask), sg_d_and(g, x, sg_d_not(g, mask))));
            return;
        }
        case SNX_A32_QARITH: { /* Rm + or - Rn, Rn doubled with saturation first for QDADD and QDSUB */
            SgD y = sg_d_sext(g, reg(g, n), 64);
            SgB first = sg_bool(g, 0);
            op = (w >> 21) & 3;
            if (op & 2) y = clamp(g, sg_d_add(g, y, y), INT32_MIN, INT32_MAX, &first);
            v = op & 1 ? sg_d_sub(g, sg_d_sext(g, x, 64), y) : sg_d_add(g, sg_d_sext(g, x, 64), y);
            r = sg_d_slice(g, clamp(g, v, INT32_MIN, INT32_MAX, &saturated), 31, 0);
            sticky_q(g, sg_b_or(g, first, saturated));
            sg_GPR_write(g, d, r);
            return;
        }
        case SNX_A32_HALF_MUL: { /* Rd (RdHi) n, Ra (RdLo) d, Rm s, Rn the low field: N picks Rn's half, M Rm's */
            SgD m = lane(g, reg(g, s), w & 0x40 ? 16 : 0, 16, 1);
            op = (w >> 21) & 3;
            if (op == 1) { /* SMLAW<y>, SMULW<y>: the top 32 bits of the 48-bit product */
                v = sg_d_slice(g, sg_d_asr(g, mul64(g, sg_d_sext(g, x, 64), m), 16), 31, 0);
                if (w & 0x20) {
                    sg_GPR_write(g, n, v);
                    return;
                }
                r = narrow(g, sg_d_add(g, sg_d_sext(g, v, 64), sg_d_sext(g, reg(g, d), 64)), &saturated);
                sticky_q(g, saturated);
                sg_GPR_write(g, n, r);
                return;
            }
            v = mul64(g, lane(g, x, w & 0x20 ? 16 : 0, 16, 1), m); /* at most 2^30 */
            if (op == 0) {                                         /* SMLA<x><y>: Q on overflow */
                r = narrow(g, sg_d_add(g, v, sg_d_sext(g, reg(g, d), 64)), &saturated);
                sticky_q(g, saturated);
                sg_GPR_write(g, n, r);
            } else if (op == 2) /* SMLAL<x><y> */
                set_rdhi_rdlo(g, n, d, sg_d_add(g, rdhi_rdlo(g, n, d), v));
            else
                sg_GPR_write(g, n, sg_d_slice(g, v, 31, 0));
            return;
        }
        case SNX_A32_DUAL_MUL: { /* Rd (RdHi) n, Ra (RdLo) d, Rm s, Rn the low field */
            SgD m = reg(g, s);
            op = (w >> 5) & 7;
            if (((w >> 20) & 7) == 5) { /* SMMLA, SMMUL, SMMLS: the top 32 bits of (Ra << 32) +/- Rn * Rm, R rounds */
                SgD acc = d != 15 ? sg_d_lsl(g, sg_d_zext(g, reg(g, d), 64), 32) : sgx_const(64, 0);
                v = mul64(g, sg_d_sext(g, x, 64), sg_d_sext(g, m, 64));
                v = op >= 6 ? sg_d_sub(g, acc, v) : sg_d_add(g, acc, v);
                if (w & 0x20) v = sg_d_add(g, v, sgx_const(64, 0x80000000u));
                sg_GPR_write(g, n, sg_d_slice(g, v, 63, 32));
                return;
            }
            if (w & 0x20) m = sg_d_ror(g, m, 16); /* X: exchange Rm's halves */
            r = mul64(g, lane(g, x, 0, 16, 1), lane(g, m, 0, 16, 1));
            v = mul64(g, lane(g, x, 16, 16, 1), lane(g, m, 16, 16, 1));
            v = op & 2 ? sg_d_sub(g, r, v) : sg_d_add(g, r, v);
            if (((w >> 20) & 7) == 4) { /* SMLALD, SMLSLD */
                set_rdhi_rdlo(g, n, d, sg_d_add(g, rdhi_rdlo(g, n, d), v));
                return;
            }
            if (d != 15) v = sg_d_add(g, v, sg_d_sext(g, reg(g, d), 64)); /* SMLAD, SMLSD; SMUAD, SMUSD without Ra */
            r = narrow(g, v, &saturated);
            sticky_q(g, saturated);
            sg_GPR_write(g, n, r);
            return;
        }
        case SNX_A32_ABS_DIFF: { /* USAD8, USADA8: Rd n, Ra d, Rm s, Rn the low field */
            SgD m = reg(g, s);
            r = d != 15 ? reg(g, d) : sgx_const(32, 0);
            for (unsigned i = 0; i < 4; ++i) {
                v = sg_d_sub(g, lane(g, x, i * 8, 8, 0), lane(g, m, i * 8, 8, 0));
                v = sg_d_select(g, less(g, v, sgx_const(64, 0)), sg_d_sub(g, sgx_const(64, 0), v), v);
                r = sg_d_add(g, r, sg_d_slice(g, v, 31, 0));
            }
            sg_GPR_write(g, n, r);
            return;
        }
        default: { /* CRC32, CRC32C: the reflected CRC of Rm's low 1, 2 or 4 bytes onto Rn, without inversions */
            unsigned bits = 8u << ((w >> 21) & 3);
            SgD poly = sgx_const(32, w & 0x200 ? 0x82f63b78u : 0xedb88320u);
            r = sg_d_xor(g, reg(g, n), bits == 32 ? x : sg_d_and(g, x, sgx_const(32, (1u << bits) - 1)));
            for (unsigned i = 0; i < bits; ++i)
                r = sg_d_xor(g, sg_d_lsr(g, r, 1),
                             sg_d_and(g, poly, sg_d_sub(g, sgx_const(32, 0), sg_d_and(g, r, sgx_const(32, 1)))));
            sg_GPR_write(g, d, r);
            return;
        }
    }
}
/* SETEND: E, which a block reads only on entry, so the block ends after it and the next one's entry checks it */
static int setend(SgCtx* g) {
    condition(g);
    if (sgx_a32_hosted(g)) { /* a hosted block's E is never set: BE stops, LE is what it is */
        if (!(g->word & SNX_ARM_E)) return 0;
        sgx_a32_stop(g, SNX_ARM_ENDIAN_UNSUPPORTED, sgx_const(32, g->word), (SnxU32)g->pc);
        return 1;
    }
    cpsr_bits(g, SNX_ARM_E, sgx_const(32, g->word & SNX_ARM_E));
    g->e_written = 1;
    return 1;
}

/* ---- VFP: llvm_vector.inc's scalar part (vector_memory, vector_transfer, vector_fp), its hot paths fp_call's ---- */

/* the D bank's byte offset off (S2n at Dn's, S2n+1 4 past it) as a state field of width bits: Dn is a number, so a
   big-endian host (RK_BIG_ENDIAN) has S2n's word 4 past its start and S2n+1's at it */
#if defined(RK_BIG_ENDIAN) && RK_BIG_ENDIAN
#define MG_ARM_VWORD 4u
#else
#define MG_ARM_VWORD 0u
#endif
static int voff_w(unsigned off, unsigned width) {
    if (width != 64 && off < 32 * 8) off ^= MG_ARM_VWORD;
    return (int)(offsetof(SnxArmCPU, d) + off);
}
/* FPSCR, as a field the D bank's offsets reach */
#define FPSCR_OFF ((unsigned)(offsetof(SnxArmCPU, fpscr) - offsetof(SnxArmCPU, d)))
/* FPSCR bits that an ARM11 VMSR may set (llvm_vector.inc's): NZCV, DN, FZ, RMode (23-22), STRIDE (21-20), LEN (18-16),
   IDC (7) and the cumulative flags. Any other bit stops the block: the trap enables (15-8), which the block does not
   model, and bits 27 and 26, which the ARM11 does not define; and what it stores, those two with them */
#define FPSCR_ARM11_WRITABLE 0xf3f7009fu
#define FPSCR_ARM11_STORED   0xfff7009fu
/* a VFP register's bits (32 or 64) in a general-purpose register */
static SgD vbits(SgCtx* g, unsigned off, unsigned width) {
    SgX* x = sgx(g);
    SgXIns* i = sgx_append(x, SGX_K_LOAD);
    i->w = (unsigned char)(width == 64 ? 64 : 32);
    i->disp = voff_w(off, width);
    i->def = sgx_vreg(x, x->n - 1, width == 64 ? 64 : 32);
    return sgx_val(width, i->def);
}
/* the half of a VFP register's word at off, an S register's bottom or top one: its word read whole, as a big-endian host
   has the half elsewhere in it and some hosts load aligned words only */
static SgD vhalf(SgCtx* g, unsigned off) {
    unsigned low = (off & 2) * 8;
    return sg_d_slice(g, vbits(g, off & ~2u, 32), low + 15, low);
}
/* the VFP register = v (bits of 32 or 64, or an XMM value of them) */
static void vset(SgCtx* g, unsigned off, unsigned width, SgD v) {
    SgX* x = sgx(g);
    if (v.x) {
        int in[4] = {0, v.x, 0, 0};
        sgx_vins(x, width == 64 ? SGX_VF_ST8 : SGX_VF_ST4, 0, 0, in, 0, 0, SGX_VM_STATE, 0, 0, voff_w(off, width), 0,
                 0);
        return;
    }
    {
        int r = sgx_reg(x, &v);
        SgXIns* i = sgx_append(x, SGX_K_STORE);
        i->w = (unsigned char)(width == 64 ? 64 : 32);
        i->in[0] = r;
        i->disp = voff_w(off, width);
    }
}
/* a VFP register as a vector value (a 4- or 8-byte load from the state) */
static SgD vxmm(SgCtx* g, unsigned off, unsigned width) {
    return sgx_xval(width, sgx_vins(sgx(g), width == 64 ? SGX_VF_LD8 : SGX_VF_LD4, 0, 0, NULL, 0, 1, SGX_VM_STATE, 0,
                                    0, voff_w(off, width), 0, 0));
}
/* FPSCR lets the hot path run: round to nearest and no trap enabled, and for arithmetic (not a compare) IXC set already
   (the inline code raises no flag): fp_call's mask. Once a region, until a VMSR or a call */
static SgB fp_state(SgCtx* g, int arithmetic) {
    SgX* x = sgx(g);
    if (!g->vfp_ok[arithmetic]) {
        SnxU32 mask = 0x00c09f00u | (arithmetic ? 0x10u : 0);
        SgD f = sg_d_and(g, vbits(g, FPSCR_OFF, 32), sgx_const(32, mask));
        g->vfp_ok[arithmetic] = sg_d_eq(g, f, sgx_const(32, arithmetic ? 0x10u : 0));
    }
    (void)x;
    return g->vfp_ok[arithmetic];
}
/* fp_plain of bits (w 32 or 64): finite, and normal, or a zero where zero_ok (0: always). The bits doubled (the sign
   out) less the smallest normal's, below the span of the finite exponents, unsigned; or 0 */
static SgB fp_plain(SgCtx* g, SgD bits, unsigned w, SgB zero_ok) {
    SnxU64 low = w == 32 ? 1ull << 24 : 1ull << 53, span = w == 32 ? 0xFE000000ull : 0xFFC0000000000000ull;
    SgD t = sg_d_lsl(g, bits, 1);
    SgB normal = sgx_d_ult(g, sg_d_sub(g, t, sgx_const(w, low)), sgx_const(w, span));
    SgB zero = sg_d_is_zero(g, t);
    if (zero_ok) zero = sg_b_and(g, zero, zero_ok);
    return sg_b_or(g, normal, zero);
}
/* bits are a zero (either sign) */
static SgB fp_zero(SgCtx* g, SgD bits) { return sg_d_is_zero(g, sg_d_lsl(g, bits, 1)); }
/* snx_arm_fp's arguments after the operands, for snx_arm_fp_x64 */
static long long fp_desc(SnxArmVectorOp op, unsigned width, SnxU32 param) {
    return (long long)((SnxU64)op | (SnxU64)width << 8 | (SnxU64)param << 32);
}
/* fp_call's helper, always: the state's registers and CPSR read again after it */
static SgD fp_helper(SgCtx* g, SnxArmVectorOp op, unsigned width, SnxU32 param, SgD a, SgD b, SgD c) {
    SgX* x = sgx(g);
    SgD args[4];
    sgx_flat(x, &a);
    sgx_flat(x, &b);
    sgx_flat(x, &c);
    args[0] = sg_d_zext(g, a, 64);
    args[1] = sg_d_zext(g, b, 64);
    args[2] = sg_d_zext(g, c, 64);
    args[3] = sgx_const(64, (SnxU64)fp_desc(op, width, param));
    sgx_a32_call(g, "snx_arm_fp_x64", args, 4, 1);
    return sgx_val(64, x->ins[x->n - 1].def);
}
/* the machine's fp_call (after this file): snx_arm_fp's operation op of width on x, y and z, its hot path where the
   machine has one (MG_ARM_FP_HOT: the operands of fadd...fcmpe then vector values) */
static SgD fp_call(SgCtx* g, SnxArmVectorOp op, unsigned width, SnxU32 param, SgD xv, SgD yv, SgD zv);

/* VLDR, VSTR, VLDM, VSTM, VPUSH, VPOP (vector_memory): word aligned, the words in their order (a doubleword's two at
   once) */
static void vfp_memory(SgCtx* g, const SnxArmVector* v) {
    SgD base = reg(g, v->n), offset = sgx_const(32, (SnxU32)v->imm);
    int single = (v->flags & AV_PRE) && !(v->flags & AV_WB);
    SgD end = v->flags & AV_UP ? sg_d_add(g, base, offset) : sg_d_sub(g, base, offset);
    SgD addr = single || (v->flags & AV_PRE) ? end : base;
    int wide = !sgx_a32_legacy(g) && !sgx_a32_checked(g); /* a doubleword at once */
    sgx_reg(sgx(g), &addr);
    if (sgx_a32_checked(g))
        sgx_a32_transfer(g, addr, v->bytes, 4, !(v->flags & AV_LOAD), 1);
    else
        sgx_a32_bounds(g, addr, v->bytes, 4);
    for (unsigned i = 0, step; i < v->bytes; i += step) {
        step = wide && !((v->d + i) & 7) && i + 8 <= v->bytes ? 8 : 4;
        if (!wide && (v->flags & AV_LOAD))
            vset(g, v->d + i, 32, sgx_a32_read(g, addr, 4, (int)i, 0));
        else if (!wide)
            sgx_a32_write(g, addr, 4, (int)i, vbits(g, v->d + i, 32), 0);
        else if (v->flags & AV_LOAD)
            vset(g, v->d + i, step * 8, sgx_a32_load(g, addr, (int)step, (int)i));
        else
            sgx_a32_store(g, addr, (int)step, (int)i, vbits(g, v->d + i, step * 8));
    }
    if (!(v->flags & AV_LOAD)) sgx_a32_written(g, addr, v->bytes);
    if (v->flags & AV_WB) sg_GPR_write(g, v->n, end);
}
/* ---- lanes: llvm_vector.inc's vget and vset, a D register's 64 bits read once and written once an instruction ---- */

/* the D bank's words an instruction reads, each loaded once: an instruction reads all its lanes before it writes one */
typedef struct {
    unsigned off[24], n;
    SgD v[24];
} VRead;
static SgD vword(SgCtx* g, VRead* r, unsigned off) {
    for (unsigned k = 0; k < r->n; ++k)
        if (r->off[k] == off) return r->v[k];
    if (r->n == 24) sg_fail("a vector instruction that reads more than 24 doublewords", off, 24);
    r->off[r->n] = off;
    r->v[r->n] = vbits(g, off, 64);
    return r->v[r->n++];
}
/* the lane of width bits at the D bank's byte offset off */
static SgD vlane(SgCtx* g, VRead* r, unsigned off, unsigned width) {
    SgD w = vword(g, r, off & ~7u);
    unsigned at = (off & 7) * 8;
    return width == 64 ? w : sg_d_slice(g, w, at + width - 1, at);
}
/* the D bank's words an instruction writes, its lanes put in them: each stored once, over the word as it was where
   its lanes don't cover all of it (vflush) */
typedef struct {
    unsigned off[24], n;
    SgD v[24];
    SnxU64 mask[24];
} VWrite;
static void vput(SgCtx* g, VWrite* o, unsigned off, unsigned width, SgD x) {
    unsigned word = off & ~7u, at = (off & 7) * 8, k = 0;
    SnxU64 m = (width == 64 ? ~0ull : (1ull << width) - 1) << at;
    while (k < o->n && o->off[k] != word) ++k;
    if (k == o->n) {
        if (k == 24) sg_fail("a vector instruction that writes more than 24 doublewords", off, 24);
        o->off[k] = word;
        o->mask[k] = 0;
        ++o->n;
    }
    sgx_flat(sgx(g), &x); /* an XMM value of fp_call's, as bits */
    if (x.w > width)
        x = sg_d_slice(g, x, width - 1, 0);
    else if (x.w < width)
        x = sg_d_zext(g, x, width);
    SgD l = width == 64 ? x : at ? sg_d_lsl(g, sg_d_zext(g, x, 64), at) : sg_d_zext(g, x, 64);
    /* an instruction's lanes are each written once (two-destination forms with D = M are zeroed before) */
    if (o->mask[k] & m) sg_fail("a vector lane written twice by one instruction", off, width);
    o->v[k] = o->mask[k] ? sg_d_or(g, o->v[k], l) : l;
    o->mask[k] |= m;
}
static void vflush(SgCtx* g, VWrite* o) {
    for (unsigned k = 0; k < o->n; ++k) {
        SgD v = o->v[k];
        if (o->mask[k] != ~0ull) v = sg_d_or(g, sg_d_and(g, vbits(g, o->off[k], 64), sgx_const(64, ~o->mask[k])), v);
        vset(g, o->off[k], 64, v);
    }
    o->n = 0;
}

/* VMRS, VMSR, VMOV between core registers and S registers or a D register's word (vector_transfer) */
static void vfp_transfer(SgCtx* g, const SnxArmVector* v) {
    if (v->op == AV_STATUS) {
        if (v->flags & AV_LOAD) {
            SgD f = vbits(g, FPSCR_OFF, 32);
            if (v->n == 15) { /* VMRS APSR_nzcv: CPSR's flags from FPSCR's */
                for (int k = 0; k < 4; ++k) sg_PSTATE_write(g, SG_PSTATE_N + k, 1, sg_d_slice(g, f, 31 - k, 31 - k));
            } else
                sg_GPR_write(g, v->n, f);
        } else {
            SgD f = reg(g, v->n);
            if (g->profile == SNX_ARM_PROFILE_ARM11) /* a bit its block does not model stops it (llvm_vector.inc's) */
                sgx_a32_trap_on(g, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, f, sgx_const(32, ~FPSCR_ARM11_WRITABLE)))),
                                SNX_ARM_BACKEND_UNSUPPORTED);
            vset(g, FPSCR_OFF, 32,
                 sg_d_and(g, f, sgx_const(32, g->profile == SNX_ARM_PROFILE_ARM11 ? FPSCR_ARM11_STORED : 0xffc0009fu)));
            g->vfp_ok[0] = g->vfp_ok[1] = 0;
        }
    } else if (v->op == AV_CORE_PAIR) {
        if (v->flags & AV_LOAD) {
            SgD a = vbits(g, v->d, 32), b = vbits(g, v->d + 4, 32);
            sg_GPR_write(g, v->n, a);
            sg_GPR_write(g, v->m, b);
        } else {
            SgD a = reg(g, v->n), b = reg(g, v->m);
            vset(g, v->d, 32, a);
            vset(g, v->d + 4, 32, b);
        }
    } else if (v->width == 32) {
        if (v->flags & AV_LOAD)
            sg_GPR_write(g, v->n, vbits(g, v->d, 32));
        else if (v->op == AV_DUP) {
            SgD r = reg(g, v->n);
            for (unsigned i = 0; i < v->bytes; i += 4) vset(g, v->d + i, 32, r);
        } else
            vset(g, v->d, 32, reg(g, v->n));
    } else if (v->flags & AV_LOAD) { /* a byte or halfword lane, sign-extended unless U */
        VRead in = {0};
        SgD l = vlane(g, &in, v->d, v->width);
        sg_GPR_write(g, v->n, v->flags & AV_UNSIGNED ? sg_d_zext(g, l, 32) : sg_d_sext(g, l, 32));
    } else { /* a byte or halfword lane, or VDUP's every lane, from Rt's low bits */
        VWrite out = {0};
        SgD r = sg_d_slice(g, reg(g, v->n), v->width - 1, 0);
        for (unsigned i = 0; i < (v->op == AV_DUP ? v->bytes : v->width / 8); i += v->width / 8)
            vput(g, &out, v->d + i, v->width, r);
        vflush(g, &out);
    }
}
/* the scalar data processing (vector_fp's, one lane) */
static void vfp_data(SgCtx* g, const SnxArmVector* v) {
    unsigned width = v->width, outwidth = width;
    SnxU32 param = 0;
    int unary = v->op == AV_MOV || (v->op >= AV_FABS && v->op <= AV_FSQRT) || v->op == AV_FCVT || v->op == AV_FTOI ||
                v->op == AV_ITOF || v->op == AV_FROUND || v->op == AV_FRECPE || v->op == AV_FRSQRTE ||
                v->op == AV_URECPE || v->op == AV_URSQRTE;
    /* the operations fp_call has a hot path for, whose operands are vector values; the others' are bits */
    int hot = MG_ARM_FP_HOT &&
              (v->op == AV_FADD || v->op == AV_FSUB || v->op == AV_FMUL || v->op == AV_FDIV || v->op == AV_FMLA ||
               v->op == AV_FMLS || v->op == AV_FNMLA || v->op == AV_FNMLS || v->op == AV_FNMUL || v->op == AV_FCMP ||
               v->op == AV_FCMPE);
    if (v->op == AV_FCVT)
        outwidth = v->aux;
    else if (v->op == AV_FTOI)
        outwidth = v->bytes * 8;
    if (v->flags & AV_UNSIGNED) param |= 1u << 28;
    if (v->op == AV_FCVT) param |= v->aux | ((SnxU32)v->imm << 24);
    if (v->op == AV_FTOI || v->op == AV_ITOF)
        param |= v->aux << 8 | (((SnxU32)v->imm >> 8) & 255) << 16 | ((SnxU32)v->imm & 15) << 24;
    if (v->op == AV_FROUND) param |= (SnxU32)v->imm << 24 | v->aux;
    unsigned inwidth = v->op == AV_ITOF ? v->aux : width, xo = unary ? v->m : v->n;
    SgD xv = inwidth == 16 ? vhalf(g, xo) : hot ? vxmm(g, xo, inwidth) : vbits(g, xo, inwidth);
    /* a unary operation's second operand is one no one reads: its first, as a load of it at width may not be aligned (a
       double from an S register) */
    SgD yv = (v->op == AV_FCMP || v->op == AV_FCMPE) && v->aux ? sgx_const(width, 0)
             : unary                                           ? xv
             : hot                                             ? vxmm(g, v->m, width)
                                                               : vbits(g, v->m, width);
    /* the destination, which the multiply-accumulates add to, and which fp_call hands the helper for every operation
       (the fused ones read it there) */
    SgD zv = hot && v->op >= AV_FMLA && v->op <= AV_FNMLS ? vxmm(g, v->d, outwidth)
             : hot || outwidth == 16                      ? sgx_const(outwidth, 0)
                                                          : vbits(g, v->d, outwidth);
    SgD r;
    switch (v->op) {
        case AV_MOV: r = xv; break;
        case AV_FABS: r = sg_d_and(g, xv, sgx_const(width, sg_ones(width - 1))); break;
        case AV_FNEG: r = sg_d_xor(g, xv, sgx_const(width, 1ull << (width - 1))); break;
        case AV_FSELECT: {
            static const unsigned conds[4] = {0, 6, 10, 12}; /* VSELEQ, VSELVS, VSELGE, VSELGT */
            r = sg_d_select(g, sgx_a32_condition(g, conds[v->aux & 3]), xv, yv);
            break;
        }
        default: r = fp_call(g, v->op, width, param, xv, yv, zv); break;
    }
    if (v->op == AV_FCMP || v->op == AV_FCMPE) {
        SgD f = vbits(g, FPSCR_OFF, 32);
        vset(g, FPSCR_OFF, 32,
             sg_d_or(g, sg_d_and(g, f, sgx_const(32, 0x0fffffffu)), sg_d_lsl(g, sg_d_slice(g, r, 31, 0), 28)));
        return;
    }
    if (v->op == AV_FTOI) {
        r = sg_d_slice(g, r, v->aux - 1, 0);
        r = v->flags & AV_UNSIGNED ? sg_d_zext(g, r, outwidth) : sg_d_sext(g, r, outwidth);
    } else if (!r.x && r.w != outwidth)
        r = sg_d_slice(g, r, outwidth - 1, 0);
    if (outwidth == 16) { /* VCVTB and VCVTT to half precision: the S register's bottom or top half */
        VWrite out = {0};
        vput(g, &out, v->d, 16, r);
        vflush(g, &out);
    } else
        vset(g, v->d, outwidth, r);
}
/* ---- Advanced SIMD: llvm_vector.inc's vector_fp, vector_integer, vector_permute and vector_struct, lane by lane, its
   lanes' 128-bit arithmetic in 64 bits: exact for lanes of 32 bits or less, the 64-bit lanes' saturations, rounded
   shifts and polynomial product made without bits past 64 ---- */

/* vector_fp's SIMD form: each lane fp_call's, under the standard FPSCR value (param bit 30) */
static void neon_fp(SgCtx* g, const SnxArmVector* v) {
    unsigned width = v->width, outwidth = width;
    SnxU32 param = 1u << 30;
    int unary = v->op == AV_MOV || (v->op >= AV_FABS && v->op <= AV_FSQRT) || v->op == AV_FCVT || v->op == AV_FTOI ||
                v->op == AV_ITOF || v->op == AV_FROUND || v->op == AV_FRECPE || v->op == AV_FRSQRTE ||
                v->op == AV_URECPE || v->op == AV_URSQRTE;
    if (v->op == AV_FCVT || v->op == AV_FTOI) outwidth = v->aux;
    unsigned lanes = v->bytes * 8 / outwidth;
    if (lanes > 8) sg_fail("a vector floating-point instruction of more than 8 lanes", lanes, 8);
    if (v->flags & AV_UNSIGNED) param |= 1u << 28;
    if (v->op == AV_FCVT) param |= v->aux | ((SnxU32)v->imm << 24);
    if (v->op == AV_FTOI || v->op == AV_ITOF)
        param |= v->aux << 8 | (((SnxU32)v->imm >> 8) & 255) << 16 | ((SnxU32)v->imm & 15) << 24;
    if (v->op == AV_FROUND) param |= (SnxU32)v->imm << 24 | v->aux;
    VRead in = {0};
    VWrite out = {0};
    SgD x[8], y[8], z[8];
    for (unsigned i = 0; i < lanes; i++) {
        unsigned inwidth = v->op == AV_ITOF ? v->aux : width;
        x[i] = vlane(g, &in, (unary ? v->m : v->n) + i * inwidth / 8, inwidth);
        y[i] = (v->op == AV_FCMP || v->op == AV_FCMPE) && v->aux
                   ? sgx_const(width, 0)
                   : vlane(g, &in, v->m + (v->flags & AV_SCALAR ? 0 : i * width / 8), width);
        if (v->flags & AV_PAIR) {
            unsigned base = i < lanes / 2 ? v->n : v->m, j = i % (lanes / 2);
            x[i] = vlane(g, &in, base + j * 2 * width / 8, width);
            y[i] = vlane(g, &in, base + (j * 2 + 1) * width / 8, width);
        }
        if (v->flags & AV_ZERO) {
            y[i] = sgx_const(width, 0);
            if (v->flags & AV_REVERSE) {
                y[i] = x[i];
                x[i] = sgx_const(width, 0);
            }
        }
        z[i] = vlane(g, &in, v->d + i * outwidth / 8, outwidth);
    }
    for (unsigned i = 0; i < lanes; i++) {
        SgD r;
        switch (v->op) {
            case AV_MOV: r = x[i]; break;
            case AV_FABS: r = sg_d_and(g, x[i], sgx_const(width, sg_ones(width - 1))); break;
            case AV_FNEG: r = sg_d_xor(g, x[i], sgx_const(width, 1ull << (width - 1))); break;
            default: r = fp_call(g, v->op, width, param, x[i], y[i], z[i]); break;
        }
        if (v->op == AV_FTOI) {
            sgx_flat(sgx(g), &r);
            r = sg_d_slice(g, r, v->aux - 1, 0);
        }
        vput(g, &out, v->d + i * outwidth / 8, outwidth, r);
    }
    vflush(g, &out);
}

/* a lane's bits as the integer they are, in 64 bits: signed unless uns (a 64-bit lane: its bits) */
static SgD widen(SgCtx* g, SgD x, int uns) { return x.w == 64 ? x : uns ? sg_d_zext(g, x, 64) : sg_d_sext(g, x, 64); }
static SgB sign_of(SgCtx* g, SgD x) { return sg_b_not(g, sg_d_is_zero(g, sg_d_slice(g, x, x.w - 1, x.w - 1))); }
/* FPSCR.QC, sticky, where any lane saturated */
static void neon_qc(SgCtx* g, SgB any) {
    SgXBool b = sgx(g)->b[any];
    if (b.kind == SGX_B_CONST && !b.v) return;
    SgD q = sg_d_lsl(g, sg_d_zext(g, sgx_bool_bits(sgx(g), any), 32), 27);
    vset(g, FPSCR_OFF, 32, sg_d_or(g, vbits(g, FPSCR_OFF, 32), q));
}
/* vsat: x (an integer in 64 bits, signed unless unsigned_x: then its 64 bits unsigned) clamped to bits bits, signed or
   (uns) unsigned, *qc ORed with whether it was outside */
static SgD vsat(SgCtx* g, SgD x, unsigned bits, int uns, int unsigned_x, SgB* qc) {
    SgB neg = unsigned_x ? sg_bool(g, 0) : sign_of(g, x), out;
    SgD r;
    if (uns) {
        SgB big = bits == 64 ? sg_bool(g, 0) : sg_b_not(g, sg_d_is_zero(g, sg_d_lsr(g, x, bits)));
        big = sg_b_and(g, big, sg_b_not(g, neg));
        r = sg_d_select(g, neg, sgx_const(64, 0), sg_d_select(g, big, sgx_const(64, sg_ones(bits)), x));
        out = sg_b_or(g, neg, big);
    } else {
        if (bits == 64) return x;
        SgB fits = sg_d_eq(g, sg_d_sext(g, sg_d_slice(g, x, bits - 1, 0), 64), x);
        SgD low = sgx_const(64, ~0ull << (bits - 1)), high = sgx_const(64, sg_ones(bits - 1));
        r = sg_d_select(g, fits, x, sg_d_select(g, neg, low, high));
        out = sg_b_not(g, fits);
    }
    *qc = sg_b_or(g, *qc, out);
    return r;
}
/* x >> n (1..64), arithmetic unless uns, rounded where round: x >> n plus bit n - 1 of x, which is what
   (x + 2^(n-1)) >> n gives without the sum's bit past 64 */
static SgD shr_round(SgCtx* g, SgD x, unsigned n, int uns, int round) {
    SgD q = n >= 64 ? (uns ? sgx_const(64, 0) : sg_d_asr(g, x, 63)) : uns ? sg_d_lsr(g, x, n) : sg_d_asr(g, x, n);
    if (!round) return q;
    SgD bit = n - 1 >= 64 ? sgx_const(64, 0) : sg_d_zext(g, sg_d_slice(g, x, n - 1, n - 1), 64);
    return sg_d_add(g, q, bit);
}
/* a 64-bit lane's saturating sum or difference (vsat of the 65-bit one) */
static SgD qaddsub64(SgCtx* g, SgD x, SgD y, int sub, int uns, SgB* qc) {
    SgD r = sub ? sg_d_sub(g, x, y) : sg_d_add(g, x, y), t;
    SgB over;
    if (uns) { /* the carry out of the sum, the borrow of the difference: bit 63 of the usual formulas */
        t = sub ? sg_d_or(g, sg_d_and(g, sg_d_not(g, x), y), sg_d_and(g, sg_d_or(g, sg_d_not(g, x), y), r))
                : sg_d_or(g, sg_d_and(g, x, y), sg_d_and(g, sg_d_or(g, x, y), sg_d_not(g, r)));
        over = sign_of(g, t);
        r = sg_d_select(g, over, sgx_const(64, sub ? 0 : ~0ull), r);
    } else { /* the operands' signs alike (unlike, for a difference) and the result's not x's */
        t = sg_d_and(g, sub ? sg_d_xor(g, x, y) : sg_d_not(g, sg_d_xor(g, x, y)), sg_d_xor(g, x, r));
        over = sign_of(g, t);
        r = sg_d_select(g, over, sg_d_select(g, sign_of(g, x), sgx_const(64, 1ull << 63), sgx_const(64, sg_ones(63))),
                        r);
    }
    *qc = sg_b_or(g, *qc, over);
    return r;
}
/* vshift: x (bits bits, signed unless uns) shifted by the signed byte at the bottom of k: left by it where it's 0 or
   more (past the lane: 0, or with saturate the value saturated where x isn't 0), right by its negation where it's
   negative, at most bits + 1, rounded where round. Its result's bits bits, a saturation ORed into *qc */
static SgD vshift(SgCtx* g, SgD x, SgD k, unsigned bits, int uns, int round, int saturate, SgB* qc) {
    SgD amount = sg_d_sext(g, sg_d_slice(g, k, 7, 0), 64), count = sg_d_sub(g, sgx_const(64, 0), amount);
    SgB left = at_least(g, amount, sgx_const(64, 0)), past = at_least(g, amount, sgx_const(64, bits));
    SgB saturated = sg_bool(g, 0);
    count = sg_d_select(g, less(g, count, sgx_const(64, 1)), sgx_const(64, 1), count);
    count = sg_d_select(g, greater(g, count, sgx_const(64, bits + 1)), sgx_const(64, bits + 1), count);
    SgD wide = widen(g, x, uns), r;
    if (bits < 64) { /* llvm_vector.inc's arithmetic: the lane's integer below 2^33, its shifts within 64 bits */
        SgD right = wide;
        if (round)
            right = sg_d_add(g, right, sg_d_shift_dyn(g, sgx_const(64, 1), sg_d_sub(g, count, sgx_const(64, 1)), 0));
        right = sg_d_shift_dyn(g, right, count, 2);
        SgD l = sg_d_shift_dyn(g, wide, sg_d_and(g, amount, sgx_const(64, 63)), 0), big = sgx_const(64, 0);
        if (saturate) {
            SgD outside = sgx_const(64, 1ull << bits);
            big = sg_d_select(g, sg_d_is_zero(g, wide), sgx_const(64, 0),
                              sg_d_select(g, sign_of(g, wide), sg_d_sub(g, sgx_const(64, 0), outside), outside));
        }
        r = sg_d_select(g, left, sg_d_select(g, past, big, l), right);
        if (saturate) r = vsat(g, r, bits, uns, 0, &saturated);
    } else { /* a 64-bit lane: the left shift's overflow seen by shifting back, the right shift rounded by its bit */
        SgD by = sg_d_and(g, amount, sgx_const(64, 63)), l = sg_d_shift_dyn(g, x, by, 0);
        SgB lost = sg_b_not(g, sg_d_eq(g, sg_d_shift_dyn(g, l, by, uns ? 1 : 2), x)),
            nonzero = sg_b_not(g, sg_d_is_zero(g, x));
        SgD most = uns ? sgx_const(64, ~0ull)
                       : sg_d_select(g, sign_of(g, x), sgx_const(64, 1ull << 63), sgx_const(64, sg_ones(63)));
        SgD lv = sg_d_select(g, past, saturate ? sg_d_select(g, nonzero, most, sgx_const(64, 0)) : sgx_const(64, 0),
                             saturate ? sg_d_select(g, lost, most, l) : l);
        /* right: count 1..65; at 64 and 65 the shift is the sign (or 0), and bit count - 1 past 63 the sign too */
        SgB far = at_least(g, count, sgx_const(64, 64));
        SgD fill = uns ? sgx_const(64, 0) : sg_d_asr(g, x, 63);
        SgD q = sg_d_select(g, far, fill, sg_d_shift_dyn(g, x, sg_d_and(g, count, sgx_const(64, 63)), uns ? 1 : 2));
        if (round) {
            SgD n1 = sg_d_sub(g, count, sgx_const(64, 1));
            SgD b = sg_d_and(g, sg_d_shift_dyn(g, x, sg_d_and(g, n1, sgx_const(64, 63)), 1), sgx_const(64, 1));
            SgD top = uns ? sgx_const(64, 0) : sg_d_lsr(g, x, 63);
            q = sg_d_add(g, q, sg_d_select(g, at_least(g, n1, sgx_const(64, 64)), top, b));
        }
        r = sg_d_select(g, left, lv, q);
        if (saturate)
            saturated = sg_b_and(g, left, sg_b_or(g, sg_b_and(g, past, nonzero), sg_b_and(g, sg_b_not(g, past), lost)));
    }
    *qc = sg_b_or(g, *qc, saturated);
    return r;
}
/* vector_permute: VEXT, VTBL, VTBX, VDUP of a scalar, VREV, VSWP, VTRN, VZIP, VUZP */
static void neon_permute(SgCtx* g, const SnxArmVector* v) {
    unsigned bytes = v->bytes, width = v->width, lanes = bytes * 8 / width;
    VRead in = {0};
    VWrite out = {0};
    if (v->op == AV_EXT) {
        SgD x[16], y[16];
        for (unsigned i = 0; i < bytes; i++) {
            x[i] = vlane(g, &in, v->n + i, 8);
            y[i] = vlane(g, &in, v->m + i, 8);
        }
        for (unsigned i = 0; i < bytes; i++) {
            unsigned index = (unsigned)v->imm + i;
            vput(g, &out, v->d + i, 8, index < bytes ? x[index] : y[index - bytes]);
        }
        vflush(g, &out);
        return;
    }
    if (v->op == AV_TBL || v->op == AV_TBX) {
        /* the table's byte at each index of M below its length (aux doublewords from N): its doubleword picked by the
           index's top bits, then shifted down by the bottom ones; else 0 (VTBL) or D's byte (VTBX) */
        SgD words[4], y[8], z[8];
        for (unsigned k = 0; k < v->aux; k++) words[k] = vword(g, &in, v->n + 8 * k);
        for (unsigned i = 0; i < bytes; i++) {
            y[i] = sg_d_zext(g, vlane(g, &in, v->m + i, 8), 64);
            z[i] = vlane(g, &in, v->d + i, 8);
        }
        for (unsigned i = 0; i < bytes; i++) {
            SgD w = words[0], which = sg_d_lsr(g, y[i], 3);
            for (unsigned k = 1; k < v->aux; k++) w = sg_d_select(g, sg_d_eq(g, which, sgx_const(64, k)), words[k], w);
            SgD at = sg_d_and(g, sg_d_lsl(g, y[i], 3), sgx_const(64, 56));
            SgD byte = sg_d_slice(g, sg_d_shift_dyn(g, w, at, 1), 7, 0);
            SgB inside = less(g, y[i], sgx_const(64, v->aux * 8));
            vput(g, &out, v->d + i, 8, sg_d_select(g, inside, byte, v->op == AV_TBX ? z[i] : sgx_const(8, 0)));
        }
        vflush(g, &out);
        return;
    }
    if (v->op == AV_DUP) {
        SgD val = vlane(g, &in, v->m, width);
        for (unsigned i = 0; i < lanes; i++) vput(g, &out, v->d + i * width / 8, width, val);
        vflush(g, &out);
        return;
    }
    /* the two-destination forms with D = M are UNKNOWN: llvm_vector.inc's zeros, not another emulator's choice */
    if (v->op != AV_REV && v->d == v->m) {
        for (unsigned i = 0; i < bytes; i += 8) vset(g, v->d + i, 64, sgx_const(64, 0));
        return;
    }
    SgD x[16], y[16], r[16], t[16];
    for (unsigned i = 0; i < lanes; i++) {
        x[i] = vlane(g, &in, v->d + i * width / 8, width);
        y[i] = vlane(g, &in, v->m + i * width / 8, width);
    }
    for (unsigned i = 0; i < lanes; i++) {
        if (v->op == AV_REV) {
            unsigned group = v->aux * 8 / width;
            r[i] = y[i / group * group + group - 1 - i % group];
        } else if (v->op == AV_SWP) {
            r[i] = y[i];
            t[i] = x[i];
        } else if (v->op == AV_TRN) {
            r[i] = (i & 1) ? y[i - 1] : x[i];
            t[i] = (i & 1) ? y[i] : x[i + 1];
        } else if (v->op == AV_ZIP) {
            r[i] = (i & 1) ? y[i / 2] : x[i / 2];
            t[i] = (i & 1) ? y[(i + lanes) / 2] : x[(i + lanes) / 2];
        } else {
            unsigned j = (i % (lanes / 2)) * 2;
            r[i] = i < lanes / 2 ? x[j] : y[j];
            t[i] = i < lanes / 2 ? x[j + 1] : y[j + 1];
        }
    }
    for (unsigned i = 0; i < lanes; i++) {
        vput(g, &out, v->d + i * width / 8, width, r[i]);
        if (v->op != AV_REV) vput(g, &out, v->m + i * width / 8, width, t[i]);
    }
    vflush(g, &out);
}
/* the carry-less product of two 64-bit lanes (VMULL.P64), its 128 bits as two words */
static void pmull64(SgCtx* g, SgD x, SgD y, SgD* lo, SgD* hi) {
    *lo = sgx_const(64, 0);
    *hi = sgx_const(64, 0);
    for (unsigned j = 0; j < 64; j++) {
        SgB on = sg_b_not(g, sg_d_is_zero(g, sg_d_slice(g, y, j, j)));
        *lo = sg_d_xor(g, *lo, sg_d_select(g, on, j ? sg_d_lsl(g, x, j) : x, sgx_const(64, 0)));
        if (j) *hi = sg_d_xor(g, *hi, sg_d_select(g, on, sg_d_lsr(g, x, 64 - j), sgx_const(64, 0)));
    }
}
/* one lane of vector_integer's arithmetic: x, y and z the lane's operands (nx, my and out bits), its result's out bits */
static SgD neon_lane(SgCtx* g, const SnxArmVector* v, unsigned width, unsigned out, unsigned nx, SgD x, SgD y, SgD z,
                     SgB* qc) {
    SnxArmVectorOp op = v->op;
    int uns = !!(v->flags & AV_UNSIGNED);
    SgD xx = widen(g, x, uns), yy = widen(g, y, uns), rr;
    switch (op) {
        case AV_AND: return sg_d_and(g, x, y);
        case AV_BIC: return sg_d_and(g, x, sg_d_not(g, y));
        case AV_ORR: return sg_d_or(g, x, y);
        case AV_ORN: return sg_d_or(g, x, sg_d_not(g, y));
        case AV_EOR: return sg_d_xor(g, x, y);
        case AV_BSL: return sg_d_or(g, sg_d_and(g, z, x), sg_d_and(g, sg_d_not(g, z), y));
        case AV_BIT: return sg_d_or(g, sg_d_and(g, y, x), sg_d_and(g, sg_d_not(g, y), z));
        case AV_BIF: return sg_d_or(g, sg_d_and(g, sg_d_not(g, y), x), sg_d_and(g, y, z));
        case AV_ADD:
        case AV_ADDL:
        case AV_ADDW:
        case AV_PADD:
        case AV_PADDL:
        case AV_PADAL:
            rr = sg_d_add(g, xx, yy);
            if (op == AV_PADAL) rr = sg_d_add(g, rr, sg_d_zext(g, z, 64));
            break;
        case AV_SUB:
        case AV_SUBL:
        case AV_SUBW: rr = sg_d_sub(g, xx, yy); break;
        case AV_HADD:
        case AV_RHADD:
        case AV_HSUB:
            rr = op == AV_HSUB ? sg_d_sub(g, xx, yy) : sg_d_add(g, xx, yy);
            if (op == AV_RHADD) rr = sg_d_add(g, rr, sgx_const(64, 1));
            rr = sg_d_asr(g, rr, 1);
            break;
        case AV_QADD:
        case AV_QSUB:
            if (width == 64) return qaddsub64(g, x, y, op == AV_QSUB, uns, qc);
            rr = vsat(g, op == AV_QADD ? sg_d_add(g, xx, yy) : sg_d_sub(g, xx, yy), width, uns, 0, qc);
            break;
        case AV_CGT:
        case AV_CGE:
        case AV_CEQ:
        case AV_TST: {
            SgD a = xx, b = yy;
            if (width == 64 && uns) { /* unsigned order as the signed one of the bits with their tops flipped */
                a = sg_d_xor(g, a, sgx_const(64, 1ull << 63));
                b = sg_d_xor(g, b, sgx_const(64, 1ull << 63));
            }
            SgB c = op == AV_CGT   ? greater(g, a, b)
                    : op == AV_CGE ? at_least(g, a, b)
                    : op == AV_CEQ ? sg_d_eq(g, x, y)
                                   : sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, x, y)));
            return sg_d_select(g, c, sgx_const(width, sg_ones(width)), sgx_const(width, 0));
        }
        case AV_MIN:
        case AV_MAX:
        case AV_PMIN:
        case AV_PMAX: return sg_d_select(g, op == AV_MIN || op == AV_PMIN ? less(g, xx, yy) : greater(g, xx, yy), x, y);
        case AV_ABD:
        case AV_ABA:
        case AV_ABDL:
        case AV_ABAL:
            rr = sg_d_select(g, greater(g, xx, yy), sg_d_sub(g, xx, yy), sg_d_sub(g, yy, xx));
            if (op == AV_ABA || op == AV_ABAL) rr = sg_d_add(g, rr, sg_d_zext(g, z, 64));
            break;
        case AV_MUL:
        case AV_MLA:
        case AV_MLS:
        case AV_MULL:
        case AV_MLAL:
        case AV_MLSL:
            rr = mul64(g, xx, yy);
            if (op == AV_MLA || op == AV_MLAL) rr = sg_d_add(g, sg_d_zext(g, z, 64), rr);
            if (op == AV_MLS || op == AV_MLSL) rr = sg_d_sub(g, sg_d_zext(g, z, 64), rr);
            break;
        case AV_PMUL:
        case AV_PMULL: {
            SgD a = sg_d_zext(g, x, 64);
            rr = sgx_const(64, 0);
            for (unsigned j = 0; j < width; j++) {
                SgB on = sg_b_not(g, sg_d_is_zero(g, sg_d_slice(g, y, j, j)));
                rr = sg_d_xor(g, rr, sg_d_select(g, on, j ? sg_d_lsl(g, a, j) : a, sgx_const(64, 0)));
            }
            break;
        }
        case AV_QDMULL:
        case AV_QDMLAL:
        case AV_QDMLSL:
        case AV_QDMULH:
        case AV_QRDMULH: { /* 2xy (+ 2^(width-1)) >> width as (xy (+ 2^(width-2))) >> (width-1): xy is at most 2^62 */
            SgD p = mul64(g, sg_d_sext(g, x, 64), sg_d_sext(g, y, 64));
            if (op == AV_QDMULH || op == AV_QRDMULH) {
                if (op == AV_QRDMULH) p = sg_d_add(g, p, sgx_const(64, 1ull << (width - 2)));
                rr = vsat(g, sg_d_asr(g, p, width - 1), out, 0, 0, qc);
                break;
            }
            if (out == 64) { /* 2xy past 64 bits only for the two most negative lanes: 2^63 */
                SgB top = sg_d_eq(g, p, sgx_const(64, 1ull << 62));
                *qc = sg_b_or(g, *qc, top);
                rr = sg_d_select(g, top, sgx_const(64, sg_ones(63)), sg_d_lsl(g, p, 1));
                if (op != AV_QDMULL) rr = qaddsub64(g, z, rr, op == AV_QDMLSL, 0, qc);
            } else {
                rr = vsat(g, sg_d_lsl(g, p, 1), out, 0, 0, qc);
                if (op != AV_QDMULL) {
                    SgD acc = sg_d_sext(g, z, 64);
                    rr = vsat(g, op == AV_QDMLAL ? sg_d_add(g, acc, rr) : sg_d_sub(g, acc, rr), out, 0, 0, qc);
                }
            }
            break;
        }
        case AV_ADDHN:
        case AV_SUBHN:
            rr = op == AV_ADDHN ? sg_d_add(g, sg_d_zext(g, x, 64), sg_d_zext(g, y, 64))
                                : sg_d_sub(g, sg_d_zext(g, x, 64), sg_d_zext(g, y, 64));
            if (v->flags & AV_ROUND) rr = sg_d_add(g, rr, sgx_const(64, 1ull << (width - 1)));
            return sg_d_slice(g, rr, nx - 1, width);
        case AV_ABS:
        case AV_NEG:
        case AV_QABS:
        case AV_QNEG: {
            SgD s = sg_d_sext(g, x, 64);
            rr = sg_d_sub(g, sgx_const(64, 0), s);
            if (op == AV_ABS || op == AV_QABS) rr = sg_d_select(g, sign_of(g, s), rr, s);
            if (op == AV_QABS || op == AV_QNEG) rr = vsat(g, rr, width, 0, 0, qc);
            break;
        }
        case AV_NOT: return sg_d_not(g, x);
        case AV_CLZ:
        case AV_CLS: {
            SgD a = op == AV_CLS ? sg_d_sext(g, x, 32) : sg_d_zext(g, x, 32);
            if (op == AV_CLS) a = sg_d_xor(g, a, sg_d_asr(g, a, 31));
            rr = sg_d_sub(g, sg_d_zext(g, sg_i_slice(g, sg_i_clz(g, a), 31, 0), 64),
                          sgx_const(64, 32 - width + (op == AV_CLS)));
            break;
        }
        case AV_CNT: { /* a byte's bits, counted in pairs, nibbles, then the byte */
            SgD a = sg_d_zext(g, x, 32);
            a = sg_d_sub(g, a, sg_d_and(g, sg_d_lsr(g, a, 1), sgx_const(32, 0x55)));
            a = sg_d_add(g, sg_d_and(g, a, sgx_const(32, 0x33)), sg_d_and(g, sg_d_lsr(g, a, 2), sgx_const(32, 0x33)));
            rr = sg_d_zext(g, sg_d_and(g, sg_d_add(g, a, sg_d_lsr(g, a, 4)), sgx_const(32, 0x0f)), 64);
            break;
        }
        case AV_SHL:
        case AV_RSHL:
        case AV_QSHL:
        case AV_QRSHL:
        case AV_QSHLU:
            if (!(v->flags & AV_SCALAR)) {
                int round = op == AV_RSHL || op == AV_QRSHL, saturate = op == AV_QSHL || op == AV_QRSHL;
                return sg_d_slice(g, vshift(g, x, y, width, uns, round, saturate, qc), width - 1, 0);
            }
            if (op == AV_QSHLU) { /* x signed, shifted left by 0 .. width-1, to an unsigned lane */
                unsigned n = (unsigned)v->imm;
                if (width < 64) {
                    rr = vsat(g, n ? sg_d_lsl(g, sg_d_sext(g, x, 64), n) : sg_d_sext(g, x, 64), width, 1, 0, qc);
                    break;
                }
                SgB neg = sign_of(g, x);
                SgB lost = n ? sg_b_not(g, sg_d_is_zero(g, sg_d_lsr(g, x, 64 - n))) : sg_bool(g, 0);
                lost = sg_b_and(g, lost, sg_b_not(g, neg));
                *qc = sg_b_or(g, *qc, sg_b_or(g, neg, lost));
                return sg_d_select(g, neg, sgx_const(64, 0),
                                   sg_d_select(g, lost, sgx_const(64, ~0ull), n ? sg_d_lsl(g, x, n) : x));
            }
            return sg_d_slice(g, vshift(g, x, sgx_const(64, v->imm & 255), width, uns, 0, op == AV_QSHL, qc), width - 1,
                              0);
        case AV_SHR:
        case AV_RSHR:
        case AV_SRA:
        case AV_RSRA:
            rr = shr_round(g, xx, (unsigned)v->imm, uns && width == 64, op == AV_RSHR || op == AV_RSRA);
            if (op == AV_SRA || op == AV_RSRA) rr = sg_d_add(g, rr, sg_d_zext(g, z, 64));
            break;
        case AV_SLI:
        case AV_SRI: {
            unsigned shift = (unsigned)v->imm;
            if (op == AV_SRI && shift == width) return z;
            SgD a = sg_d_zext(g, x, 64), c = sg_d_zext(g, z, 64);
            SnxU64 mask = op == AV_SLI ? (1ull << shift) - 1 : ~(~0ull >> (64 - width + shift));
            rr = op == AV_SLI ? (shift ? sg_d_lsl(g, a, shift) : a) : (shift ? sg_d_lsr(g, a, shift) : a);
            rr = sg_d_or(g, rr, sg_d_and(g, c, sgx_const(64, mask)));
            break;
        }
        case AV_SHLL: rr = v->imm ? sg_d_lsl(g, xx, (unsigned)v->imm) : xx; break;
        case AV_MOVN: return sg_d_slice(g, x, width - 1, 0);
        case AV_QMOVN:
        case AV_QMOVUN:
        case AV_SHRN:
        case AV_QSHRN:
        case AV_QSHRUN: {
            /* x of nx bits, signed for the forms to unsigned of a signed source, else as U says: where nx is 64 and it
               is unsigned, its 64 bits unsigned */
            int signed_x = op == AV_QMOVUN || op == AV_QSHRUN || !uns;
            rr = widen(g, x, !signed_x);
            if (op == AV_SHRN || op == AV_QSHRN || op == AV_QSHRUN)
                rr = shr_round(g, rr, (unsigned)v->imm, !signed_x, !!(v->flags & AV_ROUND));
            if (op != AV_SHRN)
                rr = vsat(g, rr, width, uns || op == AV_QMOVUN || op == AV_QSHRUN, !signed_x && nx == 64, qc);
            break;
        }
        default: sg_fail("an Advanced SIMD integer operation this back end does not know", op, 0);
    }
    return out == 64 ? rr : sg_d_slice(g, rr, out - 1, 0);
}
/* vector_integer: each lane's operands read, then each lane's result written (out of 128 bits: VMULL.P64's two words) */
static void neon_integer(SgCtx* g, const SnxArmVector* v) {
    SnxArmVectorOp op = v->op;
    if (op >= AV_REV && op <= AV_SWP) {
        neon_permute(g, v);
        return;
    }
    unsigned width = v->width, out = width, nx = width, my = width;
    int narrow = op == AV_MOVN || op == AV_QMOVN || op == AV_QMOVUN || op == AV_SHRN || op == AV_QSHRN ||
                 op == AV_QSHRUN || op == AV_ADDHN || op == AV_SUBHN;
    int wide = (op >= AV_ADDL && op <= AV_QDMLSL) || op == AV_PADDL || op == AV_PADAL || op == AV_SHLL;
    if (narrow) nx = my = width * 2;
    if (wide) out = width * 2;
    if (op == AV_ADDW || op == AV_SUBW) nx = out;
    unsigned lanes = v->bytes * 8 / out;
    int unary = (op >= AV_ABS && op <= AV_NOT) || op == AV_MOVN || op == AV_QMOVN || op == AV_QMOVUN || op == AV_SHRN ||
                op == AV_QSHRN || op == AV_QSHRUN || op == AV_SHLL || op == AV_SHR || op == AV_RSHR || op == AV_SRA ||
                op == AV_RSRA || op == AV_SLI || op == AV_SRI ||
                (v->flags & AV_SCALAR && (op == AV_SHL || op == AV_QSHL || op == AV_QSHLU));
    int pair = op == AV_PADD || op == AV_PMIN || op == AV_PMAX || op == AV_PADDL || op == AV_PADAL;
    VRead in = {0};
    VWrite o = {0};
    SgD x[16], y[16], z[16];
    if (lanes > 16) sg_fail("a vector integer instruction of more than 16 lanes", lanes, 16);
    for (unsigned i = 0; i < lanes; i++) {
        if (pair) {
            unsigned base = (op == AV_PADDL || op == AV_PADAL) ? v->m : i < lanes / 2 ? v->n : v->m;
            unsigned j = (op == AV_PADDL || op == AV_PADAL) ? i : i % (lanes / 2);
            x[i] = vlane(g, &in, base + j * 2 * width / 8, width);
            y[i] = vlane(g, &in, base + (j * 2 + 1) * width / 8, width);
        } else {
            x[i] = vlane(g, &in, (unary ? v->m : v->n) + i * nx / 8, nx);
            y[i] = vlane(g, &in, v->m + (v->flags & AV_SCALAR ? 0 : i * my / 8), my);
        }
        if (v->flags & AV_ZERO) {
            y[i] = sgx_const(width, 0);
            if (v->flags & AV_REVERSE) {
                y[i] = x[i];
                x[i] = sgx_const(width, 0);
            }
        }
        z[i] = out == 128 ? sgx_const(64, 0) : vlane(g, &in, v->d + i * out / 8, out);
    }
    SgB qc = sg_bool(g, 0);
    for (unsigned i = 0; i < lanes; i++) {
        if (out == 128) {
            SgD lo, hi;
            pmull64(g, x[i], y[i], &lo, &hi);
            vput(g, &o, v->d, 64, lo);
            vput(g, &o, v->d + 8, 64, hi);
        } else
            vput(g, &o, v->d + i * out / 8, out, neon_lane(g, v, width, out, nx, x[i], y[i], z[i], &qc));
    }
    vflush(g, &o);
    neon_qc(g, qc);
}
/* vector_struct: VLD1-4 and VST1-4, whole registers, one lane, or one element to every lane; one check of the bytes and
   their alignment first, as llvm.c's bounds */
static void neon_struct(SgCtx* g, const SnxArmVector* v) {
    unsigned eb = v->width / 8, lanes = 8 / eb, address = 0;
    SgD base = reg(g, v->n);
    SgD offset = v->m == 13 ? sgx_const(32, v->bytes) : reg(g, v->m);
    sgx_reg(sgx(g), &base);
    sgx_a32_bounds(g, base, v->bytes, v->alignment);
    VRead in = {0};
    VWrite o = {0};
    for (unsigned gr = 0; gr < v->groups; gr++) {
        unsigned elements = v->flags & (AV_LANE | AV_REPLICATE) ? 1 : lanes;
        for (unsigned e = 0; e < elements; e++)
            for (unsigned s = 0; s < v->count; s++) {
                unsigned d = v->d + (gr + s * v->stride) * 8 + (v->flags & AV_LANE ? v->lane : e) * eb;
                unsigned at = v->flags & AV_REPLICATE ? s * eb : address;
                if (v->flags & AV_LOAD) {
                    SgD x = sgx_a32_load(g, base, (int)eb, (int)at);
                    if (v->flags & AV_REPLICATE)
                        for (unsigned k = 0; k < lanes; k++) vput(g, &o, d + k * eb, v->width, x);
                    else
                        vput(g, &o, d, v->width, x);
                } else
                    sgx_a32_store(g, base, (int)eb, (int)at, vlane(g, &in, d, v->width));
                address += eb;
            }
    }
    if (v->flags & AV_LOAD)
        vflush(g, &o);
    else
        sgx_a32_written(g, base, v->bytes);
    if (v->flags & AV_WB) sg_GPR_write(g, v->n, sg_d_add(g, base, offset));
}
/* VMOV and VMVN of an immediate (aux 0), VORR and VBIC of one (aux 1 and 2), each lane */
static void neon_imm(SgCtx* g, const SnxArmVector* v) {
    VRead in = {0};
    VWrite o = {0};
    for (unsigned i = 0; i < v->bytes; i += v->width / 8) {
        SgD k = sgx_const(v->width, v->imm);
        if (v->aux)
            k = v->aux == 1 ? sg_d_or(g, vlane(g, &in, v->d + i, v->width), k)
                            : sg_d_and(g, vlane(g, &in, v->d + i, v->width), sgx_const(v->width, ~v->imm));
        vput(g, &o, v->d + i, v->width, k);
    }
    vflush(g, &o);
}
/* AESE, AESD, AESMC, AESIMC, the SHA-1 and SHA-256 rounds: snx_arm_crypto on the D bank, as llvm_vector.inc's */
static void neon_crypto(SgCtx* g, const SnxArmVector* v) {
    SgD args[4] = {sgx_const(32, v->d), sgx_const(32, v->n), sgx_const(32, v->m), sgx_const(32, v->aux)};
    sgx_a32_commit(g);
    sgx_a32_call(g, "snx_arm_crypto", args, 4, 0);
}

/* FPSCR's bits the ARM11's VFP data processing stops at, for the backend (llvm_vector.inc's vector_instruction): an
   enabled exception, and a short vector (LEN, STRIDE) but for a compare or a conversion, which are scalar */
static void arm11_stops(SgCtx* g, const SnxArmVector* v) {
    int scalar = v->op == AV_FCMP || v->op == AV_FCMPE || v->op == AV_FCVT || v->op == AV_FTOI || v->op == AV_ITOF;
    SnxU32 stop = SNX_ARM_FPSCR_TRAP_ENABLES | (scalar ? 0 : SNX_ARM_FPSCR_LEN_STRIDE);
    if (v->op == AV_MEM || v->op == AV_CORE || v->op == AV_CORE_PAIR || v->op == AV_STATUS) return;
    sgx_a32_trap_on(g, sg_b_not(g, sg_d_is_zero(g, sg_d_and(g, vbits(g, FPSCR_OFF, 32), sgx_const(32, stop)))),
                    SNX_ARM_BACKEND_UNSUPPORTED);
}
/* a VFP or Advanced SIMD word (llvm_vector.inc's vector_instruction) */
static void vfp(SgCtx* g) {
    SnxArmVector v = {0};
    condition(g);
    if (!snx_arm_vector_decode(g->word, &v)) {
        sg_undefined(g);
        return;
    }
    if (g->profile == SNX_ARM_PROFILE_ARM11) {
        /* odd double-register multiple counts are legacy FSTMX/FLDMX forms with an extra format word */
        int multiple = !(v.flags & AV_PRE) || (v.flags & AV_WB);
        if (v.op == AV_MEM && (v.bytes > 128 || (multiple && v.width == 64 && (v.imm & 4)))) {
            sgx_a32_trap(g, SNX_ARM_BACKEND_UNSUPPORTED);
            return;
        }
        arm11_stops(g, &v);
    }
    if (v.op == AV_CRYPTO)
        neon_crypto(g, &v);
    else if (v.op == AV_MEM)
        vfp_memory(g, &v);
    else if (v.op == AV_STRUCT)
        neon_struct(g, &v);
    else if (v.op == AV_CORE || v.op == AV_CORE_PAIR || v.op == AV_STATUS || (v.op == AV_DUP && !(v.flags & AV_SIMD)))
        vfp_transfer(g, &v);
    else if (v.op == AV_IMM)
        neon_imm(g, &v);
    else if (v.op == AV_MOV || (v.op >= AV_FADD && v.op <= AV_FSELECT) || v.op == AV_URECPE || v.op == AV_URSQRTE) {
        if (v.flags & AV_SIMD)
            neon_fp(g, &v);
        else
            vfp_data(g, &v);
    } else
        neon_integer(g, &v);
}

/* llvm.c's instruction() for the kinds the Sail's clauses don't take: 1 where the block ends after it, 0 where it
   goes on; a kind this back end doesn't make refuses the block */
static int instruction(SgCtx* g, SnxA32Kind kind) {
    SnxU32 w = g->word;
    if (sgx_a32_checked(g) &&
        (kind == SNX_A32_ACQUIRE_RELEASE ||
         ((kind == SNX_A32_VECTOR || kind == SNX_A32_EXCLUSIVE) && g->profile != SNX_ARM_PROFILE_ARM11))) {
        condition(g);
        sgx_a32_trap(g, SNX_ARM_BACKEND_UNSUPPORTED);
        return 1;
    }
    switch (kind) {
        case SNX_A32_BX: { /* BLX of a register on the ARM7TDMI, whose decoder runs it and the Sail has from ARMv5 on */
            condition(g);
            SgD target = reg(g, w & 15); /* it may be LR: read before the return address is written */
            branch_exchange(g, target);
            if (w & 0x20) sg_GPR_write(g, 14, sgx_const(32, (SnxU32)g->pc + 4));
            return 1;
        }
        case SNX_A32_SINGLE:
        case SNX_A32_EXTRA: return memory(g, kind);
        case SNX_A32_MULTIPLE: return multiple(g);
        case SNX_A32_MUL:
        case SNX_A32_MULL:
        case SNX_A32_MLS: multiply(g, kind); return 0;
        case SNX_A32_DIV: divide(g); return 0;
        case SNX_A32_CLZ:
        case SNX_A32_REV:
        case SNX_A32_EXTEND:
        case SNX_A32_BITFIELD:
        case SNX_A32_MOVW:
        case SNX_A32_MOVT:
        case SNX_A32_MRS:
        case SNX_A32_MSR: return miscellaneous(g, kind);
        case SNX_A32_EXCLUSIVE:
            if (sgx_a32_checked(g))
                checked_exclusive(g);
            else
                exclusive(g);
            return 0;
        case SNX_A32_ACQUIRE_RELEASE: acquire_release(g); return 0;
        case SNX_A32_CLREX:
            condition(g);
            sgx_a32_put(g, offsetof(SnxArmCPU, exclusive_valid), 32, sgx_const(32, 0));
            return 0;
        case SNX_A32_BARRIER: /* stronger than each valid DMB and DSB scope, never just a compiler barrier */
            condition(g);
            sgx_a32_machine(g, SGX_A32_FENCE, 0);
            return 0;
        case SNX_A32_HINT: /* hints may complete without waiting; a trace's bit for one skipped is its condition's */
            if (g->skipped) condition(g);
            return 0;
        case SNX_A32_UADD8: /* the ARM11's, the A57's decoder's PARALLEL */
        case SNX_A32_PARALLEL: parallel(g); return 0;
        case SNX_A32_SEL: media(g, SNX_A32_SELECT); return 0;    /* the ARM11's */
        case SNX_A32_MULH: media(g, SNX_A32_HALF_MUL); return 0; /* the ARM11's: the same words */
        case SNX_A32_SATURATE:
        case SNX_A32_PACK:
        case SNX_A32_SELECT:
        case SNX_A32_QARITH:
        case SNX_A32_HALF_MUL:
        case SNX_A32_DUAL_MUL:
        case SNX_A32_ABS_DIFF:
        case SNX_A32_CRC32: media(g, kind); return 0;
        case SNX_A32_SETEND: return setend(g);
        case SNX_A32_VECTOR: vfp(g); return 0;
        case SNX_A32_COPROC: { /* the backend's (SnxArmRt.coproc): it reads or writes the registers, or stops here */
            SgD word = sgx_const(32, w);
            condition(g);
            sgx_a32_pc(g, sgx_const(32, (SnxU32)g->pc));
            sgx_a32_call(g, "snx_arm_coproc", &word, 1, 0);
            sgx_a32_machine(g, SGX_A32_CHECK, 1); /* the block returns where the backend stopped */
            return 0;
        }
        case SNX_A32_SVC:
        case SNX_A32_BKPT:
            condition(g);
            sg_exception(g, kind == SNX_A32_SVC ? SNX_ARM_SVC : SNX_ARM_BREAKPOINT,
                         sgx_const(32, kind == SNX_A32_SVC ? w & 0xffffff : ((w >> 4) & 0xfff0) | (w & 15)));
            return 1;
        case SNX_A32_UNDEFINED:
            condition(g);
            sg_undefined(g);
            return 1;
        default: sg_fail("an A32 instruction this back end does not make", kind, 0);
    }
}

/* ---- the block ---- */

static void name_of(char* out, size_t size, SnxU32 key) {
    snprintf(out, size, "%c_%08x", key & 1 ? 'T' : 'A', (unsigned)(key & ~1u));
}

/* the code's word i (little-endian) */
static SnxU32 word_at(const unsigned char* code, size_t i) {
    const unsigned char* p = code + 4 * i;
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}

/* the block's way past an instruction its condition skipped, bound where the two meet: with a trace (the ARM7TDMI's)
   the skipped instruction's bit first, which the way that ran jumps over where it comes here (on) */
static void bind_skip(MG_ARM* c, int on) {
    SgCtx* g = &c->g;
    SgXBuf* b = &c->b;
    int over = -1;
    flush(c);
    if (!g->skipped || g->thumb || !snx_arm_timed_condition(g->word)) {
        sgx_a32_bind(g, b, g->skip);
        g->skip = -1;
        return;
    }
    if (on) {
        over = sgx_a32_label(g);
        sgx_a32_jmp(g, b, over);
    }
    sgx_a32_bind(g, b, g->skip);
    g->skip = -1;
    SnxU32 i = ((SnxU32)g->pc - g->block_pc) >> 2;
    size_t at = g->skipped + (size_t)(i >> 5) * 4;
    SgXIns* l = sgx_append(&g->x, SGX_K_LOAD);
    l->w = 32;
    l->disp = (int)at;
    l->def = sgx_vreg(&g->x, g->x.n - 1, 32);
    sgx_a32_put(g, at, 32, sg_d_or(g, sgx_val(32, l->def), sgx_const(32, 1u << (i & 31))));
    flush(c);
    if (on) sgx_a32_bind(g, b, over);
}
/* the A32 words of the block, until one ends it: how many (a Thumb block's halfwords are thumb_code's) */
static size_t arm_code(MG_ARM* c, const unsigned char* code, size_t count, SnxU32 pc) {
    SgCtx* g = &c->g;
    SgX* x = &g->x;
    for (size_t i = 0;; ++i) {
        SnxU32 w = word_at(code, i);
        if (x->n > SGX_MAXI - SGX_MAXI / 4 || x->nv > SGX_MAXV - SGX_MAXV / 4 || x->nb > SGX_MAXB - SGX_MAXB / 4)
            flush(c);
        g->word = w;
        g->pc = pc + 4 * (SnxU32)i;
        g->next = (SnxU32)g->pc + 4;
        g->branched = g->undefined = 0;
        g->iset = 0;
        g->skip = -1;
        g->when = 0;
        g->yields = 0;
        sgx_a32_fetch(g, w, 4);
        g->start = x->n;
        /* the Sail's clauses' (their condition and their end their own), else the emitters here */
        int stop = sg_decode_a32(g) ? g->branched || (g->undefined && g->skip < 0)
                                    : instruction(g, snx_a32_decode_profile(g->profile, w));
        if (x->unmade) sg_fail("a primitive this host has no form of", 0, 0);
        sgx_a32_finish(g);
        if (!stop) sgx_a32_yield_check(g);
        if (stop || i + 1 == count) return i + 1;
        if (g->skip >= 0) bind_skip(c, 1);
    }
}
#include "common/cpu/recompiler/mach/mach_thumb.inc"

/* checked memory's entry (llvm.c's memory_entry): the state the bus's, of this ABI */
static void checked_entry(SgCtx* g) {
    SgX* x = sgx(g);
    static const struct {
        size_t at;
        unsigned w;
    } field[2] = {{offsetof(SnxArmCheckedCPU, abi), 32}, {offsetof(SnxArmCheckedCPU, access), 64}};
    for (int k = 0; k < 2; ++k) {
        SgXIns* l = sgx_append(x, SGX_K_LOAD);
        l->w = (unsigned char)field[k].w;
        l->disp = (int)field[k].at;
        l->def = sgx_vreg(x, x->n - 1, field[k].w);
        SgD v = sgx_val(field[k].w, l->def);
        SgB bad = k ? sg_d_is_zero(g, v) : sg_b_not(g, sg_d_eq(g, v, sgx_const(32, SNX_ARM_CHECKED_ABI)));
        sgx_a32_exit(g, bad, SNX_ARM_BAD_ENTRY, sgx_const(32, SNX_ARM_CHECKED_ABI), 1);
    }
}
/* one block into c->b, A32 or (thumb) Thumb: its size, 0 where refused (t_mach_arm.why says why) */
static size_t block(MG_ARM* c, const unsigned char* code, size_t count, SnxU32 pc, int thumb, size_t* consumed,
                    MG_ARM_NEXT succ, void* user) {
    SgCtx* g = &c->g;
    SgX* x = &g->x;
    SgXBuf* b = &c->b;
    jmp_buf jb;
    jmp_buf* saved = t_mach_arm.fail;
    int bad = 0;
    unsigned itstate = 0; /* ITSTATE after a Thumb block's last instruction */
    size_t units;
    t_mach_arm.why = NULL;
    t_mach_arm.fail = &jb;
    if (setjmp(jb)) {
        t_mach_arm.fail = saved;
        return 0;
    }
    b->n = 0;
    b->disp = 0;
    b->index = -1;
    c->nrel = 0;
    /* the assembler's state of a function (its buffers kept) */
    c->a.nframe = 0;
    c->a.slots_max = 0;
    c->a.cold.n = 0;
    c->a.ncjumps = c->a.nccalls = c->a.npool = c->a.nrips = 0;
    c->a.x = x;
    c->a.b = b;
    c->a.machine = g;
    c->a.reloc = unit_reloc;
    g->nlabels = g->nfix = g->nexits = g->nmonitors = g->nouts = g->nbuses = 0;
    g->profile = c->options.profile;
    g->memory = c->options.memory;
    g->trace = c->options.trace;
    g->skipped = c->options.skipped;
    g->block_pc = pc;
    g->yields = 0;
    x->n = 0;
    x->nv = 1;
    x->nb = 1;
    x->features = c->features;
    x->flags_ins = -1;
    x->base = SGX_BASE;
    x->pool = c->pool;
    x->unmade = 0;
    g->pending = 0;
    g->origin = -1;
    g->skip = -1;
    g->when = 0;
    g->group = 0;
    g->deferred = 0;
    g->thumb = thumb;
    g->itstate = g->it_after = g->it_next = 0;
    g->it_set = 0;
    g->pair = 0;
    g->offset = 0;
    g->ret = sgx_a32_label(g);
    flush(c);
    g->e_written = 0;
    mg_prologue(c, pc, &bad);
    if (sgx_a32_checked(g)) checked_entry(g);
    units = thumb ? thumb_code(c, code, count, pc, &itstate) : arm_code(c, code, count, pc);
    *consumed = units;
    SnxU32 nexts[2];
    size_t nnext = succ ? succ(user, pc | (SnxU32)thumb, units, count, nexts) : 0;
    /* a Thumb block that ends inside an IT block: its ITSTATE left in CPSR, which the next block's entry looks at (so
       the jump there goes through its checks) */
    SnxU32 it = itstate ? sg_it_bits(itstate) : 0;
    if (it && sgx_a32_hosted(g)) nnext = 0; /* its dispatch loop's, which looks at ITSTATE */
    /* the end: the last instruction's way (a branch's target, the backend's exit, or on), then, past its condition's
       skip, the next instruction's */
    if (g->branched) {
        SgD t = g->target;
        int tconst = sgx_is_const(&t), to_thumb = thumb;
        if (g->pending >> SG_PSTATE_T & 1) {
            SgD f = g->field[SG_PSTATE_T];
            if (sgx_is_const(&f))
                to_thumb = (int)(f.k & 1);
            else
                tconst = 0;
        }
        if (!tconst) sgx_a32_pc(g, t); /* a known target's R15 go_on's */
        if (it) cpsr_bits(g, 0, sgx_const(32, it));
        flush(c);
        if (tconst)
            mg_go_on(c, (SnxU32)t.k | (SnxU32)to_thumb, pc, nexts, nnext, it != 0);
        else
            sgx_a32_jmp(g, b, g->ret);
    } else if (!g->undefined) {
        /* on, both ways past the skip */
        if (g->skip >= 0)
            bind_skip(c, 1);
        else
            flush(c);
        if (it) {
            cpsr_bits(g, 0, sgx_const(32, it));
            flush(c);
        }
        mg_go_on(c, g->next | (SnxU32)thumb, pc, nexts, nnext, g->e_written || it);
    } else
        flush(c); /* its own code left */
    if (g->skip >= 0) {
        bind_skip(c, 0);
        if (it) {
            cpsr_bits(g, 0, sgx_const(32, it));
            flush(c);
        }
        mg_go_on(c, g->next | (SnxU32)thumb, pc, nexts, nnext, it != 0);
    }
    sgx_a32_stubs(g, b);
    cold_ways(c);
    const SnxU32 first = thumb ? half_at(code, 0) : word_at(code, 0);
    const int entered = !sgx_a32_hosted(g); /* by way of its checks, one of which may fail */
    if (entered && !MG_ARM_EXIT_LAST(c)) mg_entry_exit(c, pc, first, bad);
    sgx_a32_bind(g, b, g->ret);
    mg_return(c);
    sgx_cold_emit(&c->a);
    if (entered && MG_ARM_EXIT_LAST(c)) mg_entry_exit(c, pc, first, bad);
    sgx_pool_emit(&c->a);
    if (mg_patch_frame(c)) {
        t_mach_arm.fail = saved;
        ++c->big; /* made again with 32-bit frame sizes (x86-64's) */
        return (size_t)-1;
    }
    if (sgx_a32_patch(g, b)) sg_fail("a jump to a label never bound", 0, 0);
    t_mach_arm.fail = saved;
    return b->n;
}

/* a block's code (size bytes), relocations and record kept for the object: 1, or 0 out of memory */
static int keep(MG_ARM* c, const unsigned char* code, size_t size, const Rel* rel, size_t nrel, SnxU32 pc,
                const char* name, size_t own) {
    if (c->ntext + size > c->cap_text) {
        size_t cap = c->cap_text ? c->cap_text * 2 : (size_t)1 << 20;
        while (cap < c->ntext + size) cap *= 2;
        unsigned char* p = (unsigned char*)realloc(c->text, cap);
        if (!p) return 0;
        c->text = p, c->cap_text = cap;
    }
    if (c->nrels + nrel > c->cap_rels) {
        size_t cap = c->cap_rels ? c->cap_rels * 2 : 4096;
        while (cap < c->nrels + nrel) cap *= 2;
        Rel* p = (Rel*)realloc(c->rels, cap * sizeof *p);
        if (!p) return 0;
        c->rels = p, c->cap_rels = cap;
    }
    if (c->nmade == c->cap_made) {
        size_t cap = c->cap_made ? c->cap_made * 2 : 1024;
        Made* p = (Made*)realloc(c->made, cap * sizeof *p);
        if (!p) return 0;
        c->made = p, c->cap_made = cap;
    }
    Made* m = &c->made[c->nmade++];
    m->pc = pc;
    snprintf(m->name, sizeof m->name, "%s", name);
    m->at = c->ntext;
    m->size = size;
    m->rel0 = c->nrels;
    m->nrel = nrel;
    m->own = own;
    memcpy(c->text + c->ntext, code, size);
    c->ntext += size;
    if (nrel) memcpy(c->rels + c->nrels, rel, nrel * sizeof *rel);
    c->nrels += nrel;
    return 1;
}

/* a block of either state, kept for the object as name (NULL: its key's) */
static int make(MG_ARM* c, const char* name, const unsigned char* code, size_t count, SnxU32 pc, int thumb,
                size_t* consumed, MG_ARM_NEXT next, void* user) {
    *consumed = 0;
    c->why = NULL;
    if (!count || count > 65536 || (pc & (thumb ? 1 : 3))) {
        c->why = "a block of no words, too many, or at a misaligned address";
        return 0;
    }
    c->big = 0;
    size_t size;
    for (;;) {
        size = block(c, code, count, pc, thumb, consumed, next, user);
        if (size == (size_t)-1) continue; /* its frame passes an 8-bit immediate: made again with 32-bit ones */
        if (!size) {
            c->why = t_mach_arm.why ? t_mach_arm.why : "unknown";
            *consumed = 0;
            return 0;
        }
        if (size <= c->b.cap) break;
        /* past the buffer: grown, and made again */
        c->b.cap = size * 2;
        c->b.p = (unsigned char*)realloc(c->b.p, c->b.cap);
        if (!c->b.p) {
            c->b.cap = 0;
            c->why = "out of memory";
            return 0;
        }
    }
    char own[24];
    if (!name) name_of(own, sizeof own, pc | (SnxU32)thumb), name = own;
    if (strlen(name) >= sizeof c->made[0].name) {
        c->why = "a block's name too long";
        return 0;
    }
    /* kept: its code, its relocations, its record */
    if (!keep(c, c->b.p, size, c->rel, (size_t)c->nrel, pc, name, c->own)) {
        c->why = "out of memory";
        return 0;
    }
    return 1;
}
int MG_ARM_API(block)(MG_ARM* c, const char* name, const unsigned char* code, size_t count, SnxU32 pc,
                      size_t* consumed, MG_ARM_NEXT next, void* user) {
    return make(c, name, code, count, pc, 0, consumed, next, user);
}
int MG_ARM_API(thumb_block)(MG_ARM* c, const char* name, const unsigned char* code, size_t count, SnxU32 pc,
                            size_t* consumed, MG_ARM_NEXT next, void* user) {
    return make(c, name, code, count, pc, 1, consumed, next, user);
}

static int by_address(const void* a, const void* b) {
    const Made *x = (const Made*)a, *y = (const Made*)b;
    if (x->pc != y->pc) return x->pc < y->pc ? -1 : 1;
    return strcmp(x->name, y->name);
}
int MG_ARM_API(object)(MG_ARM* c, unsigned char** object, size_t* size, size_t* blocks) {
    SnxXElf* o = snx_xelf_new();
    *object = NULL;
    *size = 0;
    if (!o) return -1;
    snx_xelf_set_machine(o, MG_ARM_EM);
    snx_xelf_set_flags(o, MG_ARM_ELF_FLAGS);
#ifdef MG_ARM_COFF
    snx_xelf_set_coff(o, MG_ARM_COFF);
#endif
#ifdef MG_ARM_ELF_BIG
    snx_xelf_set_big(o, MG_ARM_ELF_BIG);
#endif
    mg_entry_exit_fn(o);
    qsort(c->made, c->nmade, sizeof *c->made, by_address);
    for (size_t i = 0; i < c->nmade; ++i) {
        const Made* m = &c->made[i];
        size_t at = snx_xelf_put(o, SNX_ELF_TEXT, c->text + m->at, m->size, 16);
        /* a block's name the descriptor and LLVM's blocks call (global); an entry the object's own (X_, local) */
        snx_xelf_symbol(o, m->name, SNX_ELF_TEXT, at, m->size,
                        m->own == (size_t)-1 && m->name[0] == 'X' ? SNX_ELF_LOCAL | SNX_ELF_FUNC
                                                                  : SNX_ELF_GLOBAL | SNX_ELF_HIDDEN | SNX_ELF_FUNC);
#ifdef MG_ARM_UNWIND
        MG_ARM_UNWIND(o, at, c->text + m->at, m->size, m->own, c->rels + m->rel0, m->nrel);
#endif
        if (m->own != (size_t)-1) {
            char own[sizeof m->name + 1];
            snprintf(own, sizeof own, "X%s", m->name);
            /* the object's own: only its blocks jump there */
            snx_xelf_symbol(o, own, SNX_ELF_TEXT, at + m->own, m->size - m->own, SNX_ELF_LOCAL | SNX_ELF_FUNC);
        }
        for (size_t k = m->rel0; k < m->rel0 + m->nrel; ++k)
            snx_xelf_reloc(o, SNX_ELF_TEXT, at + c->rels[k].at, c->rels[k].sym, c->rels[k].type, c->rels[k].addend);
    }
    if (blocks) *blocks = c->nmade;
    /* the blocks' code given back before the object is written: it has them */
    free(c->text), free(c->rels), free(c->made);
    c->text = NULL, c->rels = NULL, c->made = NULL;
    c->ntext = c->cap_text = c->nrels = c->cap_rels = c->nmade = c->cap_made = 0;
    int rc = snx_xelf_write(o, object, size);
    snx_xelf_free(o);
    return rc ? -1 : 0;
}
int MG_ARM_API(absorb)(MG_ARM* c, MG_ARM* from) {
    for (size_t i = 0; i < from->nmade; ++i) {
        const Made* m = &from->made[i];
        if (!keep(c, from->text + m->at, m->size, from->rels + m->rel0, m->nrel, m->pc, m->name, m->own)) return -1;
    }
    free(from->text), free(from->rels), free(from->made);
    from->text = NULL, from->rels = NULL, from->made = NULL;
    from->ntext = from->cap_text = from->nrels = from->cap_rels = from->nmade = from->cap_made = 0;
    return 0;
}

