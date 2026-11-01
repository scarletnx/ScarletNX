/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/textures/decoders.cpp, src/video_core/textures/decoders.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The GPU's host side: guest memory as the GPU sees it, the block linear layout, and the GPU thread's queue.

   The GPU thread runs the calls queued for it (gpu_call) one at a time, in order: NVN's replay of a queue's
   submissions (gpu/nvn/nvn_replay.c), its copies, and vi's presents. A game pause holds the thread between calls
   (gpu_hold) */
#include "nx/runtime/state_fns.h"
#include "nx/runtime/state_io.h"
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/memory/own.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"

void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len);
void (*g_gpu_before_write)(u64 gva, u64 size);
int g_gpu_releases;
void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
void (*g_gpu_mapping_end)(void);

/* ------------------------------------------------------------ guest memory through the GPU's address space */
#define GMEM_PAGE 0x1000u /* the granule unmapped memory and memory states are walked in: a 4 KiB page */
/* a memory state's type, bits 7:0 (Switchbrew's MemoryState): the rest are its attribute flags */
#define MEM_STATE_TYPE_MASK 0xFFu
/* the runs of guest memory behind [gva, gva + size), copied in or out: 1 when every page was mapped. An unmapped
   page, or one the guest freed after mapping it here (gmmu_run), reads as zero and takes no write. What the GPU holds
   of a run lands first (memory/own.c): the caller may hold the renderer's lock, and a trap can't be answered then.
   A read lands only bytes it reads: what the GPU holds beside them on their pages stays there (a shader's constant
   buffer words beside a copy engine's result, in one game every frame) */
static int copy_runs(u64 gva, u8* in, const u8* out, u64 size) {
    int all = 1;
    while (size) {
        u64 cpu = 0, n = gmmu_run(gva, size, &cpu);
        if (!n) {
            n = GMEM_PAGE - (gva & (GMEM_PAGE - 1));
            if (n > size) n = size;
            if (in) memset(in, 0, (size_t)n);
            all = 0;
            goto next;
        }
        if (__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) {
            /* a read none of whose bytes are the GPU's: own_stage copied them, through the mirror where a page is
               trapped */
            if (in && !own_stage(cpu, n, in, NULL, 0)) goto next;
            own_acquire(cpu, n);
        }
        if (in)
            memcpy(in, GP(cpu), (size_t)n);
        else
            memcpy(GP(cpu), out, (size_t)n);
    next:
        if (in)
            in += n;
        else
            out += n;
        gva += n;
        size -= n;
    }
    return all;
}
void gmem_read_raw(u64 gva, void* dst, u64 size) { copy_runs(gva, (u8*)dst, NULL, size); }
void gmem_write_raw(u64 gva, const void* src, u64 size) { copy_runs(gva, NULL, (const u8*)src, size); }
/* whether anything the renderer holds may cover [gva, gva + size): one lookup where each hook below makes its own */
static int mem_pending(u64 gva, u64 size) { return !g_renderer->mem_pending || g_renderer->mem_pending(gva, size); }
void gmem_need(u64 gva, u64 size) {
    if ((__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) || g_cs_any || g_gfx_any || g_gfx_img_any) &&
        !mem_pending(gva, size))
        return;
    if (!size) return;
    if (g_cs_any && g_renderer->cs_mem_need) g_renderer->cs_mem_need(gva, size);
    if ((g_gfx_any || g_gfx_img_any) && g_renderer->gfx_mem_need) g_renderer->gfx_mem_need(gva, size);
    if (__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) && g_renderer->release_need)
        g_renderer->release_need(gva, size);
}
/* the renderer's hook (vkr_before_write) makes a release queued over the bytes happen first, with everything else
   pending there */
static void gmem_before_write(u64 gva, u64 size) {
    if (size && g_gpu_before_write) g_gpu_before_write(gva, size);
}
int gmem_read(u64 gva, void* dst, u64 size) {
    if ((__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) || g_cs_any || g_gfx_any || g_gfx_img_any) &&
        !mem_pending(gva, size))
        return copy_runs(gva, (u8*)dst, NULL, size) ? 0 : -1;
    /* a queued release's payload is known: the stream reads it without waiting for the work before it, as the console's
       GPU, in order, would find it there (one game's conditional rendering read report pairs this way, waiting for the
       GPU 60 times a second) */
    if (__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE) && g_renderer->release_read) {
        int r = g_renderer->release_read(gva, dst, size);
        if (r > 0) return 0;
        if (r < 0) g_renderer->release_need(gva, size);
        if (g_cs_any) g_renderer->cs_mem_need(gva, size);
        if (g_gfx_any || g_gfx_img_any) g_renderer->gfx_mem_need(gva, size);
        gmem_read_raw(gva, dst, size);
        return 0;
    }
    gmem_need(gva, size);
    return copy_runs(gva, (u8*)dst, NULL, size) ? 0 : -1;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    int all;
    if (!size) return 0;
    gmem_before_write(gva, size);
    all = copy_runs(gva, NULL, (const u8*)src, size);
    gpu_mem_written(gva, size);
    return all ? 0 : -1;
}
/* the host pointer of [gva, gva + size) when contiguous, what the GPU holds of it landed (memory/own.c): the caller
   reads or writes it on the CPU, maybe with the renderer's lock held */
u8* gmem_ptr_untracked(u64 gva, u64 size) {
    u64 cpu = 0;
    if (!size) size = 1;
    if (gmmu_run(gva, size, &cpu) != size) return NULL;
    if (__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) own_acquire(cpu, size);
    return GP(cpu);
}
u8* gmem_host(u64 gva, u64 size) {
    u64 cpu = 0;
    if (!size) size = 1;
    return gmmu_run(gva, size, &cpu) == size ? GP(cpu) : NULL;
}
u8* gmem_ptr(u64 gva, u64 size) {
    u8* p = gmem_ptr_untracked(gva, size);
    if (p) gmem_need(gva, size ? size : 1);
    return p;
}
u32 gmem_r32(u64 gva) {
    u8 b[4] = {0};
    gmem_read(gva, b, 4);
    return snx_ld_le32(b);
}
void gmem_w32(u64 gva, u32 v) {
    u8 b[4];
    snx_st_le32(b, v);
    gmem_write(gva, b, 4);
}
/* Hardware fetches a program only up to its end, and TotK has compute programs ending next to an uncommitted page:
   the read stops at the first page that has no GPU mapping or no guest memory behind it */
static u64 code_extent(u64 gva, u64 size) {
    MemBlock b;
    int have = 0;
    u64 done = 0;
    memset(&b, 0, sizeof b);
    while (done < size) {
        u64 a = gva + done, n = GMEM_PAGE - (a & (GMEM_PAGE - 1)), cpu = gmmu_translate(a);
        u32 state;
        if (!cpu) break;
        if (!have || cpu < b.addr || cpu - b.addr >= b.size) { /* a block's pages share its state */
            mem_query(cpu, &b);
            have = 1;
        }
        state = b.state & MEM_STATE_TYPE_MASK;
        if (!b.size || state == (MS_FREE & MEM_STATE_TYPE_MASK) || state == (MS_INACCESSIBLE & MEM_STATE_TYPE_MASK))
            break;
        done += n < size - done ? n : size - done;
    }
    return done;
}
void gmem_read_code(u64 gva, void* dst, u64 size) {
    u64 done = code_extent(gva, size);
    if (done) {
        gmem_need(gva, done);
        gmem_read_raw(gva, dst, done);
    }
    memset((u8*)dst + done, 0, (size_t)(size - done));
}
/* A program's window reaches past its end into whatever the game keeps after it, results of the GPU's included: one
   game's compute lookups landed those at every dispatch's first lookup in a submit, a wait for the GPU each. The
   CPU's bytes are read up to the first byte the GPU holds, through the pool's mirror where a page is trapped
   (own_stage), and nothing lands */
u64 gmem_read_code_cpu(u64 gva, void* dst, u64 size) {
    u64 done = code_extent(gva, size), at = 0;
    if (done) gmem_need(gva, done);
    while (at < done) {
        u64 cpu = 0, n = gmmu_run(gva + at, done - at, &cpu), m;
        OwnRange o;
        if (!n) break;
        if (!__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) {
            memcpy((u8*)dst + at, GP(cpu), (size_t)n);
            at += n;
            continue;
        }
        m = own_lookup(cpu, n, &o, 1) ? o.addr - cpu : n;
        if (own_stage(cpu, m, (u8*)dst + at, NULL, 0)) break; /* an owner since: from here on the GPU's */
        at += m;
        if (m < n) break;
    }
    memset((u8*)dst + at, 0, (size_t)(size - at));
    return at;
}
int gmem_code_cut(u64 len, u64 cpu, u64 size) { return cpu < size && cpu < len + 16; }

/* ------------------------------------------------------------ block linear layout (Eden's decoders.cpp) */
/* A byte's offset in the layout (texture/blocklinear.h, docs/BLOCKLINEAR.md) is the sum of an x part and a (y, z)
   part, as in Eden's SwizzleImpl (offset_x + offset_y + offset_z), so a row's base and a column's offset can be
   computed apart. The x part: the block column x falls in (x / 64, blocks are 2^(9 + bh + bd) bytes) and the byte's
   place in its GOB's row */
static u64 bl_x(u32 x, u32 bh, u32 bd) {
    return ((u64)(x >> GOB_SIZE_X_SHIFT) << (GOB_SIZE_SHIFT + bh + bd)) + gob_x_offset(x);
}
/* the (y, z) part: z's slab of 2^bd slices (each rows of blocks, gobs_x blocks to a row), the row of blocks y's GOB
   row gy falls in, the GOB's place in its block (slice z mod 2^bd, then GOB row gy mod 2^bh) and the row's place in
   its GOB */
static u64 bl_yz(u32 y, u32 z, u32 wb, u32 h, u32 bh, u32 bd) {
    u64 gobs_x = bl_gobs_x(wb), gy = y >> GOB_SIZE_Y_SHIFT;
    u64 rows = bl_block_rows(h, bh); /* rows of blocks in a slab */
    u64 block = bl_block_size(bh, bd);
    u64 in_block = ((((u64)z & ((1u << bd) - 1)) << bh) + (gy & ((1u << bh) - 1))) << GOB_SIZE_SHIFT;
    return (((u64)(z >> bd) * rows + (gy >> bh)) * gobs_x) * block + in_block + gob_y_offset(y);
}
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    return bl_x(x_bytes, bh_log2, bd_log2) + bl_yz(y, z, width_bytes, height, bh_log2, bd_log2);
}
/* the guest bytes of a pixel of a color format, for render targets and 2D surfaces alike (nv50's surface formats,
   Eden's RenderTargetFormat values) */
u32 gpu_color_bpp(u32 format) {
    if (format >= 0xC0 && format <= 0xC5) return 16; /* RGBA32 and RGBX32 */
    if (format >= 0xC6 && format <= 0xCE) return 8;  /* RGBA16, RG32, RGBX16 */
    if (format >= 0xE8 && format <= 0xF2) return 2;  /* B5G6R5, BGR5A1, RG8, R16 */
    if (format >= 0xF3 && format <= 0xF7) return 1;  /* R8, A8 */
    if (format == 0xF8 || format == 0xFB || format == 0xFC) return 2;
    return 4;
}

/* ------------------------------------------------------------ what the game can observe */
void gpu_signal_point(void) {
    if (g_gpu_before_write) g_gpu_before_write(0, 0);
}
void gpu_release_write(u64 gva, const void* data, u32 len) {
    if (g_renderer->release_write) {
        g_renderer->release_write(gva, data, len);
        return;
    }
    gpu_signal_point();
    gmem_write(gva, data, len);
}
void gpu_mem_changed(u64 cpu_addr, u64 len) {
    if (g_gpu_mem_changed) g_gpu_mem_changed(cpu_addr, len);
}

/* ------------------------------------------------------------ the queue */
/* A call of the GPU thread's queue. Calls are linked in queue order, and the GPU thread alone takes them out */
typedef struct GpuItem GpuItem;
struct GpuItem {
    GpuItem* next;
    void (*fn)(void*);
    void* arg;
};
#define GPU_THREAD_STACK ((size_t)16 << 20) /* the GPU thread's stack: 16 MiB */
static PMutex g_qlk = PMUTEX_INIT;
static PCond g_qcv;
static int g_qcv_ready, g_qthread;
static GpuItem *g_qhead, *g_qtail;
static int g_hold_want, g_hold_at; /* a game pause asked the GPU thread to hold, and it holds (gpu_hold) */
/* for the stall report: whether this is the GPU thread, whether it runs a call and since when, and the blocking wait
   it is in (gpu_wait_note) */
static _Thread_local int t_gpu;
static int g_run_call;
static u64 g_run_since;
static const char* g_wait_what;
static u64 g_wait_value, g_wait_since;

int gpu_wait_note(const char* what, u64 value) {
    if (!t_gpu) return 0;
    if (what) {
        __atomic_store_n(&g_wait_value, value, __ATOMIC_RELAXED);
        __atomic_store_n(&g_wait_since, plat_ns(), __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_wait_what, what, __ATOMIC_RELEASE);
    return 1;
}
/* read unlocked from the watchdog's thread: each value on its own, which is enough to say where the GPU thread is */
void gpu_report_stall(void) {
    u64 now = plat_ns();
    const char* what = __atomic_load_n(&g_wait_what, __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&g_qthread, __ATOMIC_RELAXED)) {
        WARN("gpu: nothing was queued for the GPU thread yet\n");
        return;
    }
    if (__atomic_load_n(&g_run_call, __ATOMIC_ACQUIRE))
        WARN("gpu: the GPU thread runs a call, for %llu ms\n",
             (unsigned long long)((now - __atomic_load_n(&g_run_since, __ATOMIC_RELAXED)) / 1000000u));
    else
        WARN("gpu: the GPU thread is between calls\n");
    if (what)
        WARN("gpu: it has waited %llu ms for %s (%llx)\n",
             (unsigned long long)((now - __atomic_load_n(&g_wait_since, __ATOMIC_RELAXED)) / 1000000u), what,
             (unsigned long long)__atomic_load_n(&g_wait_value, __ATOMIC_RELAXED));
    if (g_renderer && g_renderer->report_stall) g_renderer->report_stall();
}
static void gpu_thread(void* arg) {
    (void)arg;
    t_gpu = 1;
    SNX_THREAD_NAME("GPU");
    pm_lock(&g_qlk);
    for (;;) {
        GpuItem* it = g_qhead;
        if (!it) {
            /* a plot, not a zone: this wait can last the rest of the run */
            SNX_PLOT("gpu idle", 1);
            pc_wait(&g_qcv, &g_qlk, -1);
            SNX_PLOT("gpu idle", 0);
            continue;
        }
        g_qhead = it->next;
        if (!g_qhead) g_qtail = NULL;
        pm_unlock(&g_qlk);
        __atomic_store_n(&g_run_since, plat_ns(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_run_call, 1, __ATOMIC_RELEASE);
        {
            SNX_ZONE(z_call, "gpu call");
            it->fn(it->arg);
            SNX_ZONE_END(z_call);
        }
        __atomic_store_n(&g_run_call, 0, __ATOMIC_RELEASE);
        free(it);
        pm_lock(&g_qlk);
    }
}
/* the queue's lock held: the condition variable and the thread, once, and the thread told there is work */
static void queue_wake(void) {
    pm_lock_announce(&g_qlk, "gpu queue"); /* the calls' handoff, game threads to the GPU thread */
    if (!g_qcv_ready) {
        pc_init(&g_qcv);
        g_qcv_ready = 1;
    }
    if (g_qhead && !g_qthread) {
        g_qthread = 1;
        if (plat_thread(gpu_thread, NULL, GPU_THREAD_STACK)) {
            char why[256];
            fatal("gpu: could not start the GPU thread: %s\n", snx_os_error(why, sizeof why));
        }
    }
    pc_broadcast(&g_qcv);
}
void gpu_init(void) {
    pm_lock(&g_qlk);
    queue_wake();
    pm_unlock(&g_qlk);
}
void gpu_call(void (*fn)(void*), void* arg) {
    GpuItem* it = (GpuItem*)calloc(1, sizeof *it);
    if (!it) fatal("gpu: out of memory queueing a call\n");
    it->fn = fn;
    it->arg = arg;
    pm_lock(&g_qlk);
    if (g_qtail)
        g_qtail->next = it;
    else
        g_qhead = it;
    g_qtail = it;
    queue_wake();
    pm_unlock(&g_qlk);
}

/* A game pause: the GPU thread gets here once the calls queued before it have run, the renderer finishes what it has
   in flight, and the thread waits until gpu_release. One that gpu_hold gave up on goes past */
static void hold_here(void* arg) {
    (void)arg;
    if (g_renderer->idle) g_renderer->idle();
    pm_lock(&g_qlk);
    if (g_hold_want) {
        g_hold_at = 1;
        pc_broadcast(&g_qcv);
        while (g_hold_want) pc_wait(&g_qcv, &g_qlk, -1);
        g_hold_at = 0;
    }
    pm_unlock(&g_qlk);
}
int gpu_hold(int timeout_ms) {
    u64 end = plat_ns() + (u64)timeout_ms * 1000000u;
    int r = 0;
    pm_lock(&g_qlk);
    if (!g_qthread) { /* nothing was ever queued: there is no GPU thread to hold */
        pm_unlock(&g_qlk);
        return 0;
    }
    g_hold_want = 1;
    pm_unlock(&g_qlk);
    gpu_call(hold_here, NULL);
    pm_lock(&g_qlk);
    while (!g_hold_at) {
        u64 now = plat_ns();
        if (now >= end) {
            g_hold_want = 0;
            r = -1;
            break;
        }
        pc_wait(&g_qcv, &g_qlk, (i64)(end - now));
    }
    pm_unlock(&g_qlk);
    if (r) {
        WARN("pause: the GPU thread didn't get to the pause within %d ms\n", timeout_ms);
        gpu_report_stall();
    }
    return r;
}
void gpu_release(void) {
    pm_lock(&g_qlk);
    g_hold_want = 0;
    pc_broadcast(&g_qcv);
    pm_unlock(&g_qlk);
}

/* ------------------------------------------------------------ save states */
/* With the GPU thread held (gpu_hold) and the renderer idle: the draw count, and the calls still queued (vi's present
   of a queued buffer) by their function and argument */
#define GPU_STATE_VERSION 2
int gpu_state_save(StateIo* io) {
    GpuItem* it;
    u32 n = 0;
    int r = 0;
    pm_lock(&g_qlk);
    r |= io_put32(io, GPU_STATE_VERSION) | io_put64(io, g_draws);
    for (it = g_qhead; it; it = it->next) n++;
    r |= io_put32(io, n);
    for (it = g_qhead; it && !r; it = it->next) {
        const char* name = state_fn_name((StateFnPtr)it->fn);
        u32 len = name ? (u32)strlen(name) : 0;
        if (!name) {
            WARN("gpu: a queued call to %p has no name in state_fns.h's tables, and can't be saved\n",
                 (void*)(uintptr_t)it->fn);
            r = -1;
            break;
        }
        r |= io_put32(io, len) | io_put(io, name, len) | io_put64(io, (u64)(uintptr_t)it->arg);
    }
    pm_unlock(&g_qlk);
    return r ? -1 : 0;
}
/* a GPU section read, not yet in place */
struct GpuStaged {
    u64 draws;
    GpuItem *head, *tail;
};
static void items_free(GpuItem* it) {
    while (it) {
        GpuItem* next = it->next;
        free(it);
        it = next;
    }
}
void gpu_state_staged_free(GpuStaged* s) {
    if (!s) return;
    items_free(s->head);
    free(s);
}
GpuStaged* gpu_state_parse(StateIo* io) {
    GpuStaged* s = (GpuStaged*)calloc(1, sizeof *s);
    u32 version, n, i;
    if (!s) return NULL;
    if (io_get32(io, &version) || version != GPU_STATE_VERSION || io_get64(io, &s->draws) || io_get32(io, &n))
        goto bad;
    for (i = 0; i < n; i++) {
        char name[256];
        u32 len;
        u64 arg;
        StateFnPtr f;
        GpuItem* it;
        if (io_get32(io, &len) || len >= sizeof name || io_get(io, name, len) || io_get64(io, &arg)) goto bad;
        name[len] = 0;
        if (!(f = state_fn_find(name))) {
            WARN("gpu: the state queues a call to %s, which this build doesn't have\n", name);
            goto bad;
        }
        if (!(it = (GpuItem*)calloc(1, sizeof *it))) goto bad;
        it->fn = (void (*)(void*))f;
        it->arg = (void*)(uintptr_t)arg;
        if (s->tail)
            s->tail->next = it;
        else
            s->head = it;
        s->tail = it;
    }
    return s;
bad:
    gpu_state_staged_free(s);
    return NULL;
}
void gpu_state_install(GpuStaged* s) {
    GpuItem* old;
    pm_lock(&g_qlk);
    old = g_qhead; /* the running game's calls, which belong to the memory the state replaces */
    g_draws = s->draws;
    g_qhead = s->head;
    g_qtail = s->tail;
    s->head = s->tail = NULL;
    queue_wake();
    pm_unlock(&g_qlk);
    items_free(old);
    free(s);
}
