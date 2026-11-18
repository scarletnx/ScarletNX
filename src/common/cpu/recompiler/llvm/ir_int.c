/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The system instructions (SYS, MRS, MSR) as LLVM IR, keyed on the generated decoder's ops and arguments; the integer
   data-processing ones, the hints, the barriers and the branches' operands are generated from their Sail (ir_sail.c).
   The system registers
   and operations EL0 reaches follow the Arm ARM's EL0 access rules under Horizon's EL1 controls (SCTLR_EL1.UCT, DZE
   and UCI set, UMA clear; CNTKCTL_EL1 lets EL0 read the counters and their frequency): every other one raises UNDEF,
   as on the console. */
#include "common/cpu/recompiler/llvm/ir_int.h"
#include "common/cpu/recompiler/llvm/ir_sail.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b

/* the values a Switch's Cortex-A57 and its kernel give EL0 (the A57 TRM; helpers/integer.c has the same) */
#define SWITCH_CTR_EL0   0x8444C004u /* 64-byte cache lines */
#define SWITCH_DCZID_EL0 4u          /* DC ZVA zeroes 64 bytes */
#define SWITCH_CNTFRQ    19200000u

/* ---- operands ---- */

/* a W result is zero-extended into its X register; writes to the zero register are dropped */
static void set_reg(SnxIr* ir, unsigned n, V v) { ir_setx(ir, n, ir_u64(ir, v)); }

/* the guest's N, Z, C and V as a nibble with N in bit 3, as ir_flags_set_nzcv takes them */
static V nzcv_read(SnxIr* ir) {
    V v = ir_i32(ir, 0);
    for (unsigned i = 0; i < 4; ++i)
        v = LLVMBuildOr(B, v, LLVMBuildShl(B, ir_flag_read(ir, i), ir_i32(ir, 3 - i), ""), "");
    return v;
}

/* ---- hints, barriers and the system instructions ---- */

static void undef(SnxIr* ir, SnxU64 pc, SnxU32 word) {
    ir_set_pc(ir, ir_i64(ir, pc));
    ir_undef(ir, pc, word);
}

/* op0:op1:CRn:CRm:op2 as helpers/integer.c's snx_mrs and snx_msr take them (op0 3) */
#define SYSREG(op1, crn, crm, op2) ((1u << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

static V mrs_helper(SnxIr* ir, SnxU32 key) {
    LLVMTypeRef params[2];
    V args[2];
    params[0] = ir->t_ptr;
    params[1] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_i32(ir, key);
    return ir_call(ir, ir_extern(ir, "snx_mrs", ir->t_i64, params, 2), args, 2);
}
static void msr_helper(SnxIr* ir, SnxU32 key, V value) {
    LLVMTypeRef params[3];
    V args[3];
    params[0] = ir->t_ptr;
    params[1] = ir->t_i32;
    params[2] = ir->t_i64;
    args[0] = ir->c;
    args[1] = ir_i32(ir, key);
    args[2] = value;
    ir_call(ir, ir_extern(ir, "snx_msr", ir->t_void, params, 3), args, 3);
}

/* MRS of the registers EL0 reads on the Switch: NULL for the rest, which are UNDEF there (the EL1 registers, the
   ID registers MIDR_EL1 included, which Horizon doesn't emulate as Linux does, DAIF without SCTLR_EL1.UMA, the
   timers' control registers) */
static V read_sysreg(SnxIr* ir, SnxU32 key) {
    switch (key) {
        case SYSREG(3, 4, 2, 0): /* NZCV */
            return ir_u64(ir, LLVMBuildShl(B, nzcv_read(ir), ir_i32(ir, 28), ""));
        case SYSREG(3, 4, 4, 0): return ir_u64(ir, ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpcr)));
        case SYSREG(3, 4, 4, 1): return ir_u64(ir, ir_cpu_load(ir, ir->t_i32, offsetof(CPU, fpsr)));
        case SYSREG(3, 13, 0, 2): return ir_cpu_load(ir, ir->t_i64, offsetof(CPU, tpidr));   /* TPIDR_EL0 */
        case SYSREG(3, 13, 0, 3): return ir_cpu_load(ir, ir->t_i64, offsetof(CPU, tpidrro)); /* TPIDRRO_EL0 */
        case SYSREG(3, 0, 0, 1): return ir_i64(ir, SWITCH_CTR_EL0);                          /* SCTLR_EL1.UCT */
        case SYSREG(3, 0, 0, 7): return ir_i64(ir, SWITCH_DCZID_EL0);
        case SYSREG(3, 14, 0, 0): return ir_i64(ir, SWITCH_CNTFRQ);
        case SYSREG(3, 14, 0, 1): /* CNTPCT_EL0, the SDK's system tick */
        case SYSREG(3, 14, 0, 2): /* CNTVCT_EL0 */
            return mrs_helper(ir, key);
        default: return NULL;
    }
}

/* MSR of the registers EL0 writes: 0 for the rest (read-only at EL0, or EL1's) */
static int write_sysreg(SnxIr* ir, SnxU32 key, V value) {
    switch (key) {
        case SYSREG(3, 4, 2, 0): /* NZCV from bits 31:28 */
            ir_flags_set_nzcv(ir, ir_u32(ir, LLVMBuildLShr(B, value, ir_i64(ir, 28), "")));
            return 1;
        case SYSREG(3, 4, 4, 0): /* FPCR: the bits the guest's features implement (FZ16 with FEAT_FP16), the
                                    runtime's FPCR hook */
            if (!(ir->guest_features & SNX_A64_FEAT_FP16))
                value = LLVMBuildAnd(B, value, ir_i64(ir, ~(SnxU64)(1u << 19)), "");
            msr_helper(ir, key, value);
            return 1;
        case SYSREG(3, 4, 4, 1): msr_helper(ir, key, value); return 1; /* FPSR: the bits the A57 keeps */
        case SYSREG(3, 13, 0, 2): ir_cpu_store(ir, value, offsetof(CPU, tpidr)); return 1;
        default: return 0;
    }
}

/* SYS, SYSL, MRS and MSR (register) */
static void sys(SnxIr* ir, const SnxA64Args* a, SnxU32 word, SnxU64 pc) {
    unsigned rt = (unsigned)a->rt;
    if (a->op0 == 3) {
        SnxU32 key = SYSREG((unsigned)a->op1, (unsigned)a->crn, (unsigned)a->crm, (unsigned)a->op2);
        if (a->l) {
            V v = read_sysreg(ir, key);
            if (v) {
                set_reg(ir, rt, v);
                return;
            }
        } else if (write_sysreg(ir, key, ir_rx(ir, rt)))
            return;
    } else if (a->op0 == 1 && !a->l && a->op1 == 3 && a->crn == 7 && a->op2 == 1) {
        switch (a->crm) {
            case 4: /* DC ZVA (SCTLR_EL1.DZE): the 64-byte block holding Xt */
                ir_call_helper(ir, SNX_IR_DC_ZVA, (V[]){ir->M, ir_rx(ir, rt)}, 2);
                return;
            case 5: { /* IC IVAU: what was compiled from the line Xt is in is stale (helpers/integer.c's snx_ic_ivau) */
                LLVMTypeRef params[2];
                V args[2];
                params[0] = ir->t_ptr;
                params[1] = ir->t_i64;
                args[0] = ir->c;
                args[1] = ir_rx(ir, rt);
                ir_call(ir, ir_extern(ir, "snx_ic_ivau", ir->t_void, params, 2), args, 2);
                return;
            }
            case 10: /* DC CVAC */
            case 11: /* DC CVAU */
            case 14: /* DC CIVAC (SCTLR_EL1.UCI): the host keeps instruction and data coherent */
                return;
            default: break;
        }
    }
    undef(ir, pc, word);
}
#undef SYSREG


int snx_ir_int_handles(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_ADR:
        case SNX_A64_OP_ADRP:
        case SNX_A64_OP_ADD_i:
        case SNX_A64_OP_ADDS_i:
        case SNX_A64_OP_SUB_i:
        case SNX_A64_OP_SUBS_i:
        case SNX_A64_OP_AND_i:
        case SNX_A64_OP_ORR_i:
        case SNX_A64_OP_EOR_i:
        case SNX_A64_OP_ANDS_i:
        case SNX_A64_OP_MOVN:
        case SNX_A64_OP_MOVZ:
        case SNX_A64_OP_MOVK:
        case SNX_A64_OP_SBFM:
        case SNX_A64_OP_BFM:
        case SNX_A64_OP_UBFM:
        case SNX_A64_OP_EXTR:
        case SNX_A64_OP_AND_r:
        case SNX_A64_OP_ORR_r:
        case SNX_A64_OP_EOR_r:
        case SNX_A64_OP_ANDS_r:
        case SNX_A64_OP_ADD_r:
        case SNX_A64_OP_SUB_r:
        case SNX_A64_OP_ADDS_r:
        case SNX_A64_OP_SUBS_r:
        case SNX_A64_OP_ADD_ext:
        case SNX_A64_OP_SUB_ext:
        case SNX_A64_OP_ADDS_ext:
        case SNX_A64_OP_SUBS_ext:
        case SNX_A64_OP_ADC:
        case SNX_A64_OP_ADCS:
        case SNX_A64_OP_SBC:
        case SNX_A64_OP_SBCS:
        case SNX_A64_OP_CCMP:
        case SNX_A64_OP_CSEL:
        case SNX_A64_OP_RBIT:
        case SNX_A64_OP_REV16:
        case SNX_A64_OP_REV32:
        case SNX_A64_OP_REV64:
        case SNX_A64_OP_CLZ:
        case SNX_A64_OP_CLS:
        case SNX_A64_OP_UDIV:
        case SNX_A64_OP_SDIV:
        case SNX_A64_OP_LSLV:
        case SNX_A64_OP_LSRV:
        case SNX_A64_OP_ASRV:
        case SNX_A64_OP_RORV:
        case SNX_A64_OP_CRC32:
        case SNX_A64_OP_CRC32C:
        case SNX_A64_OP_MADD_w:
        case SNX_A64_OP_MSUB_w:
        case SNX_A64_OP_MADD_x:
        case SNX_A64_OP_MSUB_x:
        case SNX_A64_OP_SMADDL:
        case SNX_A64_OP_SMSUBL:
        case SNX_A64_OP_UMADDL:
        case SNX_A64_OP_UMSUBL:
        case SNX_A64_OP_SMULH:
        case SNX_A64_OP_UMULH:
        case SNX_A64_OP_B_cond:
        case SNX_A64_OP_CBZ:
        case SNX_A64_OP_TBZ:
        case SNX_A64_OP_BR:
        case SNX_A64_OP_BLR:
        case SNX_A64_OP_RET:
        /* The supported PAuth execution model has SCTLR's EnIA/EnIB/EnDA/EnDB clear. Signing and
           authentication leave pointers unchanged; authenticated branches still transfer control. */
        case SNX_A64_OP_XPACLRI:
        case SNX_A64_OP_XPACI:
        case SNX_A64_OP_XPACD:
        case SNX_A64_OP_RETA:
        case SNX_A64_OP_BRA:
        case SNX_A64_OP_BRAZ:
        case SNX_A64_OP_BLRA:
        case SNX_A64_OP_BLRAZ:
        case SNX_A64_OP_PACIA1716:
        case SNX_A64_OP_PACIB1716:
        case SNX_A64_OP_AUTIA1716:
        case SNX_A64_OP_AUTIB1716:
        case SNX_A64_OP_PACIAZ:
        case SNX_A64_OP_PACIASP:
        case SNX_A64_OP_PACIBZ:
        case SNX_A64_OP_PACIBSP:
        case SNX_A64_OP_AUTIAZ:
        case SNX_A64_OP_AUTIASP:
        case SNX_A64_OP_AUTIBZ:
        case SNX_A64_OP_AUTIBSP:
        case SNX_A64_OP_PACIA:
        case SNX_A64_OP_PACIB:
        case SNX_A64_OP_PACDA:
        case SNX_A64_OP_PACDB:
        case SNX_A64_OP_AUTIA:
        case SNX_A64_OP_AUTIB:
        case SNX_A64_OP_AUTDA:
        case SNX_A64_OP_AUTDB:
        case SNX_A64_OP_PACGA:
        case SNX_A64_OP_SVC:
        case SNX_A64_OP_BRK:
        case SNX_A64_OP_NOP:
        case SNX_A64_OP_YIELD:
        case SNX_A64_OP_WFE:
        case SNX_A64_OP_WFI:
        case SNX_A64_OP_SEV:
        case SNX_A64_OP_SEVL:
        case SNX_A64_OP_CLREX:
        case SNX_A64_OP_DSB_DMB:
        case SNX_A64_OP_ISB:
        case SNX_A64_OP_SYS:
        /* the ounce target's: FlagM, and ESB, a NOP at EL0 with no SError to synchronize */
        case SNX_A64_OP_CFINV:
        case SNX_A64_OP_RMIF:
        case SNX_A64_OP_SETF8:
        case SNX_A64_OP_SETF16:
        case SNX_A64_OP_ESB: return 1;
        default: return 0;
    }
}

/* the plain instructions whose emitters sailgen generates from the Sail (a64.roots) */
static int generated(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_ADR:
        case SNX_A64_OP_ADRP:
        case SNX_A64_OP_ADD_i:
        case SNX_A64_OP_ADDS_i:
        case SNX_A64_OP_SUB_i:
        case SNX_A64_OP_SUBS_i:
        case SNX_A64_OP_AND_i:
        case SNX_A64_OP_ORR_i:
        case SNX_A64_OP_EOR_i:
        case SNX_A64_OP_ANDS_i:
        case SNX_A64_OP_MOVN:
        case SNX_A64_OP_MOVZ:
        case SNX_A64_OP_MOVK:
        case SNX_A64_OP_SBFM:
        case SNX_A64_OP_BFM:
        case SNX_A64_OP_UBFM:
        case SNX_A64_OP_EXTR:
        case SNX_A64_OP_AND_r:
        case SNX_A64_OP_ORR_r:
        case SNX_A64_OP_EOR_r:
        case SNX_A64_OP_ANDS_r:
        case SNX_A64_OP_ADD_r:
        case SNX_A64_OP_SUB_r:
        case SNX_A64_OP_ADDS_r:
        case SNX_A64_OP_SUBS_r:
        case SNX_A64_OP_ADD_ext:
        case SNX_A64_OP_SUB_ext:
        case SNX_A64_OP_ADDS_ext:
        case SNX_A64_OP_SUBS_ext:
        case SNX_A64_OP_ADC:
        case SNX_A64_OP_ADCS:
        case SNX_A64_OP_SBC:
        case SNX_A64_OP_SBCS:
        case SNX_A64_OP_CCMP:
        case SNX_A64_OP_CSEL:
        case SNX_A64_OP_RBIT:
        case SNX_A64_OP_REV16:
        case SNX_A64_OP_REV32:
        case SNX_A64_OP_REV64:
        case SNX_A64_OP_CLZ:
        case SNX_A64_OP_CLS:
        case SNX_A64_OP_UDIV:
        case SNX_A64_OP_SDIV:
        case SNX_A64_OP_LSLV:
        case SNX_A64_OP_LSRV:
        case SNX_A64_OP_ASRV:
        case SNX_A64_OP_RORV:
        case SNX_A64_OP_CRC32:
        case SNX_A64_OP_CRC32C:
        case SNX_A64_OP_MADD_w:
        case SNX_A64_OP_MSUB_w:
        case SNX_A64_OP_MADD_x:
        case SNX_A64_OP_MSUB_x:
        case SNX_A64_OP_SMADDL:
        case SNX_A64_OP_SMSUBL:
        case SNX_A64_OP_UMADDL:
        case SNX_A64_OP_UMSUBL:
        case SNX_A64_OP_SMULH:
        case SNX_A64_OP_UMULH:
        case SNX_A64_OP_XPACI:
        case SNX_A64_OP_XPACD:
        case SNX_A64_OP_PACGA:
        case SNX_A64_OP_PACIA:
        case SNX_A64_OP_PACIB:
        case SNX_A64_OP_PACDA:
        case SNX_A64_OP_PACDB:
        case SNX_A64_OP_AUTIA:
        case SNX_A64_OP_AUTIB:
        case SNX_A64_OP_AUTDA:
        case SNX_A64_OP_AUTDB:
        case SNX_A64_OP_CFINV:
        case SNX_A64_OP_RMIF:
        case SNX_A64_OP_SETF8:
        case SNX_A64_OP_SETF16:
        /* NOP: the hint space's unallocated part, and the decoder's other NOP rows, the prefetches (PRFM, PRFUM),
           whose Sail does nothing (nx.sail's Prefetch) */
        case SNX_A64_OP_NOP:
        /* the hint space (the PAuth hints with the keys disabled), the barriers, CLREX */
        case SNX_A64_OP_YIELD:
        case SNX_A64_OP_WFE:
        case SNX_A64_OP_WFI:
        case SNX_A64_OP_SEV:
        case SNX_A64_OP_SEVL:
        case SNX_A64_OP_ESB:
        case SNX_A64_OP_XPACLRI:
        case SNX_A64_OP_PACIA1716:
        case SNX_A64_OP_PACIB1716:
        case SNX_A64_OP_AUTIA1716:
        case SNX_A64_OP_AUTIB1716:
        case SNX_A64_OP_PACIAZ:
        case SNX_A64_OP_PACIASP:
        case SNX_A64_OP_PACIBZ:
        case SNX_A64_OP_PACIBSP:
        case SNX_A64_OP_AUTIAZ:
        case SNX_A64_OP_AUTIASP:
        case SNX_A64_OP_AUTIBZ:
        case SNX_A64_OP_AUTIBSP:
        case SNX_A64_OP_CLREX:
        case SNX_A64_OP_DSB_DMB:
        case SNX_A64_OP_ISB: return 1;
        default: return 0;
    }
}

void snx_ir_int(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    const SnxA64Args* a = &insn->args;
    SnxA64Op op = insn->op;
    if (generated(op)) {
        if (ir->int_reference)
            ir->int_reference(ir, insn, word, pc);
        else if (!snx_ir_sail(ir, word, pc) && !ir->fallbacks++) {
            /* the decoder's op and the Sail's decode clauses disagree on the word: the module is refused, naming it */
            ir->fallback_word = word;
            ir->fallback_pc = pc;
        }
        return;
    }
    switch (op) {
        case SNX_A64_OP_SYS: sys(ir, a, word, pc); break;
        /* the prefetches; the flow ops have no body */
        default: break;
    }
}

/* the flow ops whose operand comes from their Sail (ir_sail.c): 1 for a conditional one (the operand is whether it is
   taken), 2 for one to a register (where to), 0 for another op */
static int generated_branch(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_B_cond:
        case SNX_A64_OP_CBZ:
        case SNX_A64_OP_TBZ: return 1;
        case SNX_A64_OP_BR:
        case SNX_A64_OP_BLR:
        case SNX_A64_OP_RET:
        case SNX_A64_OP_BRA:
        case SNX_A64_OP_BRAZ:
        case SNX_A64_OP_BLRA:
        case SNX_A64_OP_BLRAZ:
        case SNX_A64_OP_RETA: return 2;
        default: return 0;
    }
}

LLVMValueRef snx_ir_int_operand(SnxIr* ir, const SnxA64Insn* insn, SnxU32 word, SnxU64 pc) {
    int kind = generated_branch(insn->op);
    if (kind) {
        LLVMValueRef taken = NULL, target = NULL;
        if (ir->operand_reference) return ir->operand_reference(ir, insn, word, pc);
        if (!snx_ir_sail_branch(ir, word, pc, &taken, &target)) {
            /* the decoder's op and the Sail's decode clauses disagree on the word: the module is refused, naming it */
            if (!ir->fallbacks++) {
                ir->fallback_word = word;
                ir->fallback_pc = pc;
            }
            return kind == 1 ? LLVMConstInt(ir->t_i1, 0, 0) : ir_i64(ir, pc);
        }
        return kind == 1 ? taken : target;
    }
    switch (insn->op) {
        case SNX_A64_OP_SVC:
        case SNX_A64_OP_BRK: return ir_i32(ir, (SnxU32)insn->args.imm & 0xFFFFu);
        default: return NULL;
    }
}
