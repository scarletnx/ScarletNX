/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/dma_pusher.cpp, src/video_core/dma_pusher.h, src/video_core/engines/puller.cpp,
   src/video_core/engines/puller.h, src/video_core/engines/engine_upload.cpp, src/video_core/engines/engine_upload.h,
   src/video_core/textures/decoders.cpp, src/video_core/textures/decoders.h, src/video_core/engines/maxwell_3d.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
   Compute-inline safe reads follow Citron's dma_pusher.cpp, commit
   acfb1dcc1ad9adf25b960c4f8f25c5bf78849406 (Zephyron's Peach fix). */
/* The GPU's host side: guest memory as the GPU sees it, the channels, the pushbuffer and its puller methods, the
   inline-to-memory engine, and the queue of GPFIFO submissions the GPU thread runs.

   A submission (gpu_submit, from nvdrv's ioctl) only queues a job: the ioctl never waits for the GPU. The GPU thread
   runs the jobs of each channel in order. A stream that has to wait (a semaphore acquire, a syncpoint wait, a fence:
   gpu_park) stops after the method that asked, and the thread runs other channels meanwhile, as the console's PFIFO
   switches channels on an acquire that fails. Calls (gpu_call) run on the GPU thread between jobs, in order.

   The stream is decoded as Eden's DmaPusher does (pushbuffer.h has the formats): each word is a command header or
   one of the data words a header owes, and each data word is a method call. Methods below 0x40 go to the puller
   (Eden's Puller: semaphores, syncpoints, binding a class to a subchannel), the others to the engine of the class
   bound to the call's subchannel (engines.c, compute.c, fermi2d.c, dma.c). */
#include "nx/runtime/state_fns.h"
#include "nx/runtime/state_io.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/gpu_queue.h"
#include "nx/memory/own.h"
#include "common/platform/time.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/inline_to_memory_regs.h"
#include "nx/gpu/maxwell/kepler_compute_regs.h"
#include "nx/gpu/maxwell/maxwell_3d_regs.h"
#include "nx/gpu/maxwell/maxwell_dma_regs.h"
#include "nx/gpu/maxwell/fermi2d_regs.h"
#include "nx/gpu/texture/blocklinear.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/tracy.h"

GpuState g_gpu;
volatile u64 g_gpu_submits;
void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len);
void (*g_gpu_before_write)(u64 gva, u64 size, int whole);
_Atomic int g_gpu_releases;
void (*g_gpu_mapping_begin)(u64 gva, u64 size, int kind);
void (*g_gpu_mapping_end)(void);
void (*g_gpu_mapping_waits)(void);

/* ------------------------------------------------------------ guest memory through the GPU's address space */
#define GMEM_PAGE           0x1000u /* the granule unmapped memory and memory states are walked in: a 4 KiB page */
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
        if (own_any()) {
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
void gmem_read_raw(u64 gva, void* dst, u64 size) {
    gq_read_wait(gva, size);
    copy_runs(gva, (u8*)dst, NULL, size);
}
void gmem_read_quiet(u64 gva, void* dst, u64 size) { copy_runs(gva, (u8*)dst, NULL, size); }
void gmem_write_raw(u64 gva, const void* src, u64 size) { copy_runs(gva, NULL, (const u8*)src, size); }
/* whether anything the renderer holds may cover [gva, gva + size): one lookup where each hook below makes its own */
static int mem_pending(u64 gva, u64 size) { return !g_renderer->mem_pending || g_renderer->mem_pending(gva, size); }
void gmem_need(u64 gva, u64 size) {
    if ((atomic_load_explicit(&g_gpu_releases, memory_order_acquire) ||
         atomic_load_explicit(&g_cs_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_gfx_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_uma_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_mir_any, memory_order_relaxed)) &&
        !mem_pending(gva, size))
        return;
    if (!size) return;
    if (atomic_load_explicit(&g_cs_any, memory_order_relaxed) && g_renderer->cs_mem_need)
        g_renderer->cs_mem_need(gva, size);
    if ((atomic_load_explicit(&g_gfx_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_uma_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_mir_any, memory_order_relaxed)) &&
        g_renderer->gfx_mem_need)
        g_renderer->gfx_mem_need(gva, size);
    if (atomic_load_explicit(&g_gpu_releases, memory_order_acquire) && g_renderer->release_need)
        g_renderer->release_need(gva, size);
}
/* the renderer's hook (vkr_before_write) makes a release queued over the bytes happen first, with everything else
   pending there: dma.c and the inline-to-memory path call it directly */
void gmem_before_write(u64 gva, u64 size, int whole) {
    if (size && g_gpu_before_write) g_gpu_before_write(gva, size, whole);
}
int gmem_read(u64 gva, void* dst, u64 size) {
    if (gq_forward(gva, dst, size)) return 0; /* the front end's own write, not yet in memory */
    gq_read_wait(gva, size); /* the front end's writes queued over it */
    if ((atomic_load_explicit(&g_gpu_releases, memory_order_acquire) ||
         atomic_load_explicit(&g_cs_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_gfx_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_uma_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_mir_any, memory_order_relaxed)) &&
        !mem_pending(gva, size))
        return copy_runs(gva, (u8*)dst, NULL, size) ? 0 : -1;
    /* a queued release's payload is known: the stream reads it without waiting for the work before it, as the console's
       GPU, in order, would find it there (one game's conditional rendering read report pairs this way, waiting for the
       GPU 60 times a second) */
    if (atomic_load_explicit(&g_gpu_releases, memory_order_acquire) && g_renderer->release_read) {
        int r = g_renderer->release_read(gva, dst, size);
        if (r > 0) return 0;
        if (r < 0) g_renderer->release_need(gva, size);
        if (atomic_load_explicit(&g_cs_any, memory_order_relaxed)) g_renderer->cs_mem_need(gva, size);
        if (atomic_load_explicit(&g_gfx_any, memory_order_relaxed) ||
            atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed) ||
            atomic_load_explicit(&g_uma_any, memory_order_relaxed) ||
            atomic_load_explicit(&g_mir_any, memory_order_relaxed))
            g_renderer->gfx_mem_need(gva, size);
        gmem_read_raw(gva, dst, size);
        return 0;
    }
    gmem_need(gva, size);
    return copy_runs(gva, (u8*)dst, NULL, size) ? 0 : -1;
}
int gmem_write_by(u64 gva, const void* src, u64 size, int kind, u32 format, u64 surface) {
    int all;
    if (!size) return 0;
    gmem_before_write(gva, size, 1); /* every byte of it */
    all = copy_runs(gva, NULL, (const u8*)src, size);
    gpu_mem_written_by(gva, size, kind, format, surface);
    return all ? 0 : -1;
}
/* the front end's write goes into the queue (gpu_queue.c), in its order with the draws: 0 then, as what isn't mapped
   is known only when it runs */
int gmem_write(u64 gva, const void* src, u64 size) {
    if (!size) return 0;
    if (gq_queues()) {
        gq_write(gva, src, size);
        return 0;
    }
    return gmem_write_now(gva, src, size);
}
int gmem_write_now(u64 gva, const void* src, u64 size) { return gmem_write_by(gva, src, size, GPU_WRITE_RUNTIME, 0, 0); }
/* A constant buffer upload goes in its place in the queue, as gmem_write's writes do: a draw queued before it reads
   the constant buffer as it was. Written at once on the front end, it skipped the queue, and the renderer thread's
   draws read the values of uploads that came after them. One game's scene drew grey blocks and a broken HUD. Only the
   record's run (gmem_write_cb_now) writes, and it keeps the upload's own stamp (gpu_mem_written_cb) */
int gmem_write_cb(u64 gva, const void* src, u64 size) {
    if (!size) return 0;
    if (gq_queues()) {
        gq_write_cb(gva, src, size);
        return 0;
    }
    return gmem_write_cb_now(gva, src, size);
}
int gmem_write_cb_now(u64 gva, const void* src, u64 size) {
    int all;
    if (!size) return 0;
    gmem_before_write(gva, size, 1); /* every byte of it */
    all = copy_runs(gva, NULL, (const u8*)src, size);
    gpu_mem_written_cb(gva, size);
    return all ? 0 : -1;
}
/* the host pointer of [gva, gva + size) when contiguous, what the GPU holds of it landed (memory/own.c): the caller
   reads or writes it on the CPU, maybe with the renderer's lock held */
u8* gmem_ptr_untracked(u64 gva, u64 size) {
    u64 cpu = 0;
    if (!size) size = 1;
    gq_read_wait(gva, size);
    if (gmmu_run(gva, size, &cpu) != size) return NULL;
    if (own_any()) own_acquire(cpu, size);
    return GP(cpu);
}
u8* gmem_host(u64 gva, u64 size) {
    u64 cpu = 0;
    if (!size) size = 1;
    gq_read_wait(gva, size);
    return gmmu_run(gva, size, &cpu) == size ? GP(cpu) : NULL;
}
/* the results pending over the bytes land first: landing them can make them GPU-owned again (the GPU mirror's
   pages, once their submit is done), which the CPU's access then lands */
u8* gmem_ptr(u64 gva, u64 size) {
    u64 cpu = 0;
    if (!size) size = 1;
    gq_read_wait(gva, size);
    if (gmmu_run(gva, size, &cpu) != size) return NULL;
    gmem_need(gva, size);
    return gmem_ptr_untracked(gva, size);
}
u32 gmem_r32(u64 gva) {
    /* the common read, a word the CPU alone wrote: no queued release, pending result or owner holds it, so guest
       memory has its bytes and the general path's checks and landing are skipped. A release's payload and a pending
       result, over any guest memory, take the general path below, and so does a word on a host page the GPU owns
       bytes of: the page is trapped, and the caller may hold the renderer's lock, which its landing needs. The front
       end's own writes still queued for the renderer's side come first (gpu_queue.c): from memory, an NVN event's add
       (nvn_replay.c's FX_ADD) added to the word from before the add queued ahead of it */
    u64 cpu = 0, owners = 0;
    u8 b[4] = {0};
    if (gq_forward(gva, b, 4)) return snx_ld_le32(b);
    if (!atomic_load_explicit(&g_cs_any, memory_order_relaxed) &&
        !atomic_load_explicit(&g_gfx_any, memory_order_relaxed) &&
        !atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed) &&
        !atomic_load_explicit(&g_uma_any, memory_order_relaxed) &&
        !atomic_load_explicit(&g_mir_any, memory_order_relaxed) &&
        !atomic_load_explicit(&g_gpu_releases, memory_order_acquire) && gmmu_run(gva, 4, &cpu) == 4 &&
        !own_hash(cpu, 4, &owners))
        return snx_ld_le32(GP(cpu));
    gmem_read(gva, b, 4);
    return snx_ld_le32(b);
}
const u8* gmem_cpu_bytes(u64 gva, u64 size) {
    u64 cpu = 0, owners = 0;
    if ((atomic_load_explicit(&g_gpu_releases, memory_order_acquire) ||
         atomic_load_explicit(&g_cs_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_gfx_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_gfx_img_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_uma_any, memory_order_relaxed) ||
         atomic_load_explicit(&g_mir_any, memory_order_relaxed)) &&
        mem_pending(gva, size))
        return NULL;
    if (!size || gmmu_run(gva, size, &cpu) != size || own_hash(cpu, size, &owners)) return NULL;
    return GP(cpu);
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
        if (!own_any()) {
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
/* A byte's offset in the layout (texture/blocklinear.h, docs/layers/gpu/blocklinear.md) is the sum of an x part and a (y, z)
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
    if (g_gpu_before_write) g_gpu_before_write(0, 0, 0);
}
void gpu_release_write(u64 gva, const void* data, u32 len) {
    if (gq_queues()) {
        gq_release(gva, data, len);
        return;
    }
    gpu_release_write_now(gva, data, len);
}
void gpu_release_write_stamped(u64 gva, const void* data) {
    if (gq_queues()) { /* the timestamp first: a game that polls the first bytes then finds it there */
        gq_release_ticks(gva + 8);
        gq_release(gva, data, 8);
        return;
    }
    u8 d[16];
    memcpy(d, data, 8);
    snx_st_le64(d + 8, gpu_ticks());
    gpu_release_write_now(gva, d, sizeof d);
}
void gpu_release_ticks_now(u64 gva) {
    u8 t[8];
    snx_st_le64(t, gpu_ticks());
    gpu_release_write_now(gva, t, sizeof t);
}
void gpu_release_write_now(u64 gva, const void* data, u32 len) {
    if (g_renderer->release_write) {
        g_renderer->release_write(gva, data, len);
        return;
    }
    gpu_signal_point();
    gmem_write(gva, data, len);
}
static void release_stamp_now(u64 gva, const void* data, int top) {
    if (g_renderer->release_stamp) {
        g_renderer->release_stamp(gva, data, top);
        return;
    }
    gpu_release_write_now(gva, data, 16);
}
void gpu_release_stamp(u64 gva, const void* data, int top) {
    if (gq_queues()) {
        gq_release_stamp(gva, data, top);
        return;
    }
    release_stamp_now(gva, data, top);
}
/* a queued timestamp report as its record runs: the ticks that it keeps where the renderer can't time it are that
   moment's, as one thread's are its replay's */
void gpu_release_stamp_ticked(u64 gva, const void* data, int top) {
    u8 d[16];
    memcpy(d, data, 8);
    snx_st_le64(d + 8, gpu_ticks());
    release_stamp_now(gva, d, top);
}
int gpu_release_stamp_sync(u64 cpu, int may_wait) {
    return g_renderer->release_stamp_sync ? g_renderer->release_stamp_sync(cpu, may_wait) : 0;
}
void gpu_mem_changed(u64 cpu_addr, u64 len) {
    if (g_gpu_mem_changed) g_gpu_mem_changed(cpu_addr, len);
}
static void may_have_changed(void) {
    if (g_renderer->mem_may_have_changed) gpu_renderer_now()->mem_may_have_changed();
}
/* a syncpoint increment of the stream, after the work before it */
static void stream_incr(u32 id) {
    if (g_renderer->stream_incr) {
        gpu_stream_incr(id);
        return;
    }
    gpu_signal_point();
    syncpt_incr(id);
}

/* ------------------------------------------------------------ inline to memory (Eden's engine_upload.cpp) */
#define I2M_MAX (64u << 20) /* the most bytes a launch may want: a longer one is a bad register value, not data */
/* Eden's ProcessExec: the launch latches its destination from the upload registers and owes copy_size bytes,
   line_length_in times line_count, which the data words bring */
void i2m_launch(GpuChannel* ch, u32* regs, int base) {
    GpuI2m* t = &ch->i2m;
    GpuUploadRegs* u = &t->regs;
    const u32* r = regs + base;
    u32 block_size = r[UPLOAD_DEST_BLOCK_SIZE];
    u64 len = (u64)r[UPLOAD_LINE_LENGTH_IN] * r[UPLOAD_LINE_COUNT];
    t->copy_size = t->write_offset = 0;
    if (len > I2M_MAX) {
        WARN_LIMIT(4, "gpu: an inline-to-memory launch of %u lines of %u bytes is over %u MiB: nothing is written\n",
                   r[UPLOAD_LINE_COUNT], r[UPLOAD_LINE_LENGTH_IN], I2M_MAX >> 20);
        return;
    }
    if (len > t->capacity) {
        u8* b = (u8*)realloc(t->inner_buffer, (size_t)len);
        if (!b) {
            WARN_LIMIT(4, "gpu: no memory for an inline-to-memory launch of %llu bytes\n", (unsigned long long)len);
            return;
        }
        t->inner_buffer = b;
        t->capacity = (u32)len;
    }
    u->line_length_in = r[UPLOAD_LINE_LENGTH_IN];
    u->line_count = r[UPLOAD_LINE_COUNT];
    u->dest.address =
        ((u64)(r[UPLOAD_DEST_ADDRESS_HIGH] & UPLOAD_DEST_ADDRESS_HIGH_MASK) << 32) | r[UPLOAD_DEST_ADDRESS_LOW];
    u->dest.pitch = r[UPLOAD_DEST_PITCH];
    u->dest.block_height = (block_size >> UPLOAD_BLOCK_HEIGHT_SHIFT) & UPLOAD_BLOCK_FIELD_MASK;
    u->dest.block_depth = (block_size >> UPLOAD_BLOCK_DEPTH_SHIFT) & UPLOAD_BLOCK_FIELD_MASK;
    if (u->dest.block_height > GOB_BLOCK_LOG2_MAX || u->dest.block_depth > GOB_BLOCK_LOG2_MAX) {
        WARN_LIMIT(4, "gpu: an inline-to-memory block of 2^%u x 2^%u GOBs is cut to %u\n", u->dest.block_height,
                   u->dest.block_depth, 1u << GOB_BLOCK_LOG2_MAX);
        if (u->dest.block_height > GOB_BLOCK_LOG2_MAX) u->dest.block_height = GOB_BLOCK_LOG2_MAX;
        if (u->dest.block_depth > GOB_BLOCK_LOG2_MAX) u->dest.block_depth = GOB_BLOCK_LOG2_MAX;
    }
    u->dest.width = r[UPLOAD_DEST_WIDTH];
    u->dest.height = r[UPLOAD_DEST_HEIGHT];
    u->dest.layer = r[UPLOAD_DEST_LAYER];
    u->dest.x = r[UPLOAD_DEST_X];
    u->dest.y = r[UPLOAD_DEST_Y];
    t->is_linear = (int)(r[UPLOAD_EXEC] & UPLOAD_EXEC_LINEAR);
    t->copy_size = (u32)len;
}
/* ProcessData's pitch linear destination: the lines pitch bytes apart, in one write when they are back to back */
static void upload_linear(const GpuI2m* t) {
    const GpuUploadRegs* u = &t->regs;
    u32 line;
    if (u->dest.pitch == u->line_length_in || u->line_count == 1) {
        gmem_write(u->dest.address, t->inner_buffer,
                   (u64)u->line_length_in * (u->dest.pitch == u->line_length_in ? u->line_count : 1));
        return;
    }
    for (line = 0; line < u->line_count; line++)
        gmem_write(u->dest.address + (u64)line * u->dest.pitch, t->inner_buffer + (u64)line * u->line_length_in,
                   u->line_length_in);
}
/* ProcessData's block linear destination (Eden's SwizzleSubrect): the lines into the rectangle at (x, y) of slice
   layer. The rows of blocks the lines fall in take the hooks once, then the lines go as 16 byte runs or less (none
   crosses one of a GOB's 16 byte runs) without them, then the write is recorded once */
/* the bytes of the destination its rows of blocks span, from the surface's start */
static void i2m_block_span(const GpuUploadRegs* u, u64* lo, u64* hi) {
    u32 bh = u->dest.block_height, bd = u->dest.block_depth;
    u64 row_of_blocks = bl_gobs_x(u->dest.width) * bl_block_size(bh, bd); /* bytes */
    u64 slab = (u64)(u->dest.layer >> bd) * bl_block_rows(u->dest.height, bh) * row_of_blocks;
    *lo = slab + ((u64)(u->dest.y >> GOB_SIZE_Y_SHIFT) >> bh) * row_of_blocks;
    *hi = slab + ((((u64)(u->dest.y + u->line_count - 1) >> GOB_SIZE_Y_SHIFT) >> bh) + 1) * row_of_blocks;
}
static void upload_block_linear(const GpuI2m* t) {
    if (gq_queues()) {
        u64 lo, hi;
        i2m_block_span(&t->regs, &lo, &hi);
        gq_i2m_block(t, t->regs.dest.address + lo, hi - lo);
        return;
    }
    gpu_i2m_block_now(t);
}
void gpu_i2m_block_now(const GpuI2m* t) {
    const GpuUploadRegs* u = &t->regs;
    u32 bh = u->dest.block_height, bd = u->dest.block_depth, line, x;
    u64 lo, hi;
    i2m_block_span(u, &lo, &hi);
    /* the lines below are written, the rows of blocks between them are not: a target there keeps its pixels */
    gmem_before_write(u->dest.address + lo, hi - lo, 0);
    for (line = 0; line < u->line_count; line++) {
        u64 row = bl_yz(u->dest.y + line, u->dest.layer, u->dest.width, u->dest.height, bh, bd);
        for (x = 0; x < u->line_length_in;) {
            u32 at = u->dest.x + x, n = GOB_RUN - (at & (GOB_RUN - 1));
            if (n > u->line_length_in - x) n = u->line_length_in - x;
            gmem_write_raw(u->dest.address + row + bl_x(at, bh, bd),
                           t->inner_buffer + (u64)line * u->line_length_in + x, n);
            x += n;
        }
    }
    gpu_mem_written_by(u->dest.address + lo, hi - lo, GPU_WRITE_INLINE, 0, 0);
}
GpuInlineStat g_gpu_inline[GPU_INLINE_N];
_Atomic u64 g_gpu_rcond[GPU_RCOND_N], g_gpu_rcond_cpu_ns;
GpuInlineMark gpu_inline_begin(void) {
    GpuInlineMark m = {own_landings(), own_land_bytes(), plat_ns()};
    return m;
}
void gpu_inline_end(int kind, u64 bytes, GpuInlineMark m) {
    GpuInlineStat* s = &g_gpu_inline[kind];
    atomic_fetch_add_explicit(&s->n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->bytes, bytes, memory_order_relaxed);
    u64 lands = own_landings();
    if (lands == m.lands) return;
    atomic_fetch_add_explicit(&s->forced, lands - m.lands, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->forced_bytes, own_land_bytes() - m.land_bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->forced_ns, plat_ns() - m.t0, memory_order_relaxed);
}
/* Eden's ProcessData: a data word's bytes, low byte first, up to copy_size; the last of them writes the launch */
void i2m_data(GpuChannel* ch, u32 v) {
    GpuI2m* t = &ch->i2m;
    u32 n;
    u8 b[4];
    if (t->write_offset >= t->copy_size) return;
    n = t->copy_size - t->write_offset < 4 ? t->copy_size - t->write_offset : 4;
    snx_st_le32(b, v); /* a data word's bytes go to memory low byte first */
    memcpy(t->inner_buffer + t->write_offset, b, n);
    t->write_offset += n;
    if (t->write_offset < t->copy_size) return;
    GpuInlineMark m = gpu_inline_begin();
    if (t->is_linear)
        upload_linear(t);
    else
        upload_block_linear(t);
    gpu_inline_end(t->is_linear ? GPU_INLINE_LINEAR : GPU_INLINE_BLOCK, t->copy_size, m);
}

/* ------------------------------------------------------------ channels */
/* the 3D state a channel starts from: zero, but for what games expect without setting it (Eden's
   InitializeRegisterDefaults), and ScarletNX's own: color masks enabled (two games never write them), sRGB writes
   on (framebuffer_srgb, else sRGB targets come out too dark), the render enable TRUE and its override
   USE_RENDER_ENABLE (deko3d never writes it), the tessellation levels 1, as the NVN path starts them (a patch
   drawn without a control program before any level is set is not culled), and every sample in the sample masks, as
   deko3d starts them (a draw before any mask is set writes them all). The registers and their values are Eden's
   Maxwell3D::Regs fields and enums (maxwell_3d_regs.h) */
static void reset_3d(u32* r) {
    static const float one = 1.0f;
    u32 i, f1;
    memcpy(&f1, &one, 4);
    for (i = 0; i < MAXWELL3D_NUM_VIEWPORTS; i++) {
        r[MAXWELL3D_VIEWPORTS + i * MAXWELL3D_VIEWPORT_STRIDE + MAXWELL3D_VIEWPORT_DEPTH_RANGE_FAR] = f1;
        r[MAXWELL3D_VIEWPORT_TRANSFORM + i * MAXWELL3D_VIEWPORT_TRANSFORM_STRIDE +
          MAXWELL3D_VIEWPORT_TRANSFORM_SWIZZLE] = MAXWELL3D_VIEWPORT_SWIZZLE_IDENTITY;
    }
    /* blending off in effect: dst = src * One + dst * Zero, the common state and each target's */
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_COLOR_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_COLOR_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_COLOR_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_ALPHA_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_ALPHA_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
    r[MAXWELL3D_BLEND + MAXWELL3D_BLEND_ALPHA_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
    for (i = 0; i < MAXWELL3D_NUM_RENDER_TARGETS; i++) {
        u32* b = &r[MAXWELL3D_BLEND_PER_TARGET + i * MAXWELL3D_BLEND_PER_TARGET_STRIDE];
        b[MAXWELL3D_BLEND_COLOR_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
        b[MAXWELL3D_BLEND_COLOR_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
        b[MAXWELL3D_BLEND_COLOR_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
        b[MAXWELL3D_BLEND_ALPHA_OP] = MAXWELL3D_BLEND_EQUATION_ADD_D3D;
        b[MAXWELL3D_BLEND_ALPHA_SOURCE] = MAXWELL3D_BLEND_FACTOR_ONE_D3D;
        b[MAXWELL3D_BLEND_PER_TARGET_ALPHA_DEST] = MAXWELL3D_BLEND_FACTOR_ZERO_D3D;
    }
    /* stencil: both faces keep every value and always pass, with full masks */
    for (i = MAXWELL3D_STENCIL_OP_FAIL; i <= MAXWELL3D_STENCIL_OP_ZPASS; i++) {
        r[MAXWELL3D_STENCIL_FRONT_OP + i] = MAXWELL3D_STENCIL_OP_KEEP_D3D;
        r[MAXWELL3D_STENCIL_BACK_OP + i] = MAXWELL3D_STENCIL_OP_KEEP_D3D;
    }
    r[MAXWELL3D_STENCIL_FRONT_OP + MAXWELL3D_STENCIL_OP_FUNC] = MAXWELL3D_COMPARISON_ALWAYS_GL;
    r[MAXWELL3D_STENCIL_BACK_OP + MAXWELL3D_STENCIL_OP_FUNC] = MAXWELL3D_COMPARISON_ALWAYS_GL;
    r[MAXWELL3D_STENCIL_FRONT_FUNC_MASK] = r[MAXWELL3D_STENCIL_FRONT_MASK] = 0xFFFFFFFFu;
    r[MAXWELL3D_STENCIL_BACK_FUNC_MASK] = r[MAXWELL3D_STENCIL_BACK_MASK] = 0xFFFFFFFFu;
    r[MAXWELL3D_STENCIL_TWO_SIDE_ENABLE] = 1;
    r[MAXWELL3D_DEPTH_TEST_FUNC] = MAXWELL3D_COMPARISON_ALWAYS_GL;
    /* clockwise front faces are NVN's default; Eden starts counter-clockwise */
    r[MAXWELL3D_GL_FRONT_FACE] = MAXWELL3D_FRONT_FACE_CLOCKWISE;
    r[MAXWELL3D_GL_CULL_FACE] = MAXWELL3D_CULL_FACE_BACK;
    r[MAXWELL3D_POINT_SIZE] = f1;
    for (i = 0; i < 6; i++) r[MAXWELL3D_TESS_LEVELS + i] = f1;
    for (i = 0; i < 4; i++) r[MAXWELL3D_SAMPLE_MASK + i] = 0xFFFF;
    for (i = 0; i < MAXWELL3D_NUM_VERTEX_ATTRIBUTES; i++)
        r[MAXWELL3D_VERTEX_ATTRIB_FORMAT + i] = MAXWELL3D_VERTEX_ATTRIB_CONSTANT; /* constant until set */
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    r[MAXWELL3D_COLOR_TARGET_MRT_ENABLE] = 1;
    r[MAXWELL3D_LINE_WIDTH_SMOOTH] = r[MAXWELL3D_LINE_WIDTH_ALIASED] = f1;
    r[MAXWELL3D_POLYGON_MODE_FRONT] = r[MAXWELL3D_POLYGON_MODE_BACK] = MAXWELL3D_POLYGON_MODE_FILL;
    for (i = 0; i < MAXWELL3D_NUM_RENDER_TARGETS; i++) r[MAXWELL3D_COLOR_MASK + i] = MAXWELL3D_COLOR_MASK_ALL;
    r[MAXWELL3D_FRAMEBUFFER_SRGB] = 1;
    r[MAXWELL3D_RENDER_ENABLE_MODE] = MAXWELL3D_RENDER_ENABLE_TRUE;
    r[MAXWELL3D_RENDER_ENABLE_OVERRIDE] = MAXWELL3D_RENDER_ENABLE_OVERRIDE_USE_RENDER_ENABLE;
}
static void channel_reset(GpuChannel* ch) {
    /* the subchannels as NVN binds them; Eden's channel binds the same classes by the stream's BindObject */
    static const u32 classes[GPU_SUBCHANNELS] = {CLASS_MAXWELL_B,
                                                 CLASS_KEPLER_COMPUTE_B,
                                                 CLASS_KEPLER_INLINE_TO_MEMORY_B,
                                                 CLASS_FERMI_TWOD_A,
                                                 CLASS_MAXWELL_DMA_COPY_A,
                                                 0,
                                                 0,
                                                 0};
    memcpy(ch->sub_class, classes, sizeof classes);
    reset_3d(ch->r3d);
    memcpy(ch->r3d_shadow, ch->r3d, sizeof ch->r3d_shadow); /* the reset values count as tracked */
    ch->r3d_dirty = ~0u; /* nothing the renderer derived from another channel's registers holds for these */
    ch->ds.owner = ch->id;
    /* Eden's Fermi2D dst.depth and src.depth: the 2D surfaces' depth, as NVIDIA's driver assumes */
    ch->r2d[FERMI2D_DST + FERMI2D_SURFACE_DEPTH] = ch->r2d[FERMI2D_SRC + FERMI2D_SURFACE_DEPTH] = 1;
}
/* the channel of syncpoint id, made at its first submission. The queue's lock held */
static GpuChannel* channel_get(u32 id) {
    GpuChannel* ch;
    if (id >= GPU_CHANNELS) fatal("gpu: channel %u is past the %u the GPU has\n", id, GPU_CHANNELS);
    if (g_gpu.chan[id]) return g_gpu.chan[id];
    ch = (GpuChannel*)calloc(1, sizeof *ch);
    if (!ch) fatal("gpu: out of memory for GPU channel %u\n", id);
    ch->id = id;
    channel_reset(ch);
    g_gpu.chan[id] = ch;
    g_gpu.nchan++;
    return ch;
}

/* ------------------------------------------------------------ the pushbuffer (Eden's dma_pusher.cpp) */
/* a word of the stream (Eden's ProcessCommands): a data word the state owes is a method call (1, with *sub, *m, *v
   and *last, the header's last word); a header starts owing words (Eden's SetState) or, inline, is a call itself */
int pb_decode(PbState* b, u32 w, u32* sub, u32* m, u32* v, int* last) {
    u32 mode, count;
    if (b->method_count) {
        *sub = b->subchannel;
        *m = b->method;
        *v = w;
        *last = b->method_count == 1;
        b->method_count--;
        if (b->mode == PB_MODE_INCREASING)
            b->method++;
        else if (b->mode == PB_MODE_INCREASE_ONCE) { /* the first word, then the next method for the rest */
            b->method++;
            b->mode = PB_MODE_NON_INCREASING;
        }
        return 1;
    }
    mode = pb_header_mode(w);
    count = pb_header_count(w);
    *sub = pb_header_subchannel(w);
    *m = pb_header_method(w);
    switch (mode) {
        case PB_MODE_INCREASING:
        case PB_MODE_NON_INCREASING:
        case PB_MODE_INCREASE_ONCE:
            b->mode = mode;
            b->subchannel = *sub;
            b->method = *m;
            b->method_count = count;
            return 0;
        case PB_MODE_INLINE: /* the count is the data */
            *v = count;
            *last = 1;
            return 1;
        default: /* the old modes carry no data words here: a stray one would otherwise eat the methods after it */
            if (w) LOG_LIMIT(8, "gpu: pushbuffer header %08x of mode %u is skipped\n", w, mode);
            return 0;
    }
}
/* the engine a subchannel's class runs on, by the class's low byte (the engine's number across GPU generations:
   0x97 is the 3D engine of MAXWELL_B 0xB197, as of its kin) */
#define CLASS_ENGINE_MASK 0xFFu
enum { ENG_NONE, ENG_3D, ENG_COMPUTE, ENG_I2M, ENG_2D, ENG_DMA };
static int engine_of(u32 cls) {
    switch (cls & CLASS_ENGINE_MASK) {
        case CLASS_MAXWELL_B& CLASS_ENGINE_MASK: return ENG_3D;
        case CLASS_KEPLER_COMPUTE_B& CLASS_ENGINE_MASK: return ENG_COMPUTE;
        case CLASS_KEPLER_INLINE_TO_MEMORY_B& CLASS_ENGINE_MASK: return ENG_I2M;
        case CLASS_FERMI_TWOD_A& CLASS_ENGINE_MASK: return ENG_2D;
        case CLASS_MAXWELL_DMA_COPY_A& CLASS_ENGINE_MASK: return ENG_DMA;
        default: return ENG_NONE;
    }
}
int gpu_class_known(u32 cls) { return engine_of(cls) != ENG_NONE; }
/* How many methods each Maxwell engine runs in one job. method_call runs only on the GPU thread, so these need no
   atomics. The job's dominant engine and method count feed the job's zone text and the per-engine plots (which engine
   ran the most methods on this job). Counting a method is enough: reading the clock twice for every one of them
   biased the timing and filled the trace. */
enum { ENG_N = ENG_DMA + 1 };
static const char* const g_eng_name[ENG_N] = {"none", "3d", "compute", "i2m", "2d", "dma"};
/* index 0 is ENG_NONE and is never plotted, so the names line up with the engine ids ENG_3D to ENG_DMA */
[[maybe_unused]] static const char* const g_eng_plot[ENG_N] = {NULL,
                                                               "gpu engine 3d methods",
                                                               "gpu engine compute methods",
                                                               "gpu engine i2m methods",
                                                               "gpu engine 2d methods",
                                                               "gpu engine dma methods"};
/* the class each engine runs, for the job's zone text: the low byte of the class names the engine (engine_of) */
static const u32 g_eng_class[ENG_N] = {0, CLASS_MAXWELL_B, CLASS_KEPLER_COMPUTE_B,
                                       CLASS_KEPLER_INLINE_TO_MEMORY_B, CLASS_FERMI_TWOD_A,
                                       CLASS_MAXWELL_DMA_COPY_A};
static u32 g_eng_calls[ENG_N];
[[maybe_unused]] static u32 g_job_methods, g_job_eng_calls;
[[maybe_unused]] static const char* g_job_eng_name = "none";
[[maybe_unused]] static u32 g_job_eng_class;
/* Eden's ProcessSemaphoreTriggerMethod: the operation SEMAPHORED names on the semaphore at the address the puller's
   SEMAPHOREA/B hold, with SEMAPHOREC's payload. A release goes through the renderer, after the work before it */
static void process_semaphore_trigger(GpuChannel* ch, u32 v) {
    u64 a = ((u64)(ch->puller[PULLER_SEMAPHORE_ADDRESS_HIGH] & PULLER_SEMAPHORE_ADDRESS_HIGH_MASK) << 32) |
            ch->puller[PULLER_SEMAPHORE_ADDRESS_LOW];
    u32 payload = ch->puller[PULLER_SEMAPHORE_SEQUENCE_PAYLOAD];
    switch (v & PULLER_SEMAPHORE_OP_MASK) {
        case PULLER_SEMAPHORE_OP_ACQUIRE_EQUAL: gpu_park(ch, GPU_PARK_SEMAPHORE, a, payload, 0); break;
        case PULLER_SEMAPHORE_OP_ACQUIRE_GEQUAL: gpu_park(ch, GPU_PARK_SEMAPHORE, a, payload, 1); break;
        case PULLER_SEMAPHORE_OP_RELEASE:
            if (v & PULLER_SEMAPHORE_RELEASE_SIZE_4BYTE) {
                u8 rel[4];
                snx_st_le32(rel, payload);
                gpu_release_write(a, rel, 4);
            } else { /* 16 bytes: the payload, 0, and the timestamp */
                u8 rel[8];
                snx_st_le32(rel, payload);
                snx_st_le32(rel + 4, 0);
                gpu_release_write_stamped(a, rel);
            }
            break;
        case PULLER_SEMAPHORE_OP_ACQUIRE_MASK:
        case PULLER_SEMAPHORE_OP_REDUCTION:
            /* ACQ_AND and REDUCTION aren't performed: the stream goes on as if they were */
            WARN_LIMIT(4, "gpu: semaphore operation %x at %llx is not performed\n", v & PULLER_SEMAPHORE_OP_MASK,
                       (unsigned long long)a);
            break;
        default: LOG_LIMIT(4, "gpu: semaphore operation %08x is unknown\n", v); break;
    }
}
/* Eden's ProcessFenceActionMethod: SYNCPOINTB increments its syncpoint after the work before it, or waits for it to
   reach SYNCPOINTA's threshold */
static void process_fence_action(GpuChannel* ch, u32 v) {
    u32 id = puller_syncpoint_id(v);
    if (puller_syncpoint_increment(v)) {
        stream_incr(id);
        return;
    }
    /* results in flight land before the stream waits on something else (the CPU may read them before it increments
       the syncpoint): a wait already met is none, and stalls nothing */
    gpu_park(ch, GPU_PARK_SYNCPOINT, id, ch->puller[PULLER_SYNCPOINT_PAYLOAD], 1);
    if (atomic_load_explicit(&ch->park, memory_order_relaxed) && g_renderer->before_syncwait)
        gpu_renderer_now()->before_syncwait();
}
/* Eden's CallPullerMethod: a method below NonPullerMethods, kept in the puller's registers */
static void puller_method(GpuChannel* ch, u32 sub, u32 m, u32 v) {
    ch->puller[m] = v;
    switch (m) {
        case PULLER_BIND_OBJECT: ch->sub_class[sub] = v & CLASS_ID_MASK; break; /* Eden's ProcessBindMethod */
        case PULLER_SEMAPHORE_OPERATION: process_semaphore_trigger(ch, v); break;
        case PULLER_SYNCPOINT_OPERATION: process_fence_action(ch, v); break;
        default: break;
    }
}
/* Eden's Puller::CallMethod: the puller's own methods, or the engine bound to the subchannel */
static void method_call(GpuChannel* ch, u32 sub, u32 m, u32 v, int last) {
    int eng = m < PULLER_NON_PULLER_METHODS ? ENG_NONE : engine_of(ch->sub_class[sub]);
    if (eng != ENG_3D) { /* the 3D engine's batched constants and merged draw come before what follows */
        r3d_const_flush(ch);
        r3d_pending_flush();
    }
    if (m < PULLER_NON_PULLER_METHODS) {
        puller_method(ch, sub, m, v);
        return;
    }
    if (snx_tracy_attached()) g_job_methods++;
    switch (eng) {
        case ENG_3D: maxwell3d_method(ch, m, v, last); break;
        case ENG_COMPUTE: compute_method(ch, m, v, last); break;
        case ENG_I2M: kepler_i2m_method(ch, m, v); break;
        case ENG_2D: fermi2d_method(ch, m, v); break;
        case ENG_DMA: dma_method(ch, m, v); break;
        default:
            LOG_LIMIT(8, "gpu: method %x for class %x on subchannel %u: no engine here runs that class\n", m,
                      ch->sub_class[sub], sub);
            break;
    }
    if (eng > ENG_NONE && eng < ENG_N && snx_tracy_attached()) g_eng_calls[eng]++;
}

GpuChannel* gpu_verify_channel_new(u32 id) {
    GpuChannel* ch = (GpuChannel*)calloc(1, sizeof *ch);
    if (!ch) fatal("gpu: out of memory for verify channel %x\n", id);
    ch->id = id;
    channel_reset(ch);
    return ch;
}
void gpu_verify_channel_free(GpuChannel* ch) {
    if (!ch) return;
    free(ch->inline_indices);
    free(ch);
}
void gpu_verify_word(GpuChannel* ch, u32 w) {
    u32 sub, m, v;
    int last;
    if (!pb_decode(&ch->pb, w, &sub, &m, &v, &last)) return;
    if (m < PULLER_NON_PULLER_METHODS) { /* the semaphores and syncpoints are the live stream's business */
        ch->puller[m] = v;
        if (m == PULLER_BIND_OBJECT) ch->sub_class[sub] = v & CLASS_ID_MASK;
        return;
    }
    if (engine_of(ch->sub_class[sub]) == ENG_3D) {
        maxwell3d_method(ch, m, v, last);
        return;
    }
    r3d_const_flush(ch);
    r3d_pending_flush();
}

/* ------------------------------------------------------------ waits */
#define WAIT_POLL_NS         500000ull /* a release the CPU writes has no wake: semaphore waits look again this often */
/* how long a wait nothing ends lasts before the stream goes on (the console raises an interrupt instead): a report
   semaphore's, and every other kind's. park_over's message names them in seconds */
#define WAIT_LIMIT_REPORT_NS 1000000000ull
#define WAIT_LIMIT_NS        2000000000ull
static int park_met(const GpuChannel* ch) {
    if (atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_SYNCPOINT ||
        atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_FENCE)
        return (i32)(syncpt_read((u32)atomic_load_explicit(&ch->park_addr, memory_order_relaxed)) -
                     atomic_load_explicit(&ch->park_val, memory_order_relaxed)) >= 0;
    {
        u32 cur = gmem_r32(atomic_load_explicit(&ch->park_addr, memory_order_relaxed));
        return atomic_load_explicit(&ch->park_geq, memory_order_relaxed)
                   ? (i32)(cur - atomic_load_explicit(&ch->park_val, memory_order_relaxed)) >= 0
                   : cur == atomic_load_explicit(&ch->park_val, memory_order_relaxed);
    }
}
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq) {
    if ((kind == GPU_PARK_SYNCPOINT || kind == GPU_PARK_FENCE) && addr >= SYNCPT_IDS) {
        WARN_LIMIT(8, "gpu: a wait for syncpoint %llu, past the %u there are, doesn't wait\n", (unsigned long long)addr,
                   SYNCPT_IDS);
        return;
    }
    atomic_store_explicit(&ch->park, kind, memory_order_relaxed);
    atomic_store_explicit(&ch->park_addr, addr, memory_order_relaxed);
    atomic_store_explicit(&ch->park_val, val, memory_order_relaxed);
    atomic_store_explicit(&ch->park_geq, geq, memory_order_relaxed);
    /* what the CPU wrote before releasing the wait is for the stream to see, met already or not */
    if (park_met(ch))
        atomic_store_explicit(&ch->park, 0, memory_order_relaxed);
    else /* the console raises an interrupt for a wait nothing ends; here the stream goes on after a while */
        ch->park_end = plat_ns() + (kind == GPU_PARK_REPORT ? WAIT_LIMIT_REPORT_NS : WAIT_LIMIT_NS);
    may_have_changed();
}
static const char* park_name(u32 kind) {
    switch (kind) {
        case GPU_PARK_SEMAPHORE: return "semaphore";
        case GPU_PARK_REPORT: return "report semaphore";
        case GPU_PARK_SYNCPOINT: return "syncpoint";
        default: return "fence";
    }
}
/* how long the channel was parked, in microseconds: the wait's limit less the time still left. gpu_hold can move
   park_end, so clamp at zero. The caller plots it and puts it in the wait's zone text, so a met wait says how long
   the stream was stopped and which kind of wait it was */
[[maybe_unused]] static u64 park_wait_us(u64 now, u64 end, u32 kind) {
    u64 limit = kind == GPU_PARK_REPORT ? WAIT_LIMIT_REPORT_NS : WAIT_LIMIT_NS;
    u64 left = end > now ? end - now : 0;
    u64 waited = limit > left ? limit - left : 0;
    SNX_PLOT("gpu park wait us", (double)waited / 1000.0);
    return waited / 1000;
}
/* 1 when the channel's wait is over (met, or timed out); otherwise the ns to look again in, or -1 for a wake */
static int park_over(GpuChannel* ch, i64* again) {
    u64 now = plat_ns();
    u32 kind;
    if (park_met(ch)) {
        kind = atomic_load_explicit(&ch->park, memory_order_relaxed);
        /* met is the frequent end of a wait: the value is the wait's kind, the text names it and what it reached.
           No guest read here: the timeout zone, which is rare, already reads the semaphore's value */
        SNX_ZONE_TV(z, "gpu wait met", kind, "ch %u %s %llx=%u %lluus", ch->id, park_name(kind),
                    (unsigned long long)atomic_load_explicit(&ch->park_addr, memory_order_relaxed),
                    atomic_load_explicit(&ch->park_val, memory_order_relaxed),
                    (unsigned long long)park_wait_us(now, ch->park_end, kind));
        atomic_store_explicit(&ch->park, 0, memory_order_relaxed);
        may_have_changed();
        return 1;
    }
    if (now >= ch->park_end) {
        kind = atomic_load_explicit(&ch->park, memory_order_relaxed);
        /* a timeout is rare: the value is the wait's kind, the text names it and what it still waits for */
        SNX_ZONE_TV(z, "gpu wait timeout", kind, "ch %u t/o %s %llx=%u now %u %lluus", ch->id, park_name(kind),
                   (unsigned long long)atomic_load_explicit(&ch->park_addr, memory_order_relaxed),
                   atomic_load_explicit(&ch->park_val, memory_order_relaxed),
                   kind == GPU_PARK_SYNCPOINT || kind == GPU_PARK_FENCE
                       ? syncpt_read((u32)atomic_load_explicit(&ch->park_addr, memory_order_relaxed))
                       : gmem_r32(atomic_load_explicit(&ch->park_addr, memory_order_relaxed)),
                   (unsigned long long)park_wait_us(now, ch->park_end, kind));
        WARN_LIMIT(
            16, "gpu: channel %u waited %u s for %s %llx to %s %u (it is %u), and goes on\n", ch->id,
            (u32)((atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_REPORT ? WAIT_LIMIT_REPORT_NS
                                                                                            : WAIT_LIMIT_NS) /
                  1000000000ull),
            park_name(atomic_load_explicit(&ch->park, memory_order_relaxed)),
            (unsigned long long)atomic_load_explicit(&ch->park_addr, memory_order_relaxed),
            atomic_load_explicit(&ch->park_geq, memory_order_relaxed) ? "reach" : "be",
            atomic_load_explicit(&ch->park_val, memory_order_relaxed),
            atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_SYNCPOINT ||
                    atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_FENCE
                ? syncpt_read((u32)atomic_load_explicit(&ch->park_addr, memory_order_relaxed))
                : gmem_r32(atomic_load_explicit(&ch->park_addr, memory_order_relaxed)));
        atomic_store_explicit(&ch->park, 0, memory_order_relaxed);
        may_have_changed();
        return 1;
    }
    *again = (i64)(ch->park_end - now);
    if ((atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_SEMAPHORE ||
         atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_REPORT) &&
        *again > (i64)WAIT_POLL_NS)
        *again = (i64)WAIT_POLL_NS;
    return 0;
}

/* ------------------------------------------------------------ the queue */
/* An item of the GPU thread's queue: a job (a submission's entries) or a call. Items are linked in queue order; the
   GPU thread alone takes them out, and reads them unlocked while it runs one, so an item never moves. A job's
   entries, their word offsets and the words fetched at submission are one allocation with it */
typedef struct GpuItem GpuItem;
struct GpuItem {
    GpuItem* next;
    void (*fn)(void*);  /* set for a call */
    int renderer;       /* the call runs on the renderer's side (gpu_call_renderer) */
    int (*poll)(void*); /* a cooperative call; zero yields without blocking another lane */
    u32 lane;
    void* arg;
    u32 chans[4], nchans;
    u64 id; /* a job's */
    u32 chan, incs, wait_id, wait_val, expect;
    u32 n, pf;       /* the entries, and those before the first SYNC entry, fetched at submission into words */
    u32 entry, word; /* where the stream goes on */
    int started;
    u32 eng_calls[ENG_N]; /* methods run per engine, kept across a park so job_end plots the whole job */
    u32 job_methods;
    u32* late; /* the entries the GPU thread fetched: [late_first, late_end) */
    u32 late_cap, late_first, late_end;
    /* the guest time it could run from: its queueing, or 0 after a wait it missed, and then the time it next runs
       (gpu_ready_ns) */
    u64 ready_ns;
};
/* the engine that ran the most methods on the job, for the zone text. Recomputed when a job parks as well as at its
   end: a parked job never reaches job_end, and its zone text must name that engine, not "none" with zero calls */
static void job_engine_update(void) {
    u32 i, best = 0;
    for (i = 1; i < ENG_N; i++)
        if (g_eng_calls[i] > g_eng_calls[best]) best = i;
    if (g_eng_calls[best]) {
        g_job_eng_name = g_eng_name[best];
        g_job_eng_calls = g_eng_calls[best];
        g_job_eng_class = g_eng_class[best];
    }
}
/* the method counts are globals while a job runs (method_call has no job). A parked job saves them here so another
   job can use the globals meanwhile, and job_end plots every method the job ran, not just its last resume */
static void job_counters_restore(const GpuItem* j) {
    memcpy(g_eng_calls, j->eng_calls, sizeof g_eng_calls);
    g_job_methods = j->job_methods;
}
static void job_counters_save(GpuItem* j) {
    memcpy(j->eng_calls, g_eng_calls, sizeof g_eng_calls);
    j->job_methods = g_job_methods;
    job_engine_update(); /* the park's zone text names the engine that ran most methods */
}
#define QUEUE_ROOM       256                /* runnable jobs at most: jobs of a waiting channel don't count */
#define GPU_THREAD_STACK ((size_t)16 << 20) /* the GPU thread's stack: 16 MiB */
static PMutex g_qlk = PMUTEX_INIT;
/* The context SNX_LOCK_MARK needs is the one g_pm_tracy_snx makes through pm_tracy_make (tracy.c). thread.c keeps it
   in the PMutex, behind its PmNamed wrapper whose first word is that profiler context; read it as pm_named does. Null
   until the lock is announced (or a client starts), so a mark is a no-op then. */
[[maybe_unused]] static SnxTracyLock* gpu_queue_context(void) {
    void* named = atomic_load_explicit(&g_qlk.tracy, memory_order_acquire);
    return named ? *(SnxTracyLock**)named : NULL;
}
static PCond g_qcv;
static int g_qcv_ready;
static _Atomic int g_qthread;
static GpuItem *g_qhead, *g_qtail;
static u64 g_qgen;                /* bumped by each new item and kick: a pass that saw an older one looks again */
static u64 g_qkicks;              /* the kicks among them */
/* A poll's wait, which nothing but WAIT_POLL_NS ended where the write it waits for came with no kick: in one game
   about 7 of these sleeps a frame, 5 ms, at the start of each frame. A stream's wait for memory (a semaphore's or a
   report's acquire) counts as a poll's. g_qpolls (atomic) is 1 while a poll waits: the
   GPU thread sets it before the pass that polls again, so a write that gpu_kick_polls finds it 0 for came before that
   pass's look (both seq_cst), and one that finds it 1 kicks */
static _Atomic int g_qpolls;
static u32 g_qwhy;                /* GPU thread: the last poll's gpu_poll_why */
static GpuPollStats g_qpstat;     /* under g_qlk */
static _Atomic u32 g_qjobs[GPU_CHANNELS]; /* each channel's queued jobs */
static u32 g_qtotal;
static _Atomic u8 g_qparked[GPU_CHANNELS]; /* the channel's oldest job waits (gpu_park) */
static u8 g_scan_stale[GPU_CHANNELS]; /* the channel closed: its submission scan starts again from reset */
static u64 g_job_ids;
static int g_hold_want, g_hold_at; /* a game pause asked the GPU thread to hold, and it holds (gpu_hold) */
/* for the stall report: whether this is the GPU thread, the channel whose job runs (its id + 1, 0 between jobs) and
   since when, and the blocking wait it is in (gpu_wait_note) */
static _Thread_local int t_gpu;
static _Atomic u32 g_run_chan;
static _Atomic u64 g_run_since;
static const char* _Atomic g_wait_what;
/* where the running job's stream is, for the stall report: the phase between methods, and the method begun last.
   Read unlocked, each field on its own; the values stay at the last job's when between jobs */
enum { GPU_AT_BETWEEN, GPU_AT_METHOD, GPU_AT_FETCH, GPU_AT_JOB_END, GPU_AT_STREAM_STOP, GPU_AT_SYNCWAIT };
static _Atomic u32 g_run_at;
static _Atomic u32 g_run_sub, g_run_method, g_run_class, g_run_entry, g_run_word;
static _Atomic u64 g_wait_value, g_wait_since;

/* a job's block after the item: its entries, each fetched entry's first word (in the words fetched at submission,
   or in late), then the words */
static u64* job_ent(const GpuItem* j) { return (u64*)(void*)(j + 1); }
static u32* job_off(const GpuItem* j) { return (u32*)(void*)(job_ent(j) + j->n); }
static u32* job_prefetched(const GpuItem* j) { return job_off(j) + j->n; }
static u32 entry_words(u64 e) { return gpfifo_size(e); }
static u64 entry_addr(u64 e) { return gpfifo_addr(e); }
static u32* job_words(GpuItem* j, u32 e) { return (e < j->pf ? job_prefetched(j) : j->late) + job_off(j)[e]; }
/* Citron's Peach fix requires safe reads for KeplerCompute's inline data:
   an earlier GPU command may have produced the payload since submission.
   Resolve that result on the GPU thread, in stream order. Only payload words
   are refreshed; command headers and other methods keep their snapshots.
   A non-increasing batch is read once, rather than landing each word. */
static u32 fetch_compute_inline(GpuItem* j, GpuChannel* ch, u32 e, u32* words) {
    u32 n = ch->pb.mode == PB_MODE_NON_INCREASING ? ch->pb.method_count : 1;
    u32 left = entry_words(job_ent(j)[e]) - j->word;
    if (n > left) n = left;
    gmem_read(entry_addr(job_ent(j)[e]) + (u64)j->word * 4, words + j->word, (u64)n * 4);
    snx_le32_array(words + j->word, n);
    return j->word + n;
}
/* the entry e the GPU thread reached was not fetched at submission: it and those after it up to the next SYNC entry
   are fetched now, before any of their methods can hand memory back to the CPU */
static int fetch_late(GpuItem* j, u32 e) {
    u32 end = e + 1, k;
    u64 total = entry_words(job_ent(j)[e]);
    SNX_ZONE_V(z, "gpu fetch late", e);
    while (end < j->n && !gpfifo_sync(job_ent(j)[end])) total += entry_words(job_ent(j)[end++]);
    if (total > j->late_cap) {
        u32* b;
        if (total > SIZE_MAX / 4 || !(b = (u32*)realloc(j->late, (size_t)total * 4))) {
            WARN_LIMIT(4, "gpu: no memory to fetch %llu pushbuffer words\n", (unsigned long long)total);
            return 0;
        }
        j->late = b;
        j->late_cap = (u32)total;
    }
    total = 0;
    for (k = e; k < end; k++) {
        u32 n = entry_words(job_ent(j)[k]);
        job_off(j)[k] = (u32)total;
        gmem_read(entry_addr(job_ent(j)[k]), j->late + total, (u64)n * 4);
        snx_le32_array(j->late + total, n); /* the stream's words, read as numbers from here on */
        total += n;
    }
    j->late_first = e;
    j->late_end = end;
    return 1;
}
/* what the stream leaves for the renderer when it stops, at its end or at a wait */
static void stream_stop(GpuChannel* ch) {
    /* why the stream stops here: a wait it reached, or the job's end with none left */
    SNX_ZONE_T(z, "gpu stream stop", "%s",
               atomic_load_explicit(&ch->park, memory_order_relaxed)
                   ? park_name(atomic_load_explicit(&ch->park, memory_order_relaxed))
                   : "job end");
    r3d_const_flush(ch);
    r3d_pending_flush();
    if (g_renderer->stream_end) gpu_stream_end();
}
/* the syncpoint value a submission promised, against the one its increments reached: after them, on the renderer's
   side */
static void job_check_run(const void* arg, u64 bytes) {
    u32 c[2];
    (void)bytes;
    memcpy(c, arg, sizeof c);
    u32 v = g_renderer->release_value_at_least ? g_renderer->release_value_at_least(c[0])
            : g_renderer->release_value        ? g_renderer->release_value(c[0])
                                               : syncpt_read(c[0]);
    if ((i32)(v - c[1]) < 0)
        WARN_LIMIT(8, "gpu: channel %u ended a submission at syncpoint value %u, short of the %u it promised\n", c[0],
                   v, c[1]);
}
static void job_end(const GpuItem* j, GpuChannel* ch) {
    u32 i;
    SNX_ZONE_V(z, "gpu job end", j->chan);
    if (snx_tracy_attached()) {
        job_engine_update();
        /* one plot per engine that ran: how many methods it ran on this job */
        for (i = 1; i < ENG_N; i++)
            if (g_eng_calls[i]) SNX_PLOT(g_eng_plot[i], (double)g_eng_calls[i]);
        SNX_PLOT("gpu job methods", (double)g_job_methods);
        SNX_ZONE_TEXT(z, "ch %u engine %s cls %x %u calls, %u methods", j->chan, g_job_eng_name,
                      g_job_eng_class, g_job_eng_calls, g_job_methods);
    }
    r3d_const_flush(ch);
    r3d_pending_flush();
    /* without a renderer that queues the increments, the work's results land before the game can see its fence. A
       renderer's queue lands them itself before each increment (release.c), so landing every pending result here
       was a full GPU wait at the end of each submission, about 2 ms a frame in one game */
    if (!g_renderer->stream_incr) gpu_signal_point();
    dma_snap_finish(j->chan, j->id);
    for (i = 0; i < j->incs; i++) stream_incr(j->chan);
    if (g_renderer->stream_end) gpu_stream_end();
    u32 check[2] = {j->chan, j->expect};
    gq_call(job_check_run, check, sizeof check);
}
/* runs a job on the GPU thread, the queue unlocked: 1 when it ran to its end, 0 when it waits */
static int run_job(GpuItem* j) {
    GpuChannel* ch = g_gpu.chan[j->chan];
    ch->dma_job_id = j->id;
    if (j->started) {
        if (snx_tracy_attached()) job_counters_restore(j); /* resumed: keep the methods run before the wait */
    } else {
        memset(g_eng_calls, 0, sizeof g_eng_calls);
        g_job_methods = 0;
    }
    g_job_eng_name = "none";
    g_job_eng_calls = 0;
    g_job_eng_class = 0;
    if (!j->started) {
        j->started = 1;
        ch->dma_launch_index = 0;
        if (j->wait_id != ~0u) { /* the fence is the stream's first wait, as nvgpu puts a syncpoint wait first */
            gpu_park(ch, GPU_PARK_FENCE, j->wait_id, j->wait_val, 1);
            if (atomic_load_explicit(&ch->park, memory_order_relaxed)) {
                atomic_store_explicit(&g_run_at, GPU_AT_SYNCWAIT, memory_order_relaxed);
                if (g_renderer->before_syncwait) gpu_renderer_now()->before_syncwait(); /* as at SYNCPOINTB */
                atomic_store_explicit(&g_run_at, GPU_AT_BETWEEN, memory_order_relaxed);
            }
            if (atomic_load_explicit(&ch->park, memory_order_relaxed)) {
                if (snx_tracy_attached()) job_counters_save(j);
                stream_stop(ch);
                return 0;
            }
        }
    }
    if (ch->mme.running) {
        mme_resume(ch);
        if (atomic_load_explicit(&ch->park, memory_order_relaxed)) {
            if (snx_tracy_attached()) job_counters_save(j);
            stream_stop(ch);
            return 0;
        }
    }
    while (j->entry < j->n) {
        u32 e = j->entry, len = entry_words(job_ent(j)[e]);
        u32* w;
        u32 inline_end = 0;
        if (e >= j->pf && (e < j->late_first || e >= j->late_end)) {
            atomic_store_explicit(&g_run_at, GPU_AT_FETCH, memory_order_relaxed);
            int got = fetch_late(j, e);
            atomic_store_explicit(&g_run_at, GPU_AT_BETWEEN, memory_order_relaxed);
            if (!got) len = 0;
        }
        w = len ? job_words(j, e) : NULL;
        while (j->word < len) {
            u32 sub, m, v;
            int last;
            if (j->word >= inline_end && ch->pb.method_count && ch->pb.method == KEPLER_COMPUTE_DATA_UPLOAD &&
                engine_of(ch->sub_class[ch->pb.subchannel]) == ENG_COMPUTE)
                inline_end = fetch_compute_inline(j, ch, e, w);
            if (pb_decode(&ch->pb, w[j->word++], &sub, &m, &v, &last)) {
                atomic_store_explicit(&g_run_sub, sub, memory_order_relaxed);
                atomic_store_explicit(&g_run_method, m, memory_order_relaxed);
                atomic_store_explicit(&g_run_class, ch->sub_class[sub], memory_order_relaxed);
                atomic_store_explicit(&g_run_entry, e, memory_order_relaxed);
                atomic_store_explicit(&g_run_word, j->word, memory_order_relaxed);
                atomic_store_explicit(&g_run_at, GPU_AT_METHOD, memory_order_relaxed);
                method_call(ch, sub, m, v, last);
                atomic_store_explicit(&g_run_at,
                                      atomic_load_explicit(&ch->park, memory_order_relaxed) ? GPU_AT_STREAM_STOP
                                                                                            : GPU_AT_BETWEEN,
                                      memory_order_relaxed);
                if (atomic_load_explicit(&ch->park, memory_order_relaxed)) {
                    if (snx_tracy_attached()) job_counters_save(j);
                    stream_stop(ch);
                    return 0;
                }
            }
        }
        j->entry++;
        j->word = 0;
    }
    atomic_store_explicit(&g_run_at, GPU_AT_JOB_END, memory_order_relaxed);
    job_end(j, ch);
    atomic_store_explicit(&g_run_at, GPU_AT_BETWEEN, memory_order_relaxed);
    return 1;
}
static void item_free(GpuItem* it) {
    free(it->late);
    free(it);
}
/* the jobs the GPU thread can run now, for the queue's room. The queue's lock held */
static u32 runnable(void) {
    u32 n = g_qtotal, c;
    for (c = 0; c < GPU_CHANNELS; c++)
        if (atomic_load_explicit(&g_qparked[c], memory_order_relaxed))
            n -= atomic_load_explicit(&g_qjobs[c], memory_order_relaxed);
    return n;
}
static int g_qmissed; /* under g_qlk: a poll of the pass running returned 0, or a stream waits for memory */
static _Atomic u64 g_ready_ns; /* gpu_ready_ns */
u64 gpu_ready_ns(void) { return atomic_load_explicit(&g_ready_ns, memory_order_relaxed); }
/* it runs now: the time its work became ready, for report timestamps (vk/stamp.c) */
static void run_ready(GpuItem* it) {
    if (!it->ready_ns) it->ready_ns = guest_ns();
    atomic_store_explicit(&g_ready_ns, it->ready_ns, memory_order_relaxed);
}
/* a call of gpu_call_renderer, as a record of the queue */
typedef struct {
    void (*fn)(void*);
    void* arg;
} CallRecord;
static void call_record_run(const void* arg, u64 bytes) {
    CallRecord c;
    (void)bytes;
    memcpy(&c, arg, sizeof c);
    SNX_ZONE(z_call, "gpu call");
    c.fn(c.arg);
    SNX_ZONE_END(z_call);
}
/* one pass over the queue, its lock held (dropped while an item runs): 1 when something ran. *wake is the time to
   look again at the waits */
static int run_pass(i64* wake) {
    u8 blocked[GPU_CHANNELS]; /* an older job of the channel is still queued */
    int calls_blocked = 0;
    u64 lanes_blocked = 0;
    GpuItem *it, *prev = NULL;
    memset(blocked, 0, sizeof blocked);
    for (it = g_qhead; it; prev = it, it = it->next) {
        GpuChannel* ch;
        int done;
        [[maybe_unused]] u32 resumed_wait = 0; /* the wait this channel was parked on, for the job's zone text */
        if (it->poll) {
            u64 lane = 1ull << it->lane;
            if (calls_blocked || (lanes_blocked & lane)) {
                it->ready_ns = 0; /* behind a wait: ready when it next runs */
                continue;
            }
            run_ready(it);
            pm_unlock(&g_qlk);
            g_qwhy = GPU_POLL_OTHER;
            done = it->poll(it->arg);
            pm_lock(&g_qlk);
            if (!done) {
                lanes_blocked |= lane;
                it->ready_ns = 0; /* it waits: ready again when the wait ends */
                g_qpstat.misses[g_qwhy < GPU_POLL_N ? g_qwhy : GPU_POLL_OTHER]++;
                if (*wake < 0 || *wake > (i64)WAIT_POLL_NS) *wake = WAIT_POLL_NS;
                g_qmissed = 1;
                continue;
            }
            if (prev)
                prev->next = it->next;
            else
                g_qhead = it->next;
            if (g_qtail == it) g_qtail = prev;
            item_free(it);
            pc_broadcast(&g_qcv);
            return 1;
        }
        if (it->fn) {
            u32 k;
            int ok = !calls_blocked;
            for (k = 0; k < it->nchans; k++)
                if (it->chans[k] < GPU_CHANNELS && blocked[it->chans[k]]) ok = 0;
            if (!ok) { /* calls keep their order */
                calls_blocked = 1;
                it->ready_ns = 0;
                continue;
            }
            if (prev)
                prev->next = it->next;
            else
                g_qhead = it->next;
            if (g_qtail == it) g_qtail = prev;
            run_ready(it);
            pm_unlock(&g_qlk);
            if (g_renderer->ready) gpu_ready();
            if (it->renderer) {
                CallRecord c = {it->fn, it->arg};
                gq_call(call_record_run, &c, sizeof c);
            } else {
                gq_drain(); /* a call may read or change what the queued records touch */
                SNX_ZONE(z_call, "gpu call");
                it->fn(it->arg);
                SNX_ZONE_END(z_call);
            }
            item_free(it);
            pm_lock(&g_qlk);
            return 1;
        }
        if (blocked[it->chan]) { /* an older job of its channel waits: the pass returns after one that runs */
            it->ready_ns = 0;
            continue;
        }
        blocked[it->chan] = 1;
        ch = g_gpu.chan[it->chan];
        if (atomic_load_explicit(&g_qparked[it->chan], memory_order_relaxed)) {
            i64 again = -1;
            int over;
            resumed_wait = atomic_load_explicit(&ch->park, memory_order_relaxed);
            pm_unlock(&g_qlk);
            over = park_over(ch, &again);
            pm_lock(&g_qlk);
            if (!over) {
                if (again >= 0 && (*wake < 0 || again < *wake)) *wake = again;
                /* a wait for memory: a release's write kicks it (gpu_kick_polls) as it does a poll's, rather than the
                   next look WAIT_POLL_NS on, which stays for what the CPU writes */
                if (atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_SEMAPHORE ||
                    atomic_load_explicit(&ch->park, memory_order_relaxed) == GPU_PARK_REPORT)
                    g_qmissed = 1;
                it->ready_ns = 0;
                continue;
            }
            atomic_store_explicit(&g_qparked[it->chan], 0, memory_order_relaxed);
        }
        run_ready(it);
        pm_unlock(&g_qlk);
        if (g_renderer->ready) gpu_ready();
        atomic_store_explicit(&g_run_since, plat_ns(), memory_order_relaxed);
        atomic_store_explicit(&g_run_chan, it->chan + 1, memory_order_release);
        {
            SNX_ZONE_C(z, "gpu job", 0x4a7fb5);
            SNX_ZONE_VALUE(z, it->chan);
            if (g_renderer->stream_run) gpu_stream_run(1);
            done = run_job(it);
            if (g_renderer->stream_run) gpu_stream_run(0);
            /* the reason this channel was switched back to (the wait's kind code and name), and which engine cost
               most: its class and method count */
            SNX_ZONE_TEXT(z, "ch %u after %s(%u), engine %s cls %x %u calls, %u methods", it->chan,
                          resumed_wait ? park_name(resumed_wait) : "no wait", resumed_wait, g_job_eng_name,
                          g_job_eng_class, g_job_eng_calls, g_job_methods);
            SNX_ZONE_END(z);
        }

        atomic_store_explicit(&g_run_chan, 0, memory_order_release);
        pm_lock(&g_qlk);
        if (!done) {
            atomic_store_explicit(&g_qparked[it->chan], 1, memory_order_relaxed); /* its jobs make room for others */
            it->ready_ns = 0;
        } else {
            if (prev)
                prev->next = it->next;
            else
                g_qhead = it->next;
            if (g_qtail == it) g_qtail = prev;
            atomic_store_explicit(&g_qjobs[it->chan],
                                  atomic_load_explicit(&g_qjobs[it->chan], memory_order_relaxed) - 1,
                                  memory_order_relaxed);
            g_qtotal--;
            item_free(it);
        }
        pc_broadcast(&g_qcv);
        return 1;
    }
    return 0;
}
int gpu_on_thread(void) { return t_gpu; }
int gpu_wait_note(const char* what, u64 value) {
    if (!t_gpu) return 0;
    if (what) {
        atomic_store_explicit(&g_wait_value, value, memory_order_relaxed);
        atomic_store_explicit(&g_wait_since, plat_ns(), memory_order_relaxed);
    }
    atomic_store_explicit(&g_wait_what, what, memory_order_release);
    return 1;
}
/* read unlocked from the watchdog's thread: each value on its own, which is enough to say where the GPU thread is */
void gpu_report_stall(void) {
    u64 now = plat_ns();
    const char* what = atomic_load_explicit(&g_wait_what, memory_order_acquire);
    u32 run = atomic_load_explicit(&g_run_chan, memory_order_acquire), c;
    if (!atomic_load_explicit(&g_qthread, memory_order_relaxed)) {
        WARN("gpu: nothing was submitted yet\n");
        return;
    }
    if (run)
        WARN("gpu: the GPU thread runs a job of channel %u, for %llu ms\n", run - 1,
             (unsigned long long)((now - atomic_load_explicit(&g_run_since, memory_order_relaxed)) / 1000000u));
    else
        WARN("gpu: the GPU thread is between jobs\n");
    if (run) {
        static const char* const at[] = {"between methods",
                                         "in a method",
                                         "fetching an entry's words",
                                         "ending the submission",
                                         "stopping the stream at a wait",
                                         "before a stream's wait"};
        u32 ati = atomic_load_explicit(&g_run_at, memory_order_relaxed);
        WARN("gpu: the job is at entry %u word %u, %s; its last method %x on subchannel %u (class %x)\n",
             atomic_load_explicit(&g_run_entry, memory_order_relaxed),
             atomic_load_explicit(&g_run_word, memory_order_relaxed), ati < sizeof at / sizeof *at ? at[ati] : "?",
             atomic_load_explicit(&g_run_method, memory_order_relaxed),
             atomic_load_explicit(&g_run_sub, memory_order_relaxed),
             atomic_load_explicit(&g_run_class, memory_order_relaxed));
    }
    if (what)
        WARN("gpu: it has waited %llu ms for %s (%llx)\n",
             (unsigned long long)((now - atomic_load_explicit(&g_wait_since, memory_order_relaxed)) / 1000000u), what,
             (unsigned long long)atomic_load_explicit(&g_wait_value, memory_order_relaxed));
    for (c = 0; c < GPU_CHANNELS; c++) {
        u32 jobs = atomic_load_explicit(&g_qjobs[c], memory_order_relaxed), park;
        const GpuChannel* ch = g_gpu.chan[c];
        if (!jobs || !ch) continue;
        park = atomic_load_explicit(&ch->park, memory_order_relaxed);
        if (!park || !atomic_load_explicit(&g_qparked[c], memory_order_relaxed)) {
            WARN("gpu: channel %u has %u jobs queued\n", c, jobs);
            continue;
        }
        {
            u64 addr = atomic_load_explicit(&ch->park_addr, memory_order_relaxed);
            u32 val = atomic_load_explicit(&ch->park_val, memory_order_relaxed);
            int sp = park == GPU_PARK_SYNCPOINT || park == GPU_PARK_FENCE;
            WARN("gpu: channel %u has %u jobs queued, and waits for %s %llx to %s %u (it is %u)\n", c, jobs,
                 park_name(park), (unsigned long long)addr,
                 sp || atomic_load_explicit(&ch->park_geq, memory_order_relaxed) ? "reach" : "be", val,
                 sp ? syncpt_read((u32)addr) : gmem_r32(addr));
        }
    }
    if (g_renderer && g_renderer->report_stall) g_renderer->report_stall(); /* the watchdog's thread: it reads, it doesn't wait */
}
/* the GPU thread's time outside its wait for work (gpu_thread_busy_ns), atomic */
static _Atomic u64 g_busy_ns;
u64 gpu_thread_busy_ns(void) { return atomic_load_explicit(&g_busy_ns, memory_order_relaxed); }
static void gpu_thread(void* arg) {
    (void)arg;
    t_gpu = 1;
    SNX_THREAD_NAME("GPU");
    pm_lock(&g_qlk);
    SNX_LOCK_MARK(gpu_queue_context()); /* held here, the game threads' submissions wait */
    for (;;) {
        u64 gen = g_qgen;
        i64 wake = -1;
        const u64 t0 = plat_ns();
        g_qmissed = 0;
        const int ran = run_pass(&wake);
        atomic_fetch_add_explicit(&g_busy_ns, plat_ns() - t0, memory_order_relaxed);
        if (ran) continue;
        /* a poll waits: set before the pass that looks again, at once where it was not, as gpu_kick_polls asks */
        const int polls = g_qmissed;
        if (polls != atomic_load_explicit(&g_qpolls, memory_order_relaxed)) {
            atomic_store_explicit(&g_qpolls, polls, memory_order_seq_cst);
            if (polls) continue;
        }
        /* A whole pass ran nothing: sleep until a new item, a kick, or the next look at a wait. A plot, not a zone:
           this wait can last the rest of the run. */
        SNX_PLOT("gpu thread idle", 1);
        if (gen == g_qgen) {
            const u64 kicks = g_qkicks, s0 = plat_ns();
            pc_wait(&g_qcv, &g_qlk, wake);
            const int end = g_qkicks != kicks ? 0 : g_qgen != gen ? 1 : 2;
            g_qpstat.sleeps[end]++;
            g_qpstat.last = (u32)end;
            g_qpstat.sleep_ns[end] += plat_ns() - s0;
        }
        SNX_PLOT("gpu thread idle", 0);
    }
}
static PMutex g_submit_lk; /* defined below with PMUTEX_INIT */
static void queue_init(void) {
    /* at each call, so that a client that attaches later still gets them; without one nothing is announced */
    if (snx_tracy_attached()) {
        pm_lock_announce(&g_qlk, "gpu queue");             /* the jobs' handoff, game threads to the GPU thread */
        pm_lock_announce(&g_submit_lk, "gpu submit scan"); /* the queue-order scans of every channel's pushbuffer */
    }
    if (!g_qcv_ready) {
        pc_init(&g_qcv);
        g_qcv_ready = 1;
    }
}
/* the queue's lock held */
static void queue_append(GpuItem* it) {
    it->next = NULL;
    if (g_qtail)
        g_qtail->next = it;
    else
        g_qhead = it;
    g_qtail = it;
    it->ready_ns = guest_ns();
    g_qgen++;
    if (!atomic_load_explicit(&g_qthread, memory_order_relaxed)) {
        atomic_store_explicit(&g_qthread, 1, memory_order_relaxed);
        if (plat_thread(gpu_thread, NULL, GPU_THREAD_STACK)) {
            char why[256];
            fatal("gpu: could not start the GPU thread: %s\n", snx_os_error(why, sizeof why));
        }
    }
    pc_broadcast(&g_qcv);
}
void gpu_init(void) {
    pm_lock(&g_qlk);
    queue_init();
    pm_unlock(&g_qlk);
}
void gpu_cpu_signal(void) { may_have_changed(); }
void gpu_kick(void) {
    pm_lock(&g_qlk);
    g_qgen++;
    g_qkicks++;
    if (g_qcv_ready) pc_broadcast(&g_qcv);
    pm_unlock(&g_qlk);
}
void gpu_kick_polls(void) {
    atomic_thread_fence(memory_order_seq_cst); /* the write before the look, as the GPU thread's flag before its poll */
    if (atomic_load_explicit(&g_qpolls, memory_order_seq_cst)) gpu_kick();
}
void gpu_poll_why(u32 why) { g_qwhy = why; }
void gpu_poll_stats(GpuPollStats* s) {
    pm_lock(&g_qlk);
    *s = g_qpstat;
    pm_unlock(&g_qlk);
}
static void call_queue(void (*fn)(void*), void* arg, const u32* chans, u32 nchans, int renderer) {
    GpuItem* it = (GpuItem*)calloc(1, sizeof *it);
    if (!it) fatal("gpu: out of memory queueing a call\n");
    it->fn = fn;
    it->arg = arg;
    if (nchans > 4) nchans = 4;
    if (nchans) memcpy(it->chans, chans, nchans * sizeof *chans);
    it->nchans = nchans;
    it->renderer = renderer;
    pm_lock(&g_qlk);
    queue_init();
    queue_append(it);
    pm_unlock(&g_qlk);
}
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) { call_queue(fn, arg, chans, nchans, 0); }
void gpu_call_renderer(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    call_queue(fn, arg, chans, nchans, 1);
}
void gpu_call_poll(int (*fn)(void*), void* arg, u32 lane) {
    if (lane >= 64 || !fn) fatal("gpu: invalid cooperative call lane %u\n", lane);
    GpuItem* it = calloc(1, sizeof *it);
    if (!it) fatal("gpu: out of memory queueing a cooperative call\n");
    it->poll = fn;
    it->arg = arg;
    it->lane = lane;
    pm_lock(&g_qlk);
    queue_init();
    queue_append(it);
    pm_unlock(&g_qlk);
}

int gpu_lane_busy(u32 lane) {
    int busy = 0;
    pm_lock(&g_qlk);
    for (const GpuItem* it = g_qhead; it && !busy; it = it->next) busy = it->poll && it->lane == lane;
    pm_unlock(&g_qlk);
    return busy;
}

/* A game pause: the GPU thread gets here once the jobs queued before it have run or wait (a channel waiting on the CPU
   stays parked, its deadline moved on by the time held, as nothing the CPU does can happen meanwhile), the renderer
   finishes what it has in flight, and the thread waits until gpu_release. One that gpu_hold gave up on goes past */
static void hold_here(void* arg) {
    u64 held;
    u32 c;
    (void)arg;
    if (g_renderer->idle) gpu_renderer_now()->idle();
    pm_lock(&g_qlk);
    if (g_hold_want) {
        g_hold_at = 1;
        pc_broadcast(&g_qcv);
        held = plat_ns();
        while (g_hold_want) pc_wait(&g_qcv, &g_qlk, -1);
        g_hold_at = 0;
        held = plat_ns() - held;
        for (c = 0; c < 128; c++)
            if (atomic_load_explicit(&g_qparked[c], memory_order_relaxed) && g_gpu.chan[c])
                g_gpu.chan[c]->park_end += held;
    }
    pm_unlock(&g_qlk);
}
int gpu_hold(int timeout_ms) {
    u64 end = plat_ns() + (u64)timeout_ms * 1000000u;
    int r = 0;
    pm_lock(&g_qlk);
    if (!atomic_load_explicit(&g_qthread,
                              memory_order_relaxed)) { /* nothing was ever submitted: there is no GPU thread to hold */
        pm_unlock(&g_qlk);
        return 0;
    }
    g_hold_want = 1;
    pm_unlock(&g_qlk);
    gpu_call(hold_here, NULL, NULL, 0);
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

/* ------------------------------------------------------------ the submission scan */
/* Each launch of the copy engine with a linear source has its source read at submission (dma.c's dma_snap_launch),
   since games free a copy's source right after submitting it and the GPU thread may run the copy frames later. The
   scan follows each channel's stream only as far as the copy engine needs, with its own subchannel classes and copy
   registers, and stops for good at anything after which the CPU may write the source first: a wait, a macro (which
   can wait), a report operation, a SYNC entry or a fence. A channel the GPU thread has caught up with starts again
   from the channel's own state */
typedef struct {
    u32 sub_class[GPU_SUBCHANNELS];
    PbState pb;
    u32 rdma[MAXWELL_DMA_METHODS];
    int stopped;
} GpuScan;
static GpuScan g_scan[GPU_CHANNELS];
static PMutex g_submit_lk = PMUTEX_INIT; /* the scans in queue order */
/* the engine methods the scan looks for: the 3D engine's report_semaphore operation, at the same method in compute
   (KEPLER_COMPUTE_REPORT_SEMAPHORE_QUERY), and the copy engine's launch */
/* 0 when the scan stops at this method */
static int scan_method(GpuScan* s, u32 chan, u64 job, u32* launches, u32 sub, u32 m, u32 v) {
    int eng;
    if (m < PULLER_NON_PULLER_METHODS) {
        u32 op = v & PULLER_SEMAPHORE_OP_MASK;
        if (m == PULLER_BIND_OBJECT) s->sub_class[sub] = v & CLASS_ID_MASK;
        if (m == PULLER_SEMAPHORE_OPERATION &&
            (op == PULLER_SEMAPHORE_OP_ACQUIRE_EQUAL || op == PULLER_SEMAPHORE_OP_ACQUIRE_GEQUAL ||
             op == PULLER_SEMAPHORE_OP_ACQUIRE_MASK))
            return 0;
        if (m == PULLER_SYNCPOINT_OPERATION && !puller_syncpoint_increment(v)) return 0;
        return 1;
    }
    eng = engine_of(s->sub_class[sub]);
    if (eng == ENG_3D && (m >= MAXWELL3D_NUM_REGS || m == MAXWELL3D_REPORT_SEMAPHORE_QUERY))
        return 0; /* a macro call (it can wait), or a report operation */
    if (eng == ENG_COMPUTE && m == KEPLER_COMPUTE_REPORT_SEMAPHORE_QUERY) return 0;
    if (eng == ENG_DMA && m < MAXWELL_DMA_METHODS) {
        s->rdma[m] = v;
        if (m == MAXWELL_DMA_LAUNCH_DMA) dma_snap_launch(chan, job, (*launches)++, s->rdma, v);
    }
    return 1;
}
static void scan_job(GpuScan* s, const GpuItem* j) {
    u32 e, i, launches = 0;
    if (j->wait_id != ~0u) { /* nothing was fetched: the CPU may write sources before the fence */
        s->stopped = 1;
        return;
    }
    for (e = 0; e < j->pf && !s->stopped; e++) {
        const u32* w = job_prefetched(j) + job_off(j)[e];
        u32 len = entry_words(job_ent(j)[e]);
        for (i = 0; i < len; i++) {
            u32 sub, m, v;
            int last;
            if (pb_decode(&s->pb, w[i], &sub, &m, &v, &last) && !scan_method(s, j->chan, j->id, &launches, sub, m, v)) {
                s->stopped = 1;
                break;
            }
        }
    }
    if (j->pf < j->n) s->stopped = 1; /* a SYNC entry: the rest is fetched when the stream gets there */
}
/* a job for the entries, with those before the first SYNC entry fetched now (none when a fence comes first): the
   guest may reuse its command ring once the ioctl returns */
#define PREFETCH_MAX_WORDS ((64u << 20) / 4) /* 64 MiB of commands fetched at submission at most */
static GpuItem* job_new(const void* entries, u32 n, u32 wait_id) {
    u64 words = 0, size;
    u32 e, pf = 0;
    GpuItem* j;
    const u8* src = (const u8*)entries;
    if (wait_id == ~0u)
        for (pf = 0; pf < n; pf++) {
            u64 en = snx_ld_le64(src + (size_t)pf * 8);
            if (gpfifo_sync(en)) break; /* SYNC: this entry and the rest wait until the stream gets there */
            if (words + entry_words(en) > PREFETCH_MAX_WORDS) break; /* the rest is fetched when reached */
            words += entry_words(en);
        }
    /* the item, the entries (8-byte aligned after it), their offsets, the words */
    size = sizeof(GpuItem) + (u64)n * 8 + (u64)n * 4 + words * 4;
    if (size > SIZE_MAX || !(j = (GpuItem*)calloc(1, (size_t)size)))
        fatal("gpu: out of memory queueing %u entries\n", n);
    j->n = n;
    if (n) memcpy(job_ent(j), entries, (size_t)n * 8);
    snx_le64_array(job_ent(j), n); /* the entries are guest words, the stream's words below too */
    j->pf = pf;
    words = 0;
    for (e = 0; e < pf; e++) {
        u32 k = entry_words(job_ent(j)[e]);
        job_off(j)[e] = (u32)words;
        gmem_read_raw(entry_addr(job_ent(j)[e]), job_prefetched(j) + words, (u64)k * 4);
        snx_le32_array(job_prefetched(j) + words, k);
        words += k;
    }
    return j;
}
void gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect) {
    GpuItem* j;
    GpuScan* s;
    GpuChannel* ch;
    int idle, stale;
    if (channel >= GPU_CHANNELS)
        fatal("gpu: a submission on channel %u, past the %u the GPU has\n", channel, GPU_CHANNELS);
    pm_lock(&g_submit_lk);
    j = job_new(entries, n, wait_id);
    j->chan = channel;
    j->incs = incs;
    j->wait_id = wait_id;
    j->wait_val = wait_val;
    j->expect = expect;
    pm_lock(&g_qlk);
    queue_init();
    stale = !g_gpu.chan[channel] || g_scan_stale[channel];
    ch = channel_get(channel);
    g_scan_stale[channel] = 0;
    idle = !atomic_load_explicit(
        &g_qjobs[channel],
        memory_order_relaxed); /* nothing of the channel is queued or running: its state stands still */
    j->id = ++g_job_ids;
    pm_unlock(&g_qlk);
    s = &g_scan[channel];
    if (stale || idle) {
        memcpy(s->sub_class, ch->sub_class, sizeof s->sub_class);
        s->pb = ch->pb;
        memcpy(s->rdma, ch->rdma, sizeof s->rdma);
        s->stopped = 0;
    }
    if (!s->stopped) scan_job(s, j);
    may_have_changed(); /* before the job can run: what the CPU wrote is for it to see */
    pm_lock(&g_qlk);
    /* a submit can wait for room on the queue: the trace marks why it was late, and for how long. No zone here: the
       wait blocks the calling thread. The message goes out only when the queue was full and a client is attached */
    const int trace_full = snx_tracy_attached();
    u64 full_t0 = trace_full ? plat_ns() : 0;
    int full = 0;
    while (runnable() >= QUEUE_ROOM) {
        full = 1;
        pc_wait(&g_qcv, &g_qlk, -1);
    }
    if (trace_full && full) {
        char msg[96];
        int n = snprintf(msg, sizeof msg, "gpu submit delayed %llu us: queue full (room %u)",
                         (unsigned long long)((plat_ns() - full_t0) / 1000), (unsigned)QUEUE_ROOM);
        if (n > 0 && n < (int)sizeof msg) SNX_MSG(msg, (size_t)n);
    }
    atomic_store_explicit(&g_qjobs[channel], atomic_load_explicit(&g_qjobs[channel], memory_order_relaxed) + 1,
                          memory_order_relaxed);
    g_qtotal++;
    queue_append(j);
    pm_unlock(&g_qlk);
    g_gpu_submits++;
    pm_unlock(&g_submit_lk);
}
void gpu_channel_close(u32 channel) {
    GpuChannel* ch;
    if (channel >= GPU_CHANNELS) return;
    r3d_pending_flush(); /* a draw it merged goes first */
    pm_lock(&g_qlk);
    ch = g_gpu.chan[channel];
    g_gpu.chan[channel] = NULL;
    if (ch) g_gpu.nchan--;
    atomic_store_explicit(&g_qparked[channel], 0, memory_order_relaxed);
    g_scan_stale[channel] = 1;
    pm_unlock(&g_qlk);
    dma_snap_reset(channel);
    if (ch) {
        if (ch->owner.pred) gpu_predicate_drop(ch->owner.pred);
        free(ch->inline_indices);
        free(ch->i2m.inner_buffer);
        free(ch);
    }
}
void gpu_release_retire(u32 channel) {
    if (g_renderer->release_retire) gpu_renderer_now()->release_retire(channel);
}
int gpu_release_wait(u32 id, u32 thresh, int block) {
    return g_renderer && g_renderer->release_wait && g_renderer->release_wait(id, thresh, block);
}

/* ------------------------------------------------------------ save states */
/* With the GPU thread held (gpu_hold) and the renderer idle. A channel goes as its bytes, with its two allocations'
   contents after it and what the renderer derives from it left out; the queue as its items in order: a job's entries,
   their offsets, the words fetched at submission and late, and where it stopped; a call (vi's present of a queued
   buffer, a closed channel's retire) by its function and argument. The submission scans and the copy snapshots go
   with them, so the jobs run as they would have.
   No profiling counter is saved: they feed only trace zones and plots, so they start from zero on a state load
   and need no version or format change */
#define GPU_STATE_VERSION 2
static u64 job_words_total(const GpuItem* j) {
    u64 w = 0;
    u32 e;
    for (e = 0; e < j->pf; e++) w += entry_words(job_ent(j)[e]);
    return w;
}
int gpu_state_save(StateIo* io) {
    GpuChannel* copy = (GpuChannel*)malloc(sizeof *copy);
    GpuItem* it;
    u32 c, n = 0;
    u64 now = plat_ns();
    int r = 0;
    if (!copy) return -1;
    pm_lock(&g_qlk);
    r |= io_put32(io, GPU_STATE_VERSION) | io_put64(io, g_job_ids) | io_put64(io, g_draws);
    for (c = 0; c < 128 && !r; c++) {
        const GpuChannel* ch = g_gpu.chan[c];
        if (!ch) continue;
        *copy = *ch;
        copy->i2m.inner_buffer = NULL;
        copy->inline_indices = NULL;
        copy->park_end = atomic_load_explicit(&ch->park, memory_order_relaxed) && ch->park_end > now
                             ? ch->park_end - now
                             : 0; /* what is left of the wait */
        r |= io_put32(io, c + 1) | io_put(io, copy, sizeof *copy);
        if (ch->i2m.inner_buffer) r |= io_put(io, ch->i2m.inner_buffer, ch->i2m.write_offset);
        if (ch->inline_indices) r |= io_put(io, ch->inline_indices, (u64)ch->inline_n * 4);
    }
    r |= io_put32(io, 0);
    for (it = g_qhead; it; it = it->next) n++;
    r |= io_put32(io, n);
    for (it = g_qhead; it && !r; it = it->next) {
        if (it->poll) {
            WARN("gpu: a cooperative call is still pending and cannot be saved\n");
            r = -1;
            break;
        }
        r |= io_put32(io, it->fn ? 1 : 0);
        if (it->fn) {
            const char* name = state_fn_name((StateFnPtr)it->fn);
            u32 len = name ? (u32)strlen(name) : 0;
            if (!name) {
                WARN("gpu: a queued call to %p has no name in state_fns.h's tables, and can't be saved\n",
                     (void*)(uintptr_t)it->fn);
                r = -1;
                break;
            }
            r |= io_put32(io, len) | io_put(io, name, len) | io_put64(io, (u64)(uintptr_t)it->arg);
            r |= io_put(io, it->chans, sizeof it->chans) | io_put32(io, it->nchans);
            continue;
        }
        r |= io_put64(io, it->id) | io_put32(io, it->chan) | io_put32(io, it->incs) | io_put32(io, it->wait_id);
        r |= io_put32(io, it->wait_val) | io_put32(io, it->expect) | io_put32(io, it->n) | io_put32(io, it->pf);
        r |= io_put32(io, it->entry) | io_put32(io, it->word) | io_put32(io, (u32)it->started);
        r |= io_put(io, job_ent(it), (u64)it->n * 8) | io_put(io, job_off(it), (u64)it->n * 4);
        r |= io_put(io, job_prefetched(it), job_words_total(it) * 4);
        r |= io_put32(io, it->late_first) | io_put32(io, it->late_end);
        if (it->late_end > it->late_first) {
            u64 w = 0;
            u32 e;
            for (e = it->late_first; e < it->late_end; e++) w += entry_words(job_ent(it)[e]);
            r |= io_put(io, it->late, w * 4);
        }
    }
    r |= io_put(io, g_scan, sizeof g_scan) | io_put(io, g_scan_stale, sizeof g_scan_stale);
    pm_unlock(&g_qlk);
    free(copy);
    return r ? -1 : dma_snap_save(io);
}
/* every channel and queued item goes, as when the channels close, with the GPU thread held */
static void gpu_state_drop(void) {
    GpuItem* it;
    u32 c;
    pm_lock(&g_qlk);
    while ((it = g_qhead) != NULL) {
        g_qhead = it->next;
        item_free(it);
    }
    g_qtail = NULL;
    g_qtotal = 0;
    memset(g_qjobs, 0, sizeof g_qjobs);
    memset(g_qparked, 0, sizeof g_qparked);
    pm_unlock(&g_qlk);
    for (c = 0; c < 128; c++) gpu_channel_close(c);
}
/* a GPU section read, not yet in place */
struct GpuStaged {
    u64 job_ids, draws;
    GpuChannel* chan[128];
    GpuItem *head, *tail;
    GpuScan scan[128];
    u8 scan_stale[128];
    DmaStaged* dma;
};
void gpu_state_staged_free(GpuStaged* s) {
    u32 c;
    if (!s) return;
    for (c = 0; c < 128; c++)
        if (s->chan[c]) {
            free(s->chan[c]->i2m.inner_buffer);
            free(s->chan[c]->inline_indices);
            free(s->chan[c]);
        }
    while (s->head) {
        GpuItem* it = s->head;
        s->head = it->next;
        item_free(it);
    }
    dma_snap_staged_free(s->dma);
    free(s);
}
/* the item at the queue's end */
static void staged_append(GpuStaged* s, GpuItem* it) {
    if (s->tail)
        s->tail->next = it;
    else
        s->head = it;
    s->tail = it;
}
GpuStaged* gpu_state_parse(StateIo* io) {
    GpuStaged* s = (GpuStaged*)calloc(1, sizeof *s);
    u32 version, c, n, i, kind;
    if (!s) return NULL;
    if (io_get32(io, &version) || version != GPU_STATE_VERSION || io_get64(io, &s->job_ids) || io_get64(io, &s->draws))
        goto bad;
    for (;;) {
        GpuChannel* ch;
        if (io_get32(io, &c)) goto bad;
        if (!c) break;
        if (c > 128 || s->chan[c - 1] || !(ch = s->chan[c - 1] = (GpuChannel*)calloc(1, sizeof *ch))) goto bad;
        if (io_get(io, ch, sizeof *ch)) {
            memset(ch, 0, sizeof *ch); /* none of its pointers are this process's */
            goto bad;
        }
        ch->i2m.inner_buffer = NULL;
        ch->inline_indices = NULL;
        if (ch->i2m.capacity) {
            if (ch->i2m.write_offset > ch->i2m.capacity || !(ch->i2m.inner_buffer = (u8*)malloc(ch->i2m.capacity)) ||
                io_get(io, ch->i2m.inner_buffer, ch->i2m.write_offset))
                goto bad;
        }
        if (ch->inline_cap) {
            if (ch->inline_n > ch->inline_cap || !(ch->inline_indices = (u32*)malloc((size_t)ch->inline_cap * 4)) ||
                io_get(io, ch->inline_indices, (u64)ch->inline_n * 4))
                goto bad;
        }
        ch->r3d_dirty = ~0u; /* nothing the renderer derived holds */
        /* the draw state's generations came from the saving process's sequence (gpu/draw_state.c), where this one may
           hand the same numbers out for other states: new ones, which nothing kept matches */
        ds_generations_renew(&ch->ds);
        ch->owner.pred = 0;        /* the predicate was the renderer's: draws run until the next render enable */
        ch->owner.xfb_started = 0; /* the byte counters were the renderer's */
        gpu_draw_states_reset();   /* and so are its copies of the states and owners */
    }
    if (io_get32(io, &n)) goto bad;
    for (i = 0; i < n; i++) {
        GpuItem* it;
        if (io_get32(io, &kind)) goto bad;
        if (kind) {
            char name[256];
            u32 len;
            u64 arg;
            StateFnPtr f;
            if (io_get32(io, &len) || len >= sizeof name || io_get(io, name, len)) goto bad;
            name[len] = 0;
            if (!(f = state_fn_find(name))) {
                WARN("gpu: the state queues a call to %s, which this build doesn't have\n", name);
                goto bad;
            }
            if (!(it = (GpuItem*)calloc(1, sizeof *it))) goto bad;
            staged_append(s, it);
            if (io_get64(io, &arg) || io_get(io, it->chans, sizeof it->chans) || io_get32(io, &it->nchans) ||
                it->nchans > sizeof it->chans / sizeof *it->chans)
                goto bad;
            it->fn = (void (*)(void*))f;
            it->arg = (void*)(uintptr_t)arg;
        } else {
            u32 h[10]; /* chan, incs, wait_id, wait_val, expect, n, pf, entry, word, started */
            u64 id, words, size;
            GpuItem* big;
            if (io_get64(io, &id) || io_get(io, h, sizeof h) || h[5] > (1u << 24) || h[6] > h[5] || h[0] >= 128)
                goto bad;
            /* the entries come first: the words fetched at submission are counted from them */
            size = sizeof(GpuItem) + (u64)h[5] * 12;
            if (!(it = (GpuItem*)calloc(1, (size_t)size))) goto bad;
            it->n = h[5];
            it->pf = h[6];
            if (io_get(io, job_ent(it), (u64)it->n * 8) || io_get(io, job_off(it), (u64)it->n * 4)) {
                free(it);
                goto bad;
            }
            words = job_words_total(it);
            if (words > (1u << 26) || !(big = (GpuItem*)realloc(it, (size_t)(size + words * 4)))) {
                free(it);
                goto bad;
            }
            it = big;
            staged_append(s, it);
            if (io_get(io, job_prefetched(it), words * 4) || io_get32(io, &it->late_first) ||
                io_get32(io, &it->late_end) || it->late_end > it->n || it->late_first > it->late_end)
                goto bad;
            it->id = id;
            it->chan = h[0];
            it->incs = h[1];
            it->wait_id = h[2];
            it->wait_val = h[3];
            it->expect = h[4];
            it->entry = h[7];
            it->word = h[8];
            it->started = (int)h[9];
            if (it->late_end > it->late_first) {
                u64 w = 0;
                u32 e;
                for (e = it->late_first; e < it->late_end; e++) w += entry_words(job_ent(it)[e]);
                if (w > (1u << 26) || !(it->late = (u32*)malloc((size_t)(w ? w : 1) * 4))) goto bad;
                it->late_cap = (u32)w;
                if (io_get(io, it->late, w * 4)) goto bad;
            }
        }
    }
    if (io_get(io, s->scan, sizeof s->scan) || io_get(io, s->scan_stale, sizeof s->scan_stale) ||
        !(s->dma = dma_snap_parse(io)))
        goto bad;
    return s;
bad:
    gpu_state_staged_free(s);
    return NULL;
}
void gpu_state_install(GpuStaged* s) {
    u64 now = plat_ns();
    GpuItem* it;
    u32 c;
    gpu_state_drop();
    pm_lock(&g_qlk);
    g_job_ids = s->job_ids;
    g_draws = s->draws;
    for (c = 0; c < 128; c++)
        if (s->chan[c]) {
            s->chan[c]->park_end += now; /* what was left of the wait, from now */
            g_gpu.chan[c] = s->chan[c];
            g_gpu.nchan++;
            s->chan[c] = NULL;
        }
    for (it = s->head; it; it = it->next)
        if (!it->fn) {
            atomic_store_explicit(&g_qjobs[it->chan],
                                  atomic_load_explicit(&g_qjobs[it->chan], memory_order_relaxed) + 1,
                                  memory_order_relaxed);
            g_qtotal++;
            /* the channel's oldest job, stopped at a wait: it waits again */
            if (it->started && g_gpu.chan[it->chan] &&
                atomic_load_explicit(&g_gpu.chan[it->chan]->park, memory_order_relaxed))
                atomic_store_explicit(&g_qparked[it->chan], 1, memory_order_relaxed);
        }
    g_qhead = s->head;
    g_qtail = s->tail;
    s->head = s->tail = NULL;
    memcpy(g_scan, s->scan, sizeof g_scan);
    memcpy(g_scan_stale, s->scan_stale, sizeof g_scan_stale);
    g_qgen++;
    pm_unlock(&g_qlk);
    dma_snap_install(s->dma);
    s->dma = NULL;
    free(s);
}
void gpu_state_schema(int (*dump)(u64* h, const char* fmt, ...), u64* h) { __builtin_dump_struct(&g_scan[0], dump, h); }
