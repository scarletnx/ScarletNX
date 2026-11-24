/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Vulkan instance, device, queues and swapchain. The renderer is built on timeline semaphores (1.2), synchronization2
   and dynamic rendering (1.3): it runs on Vulkan 1.3, or on 1.2 with the extensions 1.3 took in (vk12_extensions). */
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/gpu_queue.h"
#include "nx/gpu/gvk/gvk.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/shading_rate.h"
#include "nx/runtime/main.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/memory/own.h"
#include "nx/gpu/vk/vertex_input.h"
#include "nx/gpu/vk/pipeline_parts.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/texture/astc_bc.h"
#include "common/gpu/vk/extension_bans.h"
#include "common/gpu/vk/device_settings.h"
#include "common/gpu/vk/gpu_pstate.h"
#include "nx/gpu/vk/hdr_swapchain.h"
#include "nx/gpu/vk/upscaler/dlss.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/bits.h"
#include "common/utilities/spirv_emit.h"
#include "common/platform/time.h"

int g_vk_preserve_infnan;
int g_vk_preserve_infnan64;
int g_vk_flush_denorm;
VkShaderStageFlags g_vk_subgroup_stages, g_vk_quad_stages;
u32 g_vk_subgroup_lanes;

VkState g_vk;
/* reentrant on the thread that holds it: landing compute results writes guest memory, which can come back here */
static PMutex g_vklock;
/* The context SNX_LOCK_MARK needs is the one g_pm_tracy_snx makes through pm_tracy_make (tracy.c). thread.c keeps it
   in the PMutex, behind its PmNamed wrapper whose first word is that profiler context; read it as pm_named does. Null
   until the lock is announced (or a client starts), so a mark is a no-op then. */
static SnxTracyLock* vk_lock_context(void) {
    void* named = atomic_load_explicit(&g_vklock.tracy, memory_order_acquire);
    return named ? *(SnxTracyLock**)named : NULL;
}
static _Thread_local int t_vkdepth;
/* from vk_lock_pause to vk_lock_resume: the hold goes on after it, and a hold of the lock in between does not end
   this one's (vk_own_release_held) */
static _Thread_local int t_vkpaused;
static _Atomic u32
    g_vk_owner; /* the thread holding it (plat_tid), 0 when none: a trap's fault on it can't be answered */
/* The lock hands off fairly. A Windows SRW lock (and a pthread mutex) lets the thread that let go take it straight back
   before the waiter it woke runs: a thread whose draws ran back to back kept it, and the threads landing GPU results and
   signalling fences waited milliseconds for each hold of a few microseconds. A thread letting go with
   others waiting in pm_lock sets the baton; until one of them (or any thread) takes the lock, a thread coming to it
   waits for the baton, not the lock, and only those already waiting compete */
static _Atomic u32 g_vk_waiting; /* threads blocked in pm_lock on g_vklock */
static _Atomic u32 g_vk_baton;   /* 1: let go to a waiter, which hasn't taken it yet */
/* For the statistics: where the holder took the lock (the return address of its outermost vk_lock or vk_trylock),
   and for each such place, the contended waits behind a hold from it, the GPU thread's and the others', and their
   nanoseconds. A table that keeps the first places it sees */
static _Atomic uintptr_t g_vk_site;
typedef struct {
    _Atomic uintptr_t at;
    _Atomic u64 n[2], ns[2]; /* [1]: the GPU thread's waits */
} LockSite;
#define LOCK_SITES 48
static LockSite g_lock_site[LOCK_SITES];
static void lock_site_note(uintptr_t at, int gpu, u64 ns) {
    for (u32 i = 0; i < LOCK_SITES; i++) {
        uintptr_t was = atomic_load_explicit(&g_lock_site[i].at, memory_order_relaxed);
        if (!was &&
            !atomic_compare_exchange_strong_explicit(&g_lock_site[i].at, &was, at, memory_order_relaxed,
                                                     memory_order_relaxed) &&
            was != at)
            continue;
        if (was && was != at) continue;
        atomic_fetch_add_explicit(&g_lock_site[i].n[gpu], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_lock_site[i].ns[gpu], ns, memory_order_relaxed);
        return;
    }
}
void vk_lock_log(void) {
    for (u32 i = 0; i < LOCK_SITES; i++) {
        uintptr_t at = atomic_load_explicit(&g_lock_site[i].at, memory_order_relaxed);
        if (!at) break;
        u64 n0 = atomic_exchange_explicit(&g_lock_site[i].n[0], 0, memory_order_relaxed),
            n1 = atomic_exchange_explicit(&g_lock_site[i].n[1], 0, memory_order_relaxed),
            ns0 = atomic_exchange_explicit(&g_lock_site[i].ns[0], 0, memory_order_relaxed),
            ns1 = atomic_exchange_explicit(&g_lock_site[i].ns[1], 0, memory_order_relaxed);
        if (!n0 && !n1) continue;
        /* the place as an offset from vk_lock itself, which the symbols of the runtime turn into a line */
        LOG("vulkan: vk_lock held from vk_lock%+lld: the GPU thread waited %llu times (%.3f ms), others %llu times "
            "(%.3f ms) in the last 600 presents\n",
            (long long)((intptr_t)at - (intptr_t)(uintptr_t)&vk_lock), (unsigned long long)n1, (double)ns1 / 1e6,
            (unsigned long long)n0, (double)ns0 / 1e6);
    }
}
static void vk_took(void) {
    atomic_store_explicit(&g_vk_owner, plat_tid(), memory_order_relaxed);
    if (atomic_load_explicit(&g_vk_baton, memory_order_relaxed)) {
        atomic_store_explicit(&g_vk_baton, 0, memory_order_release);
        plat_wake_word(&g_vk_baton);
    }
}
/* The resolving thread runs under the renderer thread's hold, which that thread keeps while it waits (vk/prep.c):
   its own takes of the lock nest in it. A take there means that its work needs the lock for itself, which it can't
   have: the resolve fails, and the renderer thread does the draw's resolve again */
static _Thread_local int t_vklent;
void vk_lock_lend(int on) {
    t_vklent = on;
    t_vkdepth = on ? 2 : 0; /* never 1: what the outermost hold does at its end stays the renderer thread's */
}
/* The share lock (draw_batches=2, share_wrap.h): while a run is split, the renderer thread and the worker each take
   it around each call into the other files of the renderer, which are written for one thread. Recursive on the
   thread that holds it. While no run is split it does nothing: the renderer thread is alone */
/* on lines of their own: each take reads g_vk_share_on, and g_share_taken moves between the cores */
alignas(64) _Atomic int g_vk_share_on;
static alignas(64) _Atomic int g_share_taken;
static _Thread_local u32 t_share_depth, t_share_held;
static _Thread_local u64 t_share_at; /* the counter when this thread took it */
/* the counts of each side (0 the renderer thread, 1 the worker: vk_render_slot): takes, takes that waited, the ticks
   waited and held, and for each wrapped function (share_wrap.h's number) its takes and ticks waited. Each side writes
   only its own, on cache lines of its own, with a relaxed load and store and no locked add: a take must not move a
   line of counts between the two cores, as it did when both sides added to one count. The log's thread reads them
   and keeps what it read last, so nothing resets them under their writer */
#define SHARE_FNS 256
enum { SH_TAKES, SH_WAITED, SH_WAIT, SH_HOLD, SH_N };
static struct {
    alignas(64) _Atomic u64 c[SH_N];
    _Atomic u64 fn[SHARE_FNS][2];
} g_share_stats[2];
static u64 g_share_read[2][SH_N], g_share_fn_read[SHARE_FNS][2]; /* the log's thread's */
static _Thread_local u32 t_share_fn = SHARE_FNS; /* the function of the next take: none */
static void share_add(_Atomic u64* c, u64 v) {
    atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + v, memory_order_relaxed);
}
void vk_share_active(int on) { atomic_store_explicit(&g_vk_share_on, on, memory_order_seq_cst); }
void vk_share_lock_at(u32 fn) {
    t_share_fn = fn;
    vk_share_lock();
}
void vk_share_fn_times(u32 fn, u64* takes, u64* wait) {
    u64 now[2];
    for (u32 k = 0; k < 2; k++)
        now[k] = atomic_load_explicit(&g_share_stats[0].fn[fn][k], memory_order_relaxed) +
                 atomic_load_explicit(&g_share_stats[1].fn[fn][k], memory_order_relaxed);
    *takes = now[0] - g_share_fn_read[fn][0];
    *wait = now[1] - g_share_fn_read[fn][1];
    g_share_fn_read[fn][0] = now[0];
    g_share_fn_read[fn][1] = now[1];
}
void vk_share_lock(void) {
    if (t_share_depth++) return;
    if (!atomic_load_explicit(&g_vk_share_on, memory_order_acquire)) return;
    const u64 t0 = __builtin_readcyclecounter();
    int waited = 0;
    while (atomic_exchange_explicit(&g_share_taken, 1, memory_order_acquire)) {
        waited = 1;
        while (atomic_load_explicit(&g_share_taken, memory_order_relaxed)) __builtin_ia32_pause();
    }
    t_share_at = __builtin_readcyclecounter();
    typeof(g_share_stats[0])* const st = &g_share_stats[vk_render_slot() != 0];
    if (t_share_fn < SHARE_FNS) {
        share_add(&st->fn[t_share_fn][0], 1);
        share_add(&st->fn[t_share_fn][1], t_share_at - t0);
        t_share_fn = SHARE_FNS;
    }
    share_add(&st->c[SH_TAKES], 1);
    share_add(&st->c[SH_WAITED], (u64)waited);
    share_add(&st->c[SH_WAIT], t_share_at - t0);
    t_share_held = 1;
}
void vk_share_unlock(void) {
    if (--t_share_depth) return;
    if (!t_share_held) return;
    t_share_held = 0;
    share_add(&g_share_stats[vk_render_slot() != 0].c[SH_HOLD], __builtin_readcyclecounter() - t_share_at);
    atomic_store_explicit(&g_share_taken, 0, memory_order_release);
}
void vk_share_times(u32 side, u64* takes, u64* waited, u64* wait, u64* hold) {
    u64* const out[SH_N] = {takes, waited, wait, hold};
    for (u32 k = 0; k < SH_N; k++) {
        const u64 now = atomic_load_explicit(&g_share_stats[side].c[k], memory_order_relaxed);
        *out[k] = now - g_share_read[side][k];
        g_share_read[side][k] = now;
    }
}
void vk_lock(void) {
    if (t_vklent == 1) { /* 2: a batch worker's, which may take it (render.c's batches) */
        gpu_spec_fail("vk_lock");
        LOG_LIMIT(8, "vulkan: draw_prep: a resolve ahead took vk_lock from vk_lock%+lld\n",
                  (long long)((intptr_t)(uintptr_t)__builtin_return_address(0) - (intptr_t)(uintptr_t)&vk_lock));
    }
    if (!t_vkdepth++) {
        uintptr_t site = (uintptr_t)__builtin_return_address(0);
        int got = !atomic_load_explicit(&g_vk_baton, memory_order_acquire) && pm_trylock(&g_vklock);
        if (!got) { /* held: the GPU thread says so for the stall report */
            uintptr_t holder = atomic_load_explicit(&g_vk_site, memory_order_relaxed);
            u64 t_wait = plat_ns();
            /* Only a contended acquisition gets a zone. The text names the holder, gated by SNX_TRACY_EMIT so
               vk_lock_owner() is read only when a client is attached. */
            SNX_ZONE_T(z_wait, "vk lock wait", "holder %u", vk_lock_owner());
            gpu_wait_note("vk_lock, which another thread holds", 0);
            /* the timeout only guards against a wake lost to the store's race: the baton is cleared at each take */
            while (atomic_load_explicit(&g_vk_baton, memory_order_acquire)) plat_wait_word(&g_vk_baton, 1, 1000000);
            if (!pm_trylock(&g_vklock)) {
                atomic_fetch_add_explicit(&g_vk_waiting, 1, memory_order_seq_cst);
                pm_lock(&g_vklock);
                SNX_LOCK_MARK(vk_lock_context()); /* held here, other threads waiting in pm_lock */
                atomic_fetch_sub_explicit(&g_vk_waiting, 1, memory_order_seq_cst);
            }
            gpu_wait_note(NULL, 0);
            lock_site_note(holder, gpu_on_thread(), plat_ns() - t_wait);
        }
        vk_took();
        atomic_store_explicit(&g_vk_site, site, memory_order_relaxed);
        /* With gpu_split, the front end calls the renderer once the queue is empty (gpu_renderer_now). A guest memory
           write in that call went to the queue, and a big one waited for the renderer thread to run the queue first,
           with this lock held. The writes queued before it waited for this lock in gmem_before_write: one game's
           NVN copies locked up so at random. Under the lock this thread is the renderer, and its writes go to memory */
        gq_direct(1);
    }
}
/* lets go of this thread's last hold, then signals the event slots its releases under it reached */
static void vk_let_go(void) {
    gq_direct(0);
    atomic_store_explicit(&g_vk_owner, 0, memory_order_relaxed);
    if (atomic_load_explicit(&g_vk_waiting, memory_order_seq_cst))
        atomic_store_explicit(&g_vk_baton, 1, memory_order_release);
    pm_unlock(&g_vklock);
    vk_release_events();
}
void vk_unlock(void) {
    /* watched bytes that this hold read with no publish (own.c owes them): the targets over them are watched again
       before the lock goes, and the next CPU access publishes them */
    if (t_vkdepth == 1 && own_owed_any()) vk_rt_watch_owed();
    if (t_vkdepth == 1 && vk_own_later_any()) vk_own_release_later();
    /* the results that commands recorded under this hold read by their address: they are recorded now. Not at a
       pause, part-way through the work that records them */
    if (t_vkdepth == 1 && !t_vkpaused && vk_own_held_any()) vk_own_release_held();
    if (!--t_vkdepth) vk_let_go();
}
int vk_trylock(void) {
    if (!t_vkdepth && !pm_trylock(&g_vklock)) return 0;
    if (!t_vkdepth++) {
        vk_took();
        atomic_store_explicit(&g_vk_site, (uintptr_t)__builtin_return_address(0), memory_order_relaxed);
    }
    return 1;
}
u32 vk_lock_owner(void) { return atomic_load_explicit(&g_vk_owner, memory_order_relaxed); }
int vk_lock_held(void) { return t_vkdepth > 0; }
int vk_lock_pause(void) {
    if (t_vkdepth != 1) return 0;
    t_vkpaused = 1;
    vk_unlock();
    return 1;
}
void vk_lock_resume(int paused) {
    if (!paused) return;
    vk_lock();
    t_vkpaused = 0;
}

/* What held the memory at an out-of-memory, at the first ones: each heap as devmem.c counts it, and the descriptor
   pools, which MoltenVK backs with Metal memory that its blocks do not hold (cmd.c). The heap's use less its blocks
   is what the driver and the other allocations of the process took. Any thread can fail, and the counts take
   devmem.c's own lock */
/* set once devmem.c knows the device's memory: vk_memory_text's threads ask it only after they see this, atomic */
static _Atomic int g_devmem_ready;
#define OOM_REPORTS 3
static void oom_report(VkResult r) {
    static _Atomic u32 reports;
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    const VkPhysicalDeviceMemoryProperties* mp;
    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) return;
    if (!atomic_load_explicit(&g_devmem_ready, memory_order_acquire) ||
        atomic_fetch_add_explicit(&reports, 1, memory_order_relaxed) >= OOM_REPORTS)
        return;
    vk_devmem_budgets(b);
    mp = vk_devmem_props();
    for (u32 h = 0; h < mp->memoryHeapCount; h++)
        LOG("vulkan: out of memory, heap %u%s: use %llu MiB, budget %llu MiB, blocks %u (%llu MiB), allocations "
            "%u (%llu MiB), heap size %llu MiB\n",
            h, mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (device local)" : "",
            (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20), b[h].blocks,
            (unsigned long long)(b[h].block_bytes >> 20), b[h].allocations,
            (unsigned long long)(b[h].allocated_bytes >> 20),
            (unsigned long long)(mp->memoryHeaps[h].size >> 20));
    LOG("vulkan: out of memory with %u descriptor pools\n", vk_desc_pools());
    vk_ledger_owners(); /* who holds it */
    vk_own_log();       /* and what of the kept results is in use, what waits freed in the pool */
}

/* Each heap and the descriptor pools in one part of a line, for a low memory warning from a thread that never calls
   Vulkan. devmem.c's functions are safe on any thread (its own lock), and the budget it fetches again now and then
   comes from vkGetPhysicalDeviceMemoryProperties2, which needs no external synchronization */
u64 vk_memory_blocks(void) {
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    const VkPhysicalDeviceMemoryProperties* mp;
    u64 sum = 0;
    if (!atomic_load_explicit(&g_devmem_ready, memory_order_acquire)) return 0;
    vk_devmem_budgets(b);
    mp = vk_devmem_props();
    for (u32 h = 0; h < mp->memoryHeapCount; h++) sum += b[h].block_bytes;
    return sum;
}
void vk_memory_text(char* out, size_t size) {
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    const VkPhysicalDeviceMemoryProperties* mp;
    size_t n = 0;
    if (!size) return;
    out[0] = 0;
    if (!atomic_load_explicit(&g_devmem_ready, memory_order_acquire)) {
        snprintf(out, size, "no GPU heaps yet, %u descriptor pools", vk_desc_pools());
        return;
    }
    vk_devmem_budgets(b);
    mp = vk_devmem_props();
    for (u32 h = 0; h < mp->memoryHeapCount && n < size; h++) {
        int w = snprintf(out + n, size - n, "GPU heap %u%s use %llu of %llu MiB (blocks %llu MiB), ", h,
                         mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (device local)" : "",
                         (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20),
                         (unsigned long long)(b[h].block_bytes >> 20));
        if (w < 0) return;
        n += (size_t)w;
    }
    if (n < size) snprintf(out + n, size - n, "%u descriptor pools", vk_desc_pools());
}

int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    if (r >= 0) return 1;
    WARN("vulkan: %s failed: %s (%d) at %s:%d\n", what, snx_vk_result_str(r), (int)r, file, line);
    if (r == VK_ERROR_DEVICE_LOST) vk_checkpoint_dump("device lost");
    oom_report(r);
    return 0;
}

u32 g_vk_validation_messages;
/* a message names an image with binds in a batch that hasn't gone to the queue yet (memory.c): among its objects, or
   in its text alone, as the layers' message about a barrier names no image object */
static int validation_unissued(const VkDebugUtilsMessengerCallbackDataEXT* d) {
    for (u32 i = 0; i < d->objectCount; i++)
        if (d->pObjects[i].objectType == VK_OBJECT_TYPE_IMAGE &&
            vk_sparse_unissued((VkImage)(uintptr_t)d->pObjects[i].objectHandle))
            return 1;
    for (const char* at = strstr(d->pMessage, "VkImage 0x"); at; at = strstr(at + 10, "VkImage 0x"))
        if (vk_sparse_unissued((VkImage)(uintptr_t)strtoull(at + 10, NULL, 16))) return 1;
    return 0;
}
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT* d, void* user) {
    (void)type;
    (void)user;
    /* an image whose binds wait in a batch for the next submission (memory.c): the layers check its memory as each
       command is recorded, and the batch goes to the queue after that, ahead of the submission that runs the command */
    if (d->pMessage && strstr(d->pMessage, "vkQueueBindSparse") && validation_unissued(d)) {
        LOG_LIMIT(4, "vulkan: the validation layers find an image unbound as a command for it is recorded, its binds "
                     "going to the queue with the next submission (not counted)\n");
        return VK_FALSE;
    }
    /* The layers up to 1.4.363 add a stage's input and output clip distances against
       maxCombinedClipAndCullDistances, which limits each: a tessellation or geometry stage that passes eight planes on
       read 16 against 8. Their 99fd2cb576 ("layers: Fix clip cull") counts each on its own, with the CTS case
       dEQP-VK.clipping.user_defined.clip_cull_distance.vert_geom as its test. Only those stages have both, and each of
       ours declares eight at most, so the check stays where it applies */
    if (d->pMessageIdName && !strcmp(d->pMessageIdName, "VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710") &&
        d->pMessage && (strstr(d->pMessage, "TESSELLATION_") || strstr(d->pMessage, "GEOMETRY_BIT"))) {
        LOG_LIMIT(4, "vulkan: the validation layers add a tessellation or geometry stage's input and output clip "
                     "distances, which their later versions count on their own (not counted)\n");
        return VK_FALSE;
    }
    g_vk_validation_messages++;
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        WARN("vulkan validation: %s\n", d->pMessage);
    else
        LOG("vulkan validation: %s\n", d->pMessage);
    return VK_FALSE;
}

/* VkPhysicalDevicePortabilitySubsetFeaturesKHR, which vulkan_beta.h holds back */
#define PORTABILITY_FEATURES_STYPE ((VkStructureType)1000163000)
typedef struct {
    VkStructureType sType;
    void* pNext;
    VkBool32 constantAlphaColorBlendFactors, events, imageViewFormatReinterpretation, imageViewFormatSwizzle,
        imageView2DOn3DImage, multisampleArrayImage, mutableComparisonSamplers, pointPolygons, samplerMipLodBias,
        separateStencilMaskRef, shaderSampleRateInterpolationFunctions, tessellationIsolines, tessellationPointMode,
        triangleFans, vertexAttributeAccessBeyondStride;
} PortabilityFeatures;
/* NVIDIA DLSS (gpu/vk/upscaler/dlss.h), on while Streamline runs: it starts before the instance, when the settings
   name the folder of NVIDIA's runtime and the render upscaler may be DLSS, says what the instance and the device need,
   and takes the device and the swapchain's calls once they are made. Where any of that can't be, it stops and the
   upscalers go on without DLSS */
static int g_dlss;
static void dlss_log(int level, const char* text) {
    if (level)
        WARN("vulkan: %s\n", text);
    else
        LOG("vulkan: %s\n", text);
}
static void dlss_off(const char* why) {
    WARN("vulkan: no DLSS: %s\n", why);
    g_dlss = 0;
}
static void dlss_start(void) {
    const char* why;
    int may = g_settings.render_upscaler == 0 || g_settings.render_upscaler == SNX_UPSCALER_DLSS + 1;
    if (g_opt_headless || !g_settings.dlss_runtime[0] || !may) return;
    /* the setting's words are NVIDIA's own choice, then the presets J, K, L and M (10 to 13) */
    g_dlss = snx_dlss_start(g_settings.dlss_runtime, g_settings.dlss_preset ? 9 + g_settings.dlss_preset : 0, dlss_log,
                            &why);
    if (!g_dlss) dlss_off(why);
}
/* the swapchain's calls through Streamline's, which does its work for each frame in the present */
static void dlss_hook(void) {
    PFN_vkVoidFunction f;
#define DLSS_HOOK(name)                                                                                                \
    if ((f = snx_dlss_proxy(#name)) != NULL)                                                                           \
        name = (PFN_##name)f;                                                                                          \
    else                                                                                                               \
        WARN("vulkan: Streamline has no " #name ": the driver's is called\n");
    DLSS_HOOK(vkCreateSwapchainKHR)
    DLSS_HOOK(vkDestroySwapchainKHR)
    DLSS_HOOK(vkGetSwapchainImagesKHR)
    DLSS_HOOK(vkAcquireNextImageKHR)
    DLSS_HOOK(vkQueuePresentKHR)
    DLSS_HOOK(vkDeviceWaitIdle)
#undef DLSS_HOOK
}

/* MoltenVK (macOS, iOS) is a portability driver: the loader lists it only for an instance that asks for those */
static int create_instance(void) {
    u32 nbe = 0;
    const char* const* be = g_opt_headless ? NULL : backend_vk_extensions(&nbe);
    /* the window's, and the surface's present timing capabilities (VK_EXT_present_timing) are asked through
       VK_KHR_get_surface_capabilities2, then DLSS's */
    const char* ext[26];
    u32 next = 0;
    for (u32 i = 0; i < nbe && next < 16; i++) ext[next++] = be[i];
    if (nbe > 16) WARN("vulkan: the window backend wants %u instance extensions, only 16 are taken\n", nbe);
    if (!g_opt_headless && snx_vk_has_instance_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        ext[next++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        g_vk.has_surface_caps2 = 1;
    }
    /* an HDR10 swapchain's color space is one of this instance extension's */
    if (!g_opt_headless && next < 17 && snx_vk_has_instance_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        ext[next++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
        g_vk.has_swapchain_colorspace = 1;
    }
    dlss_start();
    if (g_dlss) {
        const char* const* dn;
        u32 nd = snx_dlss_instance_extensions(&dn), before = next;
        for (u32 i = 0; i < nd && g_dlss; i++) {
            int there = 0;
            for (u32 j = 0; j < next && !there; j++) there = !strcmp(ext[j], dn[i]);
            if (there) continue;
            if (next < sizeof ext / sizeof *ext && snx_vk_has_instance_extension(dn[i])) {
                ext[next++] = dn[i];
                continue;
            }
            snx_dlss_stop();
            dlss_off(next < sizeof ext / sizeof *ext ? "the Vulkan loader lacks an instance extension DLSS needs"
                                                     : "too many instance extensions");
            next = before;
        }
    }
    SnxVkInstanceInfo info = {.app_name = "ScarletNX",
                              .extensions = ext,
                              .extension_count = next,
                              .api_version = g_vk.api_cap == VK_API_VERSION_1_2 ? g_vk.api_cap : 0, /* 1.1's: auto's */
                              .validation = g_settings.vulkan_validation_layer != 0,
                              .gpu_assisted = g_settings.vulkan_validation_layer == 2,
                              .portability = true,
                              .debug = debug_cb};
    SnxVkInstance made;
    if (!VK_OK(snx_vk_instance_create(&info, &made))) return 0;
    if (info.validation && !made.validation)
        WARN("vulkan: validation was asked for (vk_validation), but "
             "VK_LAYER_KHRONOS_validation is not installed\n");
    g_vk.instance = made.instance;
    g_vk.debug = made.messenger;
    return 1;
}

/* the codecs Vulkan Video decodes for the runtime (the vulkan_video setting), each with its extension */
static const struct {
    VkVideoCodecOperationFlagsKHR op;
    const char *name, *ext;
} video_codecs[] = {{VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR, "H.264", VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME},
                    {VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR, "AV1", VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME}};
/* the queue family that decodes with Vulkan Video, other than the graphics and compute ones: the one that decodes the
   most of want's codecs (VK_VIDEO_CODEC_OPERATION_DECODE_*), the first of those, *codecs the ones it does; ~0u when
   none decodes any */
static u32 video_decode_family(VkVideoCodecOperationFlagsKHR want, VkVideoCodecOperationFlagsKHR* codecs) {
    u32 n = 0, found = ~0u;
    vkGetPhysicalDeviceQueueFamilyProperties2(g_vk.phys, &n, NULL);
    VkQueueFamilyProperties2* q = calloc(n ? n : 1, sizeof *q);
    VkQueueFamilyVideoPropertiesKHR* v = calloc(n ? n : 1, sizeof *v);
    if (!q || !v) fatal("vulkan: out of memory for the queue families");
    for (u32 i = 0; i < n; i++) {
        v[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        q[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        q[i].pNext = &v[i];
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(g_vk.phys, &n, q);
    *codecs = 0;
    for (u32 i = 0; i < n; i++) {
        VkVideoCodecOperationFlagsKHR has = v[i].videoCodecOperations & want;
        if ((q[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && i != g_vk.gfx_family &&
            i != g_vk.compute_family && snx_popcount64(has) > snx_popcount64(*codecs)) {
            found = i;
            *codecs = has;
        }
    }
    free(q);
    free(v);
    return found;
}

/* What the renderer takes from Vulkan 1.3, as a 1.2 device has it: these extensions, their features (create_device)
   and their commands under the core names (vk12_commands), and SPIR-V 1.5. A read-only depth target's store op NONE
   comes from VK_KHR_load_store_op_none or the EXT it was promoted from (vk12_store_none) */
static const char* const vk12_extensions[] = {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                              VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                                              VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
                                              VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
                                              VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
                                              VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME,
                                              VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME,
                                              VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME};
#define VK12_EXTENSIONS (sizeof vk12_extensions / sizeof *vk12_extensions)
static const char* vk12_store_none(const SnxVkExtensions* e) {
    return snx_vk_extensions_has(e, VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME) ? VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME
           : snx_vk_extensions_has(e, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME)
               ? VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME
               : NULL;
}
/* how many of those e lacks, their names in out (size bytes, may be 0), comma separated */
u32 vk_1_2_missing(const SnxVkExtensions* e, char* out, size_t size) {
    u32 n = 0;
    size_t at = 0;
    if (size) out[0] = 0;
    for (u32 i = 0; i <= VK12_EXTENSIONS; i++) {
        const char* name = i < VK12_EXTENSIONS ? vk12_extensions[i] : VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME;
        if (i < VK12_EXTENSIONS ? snx_vk_extensions_has(e, name) : vk12_store_none(e) != NULL) continue;
        if (at < size) at += (size_t)snprintf(out + at, size - at, "%s%s", n ? ", " : "", name);
        n++;
    }
    return n;
}
/* what the 1.2 way takes on a Vulkan 1.1 device beside vk12_extensions, where 1.2 made it core: a timeline
   semaphore and the driver's identity. How many of those and of vk12_extensions e lacks, their names in out as
   vk_1_2_missing writes them */
u32 vk_1_1_missing(const SnxVkExtensions* e, char* out, size_t size) {
    static const char* const core12[] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                                         VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME};
    char rest[512];
    u32 n = 0, more = vk_1_2_missing(e, rest, sizeof rest);
    size_t at = 0;
    if (size) out[0] = 0;
    for (u32 i = 0; i < sizeof core12 / sizeof *core12; i++) {
        if (snx_vk_extensions_has(e, core12[i])) continue;
        if (at < size) at += (size_t)snprintf(out + at, size - at, "%s%s", n ? ", " : "", core12[i]);
        n++;
    }
    if (more && at < size) snprintf(out + at, size - at, "%s%s", n ? ", " : "", rest);
    return n + more;
}
int vk_api_setting(const char* text, u32* cap, char* why, size_t n) {
    if (!strcmp(text, "auto") || !strcmp(text, "1.2") || !strcmp(text, "1.1")) {
        *cap = text[0] == 'a' ? 0 : text[2] == '2' ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
        return 1;
    }
    snprintf(why, n, "vk_api=%s is not auto, 1.2 or 1.1", text);
    return 0;
}

/* the version the renderer runs a device of api at: the device's, up to 1.3 (or vk_api's 1.2). vk_api=1.1 holds no
   device back: it names what the 1.2 way lacks on a 1.1 one, which no device then runs (start_failure_devices) */
static u32 vk_api_for(u32 api) {
    u32 most = g_vk.api_cap == VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_3;
    return api < most ? api : most;
}

/* Vulkan 1.3, or 1.2 with vk12_extensions, and a graphics queue. Only windowed runs need WSI and presentation support.
   *gfx_queues: how many queues the graphics family has */
static int device_score(VkPhysicalDevice pd, const VkPhysicalDeviceProperties* p, u32* gfx, u32* compute,
                        u32* gfx_queues) {
    SnxVkExtensions e;
    u32 api = vk_api_for(p->apiVersion);
    int listed = snx_vk_device_extensions(pd, &e);
    int swapchain = listed && snx_vk_extensions_has(&e, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    int lacks = api < VK_API_VERSION_1_3 && (!listed || vk_1_2_missing(&e, NULL, 0));
    snx_vk_extensions_free(&e);
    if ((!g_opt_headless && !swapchain) || api < VK_API_VERSION_1_2 || lacks) return -1;
    u32 nq = 0;
    VkQueueFamilyProperties* q = snx_vk_queue_families(pd, &nq);
    /* ASTC uploads use compute */
    *gfx = snx_vk_queue_family(pd, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                               g_opt_headless ? VK_NULL_HANDLE : g_vk.surface, 0);
    *compute = ~0u;
    /* a compute family without graphics runs alongside the graphics queue */
    for (u32 i = 0; i < nq; i++)
        if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && *compute == ~0u)
            *compute = i;
    if (*compute == ~0u) *compute = *gfx;
    *gfx_queues = *gfx < nq ? q[*gfx].queueCount : 0;
    free(q);
    if (*gfx == ~0u) return -1;
    /* of two devices of a kind, the one with 1.3 */
    return (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 6
            : p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 4
                                                                      : 2) +
           (api >= VK_API_VERSION_1_3);
}
/* device_score as common/gpu/vk/device.c rates a device (the request's score) */
static int device_rating(VkPhysicalDevice pd, const VkPhysicalDeviceProperties* p, void* user) {
    u32 gf, cf, gq;
    (void)user;
    return device_score(pd, p, &gf, &cf, &gq);
}

/* g_vk.start_failure for no device found: each device with its Vulkan version, which a driver under 1.3 holds back
   (a GPU driver package may have a newer one: gpu_driver), and what a 1.2 one lacks of vk12_extensions; with none
   listed by a GPU driver package, the package and the GPU it does not drive */
static void start_failure_devices(void) {
    VkPhysicalDevice devices[8];
    u32 n = 8, elevens = 0;
    size_t at = 0;
    if (vkEnumeratePhysicalDevices(g_vk.instance, &n, devices) < 0) n = 0;
    if (!n && backend_vk_no_device(g_vk.start_failure, sizeof g_vk.start_failure)) {
        WARN("vulkan: %s\n", g_vk.start_failure);
        return;
    }
    for (u32 i = 0; i < n && at < sizeof g_vk.start_failure; ++i) {
        VkPhysicalDeviceProperties p;
        char lacks[512] = "";
        vkGetPhysicalDeviceProperties(devices[i], &p);
        u32 api = vk_api_for(p.apiVersion);
        /* vk_api=1.1 asks what the 1.2 way lacks on a 1.1 device too: the extensions 1.2 made core with the others */
        int eleven = g_vk.api_cap == VK_API_VERSION_1_1 && api >= VK_API_VERSION_1_1 && api < VK_API_VERSION_1_2;
        if ((api >= VK_API_VERSION_1_2 && api < VK_API_VERSION_1_3) || eleven) {
            SnxVkExtensions e;
            char names[400];
            if (!snx_vk_device_extensions(devices[i], &e))
                snprintf(lacks, sizeof lacks, " (its extensions could not be listed)");
            else if (eleven ? vk_1_1_missing(&e, names, sizeof names) : vk_1_2_missing(&e, names, sizeof names))
                snprintf(lacks, sizeof lacks, " without %s", names);
            else if (eleven)
                snprintf(lacks, sizeof lacks, " with every extension the 1.2 way takes");
            snx_vk_extensions_free(&e);
            elevens += eleven;
        }
        WARN("vulkan: %s has Vulkan %u.%u.%u%s, driver %08x\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), lacks, p.driverVersion);
        at += (size_t)snprintf(g_vk.start_failure + at, sizeof g_vk.start_failure - at, "%s%s has Vulkan %u.%u.%u%s",
                               i ? "; " : "", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), lacks);
    }
#ifdef __ANDROID__
    const char* hint = n ? ". A GPU driver package may have it (Settings > App > GPU Drivers)" : "";
#else
    const char* hint = "";
#endif
    if (at < sizeof g_vk.start_failure)
        at += (size_t)snprintf(
            g_vk.start_failure + at, sizeof g_vk.start_failure - at,
            "%sScarletNX needs Vulkan 1.3, or 1.2 with the extensions 1.3 took in, and a %s queue%s",
            n ? ", and " : "No Vulkan device: ", g_opt_headless ? "graphics" : "graphics and presentation", hint);
    if (elevens && at < sizeof g_vk.start_failure)
        snprintf(g_vk.start_failure + at, sizeof g_vk.start_failure - at,
                 ". vk_api=1.1 named what the 1.2 way lacks on a Vulkan 1.1 device, which the renderer has no way for "
                 "yet: its shaders are SPIR-V 1.5, its features read through Vulkan 1.1's and 1.2's structs");
}

/* the shapes of f16 A and B into an f32 accumulator in subgroup scope in compute that the device multiplies, as
   g_spirv_translate.hmma_coop_shapes asks (VK_KHR_cooperative_matrix: RADV from GFX11): a bit for each of an SM86
   HMMA's exact 16x8x16 and 16x8x8, a 16x16x16 and Metal's simdgroup_matrix 8x8x8. 0 when it has no product in
   compute */
static u32 coop_f16_f32_shapes(VkPhysicalDevice phys) {
    VkPhysicalDeviceCooperativeMatrixPropertiesKHR cp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cp};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    if (!(cp.cooperativeMatrixSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        !vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        return 0;
    u32 n = 0;
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(phys, &n, NULL);
    VkCooperativeMatrixPropertiesKHR props[64];
    if (n > 64) n = 64;
    for (u32 i = 0; i < n; i++)
        props[i] = (VkCooperativeMatrixPropertiesKHR){.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR};
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(phys, &n, props);
    u32 shapes = 0;
    for (u32 i = 0; i < n; i++) {
        u32 m = props[i].MSize, cols = props[i].NSize, k = props[i].KSize;
        if (props[i].AType != VK_COMPONENT_TYPE_FLOAT16_KHR || props[i].BType != VK_COMPONENT_TYPE_FLOAT16_KHR ||
            props[i].CType != VK_COMPONENT_TYPE_FLOAT32_KHR ||
            props[i].ResultType != VK_COMPONENT_TYPE_FLOAT32_KHR || props[i].saturatingAccumulation ||
            props[i].scope != VK_SCOPE_SUBGROUP_KHR)
            continue;
        if (m == 16 && cols == 16 && k == 16) shapes |= SH_COOP_16x16x16;
        else if (m == 16 && cols == 8 && k == 16) shapes |= SH_COOP_16x8x16;
        else if (m == 16 && cols == 8 && k == 8) shapes |= SH_COOP_16x8x8;
        else if (m == 8 && cols == 8 && k == 8) shapes |= SH_COOP_8x8x8;
    }
    return shapes;
}

/* The subgroup sizes a pipeline stage can require, where has says the device has them: Vulkan 1.3's, or a 1.2 device's
   from VK_EXT_subgroup_size_control, one structure for both. All zero without; next is read with them */
static VkPhysicalDeviceSubgroupSizeControlProperties subgroup_sizes(int has, void* next) {
    VkPhysicalDeviceSubgroupSizeControlProperties sp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES, .pNext = next};
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sp};
    if (has) vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    sp.pNext = NULL;
    return sp;
}
/* An Ounce's mesh draws (render.c): VK_EXT_mesh_shader's mesh stage, and its task stage when the device has one. The
   programs' lanes are a warp's (sm86.h), so both stages need a required subgroup size of 32 in full subgroups
   (subgroupSizeControl, computeFullSubgroups, requiredSubgroupSizeStages: sizes says whether the device has them,
   Vulkan 1.3's or VK_EXT_subgroup_size_control's). Without them, or with the extension off (a ban on the device's
   driver, or vk_ext_mesh_shader=off: its own line says which), the draws are skipped, and the log says why once */
static void mesh_choose(int has_ext, const VkPhysicalDeviceMeshShaderFeaturesEXT* f,
                        const VkPhysicalDeviceVulkan13Features* f13, int sizes) {
    VkPhysicalDeviceMeshShaderPropertiesEXT mp = {.sType =
                                                      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    VkPhysicalDeviceSubgroupSizeControlProperties sp = subgroup_sizes(sizes, has_ext ? &mp : NULL);
    const VkShaderStageFlags both = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT;
    const char* why = !has_ext         ? "VK_EXT_mesh_shader is off (the line on VK_EXT_mesh_shader says why)"
                      : !f->meshShader ? "this GPU has no meshShader"
                      : !f13->subgroupSizeControl || !f13->computeFullSubgroups
                          ? "this GPU has no subgroup size control"
                      : sp.minSubgroupSize > 32 || sp.maxSubgroupSize < 32 ? "this GPU has no subgroups of 32"
                      : (sp.requiredSubgroupSizeStages & VK_SHADER_STAGE_MESH_BIT_EXT) == 0
                          ? "this GPU has no required subgroup size in the mesh stage"
                          : NULL;
    g_vk.has_mesh = why == NULL;
    g_vk.has_task = g_vk.has_mesh && f->taskShader && (sp.requiredSubgroupSizeStages & both) == both;
    g_vk.mesh_stages = g_vk.has_mesh ? VK_SHADER_STAGE_MESH_BIT_EXT | (g_vk.has_task ? both : 0) : 0;
    u32 groups = g_vk.has_task ? mp.maxTaskWorkGroupCount[0] : mp.maxMeshWorkGroupCount[0];
    g_vk.mesh_groups_max = g_vk.has_mesh ? groups : 0;
    g_spirv_translate.mesh_tasks_max =
        g_vk.has_task ? (mp.maxMeshWorkGroupCount[0] < mp.maxMeshWorkGroupTotalCount ? mp.maxMeshWorkGroupCount[0]
                                                                                     : mp.maxMeshWorkGroupTotalCount)
                      : 0;
    g_vk.mesh_output_bytes_max = mp.maxMeshOutputMemorySize;
    g_vk.mesh_vertex_granule = mp.meshOutputPerVertexGranularity;
    g_vk.mesh_primitive_granule = mp.meshOutputPerPrimitiveGranularity;
    /* The path a draw with a task program takes (vk_mesh_path). The native task and mesh stages in one submission is
       the shape that faulted NVIDIA's driver and that hangs RADV on RDNA4 (docs/layers/gpu/driver-quirks.md): Auto
       takes the compute-task path, which runs the task program as a compute dispatch (sm86.h), and Native keeps the
       two stages together for a device whose path is measured to work. The task program's lanes are a warp's, so its
       compute pipeline needs a subgroup of 32, whole (has_cs_warp32) */
    g_vk.mesh_task_compute = g_vk.has_task && g_vk.has_cs_warp32 && g_settings.mesh_task_path != 1;
    if (why)
        LOG("vulkan: mesh draws are skipped: %s\n", why);
    else if (g_vk.mesh_task_compute)
        LOG("vulkan: mesh draws use VK_EXT_mesh_shader, with a task program as a compute dispatch (vk_mesh_path=%s)\n",
            g_settings.mesh_task_path == 2 ? "compute" : "auto");
    else if (g_vk.has_task && g_settings.mesh_task_path != 1)
        LOG("vulkan: mesh draws keep their task and mesh stages together: a compute dispatch of the task program needs "
            "subgroups of 32 (vk_mesh_path=%d)\n",
            g_settings.mesh_task_path);
    else
        LOG("vulkan: mesh draws use VK_EXT_mesh_shader%s\n",
            g_vk.has_task ? " with task shaders" : ", without task shaders");
}
/* How the ASTC textures the GPU decodes are held, where it samples BCn but not ASTC (astc_mode, gpu/texture/astc_bc.h).
   Auto follows the room of the heap images go to as the game runs
   (vk_astc_auto_trimmed, vk_astc_auto_present); its line says the budget and the use it starts from. vk_mem_use's
   budget is the one the caches keep within: the driver's, or vk_budget_mb where that is lower. Without
   VK_EXT_memory_budget both numbers are devmem.c's own, the blocks it took for the use and 80% of the heap's size for
   the budget, which see nothing other programs take: Auto follows those, and its line says so */
static const char g_astc_exact[] = "decoded to RGBA8, exact, in four times BC7's memory",
                  g_astc_bc7[] = "decoded to RGBA8 at once and encoded again as BC7 between frames, a quarter of "
                                 "RGBA8's memory";
static void astc_choose(void) {
    u64 usage = 0, budget = 0;
    if (g_vk.feat.textureCompressionASTC_LDR) return; /* sampled as the game's blocks */
    if (g_settings.astc_mode == 1) {
        LOG("vulkan: astc_mode=1 (Exact): ASTC textures are %s\n", g_astc_exact);
        return;
    }
    if (!g_vk.feat.textureCompressionBC) return; /* RGBA8, which vk_blocks_say's line says */
    /* An Ounce program's Auto was BC7 at load once, as feat/ounce held them. In one Ounce game's race on a GPU of 16
       GB, BC7 at load made 54 slow frames where Exact made 7 (its encoder's pace waited for the GPU in the draws that
       loaded them), and its caches evicted textures they took back within 60 presents about five times as often */
    g_vk.astc_auto = (SnxAstcAuto){0, 0, 0};
    g_vk.astc_at_load = g_settings.astc_mode == 3;
    if (g_settings.astc_mode == 0) {
        vk_mem_use(&usage, &budget);
        LOG("vulkan: astc_mode=0 (Auto): ASTC textures that load are %s while heap %d has room, and BC7 from the first "
            "trim of its caches, past %llu of the %llu MB budgeted, until the room under that is more than any "
            "trim has left: %llu MB in use as the device starts%s\n",
            g_astc_exact, vk_mem_heap(), (unsigned long long)(vk_mem_trim_start(budget) >> 20),
            (unsigned long long)(budget >> 20), (unsigned long long)(usage >> 20),
            g_vk.has_budget ? ""
                            : "; the driver has no VK_EXT_memory_budget, so these are the renderer's own numbers, "
                              "its memory blocks for the use and 80% of the heap's size for the budget, blind to what "
                              "other programs take");
    } else if (g_settings.astc_mode == 3)
        LOG("vulkan: astc_mode=3 (BC7 at load): ASTC textures are encoded as BC7 as they load, a quarter of RGBA8's "
            "memory\n");
    else
        LOG("vulkan: astc_mode=2 (BC7): ASTC textures are %s\n", g_astc_bc7);
}
/* Auto follows the memory here: astc_mode 0, ASTC decoded where BC7 is sampled */
static int astc_auto_follows(void) {
    return g_settings.astc_mode == 0 && !g_vk.feat.textureCompressionASTC_LDR &&
           g_vk.feat.textureCompressionBC;
}
/* the use and the budget of a switch's line: "U of B MB budgeted in use, R MB free", or past it */
static void astc_room(char* out, size_t size, u64 usage, u64 budget) {
    if (usage <= budget)
        snprintf(out, size, "%llu of %llu MB budgeted in use, %llu MB free", (unsigned long long)(usage >> 20),
                 (unsigned long long)(budget >> 20), (unsigned long long)((budget - usage) >> 20));
    else
        snprintf(out, size, "%llu MB in use, %llu MB past the %llu budgeted", (unsigned long long)(usage >> 20),
                 (unsigned long long)((usage - budget) >> 20), (unsigned long long)(budget >> 20));
}
void vk_astc_auto_trimmed(u64 usage, u64 left, u64 budget, int force) {
    char room[160];
    const u64 mark = vk_mem_trim_start(budget);
    if (!astc_auto_follows() || !snx_astc_auto_trimmed(&g_vk.astc_auto, left, mark)) return;
    u32 held = 0;
    u64 held_bytes = 0;
    vk_tex_later_exact(&held, &held_bytes);
    snx_astc_auto_converted(&g_vk.astc_auto, held_bytes);
    astc_room(room, sizeof room, usage, budget);
    LOG("vulkan: astc_mode=0 (Auto): ASTC textures are BC7 from now on, %s, and the %u held as RGBA8 (%llu MB) whose "
        "memory is unchanged too, read again at their next use: heap %d has %s, %s the %llu MB where its caches' trim "
        "starts%s\n",
        g_astc_bc7, held, (unsigned long long)(held_bytes >> 20), vk_mem_heap(), room, usage > mark ? "past" : "under",
        (unsigned long long)(mark >> 20), force ? ", and an image found no memory" : "");
}
void vk_astc_auto_present(void) {
    char room[160];
    u64 usage, budget;
    if (!astc_auto_follows() || !g_vk.astc_auto.bc7) return;
    vk_pipeline_bc7_cache();
    vk_mem_use(&usage, &budget);
    const u64 mark = vk_mem_trim_start(budget);
    /* the band's rule first: the walk over the textures runs only where it would turn Exact */
    if (usage >= mark || mark - usage <= g_vk.astc_auto.band) return;
    const u64 held = vk_tex_astc_bc7_bytes();
    if (!snx_astc_auto_room(&g_vk.astc_auto, usage, mark, held)) return;
    astc_room(room, sizeof room, usage, budget);
    LOG("vulkan: astc_mode=0 (Auto): ASTC textures that load from now on are Exact again: heap %d has %s, %llu MB "
        "under the %llu MB where its caches' trim starts, more than the %llu MB the trims of this run left under it "
        "at most and the %llu MB its ASTC textures held as BC7 would take more as RGBA8\n",
        vk_mem_heap(), room, (unsigned long long)((mark - usage) >> 20), (unsigned long long)(mark >> 20),
        (unsigned long long)(g_vk.astc_auto.band >> 20), (unsigned long long)((held * 3) >> 20));
}
void vk_astc_auto_say(void) {
    if (!astc_auto_follows()) return;
    LOG("vulkan: astc_mode=0 (Auto): ASTC textures that load now are %s, after %llu switches; the trims left at most "
        "%llu MB under where they start\n",
        g_vk.astc_auto.bc7 ? "BC7" : "Exact", (unsigned long long)g_vk.astc_auto.switches,
        (unsigned long long)(g_vk.astc_auto.band >> 20));
}

/* a line of the extension bans (extension_bans.h) for the log */
static void ban_say(const char* line, int warn) {
    if (warn)
        WARN("%s\n", line);
    else
        LOG("%s\n", line);
}

/* the extensions the renderer can't run without on this device, of e, the list its driver gives, and why: the swapchain
   of a window, a portability driver's subset, which Vulkan has turned on wherever a driver lists it, and on a 1.2
   device what 1.3 took in (vk12_extensions, and one of the store op NONE's two). Into need, VK_NEEDS at most: how many */
#define VK_NEEDS (VK12_EXTENSIONS + 3)
typedef char vk_needs_fit[VK_NEEDS <= SNX_VK_NEEDS ? 1 : -1]; /* common/gpu/vk/device.c takes SNX_VK_NEEDS at most */
static u32 extension_needs(const SnxVkExtensions* e, int core13, VkExtensionNeed* need) {
    u32 needs = 0;
    if (!g_opt_headless)
        need[needs++] = (VkExtensionNeed){VK_KHR_SWAPCHAIN_EXTENSION_NAME, "the window is drawn with it"};
    need[needs++] = (VkExtensionNeed){"VK_KHR_portability_subset",
                                      "Vulkan has a portability driver's subset on wherever the driver lists it"};
    if (!core13) {
        static const char why[] = "the device has Vulkan 1.2, where ScarletNX needs it for what Vulkan 1.3 took in";
        for (u32 i = 0; i < VK12_EXTENSIONS; i++) need[needs++] = (VkExtensionNeed){vk12_extensions[i], why};
        /* the store op NONE: one of the two, the other where only one is listed */
        int khr = snx_vk_extensions_has(e, VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME),
            ext = snx_vk_extensions_has(e, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
        if (khr != ext) need[needs++] = (VkExtensionNeed){khr ? VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME
                                                              : VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME,
                                                          why};
    }
    return needs;
}

/* the stages whose programs the translator gives subgroup operations, and what VOTE and SHFL do in the others: after
   has_cs_warp32, which may add compute */
static void log_subgroups(u32 size, VkSubgroupFeatureFlags operations, VkShaderStageFlags stages) {
    static const struct {
        VkShaderStageFlags bit;
        const char* name;
    } names[] = {{VK_SHADER_STAGE_VERTEX_BIT, "vertex"},
                 {VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "control"},
                 {VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "evaluation"},
                 {VK_SHADER_STAGE_GEOMETRY_BIT, "geometry"},
                 {VK_SHADER_STAGE_FRAGMENT_BIT, "fragment"},
                 {VK_SHADER_STAGE_COMPUTE_BIT, "compute"}};
    char with[80] = "", without[80] = "";
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char* list = (g_vk_subgroup_stages & names[i].bit) ? with : without;
        size_t n = strlen(list);
        snprintf(list + n, 80 - n, "%s%s", n ? ", " : "", names[i].name);
    }
    LOG("vulkan: subgroups of %u lanes, operations %#x, stages %#x: VOTE and SHFL use them in %s programs%s\n", size,
        (unsigned)operations, (unsigned)stages, with[0] ? with : "no",
        g_vk_subgroup_stages || !stages ? (size < 32 && (g_vk_subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT)
                                               ? ", in subgroups of 32 that their pipelines ask for"
                                               : "")
                                        : ", as they need 32 lanes or more, ballot, shuffle and arithmetic");
    if (without[0])
        LOG("vulkan: %s programs have no subgroup: a vertex, evaluation, geometry or fragment one among them that "
            "votes or shuffles runs each invocation as one lane alone (a fragment's reads within its quad through fine "
            "derivatives), a compute one too where its adds, minima and maxima come to the same, and a control one or "
            "any other compute one is not translated\n",
            without);
}

/* the commands of Vulkan 1.3 the renderer calls, as a 1.2 device has them: by the names of its extensions, the same
   calls (the core names stay unloaded there). 0, with *lacks the first one the driver has not */
static int vk12_commands(const char** lacks) {
#define VK12_COMMAND(name, by)                                                                                         \
    name = name##by;                                                                                                   \
    if (!name && !*lacks) *lacks = #name #by
    *lacks = NULL;
    VK12_COMMAND(vkCmdBeginRendering, KHR);
    VK12_COMMAND(vkCmdEndRendering, KHR);
    VK12_COMMAND(vkCmdPipelineBarrier2, KHR);
    VK12_COMMAND(vkCmdWriteTimestamp2, KHR);
    VK12_COMMAND(vkQueueSubmit2, KHR);
    VK12_COMMAND(vkCmdBindVertexBuffers2, EXT);
    VK12_COMMAND(vkCmdSetCullMode, EXT);
    VK12_COMMAND(vkCmdSetFrontFace, EXT);
    VK12_COMMAND(vkCmdSetPrimitiveTopology, EXT);
    VK12_COMMAND(vkCmdSetViewportWithCount, EXT);
    VK12_COMMAND(vkCmdSetScissorWithCount, EXT);
    VK12_COMMAND(vkCmdSetDepthTestEnable, EXT);
    VK12_COMMAND(vkCmdSetDepthWriteEnable, EXT);
    VK12_COMMAND(vkCmdSetDepthCompareOp, EXT);
    VK12_COMMAND(vkCmdSetDepthBoundsTestEnable, EXT);
    VK12_COMMAND(vkCmdSetStencilTestEnable, EXT);
    VK12_COMMAND(vkCmdSetStencilOp, EXT);
    VK12_COMMAND(vkCmdSetDepthBiasEnable, EXT);
    VK12_COMMAND(vkCmdSetPrimitiveRestartEnable, EXT);
    VK12_COMMAND(vkCmdSetRasterizerDiscardEnable, EXT);
    VK12_COMMAND(vkGetDeviceImageMemoryRequirements, KHR);
    VK12_COMMAND(vkGetDeviceImageSparseMemoryRequirements, KHR);
#undef VK12_COMMAND
    return !*lacks;
}

/* What the Switch's choice of its device starts from, and leaves for the rest of the start: g_vk and the
   translator's caps as they were before the device, which each choice starts from (the device made again decides
   everything anew, the swapchain's lock among it, which nothing takes before there is a swapchain), and what it
   decided that the queues, the memory and the log read */
typedef struct {
    VkState vk;
    ShCaps translate;
    int core13, second, sparse_queue;
    const char* store_none_ext;
    int has_drm; /* VK_EXT_physical_device_drm in the device's list, after its setting and the bans */
    int pstate;  /* gpu_pstate's value (SNX_GPU_PSTATE_*) */
    const char* boost_lacks; /* why an NVIDIA GPU that gpu_pstate holds has no VK_NV_low_latency2, NULL for none */
} DeviceChoice;

/* extension_needs, of a device as common/gpu/vk/device.c asks for them */
static uint32_t device_needs(const VkPhysicalDeviceProperties* p, const SnxVkExtensions* e, VkExtensionNeed* need,
                             void* user) {
    (void)user;
    return extension_needs(e, vk_api_for(p->apiVersion) >= VK_API_VERSION_1_3, need);
}

/* the Switch's device on the device device_rating rates highest (common/gpu/vk/device.h): the translator's caps, its
   features, extensions and queues, and the device made */
static bool choose_device(SnxVkChoice* c, void* user) {
    DeviceChoice* d = user;
    g_vk = d->vk;
    g_spirv_translate = d->translate;
    g_vk.phys = c->phys;
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    g_vk.api = vk_api_for(g_vk.props.apiVersion);
    c->api = g_vk.api;
    const int core13 = g_vk.api >= VK_API_VERSION_1_3;
    /* SPIR-V 1.6 is Vulkan 1.3's: a 1.2 device takes 1.5, which declares demotion as its extension (the translator
       does either way) */
    spv_emit_set_version(core13 ? 0x00010600u : 0x00010500u);
    u32 gfx_queues = 0;
    device_score(g_vk.phys, &g_vk.props, &g_vk.gfx_family, &g_vk.compute_family, &gfx_queues);
    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                              .pNext = &subgroup};
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    const VkSubgroupFeatureFlags warp_operations =
        VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    const int warp_operates = (subgroup.supportedOperations & warp_operations) == warp_operations;
    g_vk_subgroup_stages = subgroup.subgroupSize >= 32 && warp_operates ? subgroup.supportedStages : 0;
    g_vk_quad_stages = subgroup.subgroupSize >= 4 &&
                               (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT)
                           ? subgroup.supportedStages
                           : 0;
    g_vk_subgroup_lanes = subgroup.subgroupSize;
    /* a driver that lists the stage and gives wrong ballot and shuffle results there (Turnip's geometry stage, as
       Eden found): its programs take the path of a device that does not list it */
    if (vk_ban("VK_SHADER_STAGE_GEOMETRY_BIT", (g_vk_subgroup_stages & VK_SHADER_STAGE_GEOMETRY_BIT) != 0, ban_say))
        g_vk_subgroup_stages &= ~(VkShaderStageFlags)VK_SHADER_STAGE_GEOMETRY_BIT;
    g_spirv_translate.quad = (g_vk_subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                             (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
    /* compute has quad operations wherever the bit is and the stage has subgroup operations at all (the quad limit,
       quadOperationsInAllStages, is about the other stages) */
    g_spirv_translate.quad_compute = (g_vk_subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                                     (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
    g_vk_preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32 != 0;
    /* a binding's stride as dynamic state, core in Vulkan 1.3, unless a driver is banned from it (MoltenVK's fails every
       pipeline that lists it): pipeline.c puts the stride in the pipeline then */
    g_vk.has_dynamic_stride = !vk_ban("VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE", 1, ban_say);
    /* the rings the CPU fills in cached host memory by default (cmd.c's stage_cached) */
    g_vk.stage_cached = vk_prefer("VK_MEMORY_PROPERTY_HOST_CACHED_BIT", 1, ban_say) != 0;
    /* the constant buffers' windows of the staging ring, whose offsets take 224 bytes of push constants a stage
       (pipeline.c, cb_windows.h) */
    g_vk.cb_windows_ban = vk_ban_find("maxPushConstantsSize");
    g_vk_preserve_infnan64 = p12.shaderSignedZeroInfNanPreserveFloat64 != 0;
    g_vk.has_minmax_swizzle = p12.filterMinmaxImageComponentMapping != 0;
    /* 32 bit alone: a device whose denorm modes can't differ per width would need 16 and 64 bit declared too. A driver
       banned from the mode (Qualcomm's miscompiles with it) has its FTZ done per instruction, as on a device without
       it */
    int flush_denorm = p12.shaderDenormFlushToZeroFloat32 &&
                       p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
    g_vk_flush_denorm = flush_denorm && !vk_ban("shaderDenormFlushToZeroFloat32", flush_denorm, ban_say);
    /* a driver that truncates a 32-bit integer it converts to a float (Turnip on an Adreno 610): I2F.F32 converts the
       integer's two halves, each exact, and adds them, which rounds to nearest even as the console does */
    g_spirv_translate.i2f_nearest =
        (vk_ban("OpConvertSToF", 1, ban_say) ? 1 : 0) | (vk_ban("OpConvertUToF", 1, ban_say) ? 2 : 0);
    /* without it, vk_pipeline_init measures whether the device flushes by itself (vk_ftz_native) */
    if (!g_vk_preserve_infnan) WARN("vulkan: FP32 signed-zero/Inf/NaN preservation unavailable on this GPU\n");

    /* compute gets a queue of its own: of a compute-only family, or else a second one of the graphics family */
    int second = g_vk.compute_family == g_vk.gfx_family && gfx_queues >= 2;
    float prio[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci[3] = {{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.gfx_family,
                                       .queueCount = second ? 2 : 1,
                                       .pQueuePriorities = prio},
                                      {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = g_vk.compute_family,
                                       .queueCount = 1,
                                       .pQueuePriorities = prio}};
    /* the features drawing uses, where the device has them */
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &have);
    VkPhysicalDeviceFeatures* want = &g_vk.feat;
#define WANT(f) want->f = have.f
    /* constant buffers are bound with the bytes the shader reads, not the 64K it declares: reads past them stay in
       bounds (their values are undefined) */
    WANT(robustBufferAccess);
    WANT(imageCubeArray);
    WANT(independentBlend);
    WANT(dualSrcBlend);
    WANT(logicOp);
    WANT(depthClamp);
    WANT(depthBiasClamp);
    WANT(fillModeNonSolid);
    WANT(depthBounds);
    WANT(wideLines);
    WANT(largePoints);
    WANT(multiViewport);
    WANT(samplerAnisotropy);
    WANT(occlusionQueryPrecise); /* samples-passed counts (query.c) are exact */
    WANT(sampleRateShading);
    WANT(alphaToOne); /* multisampled draws asking for it (pipeline.c) */
    /* BCn, ASTC and ETC2 sampled as they are where the settings ask for it and the device has them, else decoded on
       the GPU as they are uploaded, BCn encoded again as ETC2 where the device samples that; ASTC decoded is encoded
       again as BC7 where the GPU samples BCn, unless astc_mode asks for exact RGBA8 (astc_choose says which, once the
       budget is known, and vk_blocks_say how each family is held). With exact RGBA8 only a texture with holes in its
       mapping is BC7 (texture.c). Thus BCn always where the device has it, which native_bc decides the sampling of */
    vk_blocks_want(&have, want);
    WANT(vertexPipelineStoresAndAtomics);
    /* indirect draws (render.c's draw_indirect): several in one call, and a base instance from the GPU's arguments */
    WANT(multiDrawIndirect);
    WANT(drawIndirectFirstInstance);
    WANT(fragmentStoresAndAtomics);
    WANT(shaderImageGatherExtended);
    WANT(shaderStorageImageExtendedFormats);
    WANT(shaderStorageImageWriteWithoutFormat);
    WANT(shaderStorageImageReadWithoutFormat);
    WANT(shaderStorageImageMultisample);
    WANT(shaderResourceMinLod);
    /* the texture table's (table.c) images and samplers, indexed by a word that is the same for the whole draw */
    WANT(shaderSampledImageArrayDynamicIndexing);
    g_spirv_translate.resource_min_lod = want->shaderResourceMinLod != 0;
    WANT(shaderClipDistance);
    WANT(shaderCullDistance);
    WANT(shaderFloat64);
    WANT(shaderInt64);
    WANT(shaderInt16);
    WANT(geometryShader);
    WANT(tessellationShader);
    WANT(shaderTessellationAndGeometryPointSize);
    /* images bigger than one allocation may be are bound in pieces, on the graphics queue, and textures that guest
       memory maps only in part get sparse residency there (memory.c) */
    int sparse_queue = 0;
    {
        u32 nq = 0;
        VkQueueFamilyProperties* q = snx_vk_queue_families(g_vk.phys, &nq);
        sparse_queue = g_vk.gfx_family < nq && (q[g_vk.gfx_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
        g_vk.gfx_timestamp_bits = g_vk.gfx_family < nq ? q[g_vk.gfx_family].timestampValidBits : 0;
        g_vk.compute_timestamp_bits = g_vk.compute_family < nq ? q[g_vk.compute_family].timestampValidBits : 0;
        free(q);
    }
    if (sparse_queue) {
        WANT(sparseBinding);
        /* textures with holes in their mapping given memory only where it is mapped (memory.c): 3D volumes in bricks,
           and other textures in the device's sparse blocks */
        WANT(sparseResidencyImage2D);
        WANT(sparseResidencyImage3D);
        /* and the blocks of such a texture that show the same guest memory bound to the same memory */
        WANT(sparseResidencyAliased);
        /* the mirror of guest memory that the programs logging their writes read and write in place (mirror.c) */
        WANT(sparseResidencyBuffer);
    }
#undef WANT
    /* points out of tessellation and geometry programs take the size those write, as the vertex program's do */
    g_spirv_translate.stage_point_size = want->shaderTessellationAndGeometryPointSize != 0;
    g_spirv_translate.geometry = want->geometryShader != 0;
    g_spirv_translate.tessellation = want->tessellationShader != 0;
    int has_pipeline_robustness = 0, has_robustness2 = 0;
    int has_shader_clock = 0, has_feedback_dynamic = 0, has_cond = 0, has_provoking = 0, video_exts = 0;
    VkVideoCodecOperationFlagsKHR video_codec_exts = 0; /* the codecs whose decode extension the device has */
    int has_list_restart = 0;
    int has_barycentric = 0;
    int has_portability = 0, has_reconvergence = 0;
    int has_calibrated = 0, has_present_id = 0, has_present_wait = 0, has_present_v2 = 0, has_calibrated_khr = 0;
    int has_google_timing = 0;
    int has_host_import = 0, has_ahb = 0, has_placed = 0, has_map2 = 0, has_dmabuf = 0, has_mutable = 0;
    int has_descriptor_buffer = 0, has_descriptor_heap = 0, has_extended_flags = 0, has_maintenance5 = 0;
    int has_maintenance8 = 0, has_copy_indirect = 0;
    int has_vertex_input = 0, has_legacy_vertex = 0, has_unused_attachments = 0;
    int has_hdr_metadata = 0;
    int has_gpl = 0, has_eds2 = 0, has_eds3 = 0; /* the extensions of pipeline libraries and their dynamic state */
    int has_depth_clip_control = 0;              /* and of the depth range as one of those states */
    int has_depth_clip_enable = 0;               /* depth clipping apart from the clamp (VK_EXT_depth_clip_enable) */
    int has_depth_clamp_control = 0;             /* the clamp's own range (VK_EXT_depth_clamp_control) */
    int has_mesh = 0;
    int has_size_control = 0; /* VK_EXT_subgroup_size_control, on a 1.2 device: 1.3 has it in its core */
    int has_mixed_dot = 0; /* VK_VALVE_shader_mixed_float_dot_product: HMMA's pairs of f16 products in one instruction */
    int has_coop = 0;      /* VK_KHR_cooperative_matrix: HMMA on the matrix unit (g_spirv_translate.hmma_coop) */
    /* VK_EXT_shader_atomic_float: RED.ADD.F32 in one instruction (g_spirv_translate.float_atomic_add) */
    int has_atomic_float = 0;
    int has_fault = 0; /* VK_EXT_device_fault: what the GPU reports of a fault, at a lost device */
    int has_diag = 0;  /* VK_NV_device_diagnostics_config, with vk_checkpoints alone */
    int has_pageable = 0;          /* VK_EXT_pageable_device_local_memory, and VK_EXT_memory_priority that it needs */
    /* VK_KHR_pipeline_executable_properties: the drivers' statistics of each pipeline, for pipeline_stats only */
    int has_exec = 0;
    /* VK_EXT_shader_object: draws bind compiled shaders and set the rest as dynamic state (shader_objects.c) */
    int has_so = 0;
    /* VK_NV_shading_rate_image and VK_KHR_fragment_shading_rate: an Ounce's shading rate image (shading_rate.c) */
    int has_nv_rate = 0, has_khr_rate = 0;
    const char* index8_ext = NULL; /* VK_KHR_index_type_uint8, or the EXT it was promoted from */
    const char* store_none_ext = NULL; /* a 1.2 device's store op NONE (vk12_store_none) */
    if (!g_opt_headless && !snx_vk_choice_enable(c, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return snx_vk_choice_refuse(c, "has no VK_KHR_swapchain to draw the window with");
    {
        /* the list after the extension settings (vk_ext_*) and the bans (common/gpu/vk/device.c) */
        const SnxVkExtensions* e = c->list;
        store_none_ext = vk12_store_none(e);
        /* again on this list, which the bans took out of: a 1.2 device banned from one of them is refused by its
           name, as one without it */
        char lacks12[400], why[480];
        if (!core13 && vk_1_2_missing(e, lacks12, sizeof lacks12)) {
            WARN("vulkan: %s has Vulkan 1.2 without %s\n", g_vk.props.deviceName, lacks12);
            snprintf(g_vk.start_failure, sizeof g_vk.start_failure,
                     "%s has Vulkan 1.2 without %s. ScarletNX needs Vulkan 1.3, or 1.2 with the extensions 1.3 took in",
                     g_vk.props.deviceName, lacks12);
            snprintf(why, sizeof why, "has Vulkan 1.2 without %s", lacks12);
            return snx_vk_choice_refuse(c, why);
        }
        { /* every device here has maintenance4 (core in 1.3, required of 1.2 above) */
            VkPhysicalDeviceMaintenance4Properties m4 = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
            VkPhysicalDeviceProperties2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m4};
            vkGetPhysicalDeviceProperties2(g_vk.phys, &q);
            g_vk.max_buffer_size = m4.maxBufferSize;
        }
        g_vk.has_divisor = snx_vk_choice_has(c, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        has_vertex_input = snx_vk_choice_has(c, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
        has_legacy_vertex = snx_vk_choice_has(c, VK_EXT_LEGACY_VERTEX_ATTRIBUTES_EXTENSION_NAME);
        has_unused_attachments = snx_vk_choice_has(c, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
        has_gpl = snx_vk_choice_has(c, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                  snx_vk_choice_has(c, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        has_eds2 = snx_vk_choice_has(c, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
        has_eds3 = snx_vk_choice_has(c, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        has_depth_clip_control = snx_vk_choice_has(c, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
        has_depth_clip_enable = snx_vk_choice_has(c, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
        has_depth_clamp_control = snx_vk_choice_has(c, VK_EXT_DEPTH_CLAMP_CONTROL_EXTENSION_NAME);
        g_vk.has_xfb = snx_vk_choice_has(c, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
        g_vk.has_mixed_samples = snx_vk_choice_has(c, VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);
        /* only when vk_checkpoints asks: a device made with it marks every draw and dispatch, and a RenderDoc capture
           of it needs the extension to replay, which other vendors' drivers lack */
        g_vk.has_nv_checkpoints =
            g_settings.gpu_checkpoints && snx_vk_choice_has(c, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        has_fault = snx_vk_choice_has(c, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        /* vk_checkpoints alone, as the checkpoints: the driver keeps more of what a fault needs (shader debug info,
           what each resource is, its own checkpoints), at a cost of speed */
        has_diag = g_settings.gpu_checkpoints && snx_vk_choice_has(c, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        has_so = snx_vk_choice_has(c, VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
        has_exec = g_settings.pipeline_stats[0] &&
                   snx_vk_choice_has(c, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        if (g_settings.pipeline_stats[0] && !has_exec)
            WARN("vulkan: pipeline_stats: the driver has no VK_KHR_pipeline_executable_properties: no statistics\n");
        g_vk.has_coverage_to_color = snx_vk_choice_has(c, VK_NV_FRAGMENT_COVERAGE_TO_COLOR_EXTENSION_NAME);
        g_vk.has_post_depth_coverage = snx_vk_choice_has(c, VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME);
        g_vk.sample_locations.available = snx_vk_choice_has(c, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
        g_vk.advanced_blend_caps.available = snx_vk_choice_has(c, VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME);
        g_vk.has_fragment_interlock = snx_vk_choice_has(c, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
        g_vk.has_border_color = snx_vk_choice_has(c, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        g_vk.has_view_min_lod = snx_vk_choice_has(c, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
        has_shader_clock = snx_vk_choice_has(c, VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
        has_descriptor_buffer = snx_vk_choice_has(c, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        has_descriptor_heap = snx_vk_choice_has(c, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        has_extended_flags = snx_vk_choice_has(c, VK_KHR_EXTENDED_FLAGS_EXTENSION_NAME);
        has_maintenance5 = snx_vk_choice_has(c, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        has_maintenance8 = snx_vk_choice_has(c, VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
        has_copy_indirect = snx_vk_choice_has(c, VK_KHR_COPY_MEMORY_INDIRECT_EXTENSION_NAME);
        has_reconvergence = snx_vk_choice_has(c, VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME);
        g_vk.has_feedback = snx_vk_choice_has(c, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
        has_feedback_dynamic = snx_vk_choice_has(c, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
        g_vk.has_local_read = snx_vk_choice_has(c, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
        g_vk.has_unified_layouts = snx_vk_choice_has(c, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
        has_cond = snx_vk_choice_has(c, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
        has_provoking = snx_vk_choice_has(c, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
        has_mesh = snx_vk_choice_has(c, VK_EXT_MESH_SHADER_EXTENSION_NAME);
        has_mixed_dot = snx_vk_choice_has(c, VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME);
        has_size_control = !core13 && snx_vk_choice_has(c, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        has_coop = snx_vk_choice_has(c, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        has_list_restart = snx_vk_choice_has(c, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
        has_atomic_float = snx_vk_choice_has(c, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
        has_barycentric = snx_vk_choice_has(c, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        g_vk.has_conservative = snx_vk_choice_has(c, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
        has_nv_rate = snx_vk_choice_has(c, VK_NV_SHADING_RATE_IMAGE_EXTENSION_NAME);
        has_khr_rate = snx_vk_choice_has(c, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        g_vk.has_depth_range_unrestricted = snx_vk_choice_has(c, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
        has_pipeline_robustness = snx_vk_choice_has(c, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
        has_robustness2 = snx_vk_choice_has(c, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        /* the driver's budget for each heap, the other processes' use taken out (devmem.c): without it the budget
           is 80% of a heap's size, blind to what the desktop and other programs hold */
        g_vk.has_budget = snx_vk_choice_has(c, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        /* memory the driver may move to system memory when the GPU's own is short, by the priorities devmem.c gives
           (vk_devmem_priority). Priorities alone are not used: devmem.c knows an allocation's owner only after the
           driver made its memory, and only pageable memory takes a new priority after that */
        has_pageable = snx_vk_choice_has(c, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME) &&
                       snx_vk_choice_has(c, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        /* the GPU's clock next to the client's, for the profiler's GPU track (gpuprof.c's calibration) */
        has_calibrated = snx_vk_choice_has(c, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        /* a portability driver's subset must be enabled when it has one (MoltenVK) */
        has_portability = snx_vk_choice_has(c, "VK_KHR_portability_subset");
        if (has_portability) snx_vk_choice_enable(c, "VK_KHR_portability_subset");
        g_vk.has_portability = has_portability;
        video_exts = snx_vk_choice_has(c, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) +
                     snx_vk_choice_has(c, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        for (u32 i = 0; i < sizeof video_codecs / sizeof *video_codecs; i++)
            if (snx_vk_choice_has(c, video_codecs[i].ext)) video_codec_exts |= video_codecs[i].op;
        if (snx_vk_choice_has(c, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME))
            index8_ext = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
        else if (snx_vk_choice_has(c, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME))
            index8_ext = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
        has_present_id = !g_opt_headless && snx_vk_choice_has(c, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        has_present_wait = !g_opt_headless && snx_vk_choice_has(c, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        if (!g_opt_headless && g_vk.has_surface_caps2)
            has_present_v2 = snx_vk_choice_has(c, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) +
                             snx_vk_choice_has(c, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME) +
                             snx_vk_choice_has(c, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
        has_calibrated_khr = snx_vk_choice_has(c, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        has_google_timing = !g_opt_headless && snx_vk_choice_has(c, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        has_host_import = snx_vk_choice_has(c, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        /* the render node gpu_pstate holds an AMD GPU's clocks on: its properties read, the device not made with it */
        d->has_drm = snx_vk_choice_has(c, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        has_placed = snx_vk_choice_has(c, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
        has_map2 = snx_vk_choice_has(c, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
        has_dmabuf = snx_vk_choice_has(c, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                     snx_vk_choice_has(c, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        /* host-only descriptor pools: the twins of a game's own descriptor sets, which its copies read (gpu/gvk) */
        has_mutable = g_settings.guest_vulkan && snx_vk_choice_has(c, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME);
#if defined(__ANDROID__) /* every driver there takes AHardwareBuffers, which guest memory can move into (uma.c) */
        has_ahb = snx_vk_choice_has(c, "VK_ANDROID_external_memory_android_hardware_buffer");
#endif
        /* an HDR10 swapchain's light levels, which display.c's frames are tone mapped for */
        has_hdr_metadata = !g_opt_headless && snx_vk_choice_has(c, VK_EXT_HDR_METADATA_EXTENSION_NAME);
    }
    /* guest memory the GPU reads in place (uma.c) */
    /* a driver that takes every import's pages again at each submission (uma.c's vk_uma_imports_dear): said where
       uma_copies leaves the choice to the line */
    static const char imports_key[] = "VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT";
    g_vk.imports_dear =
        (g_settings.guest_buffers_copied ? vk_ban_find(imports_key) : vk_ban(imports_key, 1, ban_say)) != 0;
    int uma_with = vk_uma_choose(has_host_import, has_ahb, has_placed && has_map2, has_dmabuf);
    /* and guest memory a game's own Vulkan maps (gpu/gvk/gvk_api.c), imported as host memory, or the device's own
       exported as a dma-buf where the device imports no host memory (a capture layer: RenderDoc takes no host memory) */
    if (uma_with == VK_UMA_WITH_HOST || (g_settings.guest_vulkan && has_host_import))
        snx_vk_choice_enable(c, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (uma_with == VK_UMA_WITH_AHB) snx_vk_choice_enable(c, "VK_ANDROID_external_memory_android_hardware_buffer");
    /* the dma-bufs of memfds that guest memory moves into (uma.c, udmabuf), which placed maps' views also take */
    int fd_exts = uma_with == VK_UMA_WITH_DMABUF;
    if (fd_exts || (g_settings.guest_vulkan && has_dmabuf)) {
        snx_vk_choice_enable(c, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    }
    /* a dma-buf takes an image only with a DRM format modifier: a game's own linear images (gpu/gvk) are linear's */
    if (g_settings.guest_vulkan && has_dmabuf &&
        snx_vk_choice_has(c, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME))
        snx_vk_choice_enable(c, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    if (g_vk.has_budget) snx_vk_choice_enable(c, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (has_hdr_metadata) {
        snx_vk_choice_enable(c, VK_EXT_HDR_METADATA_EXTENSION_NAME);
        g_vk.has_hdr_metadata = 1;
    }
    if (g_vk.has_depth_range_unrestricted) snx_vk_choice_enable(c, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    if (g_vk.has_mixed_samples) snx_vk_choice_enable(c, VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);
    /* vk_checkpoints=1: the hanging draw's or dispatch's marker (record.c's vk_checkpoint_dump), and checkpoints.c's */
    if (g_vk.has_nv_checkpoints) snx_vk_choice_enable(c, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (g_vk.has_coverage_to_color) snx_vk_choice_enable(c, VK_NV_FRAGMENT_COVERAGE_TO_COLOR_EXTENSION_NAME);
    if (g_vk.has_post_depth_coverage) snx_vk_choice_enable(c, VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME);
    if (g_vk.sample_locations.available) {
        snx_vk_choice_enable(c, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
        VkPhysicalDeviceSampleLocationsPropertiesEXT locations = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  .pNext = &locations};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &properties);
        g_vk.sample_locations.counts = locations.sampleLocationSampleCounts;
        g_vk.sample_locations.subpixel_bits = locations.sampleLocationSubPixelBits;
        g_vk.sample_locations.variable = locations.variableSampleLocations;
        g_vk.sample_locations.coordinate_min = locations.sampleLocationCoordinateRange[0];
        g_vk.sample_locations.coordinate_max = locations.sampleLocationCoordinateRange[1];
        for (u32 i = 0; i < 5; i++) {
            VkMultisamplePropertiesEXT multisample = {.sType = VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT};
            vkGetPhysicalDeviceMultisamplePropertiesEXT(g_vk.phys, (VkSampleCountFlagBits)(1u << i), &multisample);
            g_vk.sample_locations.grid[i] = multisample.maxSampleLocationGridSize;
        }
    }
    /* the extension only with a host time domain among the calibrateable ones: CLOCK_MONOTONIC_RAW is the clock
       the Tracy client keeps its own time in, so the two domains' pair maps the GPU's timestamps onto it */
    if (has_calibrated) {
        u32 nd = 0;
        if (VK_OK(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(g_vk.phys, &nd, NULL)) && nd) {
            VkTimeDomainEXT* dom = malloc(sizeof *dom * nd);
            if (dom) {
                if (VK_OK(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(g_vk.phys, &nd, dom)))
                    for (u32 i = 0; i < nd; i++) {
                        if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT) g_vk.has_calibrated_host = 1;
                        if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) g_vk.has_calibrated_mono = 1;
                        if (dom[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT) g_vk.has_calibrated_qpc = 1;
                    }
                free(dom);
            }
        }
        if (g_vk.has_calibrated_host || g_vk.has_calibrated_qpc)
            snx_vk_choice_enable(c, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }
    /* FFmpeg's Vulkan decoder loads the 1.3 commands by their core names */
    g_vk.video_codecs = 0;
    g_vk.video_family = g_settings.vulkan_video_decode && video_exts == 2 && video_codec_exts && core13
                            ? video_decode_family(video_codec_exts, &g_vk.video_codecs)
                            : ~0u;
    if (g_vk.video_family != ~0u) {
        snx_vk_choice_enable(c, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        for (u32 i = 0; i < sizeof video_codecs / sizeof *video_codecs; i++)
            if (g_vk.video_codecs & video_codecs[i].op) {
                snx_vk_choice_enable(c, video_codecs[i].ext);
                LOG("vulkan: %s movies decode with Vulkan Video, on queue family %u (experimental)\n",
                    video_codecs[i].name, g_vk.video_family);
            } else if (video_codec_exts & video_codecs[i].op)
                LOG("vulkan: %s movies decode on the CPU: queue family %u, which Vulkan Video decodes on, doesn't "
                    "decode %s\n",
                    video_codecs[i].name, g_vk.video_family, video_codecs[i].name);
            else
                LOG("vulkan: %s movies decode on the CPU: %s is off (its line above says why)\n", video_codecs[i].name,
                    video_codecs[i].ext);
    } else if (g_settings.vulkan_video_decode && !core13)
        WARN("vulkan: Vulkan Video decode was asked for, but FFmpeg's Vulkan decoder needs Vulkan 1.3 and this device "
             "runs 1.2: FFmpeg decodes movies on the CPU\n");
    else if (g_settings.vulkan_video_decode)
        WARN("vulkan: Vulkan Video decode was asked for, but the device has no H.264 or AV1 decode queue: FFmpeg "
             "decodes movies on the CPU\n");
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdiv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceImageViewMinLodFeaturesEXT fminlod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxfb = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT qadvanced = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT qinterlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceShaderClockFeaturesKHR qclock = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR qreconv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
    VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE qmixdot = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR qcoop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT qatomic_float = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDevicePipelineRobustnessFeaturesEXT qpipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT qrobust = {.sType =
                                                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT qmutable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features q12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceMeshShaderFeaturesEXT qmesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceVulkan11Features q11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan13Features q13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeatures qsize = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    /* a 1.2 device's: those of vk12_extensions, which the 1.3 struct holds on a 1.3 one (and EDS's are core there) */
    VkPhysicalDeviceSynchronization2Features qsync2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures qrendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceMaintenance4Features qmaintenance4 = {.sType =
                                                              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures qdemote = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT qeds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT qeds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceFaultFeaturesEXT qfault = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV qdiag = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV};
    VkPhysicalDeviceMemoryPriorityFeaturesEXT qpriority = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT qpageable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT qso = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR qexec = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT qdesc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT qheap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceExtendedFlagsFeaturesKHR qflags = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_FLAGS_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR qmaintenance5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceMaintenance8FeaturesKHR qmaintenance8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR qcopy = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT qloop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT qloop_dyn = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR qlocal = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR qunified = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT qcond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT qprov = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT qlist_restart = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR qbarycentric = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR qindex8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT qvertex_input = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT qlegacy_vertex = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT qunused = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT qgpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    /* qeds2 above: a 1.2 device's, and the pipeline libraries' patch control points (has_eds2) */
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT qeds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT qclip_control = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT qclip_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceDepthClampControlFeaturesEXT qclamp_control = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceShadingRateImageFeaturesNV qnv_rate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR qkhr_rate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    /* with the subset enabled, each of its features is off until the device asks for it: a view's swizzle, for one */
    PortabilityFeatures qport = {.sType = PORTABILITY_FEATURES_STYPE};
    VkPhysicalDevicePresentIdFeaturesKHR qpid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR qpwait = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentId2FeaturesKHR qpid2 = {.sType =
                                                       VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    VkPhysicalDevicePresentWait2FeaturesKHR qpwait2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR};
    VkPhysicalDevicePresentTimingFeaturesEXT qtiming = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    int present_wait = has_present_id && has_present_wait;
    has_present_v2 = has_present_v2 == 3 && has_calibrated_khr;
    int robustness = has_pipeline_robustness && has_robustness2;
    void* const query[] = {&f2,
                           has_descriptor_buffer ? &qdesc : NULL,
                           has_descriptor_heap ? &qheap : NULL,
                           has_extended_flags ? &qflags : NULL,
                           has_maintenance5 ? &qmaintenance5 : NULL,
                           has_maintenance8 ? &qmaintenance8 : NULL,
                           has_copy_indirect ? &qcopy : NULL,
                           &q11,
                           &q12,
                           core13 ? &q13 : NULL,
                           has_size_control ? &qsize : NULL,
                           has_mesh ? &qmesh : NULL,
                           core13 ? NULL : &qsync2,
                           core13 ? NULL : &qrendering,
                           core13 ? NULL : &qmaintenance4,
                           core13 ? NULL : &qdemote,
                           core13 ? NULL : &qeds,
                           /* a 1.2 device's, and the patch control points of pipeline libraries' dynamic state */
                           !core13 || has_eds2 ? &qeds2 : NULL,
                           g_vk.has_divisor ? &fdiv : NULL,
                           g_vk.has_border_color ? &fbc : NULL,
                           g_vk.has_view_min_lod ? &fminlod : NULL,
                           g_vk.has_xfb ? &fxfb : NULL,
                           g_vk.advanced_blend_caps.available ? &qadvanced : NULL,
                           g_vk.has_fragment_interlock ? &qinterlock : NULL,
                           g_vk.has_feedback ? &qloop : NULL,
                           has_feedback_dynamic ? &qloop_dyn : NULL,
                           g_vk.has_local_read ? &qlocal : NULL,
                           g_vk.has_unified_layouts ? &qunified : NULL,
                           has_cond ? &qcond : NULL,
                           has_provoking ? &qprov : NULL,
                           has_list_restart ? &qlist_restart : NULL,
                           has_barycentric ? &qbarycentric : NULL,
                           index8_ext ? &qindex8 : NULL,
                           has_vertex_input ? &qvertex_input : NULL,
                           has_legacy_vertex ? &qlegacy_vertex : NULL,
                           has_unused_attachments ? &qunused : NULL,
                           has_gpl ? &qgpl : NULL,
                           has_eds3 ? &qeds3 : NULL,
                           has_depth_clip_control ? &qclip_control : NULL,
                           has_depth_clip_enable ? &qclip_enable : NULL,
                           has_depth_clamp_control ? &qclamp_control : NULL,
                           has_portability ? &qport : NULL,
                           has_reconvergence ? &qreconv : NULL,
                           robustness ? &qpipeline : NULL,
                           has_robustness2 ? &qrobust : NULL,
                           present_wait ? &qpid : NULL,
                           present_wait ? &qpwait : NULL,
                           has_present_v2 ? &qpid2 : NULL,
                           has_present_v2 ? &qpwait2 : NULL,
                           has_present_v2 ? &qtiming : NULL,
                           has_shader_clock ? &qclock : NULL,
                           has_mixed_dot ? &qmixdot : NULL,
                           has_coop ? &qcoop : NULL,
                           has_atomic_float ? &qatomic_float : NULL,
                           has_fault ? &qfault : NULL,
                           has_diag ? &qdiag : NULL,
                           has_pageable ? &qpriority : NULL,
                           has_pageable ? &qpageable : NULL,
                           has_exec ? &qexec : NULL,
                           has_so ? &qso : NULL,
                           has_nv_rate ? &qnv_rate : NULL,
                           has_mutable ? &qmutable : NULL,
                           has_khr_rate ? &qkhr_rate : NULL};
    snx_vk_chain(query, sizeof query / sizeof *query);
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
    /* Each order between submissions, queues and the CPU is a timeline value: the graphics and compute timelines
       (cmd.c, compute.c), the sparse binds (memory.c), and the CPU's waits for a path of its own (vk_host_next). It is
       core in 1.2 (VK_KHR_timeline_semaphore before), and 1.2 requires it of each device, so the renderer has no
       way without it. extension_bans.txt's "warn" lines on it are said (vk_bans_filter): an "off" line could not be
       kept, and the log says so */
    if (!q12.timelineSemaphore)
        fatal("vulkan: %s has no timeline semaphores, which Vulkan 1.2 requires", g_vk.props.deviceName);
    if (vk_ban_find("timelineSemaphore"))
        WARN("vulkan: extension_bans.txt turns timelineSemaphore off on this driver, but the renderer has no way "
             "without it: it stays on\n");
    LOG("vulkan: timeline semaphores on (core 1.2): graphics, compute, sparse binds and the CPU's waits\n");
    /* without it the shaders do their FP16 work in 32 bits */
    if (vk_ban("shaderFloat16", q12.shaderFloat16, ban_say)) q12.shaderFloat16 = VK_FALSE;
    if (!core13) {
        q13.synchronization2 = qsync2.synchronization2;
        q13.dynamicRendering = qrendering.dynamicRendering;
        q13.maintenance4 = qmaintenance4.maintenance4;
        q13.shaderDemoteToHelperInvocation = qdemote.shaderDemoteToHelperInvocation;
        /* not needed to run: the mesh draws and the compute subgroups of 32 are left out without them */
        q13.subgroupSizeControl = qsize.subgroupSizeControl;
        q13.computeFullSubgroups = qsize.computeFullSubgroups;
        static const char* const names[] = {"synchronization2",     "dynamicRendering",
                                            "maintenance4",         "shaderDemoteToHelperInvocation",
                                            "extendedDynamicState", "extendedDynamicState2"};
        const VkBool32 have12[] = {q13.synchronization2,      q13.dynamicRendering,
                                   q13.maintenance4,          q13.shaderDemoteToHelperInvocation,
                                   qeds.extendedDynamicState, qeds2.extendedDynamicState2};
        char lacks[256] = "";
        size_t at = 0;
        for (u32 i = 0; i < sizeof names / sizeof *names; i++)
            if (!have12[i] && at < sizeof lacks)
                at += (size_t)snprintf(lacks + at, sizeof lacks - at, "%s%s", at ? ", " : "", names[i]);
        if (at) {
            WARN("vulkan: %s has Vulkan 1.2 and the extensions 1.3 took in, but not their features %s\n",
                 g_vk.props.deviceName, lacks);
            snprintf(
                g_vk.start_failure, sizeof g_vk.start_failure,
                "%s has Vulkan 1.2 without the features %s. ScarletNX needs Vulkan 1.3, or 1.2 with the extensions "
                "1.3 took in and their features",
                g_vk.props.deviceName, lacks);
            char why[320];
            snprintf(why, sizeof why, "has Vulkan 1.2 without the features %s", lacks);
            return snx_vk_choice_refuse(c, why);
        }
    }
    g_vk.has_present_wait = has_present_id && has_present_wait && qpid.presentId && qpwait.presentWait;
    g_vk.has_present_timing = g_vk.has_present_wait && has_present_v2 && qpid2.presentId2 && qpwait2.presentWait2 &&
                              qtiming.presentTiming && qtiming.presentAtAbsoluteTime;
    g_vk.has_display_timing = has_google_timing && !g_vk.has_present_wait;
    /* VK_EXT_robustness2 for the whole device: reads past a buffer's bound range are zero and writes there are
       dropped (the console's constant buffers read zero past their size, and an unmapped page of a program's storage
       map is index 0xffffffff, shader_gbuf_word_index), image reads out of bounds are zero (an alpha of 1 where the
       format has none), and empty slots take null descriptors (vk_null_descriptors). A ban of robustBufferAccess2 also
       stops the bounds of vertex inputs below, and the device-wide feature needs robustBufferAccess */
    VkPhysicalDeviceRobustness2PropertiesEXT robust_limits = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT};
    if (has_robustness2) {
        /* an extension's feature, keyed as extension_bans.txt's head says (extension.feature) */
        if (vk_ban("VK_EXT_robustness2.robustBufferAccess2", qrobust.robustBufferAccess2, ban_say))
            qrobust.robustBufferAccess2 = VK_FALSE;
        if (vk_ban("VK_EXT_robustness2.robustImageAccess2", qrobust.robustImageAccess2, ban_say))
            qrobust.robustImageAccess2 = VK_FALSE;
        if (vk_ban("VK_EXT_robustness2.nullDescriptor", qrobust.nullDescriptor, ban_say))
            qrobust.nullDescriptor = VK_FALSE;
        VkPhysicalDeviceProperties2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                             .pNext = &robust_limits};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &query);
    }
    g_vk.robust_buffer2 = qrobust.robustBufferAccess2 && want->robustBufferAccess;
    g_vk.robust_image2 = qrobust.robustImageAccess2 != 0;
    g_vk.null_descriptor = qrobust.nullDescriptor != 0;
    g_vk.robust_ubo_align = (u32)robust_limits.robustUniformBufferAccessSizeAlignment;
    g_vk.robust_ssbo_align = (u32)robust_limits.robustStorageBufferAccessSizeAlignment;
    LOG("vulkan: robustness2: robustBufferAccess2=%d robustImageAccess2=%d nullDescriptor=%d (uniform ranges checked "
        "in %u-byte steps, storage in %u)%s\n",
        g_vk.robust_buffer2, g_vk.robust_image2, g_vk.null_descriptor, g_vk.robust_ubo_align, g_vk.robust_ssbo_align,
        has_robustness2 ? "" : ", the device has no VK_EXT_robustness2");
    /* device-wide robustBufferAccess2 already gives vertex inputs their strict bounds: the per-pipeline struct (and
       VK_EXT_pipeline_robustness) is only needed where the device feature is off, so don't chain a redundant one */
    g_vk.has_vertex_robustness2 = qpipeline.pipelineRobustness && qrobust.robustBufferAccess2 && !g_vk.robust_buffer2;
    g_vk.has_cond_render = has_cond && qcond.conditionalRendering;
    g_vk.has_fault = has_fault && qfault.deviceFault;
    g_vk.has_nv_diag_config = has_diag && qdiag.diagnosticsConfig;
    g_vk.has_pageable = has_pageable && qpriority.memoryPriority && qpageable.pageableDeviceLocalMemory;
    g_vk.has_exec_stats = has_exec && qexec.pipelineExecutableInfo;
    g_vk.has_provoking_last = has_provoking && qprov.provokingVertexLast;
    g_vk.has_list_restart = has_list_restart && qlist_restart.primitiveTopologyListRestart;
    g_vk.has_patch_list_restart = g_vk.has_list_restart && qlist_restart.primitiveTopologyPatchListRestart;
    if (g_vk.has_list_restart)
        LOG("vulkan: the restart index restarts lists as it does strips%s (VK_EXT_primitive_topology_list_restart)\n",
            g_vk.has_patch_list_restart ? ", and patch lists" : ", not patch lists");
    else
        LOG("vulkan: the restart index restarts strips and fans only: it is a vertex in a list, as the device has %s\n",
            has_list_restart ? "VK_EXT_primitive_topology_list_restart without its list feature"
                             : "no VK_EXT_primitive_topology_list_restart");
    g_vk.has_barycentric = has_barycentric && qbarycentric.fragmentShaderBarycentric;
    /* pipelines made one at a time where a driver's compiler can't take several threads at once: the extension bans'
       off lines on vkCreateGraphicsPipelines. Elsewhere the lock would make each fast link wait behind a part's
       compile or a background optimized link, 1.2 to 1.7 ms a link where 0.02 ms is the link's own */
    snx_vk_compile_serial(vk_ban("vkCreateGraphicsPipelines", 1, ban_say) != 0);
    /* the drivers whose own interpolation of a clipped primitive's inputs is the console's take no varying pass
       (render.c's draw_varying): the extension bans' prefer lines on Location */
    g_vk.host_varying = vk_prefer("Location", 1, ban_say) != 0;
    if (g_vk.host_varying)
        LOG("vulkan: fragment inputs are the driver's interpolation, which keeps the console's precision\n");
    else if (g_vk.has_barycentric)
        LOG("vulkan: an unculled quad draw's fragment inputs are interpolated from its vertices' values "
            "(VK_KHR_fragment_shader_barycentric), another draw's position inputs read from the fragment's place\n");
    else
        LOG("vulkan: a draw's position inputs are read from the fragment's place; an unculled quad draw's other "
            "inputs are %s, as the device has %s\n",
            g_vk.feat.geometryShader ? "interpolated from its vertices' values, which a geometry program hands over"
                                     : "the driver's interpolation",
            has_barycentric ? "VK_KHR_fragment_shader_barycentric without its feature"
                            : "no VK_KHR_fragment_shader_barycentric");
    g_vk.has_index8 = index8_ext && qindex8.indexTypeUint8;
    {
        VkPhysicalDeviceSubgroupSizeControlProperties sizes = subgroup_sizes(core13 || has_size_control, NULL);
        g_vk.has_cs_warp32 = q13.subgroupSizeControl && q13.computeFullSubgroups && sizes.minSubgroupSize <= 32 &&
                             sizes.maxSubgroupSize >= 32 &&
                             (sizes.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    }
    /* a device whose own subgroups are narrower and that lets a compute pipeline ask for 32 serves a compute program's
       VOTE and SHFL so: compute.c asks, for a program that reads other lanes */
    if (warp_operates && g_vk.has_cs_warp32 && (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
        g_vk_subgroup_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
        g_spirv_translate.quad_compute = (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0;
    }
    log_subgroups(subgroup.subgroupSize, subgroup.supportedOperations, subgroup.supportedStages);
    /* after has_cs_warp32: the compute-task path's task program needs it (mesh_choose) */
    mesh_choose(has_mesh, &qmesh, &q13, core13 || has_size_control);
    {
        /* each draw's vertex layout as dynamic state, or a pipeline per layout: the setting, then the device and the
           drivers known to draw it wrong (vertex_input.h). A value the setting doesn't take stops the game here, as
           a player who wrote vertex_input_dynamic_state=of would otherwise play with what they meant to turn off */
        int mode = vk_vi_mode(g_settings.vertex_input_dynamic_state), warn;
        char line[2048], ban[1200];
        uint32_t ban_line = vk_ban_find(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
        if (ban_line) vk_ban_why(ban_line, ban, sizeof ban);
        if (mode < 0)
            fatal("settings: vertex_input_dynamic_state=%s is not auto, on or off",
                  g_settings.vertex_input_dynamic_state);
#if defined(__ANDROID__)
        int android = 1;
#else
        int android = 0;
#endif
        VkViDevice vd = {g_vk.props.deviceName,
                         p12.driverName,
                         p12.driverInfo,
                         (u32)p12.driverID,
                         g_vk.props.driverVersion,
                         has_vertex_input ? (qvertex_input.vertexInputDynamicState ? 2 : 1) : 0,
                         android,
                         ban_line ? ban : NULL};
        g_vk.vertex_input_dynamic = vk_vi_choose(mode, &vd, line, sizeof line, &warn);
        if (warn)
            WARN("%s\n", line);
        else
            LOG("%s\n", line);
    }
    /* VK_EXT_legacy_vertex_attributes reads an attribute at any byte, which the console does: a stream whose stride
       isn't a multiple of its attributes' components is bound as the guest has it, where each draw of it otherwise
       reads a copy restrided on the GPU (render.c's restride), a dispatch that ends the rendering pass. The extension
       needs VK_EXT_vertex_input_dynamic_state (its dependency), and the renderer takes it with the dynamic layout */
    g_vk.has_legacy_vertex = has_legacy_vertex && qlegacy_vertex.legacyVertexAttributes && g_vk.vertex_input_dynamic;
    const char* legacy_why =
        !has_legacy_vertex ? "no VK_EXT_legacy_vertex_attributes, or a setting or a ban took it out"
        : !qlegacy_vertex.legacyVertexAttributes ? "VK_EXT_legacy_vertex_attributes without legacyVertexAttributes"
                                                 : "the vertex layout is no dynamic state";
    /* VK_EXT_dynamic_rendering_unused_attachments: a pipeline leaves the format of a target its draws write nothing
       to UNDEFINED, so draws into passes that hold other formats there share it (render.c's draw_key) */
    g_vk.has_unused_attachments = has_unused_attachments && qunused.dynamicRenderingUnusedAttachments;
    if (g_vk.has_unused_attachments)
        LOG("vulkan: a pipeline leaves out the color targets its draws write nothing to "
            "(VK_EXT_dynamic_rendering_unused_attachments)\n");
    if (g_vk.has_legacy_vertex)
        LOG("vulkan: a vertex stream of any stride is bound as the guest has it (VK_EXT_legacy_vertex_attributes)\n");
    else
        LOG("vulkan: a vertex stream whose stride isn't a multiple of its attributes' components is restrided on the "
            "GPU for each draw: %s\n",
            legacy_why);
    /* the dynamic state and the barriers within a pass only serve feedback loops */
    g_vk.has_feedback = g_vk.has_feedback && qloop.attachmentFeedbackLoopLayout;
    g_vk.has_feedback_dynamic =
        g_vk.has_feedback && has_feedback_dynamic && qloop_dyn.attachmentFeedbackLoopDynamicState;
    g_vk.has_local_read = g_vk.has_feedback && g_vk.has_local_read && qlocal.dynamicRenderingLocalRead;
    /* the extension alone allows GENERAL where the specification asks for it. The feature says that GENERAL costs
       nothing against each use's own layout, which is why the renderer then keeps its images there */
    g_vk.has_unified_layouts = g_vk.has_unified_layouts && qunified.unifiedImageLayouts;
    g_spirv_translate.maximal_reconvergence = has_reconvergence && qreconv.shaderMaximalReconvergence;
    g_spirv_translate.clock_scopes =
        has_shader_clock ? (qclock.shaderSubgroupClock ? 1 : 0) | (qclock.shaderDeviceClock ? 2 : 0) : 0;
    g_vk.has_divisor = g_vk.has_divisor && fdiv.vertexAttributeInstanceRateDivisor;
    {
        /* an Ounce's shading rate image: NVIDIA's extension where it is, as it has the console's palettes, else the
           attachment of the KHR one. Each palette entry of the device's image is a rate (shading_rate.c), 13 of them */
        VkPhysicalDeviceShadingRateImagePropertiesNV pnv = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_PROPERTIES_NV};
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR pkhr = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        if (has_nv_rate) p2.pNext = &pnv;
        else if (has_khr_rate) p2.pNext = &pkhr;
        if (p2.pNext) vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        VkVrsDevice vd = {.ounce = g_ounce,
                          .nv = has_nv_rate && qnv_rate.shadingRateImage,
                          .nv_texel = pnv.shadingRateTexelSize,
                          .nv_palette = pnv.shadingRatePaletteSize,
                          .khr = has_khr_rate && qkhr_rate.attachmentFragmentShadingRate &&
                                 qkhr_rate.pipelineFragmentShadingRate,
                          .khr_min = pkhr.minFragmentShadingRateAttachmentTexelSize,
                          .khr_max = pkhr.maxFragmentShadingRateAttachmentTexelSize,
                          .khr_aspect = pkhr.maxFragmentShadingRateAttachmentTexelSizeAspectRatio};
        char line[300];
        g_vk.vrs = vk_vrs_choose(&vd, &g_vk.vrs_texel, line, sizeof line);
        g_spirv_translate.shading_rate = g_vk.vrs == VK_VRS_NV    ? SH_RATE_NV
                                         : g_vk.vrs == VK_VRS_KHR ? SH_RATE_KHR
                                                                  : SH_RATE_NONE;
        LOG("%s\n", line);
    }
    {
        /* pipelines from libraries (pipeline_parts.h), and the pre-rasterization state they take as dynamic state */
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        if (has_gpl) {
            VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
            vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        }
        VkGplDevice gd = {.setting = g_settings.pipeline_libraries,
                          .has_ext = has_gpl,
                          .library = qgpl.graphicsPipelineLibrary,
                          .fast_link = gp.graphicsPipelineLibraryFastLinking,
                          .independent_interpolation = gp.graphicsPipelineLibraryIndependentInterpolationDecoration,
                          .polygon = qeds3.extendedDynamicState3PolygonMode,
                          .depth_clamp = qeds3.extendedDynamicState3DepthClampEnable,
                          .provoking = qeds3.extendedDynamicState3ProvokingVertexMode,
                          .conservative_mode = qeds3.extendedDynamicState3ConservativeRasterizationMode,
                          .conservative_size = qeds3.extendedDynamicState3ExtraPrimitiveOverestimationSize,
                          .patch = qeds2.extendedDynamicState2PatchControlPoints,
                          .depth_range = qeds3.extendedDynamicState3DepthClipNegativeOneToOne,
                          .depth_clip_control = qclip_control.depthClipControl,
                          .depth_clip = qeds3.extendedDynamicState3DepthClipEnable,
                          .has_depth_clip_enable = has_depth_clip_enable && qclip_enable.depthClipEnable,
                          .has_provoking_last = g_vk.has_provoking_last,
                          .has_conservative = g_vk.has_conservative};
        char line[400];
        vk_gpl_bans(&gd, ban_say);
        g_vk.has_gpl = vk_gpl_choose(&gd, &g_vk.dyn_pr, line, sizeof line);
        LOG("%s\n", line);
        /* the blending as dynamic state: a draw's pipeline serves every blend of its programs and state, and the draw
           sets its own (render.c). Each of the three members a ban can turn off */
        const int blend_members = has_eds3 && qeds3.extendedDynamicState3ColorBlendEnable &&
                                  qeds3.extendedDynamicState3ColorBlendEquation &&
                                  qeds3.extendedDynamicState3ColorWriteMask;
        g_vk.dyn_blend =
            blend_members &&
            !vk_ban("VK_EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEnable", 1, ban_say) &&
            !vk_ban("VK_EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEquation", 1, ban_say) &&
            !vk_ban("VK_EXT_extended_dynamic_state3.extendedDynamicState3ColorWriteMask", 1, ban_say);
        if (g_vk.dyn_blend)
            LOG("vulkan: a draw's blending is dynamic state (VK_EXT_extended_dynamic_state3): one pipeline serves every "
                "blend of its programs\n");
        else
            LOG("vulkan: a draw's blending is in its pipeline, one for each blend: %s\n",
                !blend_members ? "the device has not VK_EXT_extended_dynamic_state3's blend enable, equation and write "
                                 "mask, or a setting or a ban took the extension out"
                               : "a ban turned one of them off");
        /* a ban that holds took the extension out of the list (vk_bans_filter): its line says why */
        uint32_t ban_line = vk_ban_find(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        if (ban_line) {
            char ban[1200];
            vk_ban_why(ban_line, ban, sizeof ban);
            LOG("%s\n", ban);
        }
        vk_depth_range_line(&gd, g_vk.dyn_pr, line, sizeof line);
        LOG("%s\n", line);
        /* Shader objects where the device has them: a draw binds its compiled shaders and sets what a pipeline held as
           dynamic state, and no pipeline is made for each combination of shaders and state (issue #1454). They take
           the pre-rasterization state that pipeline libraries take as dynamic (render.c's pr_dynamic) and the vertex
           layout as the draws set it, so they need both. A draw they can't take is made as a pipeline
           (shader_objects.c's vk_so_takes) */
        const char* so_why = !has_so ? "the device has no VK_EXT_shader_object, or a setting or a ban took it out"
                             : !qso.shaderObject ? "the device has VK_EXT_shader_object without shaderObject"
                             : !g_vk.has_gpl ? "pipeline libraries are off, and with them the dynamic pre-rasterization "
                                               "state"
                             : !g_vk.vertex_input_dynamic ? "the vertex layout is no dynamic state"
                             : g_vk.has_vertex_robustness2
                                 ? "vertex inputs take their strict bounds from each pipeline, without the device's "
                                   "robustBufferAccess2"
                                 : NULL;
        g_vk.has_shader_objects = !so_why;
        /* a shader object has no executables to ask statistics of: a draw that binds them makes no pipeline, and no
           row */
        if (g_vk.has_shader_objects && g_vk.has_exec_stats)
            WARN("vulkan: pipeline_stats: draws bind shader objects (VK_EXT_shader_object), which the driver gives no "
                 "statistics of: only compute pipelines, and the pipelines of draws shader objects can't take, get "
                 "rows; with vk_ext_shader_object=off every draw's program gets them\n");
        if (g_vk.has_shader_objects) {
            VkPhysicalDeviceShaderObjectPropertiesEXT sop = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sop};
            vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
            memcpy(g_vk.so_binary_uuid, sop.shaderBinaryUUID, VK_UUID_SIZE);
            g_vk.so_binary_version = sop.shaderBinaryVersion;
            LOG("vulkan: draws bind shader objects (VK_EXT_shader_object), and go through pipelines only where they "
                "can't\n");
        } else
            LOG("vulkan: draws go through pipelines: %s\n", so_why);
        /* depth clipping and clamping apart, as the console's viewport clip control has them, where the device can;
           with pipeline libraries' dynamic clamp only if the clip is dynamic too (pipeline_parts.h) */
        int clamp_alone = (g_vk.dyn_pr & VK_DYN_PR_DEPTH_CLAMP) && !(g_vk.dyn_pr & VK_DYN_PR_DEPTH_CLIP);
        g_vk.has_depth_clip_enable = gd.has_depth_clip_enable && !clamp_alone;
        if (g_vk.has_depth_clip_enable)
            LOG("vulkan: depth is clipped and clamped as the console sets each (VK_EXT_depth_clip_enable%s)\n",
                g_vk.dyn_pr & VK_DYN_PR_DEPTH_CLIP ? ", both as dynamic state" : "");
        else
            LOG("vulkan: depth clipping goes with the clamp, one switch: a fragment's depth past the viewport's range is "
                "undefined with the clamp off, where the console clamps it (%s)\n",
                !has_depth_clip_enable ? "VK_EXT_depth_clip_enable is off: its line above says why"
                : !qclip_enable.depthClipEnable
                    ? "the device has VK_EXT_depth_clip_enable without depthClipEnable"
                    : "the clamp is dynamic state and the device has no extendedDynamicState3DepthClipEnable");
        g_vk.has_depth_clamp_range = has_depth_clamp_control && qclamp_control.depthClampControl;
        if (g_vk.has_depth_clamp_range)
            LOG("vulkan: a clamped depth stays in the viewport's clip range, as the console clamps it "
                "(VK_EXT_depth_clamp_control, as dynamic state)\n");
        else
            LOG("vulkan: a clamped depth stays in the viewport transform's range, where the console clamps to the "
                "viewport's clip range (%s)\n",
                !has_depth_clamp_control ? "VK_EXT_depth_clamp_control is off: its line above says why"
                                         : "the device has VK_EXT_depth_clamp_control without depthClampControl");
    }
    g_vk.has_xfb = g_vk.has_xfb && fxfb.transformFeedback;
    g_vk.has_geometry_streams = g_vk.has_xfb && fxfb.geometryStreams;
    g_spirv_translate.geometry_streams = g_vk.has_geometry_streams;
    if (g_vk.has_geometry_streams) {
        VkPhysicalDeviceTransformFeedbackPropertiesEXT properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &properties};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &query);
        g_vk.geometry_streams_lines_triangles = properties.transformFeedbackStreamsLinesTriangles;
    }
    g_spirv_translate.geometry_streams_lines_triangles = g_vk.geometry_streams_lines_triangles != 0;
    g_vk.has_sample_interlock = g_vk.has_fragment_interlock && qinterlock.fragmentShaderSampleInterlock;
    /* without the extension (MoltenVK) a capture's vertex program stores its words itself, which vertex stores allow */
    g_spirv_translate.xfb_stores = !g_vk.has_xfb && want->vertexPipelineStoresAndAtomics;
    if (g_spirv_translate.xfb_stores)
        LOG("vulkan: no VK_EXT_transform_feedback: vertex programs store what transform feedback captures\n");
    g_vk.has_fragment_interlock = g_vk.has_fragment_interlock && qinterlock.fragmentShaderPixelInterlock;
    if (g_vk.advanced_blend_caps.available) {
        VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT advanced = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  .pNext = &advanced};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &properties);
        g_vk.advanced_blend_caps.coherent = qadvanced.advancedBlendCoherentOperations;
        g_vk.advanced_blend_caps.max_attachments = advanced.advancedBlendMaxColorAttachments;
        g_vk.advanced_blend_caps.nonpremultiplied_source = advanced.advancedBlendNonPremultipliedSrcColor;
        g_vk.advanced_blend_caps.correlated_overlap = advanced.advancedBlendCorrelatedOverlap;
        g_vk.advanced_blend_caps.all_operations = advanced.advancedBlendAllOperations;
    }
    g_vk.has_border_color = g_vk.has_border_color && fbc.customBorderColors && fbc.customBorderColorWithoutFormat;
    g_vk.has_view_min_lod = g_vk.has_view_min_lod && fminlod.minLod;
    if (g_vk.has_border_color) { /* how many samplers may have a custom border color: as few as 32 */
        VkPhysicalDeviceCustomBorderColorPropertiesEXT cbp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 cp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cbp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &cp2);
        g_vk.border_color_max = cbp.maxCustomBorderColorSamplers;
    }
    if (has_descriptor_buffer && qdesc.descriptorBuffer && q12.bufferDeviceAddress) {
        g_vk.descriptor_props = (VkPhysicalDeviceDescriptorBufferPropertiesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 desc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                            .pNext = &g_vk.descriptor_props};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &desc);
        g_vk.has_descriptor_buffers =
            g_vk.descriptor_props.combinedImageSamplerDescriptorSingleArray &&
            g_vk.descriptor_props.maxSamplerDescriptorBufferRange >= (64ull << 20) &&
            g_vk.descriptor_props.maxResourceDescriptorBufferRange >= (64ull << 20) &&
            g_vk.descriptor_props.samplerDescriptorBufferAddressSpaceSize >= (VK_FRAMES * (64ull << 20)) &&
            g_vk.descriptor_props.resourceDescriptorBufferAddressSpaceSize >= (VK_FRAMES * (64ull << 20)) &&
            g_vk.descriptor_props.descriptorBufferAddressSpaceSize >= (VK_FRAMES * (64ull << 20));
    }
    /* Automatic by where the device's memory is (vk_descriptors_auto) */
    int descriptors = vk_descriptors_auto(g_settings.descriptor_buffers, g_vk.mem_model == VK_MEM_UMA);
    if (descriptors != g_settings.descriptor_buffers)
        LOG("vulkan: vk_descriptors_mode=%d (Automatic) is %d, %s, on this GPU, which %s\n",
            g_settings.descriptor_buffers, descriptors, descriptors ? "descriptor heaps" : "descriptor sets",
            g_vk.mem_model == VK_MEM_UMA ? "shares the system's memory" : "has memory of its own");
    g_vk.has_descriptor_buffers = g_vk.has_descriptor_buffers && descriptors;
    if (g_vk.has_descriptor_buffers && !vk_descriptors_memory(g_vk.phys)) {
        WARN("vulkan: descriptor buffers need host-visible, coherent and cached memory, which this device has not: "
             "graphics descriptors use sets\n");
        g_vk.has_descriptor_buffers = 0;
    }
    const char* heaps_missing = vk_descriptor_heap_missing(
        has_descriptor_heap, has_descriptor_heap && qheap.descriptorHeap, q12.bufferDeviceAddress,
        (has_extended_flags && qflags.extendedFlags) || (has_maintenance5 && qmaintenance5.maintenance5));
    g_vk.has_descriptor_heap_support = !heaps_missing && descriptors >= 2;
    if (heaps_missing && descriptors >= 2)
        LOG("vulkan: vk_descriptors_mode=%d asks for descriptor heaps, which this device has not (%s): graphics "
            "descriptors use %s\n",
            g_settings.descriptor_buffers, heaps_missing, g_vk.has_descriptor_buffers ? "buffers" : "sets");
    g_vk.has_descriptor_heaps = g_vk.has_descriptor_heap_support;
    /* the compute-task path's task program binds its set with descriptor sets or buffers: a heap's bind is the
       graphics pipeline's (descriptors.c's vk_descriptors_bind) */
    if (g_vk.has_descriptor_heaps && g_vk.mesh_task_compute) {
        g_vk.mesh_task_compute = 0;
        LOG("vulkan: mesh draws keep their task and mesh stages together: the compute-task path needs descriptor sets or "
            "buffers, and this device's graphics descriptors are heaps (vk_mesh_path=%d)\n",
            g_settings.mesh_task_path);
    }
    g_vk.cb_address = g_settings.constant_buffers_by_address;
    /* the copies name device addresses: the device's memory gets them for these too (bufferDeviceAddress) */
    g_vk.has_copy_indirect =
        g_settings.indirect_copies && has_copy_indirect && qcopy.indirectMemoryCopy && q12.bufferDeviceAddress;
    if (g_vk.has_copy_indirect) { /* recorded in the graphics queue's command buffers: its family has to take them */
        VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR pcopy = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pcopy};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
        g_vk.has_copy_indirect = (pcopy.supportedQueues & VK_QUEUE_GRAPHICS_BIT) != 0;
    }
    g_vk.has_inplace_loads = g_settings.texture_loads_in_place && q12.bufferDeviceAddress;
    LOG("vulkan: kept results are copied %s, and texture loads read them %s\n",
        g_vk.has_copy_indirect                                      ? "by indirect copies"
        : !g_settings.indirect_copies                               ? "one command each (vk_copy_indirect=0)"
        : !has_copy_indirect || !qcopy.indirectMemoryCopy          ? "one command each (no VK_KHR_copy_memory_indirect)"
        : !q12.bufferDeviceAddress                                  ? "one command each (no bufferDeviceAddress)"
                                                                    : "one command each (not on the graphics queue)",
        g_vk.has_inplace_loads              ? "in place"
        : !g_settings.texture_loads_in_place ? "from copies (vk_inplace_loads=0)"
                                             : "from copies (no bufferDeviceAddress)");
    if (g_vk.has_descriptor_heap_support) {
        g_vk.descriptor_heap_props = (VkPhysicalDeviceDescriptorHeapPropertiesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                             .pNext = &g_vk.descriptor_heap_props};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
    }
    /* FFmpeg's decoder makes views of its two plane pictures */
    g_spirv_translate.dynamic_texture_offsets =
        has_maintenance8 && qmaintenance8.maintenance8 && want->shaderImageGatherExtended;
    LOG("vulkan: dynamic texture offsets use %s\n",
        g_spirv_translate.dynamic_texture_offsets ? "native maintenance8 sampling" : "portable dispatch");
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .shaderDrawParameters = q11.shaderDrawParameters,
                                            .samplerYcbcrConversion =
                                                g_vk.video_family != ~0u && q11.samplerYcbcrConversion};
    g_vk.has_shader_draw_parameters = q11.shaderDrawParameters != 0;
    g_spirv_translate.draw_parameters = g_vk.has_shader_draw_parameters;
    g_vk.has_draw_indirect_count = q12.drawIndirectCount != 0;
    /* Enable mirror clamp and vertex shader Layer output when the device supports them. */
    g_vk.has_mirror_clamp = q12.samplerMirrorClampToEdge;
    g_vk.has_host_query_reset = q12.hostQueryReset != 0; /* stamp.c's report timestamps */
    /* the texture table of SM86 programs (table.c): one set that the renderer writes while draws that bind it are
       pending, with slots that hold nothing yet. vk_pipeline_init decides with the limits and the other caps */
    /* SM86 programs' global memory by device address (ShCaps.gbuf_address, with the texture table): the buffers its
       bindings come from and their memory get device addresses. vk_pipeline_init decides with the other caps */
    g_vk.has_gbuf_address = g_shader_sm86 && q12.bufferDeviceAddress && g_vk.feat.shaderInt64;
    g_vk.has_table_features =
        g_shader_sm86 && q12.descriptorBindingPartiallyBound && q12.descriptorBindingSampledImageUpdateAfterBind &&
        q12.descriptorBindingUpdateUnusedWhilePending && g_vk.feat.shaderSampledImageArrayDynamicIndexing;
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f11,
        .samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge,
        .samplerFilterMinmax = q12.samplerFilterMinmax,
        .timelineSemaphore = VK_TRUE,
        .hostQueryReset = q12.hostQueryReset,
        .bufferDeviceAddress = g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support ||
                               g_vk.has_copy_indirect || g_vk.has_inplace_loads || g_vk.has_gbuf_address,
        .shaderFloat16 = q12.shaderFloat16,
        .shaderOutputViewportIndex = q12.shaderOutputViewportIndex,
        .shaderOutputLayer = q12.shaderOutputLayer,
        .drawIndirectCount = q12.drawIndirectCount,
        .shaderSampledImageArrayNonUniformIndexing = q12.shaderSampledImageArrayNonUniformIndexing,
        .descriptorBindingPartiallyBound = g_vk.has_table_features,
        .descriptorBindingSampledImageUpdateAfterBind = g_vk.has_table_features,
        .descriptorBindingUpdateUnusedWhilePending = g_vk.has_table_features};
    /* shaders pick an indexed texture per invocation from an array (SNX_VKB_TEXARRAY) */
    g_spirv_translate.texture_arrays = q12.shaderSampledImageArrayNonUniformIndexing != 0;
    g_spirv_translate.float16 = q12.shaderFloat16 != 0;
    g_spirv_translate.float64 = have.shaderFloat64 != 0;
    /* its f16 vectors are Float16's, so shaderFloat16 too */
    g_spirv_translate.mixed_dot =
        has_mixed_dot && qmixdot.shaderMixedFloatDotProductFloat16AccFloat32 && g_spirv_translate.float16;
    g_spirv_translate.float_atomic_add = has_atomic_float && qatomic_float.shaderBufferFloat32AtomicAdd;
    if (shader_hmma_f16_unused())
        LOG("vulkan: hmma_f16=1 asks for HMMA's sums in half floats; they stay in f32, as hmma_f16=0's: %s\n",
            shader_hmma_f16_unused());
    /* its f16 matrices are Float16's, its modules the Vulkan memory model's, and its matrices span a subgroup of 32.
       The shapes are the ones vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR lists: an SM86 HMMA's exact 16x8x16
       and 16x8x8 where they are, a 16x16x16, and Metal's simdgroup_matrix 8x8x8 */
    /* A subgroup of 32 lanes, which is what an HMMA and its matrices span: has_cs_warp32 where the device lets a
       pipeline ask for one, or where its subgroups are already 32 and compute has them (Metal's are, at every
       stage). Gating the matrix unit on the request alone left every driver whose size is fixed without it */
    int warp32 = g_vk.has_cs_warp32 || (subgroup.subgroupSize == 32 &&
                                        (g_vk_subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0);
    u32 coop_shapes = has_coop && qcoop.cooperativeMatrix && g_spirv_translate.float16 && q12.vulkanMemoryModel &&
                              q12.vulkanMemoryModelDeviceScope && warp32
                          ? coop_f16_f32_shapes(g_vk.phys)
                          : 0;
    g_spirv_translate.hmma_coop = coop_shapes ? (int)g_vk.props.limits.maxComputeSharedMemorySize : 0;
    g_spirv_translate.hmma_coop_shapes = coop_shapes;
    if (coop_shapes) {
        char names[64] = "";
        static const struct {
            u32 bit;
            const char* name;
        } listed[] = {{SH_COOP_16x16x16, "16x16x16"}, {SH_COOP_16x8x16, "16x8x16"},
                      {SH_COOP_16x8x8, "16x8x8"}, {SH_COOP_8x8x8, "8x8x8"}};
        for (u32 i = 0; i < 4; i++)
            if (coop_shapes & listed[i].bit) {
                size_t used = strlen(names);
                snprintf(names + used, sizeof names - used, "%s%s", used ? ", " : "", listed[i].name);
            }
        LOG("vulkan: HMMA runs on the device's cooperative matrices, %s a product, shared memory's own fragments "
            "after the program's\n", names);
    }
    f12.vulkanMemoryModel = f12.vulkanMemoryModelDeviceScope = g_spirv_translate.hmma_coop != 0;
    /* a game's own SPIR-V (gpu/gvk) is the console's driver's to take, which lays blocks out as scalars */
    f12.scalarBlockLayout = g_settings.guest_vulkan && q12.scalarBlockLayout;
    f12.uniformBufferStandardLayout = g_settings.guest_vulkan && q12.uniformBufferStandardLayout;
    /* and indexes its arrays of buffers per vertex, which gvk_spirv.c marks NonUniform for the host's driver */
    f12.shaderUniformBufferArrayNonUniformIndexing =
        g_settings.guest_vulkan && q12.shaderUniformBufferArrayNonUniformIndexing;
    f12.shaderStorageBufferArrayNonUniformIndexing =
        g_settings.guest_vulkan && q12.shaderStorageBufferArrayNonUniformIndexing;
    f12.shaderStorageImageArrayNonUniformIndexing =
        g_settings.guest_vulkan && q12.shaderStorageImageArrayNonUniformIndexing;
    f12.shaderInputAttachmentArrayNonUniformIndexing =
        g_settings.guest_vulkan && q12.shaderInputAttachmentArrayNonUniformIndexing;
    f12.shaderUniformTexelBufferArrayNonUniformIndexing =
        g_settings.guest_vulkan && q12.shaderUniformTexelBufferArrayNonUniformIndexing;
    f12.shaderStorageTexelBufferArrayNonUniformIndexing =
        g_settings.guest_vulkan && q12.shaderStorageTexelBufferArrayNonUniformIndexing;
    g_vk.gvk_nonuniform =
        (f12.shaderUniformBufferArrayNonUniformIndexing ? GVK_SPIRV_UBO_NONUNIFORM : 0) |
        (f12.shaderStorageBufferArrayNonUniformIndexing ? GVK_SPIRV_SSBO_NONUNIFORM : 0) |
        (f12.shaderSampledImageArrayNonUniformIndexing ? GVK_SPIRV_SAMPLED_NONUNIFORM : 0) |
        (f12.shaderStorageImageArrayNonUniformIndexing ? GVK_SPIRV_STORAGE_IMAGE_NONUNIFORM : 0) |
        (f12.shaderInputAttachmentArrayNonUniformIndexing ? GVK_SPIRV_INPUT_NONUNIFORM : 0) |
        (f12.shaderUniformTexelBufferArrayNonUniformIndexing ? GVK_SPIRV_UNIFORM_TEXEL_NONUNIFORM : 0) |
        (f12.shaderStorageTexelBufferArrayNonUniformIndexing ? GVK_SPIRV_STORAGE_TEXEL_NONUNIFORM : 0);
    if (g_settings.guest_vulkan)
        LOG("gvk: the host indexes arrays of descriptors by what differs between invocations, kinds %x of %x (a kind "
            "it lacks keeps the game's shaders as they are)\n",
            g_vk.gvk_nonuniform, GVK_SPIRV_NONUNIFORM_ALL);
    /* HMMA off the matrix unit, by the rules: pairs of lanes with subgroups of 32 where the extension bans prefer
       them (OpGroupNonUniformShuffleXor), and A and B through shared memory where the driver loops for a shuffle
       whose index differs between lanes, which no feature or property tells, so the table does. hmma_mode settles
       the way before the first translation (vk_hmma_pick), Auto by the bench, keeping the rules' where it cannot
       run */
    int compute_subgroups = (g_vk_subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    g_vk.hmma_rule =
        shader_hmma_rule(compute_subgroups, vk_prefer("OpGroupNonUniformShuffleXor", g_vk.has_cs_warp32, ban_say) != 0,
                         vk_ban("OpGroupNonUniformShuffle", compute_subgroups, ban_say) != 0);
    shader_hmma_take(g_vk.hmma_rule, (int)g_vk.props.limits.maxComputeSharedMemorySize);
    /* shaderDenormPreserveFloat16 is the device's word that it can keep f16 subnormals: without it an SM86 program
       packs those by hand (sm86_translate.c's pack_half), as Turnip on an Adreno 610 flushed them to zero */
    if (vk_ban("shaderDenormPreserveFloat16", p12.shaderDenormPreserveFloat16, ban_say))
        p12.shaderDenormPreserveFloat16 = VK_FALSE;
    g_spirv_translate.half_flush = !p12.shaderDenormPreserveFloat16;
    g_spirv_translate.half_rounding = 0;
    if (q12.shaderFloat16 && p12.shaderRoundingModeRTEFloat16 && p12.shaderDenormPreserveFloat16 &&
        p12.shaderSignedZeroInfNanPreserveFloat16 &&
        p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE &&
        p12.roundingModeIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE) {
        int linked = p12.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY ||
                     p12.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_32_BIT_ONLY;
        if (!linked || (have.shaderFloat64 && p12.shaderRoundingModeRTEFloat64 && p12.shaderDenormPreserveFloat64 &&
                        p12.shaderSignedZeroInfNanPreserveFloat64))
            g_spirv_translate.half_rounding = linked ? 3 : 1;
    }
    g_spirv_translate.output_layer =
        q12.shaderOutputLayer != 0; /* ShaderLayer: a vertex or tessellation program's layer */
    g_spirv_translate.viewport_index = have.multiViewport ? 2 | (q12.shaderOutputViewportIndex ? 1 : 0) : 0;
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &f12,
                                            .synchronization2 = VK_TRUE,
                                            .dynamicRendering = VK_TRUE,
                                            .maintenance4 = q13.maintenance4,
                                            /* KIL demotes the lane; S2R queries its helper status */
                                            .shaderDemoteToHelperInvocation = VK_TRUE,
                                            .pipelineCreationCacheControl = q13.pipelineCreationCacheControl,
                                            /* the task and mesh stages' subgroups of 32, whole (mesh_choose), and
                                               those of the compute programs that use other lanes' values */
                                            .subgroupSizeControl = g_vk.has_mesh || g_vk.has_cs_warp32,
                                            .computeFullSubgroups = g_vk.has_mesh || g_vk.has_cs_warp32};
    /* the BC7 encoder's build with packed dot products (astc.c's bc7_pipe), where the GPU has an instruction for
       them: a GeForce GT 1030 encodes 5% faster with it. Spelled out, they are the same numbers */
    g_vk.bc7_dot = 0;
    if (core13 && q13.shaderIntegerDotProduct) {
        VkPhysicalDeviceVulkan13Properties dp13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 dpp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp13};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &dpp2);
        g_vk.bc7_dot = dp13.integerDotProduct4x8BitPackedUnsignedAccelerated != 0;
    }
    f13.shaderIntegerDotProduct = g_vk.bc7_dot ? VK_TRUE : VK_FALSE;
    /* a 1.2 device's chain leaves f13 out, and q13 was not queried: no cache-only pipelines there */
    g_vk.has_cache_control = core13 && q13.pipelineCreationCacheControl;
    void* chain = &f13;
    /* a 1.2 device: those features as its extensions' (vk12_extensions), the 1.3 struct left out */
    VkPhysicalDeviceSynchronization2Features fsync2 = {.sType =
                                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                                                       .pNext = &f12,
                                                       .synchronization2 = VK_TRUE};
    VkPhysicalDeviceDynamicRenderingFeatures frendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &fsync2,
        .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceMaintenance4Features fmaintenance4 = {.sType =
                                                              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,
                                                          .pNext = &frendering,
                                                          .maintenance4 = VK_TRUE};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures fdemote = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
        .pNext = &fmaintenance4,
        .shaderDemoteToHelperInvocation = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = &fdemote,
        .extendedDynamicState = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
        .pNext = &feds,
        .extendedDynamicState2 = VK_TRUE};
    /* the subgroup sizes f13 asks for on a 1.3 device, from VK_EXT_subgroup_size_control, where they are used */
    VkPhysicalDeviceSubgroupSizeControlFeatures fsize = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
        .pNext = &feds2,
        .subgroupSizeControl = f13.subgroupSizeControl,
        .computeFullSubgroups = f13.computeFullSubgroups};
    if (!core13) {
        chain = &feds2;
        for (u32 i = 0; i < VK12_EXTENSIONS; i++) snx_vk_choice_enable(c, vk12_extensions[i]);
        snx_vk_choice_enable(c, store_none_ext);
        if (fsize.subgroupSizeControl) {
            chain = &fsize;
            snx_vk_choice_enable(c, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        }
    }
    VkPhysicalDeviceMaintenance8FeaturesKHR fmaintenance8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR, .maintenance8 = VK_TRUE};
    if (g_spirv_translate.dynamic_texture_offsets) {
        fmaintenance8.pNext = chain;
        chain = &fmaintenance8;
        snx_vk_choice_enable(c, VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    }
    VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR fcopy = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR, .indirectMemoryCopy = VK_TRUE};
    if (g_vk.has_copy_indirect) {
        fcopy.pNext = chain;
        chain = &fcopy;
        snx_vk_choice_enable(c, VK_KHR_COPY_MEMORY_INDIRECT_EXTENSION_NAME);
    }
    VkPhysicalDeviceDescriptorBufferFeaturesEXT fdesc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
        .pNext = chain,
        .descriptorBuffer = VK_TRUE};
    if (g_vk.has_descriptor_buffers) {
        chain = &fdesc;
        snx_vk_choice_enable(c, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    }
    VkPhysicalDeviceDescriptorHeapFeaturesEXT fheap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT, .descriptorHeap = VK_TRUE};
    VkPhysicalDeviceExtendedFlagsFeaturesKHR fflags = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_FLAGS_FEATURES_KHR, .extendedFlags = VK_TRUE};
    VkPhysicalDeviceMaintenance5FeaturesKHR fmaintenance5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .maintenance5 = VK_TRUE};
    if (g_vk.has_descriptor_heap_support) {
        if (has_maintenance5 && qmaintenance5.maintenance5) {
            fmaintenance5.pNext = chain;
            chain = &fmaintenance5;
            snx_vk_choice_enable(c, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        } else {
            fflags.pNext = chain;
            chain = &fflags;
            snx_vk_choice_enable(c, VK_KHR_EXTENDED_FLAGS_EXTENSION_NAME);
        }
        fheap.pNext = chain;
        chain = &fheap;
        snx_vk_choice_enable(c, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
    }
    if (g_vk.has_divisor) {
        fdiv = (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
            .pNext = chain,
            .vertexAttributeInstanceRateDivisor = VK_TRUE};
        chain = &fdiv;
        snx_vk_choice_enable(c, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    }
    if (g_vk.has_border_color) {
        fbc = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .pNext = chain,
            .customBorderColors = VK_TRUE,
            .customBorderColorWithoutFormat = VK_TRUE};
        chain = &fbc;
        snx_vk_choice_enable(c, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    }
    if (g_vk.has_view_min_lod) {
        fminlod = (VkPhysicalDeviceImageViewMinLodFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT, .pNext = chain, .minLod = VK_TRUE};
        chain = &fminlod;
        snx_vk_choice_enable(c, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    }
    if (g_vk.has_xfb) {
        fxfb = (VkPhysicalDeviceTransformFeedbackFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
            .pNext = chain,
            .transformFeedback = VK_TRUE,
            .geometryStreams = g_vk.has_geometry_streams};
        chain = &fxfb;
        snx_vk_choice_enable(c, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    }
    /* VK_DESCRIPTOR_POOL_CREATE_HOST_ONLY_BIT_EXT needs the feature on */
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT fmutable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT,
        .mutableDescriptorType = VK_TRUE};
    g_vk.gvk_host_sets = has_mutable && qmutable.mutableDescriptorType;
    if (g_vk.gvk_host_sets) {
        fmutable.pNext = chain;
        chain = &fmutable;
        snx_vk_choice_enable(c, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME);
    }
    /* the heap as GPU memory where it is (uma.c's placed maps) */
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT fplaced = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
        .pNext = chain,
        .memoryMapPlaced = VK_TRUE,
        .memoryUnmapReserve = VK_TRUE};
    if (vk_uma_can_place()) {
        chain = &fplaced;
        snx_vk_choice_enable(c, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
        vk_uma_dmabuf(has_dmabuf); /* a view of it maps the same memory again, through a dma-buf */
        if (vk_uma_can_view() && !fd_exts) {
            snx_vk_choice_enable(c, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            snx_vk_choice_enable(c, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        }
    }
    VkPhysicalDeviceShaderClockFeaturesKHR fclock = {.sType =
                                                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
                                                     .pNext = chain,
                                                     .shaderSubgroupClock = !!(g_spirv_translate.clock_scopes & 1),
                                                     .shaderDeviceClock = !!(g_spirv_translate.clock_scopes & 2)};
    if (g_spirv_translate.clock_scopes) {
        chain = &fclock;
        snx_vk_choice_enable(c, VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
    }
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR freconv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,
        .pNext = chain,
        .shaderMaximalReconvergence = VK_TRUE};
    if (g_spirv_translate.maximal_reconvergence) {
        chain = &freconv;
        snx_vk_choice_enable(c, VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME);
    }
    VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE fmixdot = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE,
        .pNext = chain,
        .shaderMixedFloatDotProductFloat16AccFloat32 = VK_TRUE};
    if (g_spirv_translate.mixed_dot) {
        chain = &fmixdot;
        snx_vk_choice_enable(c, VK_VALVE_SHADER_MIXED_FLOAT_DOT_PRODUCT_EXTENSION_NAME);
    }
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcoop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
        .pNext = chain,
        .cooperativeMatrix = VK_TRUE};
    if (g_spirv_translate.hmma_coop) {
        chain = &fcoop;
        snx_vk_choice_enable(c, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    }
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT fatomic_float = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT,
        .pNext = chain,
        .shaderBufferFloat32AtomicAdd = VK_TRUE};
    if (g_spirv_translate.float_atomic_add) {
        chain = &fatomic_float;
        snx_vk_choice_enable(c, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
    }
    /* a lost device's fault, which vk_checkpoint_dump logs */
    VkPhysicalDeviceFaultFeaturesEXT ffault = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .pNext = chain, .deviceFault = VK_TRUE};
    if (g_vk.has_fault) {
        chain = &ffault;
        snx_vk_choice_enable(c, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
    /* vk_checkpoints=1 on NVIDIA: the driver keeps shader debug info and what each resource is, and sets its own
       checkpoints, for what a lost device's fault (vk_checkpoint_dump) and the checkpoints report */
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV fdiag = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV,
        .pNext = chain,
        .diagnosticsConfig = VK_TRUE};
    VkDeviceDiagnosticsConfigCreateInfoNV cdiag = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV,
        .pNext = &fdiag,
        .flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV |
                 VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV |
                 VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV};
    if (g_vk.has_nv_diag_config && snx_vk_choice_enable(c, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME))
        chain = &cdiag;
    /* the GPU's own memory that the driver may move to system memory when it is short, the lowest priority first */
    VkPhysicalDeviceMemoryPriorityFeaturesEXT fpriority = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT,
        .pNext = chain,
        .memoryPriority = VK_TRUE};
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT fpageable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT,
        .pNext = &fpriority,
        .pageableDeviceLocalMemory = VK_TRUE};
    if (g_vk.has_pageable) {
        chain = &fpageable;
        snx_vk_choice_enable(c, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME);
        LOG("vulkan: the driver may move the GPU's memory to system memory where it is short, the lowest priority "
            "first: render targets and rings high, textures with drawn pixels medium, textures loaded from memory low "
            "(VK_EXT_pageable_device_local_memory)\n");
    }
    VkPhysicalDeviceShaderObjectFeaturesEXT fso = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, .pNext = chain, .shaderObject = VK_TRUE};
    if (g_vk.has_shader_objects) {
        chain = &fso;
        snx_vk_choice_enable(c, VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
    }
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR fexec = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
        .pNext = chain,
        .pipelineExecutableInfo = VK_TRUE};
    if (g_vk.has_exec_stats) {
        chain = &fexec;
        snx_vk_choice_enable(c, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    }
    PortabilityFeatures fport = qport;
    fport.pNext = chain;
    if (has_portability) chain = &fport;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT floop = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
        .pNext = chain,
        .attachmentFeedbackLoopLayout = VK_TRUE};
    if (g_vk.has_feedback) {
        chain = &floop;
        snx_vk_choice_enable(c, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
    }
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT floop_dyn = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = chain,
        .attachmentFeedbackLoopDynamicState = VK_TRUE};
    if (g_vk.has_feedback_dynamic) {
        chain = &floop_dyn;
        snx_vk_choice_enable(c, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
    }
    /* a barrier within a vkCmdBeginRendering pass needs this feature (VUID-vkCmdPipelineBarrier2-None-09553) */
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flocal = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR,
        .pNext = chain,
        .dynamicRenderingLocalRead = VK_TRUE};
    if (g_vk.has_local_read) {
        chain = &flocal;
        snx_vk_choice_enable(c, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    }
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR funified = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
        .pNext = chain,
        .unifiedImageLayouts = VK_TRUE};
    if (g_vk.has_unified_layouts) {
        chain = &funified;
        snx_vk_choice_enable(c, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
    }
    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT fadvanced = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT,
        .pNext = chain,
        .advancedBlendCoherentOperations = g_vk.advanced_blend_caps.coherent};
    if (g_vk.advanced_blend_caps.available) {
        chain = &fadvanced;
        snx_vk_choice_enable(c, VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME);
    }
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT finterlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
        .pNext = chain,
        .fragmentShaderPixelInterlock = VK_TRUE,
        .fragmentShaderSampleInterlock = g_vk.has_sample_interlock};
    if (g_vk.has_fragment_interlock) {
        chain = &finterlock;
        snx_vk_choice_enable(c, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    }
    /* the render enable's comparison of an occlusion query's count, made on the GPU (gpu/vk/query.c) */
    VkPhysicalDeviceConditionalRenderingFeaturesEXT fcond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
        .pNext = chain,
        .conditionalRendering = VK_TRUE};
    if (g_vk.has_cond_render) {
        chain = &fcond;
        snx_vk_choice_enable(c, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
    }
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR fbarycentric = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
        .pNext = chain,
        .fragmentShaderBarycentric = VK_TRUE};
    if (g_vk.has_barycentric) {
        chain = &fbarycentric;
        snx_vk_choice_enable(c, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    }
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT flist_restart = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT,
        .pNext = chain,
        .primitiveTopologyListRestart = VK_TRUE,
        .primitiveTopologyPatchListRestart = (VkBool32)g_vk.has_patch_list_restart};
    if (g_vk.has_list_restart) {
        chain = &flist_restart;
        snx_vk_choice_enable(c, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    }
    VkPhysicalDeviceProvokingVertexFeaturesEXT fprov = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
        .pNext = chain,
        .provokingVertexLast = VK_TRUE};
    if (g_vk.has_provoking_last) {
        chain = &fprov;
        snx_vk_choice_enable(c, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    }
    /* conservative rasterization (the 3D engine's CONSERVATIVE_RASTER, NVN's SetConservativeRasterEnable), overestimated
       by the dilation the game asks for up to the device's most: the extension has no features to enable */
    if (g_vk.has_conservative) {
        VkPhysicalDeviceConservativeRasterizationPropertiesEXT cons = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cons};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        g_vk.conservative_extra = cons.maxExtraPrimitiveOverestimationSize;
        snx_vk_choice_enable(c, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
    }
    /* an Ounce's shading rate image (shading_rate.c): the one extension that g_vk.vrs chose. The two cannot be on
       together (VUID-VkDeviceCreateInfo-shadingRateImage-04478) */
    VkPhysicalDeviceShadingRateImageFeaturesNV fnv_rate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV,
        .pNext = chain,
        .shadingRateImage = VK_TRUE};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fkhr_rate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
        .pNext = chain,
        .pipelineFragmentShadingRate = VK_TRUE,
        .attachmentFragmentShadingRate = VK_TRUE};
    if (g_vk.vrs != VK_VRS_NONE) {
        chain = g_vk.vrs == VK_VRS_NV ? (void*)&fnv_rate : (void*)&fkhr_rate;
        snx_vk_choice_enable(c, g_vk.vrs == VK_VRS_NV ? VK_NV_SHADING_RATE_IMAGE_EXTENSION_NAME
                                                      : VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    }
    /* the 3D engine's 8-bit indices, read as they are (render.c); without it they are widened on the GPU */
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvertex_input = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = chain,
        .vertexInputDynamicState = VK_TRUE};
    VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT flegacy_vertex = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT,
        .legacyVertexAttributes = VK_TRUE};
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT funused = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT,
        .dynamicRenderingUnusedAttachments = VK_TRUE};
    if (g_vk.vertex_input_dynamic) {
        chain = &fvertex_input;
        snx_vk_choice_enable(c, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    }
    if (g_vk.has_legacy_vertex) {
        flegacy_vertex.pNext = chain;
        chain = &flegacy_vertex;
        snx_vk_choice_enable(c, VK_EXT_LEGACY_VERTEX_ATTRIBUTES_EXTENSION_NAME);
    }
    if (g_vk.has_unused_attachments) {
        funused.pNext = chain;
        chain = &funused;
        snx_vk_choice_enable(c, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
    }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
        .pNext = chain,
        .graphicsPipelineLibrary = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2_patch = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
        .extendedDynamicState2PatchControlPoints = VK_TRUE};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
        .extendedDynamicState3PolygonMode = (g_vk.dyn_pr & VK_DYN_PR_POLYGON) != 0,
        .extendedDynamicState3DepthClampEnable = (g_vk.dyn_pr & VK_DYN_PR_DEPTH_CLAMP) != 0,
        .extendedDynamicState3ProvokingVertexMode = (g_vk.dyn_pr & VK_DYN_PR_PROVOKING) != 0,
        .extendedDynamicState3ConservativeRasterizationMode = (g_vk.dyn_pr & VK_DYN_PR_CONSERVATIVE) != 0,
        .extendedDynamicState3ExtraPrimitiveOverestimationSize = (g_vk.dyn_pr & VK_DYN_PR_CONSERVATIVE) != 0,
        .extendedDynamicState3DepthClipNegativeOneToOne = (g_vk.dyn_pr & VK_DYN_PR_DEPTH_RANGE) != 0,
        .extendedDynamicState3DepthClipEnable = (g_vk.dyn_pr & VK_DYN_PR_DEPTH_CLIP) != 0};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fclip_control = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, .depthClipControl = VK_TRUE};
    if (g_vk.has_gpl) {
        chain = &fgpl;
        snx_vk_choice_enable(c, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        if ((g_vk.dyn_pr & VK_DYN_PR_PATCH) && !core13) /* a 1.2 device has EDS2 in its chain (vk12_extensions) */
            feds2.extendedDynamicState2PatchControlPoints = VK_TRUE;
        else if (g_vk.dyn_pr & VK_DYN_PR_PATCH) {
            feds2_patch.pNext = chain;
            chain = &feds2_patch;
            snx_vk_choice_enable(c, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
        }
        if (g_vk.dyn_pr & ~VK_DYN_PR_PATCH) {
            feds3.pNext = chain;
            chain = &feds3;
            snx_vk_choice_enable(c, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        }
        if (g_vk.dyn_pr & VK_DYN_PR_DEPTH_RANGE) {
            fclip_control.pNext = chain;
            chain = &fclip_control;
            snx_vk_choice_enable(c, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
        }
    }
    feds3.extendedDynamicState3ColorBlendEnable = feds3.extendedDynamicState3ColorBlendEquation =
        feds3.extendedDynamicState3ColorWriteMask = (VkBool32)g_vk.dyn_blend;
    if (g_vk.dyn_blend && !(g_vk.has_gpl && (g_vk.dyn_pr & ~VK_DYN_PR_PATCH))) { /* not in the chain above */
        feds3.pNext = chain;
        chain = &feds3;
        snx_vk_choice_enable(c, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    }
    VkPhysicalDeviceDepthClipEnableFeaturesEXT fclip_enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
        .pNext = chain,
        .depthClipEnable = VK_TRUE};
    if (g_vk.has_depth_clip_enable) {
        chain = &fclip_enable;
        snx_vk_choice_enable(c, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    }
    VkPhysicalDeviceDepthClampControlFeaturesEXT fclamp_control = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_CONTROL_FEATURES_EXT,
        .pNext = chain,
        .depthClampControl = VK_TRUE};
    if (g_vk.has_depth_clamp_range) {
        chain = &fclamp_control;
        snx_vk_choice_enable(c, VK_EXT_DEPTH_CLAMP_CONTROL_EXTENSION_NAME);
    }
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR findex8 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR,
        .pNext = chain,
        .indexTypeUint8 = VK_TRUE};
    if (g_vk.has_index8) {
        chain = &findex8;
        snx_vk_choice_enable(c, index8_ext);
    }
    /* when each present is displayed (display.c), which the game's pace goes by */
    VkPhysicalDevicePresentIdFeaturesKHR fpid = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = chain, .presentId = VK_TRUE};
    VkPhysicalDevicePresentWaitFeaturesKHR fpwait = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .pNext = &fpid, .presentWait = VK_TRUE};
    if (g_vk.has_present_wait) {
        chain = &fpwait;
        snx_vk_choice_enable(c, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }
    /* a game frame held until the refresh it is due at (display.c), where the surface can */
    VkPhysicalDevicePresentId2FeaturesKHR fpid2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .pNext = chain, .presentId2 = VK_TRUE};
    VkPhysicalDevicePresentWait2FeaturesKHR fpwait2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR,
        .pNext = &fpid2,
        .presentWait2 = VK_TRUE};
    VkPhysicalDevicePresentTimingFeaturesEXT ftiming = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
        .pNext = &fpwait2,
        .presentTiming = VK_TRUE,
        .presentAtAbsoluteTime = VK_TRUE};
    if (g_vk.has_present_timing) {
        chain = &ftiming;
        snx_vk_choice_enable(c, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
        snx_vk_choice_enable(c, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME); /* present timing's requirement */
    }
    if (g_vk.has_display_timing)
        snx_vk_choice_enable(c, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME); /* when presents are shown */
    /* an NVIDIA GPU's clocks held through VK_NV_low_latency2's boost (gpu_pstate.h), which needs present ids */
    SnxGpuPstateDevice pstate = {.vendor = g_vk.props.vendorID, .type = (int)g_vk.props.deviceType};
    d->boost_lacks = NULL;
    if (pstate.vendor == SNX_GPU_VENDOR_NVIDIA && snx_gpu_pstate_wanted(d->pstate, &pstate)) {
        if (!snx_vk_choice_has(c, VK_NV_LOW_LATENCY_2_EXTENSION_NAME))
            d->boost_lacks = "its device has no VK_NV_low_latency2 (the extension's line says why)";
        else if (!g_vk.has_present_wait && !g_vk.has_present_timing)
            d->boost_lacks = "its presents carry no ids (VK_KHR_present_id with present_wait, or their 2s), which "
                             "VK_NV_low_latency2 needs: a run without a window, or vk_ext_khr_present_id off";
        else
            g_vk.has_low_latency2 = snx_vk_choice_enable(c, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    }
    VkPhysicalDevicePipelineRobustnessFeaturesEXT fpipeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT,
        .pNext = chain,
        .pipelineRobustness = VK_TRUE};
    if (g_vk.has_vertex_robustness2) {
        chain = &fpipeline;
        snx_vk_choice_enable(c, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
    }
    VkPhysicalDeviceRobustness2FeaturesEXT frobust = {.sType =
                                                          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                                                      .pNext = chain,
                                                      .robustBufferAccess2 = (VkBool32)g_vk.robust_buffer2,
                                                      .robustImageAccess2 = (VkBool32)g_vk.robust_image2,
                                                      .nullDescriptor = (VkBool32)g_vk.null_descriptor};
    if (g_vk.has_vertex_robustness2 || g_vk.robust_buffer2 || g_vk.robust_image2 || g_vk.null_descriptor) {
        chain = &frobust;
        snx_vk_choice_enable(c, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }
    VkPhysicalDeviceMeshShaderFeaturesEXT fmesh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
                                                   .pNext = chain,
                                                   .meshShader = VK_TRUE,
                                                   .taskShader = (VkBool32)g_vk.has_task};
    if (g_vk.has_mesh) {
        chain = &fmesh;
        snx_vk_choice_enable(c, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }
    /* vk_checkpoints=1: checkpoints.c's markers too, on VK_NV_device_diagnostic_checkpoints, which the device enables
       above for this setting where it has it (has_nv_checkpoints) */
    if (g_settings.gpu_checkpoints) {
        g_vk.has_checkpoints = g_vk.has_nv_checkpoints;
        if (!g_vk.has_checkpoints)
            WARN("vulkan: vk_checkpoints: this driver has no VK_NV_device_diagnostic_checkpoints\n");
    }
    u32 nq = g_vk.compute_family == g_vk.gfx_family ? 1 : 2;
    if (g_vk.video_family != ~0u)
        qci[nq++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = g_vk.video_family,
                                              .queueCount = 1,
                                              .pQueuePriorities = prio};
    if (g_dlss) {
        /* DLSS's device extensions, which Streamline names at run time, go into the choice past snx_vk_choice_enable:
           no vk_ext_* setting names them, and the player turns them off with DLSS (dlss_runtime, render_upscaler) */
        const char* why = "it needs Vulkan 1.3";
        if (!core13 || !snx_dlss_device(g_vk.phys, c->extensions, &c->extension_count, SNX_VK_CHOICE_EXTENSIONS, &f12,
                                        &q12, &f13, &q13, &why)) {
            if (!core13) snx_dlss_stop();
            dlss_off(why);
        }
    }
    if (!snx_vk_choice_create(c, chain, &g_vk.feat, qci, nq)) return false;
    g_vk.gfx_queues = second ? 2 : 1;
    g_vk.feat11 = f11;
    g_vk.feat12 = f12;
    g_vk.feat13 = f13;
    g_vk.feat11.pNext = &g_vk.feat12;
    g_vk.feat12.pNext = core13 ? &g_vk.feat13 : NULL;
    g_vk.feat13.pNext = NULL;
    g_vk.feat2 = (VkPhysicalDeviceFeatures2){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &g_vk.feat11, .features = g_vk.feat};
    d->core13 = core13;
    d->second = second;
    d->sparse_queue = sparse_queue;
    d->store_none_ext = store_none_ext;
    return true;
}

static void pstate_say(const char* line, int warn) {
    if (warn)
        WARN("vulkan: %s\n", line);
    else
        LOG("vulkan: %s\n", line);
}

/* The GPU's clocks held while the game runs, as gpu_pstate asks (common/gpu/vk/gpu_pstate.h), on the render node the
   driver names where the device lists VK_EXT_physical_device_drm (has_drm). A value the setting doesn't take stops the
   game here, as a player who wrote gpu_pstate=of would otherwise play with what they meant to turn off */
static void gpu_pstate_hold(int mode, int has_drm, const char* boost_lacks) {
    VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
    if (has_drm) {
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    }
    SnxGpuPstateDevice d = {g_vk.props.deviceName,
                            g_vk.props.vendorID,
                            (int)g_vk.props.deviceType,
                            drm.hasRender,
                            drm.renderMajor,
                            drm.renderMinor,
                            "/sys",
                            "/dev",
                            boost_lacks};
    snx_gpu_pstate_hold(mode, &d, pstate_say);
}

/* The device, on the device device_rating rates highest (choose_device), made again once without an extension the
   first one's driver lacked a command of (common/gpu/vk/device.h), and what the renderer sets up on it. 0 with the
   reason in g_vk.start_failure */
static int create_device(void) {
    /* the GPU's clocks held (gpu_pstate_hold): read here, as an NVIDIA device is made with what holds them */
    int pstate = snx_gpu_pstate_mode(g_settings.gpu_pstate);
    if (pstate < 0) fatal("settings: gpu_pstate=%s is not auto, on or off", g_settings.gpu_pstate);
    DeviceChoice d = {.vk = g_vk, .translate = g_spirv_translate, .pstate = pstate};
    VkExtensionSetting settings[VK_EXTENSION_SETTINGS];
    char none[160], err[1024];
    snprintf(none, sizeof none,
             "no Vulkan 1.3 device, or 1.2 with the extensions 1.3 took in, with a suitable %s queue",
             g_opt_headless ? "graphics" : "presentation");
    SnxVkDeviceRequest request = {.who = "vulkan",
                                  .none = none,
                                  .instance = g_vk.instance,
                                  .api = VK_API_VERSION_1_2,
                                  .queues = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                                  .surface = g_opt_headless ? VK_NULL_HANDLE : g_vk.surface,
                                  .score = device_rating,
                                  .choose = choose_device,
                                  .user = &d,
                                  .needs_of = device_needs,
                                  .say = ban_say};
    snx_vk_device_settings(&request, settings);
    SnxVkDevice made;
    if (!snx_vk_device_make(&request, &made, err, sizeof err)) {
        /* a table that does not read is the build's fault, and which bans hold would be unknown; a setting that does
           not read stops the game, as the player meant a ban, a hidden command or an extension's value the run would
           otherwise leave as it is */
        if (made.refused == SNX_VK_REFUSED_TABLE) fatal("vulkan: %s", err);
        if (made.refused == SNX_VK_REFUSED_SETTING) fatal("settings: %s", err);
        WARN("vulkan: %s\n", err);
        /* each device and what it lacks, where the choice said nothing of its own */
        if (made.passed_over && !g_vk.start_failure[0]) start_failure_devices();
        if (!g_vk.start_failure[0]) snprintf(g_vk.start_failure, sizeof g_vk.start_failure, "%s", err);
        return 0;
    }
    g_vk.dev = made.device;
    memcpy(g_vk.dev_ext, made.extensions, made.extension_count * sizeof *made.extensions);
    g_vk.dev_ext_n = made.extension_count;
    const int core13 = d.core13, second = d.second, sparse_queue = d.sparse_queue;
    const char* store_none_ext = d.store_none_ext;
    vk_bans_features_say(ban_say); /* the known problems of features, which no ban turns off */
    const char* lacks_command = NULL;
    if (!core13 && !vk12_commands(&lacks_command)) {
        WARN("vulkan: %s enabled the extensions 1.3 took in, but has no %s\n", g_vk.props.deviceName, lacks_command);
        snprintf(g_vk.start_failure, sizeof g_vk.start_failure,
                 "%s has Vulkan 1.2 and the extensions 1.3 took in, but its driver has no %s", g_vk.props.deviceName,
                 lacks_command);
        vkDestroyDevice(g_vk.dev, NULL);
        g_vk.dev = VK_NULL_HANDLE;
        return 0;
    }
    if (g_vk.has_checkpoints) vk_ck_init();
    pm_lock_announce(&g_vklock, "vulkan"); /* the device's calls, every thread's */
    vkGetDeviceQueue(g_vk.dev, g_vk.gfx_family, 0, &g_vk.gfx);
    vkGetDeviceQueue(g_vk.dev, g_vk.compute_family, second ? 1 : 0, &g_vk.compute);
    if (g_dlss) {
        /* DLSS records into the frame's command buffers and asks for no queue of its own: the graphics queue's
           family is the one it is told of, for both */
        const char* why;
        if (snx_dlss_attach(g_vk.instance, g_vk.phys, g_vk.dev, g_vk.gfx_family, 0, g_vk.gfx_family, 0, &why))
            dlss_hook();
        else
            dlss_off(why);
    }
    vk_mem_init(sparse_queue);
    /* the renderer's own allocator (devmem.c): its blocks are 64 MiB at most, and no more than the cap */
    if (!vk_devmem_init(g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support || g_vk.has_copy_indirect ||
                        g_vk.has_inplace_loads || g_vk.has_gbuf_address))
        return 0;
    atomic_store_explicit(&g_devmem_ready, 1, memory_order_release);
    { /* a GPU of 4 GiB or less: memory held back for render targets (devmem.c vk_devmem_reserve) */
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        u64 most = 0;
        for (u32 h = 0; h < mp.memoryHeapCount; h++)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > most)
                most = mp.memoryHeaps[h].size;
        /* the memory types a depth target may take */
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_D32_SFLOAT,
                                .extent = {64, 64, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkImage probe;
        if (g_vk.mem_model != VK_MEM_UMA && most <= (4ull << 30) &&
            vkCreateImage(g_vk.dev, &ii, NULL, &probe) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(g_vk.dev, probe, &req);
            vkDestroyImage(g_vk.dev, probe, NULL);
            vk_devmem_reserve(64ull << 20, req.memoryTypeBits);
        }
    }
    if (g_vk.has_descriptor_heaps && !vk_descriptor_heaps_init(&g_vk.descriptor_heap_props)) {
        g_vk.has_descriptor_heaps = 0;
        g_vk.has_descriptor_heap_support = 0;
    }
    LOG("vulkan: graphics descriptors use %s%s\n", g_vk.has_descriptor_heaps ? "heaps" :
        g_vk.has_descriptor_buffers ? "buffers" : "sets",
        g_vk.has_descriptor_heaps ? (g_vk.has_descriptor_buffers ? " with buffer fallback" : " with set fallback") : "");
    vk_uma_init();
    astc_choose();
    vk_blocks_say();
    {
        /* which driver runs, by its own name: the system's, or a package's (gpu_driver) */
        VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &pp);
        g_vk.driver_id = (u32)dp.driverID;
        snprintf(g_vk.driver_name, sizeof g_vk.driver_name, "%s", dp.driverName);
        LOG("vulkan: the driver that runs: %s, %s (driver ID %d)\n", dp.driverName, dp.driverInfo, (int)dp.driverID);
    }
    gpu_pstate_hold(pstate, d.has_drm, d.boost_lacks);
    if (g_vk.api_cap == VK_API_VERSION_1_1)
        LOG("vulkan: vk_api=1.1 on a Vulkan %u.%u device, which runs as auto would: the setting takes in only a 1.1 "
            "one\n",
            VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion));
    else if (g_vk.api_cap && g_vk.props.apiVersion >= VK_API_VERSION_1_3)
        LOG("vulkan: vk_api=1.2 runs the Vulkan 1.2 way on this %u.%u device\n",
            VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion));
    else if (g_vk.api_cap)
        LOG("vulkan: vk_api=1.2 on a Vulkan %u.%u device, which runs the 1.2 way as auto would\n",
            VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion));
    if (core13)
        LOG("vulkan: the Vulkan 1.3 way: synchronization2, dynamic rendering and the extended dynamic state are core\n");
    else {
        char names[512];
        size_t at = 0;
        for (u32 i = 0; i < VK12_EXTENSIONS && at < sizeof names; i++)
            at += (size_t)snprintf(names + at, sizeof names - at, "%s, ", vk12_extensions[i]);
        LOG("vulkan: the Vulkan 1.2 way: what 1.3 has from %s%s, shaders in SPIR-V 1.5, no Vulkan Video decode\n",
            names, store_none_ext);
    }
    LOG("vulkan: %s, Vulkan %u.%u.%u, driver %08x, graphics queue family %u, compute %u%s\n", g_vk.props.deviceName,
        VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
        VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.props.driverVersion, g_vk.gfx_family, g_vk.compute_family,
        g_vk.compute_family != g_vk.gfx_family ? " (separate family)"
        : second                               ? " (a second queue)"
                                               : " (the graphics queue)");
    /* A draw that writes a target it samples at its own pixel gets the value from before the draw on the console.
       In place, a later primitive of the draw can read what an earlier one wrote. With fragment shader interlock, a
       capture in an interlock at the program's start saves the old pixel (vk/predraw.c): the drivers that take it
       are the extension bans' prefer lines on VK_FEEDBACK_CAPTURE, each with its measure, as on some the interlock
       waits on more than the fragments that overlap. On the others with dynamic rendering local read, the renderer
       draws the draw two times in one pass (render.c predraw_run), set up three times on the CPU (the draw that
       finds it, the save draw, the guest's), with one by-region barrier. Without both, the draw stays in place: a
       copy of the target before each such draw costs too much (5 ms a frame outdoors on a desktop GPU) */
    const int prefer = vk_prefer(VK_FEEDBACK_CAPTURE, g_vk.has_fragment_interlock, ban_say) != 0;
    g_vk.feedback_mode = vk_feedback_choice(prefer, g_vk.has_feedback, g_vk.has_fragment_interlock,
                                            g_vk.feat.fragmentStoresAndAtomics, g_vk.has_local_read);
    LOG("vulkan: a draw that samples a target it writes at its own pixel %s (%s, fragment shader interlock %s, "
        "dynamic rendering local read %s)\n",
        g_vk.feedback_mode == VK_FEEDBACK_INTERLOCK ? "reads the pixel as it was before the draw, from a capture in "
                                                      "a fragment interlock"
        : g_vk.feedback_mode == VK_FEEDBACK_PREDRAW ? "is drawn two times, the first saving the target"
                                                    : "reads the target in place",
        g_vk.feedback_mode == VK_FEEDBACK_INTERLOCK ? "the extension bans prefer the interlock on this driver"
        : prefer ? "the extension bans prefer the interlock on this driver, without all the capture needs here: two "
                   "draws where it can"
                 : "no prefer line of the extension bans on " VK_FEEDBACK_CAPTURE " holds here: two draws where it can",
        g_vk.has_fragment_interlock ? "yes" : "no", g_vk.has_local_read ? "yes" : "no");
    if (g_vk.has_feedback)
        LOG("vulkan: draws sample the color targets they render to in place (attachment feedback loops, %s, %s)\n",
            g_vk.has_feedback_dynamic ? "enabled by dynamic state" : "enabled per pipeline",
            g_vk.has_local_read ? "ordered by barriers within the pass" : "ordered by beginning the pass again");
    else
        LOG("vulkan: draws that sample a color target they render to read a copy of it (no attachment feedback "
            "loops)\n");
    vk_bans_unclaimed(ban_say); /* a ban on something nothing here asked about, said rather than dropped */
    return 1;
}

/* the swapchain destroyed. Its images' semaphores: a present may still wait on one after the device is idle (the
   presentation engine isn't covered by vkDeviceWaitIdle), so they go a swapchain later, when those presents are long
   done */
static void swapchain_retire(void) { /* sc_lock held */
    static VkSemaphore retired[VK_SC_MAX];
    static u32 nretired;
    for (u32 i = 0; i < nretired; i++) vkDestroySemaphore(g_vk.dev, retired[i], NULL);
    nretired = 0;
    for (u32 i = 0; i < g_vk.sc_count; i++) retired[nretired++] = g_vk.sc_done[i];
    vk_ui_swapchain_gone();
    vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    g_vk.sc_clock = VK_CLOCK_NONE;
    g_vk.sc_hold = 0;
    g_vk.sc_count = 0;
}
/* vkDeviceWaitIdle, which takes every queue: those of the decoder's (FFmpeg's) are excluded by vk_lock, held here */
static void device_wait_idle(void) {
    vk_queue_lock(g_vk.gfx);
    if (g_vk.compute != g_vk.gfx) vk_queue_lock(g_vk.compute);
    vkDeviceWaitIdle(g_vk.dev);
    if (g_vk.compute != g_vk.gfx) vk_queue_unlock(g_vk.compute);
    vk_queue_unlock(g_vk.gfx);
}

/* the window's surface made anew, its swapchain to follow: Android destroys a window's surface while the app is in
   the background, and the Vulkan surface on it is lost for good */
int vk_surface_recreate(void) {
    vk_rec_sync(); /* the device wait covers only what was submitted */
    device_wait_idle();
    pm_lock(&g_vk.sc_lock);
    if (g_vk.swapchain) swapchain_retire();
    pm_unlock(&g_vk.sc_lock);
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        VkBool32 present = VK_FALSE; /* the queue chosen for the old surface, asked again for the new one */
        if (vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.gfx_family, g_vk.surface, &present) == VK_SUCCESS &&
            present)
            return 1;
        WARN_LIMIT(4, "vulkan: the window's new surface can't be presented to from the graphics queue\n");
        return 0;
    }
    WARN_LIMIT(4, "vulkan: the window's surface could not be made again\n");
    return 0;
}

/* the window's release waits this long for each thing another thread holds */
#define RELEASE_WAIT_NS 2000000000ull
/* a queue's lock, or with q VK_NULL_HANDLE the swapchain's, taken by until (plat_ns): 0 when another thread held it
   all that while */
static int release_lock_by(VkQueue q, u64 until) {
    while (q ? !vk_queue_trylock(q) : !pm_trylock(&g_vk.sc_lock)) {
        if (plat_ns() >= until) return 0;
        plat_sleep_ns(1000000);
    }
    return 1;
}
/* The window let go for a fatal error's screen (sdl3.c), from the thread that had the error or from the main thread
   that shows it. No wait here outlasts RELEASE_WAIT_NS: a thread that holds the renderer and doesn't let go (stuck on a
   lost device), or the recording thread, which an error or a crash stops inside a packet for good (its ring never
   drains, and a submit or a present it was in keeps the queue's lock or the swapchain's), must not keep the error
   unseen: a wait with no end on the main thread freezes the window with no error shown */
int vk_window_release(void) {
    u64 until = plat_ns() + RELEASE_WAIT_NS;
    const char* held = NULL;
    int released = 0;
    while (!vk_trylock()) {
        if (plat_ns() >= until) {
            WARN("vulkan: thread %u has held the renderer for %llu s: the window is not let go for the error's "
                 "screen\n",
                 vk_lock_owner(), RELEASE_WAIT_NS / 1000000000ull);
            return 0;
        }
        plat_sleep_ns(10000000);
    }
    until = plat_ns() + RELEASE_WAIT_NS; /* the ring's own while, whatever the lock took */
    if (!vk_rec_sync_until(until))
        WARN("vulkan: the recording thread has not finished its commands in %llu s: the window is let go without "
             "them\n",
             RELEASE_WAIT_NS / 1000000000ull);
    /* device_wait_idle's locks, then the swapchain's */
    if (!release_lock_by(g_vk.gfx, until))
        held = "graphics queue";
    else {
        if (g_vk.compute != g_vk.gfx && !release_lock_by(g_vk.compute, until))
            held = "compute queue";
        else {
            vkDeviceWaitIdle(g_vk.dev); /* a lost device answers at once */
            if (g_vk.compute != g_vk.gfx) vk_queue_unlock(g_vk.compute);
        }
        vk_queue_unlock(g_vk.gfx);
    }
    if (!held && !release_lock_by(VK_NULL_HANDLE, until)) held = "swapchain";
    if (!held) {
        if (g_vk.swapchain) swapchain_retire();
        pm_unlock(&g_vk.sc_lock);
        if (g_vk.surface) vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
        g_vk.surface = VK_NULL_HANDLE;
        released = 1;
    } else
        WARN("vulkan: another thread has held the %s's lock for %llu s: the window is not let go for the error's "
             "screen\n",
             held, RELEASE_WAIT_NS / 1000000000ull);
    /* every hold of this thread's at once: it won't return to the callers that took them */
    t_vkdepth = 0;
    vk_let_go();
    return released;
}

/* The surface's formats read for a game's HDR output: whether a game is told the display is HDR (g_vk.hdr_display,
   said in the log with hdr_game on, each time it changes), and for frames of an HDR space the image that takes them.
   0 when there is none, and for an SDR frame */
static int hdr_surface(u32 space, VkSurfaceFormatKHR* fmt) {
    static int said = -1;
    VkSurfaceFormatKHR* formats = NULL;
    const char* why;
    u32 n = 0;
    int on, found;
    if (g_settings.game_hdr && g_vk.has_swapchain_colorspace && g_vk.has_hdr_metadata &&
        vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &n, NULL) == VK_SUCCESS && n) {
        formats = calloc(n, sizeof *formats);
        if (!formats || vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &n, formats) < 0) n = 0;
    }
    on = vk_hdr_display(g_settings.game_hdr, g_vk.has_swapchain_colorspace, g_vk.has_hdr_metadata, formats, n, &why);
    if (g_settings.game_hdr && on != said) {
        if (on)
            LOG("vulkan: hdr_game=1: a game is told the display is HDR, and its HDR10 and scRGB frames are shown as "
                "they are\n");
        else
            LOG("vulkan: hdr_game=1, and a game is told the display is SDR: %s\n", why);
        said = on;
    }
    atomic_store_explicit(&g_vk.hdr_display, on, memory_order_relaxed);
    found = on && vk_hdr_surface_format(space, formats, n, fmt);
    if (space != SNX_HDR_SPACE_SRGB && !on) /* on: the surface lists both spaces' formats */
        LOG_LIMIT(4, "vulkan: %s frames are shown on an SDR swapchain, their values as they are: %s\n",
                  space == SNX_HDR_SPACE_PQ ? "HDR10" : "scRGB", why);
    free(formats);
    return found;
}

void vk_swapchain_failure(char* out, size_t size, const char* device, const char* driver, VkResult r) {
    snprintf(out, size, "the swapchain could not be made on %s (%s): %s (%d)", device,
             driver[0] ? driver : "its driver", snx_vk_result_str(r), (int)r);
}
int vk_swapchain_create(int kind) {
    if (g_opt_headless) return 0; /* no WSI calls, even on a resize/recreate path */
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (cr != VK_SUCCESS) { /* asked again on each present while it fails: said a few times */
        WARN_LIMIT(4, "vulkan: the window's surface could not be queried: %s (%d)\n", snx_vk_result_str(cr), (int)cr);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        WARN("vulkan: the window's surface can't be drawn to by copies, which presenting needs\n");
        return 0;
    }
    int ww, wh; /* what presents compare against, whatever the surface makes of it */
    backend_window_size(&ww, &wh);
    /* Wayland: the swapchain decides, so the window's size */
    VkExtent2D ext = snx_vk_swap_extent(&caps, (VkExtent2D){ww > 0 ? (u32)ww : 0, wh > 0 ? (u32)wh : 0});
    if (!ext.width || !ext.height) return 0;

    /* 8-bit, in the encoding of the frames presented: a blit from an sRGB target to a UNORM image decodes the bytes,
       where the console's display shows them as they are. A game's HDR frames have an image of their own space, where
       the display takes them (hdr_surface) */
    static const VkFormat srgb_formats[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    static const VkFormat unorm_formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkSurfaceFormatKHR fmt;
    u32 space = kind == VK_SC_PQ ? SNX_HDR_SPACE_PQ : kind == VK_SC_SCRGB ? SNX_HDR_SPACE_SCRGB : SNX_HDR_SPACE_SRGB;
    int srgb = kind == VK_SC_SRGB;
    if (!hdr_surface(space, &fmt)) {
        space = SNX_HDR_SPACE_SRGB;
        if (!(srgb && snx_vk_surface_format(g_vk.phys, g_vk.surface, srgb_formats, 2, &fmt)))
            snx_vk_surface_format(g_vk.phys, g_vk.surface, unorm_formats, 2, &fmt);
    }
    if (srgb && fmt.format != VK_FORMAT_B8G8R8A8_SRGB && fmt.format != VK_FORMAT_R8G8B8A8_SRGB) {
        LOG_LIMIT(1, "vulkan: the window has no sRGB format: frames in sRGB show darker\n");
    }

    /* vsync=1: FIFO, which every device has. Otherwise mailbox, else immediate, else FIFO all the same. Read once: the
       main thread's .ini reload writes it */
    int vsync = atomic_load_explicit((_Atomic int*)&g_settings.vsync, memory_order_relaxed);
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, NULL);
    VkPresentModeKHR* ms = calloc(nm ? nm : 1, sizeof *ms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nm, ms);
    VkPresentModeKHR mode = snx_vk_present_mode(ms, nm, vsync);
    free(ms);

    /* an image for each frame in flight beyond what the driver keeps: a frame's present waits in the recording ring
       (vk_rec_present) while the next frame acquires, which takes no wait, so with one image fewer it found none.
       Where the game's pace waits for the display (frame_pacing, FIFO with a display clock), a frame is released only
       once the one before is displayed, so frames don't queue for the display: one image more than the driver keeps
       is enough, and more would only let frames that came early (a menu's, fps_limit's) queue */
    int clock = g_vk.has_present_wait || g_vk.has_display_timing;
    u32 extra = clock && mode == VK_PRESENT_MODE_FIFO_KHR &&
                        atomic_load_explicit((_Atomic int*)&g_settings.frame_pacing, memory_order_relaxed)
                    ? 1
                    : VK_FRAMES;
    u32 count = snx_vk_swap_image_count(&caps, extra, VK_SC_MAX);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) /* the lowest one it has */
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    /* frames are drawn upright: a display held other than its own way (a phone held wide) has its compositor turn
       them, where its current transform would say they come turned already */
    VkSurfaceTransformFlagBitsKHR transform = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                  : caps.currentTransform;
    VkSwapchainKHR old = g_vk.swapchain;
    if (old) {
        vk_rec_sync();
        device_wait_idle();
    }
    vk_rec_present_result(); /* the old swapchain's presents, all made (a retire drained the ring), say nothing of it */
    /* frames held until a time (display.c), under FIFO where the surface can: present ids and waits 2 with it */
    int timing = 0;
    VkPresentStageFlagsEXT stages = 0;
    if (g_vk.has_present_timing && mode == VK_PRESENT_MODE_FIFO_KHR) {
        VkPresentTimingSurfaceCapabilitiesEXT ct = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
        VkSurfaceCapabilitiesPresentId2KHR cid = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,
                                                  .pNext = &ct};
        VkSurfaceCapabilitiesPresentWait2KHR cwait = {
            .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR, .pNext = &cid};
        VkSurfaceCapabilities2KHR c2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &cwait};
        VkPhysicalDeviceSurfaceInfo2KHR si2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                               .surface = g_vk.surface};
        timing = vkGetPhysicalDeviceSurfaceCapabilities2KHR(g_vk.phys, &si2, &c2) == VK_SUCCESS &&
                 ct.presentTimingSupported && ct.presentAtAbsoluteTimeSupported && cid.presentId2Supported &&
                 cwait.presentWait2Supported;
        stages = ct.presentStageQueries;
    }
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .flags = timing ? VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR |
                                                         VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR |
                                                         VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT
                                                   : 0,
                                   .surface = g_vk.surface,
                                   .minImageCount = count,
                                   .imageFormat = fmt.format,
                                   .imageColorSpace = fmt.colorSpace,
                                   .imageExtent = ext,
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, /* ui.c's pass draws it all */
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = transform,
                                   .compositeAlpha = alpha,
                                   .presentMode = mode,
                                   .clipped = VK_TRUE,
                                   .oldSwapchain = old};
    /* a swapchain that takes a sleep mode, for the boost below (gpu_pstate_hold) */
    VkSwapchainLatencyCreateInfoNV latency = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
                                              .latencyModeEnable = VK_TRUE};
    if (g_vk.has_low_latency2) ci.pNext = &latency;
    /* old is retired by the create, made or not: on any failure it goes too, and the next present starts anew */
    VkSwapchainKHR sc;
    VkSemaphore done[VK_SC_MAX];
    u32 nimg = 0, ndone = 0;
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    pm_lock(&g_vk.sc_lock); /* until the new swapchain is g_vk's: the old one is retired from here */
    cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &sc);
    if (cr != VK_SUCCESS) { /* tried again on each present while it fails: said a few times */
        char said[sizeof g_vk.start_failure];
        vk_swapchain_failure(said, sizeof said, g_vk.props.deviceName, g_vk.driver_name, cr);
        WARN_LIMIT(4, "vulkan: %s\n", said);
        /* the first one, at the start, is why the run cannot start (renderer_failed) */
        if (!g_vk.swapchain && !old && !g_vk.start_failure[0])
            snprintf(g_vk.start_failure, sizeof g_vk.start_failure, "%s", said);
        if (cr == VK_ERROR_SURFACE_LOST_KHR) g_vk.sc_lost = 1;
        if (old) swapchain_retire();
        pm_unlock(&g_vk.sc_lock);
        return 0;
    }
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &nimg, NULL);
    if (nimg > VK_SC_MAX) WARN("vulkan: the swapchain has %u images, more than the %d taken\n", nimg, VK_SC_MAX);
    while (nimg <= VK_SC_MAX && ndone < nimg && VK_OK(vkCreateSemaphore(g_vk.dev, &si, NULL, &done[ndone]))) ndone++;
    if (nimg > VK_SC_MAX || ndone < nimg) {
        while (ndone) vkDestroySemaphore(g_vk.dev, done[--ndone], NULL);
        vkDestroySwapchainKHR(g_vk.dev, sc, NULL);
        if (old) swapchain_retire();
        pm_unlock(&g_vk.sc_lock);
        return 0;
    }
    if (old) swapchain_retire();
    g_vk.swapchain = sc;
    if (g_vk.has_low_latency2) {
        /* the boost alone: no sleep is asked, so its low latency mode stays off and the game keeps its own pace */
        VkLatencySleepModeInfoNV boost = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
                                          .lowLatencyMode = VK_FALSE,
                                          .lowLatencyBoost = VK_TRUE};
        VkResult br = vkSetLatencySleepModeNV(g_vk.dev, sc, &boost);
        if (br != VK_SUCCESS)
            WARN("vulkan: gpu_pstate: vkSetLatencySleepModeNV refused the swapchain's boost: %s (%d): the driver "
                 "chooses the clocks\n",
                 snx_vk_result_str(br), (int)br);
        else if (!g_vk.sc_made)
            LOG("vulkan: gpu_pstate: the swapchain asks the GPU's boost (vkSetLatencySleepModeNV, lowLatencyBoost)\n");
    }
    /* the times presents are held until: the system's CLOCK_MONOTONIC (plat_monotonic_ns) if the swapchain has it,
       else a present stage's own clock, calibrated against CLOCK_MONOTONIC at each present (display.c), the stage
       the first pixel goes out at if it can be. VK_GOOGLE_display_timing's are CLOCK_MONOTONIC's, under FIFO */
    g_vk.sc_clock = timing                    ? VK_CLOCK_WAIT2
                    : g_vk.has_present_wait   ? VK_CLOCK_WAIT
                    : g_vk.has_display_timing ? VK_CLOCK_GOOGLE
                                              : VK_CLOCK_NONE;
    g_vk.sc_hold = g_vk.sc_clock == VK_CLOCK_GOOGLE && mode == VK_PRESENT_MODE_FIFO_KHR && plat_monotonic_ns();
    g_vk.sc_time_stage = 0;
    if (timing && plat_monotonic_ns()) {
        VkTimeDomainKHR dom[16];
        u64 ids[16], counter = 0;
        VkSwapchainTimeDomainPropertiesEXT tp = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
                                                 .timeDomainCount = 16,
                                                 .pTimeDomains = dom,
                                                 .pTimeDomainIds = ids};
        VkResult tr = vkGetSwapchainTimeDomainPropertiesEXT(g_vk.dev, sc, &tp, &counter);
        for (u32 i = 0; (tr == VK_SUCCESS || tr == VK_INCOMPLETE) && i < tp.timeDomainCount && i < 16; i++)
            if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) {
                g_vk.sc_hold = 1;
                g_vk.sc_time_domain = ids[i];
                g_vk.sc_time_stage = 0;
            } else if (dom[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT && !g_vk.sc_hold && stages &&
                       g_vk.has_calibrated_mono) {
                g_vk.sc_hold = 1;
                g_vk.sc_time_domain = ids[i];
                g_vk.sc_time_stage = stages & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
                                         ? VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
                                         : stages & -stages;
            }
    }
    pm_unlock(&g_vk.sc_lock);
    vk_display_fifo(mode == VK_PRESENT_MODE_FIFO_KHR);
    g_vk.sc_extent = ext;
    g_vk.sc_win_w = ww;
    g_vk.sc_win_h = wh;
    g_vk.sc_srgb = fmt.format == VK_FORMAT_B8G8R8A8_SRGB || fmt.format == VK_FORMAT_R8G8B8A8_SRGB;
    g_vk.sc_space = space;
    g_vk.sc_made++;
    g_vk.sc_format = fmt.format;
    g_vk.sc_turned = transform != caps.currentTransform;
    g_vk.sc_vsync = vsync;
    g_vk.sc_count = nimg;
    vkGetSwapchainImagesKHR(g_vk.dev, sc, &g_vk.sc_count, g_vk.sc_images);
    memcpy(g_vk.sc_done, done, sizeof done[0] * nimg);
    LOG("vulkan: swapchain %ux%u, %u images%s%s, %s\n", ext.width, ext.height, g_vk.sc_count,
        g_vk.sc_srgb                           ? ", sRGB"
        : g_vk.sc_space == SNX_HDR_SPACE_PQ    ? ", HDR10 (PQ, BT.2020)"
        : g_vk.sc_space == SNX_HDR_SPACE_SCRGB ? ", scRGB (linear, 1.0 at 80 nits)"
                                               : "",
        g_vk.sc_hold    ? ", presents timed and held until they are due"
        : g_vk.sc_clock ? ", presents timed"
                        : "",
        mode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
        : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                : "fifo");
    return 1;
}

/* a start that failed once the window had its surface: the surface goes. On Android it holds the window, which takes
   one producer at a time, and the error's own screen (backend/sdl3.c's fatal_screen) could not draw in it: the system's
   plain box said the error instead */
static int start_failed(void) {
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    return 0;
}

const char* vk_start_failure(void) { return g_vk.start_failure; }

int vk_init(void) {
    char why[64];
    u32 cap;
    /* the instance is made for the version the setting allows, so it is read before. A value it takes not stops the
       game, as a player who wrote vk_api=1.3 would otherwise play the way they meant to leave */
    if (!vk_api_setting(g_settings.vulkan_api, &cap, why, sizeof why)) fatal("settings: %s", why);
    if (cap) g_vk.api_cap = cap;
    pm_init(&g_vklock);
    pm_init(&g_vk.sc_lock);
    pm_lock_announce(&g_vk.sc_lock, "vk swapchain"); /* the game's threads and the GPU thread around present */
    if (g_opt_headless) {
        /* Load libvulkan/vulkan-1 directly, or the driver the program's folders carry (an Apple app's MoltenVK): no SDL
           video driver or headless-surface extension is needed. */
        void* own = backend_vk_driver();
        if (own)
            volkInitializeCustom((PFN_vkGetInstanceProcAddr)own);
        else if (!VK_OK(volkInitialize()))
            return 0;
    } else {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)backend_vk_loader();
        if (!gipa) {
            WARN("vulkan: no Vulkan loader (is a Vulkan driver installed?)\n");
            return 0;
        }
        volkInitializeCustom(gipa);
    }
    vk_renderdoc_load(); /* its capture layer is enabled when the instance is created */
    if (!create_instance()) return 0;
    backend_vk_check(); /* the instance opened the driver: the one asked for, or the run ends */
    if (!g_opt_headless && !backend_vk_surface(g_vk.instance, &g_vk.surface)) {
        snprintf(g_vk.start_failure, sizeof g_vk.start_failure, "the window backend could not create a Vulkan surface");
        WARN("vulkan: %s\n", g_vk.start_failure);
        return 0;
    }
    if (!create_device()) return start_failed();
    return g_opt_headless || vk_swapchain_create(0) ? 1 : start_failed();
}
