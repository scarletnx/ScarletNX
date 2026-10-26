/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Reconcile a shader's mapped output with guest memory. The host may have written the guest range while the GPU was
   running: shader-changed bytes land only where the guest still holds the value the shader originally read. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"

/* Nested because a result can flush an overlapping render target while landing. */
static _Thread_local unsigned t_result_depth;
void vk_result_begin(void) { t_result_depth++; }
void vk_result_end(void) { t_result_depth--; }
int vk_result_landing(void) { return t_result_depth != 0; }

/* whether [p, p + n) overlaps the guest's address space, where pages the GPU holds fault when read */
static int in_guest(const u8* p, u32 n) { return p < g_mem + AS_SIZE && p + n > g_mem; }

/* the bytes of [k, k + w) where res and org differ (w at most 8): eight at a time when there are eight */
static int granule_differs(const u8* res, const u8* org, u32 k, u32 w) {
    u64 a, b;
    if (w < 8) return memcmp(res + k, org + k, w) != 0;
    memcpy(&a, res + k, 8);
    memcpy(&b, org + k, 8);
    return a != b;
}

void vk_write_changed_runs(u64 addr, const u8* res, const u8* org, u32 size, VkChangedRun run, void* user) {
    /* res and org are host copies: the shader's output read back into host-cached memory, and the bytes it started
       from, snapshotted when it was bound (buffer.c, compute.c, surface.c). Comparing them reads no guest page, so it
       can't fault on one the GPU holds. Only cur below reads guest memory, after vk_rt_before_write, which the first
       change takes: a result that changed nothing reads no guest memory, allocates and flushes nothing */
    u8 *dst = NULL, *tmp = NULL;
    const u8* cur = NULL;
    u32 run_from = 0, run_to = 0, ws = 0, we = 0; /* the run of changed granules, and the bytes to write */
    int begun = 0, changed = 0;
    if (!size) return;
    if (in_guest(res, size) || in_guest(org, size)) fatal("vulkan: a storage result compared in guest memory");
    for (u32 i = 0; i < size;) {
        u32 end = size - i < 1024 ? size - i : 1024;
        end += i;
        /* Large storage bindings often change only a few bytes: the host's vectorized comparison of the two host
           copies skips their unchanged ranges, and guest memory is read only where the shader changed a byte */
        if (!memcmp(res + i, org + i, end - i)) {
            i = end;
            continue;
        }
        for (u32 k = i; k < end; k += 8) { /* i is a multiple of 1024, so granules start at multiples of 8 */
            u32 w = end - k < 8 ? end - k : 8;
            if (!granule_differs(res, org, k, w)) continue;
            if (run && run_to == k && run_to)
                run_to = k + w;
            else {
                if (run && run_to) run(user, run_from, run_to);
                run_from = k;
                run_to = k + w;
            }
            if (!begun) {
                begun = 1;
                vk_result_begin();
                vk_rt_before_write(addr, size);
                dst = gmem_ptr(addr, size);
                cur = dst;
                if (!cur) {
                    tmp = malloc(size);
                    if (!tmp) fatal("Out of memory landing a shader storage buffer");
                    gmem_read(addr, tmp, size);
                    cur = tmp;
                }
            }
            /* the bytes the shader changed where guest memory still holds what it started from, in runs that
               go on across granules */
            for (u32 b = k; b < k + w; b++) {
                if (res[b] == org[b] || cur[b] != org[b]) continue;
                if (we != b) { /* not where the pending run ends: it is written, and a new one starts */
                    if (we != ws) {
                        if (dst)
                            memcpy(dst + ws, res + ws, we - ws);
                        else
                            gmem_write(addr + ws, res + ws, we - ws);
                    }
                    ws = b;
                }
                we = b + 1;
                changed = 1;
            }
        }
        i = end;
    }
    if (run && run_to) run(user, run_from, run_to);
    if (we != ws) {
        if (dst)
            memcpy(dst + ws, res + ws, we - ws);
        else
            gmem_write(addr + ws, res + ws, we - ws);
    }
    free(tmp);
    if (changed) gpu_mem_written_shader(addr, size); /* other GPU virtual addresses can alias these guest pages */
    if (begun) vk_result_end();
}
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) {
    vk_write_changed_runs(addr, res, org, size, NULL, NULL);
}
