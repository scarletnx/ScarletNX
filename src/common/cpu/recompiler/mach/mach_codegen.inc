/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native back ends' guest functions, what any machine's shares: a guest function's code. Included by a machine's
   code generator (x64/x64_codegen.c, arm64/arm64_codegen.c) after sailgen's emitters of that machine
   (a64_<machine>.inc), and followed by the machine's glue, which it declares below: the bytes of the ABI and the flow
   (the prologue, the jumps and their fixups, the calls, the entry's checks, the dispatch block, SVC, BRK and UNDEF, the
   pins' loads and stores) and the relocations of its objects. The machine also gives its AdvSIMD instructions (vec()
   and the vector helpers the structures' loads and stores take: vread, vwrite, vzero, lane_get, lane_put, vdup), and
   these constants: MG_EM (its ELF machine; MG_ELF_FLAGS, its objects' e_flags, the host ABI's, 0 by default),
   MG_R_ABS64 (an absolute doubleword's relocation), SGX_FN_TARGET (the register a call's target and a function's entry
   are in), the pins' host registers (mg_pin_hosts, MG_NPINS, MG_NPINS_FIXED where an exclusive, an atomic or CRC32
   takes the last ones, MG_PIN_RESERVED(h) whether a pin's register is one values may be in), the vector pins'
   (MG_VPIN(k), the k-th, MG_NVPINS) and the vector registers no value may take (MG_XRESERVED: the callee's).
   MG_FAR_JUMPS where its jumps have a longer form (Unit.far), MG_POOL_REACH how far the code goes past the first LDR of
   a literal before the literals are put in it (mg_pool_island), MG_DIRECT_ENTRY where a function's entry checks its
   target before the frame, so that a call of its start comes in past the check (mg_direct): how far back a call reaches
   one so with no relocation's help.

   Each basic block's instructions are sailgen's emitters', appended to one SSA region (registers read once, flags
   reused, constants folded) and assembled when the block ends. The guest's registers are in the CPU structure at
   every block's start and end (or in their pins), so a block needs nothing of another's and a call nothing of its
   caller's */

#ifndef MG_ELF_FLAGS
#define MG_ELF_FLAGS 0
#endif
/* the object's byte order: the machine's (MG_ELF_BIG 1 for a big-endian one), its tables swapped where it isn't this
   host's (snx_elf_byteswap) */
#ifndef MG_ELF_BIG
#define MG_ELF_BIG 0
#endif
/* a relocation's r_info of a symbol and a type: the ELF64 one's, or the machine's (MIPS64's n64 layout) */
#ifndef MG_R_INFO
#define MG_R_INFO(sym, type) ELF64_R_INFO((sym), (type))
#endif

/* ---- the unit: its code, labels, relocations, symbols ---- */

typedef struct {
    size_t at;   /* the jump's place (x86's rel32, arm64's word) */
    int label;
    int kind;    /* the machine's: which field of the word the offset goes in */
} Fixup;
typedef struct {
    size_t at;
    char* sym;
    long long addend;
    unsigned type; /* the machine's relocation (R_X86_64_PLT32, R_AARCH64_CALL26...) */
} Reloc;
typedef struct {
    char* name;
    size_t at, size;
    size_t entry; /* a call of the function's start comes in this far past at, its entry's check behind it */
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
    const SnxNativeOptions* options;
    const SnxFunction* f;
    int pinned;          /* the function keeps guest registers in host ones (pin): the callee's saved */
    SnxU64* live;        /* each instruction's live-in guest registers (bits 0-31) and flags (32-35), or NULL */
    SnxU64* live_buf;    /* live's room */
    SnxU32* vlive;       /* each instruction's live-in V registers, with live */
    SnxU32* vlive_buf;
    size_t cap_vlive;
    unsigned char* exits; /* each instruction's: a loop's back edge whose fall-through leaves that loop, or NULL */
    unsigned char* exits_buf;
    size_t cap_exits;
    size_t cap_live;
    signed char pin[32]; /* each X register's host register, -1 for none */
    signed char vpin[32]; /* each V register's vector register, -1 for none */
    signed char mempin;   /* the guest memory's base's host register, -1 for none */
    SnxU64 pin_wr;        /* the X registers (31 the SP) some instruction of the function may write: a pin of none of
                             them is never stored, the state holding its value */
    SnxU32 vpin_wr;       /* the same of the V registers */
    SnxU64 pin_sel;       /* the pins a load of them reloads (the prologue's: those live where the function starts) */
    SnxU32 vpin_sel;
    SnxU64* dsync;        /* each instruction's X registers a store of the pins there stores: those it may write and
                             those an instruction before it may have, with no call (which syncs them) between */
    SnxU32* vdsync;
    size_t cap_dsync, cap_vdsync;
    SnxU64 cur_dirty;     /* the pins a store of them stores now: the instruction's dsync, 0 past a call */
    SnxU32 vcur_dirty;
    size_t index; /* the function's in the codegen: its snx_stale byte */
    size_t at;    /* the index of the function's instruction being made, its count past them */
    int far;      /* the function's jumps are the machine's longest (MG_FAR_JUMPS): its short ones did not reach */
    /* the addresses of the function's instructions there is no code for, undefined where they run */
    SnxU64* none;
    size_t nnone, cap_none;
    int again;    /* the machine's glue refused the function to make it again its way (mg_finish), once */
    /* the function calls a helper that computes with the host's floating point (MG_HOST_FP_HELPER names them), so its
       entry puts the host's FP environment back: fp_entry, which the machine's mg_finish sets where the function it
       made has host_fp and no check, to make it again with one (the function with no floating point has none) */
    int host_fp, fp_entry;
    int dispatch, d_label, mismatch_label;
    int start_label; /* the function's first word, its entry for any target */
#ifdef MG_DIRECT_ENTRY
    size_t first;   /* the unit's first function in the codegen */
    size_t* sym_of; /* each function of the unit's made already: its symbol's index + 1, 0 where none */
#endif
    int nstubs, cap_stubs;
    /* the function's BRs through a recovered jump table (MG_JTAB): each one's table, its label, the guest address of its
       first slot and its slots (a word each), whose labels are jslots[first...] (the dispatch block's where no case) */
    struct JTab {
        int label;
        SnxU64 lo;
        unsigned n, first;
    } * jtabs;
    int njtabs;
    size_t cap_jtabs;
    int* jslots;
    size_t njslots, cap_jslots;
    struct Stub {
        int label;
        int kind; /* 0 a branch to target; 1 a poll then target (resume); 2 a return (leave); 3 a pending poll
                     (poll_slow) */
        SnxU64 target;
        SnxU64 address;
        SnxU64 dirty; /* cur_dirty where it was made */
        SnxU32 vdirty;
    } * stubs;
} Unit;

static void* grow(void* p, size_t* cap, size_t need, size_t each) {
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : 64;
    if (*cap < need) *cap = need;
    p = realloc(p, *cap * each);
    if (!p) {
        fprintf(stderr, "native back end: out of memory\n");
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


/* ---- the machine's glue (defined after this file, by the machine's code generator) ---- */

/* a jump to label, and one on the host's condition code cc */
static void jmp(Unit* u, int label);
static void jcc(Unit* u, int cc, int label);
/* the pinned guest registers to the state (store) or from it, every one or (caller) those a call changes */
static void pins_of(Unit* u, SgXBuf* b, int store, int caller);
static void prologue(Unit* u);
static void epilogue(Unit* u);
/* the pins stored, the epilogue, the return */
static void leave(Unit* u);
/* CALLDYN of the target in SGX_FN_TARGET: the dispatch table's function, else the runtime's miss; tail: then return */
static void calldyn(Unit* u, int tail);
/* the target in SGX_FN_TARGET, as a call's or a function's entry */
static void set_target(Unit* u, SnxU64 target);
/* a call of the function sym (a relocation), its target in SGX_FN_TARGET; tail: the epilogue and a jump */
static void call_sym(Unit* u, const char* sym, int tail);
/* if (c->pc != next) return (and where the function's code went stale) */
static void check_return(Unit* u, SnxU64 next);
/* the poll of a backward branch: if (c->poll) { c->pc = resume; c->rt->poll(c); F_start(c, resume); return; } */
static void poll_then(Unit* u, SnxU64 resume);
#ifdef MG_POLL_INLINE
/* the poll split for a backward branch laid out with one taken jump a pass of the loop: a jump to label where no poll
   is pending (it may change the host's flags), and the pending poll's call alone, out of line */
static void poll_jz(Unit* u, int label);
static void poll_slow(Unit* u, SnxU64 resume);
#endif
#ifdef MG_JTAB
/* a BR's jump through table k (the target in SGX_FN_TARGET): to the case's label, else (a poll pending, the target
   no case of it) to the dispatch block, CPU.pc stored already; and the tables' words at the function's end */
static void jtab_jump(Unit* u, int k);
static void jtab_emit(Unit* u);
#endif
/* c->pc = address, c->rt->undef(c, address, word); SVC n (c->pc = next, c->rt->svc or svcs[n], the pins around it);
   c->pc = address, c->rt->brk(c, address, imm) */
static void mg_undef(Unit* u, SnxU64 address, SnxU32 word);
static void mg_svc(Unit* u, SnxU64 next, unsigned n, int direct);
static void mg_brk(Unit* u, SnxU64 address, unsigned imm);
/* the function's entry, after the prologue: its stale check (options->stale), then e != start going to
   u->mismatch_label */
static void mg_entry(Unit* u, size_t index, SnxU64 start);
#ifdef MG_DIRECT_ENTRY
/* the unit's functions check their target before the frame (mg_check), and a call of one's start comes in past it,
   with no target in SGX_FN_TARGET: 1, 0 where the entry is mg_entry's */
static int mg_direct(const Unit* u);
/* if (e != start) goto label, before the prologue */
static void mg_check(Unit* u, SnxU64 start, int label);
/* after the prologue: if (snx_stale[index]) { c->pc = given ? e : start; return; }, where options->stale */
static void mg_stale(Unit* u, size_t index, int given, SnxU64 start);
#endif
#ifdef MG_ENTRY_STALE
static void mg_entry_stale(Unit* u, size_t index);
/* the rest of mg_entry's before the compare (x86-64's host FP environment), for a function entered at its start alone */
static void mg_entry_fp(Unit* u);
#endif
/* at u->mismatch_label: c->pc = e and the dispatch block (several leaders), else CALLDYN(e) */
static void mg_dispatch(Unit* u, int lab0);
/* the frame's size in the prologue and every epilogue, and the labels' jumps since fixup fix0: 1, 0 where one isn't
   bound or out of reach */
static int mg_finish(Unit* u, size_t fix0);
/* a relocation of the unit's to one of its own functions, resolved in place: 1, 0 where it stays one. target_at is
   the entry of a call of the function's start, past its check where it has one (Sym.entry): every call of one of the
   codegen's functions is of its start (call_target), and a function's jump to its own entry a label's */
static int mg_resolve_local(Unit* u, const Reloc* r, size_t target_at);
#ifdef MG_POOL_REACH
/* the literals so far, here, a jump over them */
static void mg_pool_island(Unit* u);
#endif
#ifdef MG_COFF
/* 1 where the unit's code is for Windows: its object is COFF (write_coff), not ELF (write_object) */
static int MG_COFF(const Unit* u);
#endif
#ifdef MG_BOUND
/* what a bound PLT stub's slot is compared with (call_stub, the BR of a stub): a jump to label where the function at
   index is stale, where the guest's Xr is not value (its pin's, else the state's), where CPU.pc is not; and CPU.pc as
   a call's target */
static void mg_if_stale(Unit* u, size_t index, int label);
static void mg_if_x_not(Unit* u, unsigned r, SnxU64 value, int label);
static void mg_if_pc_not(Unit* u, SnxU64 value, int label);
static void mg_target_pc(Unit* u);
#endif
/* the AdvSIMD instructions (1 made, 0 not one of these, -1 UNDEF) and the vector helpers */
static int vec(Unit* u, const SnxA64Insn* insn, SnxU32 word);
static void unit_reloc(void* machine, size_t at, const char* sym, long long addend);
static int vread(SgCtx* g, unsigned n);
static void vwrite(SgCtx* g, unsigned d, int r, int q);
static int vzero(SgCtx* g);
static SgD lane_get(SgCtx* g, unsigned size, int a, int i);
static int lane_put(SgCtx* g, unsigned size, int a, int i, SgD v);
static int vdup(SgCtx* g, unsigned size, int a, int e);

/* whether CRC32 word w takes the machine's fixed registers (the last pins'); every one does where the machine says
   nothing (MG_CRC_FIXED) */
#ifndef MG_CRC_FIXED
#define MG_CRC_FIXED(u, w) 1
#endif
/* the bits of v (a W's or an X's) in the other order: the machine's, where it has better (MG_RBIT) */
#ifdef MG_RBIT
static SgD MG_RBIT(Unit* u, SgD v);
#else
#define MG_RBIT(u, v) sg_d_bitreverse((u)->g, (v))
#endif

static void pins(Unit* u, int store) { pins_of(u, &u->b, store, 0); }
/* the region's own stores of the pins (UNDEF's flush, the spill and reload around a helper's call) are assembled
   with the region, or with the cold paths at the function's end, not where cur_dirty is theirs: every pin written */
static void pins_all(Unit* u, SgXBuf* b, int store, int caller) {
    SnxU64 d = u->cur_dirty, sel = u->pin_sel;
    SnxU32 vd = u->vcur_dirty, vsel = u->vpin_sel;
    u->cur_dirty = u->pin_sel = ~(SnxU64)0, u->vcur_dirty = u->vpin_sel = ~0u;
    pins_of(u, b, store, caller);
    u->cur_dirty = d, u->vcur_dirty = vd, u->pin_sel = sel, u->vpin_sel = vsel;
}
static void pins_flush(void* machine) { pins_all((Unit*)machine, ((Unit*)machine)->a.b, 1, 0); }
/* the region assembler's: into the buffer it writes (the cold one on a cold path) */
static void pins_cs(void* machine, int store) { pins_all((Unit*)machine, ((Unit*)machine)->a.b, store, 1); }

/* the names of the machine's registers, for fixed_fault's line */
#ifndef MG_GPR_NAME
#define MG_GPR_NAME(r, buf) (snprintf((buf), sizeof(buf), "r%d", (r)), (buf))
#endif
#ifndef MG_VREG_PREFIX
#define MG_VREG_PREFIX "v"
#endif
/* SgXAsm's fixed_fault: instruction at of the region (the guest word it was made for, SgXIns' src) writes host register
   reg for itself, where a pin keeps a guest register for the whole function, or its machine sequence writes reg
   without declaring it (undeclared). The function's code would lose that register, or a value the allocator left in
   reg: a defect of the back end, which stops the recompile rather than make wrong code */
static void fixed_fault(void* machine, int at, int reg, int xmm, int undeclared) {
    Unit* u = (Unit*)machine;
    const SnxFunction* f = u->f;
    int src = at >= 0 && at < u->g->x.n ? u->g->x.ins[at].src : -1, r;
    char name[16], gpr[16], held[48] = "no guest register", where[96];
    if (xmm)
        snprintf(name, sizeof name, "%s%d", MG_VREG_PREFIX, reg);
    else
        snprintf(name, sizeof name, "%s", MG_GPR_NAME(reg, gpr));
    for (r = 0; r < 32 && !undeclared; ++r)
        if (xmm ? u->vpin[r] == reg : u->pin[r] == reg)
            snprintf(held, sizeof held, "the guest's %s%d", xmm ? "V" : "X", r);
    if (!xmm && !undeclared && u->mempin == reg) snprintf(held, sizeof held, "the guest memory's base");
    if (src >= 0 && (size_t)src < f->count) {
        SnxU64 pc = snx_function_address(f, (size_t)src);
        SnxU32 word = 0;
        (void)snx_codegen_word((SnxCodegen*)u->codegen, pc, &word);
        snprintf(where, sizeof where, "the word %08x at %llx", (unsigned)word, (unsigned long long)pc);
    } else
        snprintf(where, sizeof where, "its entry, its dispatch or a stub (no word of its own)");
    if (undeclared)
        fprintf(stderr,
                "native back end: function %llx, %s: its machine sequence writes %s, which its instruction does not "
                "declare (a value may be left there): nothing of it is made\n",
                (unsigned long long)f->start, where, name);
    else
        fprintf(stderr,
                "native back end: function %llx, %s: writes %s for itself, which keeps %s for the whole function (its "
                "pin): nothing of it is made\n",
                (unsigned long long)f->start, where, name, held);
    abort();
}

/* a function's symbol */
static void fname(char* out, size_t size, const char* prefix, SnxU64 address) {
    snprintf(out, size, "%s%llX", prefix, (unsigned long long)address);
}

#ifdef MG_DIRECT_ENTRY
/* a call of callee's start comes in past its check, no target given, where callee is this function or one the unit
   made before it, within MG_DIRECT_ENTRY of here: the call is resolved in the unit then. One made later may be
   another back end's yet, whose entry reads the target */
static int past_check(const Unit* u, const SnxFunction* callee) {
    size_t i = (size_t)(callee - u->codegen->functions);
    const Sym* s;
    if (!mg_direct(u)) return 0;
    if (i == u->index)
        s = &u->syms[u->nsyms - 1];
    else if (i >= u->first && i < u->index && u->sym_of[i - u->first])
        s = &u->syms[u->sym_of[i - u->first] - 1];
    else
        return 0;
    return u->b.n - (s->at + s->entry) < MG_DIRECT_ENTRY;
}
#endif
/* call(): F_target directly where the target is one of the codegen's functions (not routed through its slot),
   snx_aot_<target> where options->direct names it (the AOT module's function in a JIT unit, another module's an AOT
   module imports), CALLDYN otherwise; tail: then return */
static void call_target(Unit* u, SnxU64 target, int tail) {
    const SnxCodegen* c = u->codegen;
    int through_slot = c->hook_direct_calls || snx_codegen_routed(c, target);
    const SnxFunction* callee = through_slot ? NULL : snx_codegen_function(c, target);
    char sym[40];
#ifdef MG_DIRECT_ENTRY
    if (callee && past_check(u, callee)) {
        fname(sym, sizeof sym, "F_", target);
        call_sym(u, sym, tail);
        return;
    }
#endif
    set_target(u, target);
    if (callee)
        fname(sym, sizeof sym, "F_", target);
    else if (!through_slot && u->options->direct && u->options->direct(u->options->direct_user, target))
        snprintf(sym, sizeof sym, "snx_aot_%llx", (unsigned long long)target);
    else {
        calldyn(u, tail);
        return;
    }
    call_sym(u, sym, tail);
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
    u->stubs[u->nstubs].dirty = u->cur_dirty;
    u->stubs[u->nstubs].vdirty = u->vcur_dirty;
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

static void regs_of(SnxU32 w, SnxU64* use, SnxU64* def);
static int writes_nzcv(SnxU32 word) {
    SnxU64 use, def;
    regs_of(word, &use, &def);
    return (def >> 32 & 15) != 0;
}

/* FPSR.QC as the region's saturating instructions leave it, put in the state: a machine whose vec() keeps a region's
   in one value (g->qc_owed, MG_QC) writes the state once, where the region ends or something may read FPSR */
#ifdef MG_QC
static void mg_qc(Unit* u);
#endif
static void qc_pay(Unit* u) {
    if (!u->g->qc_owed) return;
#ifdef MG_QC
    mg_qc(u);
#endif
    u->g->qc_owed = 0;
}

/* the region so far assembled, and the block's state forgotten: the next instruction starts a new one */
static void flush(Unit* u) {
    SgCtx* g = u->g;
    int k;
    qc_pay(u);
#ifdef MG_HOST_FP_HELPER
    for (k = 0; k < g->x.n && !u->host_fp; ++k)
        u->host_fp = MG_HOST_FP_HELPER(g->x.ins[k].sym); /* only a call keeps a symbol */
#endif
    if (g->x.n) sgx_assemble_region(&u->a, &g->x);
#ifdef MG_POOL_REACH
    if (u->a.nrips && u->b.n - u->a.rips[0].at > MG_POOL_REACH) mg_pool_island(u);
#endif
    g->x.end_dead = 0;
    g->x.end_vdead = 0;
    for (k = 0; k < 32; ++k) g->gpr[k] = g->vd[k][0] = g->vd[k][1] = g->vx[k] = g->gk_known[k] = 0;
    g->fp_state[0] = g->fp_state[1] = 0, g->fp_bound[0] = g->fp_bound[1] = 0;
    g->fp_dbound[0] = g->fp_dbound[1] = 0, g->fp_fz = g->fp_any = 0, g->fp_any_bits = 0;
    g->mem = 0;
    g->origin = -1;
    g->x.flags_ins = -1;
}

/* flush where control goes on at target (the next instruction, a branch's): what is dead there needs no store */
static SnxU64 dead_at(Unit* u, SnxU64 target);
static SnxU32 vdead_at(Unit* u, SnxU64 target);
static void flush_to(Unit* u, SnxU64 target) {
    u->g->x.end_dead = dead_at(u, target);
    u->g->x.end_vdead = vdead_at(u, target);
    flush(u);
}

/* the machine's instructions of a function's region */
enum { SGX_FN_JCC = 16, SGX_FN_CALLDYN, SGX_FN_TAILDYN, SGX_FN_JCC_BACK, SGX_FN_JTAB };
static inline void sgx_machine_op(SgXAsm* a, int at, const unsigned char* r) {
    Unit* u = (Unit*)a->machine;
    SgXIns* i = &a->x->ins[at];
    (void)r;
    switch (i->op) {
    case SGX_FN_JCC: jcc(u, i->cc, i->disp); break;
#ifdef MG_POLL_INLINE
    case SGX_FN_JCC_BACK: {
        /* a loop's backward branch: past it where not taken (the flags as they were, for the region's code after);
           taken, to the header where no poll is pending, else to the poll's stub (imm) */
        int skip = new_label(u);
        jcc(u, i->cc ^ 1, skip);
        poll_jz(u, i->disp);
        jmp(u, (int)i->imm);
        bind(u, skip);
        break;
    }
#endif
#ifdef MG_JTAB
    case SGX_FN_JTAB: jtab_jump(u, (int)i->imm); break; /* the target in SGX_FN_TARGET (in[0]) */
#endif
    case SGX_FN_CALLDYN: calldyn(u, 0); break;      /* the target in SGX_FN_TARGET (in[0]) */
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
    i->fixed[0] = SGX_FN_TARGET;
    i->calls = 1;
}

/* a branch's condition at the region's end: JCC to label on its condition code */
static void jump_if(Unit* u, SgB taken, int label, SnxU64 target) {
    SgX* x = &u->g->x;
    int from, cc = sgx_bool_cc(x, taken, &from);
    SgXIns* i = sgx_append(x, SGX_K_MACHINE);
    i->op = SGX_FN_JCC;
    i->cc = (unsigned char)cc;
    i->disp = label;
    i->flags_from = from;
    i->kv[1] = dead_at(u, target);
    i->kv[0] = vdead_at(u, target);
    i->jdead = i->kv[1] != 0 || i->kv[0] != 0;
}
#ifdef MG_POLL_INLINE
/* the same for a backward branch to the function's instruction header, its poll pending going to the stub slow */
static void jump_if_back(Unit* u, SgB taken, int header, int slow, SnxU64 target) {
    SgX* x = &u->g->x;
    jump_if(u, taken, header, target);
    x->ins[x->n - 1].op = SGX_FN_JCC_BACK;
    x->ins[x->n - 1].imm = slow;
}
#endif

/* ---- what the Sail roots don't have: loads and stores, vectors and system instructions, on the region's SSA ---- */

/* Xn as an address: SP for 31 */
static SgD base_of(SgCtx* g, unsigned rn) { return sg_GPR_read(g, rn); }
/* Xm as an operand: XZR for 31 */
static SgD xzr_or(SgCtx* g, unsigned rm) { return rm == 31 ? sgx_const(64, 0) : sg_GPR_read(g, rm); }

/* the machine's form of a guest access of the SIMD&FP registers of 4, 8 or 16 bytes (a load zeroing the rest of the
   register, as the load's Vt wants): its operand reg the register, rm the memory */
static const SgXVForm* v_form(int bytes, int store) {
    if (bytes == 16) return store ? SGX_VF_ST16 : SGX_VF_LD16;
    if (bytes == 8) return store ? SGX_VF_ST8 : SGX_VF_LD8;
    return store ? SGX_VF_ST4 : SGX_VF_LD4;
}
static int vload(SgCtx* g, SgD address, int bytes);
static void vstore(SgCtx* g, SgD address, int r, int bytes);
static void v_load(SgCtx* g, unsigned t, SgD address, int bytes) {
    SgD v;
    if (bytes >= 4) {
        sg__V_write(g, t, sgx_xval(128, vload(g, address, bytes)));
        return;
    }
    v = sg_GuestMemRead(g, address, bytes, 0);
    sg__V_write(g, t, sg_d_zext(g, v, 128));
}
static void v_store(SgCtx* g, unsigned t, SgD address, int bytes) {
    SgD v = sg__V_read_bits(g, t, 8 * bytes - 1, 0);
    if (bytes >= 4) {
        vstore(g, address, sgx_xreg(sgx(g), &v), bytes);
        return;
    }
    sg_GuestMemWrite(g, address, bytes, 0, v);
}

/* LDR, STR (immediate: unsigned offset, unscaled, pre- and post-index; register offset; literal), LDP, STP, LDNP,
   STNP of the SIMD&FP registers, B to Q */
static int fp_ldst(Unit* u, SnxU32 word, SnxU64 pc) {
    if (!SGX_HAS_VREGS) return 0; /* vector values: the Sail's emitters make it in doublewords */
    SgCtx* g = u->g;
    unsigned rt = word & 31, rn = word >> 5 & 31;
    if ((word & 0x3F000000u) == 0x1C000000u) { /* LDR (literal) */
        unsigned opc = word >> 30;
        long long imm = snx_sext64((word >> 5) & 0x7FFFF, 19) * 4; /* imm19 * 4 */
        if (opc == 3) return 0;
        v_load(g, rt, sgx_const(64, pc + (SnxU64)imm), 4 << opc);
        return 1;
    }
    if ((word & 0x3C000000u) == 0x2C000000u) { /* the pairs: opc 101 1 0 idx L imm7 Rt2 Rn Rt */
        unsigned opc = word >> 30, idx = word >> 23 & 7, l = word >> 22 & 1, rt2 = word >> 10 & 31;
        int bytes = 4 << opc;
        long long off = snx_sext64((word >> 15) & 0x7F, 7);
        SgD base, a;
        if (opc == 3 || idx > 3 || (l && rt == rt2)) return 0;
        off *= bytes;
        base = base_of(g, rn);
        a = idx == 1 ? base : sg_d_add(g, base, sgx_const(64, (SnxU64)off));
        if (l) {
            /* both loads first: the second's V register may be the first's (rejected above), never the address */
            v_load(g, rt, a, bytes);
            v_load(g, rt2, sg_d_add(g, a, sgx_const(64, (SnxU64)bytes)), bytes);
        } else {
            v_store(g, rt, a, bytes);
            v_store(g, rt2, sg_d_add(g, a, sgx_const(64, (SnxU64)bytes)), bytes);
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
            long long off = snx_sext64((word >> 12) & 0x1FF, 9);
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
    if (!SGX_HAS_VREGS) return 0; /* vector values: the Sail's emitters make it in doublewords */
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
    if (!SGX_HAS_VREGS) return 0; /* vector values: the Sail's emitters make it in doublewords */
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
    if (!SGX_HAS_VREGS) return 0; /* vector values: the Sail's emitters make it in doublewords */
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
#ifdef MG_X64_VEC_INS
    if ((word & 0xFFE0FC00u) == 0x4E001C00u || (word & 0xFFE08400u) == 0x6E000400u) return 0; /* INS: vec()'s */
#endif
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

/* SYS, MRS and MSR: what EL0 reads and writes on the Switch, the cache maintenance it does; 0 for the rest, which no
   code is made for (undefined where it runs) */
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
            /* the constants EL0 reads: CTR_EL0 (SCTLR_EL1.UCT), DCZID_EL0, CNTFRQ_EL0 */
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
            default: return -1; /* UNDEF at EL0 on the Switch */
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
            g->fp_dbound[0] = g->fp_dbound[1] = 0, g->fp_fz = g->fp_any = 0, g->fp_any_bits = 0;
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
        default: return -1; /* read-only at EL0, or EL1's: UNDEF */
        }
    }
    if (op0 == 1 && !l && op1 == 3 && crn == 7 && op2 == 1) {
        SgD a = xzr_or(g, rt);
        switch (crm) {
        case 4: { /* DC ZVA: the 64 bytes of the block at a zeroed, four 16-byte stores */
            SgX* x = sgx(g);
            SgD block = sg_d_and(g, a, sgx_const(64, ~(SnxU64)63));
            if (!SGX_HAS_VREGS) { /* eight of 8 bytes */
                for (int d = 0; d < 64; d += 8) sg_GuestMemWrite(g, sg_d_add_int(g, block, d), 8, 0, sgx_const(64, 0));
                return 1;
            }
            int z = sgx_xconst(x, 0, 0), base = sgx_mem(g), index = sgx_reg(x, &block), k, in[4] = {0, z, 0, 0};
            for (k = 0; k < 4; ++k)
                sgx_vins(x, SGX_VF_ST16, 0, 0, in, 0, 0, SGX_VM_GUEST, base, index, 16 * k, 0, 0);
            return 1;
        }
        case 5: sgx_call(g, "snx_ic_ivau", 0, 1, &a, 1, 0); return 1;
        case 10:
        case 11:
        case 14: return 1;
        default: return -1;
        }
    }
    return -1; /* the rest of SYS and SYSL: UNDEF */
}
#undef SYSREG

/* ---- the SIMD&FP structures: LD1-LD4, ST1-ST4 (multiple and single), LD1R-LD4R ---- */

/* bytes 4, 8 or 16 of guest memory at address as a vector register (v_form's), and the other way */
static int vload(SgCtx* g, SgD address, int bytes) {
    SgX* x = sgx(g);
    const SgXVForm* f = v_form(bytes, 0);
    int disp = 0, base, index;
    base = sgx_base(g, &address, &disp), index = sgx_reg(x, &address);
    return sgx_vins(x, f, f->reg, 0, NULL, 0, f->rm, SGX_VM_GUEST, base, index, disp, 0, 0);
}
static void vstore(SgCtx* g, SgD address, int r, int bytes) {
    SgX* x = sgx(g);
    const SgXVForm* f = v_form(bytes, 1);
    int disp = 0, base, index, in[4] = {0, 0, 0, 0};
    base = sgx_base(g, &address, &disp), index = sgx_reg(x, &address);
    in[(int)f->reg] = r;
    sgx_vins(x, f, f->rm, 0, in, 0, f->rm, SGX_VM_GUEST, base, index, disp, 0, 0);
}

#ifdef MG_X64_STRUCT_FAST
/* Wide LD_mult and ST_mult through 16-byte chunks. PUNPCK handles two registers, PSHUFB the others.
   Return 0 for shapes that are not whole chunks, so the Sail loop makes them. Chunks follow address order. */
static int struct_ldst_fast(SgCtx* g, unsigned selem, unsigned size, int load, unsigned rt, SgD base, int q) {
    SgX* x = sgx(g);
    unsigned esize = 1u << size, c, s, j;
    unsigned nchunks, bytes = selem * (q ? 16u : 8u), last;
    int chunk[4];
    static const SgXVForm* const lo[4] = {&sgx_v_punpcklbw, &sgx_v_punpcklwd, &sgx_v_punpckldq, &sgx_v_punpcklqdq};
    static const SgXVForm* const hi[4] = {&sgx_v_punpckhbw, &sgx_v_punpckhwd, &sgx_v_punpckhdq, &sgx_v_punpckhqdq};
    if (selem < 2 || selem > 4 || size > 3 || !(x->features & SGX_F_SSSE3)) return 0;
    nchunks = (bytes + 15) / 16;
    last = bytes - 16 * (nchunks - 1); /* the last chunk's bytes: 8 for three D registers' 24, else 16 */
    if (load)
        for (c = 0; c < nchunks; ++c)
            chunk[c] = vload(g, sg_d_add(g, base, sgx_const(64, 16 * c)), c + 1 == nchunks ? (int)last : 16);
    if (q && load && selem == 2) {
        /* one mask gathers a register's elements from either chunk, so the two masks serve both loads */
        if (size == 3) {
            /* two quadwords per chunk: the halves of the chunks are the registers already */
            vwrite(g, rt & 31, sgx_v2(x, &sgx_v_punpcklqdq, chunk[0], chunk[1], 0), 1);
            vwrite(g, (rt + 1) & 31, sgx_v2(x, &sgx_v_punpckhqdq, chunk[0], chunk[1], 0), 1);
            return 1;
        }
        for (s = 0; s < 2; ++s) {
            SnxU64 lo = 0, hi = 0;
            int mask, sh0, sh1;
            for (j = 0; j < 16; ++j) {
                SnxU64 b = j < 8 ? (SnxU64)((2 * (j / esize) + s) * esize + j % esize) : 0x80ull;
                if (j < 8)
                    lo |= b << (8 * j);
                else
                    hi |= b << (8 * (j - 8));
            }
            mask = sgx_xconst(x, lo, hi);
            sh0 = sgx_v2(x, &sgx_v_pshufb, chunk[0], mask, 0);
            sh1 = sgx_v2(x, &sgx_v_pshufb, chunk[1], mask, 0);
            vwrite(g, (rt + s) & 31, sgx_v2(x, &sgx_v_punpcklqdq, sh0, sh1, 0), 1);
        }
        return 1;
    }
    if (q && !load && selem == 2) {
        /* an unpack of the two registers makes each chunk: no PSHUFB mask, and each register is read once */
        int v0 = vread(g, rt & 31), v1 = vread(g, (rt + 1) & 31);
        vstore(g, base, sgx_v2(x, lo[size], v0, v1, 0), 16);
        vstore(g, sg_d_add(g, base, sgx_const(64, 16)), sgx_v2(x, hi[size], v0, v1, 0), 16);
        return 1;
    }
    if (q && !load && selem == 4) {
        /* a butterfly of unpacks makes each chunk from the four registers, in address order, with no PSHUFB mask
           and no OR layer: the first unpacks pair Vt with Vt+1 and Vt+2 with Vt+3, and the second interleave
           puts one element of each of the four in each group of four bytes. A pair of quadwords (size 3) is
           already a chunk: the second interleave would be of 16-byte halves, which no instruction makes */
        int v[4], a01, a23, b01, b23, ci;
        for (s = 0; s < 4; ++s) v[s] = vread(g, (rt + s) & 31);
        if (size == 3) {
            vstore(g, base, sgx_v2(x, &sgx_v_punpcklqdq, v[0], v[1], 0), 16);
            vstore(g, sg_d_add(g, base, sgx_const(64, 16)), sgx_v2(x, &sgx_v_punpcklqdq, v[2], v[3], 0), 16);
            vstore(g, sg_d_add(g, base, sgx_const(64, 32)), sgx_v2(x, &sgx_v_punpckhqdq, v[0], v[1], 0), 16);
            vstore(g, sg_d_add(g, base, sgx_const(64, 48)), sgx_v2(x, &sgx_v_punpckhqdq, v[2], v[3], 0), 16);
            return 1;
        }
        a01 = sgx_v2(x, lo[size], v[0], v[1], 0);
        a23 = sgx_v2(x, lo[size], v[2], v[3], 0);
        b01 = sgx_v2(x, hi[size], v[0], v[1], 0);
        b23 = sgx_v2(x, hi[size], v[2], v[3], 0);
        for (ci = 0; ci < 4; ++ci) {
            int lo01 = ci & 2 ? b01 : a01, lo23 = ci & 2 ? b23 : a23;
            int wide = sgx_v2(x, ci & 1 ? hi[size + 1] : lo[size + 1], lo01, lo23, 0);
            vstore(g, sg_d_add(g, base, sgx_const(64, 16 * ci)), wide, 16);
        }
        return 1;
    }
    if (q && load && selem == 4) {
        /* the inverse butterfly: unpacks at the element's width gather each register's elements in order, one
           fewer round for each doubling of the element; the quadword unpacks then join the register's two halves.
           No PSHUFB mask and no OR layer. q is 1 here, as the caller only takes this path for Q forms */
        int t[4] = {chunk[0], chunk[1], chunk[2], chunk[3]}, a01, a23, b01, b23, round;
        for (round = 0; round < 3 - (int)size; ++round) {
            a01 = sgx_v2(x, lo[size], t[0], t[1], 0);
            b01 = sgx_v2(x, hi[size], t[0], t[1], 0);
            a23 = sgx_v2(x, lo[size], t[2], t[3], 0);
            b23 = sgx_v2(x, hi[size], t[2], t[3], 0);
            t[0] = a01, t[1] = b01, t[2] = a23, t[3] = b23;
        }
        vwrite(g, rt & 31, sgx_v2(x, &sgx_v_punpcklqdq, t[0], t[2], 0), 1);
        vwrite(g, (rt + 1) & 31, sgx_v2(x, &sgx_v_punpckhqdq, t[0], t[2], 0), 1);
        vwrite(g, (rt + 2) & 31, sgx_v2(x, &sgx_v_punpcklqdq, t[1], t[3], 0), 1);
        vwrite(g, (rt + 3) & 31, sgx_v2(x, &sgx_v_punpckhqdq, t[1], t[3], 0), 1);
        return 1;
    }
    if (!q && load && selem == 2 && size == 2) {
        /* one chunk: two quadwords per register. PSHUFD gathers either register's pair of elements. */
        int c0 = chunk[0];
        vwrite(g, rt & 31, sgx_v1(x, &sgx_v_pshufd, c0, 0x08), 0);
        vwrite(g, (rt + 1) & 31, sgx_v1(x, &sgx_v_pshufd, c0, 0x8D), 0);
        return 1;
    }
    if (!q && load && selem == 4 && size >= 1) {
        /* two chunks hold the four D registers' halves: pair the chunks at the element's width, then unpack the
           pairs into one register each. The high quadwords hold the next register; vwrite clears the other half. */
        int a, b, u0, u1;
        a = size == 2 ? chunk[0] : sgx_v2(x, lo[1], chunk[0], chunk[1], 0);
        b = size == 2 ? chunk[1] : sgx_v2(x, hi[1], chunk[0], chunk[1], 0);
        u0 = sgx_v2(x, size == 2 ? lo[2] : lo[1], a, b, 0);
        u1 = sgx_v2(x, size == 2 ? hi[2] : hi[1], a, b, 0);
        vwrite(g, rt & 31, u0, 0);
        vwrite(g, (rt + 1) & 31, sgx_v2(x, &sgx_v_punpckhqdq, u0, u0, 0), 0);
        vwrite(g, (rt + 2) & 31, u1, 0);
        vwrite(g, (rt + 3) & 31, sgx_v2(x, &sgx_v_punpckhqdq, u1, u1, 0), 0);
        return 1;
    }
    if (!q && !load && selem == 2) { /* the two D registers' elements in turn: one unpack, one chunk */
        vstore(g, base, sgx_v2(x, lo[size], vread(g, rt & 31), vread(g, (rt + 1) & 31), 0), 16);
        return 1;
    }
    if (!q && !load && selem == 4) { /* the butterfly's low halves: two chunks */
        int v[4], a01, a23;
        for (s = 0; s < 4; ++s) v[s] = vread(g, (rt + s) & 31);
        a01 = sgx_v2(x, lo[size], v[0], v[1], 0);
        a23 = sgx_v2(x, lo[size], v[2], v[3], 0);
        vstore(g, base, sgx_v2(x, lo[size + 1], a01, a23, 0), 16);
        vstore(g, sg_d_add(g, base, sgx_const(64, 16)), sgx_v2(x, hi[size + 1], a01, a23, 0), 16);
        return 1;
    }
    if (load) {
        for (s = 0; s < selem; ++s) {
            int reg = 0;
            for (c = 0; c < nchunks; ++c) {
                SnxU64 lo = 0, hi = 0;
                int sh;
                for (j = 0; j < 16; ++j) {
                    unsigned off = (j / esize) * selem * esize + s * esize + j % esize;
                    SnxU64 b = off >= 16 * c && off < 16 * (c + 1) ? (SnxU64)(off - 16 * c) : 0x80ull;
                    if (j < 8)
                        lo |= b << (8 * j);
                    else
                        hi |= b << (8 * (j - 8));
                }
                sh = sgx_v2(x, &sgx_v_pshufb, chunk[c], sgx_xconst(x, lo, hi), 0);
                reg = c ? sgx_v2(x, &sgx_v_por, reg, sh, 0) : sh;
            }
            vwrite(g, (rt + s) & 31, reg, q);
        }
    } else {
        for (c = 0; c < nchunks; ++c) {
            int wide = 0;
            for (s = 0; s < selem; ++s) {
                SnxU64 lo = 0, hi = 0;
                int sh, src = vread(g, (rt + s) & 31);
                for (j = 0; j < 16; ++j) {
                    unsigned off = 16 * c + j, k = off / esize;
                    SnxU64 b = k % selem == s ? (SnxU64)(k / selem * esize + off % esize) : 0x80ull;
                    if (j < 8)
                        lo |= b << (8 * j);
                    else
                        hi |= b << (8 * (j - 8));
                }
                sh = sgx_v2(x, &sgx_v_pshufb, src, sgx_xconst(x, lo, hi), 0);
                wide = s ? sgx_v2(x, &sgx_v_por, wide, sh, 0) : sh;
            }
            vstore(g, sg_d_add(g, base, sgx_const(64, 16 * c)), wide, c + 1 == nchunks ? (int)last : 16);
        }
    }
    return 1;
}
#endif

/* 1 made, 0 not one of these, -1 UNDEF */
static int struct_ldst(Unit* u, SnxU32 word) {
    if (!SGX_HAS_VREGS) return 0; /* vector values: the Sail's emitters make it in doublewords */
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
#ifdef MG_X64_STRUCT_FAST
            if (struct_ldst_fast(g, selem, size, l, rt, base, (int)q)) {
                /* made in 16-byte chunks */
            } else
#endif
            {
                /* interleaved: structure e's element s is V[t + s]'s element e, one at a time */
                int regs[4];
                for (s = 0; s < selem; ++s) regs[s] = l ? vzero(g) : vread(g, (rt + s) & 31);
                for (e = 0; e < ne; ++e)
                    for (s = 0; s < selem; ++s) {
                        a = sg_d_add(g, base, sgx_const(64, (e * selem + s) * eb));
                        if (l)
                            regs[s] = lane_put(g, size, regs[s], (int)e, sg_GuestMemRead(g, a, eb, 0));
                        else
                            sg_GuestMemWrite(g, a, eb, 0, lane_get(g, size, regs[s], (int)e));
                    }
                if (l)
                    for (s = 0; s < selem; ++s) vwrite(g, (rt + s) & 31, regs[s], (int)q);
            }
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

/* the instruction at address, word; 0 where the back end has no code for it (undefined where it runs) */
/* SBFM and UBFM (LSL, LSR, ASR, SBFX, UBFX, SBFIZ, UBFIZ, SXTB to SXTW, UXTB, UXTH): one or two shifts, a MOVSX or
   a mask, not the Sail's rotation, masks and Replicate of the sign. BFM, and reserved fields (the Sail's UNDEF), are
   the Sail's */
static int bitfield(Unit* u, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned sf = word >> 31, opc = word >> 29 & 3, n = word >> 22 & 1, immr = word >> 16 & 63, imms = word >> 10 & 63;
    unsigned rn = word >> 5 & 31, rd = word & 31, d = sf ? 64 : 32;
    SgD v, r;
    if ((word >> 23 & 0x3F) != 0x26 || (opc != 0 && opc != 2) || n != sf || (!sf && (immr > 31 || imms > 31))) return 0;
    v = xzr_or(g, rn);
    if (!sf) v = sg_d_slice(g, v, 31, 0);
    if (opc == 2) {
        if (imms == d - 1) r = sg_d_lsr(g, v, immr);                   /* LSR */
        else if (imms + 1 == immr) r = sg_d_lsl(g, v, d - 1 - imms);   /* LSL */
        else if (imms >= immr) r = sg_d_zext(g, sg_d_slice(g, v, imms, immr), d); /* UBFX, UXTB, UXTH */
        else r = sg_d_lsl(g, sg_d_zext(g, sg_d_slice(g, v, imms, 0), d), d - immr); /* UBFIZ */
    } else {
        if (imms == d - 1) r = sg_d_asr(g, v, immr);                   /* ASR */
        else if (imms >= immr && !immr) r = sg_d_sext(g, sg_d_slice(g, v, imms, 0), d); /* SXTB, SXTH, SXTW */
        else if (imms >= immr) r = sg_d_asr(g, sg_d_lsl(g, v, d - 1 - imms), d - 1 - imms + immr); /* SBFX */
        else r = sg_d_lsl(g, sg_d_sext(g, sg_d_slice(g, v, imms, 0), d), d - immr); /* SBFIZ */
    }
    if (rd != 31) sg_GPR_write(g, rd, sf ? r : sg_d_zext(g, r, 64));
    return 1;
}

/* RBIT, REV16, REV32 and REV: the machine's byte reversal and what of it is left to move, not the Sail's loops of bits
   and of bytes. X registers' REV of W's size is reserved, the Sail's */
static int reverse(Unit* u, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned sf = word >> 31, opc = word >> 10 & 3, rn = word >> 5 & 31, rd = word & 31, w = sf ? 64 : 32;
    if ((word & 0x7FFFF000u) != 0x5AC00000u || (opc == 3 && !sf)) return 0;
    SgD v = xzr_or(g, rn), r;
    if (!sf) v = sg_d_slice(g, v, 31, 0);
    if (!opc)
        r = MG_RBIT(u, v);
    else if (opc == 1 && sf) { /* REV16's halves of X: each byte with its neighbour */
        SgD m = sgx_const(64, 0x00FF00FF00FF00FFull);
        r = sg_d_or(g, sg_d_and(g, sg_d_lsr(g, v, 8), m), sg_d_lsl(g, sg_d_and(g, v, m), 8));
    } else {
        sgx_flat(sgx(g), &v);
        r = SGX_OP(sgx(g), rev, w, v);
        if (opc != 2 + sf) r = sg_d_or(g, sg_d_lsl(g, r, w / 2), sg_d_lsr(g, r, w / 2)); /* the halves back in order */
    }
    if (rd != 31) sg_GPR_write(g, rd, sf ? r : sg_d_zext(g, r, 64));
    return 1;
}

/* ---- PLT stubs the program's linker binds (SnxBoundBranch) ----
   On a machine without MG_BOUND every call of a stub runs the stub */

#ifdef MG_BOUND
/* The function the BR at address reaches once the program's linker has bound it (options->bound), when a call there
   needs no lookup: a function of the codegen, or one the module imports (options->direct), that the module does not
   send through its dispatch slot, with *guard set when its slot may hold another function. 0 for any other BR */
static SnxU64 bound_function(Unit* u, SnxU64 address, int* guard) {
    const SnxCodegen* c = u->codegen;
    SnxU64 bound = u->options->bound ? u->options->bound(u->options->bound_user, address, guard) : 0;
    if (!bound || c->hook_direct_calls || snx_codegen_routed(c, bound)) return 0;
    if (snx_codegen_function(c, bound)) return bound;
    return u->options->direct && u->options->direct(u->options->direct_user, bound) ? bound : 0;
}

/* The function a BL or B to target calls when target is a PLT stub that can run in the call's place: a function of
   the codegen that is ADRP x16, LDR x17, [x16, #n], ADD x16, x16, #n and a BR x17 that is bound (bound_function),
   with *guard as that gives it and the stub's first three words in w. 0 for any other target */
static SnxU64 stub_function(Unit* u, SnxU64 target, SnxU32* w, int* guard) {
    SnxCodegen* c = (SnxCodegen*)u->codegen;
    SnxU32 br = 0;
    unsigned i;
    if (!u->options->bound || !snx_codegen_function(c, target)) return 0;
    for (i = 0; i < 3; ++i)
        if (!snx_codegen_word(c, target + 4 * i, &w[i])) return 0;
    if (!snx_codegen_word(c, target + 12, &br) || (w[0] & 0x9F00001FUL) != 0x90000010UL ||
        (w[1] & 0xFFC003FFUL) != 0xF9400211UL || (w[2] & 0xFFC003FFUL) != 0x91000210UL || br != 0xD61F0220UL)
        return 0;
    return bound_function(u, target + 12, guard);
}

/* A BL or B to the PLT stub at stub, bound to the function bound, run in the stub's place, after the guest's
   registers were flushed. Without guard the slot is bound to that function by whatever links the program, so it is
   not read: x16 as the stub's ADRP and ADD leave it, x17 the function's address as the bound slot gives it, then
   F_bound. With guard, the stub's ADRP, LDR and ADD as at their own addresses, then if (x17 == bound) F_bound, else
   F_stub, whose own code takes whatever else the slot holds. A stub whose code the guest changed is stale: none of
   its instructions run here, and its function's entry leaves for the code there now. tail: each way returns.
   0 where the back end can't make the stub's load (no code for it then: undefined where it runs) */
static int call_stub(Unit* u, SnxU64 stub, const SnxU32* w, SnxU64 bound, int guard, int tail) {
    SgCtx* g = u->g;
    const SnxCodegen* c = u->codegen;
    long long pages = (long long)((w[0] >> 5 & 0x7FFFF) << 2 | (w[0] >> 29 & 3)); /* ADRP's immhi:immlo */
    int other = guard || u->options->stale ? new_label(u) : -1, join = tail ? -1 : new_label(u);
    SnxU64 page;
    if (pages & 0x100000) pages -= 0x200000;
    page = (stub & ~(SnxU64)0xFFF) + (SnxU64)pages * 4096;
    if (u->options->stale) mg_if_stale(u, (size_t)(snx_codegen_function(c, stub) - c->functions), other);
    sg_GPR_write(g, 16, sgx_const(64, page));
    if (guard) {
        SnxU32 word = g->word;
        SnxU64 pc = g->pc;
        g->word = w[1]; /* the stub's LDR x17, [x16, #n] */
        g->pc = stub + 4;
        g->start = g->x.n;
        g->branched = g->undefined = 0;
        g->taken = 0;
        g->exclusive_count = g->exclusive_next = 0;
        g->status = 0;
        if (!sg_decode(g) || g->x.unmade || g->undefined) return 0;
        g->word = word;
        g->pc = pc;
    } else
        sg_GPR_write(g, 17, sgx_const(64, bound));
    sg_GPR_write(g, 16, sgx_const(64, page + (w[2] >> 10 & 0xFFF)));
    flush(u);
    if (guard) mg_if_x_not(u, 17, bound, other);
    call_target(u, bound, tail);
    if (!tail) jmp(u, join);
    if (other >= 0) {
        bind(u, other);
        call_target(u, stub, tail);
    }
    if (!tail) bind(u, join);
    return 1;
}

/* a call (tail: a tail call) of target once the guest's registers are flushed: a bound PLT stub's function in the
   stub's place, else call_target. 0 where the stub's load can't be made */
static int call_or_stub(Unit* u, SnxU64 target, int tail) {
    SnxU32 w[3] = {0, 0, 0};
    int guard = 0;
    SnxU64 bound = stub_function(u, target, w, &guard);
    if (bound) return call_stub(u, target, w, bound, guard, tail);
    call_target(u, target, tail);
    return 1;
}
#else
static int call_or_stub(Unit* u, SnxU64 target, int tail) {
    call_target(u, target, tail);
    return 1;
}
#endif

/* bits hi..lo of a SIMD&FP register */
static SgD vbits(SgCtx* g, unsigned n, unsigned hi, unsigned lo) {
    return sg_d_slice(g, sg__V_read_bits(g, n, 127, 0), hi, lo);
}
/* v9.5's and v9.6's, which the Sail (v9.4) has no clause of: FEAT_FPRCVT's conversions between a SIMD&FP register's
   integer and its float, FEAT_FAMINMAX's extremes of the magnitudes, FEAT_LUT's tables of 2- and 4-bit numbers. 1 made,
   0 not one of them */
static int newer(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    static const signed char fp_size[4] = {32, 64, 0, 16};
    SgCtx* g = u->g;
    const SnxA64Args* a = &insn->args;
    unsigned rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31, e, n, bits;
    unsigned isize = word >> 31 ? 64 : 32, fsize = (unsigned)fp_size[word >> 22 & 3];
    SgD r, table;
    int round, uns = 0;
    switch (insn->op) {
    case SNX_A64_OP_UCVTF_simd: uns = 1; /* fallthrough */
    case SNX_A64_OP_SCVTF_simd:
        r = vbits(g, rn, isize - 1, 0);
        r = sg_FixedToFP(g, 0, r, 0, uns, sg_FPCR_read(g), SG_FP_ROUNDING_FPCR, fsize);
        sg__V_write(g, rd, sg_d_zext(g, r, 128));
        return 1;
    case SNX_A64_OP_FCVTNU_g_simd: uns = 1; /* fallthrough */
    case SNX_A64_OP_FCVTNS_g_simd: round = 0; break;
    case SNX_A64_OP_FCVTPU_g_simd: uns = 1; /* fallthrough */
    case SNX_A64_OP_FCVTPS_g_simd: round = 1; break;
    case SNX_A64_OP_FCVTMU_g_simd: uns = 1; /* fallthrough */
    case SNX_A64_OP_FCVTMS_g_simd: round = 2; break;
    case SNX_A64_OP_FCVTZU_g_simd: uns = 1; /* fallthrough */
    case SNX_A64_OP_FCVTZS_g_simd: round = 3; break;
    case SNX_A64_OP_FCVTAU_g_simd: uns = 1; /* fallthrough */
    case SNX_A64_OP_FCVTAS_g_simd: round = 4; break;
    case SNX_A64_OP_FAMAX:
    case SNX_A64_OP_FAMIN:
        bits = 8u << a->esz;
        r = sg_d_empty(128);
        for (e = 0; e < (a->q ? 128 : 64) / bits; ++e) {
            SgD o[2] = {vbits(g, rn, (e + 1) * bits - 1, e * bits), vbits(g, rm, (e + 1) * bits - 1, e * bits)};
            for (n = 0; n < 2; ++n) { /* the magnitudes, but a NaN's sign is its own */
                SgD m = sg_d_and(g, o[n], sgx_const(bits, sg_ones(bits - 1)));
                SnxU64 inf = sg_ones(bits - 1) & ~sg_ones(bits == 64 ? 52 : bits == 32 ? 23 : 10);
                SgB nan = sgx_d_ult(g, sgx_const(64, inf), sg_d_zext(g, m, 64));
                o[n] = sg_d_select(g, nan, sg_d_zext(g, o[n], 64), sg_d_zext(g, m, 64));
                o[n] = sg_d_slice(g, o[n], bits - 1, 0);
            }
            /* no operand is flushed, whatever FPCR.FZ and FZ16 hold */
            table = sg_d_and(g, sg_FPCR_read(g), sgx_const(64, ~(SnxU64)(1u << 24 | 1u << 19)));
            table = sgx_fp(g, "snx_x64_fp_minmax", 0, table, o, 2, 1,
                           bits | (SnxU32)(insn->op == SNX_A64_OP_FAMIN) << 8, bits);
            sg_d_set_slice(g, &r, (e + 1) * bits - 1, e * bits, table);
        }
        sg__V_write(g, rd, r);
        return 1;
    case SNX_A64_OP_LUTI2_1b:
    case SNX_A64_OP_LUTI2_1h:
    case SNX_A64_OP_LUTI4_1b:
    case SNX_A64_OP_LUTI4_2h: {
        unsigned by = insn->op == SNX_A64_OP_LUTI2_1b || insn->op == SNX_A64_OP_LUTI2_1h ? 2 : 4; /* a number's bits */
        unsigned at;
        bits = insn->op == SNX_A64_OP_LUTI2_1b || insn->op == SNX_A64_OP_LUTI4_1b ? 8 : 16;
        n = 128 / bits;
        at = (unsigned)a->idx * n * by; /* the segment of Vm */
        table = vbits(g, rn, 127, 0);
        if (insn->op == SNX_A64_OP_LUTI4_2h) table = sg_d_concat(g, vbits(g, (rn + 1) & 31, 127, 0), table);
        r = sg_d_empty(128);
        for (e = 0; e < n; ++e) {
            SgI i = sg_i_uint(g, vbits(g, rm, at + (e + 1) * by - 1, at + e * by));
            sg_d_set_slice(g, &r, (e + 1) * bits - 1, e * bits, sg_d_elem_dyn(g, table, i, bits));
        }
        sg__V_write(g, rd, r);
        return 1;
    }
    default: return 0;
    }
    r = sg_FPToFixed(g, 0, vbits(g, rn, fsize - 1, 0), 0, uns, sg_FPCR_read(g), round, isize);
    sg__V_write(g, rd, sg_d_zext(g, r, 128));
    return 1;
}

/* FEAT_FP8's, as FPMR zero has them (E5M2, no scale, no saturation): the state holds no FPMR, and an MSR or MRS of it
   is undefined, so zero is what it stays */
static int fp8(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31, q = word >> 30 & 1, e, n, bits, count;
    unsigned byte = (word >> 11 & 1) << 3 | (word >> 19 & 7), lane = 0; /* by element: Vm's byte; Vn's in the group */
    int by_element = 0;
    SgD r = sg_d_empty(128), args[4];
    switch (insn->op) {
    case SNX_A64_OP_FSCALE:
        bits = 8u << insn->args.esz;
        for (e = 0; e < (q ? 128 : 64) / bits; ++e) {
            args[0] = vbits(g, rn, (e + 1) * bits - 1, e * bits);
            args[1] = vbits(g, rm, (e + 1) * bits - 1, e * bits);
            args[0] = sgx_fp(g, "snx_x64_fp_scale", 0, sg_FPCR_read(g), args, 2, 1, bits, bits);
            sg_d_set_slice(g, &r, (e + 1) * bits - 1, e * bits, args[0]);
        }
        sg__V_write(g, rd, r);
        return 1;
    case SNX_A64_OP_F1CVTL:
    case SNX_A64_OP_F2CVTL:
    case SNX_A64_OP_BF1CVTL:
    case SNX_A64_OP_BF2CVTL:
        for (e = 0; e < 8; ++e) {
            args[0] = sg_d_zext(g, vbits(g, rn, 64 * q + 8 * e + 7, 64 * q + 8 * e), 64);
            args[1] = sgx_const(32, insn->op == SNX_A64_OP_BF1CVTL || insn->op == SNX_A64_OP_BF2CVTL);
            args[0] = sg_d_slice(g, sgx_call(g, "snx_x64_fp8_widen", 0, 1, args, 2, 64), 15, 0);
            sg_d_set_slice(g, &r, 16 * e + 15, 16 * e, args[0]);
        }
        sg__V_write(g, rd, r);
        return 1;
    case SNX_A64_OP_FCVTN_bh:
    case SNX_A64_OP_FCVTN_bs:
        bits = insn->op == SNX_A64_OP_FCVTN_bh ? 16 : 32;
        n = bits == 16 && !q ? 4 : 128 / bits; /* of Vn, and then of Vm */
        count = bits == 32 && q ? 64 : 0;      /* FCVTN2's: the register's top half, the other kept */
        if (count) sg_d_set_slice(g, &r, 63, 0, vbits(g, rd, 63, 0));
        for (e = 0; e < 2 * n; ++e) {
            unsigned at = e % n * bits, to = count + 8 * e;
            args[0] = sg_d_zext(g, vbits(g, e < n ? rn : rm, at + bits - 1, at), 64);
            args[1] = sgx_const(32, bits);
            args[0] = sg_d_slice(g, sgx_call(g, "snx_x64_fp8_narrow", 0, 1, args, 2, 64), 7, 0);
            sg_d_set_slice(g, &r, to + 7, to, args[0]);
        }
        sg__V_write(g, rd, r);
        return 1;
    /* sums of n products of bytes, each into an element of bits */
    case SNX_A64_OP_FDOT_sb_vi:
        byte = 4 * ((word >> 11 & 1) << 1 | (word >> 21 & 1));
        by_element = 1; /* fallthrough */
    case SNX_A64_OP_FDOT_sb_v: bits = 32, n = 4, count = q ? 4 : 2; break;
    case SNX_A64_OP_FDOT_hb_vi:
        byte = 2 * ((word >> 11 & 1) << 2 | (word >> 20 & 3));
        rm &= 15;
        by_element = 1; /* fallthrough */
    case SNX_A64_OP_FDOT_hb_v: bits = 16, n = 2, count = q ? 8 : 4; break;
    case SNX_A64_OP_FMLAL_hb_vi:
        rm &= 7;
        by_element = 1; /* fallthrough */
    case SNX_A64_OP_FMLAL_hb_v: bits = 16, n = 1, count = 8, lane = q; break;
    case SNX_A64_OP_FMLALL_sb_vi:
        rm &= 7;
        by_element = 1; /* fallthrough */
    case SNX_A64_OP_FMLALL_sb_v: bits = 32, n = 1, count = 4, lane = q << 1 | (word >> 22 & 1); break;
    case SNX_A64_OP_FMMLA_sb: bits = 32, n = 8, count = 4; break;
    case SNX_A64_OP_FMMLA_hb: bits = 16, n = 4, count = 8; break;
    default: return 0;
    }
    for (e = 0; e < count; ++e) {
        unsigned an, am;
        if (n * 8 > bits) /* a matrix of 2 rows and 2 columns in each bits * 4 */
            an = e / 4 * 64 + (e >> 1 & 1) * 8 * n, am = e / 4 * 64 + (e & 1) * 8 * n;
        else
            an = am = e * bits + 8 * lane;
        if (by_element) am = 8 * byte;
        args[0] = sg_d_zext(g, vbits(g, rd, (e + 1) * bits - 1, e * bits), 64);
        args[1] = sg_d_zext(g, vbits(g, rn, an + 8 * n - 1, an), 64);
        args[2] = sg_d_zext(g, vbits(g, rm, am + 8 * n - 1, am), 64);
        args[3] = sgx_const(32, bits | n << 8);
        args[0] = sg_d_slice(g, sgx_call(g, "snx_x64_fp8_dot", 0, 0, args, 4, 64), (long long)bits - 1, 0);
        sg_d_set_slice(g, &r, (e + 1) * bits - 1, e * bits, args[0]);
    }
    sg__V_write(g, rd, r);
    return 1;
}

/* FEAT_CMPBR's: CB<cc> of two registers (CBB<cc>'s and CBH<cc>'s low byte or halfword), or of one and a 6-bit
   integer */
static void compare_branch(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    /* the comparisons sgx_d_cmp64 has: == != < <= > >=; the ones cc names, of registers and of an integer */
    static const signed char reg[8] = {4, 5, 4, 5, 0, 0, 0, 1}, imm[8] = {4, 2, 4, 2, 0, 0, 0, 1};
    SgCtx* g = u->g;
    unsigned cc = word >> 21 & 7, bits;
    int is_signed = cc < 2, by_reg = insn->op == SNX_A64_OP_CB_cond;
    SgD a = xzr_or(g, word & 31), b;
    bits = by_reg ? 8u << insn->args.esz : word >> 31 ? 64 : 32;
    a = sg_d_slice(g, a, bits - 1, 0);
    b = by_reg ? sg_d_slice(g, xzr_or(g, word >> 16 & 31), bits - 1, 0) : sgx_const(bits, word >> 15 & 63);
    a = is_signed ? sg_d_sext(g, a, 64) : sg_d_zext(g, a, 64);
    b = is_signed ? sg_d_sext(g, b, 64) : sg_d_zext(g, b, 64);
    g->taken = sgx_d_cmp64(g, by_reg ? reg[cc] : imm[cc], a, b, !is_signed);
    g->branched = 1;
}

/* FEAT_LSE128's LDCLRP, LDSETP and SWPP: CASP's loop, the pair read until no other write came between */
static void pair_atomic(Unit* u, const SnxA64Insn* insn, SnxU32 word, SnxU64 address) {
    SgCtx* g = u->g;
    unsigned rt = word & 31, rn = word >> 5 & 31, rt2 = word >> 16 & 31;
    int again = new_label(u);
    SgD base, old, value, next, got;
    flush_to(u, address);
    bind(u, again);
    base = sg_GPR_read(g, rn);
    old = sg_d_concat(g, sg_GuestMemRead(g, sg_d_add_int(g, base, 8), 8, 0), sg_GuestMemRead(g, base, 8, 0));
    value = sg_d_concat(g, sg_GPR_read(g, rt2), sg_GPR_read(g, rt));
    next = insn->op == SNX_A64_OP_LDCLRP   ? sg_d_and(g, old, sg_d_not(g, value))
           : insn->op == SNX_A64_OP_LDSETP ? sg_d_or(g, old, value)
                                           : value;
    got = sg_GuestCompareSwap(g, base, old, next);
    jump_if(u, sg_b_not(g, sg_d_eq(g, got, old)), again, address);
    sg_GPR_write(g, rt, sg_d_slice(g, got, 63, 0));
    sg_GPR_write(g, rt2, sg_d_slice(g, got, 127, 64));
}

/* FEAT_MOPS's SET and CPY: option A (PSTATE.C 0), the whole of it in the first of the three instructions, as it may be;
   the other two do what Xn still holds, nothing after it. A copy that must go backward leaves Xd and Xs as they were */
static int memory_op(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    unsigned rd = word & 31, rn = word >> 5 & 31, rs = word >> 16 & 31;
    SgD d, s, n, args[3], zero = sgx_const(64, 0);
    SgB back;
    int set = 0, first = 0;
    switch (insn->op) {
    case SNX_A64_OP_SETP:
    case SNX_A64_OP_SETGP: first = 1; /* fallthrough */
    case SNX_A64_OP_SETM:
    case SNX_A64_OP_SETE:
    case SNX_A64_OP_SETGM:
    case SNX_A64_OP_SETGE: set = 1; break;
    case SNX_A64_OP_CPYP:
    case SNX_A64_OP_CPYFP: first = 1; break;
    case SNX_A64_OP_CPYM:
    case SNX_A64_OP_CPYE:
    case SNX_A64_OP_CPYFM:
    case SNX_A64_OP_CPYFE: break;
    default: return 0;
    }
    d = sg_GPR_read(g, rd);
    n = sg_GPR_read(g, rn);
    s = xzr_or(g, rs);
    if (first) {
        SgD most = sgx_const(64, set ? 0x7FFFFFFFFFFFFFFFull : 0x007FFFFFFFFFFFFFull);
        n = sg_d_select(g, sgx_d_ult(g, most, n), most, n);
        /* backward: Xd past Xs and before its end */
        back = sg_bool(g, 0);
        if (insn->op == SNX_A64_OP_CPYP) back = sg_b_and(g, sgx_d_ult(g, s, d), sgx_d_ult(g, sg_d_sub(g, d, s), n));
        args[0] = d, args[1] = set ? n : s, args[2] = set ? s : n;
        sgx_call(g, set ? "snx_x64_set" : "snx_x64_copy", 0, 1, args, 3, 0);
        sg_GPR_write(g, rd, sg_d_select(g, back, d, sg_d_add(g, d, n)));
        if (!set) sg_GPR_write(g, rs, sg_d_select(g, back, s, sg_d_add(g, s, n)));
        sg_PSTATE_write(g, SG_PSTATE_N, 4, sgx_const(4, 0));
    } else {
        /* Xn below 0: so many bytes before Xd (and Xs); above, a copy backward's, from Xd (and Xs) on */
        SgB forward = sgx_d_cmp64(g, 2, n, zero, 0);
        SgD left = sg_d_select(g, forward, sg_d_sub(g, zero, n), n);
        SgD to = sg_d_select(g, forward, sg_d_add(g, d, n), d);
        args[0] = to;
        args[1] = set ? left : sg_d_select(g, forward, sg_d_add(g, s, n), s);
        args[2] = set ? s : left;
        sgx_call(g, set ? "snx_x64_set" : "snx_x64_copy", 0, 1, args, 3, 0);
    }
    sg_GPR_write(g, rn, zero);
    return 1;
}

/* instructions past the guests' CPUs that are others', or nothing, where EL0 runs: 1 made, -1 UNDEF, 0 the Sail's (of
   g->word). No allocation tag is kept (FEAT_MTE): STG and ST2G only move their base, the zeroing forms clear their
   16-byte granules too, LDG gives a tag of 0 and STGP stores its pair; the forms of many tags are
   EL1's */
static int past_guests(Unit* u, const SnxA64Insn* insn, SnxU32 word, SnxU64 address_of) {
    SgCtx* g = u->g;
    const SnxA64Args* a = &insn->args;
    SnxU64 offset = (SnxU64)(int64_t)a->imm;
    SgD base, address;
    int k, bytes = 0;
    switch (insn->op) {
    /* PSTATE.DIT, SSBS and TCO change nothing this CPU keeps */
    case SNX_A64_OP_MSR_i_DIT:
    case SNX_A64_OP_MSR_i_SBSS:
    case SNX_A64_OP_MSR_i_TCO: return 1;
    /* a speculation barrier is a whole one, and DSB's nXS form is DSB */
    case SNX_A64_OP_SB:
    case SNX_A64_OP_DSB_nXS: g->word = 0xD5033F9Fu; return 0;
    /* waits that may end at any time: YIELD */
    case SNX_A64_OP_WFET:
    case SNX_A64_OP_WFIT: g->word = 0xD503203Fu; return 0;
    /* no guarded control stack and no streaming mode, and the array forms of the tags are EL1's */
    case SNX_A64_OP_MSR_i_SVCR:
    case SNX_A64_OP_GCSSTR:
    case SNX_A64_OP_LDGM:
    case SNX_A64_OP_STGM:
    case SNX_A64_OP_STZGM: return -1;
    case SNX_A64_OP_LDG:
        if (a->rt != 31)
            sg_GPR_write(g, a->rt, sg_d_and(g, sg_GPR_read(g, a->rt), sgx_const(64, ~((SnxU64)15 << 56))));
        return 1;
    case SNX_A64_OP_STGP: offset <<= 4; bytes = -1; break;
    case SNX_A64_OP_STZG: bytes = 16; break;
    case SNX_A64_OP_STZ2G: bytes = 32; break;
    case SNX_A64_OP_STG:
    case SNX_A64_OP_ST2G: break;
    case SNX_A64_OP_LDCLRP:
    case SNX_A64_OP_LDSETP:
    case SNX_A64_OP_SWPP: pair_atomic(u, insn, word, address_of); return 1;
    default: return memory_op(u, insn, word) || newer(u, insn, word) || fp8(u, insn, word);
    }
    base = sg_GPR_read(g, a->rn);
    address = a->p ? base : sg_d_add_int(g, base, (long long)offset);
    if (bytes < 0) {
        sg_GuestMemWrite(g, address, 8, 0, xzr_or(g, (unsigned)a->rt));
        sg_GuestMemWrite(g, sg_d_add_int(g, address, 8), 8, 0, xzr_or(g, (unsigned)a->rt2));
    }
    for (k = 0; k < bytes; k += 8) sg_GuestMemWrite(g, sg_d_add_int(g, address, k), 8, 0, sgx_const(64, 0));
    if (a->w) sg_GPR_write(g, a->rn, a->p ? sg_d_add_int(g, base, (long long)offset) : address);
    return 1;
}

/* u->f can't be made, for its instruction u->at (its count: for no instruction's), or for what failed there (why, else
   NULL): said, once a function, with what it becomes */
static void no_code(Unit* u, const char* why) {
    const SnxFunction* f = u->f;
    const char* becomes = "it raises an undefined instruction where it runs";
    SnxU64 address;
    SnxU32 word = 0;
    SnxA64Insn insn;
    if (u->options->refused) ++*u->options->refused;
    if (u->at >= f->count) {
        snx_error("native back end: the function at 0x%llx: %s", (unsigned long long)f->start,
                  why ? why : "longer than its branches reach");
        return;
    }
    address = snx_function_address(f, u->at);
    (void)snx_codegen_word((SnxCodegen*)u->codegen, address, &word);
    snx_a64_decode_word(word, u->codegen->features, &insn);
    if (why) {
        snx_error("native back end: the function at 0x%llx: %s, at or before %s (%08x) at 0x%llx: %s",
                  (unsigned long long)f->start, why, snx_a64_op_text(insn.op), (unsigned)word,
                  (unsigned long long)address, becomes);
        return;
    }
    SnxU64 past = insn.feature & ~(SnxU64)SNX_A64_TARGET_OUNCE;
    snx_error("native back end: the function at 0x%llx: %s (%08x) at 0x%llx %s%s, which it has no code for (the "
              "guest's target is %s): %s",
              (unsigned long long)f->start, snx_a64_op_text(insn.op), (unsigned)word, (unsigned long long)address,
              past ? "needs " : "is one", past ? snx_a64_feature_text(past) : "",
              snx_a64_target_text(u->codegen->features), becomes);
}

/* a call's reload of the pins: those live where it returns (the next instruction); and past it, until the next
   instruction, every pin in sync with the state (stored before it, loaded after) or dead. Past undef's or brk's, which
   may change the state and which nothing is loaded after, the state is the newer: the exit stores none */
static void call_masks(Unit* u, size_t i) {
    const SnxFunction* f = u->f;
    if (u->pinned && u->live && i + 1 < f->count && snx_function_address(f, i + 1) == snx_function_address(f, i) + 4)
        u->pin_sel = u->live[i + 1], u->vpin_sel = u->vlive ? u->vlive[i + 1] : ~0u;
}
static void after_call(Unit* u) {
    u->pin_sel = ~(SnxU64)0, u->vpin_sel = ~0u;
    u->cur_dirty = 0, u->vcur_dirty = 0;
}

#ifdef MG_JTAB
/* the table of the BR at address (snx_codegen_jump_table's cases), its slots from the first case to the last, each the
   case's leader label or the dispatch block's: its index in u->jtabs, -1 where it has no case in the function or is
   too sparse */
static int jtab_new(Unit* u, SnxU64 address) {
    SnxU64 t[SNX_JUMP_TABLE_MAX], lo = ~(SnxU64)0, hi = 0;
    size_t n = snx_codegen_jump_table((SnxCodegen*)u->codegen, address, t, SNX_JUMP_TABLE_MAX), k, cases = 0;
    const SnxFunction* f = u->f;
    struct JTab* j;
    unsigned span, s;
    for (k = 0; k < n; ++k) {
        size_t at;
        if (!snx_function_contains(f, t[k]) || !f->leaders[at = index_of(f, t[k])] || snx_function_address(f, at) != t[k])
            continue;
        lo = t[k] < lo ? t[k] : lo, hi = t[k] > hi ? t[k] : hi;
        t[cases++] = t[k];
    }
    /* a function of few leaders: the dispatch block's compares, as many, are quicker (its matching jump is to a
       label the predictor knows; the table's waits on its load) */
    if (!cases || (hi - lo) / 4 >= 1024 || f->leader_count <= MG_JTAB_LEADERS) return -1;
    span = (unsigned)((hi - lo) / 4 + 1);
    u->jtabs = (struct JTab*)grow(u->jtabs, &u->cap_jtabs, (size_t)u->njtabs + 1, sizeof *u->jtabs);
    u->jslots = (int*)grow(u->jslots, &u->cap_jslots, u->njslots + span, sizeof *u->jslots);
    if (!u->jtabs || !u->jslots) return -1;
    j = &u->jtabs[u->njtabs];
    j->label = new_label(u);
    j->lo = lo;
    j->n = span;
    j->first = (unsigned)u->njslots;
    for (s = 0; s < span; ++s) u->jslots[j->first + s] = u->d_label;
    for (k = 0; k < cases; ++k) u->jslots[j->first + (t[k] - lo) / 4] = (int)index_of(f, t[k]);
    u->njslots += span;
    return u->njtabs++;
}
#endif

static int instruction(Unit* u, size_t index, SnxU64 address, SnxU32 word) {
    SgCtx* g = u->g;
    SgX* x = &g->x;
    const SnxFunction* f = u->f;
    SnxA64Insn insn;
    SnxU64 next = address + 4, target;
    int terminal = 0;
    snx_a64_decode_word(word, u->codegen->features, &insn);
    /* a long block is several regions: the guest's registers are in the state at each one's end */
    if (x->n > SGX_MAXI - SGX_MAXI / 4 || x->nv > SGX_MAXV - SGX_MAXV / 4 || x->nb > SGX_MAXB - SGX_MAXB / 4)
        flush_to(u, address);
    /* the branches leave the region, and MRS and MSR are FPSR's; before a compare, its flags kept for the branch */
    if (g->qc_owed && (insn.flow != SNX_A64_FLOW_PLAIN || insn.op == SNX_A64_OP_SYS || writes_nzcv(word))) qc_pay(u);
    g->word = word;
    g->pc = address;
    g->start = x->n;
    g->branched = g->undefined = 0;
    g->taken = 0;
    g->exclusive_count = g->exclusive_next = 0;
    g->status = 0;
    g->qc = 0;
    for (size_t k = 0; k < u->nnone; ++k)
        if (u->none[k] == address) goto undef;
    for (size_t k = 0; k < u->options->without_count; ++k)
        if (u->options->without[k] == word) return 0;
    switch (insn.flow) {
    case SNX_A64_FLOW_UNDEF:
    undef:
        flush(u);
        mg_undef(u, address, word);
        after_call(u);
        leave(u);
        return 1;
    case SNX_A64_FLOW_PLAIN:
        if (insn.op == SNX_A64_OP_SYS) {
            int k = sys(u, word);
            if (k < 0) goto undef;
            if (!k) return 0;
            break;
        }
        if (bitfield(u, word) || reverse(u, word) || fp_ldst(u, word, address) || simd_imm(u, word) ||
            simd_scalar_cvt(u, word) || simd_copy(u, word))
            break;
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
        if (insn.feature & ~(SnxU64)SNX_A64_TARGET_OUNCE) {
            int v = past_guests(u, &insn, word, address);
            if (v < 0) goto undef;
            if (v) break;
        }
        if (insn.op == SNX_A64_OP_LDXR || insn.op == SNX_A64_OP_LDXP || insn.op == SNX_A64_OP_STXR) {
            /* the fields the encoding wants all ones, which the Sail's decode reads so and the processor ignores: Rt2
               of a single's, Rs of a load's */
            if (insn.op != SNX_A64_OP_LDXP) g->word |= 31u << 10;
            if (insn.op != SNX_A64_OP_STXR) g->word |= 31u << 16;
        }
        if ((word >> 25 & 7) == 7 && (x->n > SGX_MAXI / 4 || x->nv > SGX_MAXV / 4 || x->nb > SGX_MAXB / 4)) {
            /* an Advanced SIMD instruction by its elements may take most of a region: it begins one */
            flush_to(u, address);
            g->start = x->n;
        }
        if (!sg_decode(g) || x->unmade) return 0;
        if (g->undefined) {
            /* the Sail's UNDEF (SGX_K_UNDEF set the pc and called the runtime): the function returns */
            if (g->qc_owed) { /* FPSR before that call, the instruction's last */
                --x->n;
                qc_pay(u);
                sg_undefined(g);
            }
            flush(u);
            after_call(u);
            leave(u);
            return 1;
        }
        break;
    case SNX_A64_FLOW_B:
        target = snx_a64_branch_target(&insn, address);
        flush_to(u, target);
        if (snx_function_contains(f, target)) {
#ifdef MG_POLL_INLINE
            if (target <= address) {
                poll_jz(u, (int)index_of(f, target));
                jmp(u, add_stub(u, 3, target, address));
                terminal = 1;
                break;
            }
#endif
            if (target <= address) poll_then(u, target);
            jmp(u, (int)index_of(f, target));
        } else if (!call_or_stub(u, target, 1))
            return 0;
        terminal = 1;
        break;
    case SNX_A64_FLOW_BC:
        if (insn.op == SNX_A64_OP_CB_cond || insn.op == SNX_A64_OP_CB_cond_imm) compare_branch(u, &insn, word);
        else if (!sg_decode(g) || x->unmade || !g->branched)
            return 0;
        target = snx_a64_branch_target(&insn, address);
        if (g->taken) {
            /* the region goes on past the JCC: what the block knows (its registers' values, the flags, constants)
               holds where the branch isn't taken. The taken path finds the guest's state where a label wants it:
               every write stored (or PSET) where it was made, and the store pass keeps the stores before a machine
               instruction */
#ifdef MG_POLL_INLINE
            if (target <= address && snx_function_contains(f, target))
                jump_if_back(u, g->taken, (int)index_of(f, target), add_stub(u, 3, target, address), target);
            else
#endif
                jump_if(u, g->taken, branch_label(u, address, target), target);
            if (target <= address && u->exits && u->exits[index]) {
                /* a loop's latch: the registers the loop doesn't read again (dead at its header) but the code after
                   it may are stored here, on the way out alone: the store pass then drops the ones in the body, dead
                   on both ways now */
                SnxU64 dead = dead_at(u, target);
                SnxU32 vdead = vdead_at(u, target);
                int k;
                for (k = 0; k < 31; ++k)
                    if ((dead >> k & 1) && g->gpr[k] && !(g->pinned && g->pin[k] >= 0)) {
                        SgXIns* st = sgx_append(x, SGX_K_STORE);
                        st->w = 64;
                        st->in[0] = g->gpr[k];
                        st->disp = (int)(offsetof(CPU, x) + 8 * (size_t)k);
                    }
                for (k = 0; k < 32; ++k)
                    if ((vdead >> k & 1) && !(g->pinned && g->vpin[k] >= 0)) {
                        if (g->vx[k]) {
                            int in[4] = {0, g->vx[k], 0, 0};
                            sgx_vins(x, SGX_VF_ST16, 0, 0, in, 0, 0, SGX_VM_STATE, 0, 0, sgx_vd_offset(k, 0), 0, 0);
                        } else {
                            int h;
                            for (h = 0; h < 2; ++h)
                                if (g->vd[k][h]) {
                                    SgXIns* st = sgx_append(x, SGX_K_STORE);
                                    st->w = 64;
                                    st->in[0] = g->vd[k][h];
                                    st->disp = sgx_vd_offset(k, h);
                                }
                        }
                    }
                /* past it the loop has ended, so what the block knows is let go (read again from the state or the
                   pins once) rather than kept live, and spilled, through the loop's body */
                for (k = 0; k < 32; ++k) g->gpr[k] = g->vd[k][0] = g->vd[k][1] = g->vx[k] = g->gk_known[k] = 0;
                g->mem = 0;
            }
            break;
        }
        flush_to(u, target);
        jmp(u, branch_label(u, address, target));
        break;
    case SNX_A64_FLOW_BL:
        target = snx_a64_branch_target(&insn, address);
        sg_GPR_write(g, 30, sgx_const(64, next));
        flush(u);
        call_masks(u, index);
        if (!call_or_stub(u, target, 0)) return 0;
        after_call(u);
        check_return(u, next);
        break;
    case SNX_A64_FLOW_BLR:
        if (!sg_decode(g) || x->unmade || !g->branched) return 0; /* it writes the link register */
        machine_target(u, SGX_FN_CALLDYN, g->target);
        call_masks(u, index);
        flush(u);
        after_call(u);
        check_return(u, next);
        break;
    case SNX_A64_FLOW_BR: {
        /* a PLT stub whose slot is bound, entered as a function (through a pointer, or by a call that does not run
           it in place): if (target == bound) F_bound, always compared, as the stub's own branch has the slot's word */
        int guard = 0;
#ifdef MG_BOUND
        SnxU64 bound = bound_function(u, address, &guard);
#else
        SnxU64 bound = 0;
        (void)guard;
#endif
        if (!sg_decode(g) || x->unmade || !g->branched) return 0;
#ifdef MG_JTAB
        if (u->dispatch && !bound) {
            int k = jtab_new(u, address);
            if (k >= 0) {
                /* a switch: CPU.pc for the dispatch block's way, then the table's jump */
                SgD t = g->target;
                int v = sgx_reg(x, &t);
                SgXIns* st = sgx_append(x, SGX_K_STORE);
                SgXIns* j;
                st->w = 64;
                st->in[0] = v;
                st->disp = (int)offsetof(CPU, pc);
                j = sgx_append(x, SGX_K_MACHINE);
                j->op = SGX_FN_JTAB;
                j->in[0] = v;
                j->fixed[0] = SGX_FN_TARGET;
                j->imm = k;
                j->mclobber = MG_JTAB_CLOBBER;
                flush(u);
                terminal = 1;
                break;
            }
        }
#endif
        if (u->dispatch || bound) {
            SgD t = g->target;
            int v = sgx_reg(x, &t);
            SgXIns* st = sgx_append(x, SGX_K_STORE);
            st->w = 64;
            st->in[0] = v;
            st->disp = (int)offsetof(CPU, pc);
            flush(u);
#ifdef MG_BOUND
            if (bound) {
                int other = new_label(u);
                mg_if_pc_not(u, bound, other);
                call_target(u, bound, 1);
                bind(u, other);
            }
            if (!u->dispatch) {
                mg_target_pc(u);
                calldyn(u, 1);
            } else
#endif
                jmp(u, u->d_label);
        } else {
            machine_target(u, SGX_FN_TAILDYN, g->target);
            flush(u);
        }
        terminal = 1;
        break;
    }
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
        mg_svc(u, next, n, u->codegen->direct_svc && n < SNX_SVC_COUNT);
        break;
    }
    case SNX_A64_FLOW_BRK:
        flush(u);
        mg_brk(u, address, (word >> 5) & 0xFFFF);
        after_call(u);
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

/* ---- liveness: which guest registers and flags an instruction reads and writes ----
   Bits 0-30 X0-X30, 31 SP, 32-35 N Z C V. What reads is overstated where unsure (all of them: a call, a return, an
   SVC, an encoding not classified here) and what writes understated (never SP, never XZR, and nothing an
   instruction only may write), so a register is only dead where nothing can read it */
#define LIVE_ALL (~(SnxU64)0 >> 28)
#define LIVE_NZCV ((SnxU64)15 << 32)
static SnxU64 live_r(unsigned r) { return (SnxU64)1 << (r & 31); }
static SnxU64 live_d(unsigned r) { return (r & 31) == 31 ? 0 : (SnxU64)1 << (r & 31); }
static void regs_of(SnxU32 w, SnxU64* use, SnxU64* def) {
    unsigned rd = w & 31, rn = w >> 5 & 31, rm = w >> 16 & 31, ra = w >> 10 & 31;
    *use = LIVE_ALL, *def = 0;
    if ((w >> 26 & 7) == 4) { /* data processing (immediate) */
        switch (w >> 23 & 7) {
        case 0:
        case 1: *use = 0, *def = live_d(rd); return;                                  /* ADR, ADRP */
        case 2: *use = live_r(rn), *def = live_d(rd) | (w >> 29 & 1 ? LIVE_NZCV : 0); return; /* ADD, SUB */
        case 4: *use = live_r(rn), *def = live_d(rd) | ((w >> 29 & 3) == 3 ? LIVE_NZCV : 0); return; /* logical */
        case 5:
            if ((w >> 29 & 3) == 1) return;
            *use = (w >> 29 & 3) == 3 ? live_r(rd) : 0, *def = live_d(rd); /* MOVN, MOVZ, MOVK */
            return;
        case 6: *use = live_r(rn) | ((w >> 29 & 3) == 1 ? live_r(rd) : 0), *def = live_d(rd); return; /* bitfield */
        case 7: *use = live_r(rn) | live_r(rm), *def = live_d(rd); return;               /* EXTR */
        default: return;
        }
    }
    if ((w >> 26 & 7) == 5) { /* branches, exceptions, system */
        if ((w >> 26 & 0x1F) == 5) {
            if (!(w >> 31)) *use = 0; /* B: its successor says; BL calls */
            return;
        }
        if ((w >> 25 & 0x3F) == 0x1A || (w >> 25 & 0x3F) == 0x1B) { /* CBZ, CBNZ, TBZ, TBNZ */
            *use = live_r(rd);
            return;
        }
        if ((w >> 24) == 0x54) { /* B.cond */
            *use = LIVE_NZCV;
            return;
        }
        if ((w & 0xFFFFF01F) == 0xD503201F) { /* hints: NOP, YIELD...; but XPACLRI's X30 and CHKFEAT's X16 */
            if ((w >> 5 & 127) != 7 && (w >> 5 & 127) != 40) *use = 0;
            return;
        }
        if ((w >> 20) == 0xD53) { /* MRS */
            *use = LIVE_NZCV, *def = live_d(rd);
            return;
        }
        return;
    }
    if ((w >> 25 & 5) == 4) { /* loads and stores */
        unsigned v = w >> 26 & 1;
        if ((w >> 27 & 7) == 5) { /* pairs */
            unsigned l = w >> 22 & 1, wb = (w >> 23 & 1) && (w >> 23 & 3) != 2;
            *use = live_r(rn);
            if ((w >> 23 & 3) == 1 || (w >> 23 & 3) == 3) *def |= live_d(rn);
            (void)wb;
            if (!v) {
                if (l) *def |= live_d(rd) | live_d(ra);
                else
                    *use |= live_r(rd) | live_r(ra);
            }
            return;
        }
        if ((w >> 27 & 7) == 3 && !(w >> 24 & 1)) { /* literal */
            *use = 0;
            if (!v && (w >> 30) != 3) *def = live_d(rd);
            return;
        }
        if ((w >> 27 & 7) == 7) { /* single registers */
            unsigned opc = w >> 22 & 3, size = w >> 30, store, prfm, idx = w >> 10 & 3;
            if (!(w >> 24 & 1)) {
                if (w >> 21 & 1) {
                    if (idx != 2) return; /* atomics, PAC loads: all */
                } else if (idx == 2)
                    return; /* LDTR, STTR: the same, but leave them */
            }
            *use = live_r(rn);
            if (!(w >> 24 & 1) && (w >> 21 & 1)) *use |= live_r(rm); /* register offset */
            if (!(w >> 24 & 1) && !(w >> 21 & 1) && (idx == 1 || idx == 3)) *def |= live_d(rn); /* writeback */
            if (v) return;
            store = opc == 0;
            prfm = size == 3 && opc == 2;
            if (size == 3 && opc == 3) {
                *use = LIVE_ALL, *def = 0;
                return;
            }
            if (prfm) return;
            if (store) *use |= live_r(rd);
            else
                *def |= live_d(rd);
            return;
        }
        if (v && (w >> 31) == 0 && (w >> 23 & 0x3E) == 0x18) { /* SIMD structures: the base, the post index */
            *use = live_r(rn) | live_r(rm);
            return;
        }
        return;
    }
    if ((w >> 25 & 7) == 5) { /* data processing (register) */
        if (!(w >> 28 & 1)) {
            /* logical (shifted), add/subtract (shifted, extended) */
            *use = live_r(rn) | live_r(rm);
            if ((w >> 24 & 0x1F) == 0x0A) *def = live_d(rd) | ((w >> 29 & 3) == 3 ? LIVE_NZCV : 0);
            else
                *def = live_d(rd) | (w >> 29 & 1 ? LIVE_NZCV : 0);
            return;
        }
        switch (w >> 21 & 0xF) {
        case 0:
            if (w >> 10 & 0x3F) return; /* RMIF, SETF */
            *use = live_r(rn) | live_r(rm) | LIVE_NZCV, *def = live_d(rd) | (w >> 29 & 1 ? LIVE_NZCV : 0); /* ADC */
            return;
        case 2: *use = live_r(rn) | live_r(rm) | LIVE_NZCV, *def = LIVE_NZCV; return; /* CCMN, CCMP */
        case 4: *use = live_r(rn) | live_r(rm) | LIVE_NZCV, *def = live_d(rd); return; /* CSEL... */
        case 6: /* 1 and 2 sources: a 1-source PAC (PACIA Xd, Xn) reads Xd too */
            *use = live_r(rn) | live_r(rm) | (w >> 30 & 1 ? live_r(rd) : 0), *def = live_d(rd);
            return;
        default:
            if (w >> 24 & 1) { /* 3 sources */
                *use = live_r(rn) | live_r(rm) | live_r(ra), *def = live_d(rd);
                return;
            }
            return;
        }
    }
    if ((w >> 25 & 7) == 7) { /* SIMD and FP: the general-purpose registers of transfers and conversions */
        *use = 0;
        /* each test alone: a form may be in more than one of these groups */
        if ((w & 0x5F000000) == 0x1E000000) *use |= live_r(rn); /* scalar FP: Rn may be a GPR (conversions from
                                                                 integers and fixed point, FMOV) */
        if ((w & 0x9FE08400) == 0x0E000400) *use |= live_r(rn); /* DUP, INS (general), UMOV, SMOV */
        if ((w & 0xFF200C00) == 0x1E200400 || (w & 0xFF200C00) == 0x1E200C00)
            *use |= LIVE_NZCV; /* FCCMP, FCSEL */
        else if ((w & 0xFF203C00) == 0x1E202000)
            *def = LIVE_NZCV; /* FCMP, FCMPE */
        return;
    }
}

/* the V registers an instruction reads and writes, as regs_of: a write only where it is the whole register and
   nothing of it is read (loads, scalar FP, TBL, the 3-same forms that don't accumulate), every register field read
   otherwise; a register list (TBL's, a structure store's) read whole */
static SnxU32 vreg_run(unsigned first, unsigned n) {
    SnxU32 m = 0;
    unsigned k;
    for (k = 0; k < n; ++k) m |= 1u << ((first + k) & 31);
    return m;
}
static void vregs_of(SnxU32 w, SnxU32* use, SnxU32* def) {
    unsigned rd = w & 31, rn = w >> 5 & 31, rm = w >> 16 & 31, ra = w >> 10 & 31;
    /* the register fields a form has: Rm (and Rm & 15: a by-element form of halfwords takes it from bits 19-16, bit
       20 is the index's) but in the groups whose bits 20-16 are an opcode or an immediate (2-register misc, across
       lanes, copy, shift and modified immediate, scalar FP's conversions); Ra in the 4-register crypto forms alone
       (EOR3, BCAX, SM3SS1: elsewhere bits 14-10 are an opcode or an immediate) */
    int no_rm = (w & 0x0F3E0C00) == 0x0E200800 || (w & 0x0F3E0C00) == 0x0E300800 || (w & 0x0FE08400) == 0x0E000400 ||
                (w & 0x0F800400) == 0x0F000400 || (w & 0x5F20FC00) == 0x1E200000 || (w & 0x5F200000) == 0x1E000000;
    SnxU32 fields = 1u << rd | 1u << rn | (no_rm ? 0 : 1u << rm | 1u << (rm & 15)) |
                    ((w & 0xFF800000) == 0xCE000000 ? 1u << ra : 0);
    *def = 0;
    if ((w >> 25 & 7) == 7) { /* SIMD and FP data processing */
        *use = fields;
        if ((w & 0xDF000000) == 0x1F000000) { /* FMADD, FMSUB, FNMADD, FNMSUB */
            *use = 1u << rn | 1u << rm | 1u << ra, *def = 1u << rd;
        } else if ((w & 0x5F200000) == 0x1E200000) { /* scalar FP */
            unsigned t = w >> 10 & 63;
            if ((t & 3) == 2 || (t & 3) == 3) *use = 1u << rn | 1u << rm, *def = 1u << rd; /* 2-source, FCSEL */
            else if ((t & 3) == 1 || (t & 15) == 8)
                *use = 1u << rn | 1u << rm; /* FCCMP, FCMP */
            else if ((t & 7) == 4)
                *use = 0, *def = 1u << rd; /* FMOV (immediate) */
            else if ((t & 31) == 16)
                *use = 1u << rn, *def = 1u << rd; /* 1-source */
        } else if ((w & 0x9F200400) == 0x0E200400) { /* AdvSIMD 3-same */
            unsigned op = w >> 11 & 31, uu = w >> 29 & 1, size = w >> 22 & 3;
            int whole = op == 3 ? !(uu && size) : op < 24 ? (0xFB7FF7u >> op & 1) : !(op == 25 || (op == 29 && !uu));
            if (whole) *use = 1u << rn | 1u << rm, *def = 1u << rd;
        } else if ((w & 0xBF208C00) == 0x0E000000) { /* TBL, TBX */
            *use = 1u << rm | vreg_run(rn, (w >> 13 & 3) + 1);
            if (w >> 12 & 1) *use |= 1u << rd; /* TBX keeps what's out of range */
            *def = 1u << rd;
        }
        return;
    }
    if ((w >> 25 & 5) == 4 && (w >> 26 & 1)) { /* SIMD&FP loads and stores */
        unsigned l = w >> 22 & 1;
        if ((w >> 27 & 7) == 5) { /* pairs */
            SnxU32 m = 1u << rd | 1u << ra;
            *use = l ? 0 : m, *def = l ? m : 0;
            return;
        }
        if ((w >> 27 & 7) == 3 && !(w >> 24 & 1)) { /* literal */
            *use = 0, *def = 1u << rd;
            return;
        }
        if ((w >> 27 & 7) == 7) {
            unsigned idx = w >> 10 & 3;
            if ((w >> 24 & 1) || (!(w >> 21 & 1) && idx != 2) || ((w >> 21 & 1) && idx == 2)) {
                *use = l ? 0 : 1u << rd, *def = l ? 1u << rd : 0; /* bit 22: LDR's opc has bit 0 set */
                return;
            }
            *use = ~0u;
            return;
        }
        /* structures: a store reads its list (4 at the most), a load may keep lanes */
        *use = vreg_run(rd, 4);
        return;
    }
    *use = 0; /* the general-purpose groups */
}

/* each instruction's live-in set (u->live), by the successors (the next where it falls through, an inner branch's
   target; LIVE_ALL past the function): backward passes until nothing changes. NULL where it can't be had */
static void liveness(Unit* u, const SnxFunction* f) {
    SnxU64 *use, *def;
    SnxU32 *vuse, *vdef;
    int* succ;
    size_t i, n = f->count;
    int changed, passes = 0;
    u->live = NULL;
    u->vlive = NULL;
    u->live_buf = (SnxU64*)grow(u->live_buf, &u->cap_live, n, sizeof *u->live_buf);
    u->vlive_buf = (SnxU32*)grow(u->vlive_buf, &u->cap_vlive, n, sizeof *u->vlive_buf);
    use = (SnxU64*)malloc(2 * n * sizeof *use);
    succ = (int*)malloc(2 * n * sizeof *succ);
    vuse = (SnxU32*)malloc(2 * n * sizeof *vuse);
    if (!use || !succ || !vuse || !u->live_buf || !u->vlive_buf) {
        free(use), free(succ), free(vuse);
        return;
    }
    u->live = u->live_buf;
    u->vlive = u->vlive_buf;
    def = use + n;
    vdef = vuse + n;
    for (i = 0; i < n; ++i) {
        SnxU64 a = snx_function_address(f, i);
        SnxU32 w = 0;
        SnxA64Insn insn;
        int falls;
        (void)snx_codegen_word((SnxCodegen*)u->codegen, a, &w);
        snx_a64_decode_word(w, u->codegen->features, &insn);
        regs_of(w, &use[i], &def[i]);
        vregs_of(w, &vuse[i], &vdef[i]);
        if (insn.flow == SNX_A64_FLOW_UNDEF || insn.flow == SNX_A64_FLOW_BL || insn.flow == SNX_A64_FLOW_BLR ||
            insn.flow == SNX_A64_FLOW_SVC || insn.flow == SNX_A64_FLOW_BRK || insn.flow == SNX_A64_FLOW_RET ||
            insn.flow == SNX_A64_FLOW_BR || insn.op == SNX_A64_OP_UNDEF || use[i] == LIVE_ALL)
            use[i] = LIVE_ALL, def[i] = 0, vuse[i] = ~0u, vdef[i] = 0; /* what isn't classified reads V too */
        falls = insn.flow != SNX_A64_FLOW_B && insn.flow != SNX_A64_FLOW_BR && insn.flow != SNX_A64_FLOW_RET &&
                insn.flow != SNX_A64_FLOW_UNDEF && insn.flow != SNX_A64_FLOW_BRK;
        succ[2 * i] = falls ? (i + 1 < n && snx_function_address(f, i + 1) == a + 4 ? (int)i + 1 : -2) : -1;
        succ[2 * i + 1] = -1;
        if (insn.flow == SNX_A64_FLOW_B || insn.flow == SNX_A64_FLOW_BC) {
            SnxU64 t = snx_a64_branch_target(&insn, a);
            succ[2 * i + 1] = snx_function_contains(f, t) ? (int)index_of(f, t) : -2; /* -2: out, all live */
        }
        u->live[i] = 0;
        u->vlive[i] = 0;
    }
    do {
        changed = 0;
        for (i = n; i-- > 0;) {
            SnxU64 out = 0, in;
            SnxU32 vout = 0, vin;
            int k;
            for (k = 0; k < 2; ++k) {
                int sx = succ[2 * i + k];
                if (sx == -2) out = LIVE_ALL, vout = ~0u;
                else if (sx >= 0)
                    out |= u->live[sx], vout |= u->vlive[sx];
            }
            in = use[i] | (out & ~def[i]);
            vin = vuse[i] | (vout & ~vdef[i]);
            if (in != u->live[i] || vin != u->vlive[i]) u->live[i] = in, u->vlive[i] = vin, changed = 1;
        }
    } while (changed && ++passes < 64);
    free(use), free(succ), free(vuse);
    if (changed) u->live = NULL, u->vlive = NULL; /* no fixed point: none known */
}
/* what is dead where control goes to instruction index (or past the function: nothing), for the store pass; SP and
   what no register is never */
static SnxU64 dead_at(Unit* u, SnxU64 target) {
    if (!u->live || !snx_function_contains(u->f, target)) return 0;
    return ~u->live[index_of(u->f, target)] & LIVE_ALL & ~((SnxU64)1 << 31);
}
static SnxU32 vdead_at(Unit* u, SnxU64 target) {
    if (!u->vlive || !snx_function_contains(u->f, target)) return 0;
    return ~u->vlive[index_of(u->f, target)];
}

/* the guest registers an instruction may write, X in the result (31 the SP), V in *v: more than regs_of's def,
   which is for liveness and leaves out some (a SIMD&FP transfer's general-purpose register, the SP as a
   destination, a structure's post-index). A call's or an SVC's callee writes the state, which the pins are
   reloaded from after it: the call itself writes the link register alone */
static SnxU64 may_write(const SnxA64Insn* insn, SnxU32 w, SnxU32* v) {
    SnxU64 use, def;
    unsigned rd = w & 31, rn = w >> 5 & 31;
    *v = 0;
    if ((w >> 25 & 7) == 7) *v = 1u << rd;
    else if ((w >> 25 & 5) == 4 && (w >> 26 & 1) && ((w >> 22 & 1) || ((w >> 27 & 7) == 3 && !(w >> 24 & 1))))
        *v = vreg_run(rd, 4) | 1u << (w >> 10 & 31); /* a load: Rt, a pair's Rt2, a structure's list */
    regs_of(w, &use, &def);
    if (use == LIVE_ALL) {
        if (insn->flow == SNX_A64_FLOW_BL || insn->flow == SNX_A64_FLOW_BLR) return live_r(30);
        if (insn->flow == SNX_A64_FLOW_RET || insn->flow == SNX_A64_FLOW_BR || insn->flow == SNX_A64_FLOW_B ||
            insn->flow == SNX_A64_FLOW_SVC || insn->flow == SNX_A64_FLOW_BRK || insn->flow == SNX_A64_FLOW_UNDEF)
            return 0;
        *v = ~0u;
        return LIVE_ALL;
    }
    if ((w >> 25 & 5) == 4) { /* loads and stores: def has Rt (and Rt2); a writeback's base, the SP too */
        unsigned idx = w >> 10 & 3;
        if (((w >> 27 & 7) == 5 && ((w >> 23 & 3) == 1 || (w >> 23 & 3) == 3)) ||
            ((w >> 27 & 7) == 7 && !(w >> 24 & 1) && !(w >> 21 & 1) && (idx == 1 || idx == 3)) ||
            ((w >> 26 & 1) && !(w >> 31) && (w >> 23 & 0x3E) == 0x18 && (w >> 23 & 1)))
            def |= live_r(rn);
        return def;
    }
    return def | live_r(rd); /* the SP as a destination; a SIMD&FP transfer's or conversion's register */
}

/* each instruction's dsync, from its own writes (u->dsync on entry) and what reaches it by succ: forward passes
   until nothing changes. A call or an SVC (sk 1) leaves nothing to store, the pins synced around it; a BR to the
   dispatch block (sk 2) reaches every leader. All stored where there is no fixed point */
static void pin_dirt(Unit* u, const SnxFunction* f, const int* succ, const unsigned char* sk) {
    size_t n = f->count, i;
    SnxU64 *in = (SnxU64*)calloc(n, sizeof *in), br = 0;
    SnxU32 *vin = (SnxU32*)calloc(n, sizeof *vin), vbr = 0;
    int changed = 1, passes = 0;
    if (in && vin)
        do {
            SnxU64 nbr = 0;
            SnxU32 nvbr = 0;
            changed = 0;
            for (i = 0; i < n; ++i) {
                SnxU64 out;
                SnxU32 vout;
                int k;
                if (f->leaders[i] && ((in[i] | br) != in[i] || (vin[i] | vbr) != vin[i]))
                    in[i] |= br, vin[i] |= vbr, changed = 1;
                out = sk[i] == 1 ? 0 : in[i] | u->dsync[i];
                vout = sk[i] == 1 ? 0 : vin[i] | u->vdsync[i];
                if (sk[i] == 2) nbr |= out, nvbr |= vout;
                for (k = 0; k < 2; ++k) {
                    int s = succ[2 * i + k];
                    if (s >= 0 && ((in[s] | out) != in[s] || (vin[s] | vout) != vin[s]))
                        in[s] |= out, vin[s] |= vout, changed = 1;
                }
            }
            if (nbr != br || nvbr != vbr) br |= nbr, vbr |= nvbr, changed = 1;
        } while (changed && ++passes < 64);
    for (i = 0; i < n; ++i)
        if (changed) u->dsync[i] = ~(SnxU64)0, u->vdsync[i] = ~0u;
        else
            u->dsync[i] |= in[i], u->vdsync[i] |= vin[i];
    free(in), free(vin);
}

/* the guest registers the function keeps in host registers (the machine's mg_pin_hosts: callee-saved ones first, no
   call changes them) for its whole run, written to the state where something else may read it (a call, SVC, BRK, UNDEF, a poll, the
   exits) and read back after: the most used, a register field of an instruction counting 1, 8 inside a loop
   (a backward branch's span). None in a small function, or one with a call of the runtime inside a block that may
   read the state (a system register's helper, WFE, SEV) */
static void choose_pins(Unit* u, const SnxFunction* f) {
    SnxU64 weight[33], vweight[32]; /* weight[32]: the guest memory's base */
    SnxU64 vuses[32];               /* each V register's instructions, weighted as vweight: its ties' breaker */
    unsigned k, best, nhosts;
    int fixed = 0; /* an exclusive, an atomic or CRC32: the last pins' registers may be their machine instructions' */
    unsigned char *loop, *sk;
    int* succ;
    size_t i, backs = 0;
    memset(u->pin, -1, sizeof u->pin);
    memset(u->vpin, -1, sizeof u->vpin);
    u->pinned = 0;
    u->mempin = -1;
    u->pin_wr = 0;
    u->vpin_wr = 0;
    u->pin_sel = ~(SnxU64)0;
    u->vpin_sel = ~0u;
    u->exits = NULL;
    u->a.reserved = 0;
    u->a.xreserved = MG_XRESERVED;
    memset(vweight, 0, sizeof vweight);
    memset(vuses, 0, sizeof vuses);
    loop = (unsigned char*)calloc(2 * (f->count + 1), 1);
    sk = loop + f->count + 1; /* each instruction's kind for pin_dirt */
    succ = (int*)malloc(2 * f->count * sizeof *succ);
    u->dsync = (SnxU64*)grow(u->dsync, &u->cap_dsync, f->count, sizeof *u->dsync);
    u->vdsync = (SnxU32*)grow(u->vdsync, &u->cap_vdsync, f->count, sizeof *u->vdsync);
    if (!loop || !succ || !u->dsync || !u->vdsync) {
        free(loop), free(succ);
        return;
    }
    memset(weight, 0, sizeof weight);
    /* each instruction's successors in the function (-1 none): the next where it falls through, a branch's target;
       the back edges (a branch to itself or before) noted */
    for (i = 0; i < f->count; ++i) {
        SnxU64 a = snx_function_address(f, i);
        SnxU32 w = 0;
        SnxA64Insn insn;
        int falls;
        (void)snx_codegen_word((SnxCodegen*)u->codegen, a, &w);
        snx_a64_decode_word(w, u->codegen->features, &insn);
        if (insn.op == SNX_A64_OP_SYS || insn.op == SNX_A64_OP_WFE || insn.op == SNX_A64_OP_SEV ||
            insn.op == SNX_A64_OP_SEVL) {
            free(loop), free(succ);
            return;
        }
        {
            SnxU32 vw;
            SnxU64 xw = may_write(&insn, w, &vw);
            /* a bound PLT stub's, in the place of a call or of a tail call */
            if (insn.flow == SNX_A64_FLOW_BL ||
                ((insn.flow == SNX_A64_FLOW_B || insn.flow == SNX_A64_FLOW_BC) &&
                 !snx_function_contains(f, snx_a64_branch_target(&insn, a))))
                xw |= live_r(16) | live_r(17);
            u->pin_wr |= xw;
            u->vpin_wr |= vw;
            u->dsync[i] = xw;
            u->vdsync[i] = vw;
            sk[i] = insn.flow == SNX_A64_FLOW_BL || insn.flow == SNX_A64_FLOW_BLR || insn.flow == SNX_A64_FLOW_SVC
                        ? 1
                        : insn.flow == SNX_A64_FLOW_BR ? 2 : 0;
        }
        falls = insn.flow != SNX_A64_FLOW_B && insn.flow != SNX_A64_FLOW_BR && insn.flow != SNX_A64_FLOW_RET &&
                insn.flow != SNX_A64_FLOW_UNDEF && insn.flow != SNX_A64_FLOW_BRK;
        succ[2 * i] = falls && i + 1 < f->count && snx_function_address(f, i + 1) == a + 4 ? (int)i + 1 : -1;
        succ[2 * i + 1] = -1;
        if (insn.flow == SNX_A64_FLOW_B || insn.flow == SNX_A64_FLOW_BC) {
            SnxU64 t = snx_a64_branch_target(&insn, a);
            if (snx_function_contains(f, t)) {
                succ[2 * i + 1] = (int)index_of(f, t);
                if (t <= a) ++backs;
            }
        }
    }
    if (f->count < 8 && !backs) {
        /* a small function's pins cost more than they save, but where it loops (a strlen) */
        free(loop), free(succ);
        return;
    }
    if (backs) {
        /* the natural loops: for each header (a back edge's target), what reaches one of its back edges without
           passing it, by the predecessors from those back edges. Each instruction's depth is the loops it is in */
        int *first = (int*)malloc((f->count + 1) * sizeof *first), *pred = (int*)malloc(2 * f->count * sizeof *pred);
        int* stack = (int*)malloc(2 * f->count * sizeof *stack), *latch_of = stack + f->count, nlatch = 0;
        unsigned* seen = (unsigned*)calloc(f->count, sizeof *seen);
        size_t e;
        u->exits = (unsigned char*)grow(u->exits_buf, &u->cap_exits, f->count, 1);
        u->exits_buf = u->exits;
        if (!first || !pred || !stack || !seen || !u->exits) {
            free(first), free(pred), free(stack), free(seen), free(loop), free(succ);
            u->exits = NULL;
            return;
        }
        memset(u->exits, 0, f->count);
        memset(first, 0, (f->count + 1) * sizeof *first);
        for (e = 0; e < 2 * f->count; ++e)
            if (succ[e] >= 0) ++first[succ[e] + 1];
        for (i = 0; i < f->count; ++i) first[i + 1] += first[i];
        {
            int* fill = stack; /* the next free entry of each instruction's predecessors, borrowed */
            for (i = 0; i < f->count; ++i) fill[i] = first[i];
            for (e = 0; e < 2 * f->count; ++e)
                if (succ[e] >= 0) pred[fill[succ[e]]++] = (int)(e / 2);
        }
        for (i = 0; i < f->count; ++i) {
            /* i a header: the back edges to it are its predecessors at or after it */
            int top = 0, j, mark = (int)i + 1;
            for (j = first[i]; j < first[i + 1]; ++j)
                if ((size_t)pred[j] >= i && seen[pred[j]] != (unsigned)mark) {
                    seen[pred[j]] = (unsigned)mark;
                    stack[top++] = pred[j];
                }
            if (!top) continue;
            {
                /* the latches, noted before the walk takes the stack */
                int t;
                for (t = 0; t < top; ++t) latch_of[t] = stack[t];
                nlatch = top;
            }
            seen[i] = (unsigned)mark;
            if (loop[i] < 5) ++loop[i];
            while (top) {
                int m = stack[--top], q;
                if (m != (int)i && loop[m] < 5) ++loop[m];
                if (m == (int)i) continue;
                /* nothing before the header: a header that doesn't dominate its latch (a loop entered in its middle
                   too) would otherwise take in all that reaches that entry */
                for (q = first[m]; q < first[m + 1]; ++q)
                    if ((size_t)pred[q] > i && seen[pred[q]] != (unsigned)mark) {
                        seen[pred[q]] = (unsigned)mark;
                        stack[top++] = pred[q];
                    }
            }
            /* a latch whose next instruction isn't the loop's: past it the loop has ended */
            for (j = 0; j < nlatch; ++j) {
                int l = latch_of[j];
                if (succ[2 * l] >= 0 && seen[succ[2 * l]] != (unsigned)mark) u->exits[l] = 1;
            }
        }
        free(first), free(pred), free(stack), free(seen);
    }
    /* what a pin saves is the load where a block (from one label to the next) reads a register before it writes it,
       and the store where it writes it: each counted once a block, 8 times a loop's level */
    {
        SnxU32 rd = 0, wr = 0, vrd = 0, vwr = 0;
        int memseen = 0;
        for (i = 0; i < f->count; ++i) {
            SnxU32 w = 0, vuse = 0, vdef = 0, m;
            SnxU64 wt, use, def;
            unsigned r;
            if (f->leaders[i]) rd = wr = vrd = vwr = 0, memseen = 0;
            (void)snx_codegen_word((SnxCodegen*)u->codegen, snx_function_address(f, i), &w);
            wt = 1ull << (3 * loop[i]);
            if ((w >> 25 & 5) == 4 && !memseen) weight[32] += wt, memseen = 1; /* the block's first access */
            if ((w >> 24 & 0x3F) == 0x08 || ((w >> 24 & 0x3F) == 0x38 && (w >> 21 & 1) && !(w >> 10 & 3)) ||
                ((w & 0x7FE0E000) == 0x1AC04000 && MG_CRC_FIXED(u, w)))
                fixed = 1;
            /* the V registers: SIMD and FP data processing reads Rn, Rm (Ra of the 3-source group) and Rd (an
               accumulation's), and writes Rd; a SIMD&FP load writes Rt (and a pair's Rt2), a store reads them */
            if ((w >> 25 & 7) == 7) {
                vuse = 1u << (w >> 5 & 31) | 1u << (w >> 16 & 31) | 1u << (w & 31);
                if ((w >> 24 & 0x1F) == 0x1F) vuse |= 1u << (w >> 10 & 31);
                vdef = 1u << (w & 31);
            } else if ((w >> 25 & 5) == 4 && (w >> 26 & 1)) {
                m = 1u << (w & 31);
                if ((w >> 27 & 7) == 5) m |= 1u << (w >> 10 & 31);
                if (w >> 22 & 1) vdef = m;
                else
                    vuse = m;
            }
            for (m = vuse & ~vwr & ~vrd; m; m &= m - 1) vweight[snx_ctz32(m)] += wt;
            vrd |= vuse & ~vwr;
            for (m = vdef & ~vwr; m; m &= m - 1) vweight[snx_ctz32(m)] += wt;
            vwr |= vdef;
            for (m = vuse | vdef; m; m &= m - 1) vuses[snx_ctz32(m)] += wt;
            regs_of(w, &use, &def);
            if (use == LIVE_ALL) continue; /* a call, a return, what isn't classified: a flush anyway */
            for (r = 0; r < 32; ++r) {
                SnxU32 bit = 1u << r;
                if ((use >> r & 1) && !(wr & bit) && !(rd & bit)) weight[r] += wt, rd |= bit;
                if ((def >> r & 1) && !(wr & bit)) weight[r] += wt, wr |= bit;
            }
        }
    }
    nhosts = fixed ? MG_NPINS_FIXED : MG_NPINS;
    for (k = 0; k < nhosts; ++k) {
        unsigned r;
        SnxU64 top = 0;
        best = 33;
        for (r = 0; r < 33; ++r)
            if ((r == 32 ? u->mempin < 0 : u->pin[r] < 0) && weight[r] > top) top = weight[r], best = r;
        if (best == 33 || top < 8u) break;
        if (best == 32) u->mempin = mg_pin_hosts[k];
        else
            u->pin[best] = mg_pin_hosts[k];
        if (MG_PIN_RESERVED(mg_pin_hosts[k])) u->a.reserved |= 1u << mg_pin_hosts[k];
        u->pinned = 1;
    }
    /* the V registers most used, in the machine's vector pins (x86-64's XMM15 down to XMM8); of two that save as many
       loads and stores, the one more instructions read or write: a loop's accumulator, whose chain would otherwise go
       through memory at each turn */
    for (k = 0; k < MG_NVPINS; ++k) {
        unsigned r;
        SnxU64 top = 0, uses = 0;
        best = 32;
        for (r = 0; r < 32; ++r)
            if (u->vpin[r] < 0 && (vweight[r] > top || (vweight[r] == top && vuses[r] > uses)))
                top = vweight[r], uses = vuses[r], best = r;
        if (best == 32 || top < 8u) break;
        u->vpin[best] = (signed char)MG_VPIN(k);
        u->a.xreserved |= 1u << MG_VPIN(k);
        u->pinned = 1;
    }
    if (u->pinned) pin_dirt(u, f, succ, sk);
    free(succ), free(loop); /* sk's too */
}

/* the function's code, or 0 (the unit as it was) where an instruction isn't this back end's */
static int function(Unit* u, const SnxFunction* f, size_t index) {
    size_t i, at0 = u->b.n, fix0 = u->nfixups, rel0 = u->nrelocs, sym0 = u->nsyms;
    int lab0 = u->nlabels, ok = 1, single_entry = 0;
    const SnxU64 seen0 = u->g->x.seen, packed0 = u->g->x.seen_packed;
    SnxU64 pin_rest = 0;
    SnxU32 vpin_rest = 0;
#ifdef MG_DIRECT_ENTRY
    int other = -1;
    SnxU64 pin_sel;
    SnxU32 vpin_sel;
#endif
    jmp_buf jb;
    jmp_buf* saved = g_fail;
    char sym[40];
    u->f = f;
    u->index = index;
    u->at = f->count;
    u->nframe = 0;
    u->a.cold.n = 0;
    u->a.ncjumps = u->a.nccalls = 0;
    u->a.npool = u->a.nrips = 0;
    u->a.slots_max = 0;
    u->host_fp = 0;
    u->nstubs = 0;
    u->njtabs = 0;
    u->njslots = 0;
    g_fail = &jb;
    g_why = NULL;
    if (setjmp(jb)) {
        ok = 0;
        goto out;
    }
    /* labels: each leader's, by index */
    for (i = 0; i < f->count; ++i) new_label(u);
    u->dispatch = f->leader_count > 1;
    u->d_label = u->dispatch ? new_label(u) : -1;
    u->mismatch_label = new_label(u);
    u->start_label = new_label(u);
    fname(sym, sizeof sym, "F_", f->start);
    u->syms = (Sym*)grow(u->syms, &u->cap_syms, u->nsyms + 1, sizeof *u->syms);
    u->syms[u->nsyms].name = strdup(sym);
    u->syms[u->nsyms].at = u->b.n;
    u->syms[u->nsyms].entry = 0;
    u->nsyms++;
    bind(u, u->start_label);
    flush(u); /* no block state of another function's */
    choose_pins(u, f);
    liveness(u, f);
    u->g->pinned = u->pinned;
    memcpy(u->g->pin, u->pin, sizeof u->pin);
    memcpy(u->g->vpin, u->vpin, sizeof u->vpin);
    u->g->mempin = u->mempin;
    u->a.pin_cs = pins_cs;
    u->a.fixed_fault = fixed_fault;
    u->a.pinned = u->a.xpinned = 0;
    for (i = 0; i < 32; ++i) {
        if (u->pin[i] >= 0) u->a.pinned |= 1u << u->pin[i];
        if (u->vpin[i] >= 0) u->a.xpinned |= 1u << u->vpin[i];
    }
    if (u->mempin >= 0) u->a.pinned |= 1u << u->mempin;
    u->g->x.src = -1;
    u->a.pin_flush = pins_flush;
    /* the prologue loads the pins live where the function starts; the mismatch's path (another entry) the others */
    if (u->pinned && u->live && f->count && snx_function_address(f, 0) == f->start) {
        u->pin_sel = u->live[0];
        u->vpin_sel = u->vlive ? u->vlive[0] : ~0u;
    }
    u->cur_dirty = ~(SnxU64)0, u->vcur_dirty = ~0u;
#ifdef MG_DIRECT_ENTRY
    /* if (e != start) goto other, the frame past it: a call of the start comes in there */
    if (mg_direct(u)) {
        other = new_label(u);
        mg_check(u, f->start, other);
        u->syms[sym0].entry = u->b.n - u->syms[sym0].at;
    }
    pin_sel = u->pin_sel, vpin_sel = u->vpin_sel;
#endif
    prologue(u);
    pin_rest = ~u->pin_sel, vpin_rest = ~u->vpin_sel;
    u->pin_sel = ~(SnxU64)0, u->vpin_sel = ~0u; /* every other load of the pins: all of them */
    /* a function of one leader, its start, is entered there alone (the table's entries and the direct calls are the
       leaders'): no compare of the entry with it and no mismatch block. Its stale check stays, where the machine has
       it apart from the compare (MG_ENTRY_STALE: mg_entry_stale); on the others such a function keeps both */
    single_entry = f->leader_count == 1 && f->count && snx_function_address(f, 0) == f->start;
#ifdef MG_DIRECT_ENTRY
    if (other >= 0) single_entry = 0; /* its compare came before the frame: the other target's way stays */
#endif
#ifdef MG_ENTRY_STALE
    if (single_entry && u->options->stale) mg_entry_stale(u, index);
    if (single_entry) mg_entry_fp(u); /* what mg_entry makes after the stale check, for the others */
#else
    if (u->options->stale) single_entry = 0;
#endif
    /* if (snx_stale[index]) { c->pc = e; return; } if (e != start) goto mismatch */
    if (!single_entry) {
#ifdef MG_DIRECT_ENTRY
        if (other >= 0)
            mg_stale(u, index, 0, f->start);
        else
#endif
            mg_entry(u, index, f->start);
    }
    if (f->count && snx_function_address(f, 0) != f->start) jmp(u, (int)index_of(f, f->start));
    {
        volatile size_t vi = 0;
        if (setjmp(jb)) {
            ok = 0;
            goto out;
        }
        for (; vi < f->count; ++vi) {
            SnxU64 address = snx_function_address(f, vi);
            SnxU32 word = 0;
            if (f->leaders[vi]) {
                flush_to(u, address);
                bind(u, lab0 + (int)vi);
            }
            (void)snx_codegen_word((SnxCodegen*)u->codegen, address, &word);
            u->at = vi;
            u->g->x.src = (int)vi;
            if (u->pinned) u->cur_dirty = u->dsync[vi], u->vcur_dirty = u->vdsync[vi];
            if (!instruction(u, vi, address, word)) {
                ok = 0;
                goto out;
            }
        }
        u->g->x.src = -1; /* what follows is no word's */
        /* every word made: a failure from here on is of the function's finish, a jump's reach (MG_FAR_JUMPS) */
        u->at = f->count;
        /* the last word left the function (a return, a branch, the tail call off its end): only a function of no
           word returns here */
        if (!f->count) {
            flush(u);
            leave(u);
        }
    }
#ifdef MG_DIRECT_ENTRY
    if (other >= 0) {
        /* the entry of another target: the start's frame and pins, its stale check, then the mismatch's way */
        bind(u, other);
        u->cur_dirty = ~(SnxU64)0, u->vcur_dirty = ~0u;
        u->pin_sel = pin_sel, u->vpin_sel = vpin_sel;
        prologue(u);
        u->pin_sel = ~(SnxU64)0, u->vpin_sel = ~0u;
        mg_stale(u, index, 1, f->start);
    }
#endif
    if (!single_entry) {
        /* mismatch: { c->pc = e; goto D; } or { CALLDYN(e); return; }; D: the poll, then switch (c->pc) over the
           leaders, else CALLDYN(c->pc) */
        bind(u, u->mismatch_label);
        u->cur_dirty = ~(SnxU64)0, u->vcur_dirty = ~0u; /* the dispatch block's: a BR's way in too */
        if (pin_rest || vpin_rest) {
            u->pin_sel = pin_rest, u->vpin_sel = vpin_rest;
            pins(u, 0);
            u->pin_sel = ~(SnxU64)0, u->vpin_sel = ~0u;
        }
        mg_dispatch(u, lab0);
    }
    for (i = 0; i < (size_t)u->nstubs; ++i) {
        struct Stub s = u->stubs[i];
        bind(u, s.label);
        u->cur_dirty = s.dirty, u->vcur_dirty = s.vdirty;
        if (s.kind == 1) {
            poll_then(u, s.target);
            jmp(u, (int)index_of(f, s.target));
#ifdef MG_POLL_INLINE
        } else if (s.kind == 3) {
            poll_slow(u, s.target);
#endif
        } else if (!call_or_stub(u, s.target, 1)) {
            ok = 0;
            goto out;
        }
    }
#ifdef MG_JTAB
    jtab_emit(u);
#endif
    /* the cold paths (the FP helpers' calls), then the constant pool */
    sgx_cold_emit(&u->a);
    sgx_pool_emit(&u->a);
    /* the frame's size, in the prologue and every epilogue; the labels' jumps */
    if (!mg_finish(u, fix0)) {
        ok = 0;
        goto out;
    }
    u->syms[sym0].size = u->b.n - u->syms[sym0].at;
out:
    g_fail = saved;
    if (!ok) {
        /* as it was: another back end makes this one */
        u->b.n = at0;
        u->nfixups = fix0;
        for (i = rel0; i < u->nrelocs; ++i) free(u->relocs[i].sym);
        u->nrelocs = rel0;
        for (i = sym0; i < u->nsyms; ++i) free(u->syms[i].name);
        u->nsyms = sym0;
        u->g->x.n = 0;
        u->g->x.nv = 1;
        u->g->x.nb = 1;
        u->g->x.unmade = 0;
        u->g->x.seen = seen0;
        u->g->x.seen_packed = packed0;
        u->g->qc_owed = 0;
    }
    u->nfixups = ok ? u->nfixups : fix0;
    u->nlabels = lab0; /* a function's labels are its own */
    return ok;
}

#ifdef MG_COFF
/* ---- the COFF object, as x64_elf.h's builder writes it: the code and its function table, the functions global but a
   JIT unit's, whose table is its one global ---- */

static void write_coff(Unit* u, unsigned char** object, size_t* size) {
    int jit = u->options->jit, rc = -1;
    SnxXElf* o = snx_xelf_new();
    size_t i;
    if (o) {
        snx_xelf_set_machine(o, MG_EM);
        snx_xelf_set_coff(o, 1);
        snx_xelf_put(o, SNX_ELF_TEXT, u->b.p, u->b.n, 16);
        for (i = 0; i < u->nsyms; ++i) {
            const Sym* s = &u->syms[i];
            snx_xelf_symbol(o, s->name, SNX_ELF_TEXT, s->at, s->size,
                            SNX_ELF_FUNC | (jit ? SNX_ELF_LOCAL : SNX_ELF_GLOBAL | SNX_ELF_HIDDEN));
            snx_xelf_unwind(o, s->at, s->at + s->size, u->b.p + s->at, s->size);
        }
        for (i = 0; i < u->nrelocs; ++i)
            snx_xelf_reloc(o, SNX_ELF_TEXT, u->relocs[i].at, u->relocs[i].sym, u->relocs[i].type, u->relocs[i].addend);
        if (jit) {
            size_t at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, 8 * u->nsyms, 8);
            snx_xelf_symbol(o, u->options->table_name, SNX_ELF_RELRO, at, 8 * u->nsyms, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN);
            for (i = 0; i < u->nsyms; ++i) snx_xelf_reloc(o, SNX_ELF_RELRO, at + 8 * i, u->syms[i].name, MG_R_ABS64, 0);
        }
        rc = snx_xelf_write(o, object, size);
        snx_xelf_free(o);
    }
    if (rc) {
        fprintf(stderr, "native back end: %s\n",
                rc == -1 ? "out of memory" : "a COFF object of a relocation or a prologue it has no form of");
        abort();
    }
}
#endif

/* ---- the ELF object ---- */

#if !defined(_WIN32) /* whose objects are COFF: MG_COFF's */
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
    size_t i, j, nundef = 0, first_global, def0;
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
            fprintf(stderr, "native back end: out of memory\n");
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
            if (hsym[h] && mg_resolve_local(u, &u->relocs[i], u->syms[hsym[h] - 1].at + u->syms[hsym[h] - 1].entry)) {
                free(u->relocs[i].sym);
                u->relocs[i].sym = NULL;
                continue;
            }
            if (hsym[h]) {
                /* the unit's own function where the machine's relocation stays one (an absolute address): its
                   symbol's, defined here */
                reloc_undef[i] = SIZE_MAX - (hsym[h] - 1);
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
    def0 = jit ? 2 : first_global; /* the unit's functions' symbols, in syms' order */
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
        /* the table: each function's address, an absolute 64-bit relocation of its symbol */
        for (i = 0; i < u->nsyms; ++i) {
            Elf64_Rela r;
            SnxU64 zero = 0;
            put(&data, &zero, 8);
            r.r_offset = 8 * i;
            r.r_info = MG_R_INFO(2 + i, MG_R_ABS64);
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
            if (!strcmp(undef[i], "snx_stale")) sym.st_other = STV_HIDDEN; /* the module's own */
            put(&symtab, &sym, sizeof sym);
        }
        for (i = 0; i < u->nrelocs; ++i) {
            Elf64_Rela r;
            if (!u->relocs[i].sym) continue;
            r.r_offset = u->relocs[i].at;
            r.r_info = reloc_undef[i] > SIZE_MAX / 2
                           ? MG_R_INFO(def0 + (SIZE_MAX - reloc_undef[i]), u->relocs[i].type)
                           : MG_R_INFO(undef0 + reloc_undef[i], u->relocs[i].type);
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
    eh.e_ident[EI_DATA] = MG_ELF_BIG ? ELFDATA2MSB : ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_REL;
    eh.e_machine = MG_EM;
    eh.e_flags = MG_ELF_FLAGS;
    eh.e_version = EV_CURRENT;
    eh.e_shoff = o.n;
    eh.e_ehsize = sizeof eh;
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = 9;
    eh.e_shstrndx = 7;
    put(&o, sh, sizeof sh);
    memcpy(o.p, &eh, sizeof eh);
    if (eh.e_ident[EI_DATA] != snx_elf_host_data()) snx_elf_byteswap(o.p, o.n, 1);
    free(strtab.p), free(symtab.p), free(rela.p), free(data.p), free(rela_data.p), free(shstr.p), free(undef);
    free(reloc_undef);
    *object = o.p;
    *size = o.n;
    return 0;
}
#endif

static int mach_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                       unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size,
                       unsigned host) {
    Unit u;
    size_t i, count = 0, need = 4096;
    *object = NULL;
    *size = 0;
    memset(made, 0, last - first);
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
    u.b.disp = 0;
    u.b.features = host;
#ifdef MG_R_FAR
    u.b.absolute = options->jit;
#endif
    u.codegen = codegen;
    u.options = options;
#ifdef MG_DIRECT_ENTRY
    u.first = first;
    u.sym_of = (size_t*)calloc(last - first + 1, sizeof *u.sym_of);
    if (!u.sym_of) {
        free(u.g);
        free(u.b.p);
        snprintf(error, error_size, "out of memory");
        return -1;
    }
#endif
    u.a.b = &u.b;
    u.a.machine = &u;
    u.a.reloc = unit_reloc;
    u.g->features = snx_a64_features(codegen->features);
    u.g->origin = -1;
    u.g->x.features = host;
    u.g->x.flags_ins = -1;
    u.g->x.base = SGX_BASE;
    u.g->x.nv = 1;
    u.g->x.nb = 1;
    u.g->x.count = options->ssa != NULL;
    for (i = first; i < last; ++i) {
        size_t before = u.b.n, relocs = u.nrelocs, syms = u.nsyms;
        const SnxU64 seen = u.g->x.seen, packed = u.g->x.seen_packed;
        if (u.b.cap - u.b.n < codegen->functions[i].count * 400 + 4096) {
            u.b.cap = u.b.cap * 2 + codegen->functions[i].count * 400 + 4096;
            u.b.p = (unsigned char*)realloc(u.b.p, u.b.cap);
        }
        u.again = u.fp_entry = 0;
        made[i - first] = (unsigned char)function(&u, &codegen->functions[i], i);
        if (!made[i - first] && (u.again || u.fp_entry))
            made[i - first] = (unsigned char)function(&u, &codegen->functions[i], i);
        if (u.b.n > u.b.cap) {
            /* past the buffer: grown, and made again, its symbol and relocations too */
            for (; u.nrelocs > relocs; --u.nrelocs) free(u.relocs[u.nrelocs - 1].sym);
            for (; u.nsyms > syms; --u.nsyms) free(u.syms[u.nsyms - 1].name);
            u.b.n = before;
            u.g->x.seen = seen;
            u.g->x.seen_packed = packed;
            u.b.cap *= 4;
            u.b.p = (unsigned char*)realloc(u.b.p, u.b.cap);
            --i;
            continue;
        }
#ifdef MG_FAR_JUMPS
        if (!made[i - first] && !u.far && u.at >= codegen->functions[i].count) {
            /* a jump's reach: made again with the long ones */
            u.far = 1;
            --i;
            continue;
        }
        u.far = 0;
#endif
        if (!made[i - first]) {
            if (!u.nnone) no_code(&u, g_why);
            if (u.at < codegen->functions[i].count) {
                SnxU64 address = snx_function_address(&codegen->functions[i], u.at);
                if (!u.nnone || u.none[u.nnone - 1] != address) {
                    /* made again, that instruction undefined */
                    u.none = (SnxU64*)grow(u.none, &u.cap_none, u.nnone + 1, sizeof *u.none);
                    u.none[u.nnone++] = address;
                    --i;
                    continue;
                }
                /* it was already: what fails there is no instruction's */
                snx_error("native back end: the function at 0x%llx is not made",
                          (unsigned long long)codegen->functions[i].start);
            }
        }
        u.nnone = 0;
        count += made[i - first];
#ifdef MG_DIRECT_ENTRY
        if (made[i - first]) u.sym_of[i - first] = u.nsyms;
#endif
        if (options->jit && !made[i - first]) break;
    }
    if (options->jit && count != last - first) count = 0;
    if (count) {
        int coff = 0;
#ifdef MG_COFF
        coff = MG_COFF(&u);
        if (coff) write_coff(&u, object, size);
#endif
#if !defined(_WIN32)
        if (!coff) write_object(&u, object, size);
#else
        (void)coff;
#endif
    }
    for (i = 0; i < u.nrelocs; ++i) free(u.relocs[i].sym);
    for (i = 0; i < u.nsyms; ++i) free(u.syms[i].name);
    free(u.relocs), free(u.syms), free(u.fixups), free(u.label_at), free(u.frame_at), free(u.stubs), free(u.b.p);
    free(u.none);
    free(u.live_buf), free(u.exits_buf), free(u.vlive_buf), free(u.dsync), free(u.vdsync);
    free(u.jtabs), free(u.jslots);
#ifdef MG_DIRECT_ENTRY
    free(u.sym_of);
#endif
    free(u.a.cold.p), free(u.a.pool), free(u.a.rips), free(u.a.cjumps), free(u.a.ccalls);
    if (options->ssa) {
        options->ssa->instructions += u.g->x.seen;
        options->ssa->bytes += u.g->x.seen * sizeof(SgXIns);
        options->ssa->packed += u.g->x.seen_packed;
    }
    free(u.g);
    if (options->jit && !count) memset(made, 0, last - first);
    return 0;
}

