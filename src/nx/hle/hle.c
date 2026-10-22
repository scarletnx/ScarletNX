/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* g_hle, the HLE's state, its initialization, and its pools (hle.h) */
#include "nx/hle/hle.h"
#include <assert.h>

/* zero, in .bss: hle_init sets what starts out non-zero */
Hle g_hle;

void hle_init(void) {
    int i;
    /* every lock: PMUTEX_INIT's value isn't all zero on every platform */
    pm_init(&g_hle.kernel.klock);
    pm_init(&g_hle.kernel.pool_lk);
    pm_init(&g_hle.svcmem.views_lk);
    pm_init(&g_hle.thread.core_lk);
    pm_init(&g_hle.ipc.lk);
    pm_init(&g_hle.acc.pic.lk);
    pm_init(&g_hle.audio.lk);
    pm_init(&g_hle.audren.lk);
    pm_init(&g_hle.ldr.lock);
    pm_init(&g_hle.misc.mm_lock);
    pm_init(&g_hle.misc.aoc_lock);
    pm_init(&g_hle.misc.irs_lock);
    pm_init(&g_hle.hid.input_lock);
    pm_init(&g_hle.nvdrv.seen_lock);
    pm_init(&g_hle.bsd.iface_lk);
    pm_init(&g_hle.bsd.bind_lk);
    pm_init(&g_hle.bsd.client_lk);
    pm_init(&g_hle.ldn.ldns_lk);
    pm_init(&g_hle.ssl.init_lk);
    pm_init(&g_hle.sm.lk);
    /* the kernel: the title until the module names its own, the first thread id, the handle table's empty free list,
       and the process and resource limit objects, which live as long as the program */
    g_hle.kernel.title_id = 0x0100000000001000ull;
    g_hle.kernel.next_tid = 0x50;
    g_hle.kernel.hfree = -1;
    g_hle.kernel.hlinear = 1;
    g_hle.kernel.process.type = KO_PROCESS;
    g_hle.kernel.process.refs = 1;
    kobj_set_name(&g_hle.kernel.process, "process");
    g_hle.svc.reslimit.type = KO_RESLIMIT;
    g_hle.svc.reslimit.refs = 1;
    kobj_set_name(&g_hle.svc.reslimit, "resource limit");
    for (i = 0; i < NCORES; i++) g_hle.thread.core_best[i] = 64; /* no thread waits for a core */
    g_hle.thread.tls_used = 8;
    /* an application's cores and priorities, until the npdm says (main.c) */
    g_hle.svcmem.core_mask = 0xF;
    g_hle.svcmem.prio_mask = 0xFFFFFFFFFFFFFFF0ull;
    /* the services' defaults */
    g_hle.misc.apm_config[0] = g_hle.misc.apm_config[1] = APM_CONFIGURATION;
    g_hle.audio.device_volume = 1.0f;
    g_hle.audren.gain = 1.0f;
    g_hle.set.vibration = 1.0f;
    g_hle.set.battery_percentage = 1;
}

/* ============================================================ the pools */
static int slot_free(const void* slot, size_t size) {
    const u8* p = (const u8*)slot;
    return !p[0] && !memcmp(p, p + 1, size - 1);
}
void* pool_take(void* base, size_t size, u32 count, u32* next, const char* what) {
    u32 i = *next, k;
    for (k = 0; k < count; k++, i = i + 1 == count ? 0 : i + 1)
        if (slot_free((u8*)base + (size_t)i * size, size)) {
            *next = i + 1 == count ? 0 : i + 1;
            return (u8*)base + (size_t)i * size;
        }
    assert(!"an HLE pool is full");
    (void)what;
    /*fatal("More than %u %s at once", (unsigned)count, what);*/ /* NDEBUG builds */
    return NULL;
}
void pool_put(void* slot, size_t size) { memset(slot, 0, size); }
