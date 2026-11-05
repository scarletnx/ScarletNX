/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A small SPIR-V evaluator for the tests of the shader translators: it runs one invocation of a module's entry
   point on the CPU, with the built-ins, the uniform buffers and the storage buffers that the test gives, and keeps
   what the invocation stores to its outputs and buffers. It takes the instructions that the translators emit for
   their small test programs, and refuses the others by name. Subgroup operations see one invocation alone, which
   the test places at a lane of its choice. Floats are the host's, with round to nearest. */
#ifndef SNX_TESTS_SPV_EVAL_H
#define SNX_TESTS_SPV_EVAL_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t builtin[64][4]; /* the value of each BuiltIn input, by its number (41 SubgroupLocalInvocationId) */
    uint32_t* ubo[64];       /* uniform and storage buffers by binding (any set): their words */
    uint32_t ubo_words[64];
    uint32_t in[32][4];  /* the inputs that are no built-ins, by location */
    uint32_t out[32][4]; /* the outputs by location */
    uint32_t out_written[32];
    uint32_t frag_depth;
    uint32_t shared[4096]; /* the first Workgroup variable's words when the invocation ends */
    uint32_t shared_words;
    int killed;         /* the invocation ended with OpKill or OpTerminateInvocation */
    uint32_t steps_max; /* 0: 1000000 */
} SpvEvalEnv;
/* Runs the module's first entry point: NULL when it returned, else why it stopped (a static string) */
const char* spv_eval(const uint32_t* words, size_t count, SpvEvalEnv* env);
#endif
