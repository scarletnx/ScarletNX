/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The RISC back ends' AArch32 blocks, the glue every one shares (after mach/mach_arm.inc, included by a machine's
   block compiler: rv64/rv64_arm.c...): the frame (the machine's rk_fn_prologue, without pins), the entry checks on the
   state's register, the chaining's tail jumps and poll, the entry checks' exit function and the entry stub of a block
   another back end makes, from risc.h's operations and the machine's branches and calls. The machine names its
   relocations (MG_ARM_R_TAIL, MG_ARM_R_LITERAL) and the exit functions' symbols (MG_ARM_ENTRY_EXIT, MG_ARM_THUMB_ENTRY,
   at most 23 characters). The VFP's arithmetic is snx_arm_fp's, its helper */

/* the scratch registers of the code before the frame (the entry checks, the exit function, an entry stub): the
   sequences' own, or the machine's where those are the callee's or the link (s390x's) */
#ifndef RK_ENTRY_S1
#define RK_ENTRY_S1 RK_S1
#define RK_ENTRY_S2 RK_S2
#endif

/* every operation snx_arm_fp's (its helper's) */
static SgD fp_call(SgCtx* g, SnxArmVectorOp op, unsigned width, SnxU32 param, SgD xv, SgD yv, SgD zv) {
    return fp_helper(g, op, width, param, xv, yv, zv);
}

static int mg_patch_frame(MG_ARM* c) {
    sgx_patch_frame(&c->a);
    return 0;
}
/* to label where r is (nonzero) or isn't 0 */
static void arm_jreg(MG_ARM* c, int nonzero, int r, int label) { risc_a32_jreg(&c->g, &c->b, nonzero, r, label); }
/* the entry (block_begin's checks: R15 the block's address, CPSR.T and E clear, a Thumb block's T set and its ITSTATE 0;
   then exit and syndrome cleared), on the state's argument register, before the frame; where a check fails, a jump to
   bad (entry_exit). Then the entry the block's own jumps from another of this back end's take (X and the block's name),
   past them: the frame. A hosted block's entry is that one alone (codegen.h) */
static void mg_prologue(MG_ARM* c, SnxU32 pc, int* bad) {
    SgXBuf* b = &c->b;
    int s = sgx_arg_regs[0];
    *bad = sgx_a32_label(&c->g);
    if (!sgx_a32_hosted(&c->g)) {
        rk_ldst(b, 1, 4, 0, RK_ENTRY_S1, s, -1, (int)(offsetof(SnxArmCPU, r) + 60));
        rk_li(b, RK_ENTRY_S2, pc);
        rk_xor(b, RK_ENTRY_S1, RK_ENTRY_S1, RK_ENTRY_S2);
        arm_jreg(c, 1, RK_ENTRY_S1, *bad);
        rk_ldst(b, 1, 4, 0, RK_ENTRY_S1, s, -1, (int)offsetof(SnxArmCPU, cpsr));
        if (c->g.thumb) { /* of T, E and ITSTATE's bits, T alone */
            rk_li(b, RK_ENTRY_S2, SNX_ARM_T | SNX_ARM_E | SG_IT_BITS);
            rk_and(b, RK_ENTRY_S1, RK_ENTRY_S1, RK_ENTRY_S2);
            rk_xori(b, RK_ENTRY_S1, RK_ENTRY_S1, SNX_ARM_T);
        } else
            rk_andi(b, RK_ENTRY_S1, RK_ENTRY_S1, SNX_ARM_T | SNX_ARM_E);
        arm_jreg(c, 1, RK_ENTRY_S1, *bad);
        rk_li(b, RK_ENTRY_S2, 0);
        rk_ldst(b, 0, 4, 0, RK_ENTRY_S2, s, -1, (int)offsetof(SnxArmCPU, exit));
        rk_ldst(b, 0, 4, 0, RK_ENTRY_S2, s, -1, (int)offsetof(SnxArmCPU, syndrome));
    }
    c->own = b->n;
    if (c->a.nframe >= 256) sg_fail("a block of too many frames", c->a.nframe, 256);
    c->a.frame_at[c->a.nframe++] = rk_fn_prologue(b, 0);
}
/* where a check of the entry failed: the second argument the block's address, the third its first word (a Thumb
   block's halfword), and the object's own exit function of its state (which sets the exit) */
static void mg_entry_exit(MG_ARM* c, SnxU32 pc, SnxU32 first, int bad) {
    SgXBuf* b = &c->b;
    sgx_a32_bind(&c->g, b, bad);
    rk_li(b, sgx_arg_regs[1], pc);
    rk_li(b, sgx_arg_regs[2], first);
    add_rel(c, rk_tail_reloc(b), c->g.thumb ? MG_ARM_THUMB_ENTRY : MG_ARM_ENTRY_EXIT, MG_ARM_R_TAIL, 0);
}
static void epilogue(MG_ARM* c) { rk_fn_epilogue(&c->b, 0); }
static void mg_return(MG_ARM* c) {
    epilogue(c);
    rk_ret(&c->b);
}
/* where the block ends with R15 key's address in key's state: a jump to the block's own entry past its checks (X and
   its name) where key is one of the successors, after the poll for one at or before the block's start; else R15
   stored and the return. public: the block's checked entry instead, R15 stored */
static void mg_go_on(MG_ARM* c, SnxU32 key, SnxU32 block, const SnxU32* next, size_t nnext, int public) {
    SgXBuf* b = &c->b;
    SgCtx* g = &c->g;
    size_t k = 0;
    while (k < nnext && next[k] != key) ++k;
    if (k == nnext || public) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, key & ~1u);
    if (k == nnext) {
        sgx_a32_jmp(g, b, g->ret);
        return;
    }
    if ((key & ~1u) <= block) {
        /* the thread's poll (the state's word at snx_arm_poll_offset): a thread of higher priority waits, the return
           with R15 stored */
        int go = sgx_a32_label(g);
        add_rel(c, rk_literal(b, RK_S1), "snx_arm_poll_offset", MG_ARM_R_LITERAL, 0);
        rk_literal_value(b, RK_S1, RK_S1, RK_SA);
        rk_ldst(b, 1, 8, 0, RK_S1, RK_S1, -1, 0);
        rk_ldst(b, 1, 4, 0, RK_S1, SGX_BASE, RK_S1, 0);
        arm_jreg(c, 0, RK_S1, go);
        if (!public) sgx_a32_state_imm(b, offsetof(SnxArmCPU, r) + 60, key & ~1u);
        sgx_a32_jmp(g, b, g->ret);
        sgx_a32_bind(g, b, go);
    }
    rk_mv(b, sgx_arg_regs[0], SGX_BASE);
    epilogue(c);
    char sym[24];
    name_of(sym + (public ? 0 : 1), sizeof sym - 1, key);
    if (!public) sym[0] = 'X';
    add_rel(c, rk_tail_reloc(b), sym, MG_ARM_R_TAIL, 0);
}

/* the exit function (state, the block's address, its first word), each object's own, where a block's entry check
   failed (llvm.c's block_begin, in its order): R15 not the address, BAD_ENTRY with the address; else CPSR.T set,
   BAD_ENTRY, else (E) ENDIAN_UNSUPPORTED, with the word; R15 left as it is */
static void entry_exit_fn(SnxXElf* o) {
    SgXBuf b = {0};
    unsigned char code[256];
    int s = sgx_arg_regs[0], pc = sgx_arg_regs[1], word = sgx_arg_regs[2];
    size_t wrong, thumb;
    b.p = code;
    b.cap = sizeof code;
    rk_ldst(&b, 1, 4, 0, RK_ENTRY_S1, s, -1, (int)(offsetof(SnxArmCPU, r) + 60));
    rk_xor(&b, RK_ENTRY_S1, RK_ENTRY_S1, pc);
    wrong = rk_bz(&b, 1, RK_ENTRY_S1);
    rk_ldst(&b, 1, 4, 0, RK_ENTRY_S1, s, -1, (int)offsetof(SnxArmCPU, cpsr));
    rk_andi(&b, RK_ENTRY_S1, RK_ENTRY_S1, SNX_ARM_T);
    rk_li(&b, RK_ENTRY_S2, SNX_ARM_ENDIAN_UNSUPPORTED);
    thumb = rk_bz(&b, 0, RK_ENTRY_S1);
    rk_li(&b, RK_ENTRY_S2, SNX_ARM_BAD_ENTRY);
    rk_patch(&b, thumb, b.n);
    rk_ldst(&b, 0, 4, 0, RK_ENTRY_S2, s, -1, (int)offsetof(SnxArmCPU, exit));
    rk_ldst(&b, 0, 4, 0, word, s, -1, (int)offsetof(SnxArmCPU, syndrome));
    rk_ret(&b);
    rk_patch(&b, wrong, b.n);
    rk_li(&b, RK_ENTRY_S2, SNX_ARM_BAD_ENTRY);
    rk_ldst(&b, 0, 4, 0, RK_ENTRY_S2, s, -1, (int)offsetof(SnxArmCPU, exit));
    rk_ldst(&b, 0, 4, 0, pc, s, -1, (int)offsetof(SnxArmCPU, syndrome));
    rk_ret(&b);
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, code, b.n, 16);
    snx_xelf_symbol(o, MG_ARM_ENTRY_EXIT, SNX_ELF_TEXT, at, b.n, SNX_ELF_LOCAL | SNX_ELF_FUNC);
}
/* the Thumb blocks' (llvm_thumb.inc's checks, in its order): R15 not the address, BAD_ENTRY with the address; CPSR.T
   clear, BAD_ENTRY with the halfword; ITSTATE in CPSR, IT_ENTRY with it; else (E) ENDIAN_UNSUPPORTED with the halfword */
static void thumb_entry_exit_fn(SnxXElf* o) {
    SgXBuf b = {0};
    unsigned char code[320];
    int s = sgx_arg_regs[0], pc = sgx_arg_regs[1], half = sgx_arg_regs[2];
    b.p = code;
    b.cap = sizeof code;
    rk_ldst(&b, 1, 4, 0, RK_ENTRY_S1, s, -1, (int)(offsetof(SnxArmCPU, r) + 60));
    rk_xor(&b, RK_ENTRY_S1, RK_ENTRY_S1, pc);
    size_t wrong = rk_bz(&b, 1, RK_ENTRY_S1);
    rk_ldst(&b, 1, 4, 0, RK_ENTRY_S1, s, -1, (int)offsetof(SnxArmCPU, cpsr));
    rk_li(&b, RK_ENTRY_S2, SNX_ARM_BAD_ENTRY);
    rk_andi(&b, pc, RK_ENTRY_S1, SNX_ARM_T); /* the address is done with */
    size_t clear = rk_bz(&b, 0, pc);
    rk_srli(&b, pc, RK_ENTRY_S1, 25, 0); /* ITSTATE: [1:0] from bits 26:25, [7:2] from 15:10 */
    rk_andi(&b, pc, pc, 3);
    rk_srli(&b, RK_ENTRY_S1, RK_ENTRY_S1, 8, 0);
    rk_andi(&b, RK_ENTRY_S1, RK_ENTRY_S1, 0xFC);
    rk_or(&b, pc, pc, RK_ENTRY_S1);
    rk_li(&b, RK_ENTRY_S2, SNX_ARM_ENDIAN_UNSUPPORTED);
    size_t none = rk_bz(&b, 0, pc);
    rk_li(&b, RK_ENTRY_S2, SNX_ARM_IT_ENTRY);
    rk_mv(&b, half, pc);
    rk_patch(&b, clear, b.n);
    rk_patch(&b, none, b.n);
    rk_ldst(&b, 0, 4, 0, RK_ENTRY_S2, s, -1, (int)offsetof(SnxArmCPU, exit));
    rk_ldst(&b, 0, 4, 0, half, s, -1, (int)offsetof(SnxArmCPU, syndrome));
    rk_ret(&b);
    rk_patch(&b, wrong, b.n);
    rk_li(&b, RK_ENTRY_S2, SNX_ARM_BAD_ENTRY);
    rk_ldst(&b, 0, 4, 0, RK_ENTRY_S2, s, -1, (int)offsetof(SnxArmCPU, exit));
    rk_ldst(&b, 0, 4, 0, pc, s, -1, (int)offsetof(SnxArmCPU, syndrome));
    rk_ret(&b);
    if (b.n > b.cap) sg_fail("the Thumb entry's exit past its room", (long long)b.n, (long long)b.cap);
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, code, b.n, 16);
    snx_xelf_symbol(o, MG_ARM_THUMB_ENTRY, SNX_ELF_TEXT, at, b.n, SNX_ELF_LOCAL | SNX_ELF_FUNC);
}
static void mg_entry_exit_fn(SnxXElf* o) {
    entry_exit_fn(o);
    thumb_entry_exit_fn(o);
}
/* a block another back end makes, entered past the checks (X and its name): R15 stored, then the block */
int MG_ARM_API(entry)(MG_ARM* c, uint32_t key) {
    SgXBuf b = {0};
    unsigned char code[64];
    b.p = code;
    b.cap = sizeof code;
    rk_li(&b, RK_ENTRY_S1, key & ~1u);
    rk_ldst(&b, 0, 4, 0, RK_ENTRY_S1, sgx_arg_regs[0], -1, (int)(offsetof(SnxArmCPU, r) + 60));
    Rel rel = {.at = rk_tail_reloc(&b), .type = MG_ARM_R_TAIL, .addend = 0};
    name_of(rel.sym, sizeof rel.sym, key);
    char name[24];
    snprintf(name, sizeof name, "X%s", rel.sym);
    return keep(c, code, b.n, &rel, 1, key & ~1u, name, (size_t)-1) ? 0 : -1;
}
