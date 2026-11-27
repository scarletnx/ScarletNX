/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generated A64 decoder (a64_decode.inc) behind the functions of a64_decoder.h */
#include "common/cpu/aarch64/a64_decoder.h"
#define SNX_A64_DECODE_ARGS /* the arguments' names and each op's list, for snx_a64_word_fields */
#include "a64_decode.inc"

SnxU64 snx_a64_features(SnxU64 features) {
    return (features ? features : SNX_A64_TARGET_SWITCH) & ~(SnxU64)SNX_A64_FEAT_EL1;
}

SnxA64Op snx_a64_decode_word(SnxU32 word, SnxU64 features, SnxA64Insn* insn) {
    return snx_a64_decode(word, snx_a64_features(features), insn);
}

SnxA64Flow snx_a64_flow(SnxU32 word, SnxU64 features) {
    SnxA64Insn insn;
    snx_a64_decode(word, snx_a64_features(features), &insn);
    return insn.flow;
}

SnxU64 snx_a64_branch_target(const SnxA64Insn* insn, SnxU64 pc) {
    /* CB<cc> (immediate) names its offset imm9, as a64.decode does: its imm is the value compared */
    int offset = insn->op == SNX_A64_OP_CB_cond_imm ? insn->args.imm9 : insn->args.imm;
    return pc + (SnxU64)(int64_t)offset;
}

const char* snx_a64_op_text(SnxA64Op op) { return snx_a64_op_name(op); }

const char* snx_a64_feature_text(SnxU64 features) { return snx_a64_feature_name(features); }

const char* snx_a64_target_text(SnxU64 features) { return snx_a64_target_name(snx_a64_features(features)); }

/* the system registers of EL0 no state here holds, so an MRS or MSR of them is undefined */
static const char* sysreg_text(SnxU32 key) {
#define SYSREG(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))
    switch (key) {
    case SYSREG(3u, 3u, 2u, 4u, 0u): return "RNDR";
    case SYSREG(3u, 3u, 2u, 4u, 1u): return "RNDRRS";
    case SYSREG(3u, 3u, 2u, 5u, 1u): return "GCSPR_EL0";
    case SYSREG(3u, 3u, 4u, 2u, 2u): return "SVCR";
    case SYSREG(3u, 3u, 4u, 2u, 5u): return "DIT";
    case SYSREG(3u, 3u, 4u, 2u, 6u): return "SSBS";
    case SYSREG(3u, 3u, 4u, 2u, 7u): return "TCO";
    case SYSREG(3u, 3u, 4u, 4u, 2u): return "FPMR";
    case SYSREG(3u, 3u, 13u, 0u, 5u): return "TPIDR2_EL0";
    case SYSREG(3u, 3u, 13u, 0u, 7u): return "SCXTNUM_EL0";
    default: return NULL;
    }
#undef SYSREG
}

void snx_a64_word_text(SnxU32 word, char* out, size_t size) {
    SnxA64Insn insn;
    SnxU64 past;
    if (snx_a64_decode_word(word, SNX_A64_TARGET_MAX, &insn) == SNX_A64_OP_UNDEF) {
        snprintf(out, size, "no instruction");
        return;
    }
    if (insn.op == SNX_A64_OP_SYS) { /* the register, or the operation, it names */
        static const char* const forms[4] = {"SYS", "MSR", "SYSL", "MRS"};
        const char* reg = sysreg_text(word >> 5 & 0xFFFF);
        char s[32];
        snprintf(s, sizeof s, "S%u_%u_C%u_C%u_%u", word >> 19 & 3, word >> 16 & 7, word >> 12 & 15, word >> 8 & 15,
                 word >> 5 & 7);
        snprintf(out, size, reg ? "%s of %s (%s)" : "%s of %s%s", forms[(word >> 20 & 1) | (word >> 20 & 2)],
                 reg ? reg : s, reg ? s : "");
        return;
    }
    past = insn.feature & ~(SnxU64)SNX_A64_FEAT_V8_0;
    snprintf(out, size, past ? "%s, which needs %s" : "%s", snx_a64_op_text(insn.op), snx_a64_feature_text(past));
}

int snx_a64_word_fields(SnxU32 word, char* out, size_t size) {
    SnxA64Insn insn;
    if (size) out[0] = 0;
    if (snx_a64_decode(word, SNX_A64_TARGET_MAX | SNX_A64_FEAT_EL1, &insn) == SNX_A64_OP_UNDEF) return 0;
    const SnxA64OpArgs* args = &snx_a64_op_args[insn.op];
    int n = snprintf(out, size, "%s", snx_a64_op_text(insn.op));
    for (unsigned i = 0; i < args->count && n >= 0 && (size_t)n < size; i++) {
        unsigned arg = snx_a64_op_arg_list[args->first + i];
        int more =
            snprintf(out + n, size - (size_t)n, " %s=%d", snx_a64_arg_names[arg], snx_a64_arg_value(&insn.args, arg));
        n = more < 0 ? more : n + more;
    }
    return 1;
}

SnxU64 snx_a64_decoder_hash(void) { return SNX_A64_DECODE_HASH; }
