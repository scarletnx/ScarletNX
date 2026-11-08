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
/* names the decoding (the .decode files and the profile) in the keys of caches of code it decoded */
SnxU64 snx_a64_decoder_hash(void);

#endif
