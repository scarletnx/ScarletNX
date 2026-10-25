/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_trace=1: the NVN functions a game resolves through nvnBootstrapLoader and nvnDeviceGetProcAddress, how
   often each is called, and how many GPFIFO submissions (by any thread) happen during each queue call. The wrappers
   replace the functions' entries in the recompiler's dispatch table, so they see calls through it (function pointers,
   which is how NVN is called) but not direct branches, unless the game module was recompiled with
   --hook-direct-calls: the counts are lower bounds otherwise. The original functions always run, so the GPU path is
   unchanged. */
#include "nx/gpu/nvn_trace.h"
#include "nx/gpu/nvn/nvn_direct.h"
#include "nx/gpu/gpu.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/rtapi.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    u64 addr, calls;
    GuestFn original;
    int queue_op; /* a queue call: its GPFIFO submissions are counted */
    char name[128];
} NvProc;
#define NVN_PROC_SLOTS 1024
static NvProc procs[NVN_PROC_SLOTS];
static u32 nprocs;
/* Guest threads can resolve functions at the same time: registration takes the lock, and publishes a slot (addr with
   release ordering) only once its original and name are filled in. Calls read the slots without it. */
static PMutex procs_lock = PMUTEX_INIT;
static GuestFn bootstrap_original, getproc_original;
static int tracing;
static void getproc_trace(CPU* c, u64 entry);

static int readable(u64 addr) {
    if (addr < AS_BASE || addr >= AS_SIZE) return 0;
    MemBlock block;
    mem_query(addr, &block);
    return block.state != MS_FREE && block.state != MS_INACCESSIBLE && (block.perm & PERM_R);
}

int nvn_read_name(u64 ptr, char name[128]) {
    u64 readable_end = 0;
    size_t i;
    for (i = 0; i < 127 && ptr + i >= ptr; i++) {
        u64 at = ptr + i;
        if (at >= readable_end) {
            if (!readable(at)) break;
            readable_end = (at | 0xFFF) + 1;
        }
        name[i] = *(const char*)GP(at);
        if (!name[i]) return i != 0;
    }
    name[i] = 0;
    return 0;
}

static u32 slot_hash(u64 addr) { return (u32)((addr >> 2) * 2654435761u) & (NVN_PROC_SLOTS - 1); }
/* the slot of addr, or NULL; never loops on a full table */
static NvProc* proc_find(u64 addr) {
    for (u32 n = 0, i = slot_hash(addr); n < NVN_PROC_SLOTS; n++, i = (i + 1) & (NVN_PROC_SLOTS - 1)) {
        u64 a = __atomic_load_n(&procs[i].addr, __ATOMIC_ACQUIRE);
        if (a == addr) return &procs[i];
        if (!a) return NULL;
    }
    return NULL;
}

static void proc_trace(CPU* c, u64 entry) {
    NvProc* p = proc_find(entry);
    if (!p) { /* generated code reads g_disp without ordering: on a weakly ordered host the lock gives it */
        pm_lock(&procs_lock);
        p = proc_find(entry);
        pm_unlock(&procs_lock);
        if (!p) fatal("nvn: a trace wrapper runs for a function it never registered (%llx)", (unsigned long long)entry);
    }
    __atomic_fetch_add(&p->calls, 1, __ATOMIC_RELAXED);
    if (!p->queue_op) {
        p->original(c, entry);
        return;
    }
    if (!strcmp(p->name, "nvnQueueSubmitCommands")) {
        u64 list = 0, ptr = c->x[2]; /* nvnQueueSubmitCommands(queue, count, lists) */
        if (c->x[1] && ptr < AS_SIZE - 8 && readable(ptr) && readable(ptr + 7)) list = gr64(ptr);
        LOG("nvn-submit: queue=%llx count=%u lists=%llx first=%llx\n", (unsigned long long)c->x[0], (u32)c->x[1],
            (unsigned long long)ptr, (unsigned long long)list);
    }
    u64 before = __atomic_load_n(&g_gpu_submits, __ATOMIC_RELAXED);
    p->original(c, entry);
    LOG("nvn-%s: GPFIFO submissions during call=%llu\n", p->name,
        (unsigned long long)(__atomic_load_n(&g_gpu_submits, __ATOMIC_RELAXED) - before));
}

static void trace_proc(u64 addr, const char* name) {
    if (addr < g_code_lo || addr >= g_code_hi || (addr & 3)) return;
    pm_lock(&procs_lock);
    GuestFn* dispatch = &g_disp[(addr - g_code_lo) / 4];
    NvProc* p = proc_find(addr);
    if (p) {
        /* identical functions folded to one address: their calls count under the first name */
        if (strcmp(p->name, name)) LOG_LIMIT(8, "nvn: %s shares its address with %s\n", name, p->name);
    } else if (*dispatch != getproc_trace && nprocs < NVN_PROC_SLOTS) {
        u32 i = slot_hash(addr);
        while (procs[i].addr) i = (i + 1) & (NVN_PROC_SLOTS - 1);
        p = &procs[i];
        p->original = *dispatch ? *dispatch : g_rtapi.miss; /* what snx_lookup gives an empty slot */
        memcpy(p->name, name, strlen(name) + 1);
        p->queue_op = !strcmp(name, "nvnQueueSubmitCommands") || !strcmp(name, "nvnQueueFlush") ||
                      !strcmp(name, "nvnQueueFenceSync") || !strcmp(name, "nvnQueuePresentTexture") ||
                      !strcmp(name, "nvnQueueWaitSync");
        __atomic_store_n(&p->addr, addr, __ATOMIC_RELEASE);
        nprocs++;
        __atomic_store_n(dispatch, proc_trace, __ATOMIC_RELEASE);
    }
    pm_unlock(&procs_lock);
}

static void getproc_trace(CPU* c, u64 entry) {
    char name[128];
    int valid = nvn_read_name(c->x[1], name); /* nvnDeviceGetProcAddress(device, name) */
    GuestFn original = __atomic_load_n(&getproc_original, __ATOMIC_ACQUIRE);
    if (!original) { /* as in proc_trace: the lock orders a host where g_disp's plain load doesn't */
        pm_lock(&procs_lock);
        original = getproc_original;
        pm_unlock(&procs_lock);
        if (!original) fatal("nvn: the nvnDeviceGetProcAddress wrapper runs before its original is known");
    }
    original(c, entry);
    if (!valid) return;
    LOG("nvn: %s -> %llx\n", name, (unsigned long long)c->x[0]);
    trace_proc(c->x[0], name);
}

static void bootstrap_trace(CPU* c, u64 entry) {
    char name[128];
    int valid = nvn_read_name(c->x[0], name); /* nvnBootstrapLoader(name) */
    bootstrap_original(c, entry);
    if (!valid) return;
    LOG("nvn: %s -> %llx\n", name, (unsigned long long)c->x[0]);
    if (strcmp(name, "nvnDeviceGetProcAddress")) {
        trace_proc(c->x[0], name);
        return;
    }
    u64 resolver = c->x[0];
    if (resolver < g_code_lo || resolver >= g_code_hi || (resolver & 3)) return;
    pm_lock(&procs_lock);
    GuestFn* slot = &g_disp[(resolver - g_code_lo) / 4];
    if (*slot && *slot != getproc_trace) {
        __atomic_store_n(&getproc_original, *slot, __ATOMIC_RELEASE);
        __atomic_store_n(slot, getproc_trace, __ATOMIC_RELEASE);
    }
    pm_unlock(&procs_lock);
}

void nvn_trace_init(void) {
    if (g_settings.nvn_trace && nvn_direct_active()) WARN("nvn: nvn_trace has nothing to wrap under nvn_direct\n");
    tracing = g_settings.nvn_trace && !nvn_direct_active();
    if (!tracing) return;
    u64 entry = kernel_find_symbol_exact("nvnBootstrapLoader");
    if (!entry || entry < g_code_lo || entry >= g_code_hi || (entry & 3)) {
        WARN("nvn: bootstrap loader symbol is unavailable\n");
        return;
    }
    GuestFn* slot = &g_disp[(entry - g_code_lo) / 4];
    if (!*slot) {
        WARN("nvn: bootstrap loader has no recompiled entry\n");
        return;
    }
    bootstrap_original = *slot;
    *slot = bootstrap_trace; /* before any guest thread runs */
    LOG("nvn: tracing bootstrap function requests (direct calls are only seen with --hook-direct-calls)\n");
}

int nvn_trace_active(void) { return tracing; }

void nvn_trace_report(void) {
    if (!tracing || !bootstrap_original) return;
    pm_lock(&procs_lock);
    u32 used = 0;
    for (u32 i = 0; i < NVN_PROC_SLOTS; i++) {
        if (!procs[i].addr) continue;
        u64 calls = __atomic_load_n(&procs[i].calls, __ATOMIC_RELAXED);
        if (!calls) continue;
        used++;
        LOG("nvn-used: %s %llu\n", procs[i].name, (unsigned long long)calls);
    }
    LOG("nvn: %u resolved addresses, %u called\n", nprocs, used);
    pm_unlock(&procs_lock);
}
