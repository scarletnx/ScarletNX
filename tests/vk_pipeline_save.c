/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The actual periodic cache saver with a stand-in driver and deterministic interleavings. No GPU is used. A busy
   or queued graphics compile must retain dirty state and never ask the driver to snapshot its cache. Activity that
   starts while a saver takes its mutex must be checked again; a later quiet save and the exit save must persist it. */
/* Include only the cache saver. ASan can retain callback tables and their callees in the full pipeline translation
   unit, so this fixture must not rely on linker garbage collection to discard unrelated shader/driver code.
   Keep the deterministic lock hooks apart from host.c's locks, which the linked common libraries also provide. */
#define pm_lock pipeline_test_lock
#define pm_unlock pipeline_test_unlock
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;
VkState g_vk;
/* Only the owner state read by the production cache saver: no shader loader, pipeline worker or callback table. */
static VkPipelineCache g_pcache;
static char g_pcache_path[1200], g_pcache_tmp[1230];
static int g_pcache_dirty;
static u64 g_pcache_used;
static PMutex g_pj_lock = PMUTEX_INIT;
static void *g_pj_head, *g_pw_head;
static int g_pj_busy;
static struct {
    int ship_ready;
    u32 nidx, ship_used, ship_said;
} g_pf;
#include "nx/gpu/vk/pipeline_cache_save.inc"
#undef pm_lock
#undef pm_unlock

static u64 now;
static int snapshots, save_lock_event;
static int queued;

u64 plat_ns(void) { return now; }
void pipeline_test_lock(PMutex* m) {
    if (m == &g_psave && save_lock_event) {
        if (save_lock_event == 1)
            g_pj_busy = 1;
        else
            pcache_note();
        save_lock_event = 0;
    }
}
void pipeline_test_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
int plat_replace_file(const char* from, const char* to) { return rename(from, to); }
static VKAPI_ATTR VkResult VKAPI_CALL snapshot(VkDevice device, VkPipelineCache cache, size_t* size, void* data) {
    (void)device, (void)cache;
    snapshots++;
    if (data) memcpy(data, "warm", 4);
    *size = 4;
    return VK_SUCCESS;
}
PFN_vkGetPipelineCacheData vkGetPipelineCacheData = snapshot;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "pipeline_save:%d: %s\n", __LINE__, #x); return 1; } } while (0)
static void reset(void) {
    g_pj_head = g_pw_head = NULL;
    g_pj_busy = 0;
    g_pcache_used = 0;
    g_pcache_dirty = 1;
    snapshots = save_lock_event = 0;
    now = PCACHE_QUIET_NS + 1;
}
int main(void) {
    g_settings.vulkan_pipeline_cache = 1; /* its default: the cache is written */
    g_pcache = (VkPipelineCache)(uintptr_t)1;
    snprintf(g_pcache_path, sizeof g_pcache_path, "pipeline_save_test.bin");
    snprintf(g_pcache_tmp, sizeof g_pcache_tmp, "pipeline_save_test.tmp");
    reset();
    pcache_note();
    vk_pipeline_save(0);
    CHECK(!snapshots && g_pcache_dirty);
    reset();
    g_pj_head = &queued;
    vk_pipeline_save(0);
    CHECK(!snapshots && g_pcache_dirty);
    reset();
    g_pw_head = &queued;
    vk_pipeline_save(0);
    CHECK(!snapshots && g_pcache_dirty);
    reset();
    g_pj_busy = 1;
    vk_pipeline_save(0);
    CHECK(!snapshots && g_pcache_dirty);
    reset();
    save_lock_event = 1; /* job starts after the first quiet/idle check */
    vk_pipeline_save(0);
    CHECK(!snapshots && g_pcache_dirty && g_pj_busy);
    reset();
    save_lock_event = 2; /* a compile publishes activity while the save mutex is taken */
    vk_pipeline_save(0);
    CHECK(!snapshots && g_pcache_dirty && g_pcache_used == now);
    /* The work stayed dirty; once quiet it is saved, and the driver is not called again without new work. */
    now += PCACHE_QUIET_NS;
    vk_pipeline_save(0);
    CHECK(snapshots == 2 && !g_pcache_dirty);
    FILE* f = fopen(g_pcache_path, "rb");
    char data[4];
    CHECK(f && fread(data, 1, sizeof data, f) == sizeof data && !memcmp(data, "warm", 4));
    CHECK(!fclose(f));
    vk_pipeline_save(0);
    CHECK(snapshots == 2);
    /* Exit retains its unconditional flush, after the renderer has quiesced its workers. */
    reset();
    g_pj_busy = 1;
    pcache_note();
    vk_pipeline_save(1);
    CHECK(snapshots == 2 && !g_pcache_dirty);
    CHECK(!remove(g_pcache_path));
    puts("pipeline cache saves defer active work and retain warm-cache persistence");
    return 0;
}
