/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The record of GPU-side writes that the renderer's caches check (gpu.h gpu_mem_written) */
#include <stdlib.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/write_pages.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* Each write stamps a new generation on every 4 KiB page it covers, by GPU address: the pages written and every other
   GPU page showing the same guest memory, found through the GMMU's index of mappings by guest address (gmmu_aliases)
   when the write is recorded. Writes are recorded a few thousand times a frame and asked about tens of thousands of
   times, so the aliasing is resolved on the side asked less, and a question reads one table with no translation. A
   mapping change stamps the pages it changes (nvdrv.c's gmmu_change_end), so a GPU page newly showing guest memory
   reads as written after anything asked before. Exact per page: a texture is dirty only when a write touched one of
   its own pages. Writers take the lock, readers don't (gpu_mem_dirty_since); the generation is published with
   release order for those reads.
   SNX_ALIAS_CHECK keeps the guest-address table as well and has gpu_mem_dirty_since answer the old way too,
   translating the range, warning when only that way finds a write */
static PMutex g_wlk = PMUTEX_INIT;
static WritePages g_wgpu, g_wcpu;
static u64 g_wgen;
static int g_wcheck = -1; /* SNX_ALIAS_CHECK, -1 until read */
static int wcheck_on(void) {
    int c = __atomic_load_n(&g_wcheck, __ATOMIC_RELAXED);
    if (c < 0) { /* threads reading it at once all read the same */
        const char* e = getenv("SNX_ALIAS_CHECK");
        c = e && *e && *e != '0';
        if (__atomic_exchange_n(&g_wcheck, c, __ATOMIC_RELAXED) < 0 && c)
            LOG("gpu: SNX_ALIAS_CHECK: GPU-side writes are checked through the GMMU too\n");
    }
    return c;
}
/* calls fn on each run of contiguous guest memory behind [gva, gva + size), from gmmu_run */
static void guest_runs(u64 gva, u64 size, int (*fn)(u64 cpu, u64 n, void* ctx), void* ctx) {
    u64 left = gva + size < gva ? ~(u64)0 - gva : size;
    while (left) {
        u64 cpu = 0, n = gmmu_run(gva, left, &cpu);
        if (n) {
            if (fn(cpu, n, ctx)) return;
        } else if ((n = 0x1000 - (gva & 0xFFF)) >= left)
            break;
        gva += n;
        left -= n;
    }
}
static void stamp_alias(void* ctx, u64 gva, u64 n) { write_pages_stamp(&g_wgpu, gva, n, *(const u64*)ctx); }
/* the renderer's watch on GPU-side writes (gpu_set_write_watch): any thread, under g_wlk */
static void (*g_wwatch)(u64 cpu, u64 n);
void gpu_set_write_watch(void (*fn)(u64 cpu, u64 n)) { __atomic_store_n(&g_wwatch, fn, __ATOMIC_RELEASE); }
/* guest memory written: the GPU pages showing it */
static void stamp_cpu(u64 cpu, u64 n, u64 gen) {
    void (*watch)(u64, u64) = __atomic_load_n(&g_wwatch, __ATOMIC_ACQUIRE);
    if (watch) watch(cpu, n);
    gmmu_aliases(cpu, n, stamp_alias, &gen);
    if (wcheck_on()) write_pages_stamp(&g_wcpu, cpu, n, gen);
}
static int stamp_run(u64 cpu, u64 n, void* ctx) {
    stamp_cpu(cpu, n, *(const u64*)ctx);
    return 0;
}
void gpu_mem_written(u64 gva, u64 size) {
    u64 gen;
    if (!size) return;
    pm_lock(&g_wlk);
    gen = g_wgen + 1;
    write_pages_stamp(&g_wgpu, gva, size, gen);
    guest_runs(gva, size, stamp_run, &gen);
    __atomic_store_n(&g_wgen, gen, __ATOMIC_RELEASE);
    pm_unlock(&g_wlk);
}
void gpu_mem_written_shader(u64 gva, u64 size) { gpu_mem_written(gva, size); }
void gpu_mem_written_cpu(u64 cpu, u64 size) {
    u64 gen;
    if (!size) return;
    pm_lock(&g_wlk);
    gen = g_wgen + 1;
    stamp_cpu(cpu, size, gen);
    __atomic_store_n(&g_wgen, gen, __ATOMIC_RELEASE);
    pm_unlock(&g_wlk);
}
u64 gpu_mem_generation(void) { return __atomic_load_n(&g_wgen, __ATOMIC_ACQUIRE); }
/* a save state's load: every page reads as written after anything asked before, through the tables' overflow, which
   covers every page */
void gpu_mem_written_all(void) {
    u64 gen;
    pm_lock(&g_wlk);
    gen = g_wgen + 1;
    __atomic_store_n(&g_wgpu.overflow, gen, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wcpu.overflow, gen, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wgen, gen, __ATOMIC_RELEASE);
    pm_unlock(&g_wlk);
}
typedef struct {
    u64 since;
    int dirty;
} DirtyAsk;
static int dirty_run(u64 cpu, u64 n, void* ctx) {
    DirtyAsk* q = (DirtyAsk*)ctx;
    q->dirty = write_pages_since(&g_wcpu, q->since, cpu, n);
    return q->dirty;
}
/* Without g_wlk, which only orders the writers: a stamp is stored after the memory it is for is written, so a reader
   holding the lock could already see new bytes under an old stamp, and reading the tables lock-free (write_pages.c)
   tells no less. Every draw asks this of each buffer, texture and target, and the lock was 2% of the GPU thread in
   one game */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    DirtyAsk q;
    if (!sz || since >= __atomic_load_n(&g_wgen, __ATOMIC_ACQUIRE)) return 0;
    if (write_pages_since(&g_wgpu, since, a, sz)) return 1;
    if (!wcheck_on()) return 0;
    q.since = since;
    q.dirty = 0;
    guest_runs(a, sz, dirty_run, &q);
    if (q.dirty)
        WARN_LIMIT(16, "gpu: guest memory behind GPU range %llx+%llx was written, but not stamped at its GPU pages\n",
                   (unsigned long long)a, (unsigned long long)sz);
    return q.dirty;
}
