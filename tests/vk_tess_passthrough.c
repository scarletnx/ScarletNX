/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Validate emitted control-shader interfaces and write standalone modules for spirv-val. No Vulkan device. */
#include "nx/gpu/vk/tess_passthrough.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>

VkState g_vk;
PFN_vkCreateShaderModule vkCreateShaderModule;
PFN_vkDestroyShaderModule vkDestroyShaderModule;
u64 vk_hash(const void* data, u64 bytes) {
    (void)data;
    (void)bytes;
    return 0;
}
int vk_ok_(VkResult result, const char* call, const char* file, int line) {
    (void)call;
    (void)file;
    (void)line;
    return result == VK_SUCCESS;
}
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}

int main(int argc, char** argv) {
    const struct {
        u32 vertices, mask;
    } cases[] = {{1, 0}, {3, 1}, {4, 0x80000001u}, {32, 0xffffffffu}};
    for (u32 c = 0; c < sizeof cases / sizeof *cases; c++) {
        size_t words;
        u32* code = vk_tess_passthrough_spirv(cases[c].vertices, cases[c].mask, &words);
        if (!code || words < 5 || code[0] != 0x07230203u) return 1;
        u32 model = 0, output_vertices = 0, offset48 = 0, offset64 = 0;
        for (size_t at = 5; at < words;) {
            u32 count = code[at] >> 16, op = code[at] & 0xffff;
            if (!count || count > words - at) return 1;
            if (op == 15 && count >= 4 && code[at + 1] == 1) model++;
            if (op == 16 && count == 4 && code[at + 2] == 26 && code[at + 3] == cases[c].vertices) output_vertices++;
            if (op == 72 && count == 5 && code[at + 3] == 35) {
                offset48 += code[at + 4] == 48;
                offset64 += code[at + 4] == 64;
            }
            at += count;
        }
        if (model != 1 || output_vertices != 1 || offset48 != 1 || offset64 != 1) return 1;
        if (argc == 2) {
            char path[4096];
            if (snprintf(path, sizeof path, "%s/tess-passthrough-%u.spv", argv[1], c) >= (int)sizeof path) return 1;
            FILE* file = fopen(path, "wb");
            if (!file || fwrite(code, 4, words, file) != words || fclose(file)) return 1;
        }
        free(code);
    }
    size_t words = 123;
    if (vk_tess_passthrough_spirv(0, 0, &words) || words) return 1;
    if (vk_tess_passthrough_spirv(33, 0, &words) || words) return 1;
    puts("PASS tessellation passthrough emitter: patch bounds and push/interface layout");
    return 0;
}
