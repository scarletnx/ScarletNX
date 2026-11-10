/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A small SPIR-V evaluator for the tests of the shader translators: it runs one invocation of a module's entry
   point on the CPU, with the built-ins, the uniform buffers, the storage buffers and the texels of the sampled images
   that the test gives, and keeps what the invocation stores to its outputs and buffers. It takes the instructions
   that the translators emit for their small test programs, and refuses the others by name. Subgroup operations see
   one invocation alone, which the test places at a lane of its choice, and a barrier does nothing. Calls take no
   arguments. Floats are the host's, with round to nearest. */
#ifndef SNX_TESTS_SPV_EVAL_H
#define SNX_TESTS_SPV_EVAL_H
#include <stddef.h>
#include <stdint.h>

/* the bindings a module may read buffers at: past SNX_VKB_END (gpu/shader/shader.h), SNX_VKB_SMAP among them */
#define SPV_EVAL_BINDINGS 128
typedef struct {
    uint32_t builtin[64][4]; /* the value of each BuiltIn input, by its number (41 SubgroupLocalInvocationId) */
    uint32_t* ubo[SPV_EVAL_BINDINGS]; /* uniform and storage buffers by binding (any set): their words */
    uint32_t ubo_words[SPV_EVAL_BINDINGS];
    uint32_t in[32][4];  /* the inputs that are no built-ins, by location */
    uint32_t out[32][4]; /* the outputs by location */
    uint32_t out_written[32];
    uint32_t frag_depth;
    uint32_t position[4];  /* the Position output (BuiltIn 0) when the invocation ends */
    uint32_t shared[4096]; /* the first Workgroup variable's words when the invocation ends */
    uint32_t shared_words;
    uint32_t texel[64][4];      /* what every sample or fetch of the sampled image at each binding returns */
    uint32_t image_size[64][3]; /* what a size query of the sampled image at each binding answers; its levels are 1 */
    uint32_t fetched[64][3];    /* the coordinates of the last OpImageFetch of the image at each binding */
    int killed;                 /* the invocation ended with OpKill or OpTerminateInvocation */
    uint32_t shuffle_id;        /* the invocation the last OpGroupNonUniformShuffle read from (its Id operand) */
    uint32_t steps_max;         /* 0: 1000000 */
    uint32_t push[20];          /* the push constant block's words */
    /* task and mesh programs, one invocation of a workgroup: the TaskPayloadWorkgroupEXT variable's words (given, and
       kept when the invocation ends), the counts of OpSetMeshOutputsEXT (vertices, primitives) and of
       OpEmitMeshTasksEXT (x, y, z), each with its flag, and where the words of each Output array go: by location,
       then 32 for Position and 33 for PrimitiveTriangleIndicesEXT (the test's memory, 0 words for none) */
    uint32_t payload[1024];
    uint32_t mesh_outputs[2], mesh_tasks[3];
    int set_outputs, emitted_tasks;
    uint32_t* arrays[34];
    uint32_t array_words[34];
} SpvEvalEnv;
/* Runs the module's first entry point: NULL when it returned, else why it stopped (a static string) */
const char* spv_eval(const uint32_t* words, size_t count, SpvEvalEnv* env);
#endif
