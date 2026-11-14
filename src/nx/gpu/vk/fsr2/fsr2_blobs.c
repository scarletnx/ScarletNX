/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shader blobs AMD FidelityFX Super Resolution 2.2.1's Vulkan backend (vk/ffx_fsr2_vk.cpp, MIT, Copyright (c)
   2022-2023 Advanced Micro Devices, Inc.) asks for when it makes a pass's pipeline: fsr2GetPermutationBlobByIndexVK,
   which AMD's build generates with a Windows tool (FidelityFX_SC.exe) and this file answers instead, from the modules
   the build compiles (cmake/fsr2_shaders.cmake). A blob names the pass's sampled images, storage images and uniform
   buffers with their bindings: read here from the module itself, the variables of descriptor set 1 by their names */
#include "nx/gpu/vk/fsr2/fsr2_spv.h"
#include <stdlib.h>
#include "vk/shaders/ffx_fsr2_shaders_vk.h"

enum { KIND_NONE, KIND_SAMPLED, KIND_STORAGE, KIND_UNIFORM, KINDS };
enum { MAX_BOUND = 32 };
typedef struct {
    int read;
    uint32_t n[KINDS];
    const char* names[KINDS][MAX_BOUND];
    uint32_t bindings[KINDS][MAX_BOUND];
} Bound;
static Bound g_fsr2_bound[9][16];

/* SPIR-V's opcodes and operands read here */
enum {
    OP_NAME = 5,
    OP_TYPE_IMAGE = 25,
    OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32,
    OP_VARIABLE = 59,
    OP_DECORATE = 71,
    DECORATION_BINDING = 33,
    DECORATION_SET = 34,
    NO_VALUE = 0xffffffffu
};
typedef struct {
    const char* name;
    uint32_t binding, set, kind, pointee;
} Id;

static void reflect(const SnxFsr2Spv* spv, Bound* b) {
    const uint32_t* w = spv->words;
    uint32_t words = spv->size / 4, bound = words > 3 ? w[3] : 0;
    Id* ids = (Id*)calloc(bound ? bound : 1, sizeof *ids);
    b->read = 1;
    if (!ids) return;
    for (uint32_t i = 0; i < bound; i++) ids[i].binding = ids[i].set = NO_VALUE;
    /* types and decorations first: a variable comes after its type, and after every name and decoration */
    for (uint32_t at = 5; at < words;) {
        uint32_t op = w[at] & 0xffff, n = w[at] >> 16;
        const uint32_t* o = w + at + 1;
        if (!n || at + n > words) break;
        if (op == OP_NAME && n >= 3 && o[0] < bound) ids[o[0]].name = (const char*)(o + 1);
        if (op == OP_DECORATE && n >= 4 && o[0] < bound) {
            if (o[1] == DECORATION_BINDING) ids[o[0]].binding = o[2];
            if (o[1] == DECORATION_SET) ids[o[0]].set = o[2];
        }
        if (op == OP_TYPE_IMAGE && n >= 9 && o[0] < bound) ids[o[0]].kind = o[6] == 2 ? KIND_STORAGE : KIND_SAMPLED;
        if (op == OP_TYPE_STRUCT && n >= 2 && o[0] < bound) ids[o[0]].kind = KIND_UNIFORM;
        if (op == OP_TYPE_POINTER && n >= 4 && o[0] < bound) ids[o[0]].pointee = o[2];
        if (op == OP_VARIABLE && n >= 4 && o[0] < bound && o[1] < bound) {
            const Id* v = &ids[o[1]];
            uint32_t type = ids[o[0]].pointee, kind = type < bound ? ids[type].kind : KIND_NONE;
            if (v->set == 1 && v->binding != NO_VALUE && v->name && kind != KIND_NONE && b->n[kind] < MAX_BOUND) {
                b->names[kind][b->n[kind]] = v->name;
                b->bindings[kind][b->n[kind]++] = v->binding;
            }
        }
        at += n;
    }
    free(ids);
}

Fsr2ShaderBlobVK fsr2GetPermutationBlobByIndexVK(FfxFsr2Pass pass, uint32_t options) {
    uint32_t k = (options & FSR2_SHADER_PERMUTATION_HDR_COLOR_INPUT ? 1u : 0) |
                 (options & FSR2_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS ? 2u : 0) |
                 (options & FSR2_SHADER_PERMUTATION_JITTER_MOTION_VECTORS ? 4u : 0) |
                 (options & FSR2_SHADER_PERMUTATION_DEPTH_INVERTED ? 8u : 0);
    Fsr2ShaderBlobVK blob = {0};
    if ((uint32_t)pass >= 9) return blob;
    const SnxFsr2Spv* spv = &snx_fsr2_spv[pass][k];
    Bound* b = &g_fsr2_bound[pass][k];
    if (!b->read) reflect(spv, b);
    blob.data = (const uint8_t*)spv->words;
    blob.size = spv->size;
    blob.storageImageCount = b->n[KIND_STORAGE];
    blob.sampledImageCount = b->n[KIND_SAMPLED];
    blob.uniformBufferCount = b->n[KIND_UNIFORM];
    blob.boundStorageImageNames = b->names[KIND_STORAGE];
    blob.boundStorageImageBindings = b->bindings[KIND_STORAGE];
    blob.boundSampledImageNames = b->names[KIND_SAMPLED];
    blob.boundSampledImageBindings = b->bindings[KIND_SAMPLED];
    blob.boundUniformBufferNames = b->names[KIND_UNIFORM];
    blob.boundUniformBufferBindings = b->bindings[KIND_UNIFORM];
    return blob;
}
