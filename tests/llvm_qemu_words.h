/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* llvm_qemu_differential's words from the generated decoder (tools/decodetree): a translation unit of its own, which
   includes the decoder's table (a64_decode.inc) for its rows and fields */
#ifndef SNX_LLVM_QEMU_WORDS_H
#define SNX_LLVM_QEMU_WORDS_H

#include <stddef.h>
#include <stdint.h>

typedef uint64_t (*LqdRandom)(void);

/* words of every row of the decoder's table, decoded under the max target: samples of each (the row's free bits
   clear, set, then random, each tried until the word decodes to the row's name), then every value of each field of
   3 bits or fewer in the first sample and in the second (free bits set, when it differs). Rows of SME and MOPS are
   left out (lqd_op_name). *out is
   allocated; the count is returned */
size_t lqd_row_words(unsigned samples, LqdRandom random, uint32_t** out);
/* the name a word decodes to under the max target, or the space of words no row decodes: "(SVE space)",
   "(SME space)", "(unallocated)". *unrunnable is set for the words QEMU's runner can't host: SME's (SMSTART would
   leave it in streaming mode) and MOPS's (their sizes and directions come from registers a random state leaves
   unbounded, and one took the runner down) */
const char* lqd_op_name(uint32_t word, int* unrunnable);
/* the name the Switch target (the Cortex-A57) decodes the word to, NULL when it is UNDEF there */
const char* lqd_switch_op_name(uint32_t word);
/* the number of rows */
size_t lqd_rows(void);

#endif
