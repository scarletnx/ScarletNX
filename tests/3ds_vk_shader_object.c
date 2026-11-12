/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The shader objects' binaries kept between runs (common/gpu/vk/shader_object.c, which the 3DS renderer draws with),
   on the 3DS's device where it has VK_EXT_shader_object: a shader compiled from SPIR-V keeps its binary, which is
   written and read back whole, and the same shader is then made from the binary with no compile. A file of another
   device, a cut file and no file give no binaries, a write leaves no temporary file, and the 3DS renderer's file is in
   the folder's shader_cache. argv[1] is a folder the test may fill */
#include "3ds/gpu/vk/shaders/fill_spv.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/gpu/vk/shader_object.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_failed;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                                    \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FOLDER\n", argv[0]);
        return 2;
    }
    const char* dir = argv[1];
    if (!snx_is_dir(dir)) (void)plat_mkdir(dir);
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[256];
    if (!m) return 1;
    if (!snx_ctr_vk_init(m, why, sizeof why)) {
        printf("no Vulkan device (%s): shader object checks skipped\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    const SnxCtrVk* v = snx_ctr_vk();
    if (!v->shader_object) {
        printf("the device draws without VK_EXT_shader_object: shader object checks skipped\n");
        snx_ctr_vk_free();
        snx_ctr_memory_free(m);
        return 77;
    }
    VkDevice dev = v->device;
    char* path = snx_path_join(dir, "shaders.bin");
    remove(path);

    /* the fill shader as a compute shader object, of its set and push constants */
    SnxCtrVkCompute fill = {0};
    CHECK(snx_ctr_vk_compute(snx_ctr_fill_spv, snx_ctr_fill_spv_size / 4, 1, 0, 16, &fill));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkShaderCreateInfoEXT info = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                  .codeSize = snx_ctr_fill_spv_size,
                                  .pCode = snx_ctr_fill_spv,
                                  .pName = "main",
                                  .setLayoutCount = 1,
                                  .pSetLayouts = &fill.set,
                                  .pushConstantRangeCount = 1,
                                  .pPushConstantRanges = &push};
    uint64_t key = snx_vk_shader_key(VK_SHADER_STAGE_COMPUTE_BIT, snx_ctr_fill_spv, snx_ctr_fill_spv_size / 4);
    CHECK(key != snx_vk_shader_key(VK_SHADER_STAGE_VERTEX_BIT, snx_ctr_fill_spv, snx_ctr_fill_spv_size / 4));

    /* no file: no binaries, and the shader compiled, its binary kept */
    SnxVkShaderBinaries b;
    snx_vk_shader_binaries_init(&b, v->shader_binary_uuid, v->shader_binary_version);
    size_t loaded = 1, size = 0;
    snx_vk_shader_binaries_load(&b, path, &loaded);
    CHECK(loaded == 0 && b.count == 0);
    VkShaderEXT first = VK_NULL_HANDLE;
    bool compiled = false;
    CHECK(snx_vk_shader_object(dev, &b, key, &info, &first, &compiled) == VK_SUCCESS);
    CHECK(first != VK_NULL_HANDLE && compiled);
    CHECK(b.count == 1 && b.dirty && snx_vk_shader_binary(&b, key, &size) && size > 0);

    /* written, and read back whole */
    size_t saved = 0;
    CHECK(snx_vk_shader_binaries_save(&b, path, &saved, why, sizeof why) == 0);
    CHECK(saved > size && !b.dirty);
    SnxVkShaderBinaries again;
    snx_vk_shader_binaries_init(&again, v->shader_binary_uuid, v->shader_binary_version);
    snx_vk_shader_binaries_load(&again, path, &loaded);
    size_t again_size = 0;
    const void* kept = snx_vk_shader_binary(&again, key, &again_size);
    CHECK(loaded == size && again.count == 1 && kept && again_size == size);
    CHECK(kept && !memcmp(kept, snx_vk_shader_binary(&b, key, &size), size));

    /* the same shader from the binary: no compile */
    VkShaderEXT second = VK_NULL_HANDLE;
    CHECK(snx_vk_shader_object(dev, &again, key, &info, &second, &compiled) == VK_SUCCESS);
    CHECK(second != VK_NULL_HANDLE && !compiled);

    /* no temporary file left beside it */
    char** names = NULL;
    size_t count = 0;
    CHECK(!snx_list_dir(dir, &names, &count));
    for (size_t i = 0; i < count; ++i) {
        size_t n = strlen(names[i]);
        CHECK(!(n > 4 && !strcmp(names[i] + n - 4, ".tmp")));
    }
    snx_free_strings(names, count);

    /* another device's file (its UUID differs), and another driver version's: not read */
    uint8_t other[VK_UUID_SIZE];
    memcpy(other, v->shader_binary_uuid, sizeof other);
    other[0] ^= 0xff;
    SnxVkShaderBinaries foreign;
    snx_vk_shader_binaries_init(&foreign, other, v->shader_binary_version);
    snx_vk_shader_binaries_load(&foreign, path, &loaded);
    CHECK(loaded == 0 && foreign.count == 0);
    snx_vk_shader_binaries_free(&foreign);
    snx_vk_shader_binaries_init(&foreign, v->shader_binary_uuid, v->shader_binary_version + 1);
    snx_vk_shader_binaries_load(&foreign, path, &loaded);
    CHECK(loaded == 0 && foreign.count == 0);
    snx_vk_shader_binaries_free(&foreign);

    /* a cut file: none of it taken */
    size_t whole_size = 0;
    unsigned char* whole = snx_read_file(path, &whole_size);
    CHECK(whole && whole_size == saved);
    /* the binary's last bytes gone */
    CHECK(whole && !snx_write_file(path, whole, whole_size - 24));
    snx_vk_shader_binaries_init(&foreign, v->shader_binary_uuid, v->shader_binary_version);
    snx_vk_shader_binaries_load(&foreign, path, &loaded);
    CHECK(loaded == 0 && foreign.count == 0);
    snx_vk_shader_binaries_free(&foreign);
    free(whole);

    /* the renderer's own: <folder>/shader_cache, where vk_shaders.<driver tag>.bin goes */
    g_settings.vulkan_pipeline_cache = 1;
    snx_ctr_vk_pipeline_cache_open(dir);
    char* cache_dir = snx_path_join(dir, "shader_cache");
    CHECK(snx_is_dir(cache_dir));

    vkDestroyShaderEXT(dev, first, NULL);
    vkDestroyShaderEXT(dev, second, NULL);
    snx_vk_shader_binaries_free(&b);
    snx_vk_shader_binaries_free(&again);
    snx_ctr_vk_compute_free(&fill);
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    remove(path);
    free(cache_dir);
    free(path);
    if (g_failed) return 1;
    puts("3ds_vk_shader_object: ok");
    return 0;
}
