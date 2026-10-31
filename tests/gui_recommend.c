/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Recommended Settings' rules (gui/recommend.c): scarletnx --probe's lines read, and each rule on both sides of its
   number, with the numbers it says */
#include "gui/recommend.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed(RecProbe* p, const char* text) {
    char line[512];
    const char* at = text;
    rec_probe_start(p);
    while (*at) {
        size_t n = strcspn(at, "\n");
        snprintf(line, sizeof line, "%.*s", (int)n, at);
        rec_probe_line(p, line);
        at += n + (at[n] == '\n');
    }
}

/* an RX 6700 XT's run, as the probe printed it */
static const char RUN[] = "probe: gpu\n"
                          "gpu=AMD Radeon RX 6700 XT (RADV NAVI22)\n"
                          "gpu_driver=radv Mesa 26.1.8\n"
                          "gpu_type=discrete\n"
                          "gpu_bc=1\n"
                          "gpu_astc=0\n"
                          "gpu_heap_mb=12272\n"
                          "gpu_budget_mb=11490\n"
                          "gpu_budget_from=driver\n"
                          "probe: pipelines\n"
                          "pipelines_auto=10\n"
                          "pipelines=6:206,10:227\n"
                          "probe: memory\n"
                          "cpus=12\n"
                          "cores=6\n"
                          "ram_available_mb=21267\n"
                          "probe: done\n";

int main(void) {
    RecProbe p;
    RecRow rows[REC_COUNT];
    char text[2048];

    /* the run read whole */
    feed(&p, RUN);
    assert(!strcmp(p.step, "done") && !strcmp(p.gpu, "AMD Radeon RX 6700 XT (RADV NAVI22)"));
    assert(!strcmp(p.gpu_driver, "radv Mesa 26.1.8") && !strcmp(p.gpu_type, "discrete"));
    assert(p.bc == 1 && p.astc == 0 && p.heap_mb == 12272 && p.budget_mb == 11490 && p.budget_from_driver);
    assert(p.auto_threads == 10 && p.n_threads == 2 && p.threads[0] == 6 && p.rates[0] == 206 && p.threads[1] == 10);
    assert(p.cpus == 12 && p.cores == 6 && p.ram_mb == 21267);
    assert(rec_gpu_tested(&p) && rec_pipelines_tested(&p) && rec_memory_tested(&p));
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_ASTC_BC].key, "astc_bc") && !strcmp(rows[REC_PIPELINE_THREADS].key, "pipeline_threads") &&
           !strcmp(rows[REC_MEMORY_FIT].key, "memory_fit"));
    /* every rule leaves this machine as it is, and says why with its numbers */
    assert(rows[REC_ASTC_BC].tested && !strcmp(rows[REC_ASTC_BC].value, "0"));
    assert(strstr(rows[REC_ASTC_BC].why, "11490 MB") && strstr(rows[REC_ASTC_BC].why, "8192 MB"));
    assert(rows[REC_PIPELINE_THREADS].tested && !strcmp(rows[REC_PIPELINE_THREADS].value, "0"));
    assert(strstr(rows[REC_PIPELINE_THREADS].why, "Auto makes 10 threads: 227 pipelines a second"));
    assert(rows[REC_MEMORY_FIT].tested && !strcmp(rows[REC_MEMORY_FIT].value, "0"));
    assert(strstr(rows[REC_MEMORY_FIT].why, "21267 MB") && strstr(rows[REC_MEMORY_FIT].why, "5009 MB"));

    /* ASTC as BC7: an 8 GB card's budget is under the line; a GPU that samples ASTC, or no BCn, gets nothing */
    snprintf(text, sizeof text, "%s", RUN);
    memcpy(strstr(text, "gpu_budget_mb=11490"), "gpu_budget_mb=07079", 19);
    feed(&p, text);
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_ASTC_BC].value, "1") && strstr(rows[REC_ASTC_BC].why, "7079 MB, under 8192 MB"));
    memcpy(strstr(text, "gpu_astc=0"), "gpu_astc=1", 10);
    feed(&p, text);
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_ASTC_BC].value, "0") && strstr(rows[REC_ASTC_BC].why, "samples ASTC itself"));
    memcpy(strstr(text, "gpu_astc=1"), "gpu_astc=0", 10);
    memcpy(strstr(text, "gpu_bc=1"), "gpu_bc=0", 8);
    feed(&p, text);
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_ASTC_BC].value, "0") && strstr(rows[REC_ASTC_BC].why, "neither BCn nor ASTC"));

    /* pipeline threads: 6 threads at 95.8% of the best are proposed, 91% is not; more than Auto's 12 when only they
       come near the best; one number tried leaves Auto */
    snprintf(text, sizeof text, "%s", RUN);
    memcpy(strstr(text, "pipelines=6:206,10:227"), "pipelines=6:206,10:215", 22);
    feed(&p, text);
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_PIPELINE_THREADS].value, "6"));
    assert(
        strstr(rows[REC_PIPELINE_THREADS].why, "6 threads make 206 pipelines a second, 96% of the best (215 with 10)"));
    feed(&p, "probe: pipelines\npipelines_auto=12\npipelines=8:200,12:230,18:300\n");
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_PIPELINE_THREADS].value, "18") &&
           strstr(rows[REC_PIPELINE_THREADS].why, "Auto's 12 make 230"));
    feed(&p, "probe: pipelines\npipelines_auto=6\npipelines=6:206\n");
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_PIPELINE_THREADS].value, "0") &&
           strstr(rows[REC_PIPELINE_THREADS].why, "as many as the cores"));

    /* Memory fit: under the pool and the room it needs; Memory (GB) 6 raises the line */
    assert(rec_memory_fit_under_mb(0) == 5009 && rec_memory_fit_under_mb(4) == 5009);
    assert(rec_memory_fit_under_mb(6) == 6640 && rec_memory_fit_under_mb(8) == 8688);
    feed(&p, "probe: memory\nram_available_mb=3900\n");
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_MEMORY_FIT].value, "1") &&
           strstr(rows[REC_MEMORY_FIT].why, "3900 MB of RAM available, under 5009 MB"));
    feed(&p, "probe: memory\nram_available_mb=6000\n");
    rec_rules(&p, 0, rows);
    assert(!strcmp(rows[REC_MEMORY_FIT].value, "0"));
    rec_rules(&p, 6, rows);
    assert(!strcmp(rows[REC_MEMORY_FIT].value, "1") && strstr(rows[REC_MEMORY_FIT].why, "under 6640 MB"));

    /* a step that failed: its rows are not tested, and say what Vulkan or the system said */
    feed(&p, "probe: gpu\ngpu_error=no Vulkan device answered (vkCreateInstance: VK_ERROR_INCOMPATIBLE_DRIVER)\n"
             "probe: memory\nram_error=this system tells no memory it can still give\nprobe: done\n");
    rec_rules(&p, 0, rows);
    assert(!rows[REC_ASTC_BC].tested && !rows[REC_ASTC_BC].value[0]);
    assert(strstr(rows[REC_ASTC_BC].why, "Not tested: no Vulkan device answered (vkCreateInstance"));
    assert(!rows[REC_PIPELINE_THREADS].tested &&
           strstr(rows[REC_PIPELINE_THREADS].why, "VK_ERROR_INCOMPATIBLE_DRIVER"));
    assert(!rows[REC_MEMORY_FIT].tested && strstr(rows[REC_MEMORY_FIT].why, "tells no memory"));
    feed(
        &p,
        "probe: pipelines\npipelines_auto=10\npipelines_error=vkCreateComputePipelines: VK_ERROR_OUT_OF_HOST_MEMORY\n");
    rec_rules(&p, 0, rows);
    assert(!rows[REC_PIPELINE_THREADS].tested && strstr(rows[REC_PIPELINE_THREADS].why, "VK_ERROR_OUT_OF_HOST_MEMORY"));
    /* a line of no key, or a key it doesn't know, changes nothing */
    feed(&p, "nonsense\nfuture_key=1\n=x\n");
    assert(!p.step[0] && p.ram_mb == -1 && p.bc == -1);

    puts("gui_recommend passed");
    return 0;
}
