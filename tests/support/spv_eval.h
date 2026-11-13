/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A small SPIR-V evaluator for the tests of the shader translators: it runs one invocation of a module's entry
   point on the CPU, with the built-ins, the uniform buffers, the storage buffers and the texels of the sampled images
   that the test gives, and keeps what the invocation stores to its outputs and buffers. It takes the instructions
   that the translators emit for their small test programs, and refuses the others by name. Alone (spv_eval), its
   subgroup operations see one invocation, which the test places at a lane of its choice, and a barrier does nothing;
   spv_eval_subgroup runs the lanes of a subgroup together. Calls take no arguments. Floats are the host's, with round
   to nearest. */
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
    uint32_t gather[64][4][4];  /* what an OpImageGather of each component of the image at each binding returns */
    int killed;                 /* the invocation ended with OpKill or OpTerminateInvocation */
    int helper;                 /* the invocation is a fragment's helper: OpIsHelperInvocationEXT answers true */
    int half_flush;             /* PackHalf2x16 flushes f16 subnormals to a signed zero, as a driver may */
    uint32_t shuffle_id;        /* the invocation the last OpGroupNonUniformShuffle read from (its Id operand) */
    uint32_t steps_max;         /* 0: 1000000 */
    uint32_t push[20];          /* the push constant block's words */
    /* the specialization constants by SpecId: spec[k] where bit k of spec_given is set, else the module's default */
    uint32_t spec[8], spec_given;
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
/* Runs it on lanes invocations together (1 to 64), envs[l] being lane l of one subgroup that is the whole workgroup:
   at each subgroup operation and barrier every lane waits for the others, the operation takes their values, and the
   Workgroup variables are one memory for all of them (each env's shared[] keeps it). Every lane must reach each of
   those, as a subgroup in uniform control flow does. NULL when they all returned, else why the first that stopped
   did, or why they couldn't go on together */
const char* spv_eval_subgroup(const uint32_t* words, size_t count, SpvEvalEnv* envs, uint32_t lanes);
#endif
