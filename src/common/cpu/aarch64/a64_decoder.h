/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A64 decoder tools/decodetree generates from a64.decode and its profile (decode/), for the recompiler */
#ifndef SNX_A64_DECODER_H
#define SNX_A64_DECODER_H

#include "common/utilities/common.h"
#include "a64_decode.h" /* generated into the build folder (cmake/decodetree.cmake) */

/* The features a decode takes: 0 is the Switch's (SNX_A64_TARGET_SWITCH, the Cortex-A57). EL1 never counts: guest
   code runs at EL0, so ERET, HVC, SMC, HLT and the EL1 MSR immediates decode as UNDEF, as they raise an undefined
   instruction there. The SNX_A64_FLOW_ERET and SNX_A64_FLOW_EXC flows can't come out of a decode */
SnxU64 snx_a64_features(SnxU64 features);
/* word's op for a guest with these features (snx_a64_features), insn filled in; SNX_A64_OP_UNDEF when none matches */
SnxA64Op snx_a64_decode_word(SnxU32 word, SnxU64 features, SnxA64Insn* insn);
/* what word does to the flow, for a guest with these features */
SnxA64Flow snx_a64_flow(SnxU32 word, SnxU64 features);
/* where a B, BL or BC (flow) at pc goes */
SnxU64 snx_a64_branch_target(const SnxA64Insn* insn, SnxU64 pc);
const char* snx_a64_op_text(SnxA64Op op);
/* the name of the first of these features (SNX_A64_FEAT_*), as the profile has it */
const char* snx_a64_feature_text(SnxU64 features);
/* the name of the target with these features (a64.profile's: "switch", "max"), "?" where none has them */
const char* snx_a64_target_text(SnxU64 features);
/* what word is to a decoder that knows every feature, for the text of an undefined instruction: "SM4E, which needs
   FEAT_SM4", "ADD_v" of a base one, or "no instruction". out of size bytes */
void snx_a64_word_text(SnxU32 word, char* out, size_t size);
/* names the decoding (the .decode files and the profile) in the keys of caches of code it decoded */
SnxU64 snx_a64_decoder_hash(void);

#endif
