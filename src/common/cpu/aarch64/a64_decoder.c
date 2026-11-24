/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generated A64 decoder (a64_decode.inc) behind the functions of a64_decoder.h */
#include "common/cpu/aarch64/a64_decoder.h"
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

void snx_a64_word_text(SnxU32 word, char* out, size_t size) {
    SnxA64Insn insn;
    SnxU64 past;
    if (snx_a64_decode_word(word, SNX_A64_TARGET_MAX, &insn) == SNX_A64_OP_UNDEF) {
        snprintf(out, size, "no instruction");
        return;
    }
    past = insn.feature & ~(SnxU64)SNX_A64_FEAT_V8_0;
    snprintf(out, size, past ? "%s, which needs %s" : "%s", snx_a64_op_text(insn.op), snx_a64_feature_text(past));
}

SnxU64 snx_a64_decoder_hash(void) { return SNX_A64_DECODE_HASH; }
