/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The pipeline cache kept between runs (common/gpu/vk/vk_common.c, used by the 3DS renderer and the Switch's), on the
   3DS's device: no file gives an empty cache, a pipeline made with it is written and read back whole, a cut file and
   one of another driver build are kept from the driver, which the reason says, and still give a cache, a write that
   fails says why, no write leaves a temporary file, and the 3DS renderer makes the folder's shader_cache for its file
   (named for the driver: 3ds_vk_draw --pipeline-cache), and nothing with vk_pipeline_cache=0. argv[1] is a folder the
   test may fill */
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/gpu/vk/shaders/fill_spv.h"
#include "common/gpu/vk/vk_common.h"
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

static long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    fclose(f);
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FOLDER\n", argv[0]);
        return 2;
    }
    const char* dir = argv[1];
    if (!snx_is_dir(dir)) (void)plat_mkdir(dir);
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[512];
    if (!m) return 1;
    if (!snx_ctr_vk_init(m, why, sizeof why)) {
        printf("no Vulkan device (%s): pipeline cache checks skipped\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    VkDevice dev = snx_ctr_vk()->device;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(snx_ctr_vk()->phys, &props);
    char* path = snx_path_join(dir, "cache.bin");
    remove(path);

    /* no file: an empty cache, and nothing to say */
    size_t loaded = 1;
    VkPipelineCache cache = snx_vk_pipeline_cache_load(dev, &props, path, &loaded, why, sizeof why);
    CHECK(cache != VK_NULL_HANDLE);
    CHECK(loaded == 0);
    CHECK(!why[0]);

    /* a pipeline made with it, written, and read back whole */
    SnxCtrVkCompute fill = {0};
    CHECK(snx_ctr_vk_compute(snx_ctr_fill_spv, snx_ctr_fill_spv_size / 4, 1, 0, 16, &fill));
    VkPipeline made = VK_NULL_HANDLE;
    CHECK(snx_vk_compute_pipeline(dev, cache, fill.layout, snx_ctr_fill_spv, snx_ctr_fill_spv_size, &made) ==
          VK_SUCCESS);
    size_t saved = 0;
    CHECK(snx_vk_pipeline_cache_save(dev, cache, path, &saved, why, sizeof why) == 0);
    CHECK(saved > 0);
    CHECK(!why[0]);
    CHECK(file_size(path) == (long)saved);
    VkPipelineCache again = snx_vk_pipeline_cache_load(dev, &props, path, &loaded, why, sizeof why);
    CHECK(again != VK_NULL_HANDLE);
    if (saved >= 32) {
        CHECK(loaded == saved);
        CHECK(!why[0]);
    } else { /* lavapipe of Mesa 26.2 gives 20 bytes of its 32-byte header, which no driver may be given back */
        printf("the driver's own data: %s\n", why);
        CHECK(loaded == 0);
        CHECK(strstr(why, "fewer than the 32") != NULL);
    }
    /* the driver takes what it wrote: the same pipeline from the loaded cache */
    VkPipeline remade = VK_NULL_HANDLE;
    CHECK(snx_vk_compute_pipeline(dev, again, fill.layout, snx_ctr_fill_spv, snx_ctr_fill_spv_size, &remade) ==
          VK_SUCCESS);

    /* a write that fails says why: a folder where the file goes, a folder that is not there */
    char *blocked = snx_path_join(dir, "blocked.bin"), *missing = snx_path_join(dir, "missing/cache.bin");
    size_t none = 1;
    (void)snx_remove_empty_dir(blocked);
    CHECK(!plat_mkdir(blocked));
    CHECK(snx_vk_pipeline_cache_save(dev, cache, blocked, &none, why, sizeof why) == -1);
    printf("onto a folder: %s\n", why);
    CHECK(none == 0);
    CHECK(strstr(why, ".tmp cannot be moved over it (") != NULL);
    CHECK(snx_vk_pipeline_cache_save(dev, cache, missing, &none, why, sizeof why) == -1);
    printf("into a folder that is not there: %s\n", why);
    CHECK(strstr(why, ".tmp cannot be written (") != NULL);

    /* no temporary file of this process's left beside it: its name ends in .<pid>.tmp, so look for any */
    char** names = NULL;
    size_t count = 0;
    CHECK(!snx_list_dir(dir, &names, &count));
    for (size_t i = 0; i < count; ++i) {
        size_t n = strlen(names[i]);
        CHECK(!(n > 4 && !strcmp(names[i] + n - 4, ".tmp")));
    }
    snx_free_strings(names, count);

    /* this device's header from another build of the driver (another pipelineCacheUUID), as a driver update leaves
       the file: kept from the driver, which Vulkan lets be given only what it gave out, and some drivers take without
       a check */
    unsigned char other[32] = {32, 0, 0, 0, 1};
    for (unsigned i = 0; i < 4; ++i) {
        other[8 + i] = (unsigned char)(props.vendorID >> 8 * i);
        other[12 + i] = (unsigned char)(props.deviceID >> 8 * i);
    }
    memcpy(other + 16, props.pipelineCacheUUID, VK_UUID_SIZE);
    other[16] ^= 0xff;
    CHECK(!snx_write_file(path, other, sizeof other));
    VkPipelineCache from_other = snx_vk_pipeline_cache_load(dev, &props, path, &loaded, why, sizeof why);
    printf("another driver build: %s\n", why);
    CHECK(from_other != VK_NULL_HANDLE);
    CHECK(loaded == 0);
    CHECK(strstr(why, "another build of the driver") && strstr(why, "starts empty"));

    /* a cut file (a run stopped by the system as it wrote, on a file system without the replace): still a cache */
    static const unsigned char cut[16] = {0x20, 0, 0, 0, 1, 0, 0, 0, 0xde, 0xad};
    CHECK(!snx_write_file(path, cut, sizeof cut));
    VkPipelineCache from_cut = snx_vk_pipeline_cache_load(dev, &props, path, &loaded, why, sizeof why);
    printf("a cut file: %s\n", why);
    CHECK(from_cut != VK_NULL_HANDLE);
    CHECK(loaded == 0);
    CHECK(strstr(why, "holds 16 bytes") && strstr(why, "starts empty"));

    /* the renderer's own: <folder>/shader_cache/vk_pipelines.d<driver>-<vendor>-<device>.bin, made at the end of a
       run in which pipelines went into it; with vk_pipeline_cache=0 neither the folder nor the file */
    char* cache_dir = snx_path_join(dir, "shader_cache");
    (void)snx_remove_tree(cache_dir);
    g_settings.vulkan_pipeline_cache = 0;
    snx_ctr_vk_pipeline_cache_open(dir);
    CHECK(!snx_path_exists(cache_dir));
    g_settings.vulkan_pipeline_cache = 1;
    snx_ctr_vk_pipeline_cache_open(dir);
    CHECK(snx_is_dir(cache_dir));

    vkDestroyPipeline(dev, made, NULL);
    vkDestroyPipeline(dev, remade, NULL);
    vkDestroyPipelineCache(dev, cache, NULL);
    vkDestroyPipelineCache(dev, again, NULL);
    vkDestroyPipelineCache(dev, from_other, NULL);
    vkDestroyPipelineCache(dev, from_cut, NULL);
    snx_ctr_vk_compute_free(&fill);
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    remove(path);
    (void)snx_remove_empty_dir(blocked);
    free(blocked);
    free(missing);
    free(cache_dir);
    free(path);
    if (g_failed) return 1;
    puts("3ds_vk_pipeline_cache: ok");
    return 0;
}
