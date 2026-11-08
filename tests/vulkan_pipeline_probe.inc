/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Opt-in diagnostic only. A package from prepare_pipeline_probe.py contains exact cached compile inputs. The
   ordinary and disabled-optimization variants use an empty application cache. Nothing is drawn or claimed
   pixel-correct by this latency experiment. Run while no game is active. */
#include "support/pipeline_probe_flags.h"
static int probe_pipeline_compile(const char* path, int disable_optimization) {
    CHECK(disable_optimization == 0 || disable_optimization == 1);
    CHECK(!SNX_HOST_BE); /* PipeKey is the local cache's host-layout structure. */
    size_t size;
    u8* data = snx_read_file(path, &size);
    CHECK(data && size >= 36 && size <= (48u << 20));
    CHECK(!memcmp(data, "SNXPPB1\n", 8));
    u32 file_key_bytes = snx_ld_le32(data + 8), pipe_key_bytes = snx_ld_le32(data + 12), words[5];
    CHECK(file_key_bytes && file_key_bytes <= 65536 && pipe_key_bytes == sizeof(PipeKey));
    u64 total = 36ull + file_key_bytes + pipe_key_bytes;
    for (u32 i = 0; i < 5; i++) {
        words[i] = snx_ld_le32(data + 16 + i * 4);
        CHECK(!words[i] || (words[i] >= 5 && words[i] <= (2u << 20)));
        total += (u64)words[i] * 4;
    }
    CHECK(total == size && words[0]);
    char expected[1024];
    size_t guest;
    vk_pipeline_file_key(expected, sizeof expected, &guest);
    if (!guest || file_key_bytes < guest || memcmp(data + 36, expected, guest)) {
        fprintf(stderr, "pipeline probe: device/capability key mismatch\n  expected: %.*s\n  captured: %.*s\n",
                (int)guest, expected, (int)file_key_bytes, (const char*)data + 36);
        free(data);
        return 1;
    }
    CHECK(guest && file_key_bytes >= guest && !memcmp(data + 36, expected, guest));
    PipeKey key;
    memcpy(&key, data + 36 + file_key_bytes, sizeof key);
    CHECK(!key.vs && !key.tcs && !key.tes && !key.gs && !key.fs);
    CHECK(key.ncolor <= 8 && key.vi.nattr <= 32 && key.vi.nbind <= 33 && key.arrays < 32);
    CHECK(key.heaps == (u32)g_vk.has_descriptor_heaps);
    for (u32 i = 0; i < key.vi.nattr; i++) CHECK(key.vi.attr[i].binding < key.vi.nbind);
    VkShaderModule modules[5] = {0};
    size_t at = 36 + file_key_bytes + pipe_key_bytes;
    u32 shader_words = 0;
    for (u32 i = 0; i < 5; i++) {
        if (!words[i]) continue;
        size_t bytes = (size_t)words[i] * 4;
        u32* aligned = malloc(bytes);
        CHECK(aligned && snx_ld_le32(data + at) == 0x07230203);
        memcpy(aligned, data + at, bytes);
        modules[i] = vk_module_spv(aligned, words[i]);
        free(aligned);
        CHECK(modules[i]);
        shader_words += words[i];
        at += bytes;
    }
    key.vs = modules[0]; key.tcs = modules[1]; key.tes = modules[2]; key.gs = modules[3]; key.fs = modules[4];
    vk_pipeline_quiesce();
    VkPipelineCache cache;
    VkPipelineCacheCreateInfo cache_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    CHECK(vkCreatePipelineCache(g_vk.dev, &cache_info, NULL, &cache) == VK_SUCCESS);
    u32 validation = g_vk_validation_messages;
    u64 made_before, hits_before, ignored;
    vk_pipeline_stats(&made_before, &hits_before, &ignored, &ignored, &ignored, &ignored);
    PFN_vkCreateGraphicsPipelines native = vkCreateGraphicsPipelines;
    g_pipeline_probe_intercept = (PipelineProbeIntercept){native, cache, disable_optimization, 0};
    vkCreateGraphicsPipelines = pipeline_probe_intercept;
    u64 begin = plat_ns();
    VkPipeline pipeline = vk_pipeline_get(&key);
    u64 elapsed = plat_ns() - begin;
    vkCreateGraphicsPipelines = native;
    CHECK(pipeline && elapsed && g_pipeline_probe_intercept.calls == 1 && g_vk_validation_messages == validation);
    u64 made_after, hits_after;
    vk_pipeline_stats(&made_after, &hits_after, &ignored, &ignored, &ignored, &ignored);
    CHECK(made_after == made_before + 1);
    printf("PIPELINE_COMPILE_PROBE {\"ns\":%llu,\"disable_optimization\":%d,\"shader_words\":%u,"
           "\"empty_application_cache\":1,\"rendered\":0,\"vendor\":%u,\"device\":%u,\"driver\":%u,"
           "\"application_cache_hits\":%llu,\"validation_messages\":%u}\n",
           (unsigned long long)elapsed, disable_optimization, shader_words, g_vk.props.vendorID,
           g_vk.props.deviceID, g_vk.props.driverVersion, (unsigned long long)(hits_after - hits_before),
           g_vk_validation_messages - validation);
    vkDestroyPipeline(g_vk.dev, pipeline, NULL);
    vkDestroyPipelineCache(g_vk.dev, cache, NULL);
    for (u32 i = 0; i < 5; i++)
        if (modules[i]) vkDestroyShaderModule(g_vk.dev, modules[i], NULL);
    free(data);
    return 0;
}
