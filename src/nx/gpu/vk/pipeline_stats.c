/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/pipeline_stats.h"
#include <stdio.h>
#include "common/utilities/common.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"

/* the file, opened at the first pipeline (lock) */
static struct {
    PMutex lock;
    FILE* f;
    int failed;
} g_pstats = {PMUTEX_INIT, NULL, 0};

VkPipelineCreateFlags vk_pipeline_stats_flags(void) {
    return g_vk.has_exec_stats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0;
}

/* the stages of an executable, as the shader corpus names them, joined by '+' */
static void stage_names(VkShaderStageFlags stages, char* out, size_t n) {
    static const struct {
        VkShaderStageFlags bit;
        const char* name;
    } names[] = {{VK_SHADER_STAGE_VERTEX_BIT, "vs"},
                 {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "tcs"},
                 {VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "tes"},
                 {VK_SHADER_STAGE_GEOMETRY_BIT, "gs"},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, "fs"},
                 {VK_SHADER_STAGE_COMPUTE_BIT, "cs"},
                 {VK_SHADER_STAGE_TASK_BIT_EXT, "task"},
                 {VK_SHADER_STAGE_MESH_BIT_EXT, "mesh"}};
    size_t at = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names && at < n; i++)
        if (stages & names[i].bit) at += (size_t)snprintf(out + at, n - at, "%s%s", at ? "+" : "", names[i].name);
}

/* s as a CSV field: in quotes, a quote doubled */
static void csv_text(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

/* the file, opened at the first call: NULL when it can't be (said once). g_pstats.lock held */
static FILE* stats_file(void) {
    if (g_pstats.f || g_pstats.failed) return g_pstats.f;
    g_pstats.f = snx_fopen(g_settings.pipeline_stats, "w");
    if (!g_pstats.f) {
        g_pstats.failed = 1;
        WARN("vulkan: pipeline_stats: %s can't be written: no pipeline statistics\n", g_settings.pipeline_stats);
        return NULL;
    }
    fputs("what,vs,tcs,tes,gs,fs,cs,executable,stages,subgroup,statistic,value\n", g_pstats.f);
    LOG("vulkan: pipeline_stats: the driver's statistics of each whole and optimized graphics pipeline, and of each "
        "compute pipeline, go to %s\n",
        g_settings.pipeline_stats);
    return g_pstats.f;
}

/* p's rows, its stages' programs in the columns vs to cs */
static void stats_rows(VkPipeline p, const char* what, const u64 programs[6]) {
    if (!g_vk.has_exec_stats || !p) return;
    VkPipelineInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = p};
    uint32_t n = 0;
    if (vkGetPipelineExecutablePropertiesKHR(g_vk.dev, &pi, &n, NULL) != VK_SUCCESS || !n) return;
    VkPipelineExecutablePropertiesKHR props[16];
    if (n > 16) n = 16; /* a graphics pipeline has one for each stage at most, and a driver's own few */
    for (uint32_t i = 0; i < n; i++)
        props[i] = (VkPipelineExecutablePropertiesKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    if (vkGetPipelineExecutablePropertiesKHR(g_vk.dev, &pi, &n, props) < 0) return;
    pm_lock(&g_pstats.lock);
    FILE* f = stats_file();
    for (uint32_t e = 0; f && e < n; e++) {
        VkPipelineExecutableInfoKHR ei = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = p, .executableIndex = e};
        uint32_t ns = 0;
        VkPipelineExecutableStatisticKHR st[64];
        /* Inside the lock, around the driver's query only: neither the wait for the lock nor the file's I/O is
           this zone's time */
        SNX_ZONE(z, "vk pipe stats write");
        int got = vkGetPipelineExecutableStatisticsKHR(g_vk.dev, &ei, &ns, NULL) == VK_SUCCESS && ns;
        if (got) {
            if (ns > 64) ns = 64;
            for (uint32_t i = 0; i < ns; i++)
                st[i] = (VkPipelineExecutableStatisticKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
            got = vkGetPipelineExecutableStatisticsKHR(g_vk.dev, &ei, &ns, st) >= 0;
        }
        SNX_ZONE_TEXT(z, "%s: executable %u, %s", what, e, got ? "statistics read" : "no statistics");
        SNX_ZONE_VALUE(z, got ? ns : 0);
        SNX_ZONE_END(z);
        if (!got) continue;
        char stages[64];
        stage_names(props[e].stages, stages, sizeof stages);
        for (uint32_t i = 0; i < ns; i++) {
            fprintf(f, "%s", what);
            for (int s = 0; s < 6; s++) {
                if (programs[s])
                    fprintf(f, ",%016llx", (unsigned long long)programs[s]);
                else
                    fputc(',', f);
            }
            fputc(',', f);
            csv_text(f, props[e].name);
            fprintf(f, ",%s,%u,", stages, props[e].subgroupSize);
            csv_text(f, st[i].name);
            switch (st[i].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                fprintf(f, ",%u\n", st[i].value.b32 ? 1u : 0u);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                fprintf(f, ",%lld\n", (long long)st[i].value.i64);
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                fprintf(f, ",%llu\n", (unsigned long long)st[i].value.u64);
                break;
            default:
                fprintf(f, ",%g\n", st[i].value.f64);
                break;
            }
        }
    }
    if (f) fflush(f);
    pm_unlock(&g_pstats.lock);
}
void vk_pipeline_stats_write(VkPipeline p, const char* what, const u64 programs[5]) {
    const u64 all[6] = {programs[0], programs[1], programs[2], programs[3], programs[4], 0};
    stats_rows(p, what, all);
}
void vk_pipeline_stats_write_compute(VkPipeline p, u64 program) {
    const u64 all[6] = {0, 0, 0, 0, 0, program};
    stats_rows(p, "compute", all);
}
