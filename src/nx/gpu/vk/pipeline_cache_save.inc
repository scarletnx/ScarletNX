/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Driver pipeline cache activity and deferred snapshots. The owner in pipeline.c defines g_pipeline, with the cache
   state, the saver's lock, worker status and shader-preparation progress, before including this implementation. */
static void pcache_note(void) {
    __atomic_store_n(&g_pipeline.pcache_used, plat_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_pipeline.pcache_dirty, 1, __ATOMIC_RELAXED);
}

VkPipelineCache vk_pipeline_cache(void) {
    pcache_note();
    return g_pipeline.pcache;
}
/* writes the pipeline cache when pipelines were made since the last write. Asked every few seconds and at exit (the
   renderer's save), outside vk_lock: the driver synchronizes the cache itself. The driver holds the cache's lock while
   it copies the cache out, so a pipeline made meanwhile waits for the whole copy, and the copies came every five seconds
   while pipelines were being made: a pipeline once waited 3.2 s with recording stopped behind it. So the asking every
   few seconds writes it only after PCACHE_QUIET_NS without a new pipeline and with no queued or in-flight graphics
   work. At exit it is written regardless. Saves one at a time, so the one at exit waits for one in progress to finish
   its file before the process ends */
#define PCACHE_QUIET_NS 20000000000ull
static int pcache_periodic_ready(void) {
    pm_lock(&g_pipeline.pj_lock);
    int idle = !g_pipeline.pj_head && !g_pipeline.pw_head && !g_pipeline.pj_busy;
    /* A worker notes completion before dropping busy under this mutex. Read the clock after observing busy, so a
       compile that finished while the saver took the lock cannot leave an older quiet-period decision in force. */
    u64 quiet = plat_ns() - __atomic_load_n(&g_pipeline.pcache_used, __ATOMIC_RELAXED);
    pm_unlock(&g_pipeline.pj_lock);
    return idle && quiet >= PCACHE_QUIET_NS;
}
void vk_pipeline_save(int at_exit) {
    if (!at_exit && !pcache_periodic_ready()) return;
    pm_lock(&g_pipeline.psave);
    /* Waiting for another saver can span new submissions. Recheck before clearing dirty or calling the driver;
       queued work matters too, because a worker can begin it as soon as its previous pipeline finishes. */
    if (!at_exit && !pcache_periodic_ready()) {
        pm_unlock(&g_pipeline.psave);
        return;
    }
    /* the shipped file's shaders the draws found, when that changed: nidx is settled once ship_ready */
    if (__atomic_load_n(&g_pipeline.pf.ship_ready, __ATOMIC_ACQUIRE) && g_pipeline.pf.nidx) {
        u32 used = __atomic_load_n(&g_pipeline.pf.ship_used, __ATOMIC_RELAXED);
        if (used != g_pipeline.pf.ship_said) {
            g_pipeline.pf.ship_said = used;
            LOG("vulkan: %u of the %u shaders translated from the game's files found by its draws so far\n", used,
                g_pipeline.pf.nidx);
        }
    }
    /* vk_pipeline_cache=0: the driver's cache is neither read nor written, its size only logged at the end */
    if (!g_settings.vulkan_pipeline_cache && at_exit && g_pipeline.pcache) {
        size_t n = 0;
        if (vkGetPipelineCacheData(g_vk.dev, g_pipeline.pcache, &n, NULL) == VK_SUCCESS)
            LOG("vulkan: the pipeline cache of this run holds %zu KB (vk_pipeline_cache=0: not written)\n", n >> 10);
    }
    if (!g_pipeline.pcache || !g_settings.vulkan_pipeline_cache ||
        !__atomic_exchange_n(&g_pipeline.pcache_dirty, 0, __ATOMIC_RELAXED)) {
        pm_unlock(&g_pipeline.psave);
        return;
    }
    size_t saved;
    char why[512];
    if (snx_vk_pipeline_cache_save(g_vk.dev, g_pipeline.pcache, g_pipeline.pcache_path, &saved, why, sizeof why)) {
        WARN("vulkan: the pipeline cache could not be written to %s: %s\n", g_pipeline.pcache_path, why);
        pcache_note(); /* written again after another quiet time, or at exit */
    }
    pm_unlock(&g_pipeline.psave);
}
