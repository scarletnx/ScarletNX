/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host programs (host_program.h): shader objects as the translation makes, in the table of generated modules, so
   pipelines and the pipeline file take them as they take the renderer's own */
#include "nx/gpu/vk/host_program.h"
#include "common/utilities/bits.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

#define HOST_PROGRAMS 4096        /* modules alive at once: a GL game links a few hundred programs */
#define HOST_TAG      0x48535450u /* the key's first word, which a translation's hash never comes with */

static struct {
    VkSh* sh[HOST_PROGRAMS];
    u32 count;
} g_host_program;

u32 vk_host_program(int stage, const u32* spirv, size_t words, const ShaderInfo* si) {
    u32 mask[SH_KEY_WORDS] = {HOST_TAG};
    u64 hash;
    VkSh* s;
    if ((stage != ST_VS && stage != ST_FS) || !spirv || !words || !si) return 0;
    hash = snx_fnv1a(snx_fnv_step(SNX_FNV_BASIS, (u32)stage), spirv, words * sizeof *spirv);
    s = vk_shader_generated_find(hash, stage, mask);
    if (s)
        for (u32 i = 0; i < g_host_program.count; i++)
            if (g_host_program.sh[i] == s) return i + 1;
    if (g_host_program.count == HOST_PROGRAMS) {
        LOG_LIMIT(1, "vulkan: a host program past the %d the renderer keeps: its draws are skipped\n", HOST_PROGRAMS);
        return 0;
    }
    if (!s) {
        VkShaderModuleCreateInfo create = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof *spirv, .pCode = spirv};
        s = calloc(1, sizeof *s);
        if (!s) return 0;
        s->hash = hash;
        s->stage = stage;
        s->si = *si;
        s->si.stage = stage;
        memcpy(s->mask, mask, sizeof s->mask);
        if (!VK_OK(vkCreateShaderModule(g_vk.dev, &create, NULL, &s->mod))) {
            LOG_LIMIT(4, "vulkan: the device refuses a host program's %s module of %zu words\n",
                      stage == ST_VS ? "vertex" : "fragment", words);
            free(s);
            return 0;
        }
        vk_shader_generated_record(s, spirv, words, NULL, 0);
    }
    g_host_program.sh[g_host_program.count++] = s;
    return g_host_program.count;
}

VkSh* vk_host_shader(u64 code, int stage) {
    u32 id = (u32)code;
    VkSh* s;
    if (!(code & DS_HOST_PROGRAM) || !id || id > g_host_program.count) return NULL;
    s = g_host_program.sh[id - 1];
    return s->stage == stage ? s : NULL;
}
