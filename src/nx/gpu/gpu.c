/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/dma_pusher.cpp, src/video_core/engines/puller.cpp,
   src/video_core/engines/engine_upload.cpp, src/video_core/textures/decoders.cpp,
   src/video_core/engines/maxwell_3d.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2022 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The GPU's host side: guest memory as the GPU sees it, the channels, the pushbuffer and its puller methods, the
   inline-to-memory engine, and the queue of GPFIFO submissions the GPU thread runs.

   A submission (gpu_submit, from nvdrv's ioctl) only queues a job: the ioctl never waits for the GPU. The GPU thread
   runs the jobs of each channel in order. A stream that has to wait (a semaphore acquire, a syncpoint wait, a fence:
   gpu_park) stops after the method that asked, and the thread runs other channels meanwhile, as the console's PFIFO
   switches channels on an acquire that fails. Calls (gpu_call) run on the GPU thread between jobs, in order. */
#include <stdlib.h>
#include <string.h>
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "nx/gpu/gpu.h"
#include "nx/memory/own.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"

GpuState g_gpu;
volatile u64 g_gpu_submits;
void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len);
void (*g_gpu_before_write)(u64 gva, u64 size);
int g_gpu_releases;
void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
void (*g_gpu_mapping_end)(void);

/* ------------------------------------------------------------ guest memory through the GPU's address space */
/* the runs of guest memory behind [gva, gva + size), copied in or out: 1 when every page was mapped. An unmapped
   page, or one the guest freed after mapping it here (gmmu_run), reads as zero and takes no write. What the GPU holds of a run lands first (memory/own.c): the caller may hold
   the renderer's lock, and a trap can't be answered then */
static int copy_runs(u64 gva, u8* in, const u8* out, u64 size) {
    int all = 1;
    while (size) {
        u64 cpu = 0, n = gmmu_run(gva, size, &cpu);
        if (!n) {
            n = 0x1000 - (gva & 0xFFF);
            if (n > size) n = size;
            if (in) memset(in, 0, (size_t)n);
            all = 0;
            goto next;
        }
        if (__atomic_load_n(&g_own_any, __ATOMIC_ACQUIRE)) own_acquire(cpu, n);
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
   pending there: dma.c and the inline-to-memory path call it directly */
void gmem_before_write(u64 gva, u64 size) {
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
void gmem_read_code(u64 gva, void* dst, u64 size) {
    MemBlock b;
    int have = 0;
    u64 done = 0;
    memset(&b, 0, sizeof b);
    while (done < size) {
        u64 a = gva + done, n = 0x1000 - (a & 0xFFF), cpu = gmmu_translate(a);
        u32 state;
        if (!cpu) break;
        if (!have || cpu < b.addr || cpu - b.addr >= b.size) { /* a block's pages share its state */
            mem_query(cpu, &b);
            have = 1;
        }
        state = b.state & 0xFF;
        if (!b.size || state == (MS_FREE & 0xFF) || state == (MS_INACCESSIBLE & 0xFF)) break;
        done += n < size - done ? n : size - done;
    }
    if (done) {
        gmem_need(gva, done);
        gmem_read_raw(gva, dst, done);
    }
    memset((u8*)dst + done, 0, (size_t)(size - done));
}

/* ------------------------------------------------------------ block linear layout (Eden's decoders.cpp) */
/* A GOB is 64 bytes by 8 rows, 512 bytes: two 32 byte halves of 256 bytes, each four pairs of rows of two 16 byte
   runs. Blocks are 2^bh GOBs high and 2^bd deep, laid out row by row, and a slice of them after another. The x, y
   and z parts of an offset add up, so a row's base and a column's offset can be computed apart */
static u64 bl_x(u32 x, u32 bh, u32 bd) {
    return ((u64)(x >> 6) << (9 + bh + bd)) + ((x & 32) << 3) + ((x & 16) << 1) + (x & 15);
}
static u64 bl_yz(u32 y, u32 z, u32 wb, u32 h, u32 bh, u32 bd) {
    u64 gobs_x = (wb + 63) >> 6, gy = y >> 3;
    u64 rows = ((((u64)h + 7) >> 3) + (1u << bh) - 1) >> bh; /* block rows in a slice of blocks */
    u64 block = (u64)512 << (bh + bd);
    u64 in_block = ((((u64)z & ((1u << bd) - 1)) << bh) + (gy & ((1u << bh) - 1))) << 9;
    return (((u64)(z >> bd) * rows + (gy >> bh)) * gobs_x) * block + in_block + (((y & 7) >> 1) << 6) +
           ((y & 1) << 4);
}
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    return bl_x(x_bytes, bh_log2, bd_log2) + bl_yz(y, z, width_bytes, height, bh_log2, bd_log2);
}
/* the guest bytes of a pixel of a color format, for render targets and 2D surfaces alike (nv50's surface formats) */
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
static void may_have_changed(void) {
    if (g_renderer->mem_may_have_changed) g_renderer->mem_may_have_changed();
}
/* a syncpoint increment of the stream, after the work before it */
static void stream_incr(u32 id) {
    if (g_renderer->stream_incr) {
        g_renderer->stream_incr(id);
        return;
    }
    gpu_signal_point();
    syncpt_incr(id);
}

/* ------------------------------------------------------------ inline to memory (Eden's engine_upload.cpp) */
#define I2M_MAX (64u << 20)
void i2m_launch(GpuChannel* ch, u32* regs, int base) {
    GpuI2m* t = &ch->i2m;
    const u32* r = regs + base;
    u64 len = (u64)r[0] * r[1];
    t->len = t->pos = 0;
    if (len > I2M_MAX) {
        WARN_LIMIT(4, "gpu: an inline-to-memory launch of %u lines of %u bytes is over %u MiB: nothing is written\n",
                   r[1], r[0], I2M_MAX >> 20);
        return;
    }
    if (len > t->cap) {
        u8* b = (u8*)realloc(t->buf, (size_t)len);
        if (!b) {
            WARN_LIMIT(4, "gpu: no memory for an inline-to-memory launch of %llu bytes\n", (unsigned long long)len);
            return;
        }
        t->buf = b;
        t->cap = (u32)len;
    }
    t->line = r[0];
    t->lines = r[1];
    t->addr = ((u64)(r[2] & 0xFF) << 32) | r[3];
    t->pitch = r[4];
    t->bh = (r[5] >> 4) & 15;
    t->bd = (r[5] >> 8) & 15;
    if (t->bh > 5 || t->bd > 5) {
        WARN_LIMIT(4, "gpu: an inline-to-memory block of 2^%u x 2^%u GOBs is cut to 32\n", t->bh, t->bd);
        if (t->bh > 5) t->bh = 5;
        if (t->bd > 5) t->bd = 5;
    }
    t->w = r[6];
    t->h = r[7];
    t->z = r[9];
    t->x = r[10];
    t->y = r[11];
    t->linear = r[12] & 1;
    t->len = (u32)len;
}
static void i2m_store(const GpuI2m* t) {
    u32 y, x;
    if (t->linear) {
        if (t->pitch == t->line || t->lines == 1) {
            gmem_write(t->addr, t->buf, (u64)t->line * (t->pitch == t->line ? t->lines : 1));
            return;
        }
        for (y = 0; y < t->lines; y++) gmem_write(t->addr + (u64)y * t->pitch, t->buf + (u64)y * t->line, t->line);
    } else {
        /* the block rows the lines fall in: the hooks once over them, then the 16 byte runs, then one record */
        u64 gobs_x = ((u64)t->w + 63) >> 6, block = (u64)512 << (t->bh + t->bd);
        u64 rows = ((((u64)t->h + 7) >> 3) + (1u << t->bh) - 1) >> t->bh;
        u64 slice = (u64)(t->z >> t->bd) * rows * gobs_x * block;
        u64 lo = slice + ((u64)(t->y >> 3) >> t->bh) * gobs_x * block;
        u64 hi = slice + ((((u64)(t->y + t->lines - 1) >> 3) >> t->bh) + 1) * gobs_x * block;
        gmem_before_write(t->addr + lo, hi - lo);
        for (y = 0; y < t->lines; y++) {
            u64 row = bl_yz(t->y + y, t->z, t->w, t->h, t->bh, t->bd);
            for (x = 0; x < t->line;) {
                u32 at = t->x + x, n = 16 - (at & 15);
                if (n > t->line - x) n = t->line - x;
                gmem_write_raw(t->addr + row + bl_x(at, t->bh, t->bd), t->buf + (u64)y * t->line + x, n);
                x += n;
            }
        }
        gpu_mem_written(t->addr + lo, hi - lo);
    }
}
void i2m_data(GpuChannel* ch, u32 v) {
    GpuI2m* t = &ch->i2m;
    u32 n;
    if (t->pos >= t->len) return;
    u8 b[4];
    n = t->len - t->pos < 4 ? t->len - t->pos : 4;
    snx_st_le32(b, v); /* a data word's bytes go to memory low byte first */
    memcpy(t->buf + t->pos, b, n);
    t->pos += n;
    if (t->pos == t->len) i2m_store(t);
}

/* ------------------------------------------------------------ channels */
/* the 3D state a channel starts from: zero, but for what games expect without setting it (Eden's
   InitializeRegisterDefaults), and ScarletNX's own: color masks enabled (Fire Emblem Three Houses and Unicorn
   Overlord never write them), sRGB writes on (0x56E, else sRGB targets come out too dark), the render enable TRUE
   (0x556) and its override USE_RENDER_ENABLE (0x651, as deko3d never writes it) */
static void reset_3d(u32* r) {
    static const float one = 1.0f;
    u32 i, f1;
    memcpy(&f1, &one, 4);
    for (i = 0; i < 16; i++) {
        r[0x300 + i * 4 + 3] = f1;     /* each viewport's depth range far */
        r[0x280 + i * 8 + 6] = 0x6420; /* each viewport's swizzle: +x +y +z +w */
    }
    r[0x4D0] = 1; /* blend: add, one, zero (D3D's enums), color and alpha */
    r[0x4D1] = 2;
    r[0x4D2] = 1;
    r[0x4D3] = 1;
    r[0x4D4] = 2;
    r[0x4D6] = 1;
    for (i = 0; i < 8; i++) {
        u32* b = &r[0x780 + i * 8];
        b[1] = 1, b[2] = 2, b[3] = 1, b[4] = 1, b[5] = 2, b[6] = 1;
    }
    r[0x4E1] = r[0x4E2] = r[0x4E3] = 1; /* stencil: keep, always, all bits */
    r[0x4E4] = 0x207;
    r[0x4E6] = r[0x4E7] = 0xFFFFFFFF;
    r[0x565] = 1; /* two sided, the back the same */
    r[0x566] = r[0x567] = r[0x568] = 1;
    r[0x569] = 0x207;
    r[0x3D6] = r[0x3D7] = 0xFFFFFFFF;
    r[0x4C3] = 0x207; /* depth test always */
    r[0x647] = 0x900; /* front face clockwise, NVN's */
    r[0x648] = 0x405; /* cull the back */
    r[0x546] = f1;    /* point size */
    for (i = 0; i < 32; i++) r[0x458 + i] = 1u << 6; /* vertex attributes constant until set */
    r[0xDF] = 1;                                      /* rasterize */
    r[0x3EB] = 1;                                     /* color target MRT */
    r[0x4EC] = r[0x4ED] = f1;                         /* line widths */
    r[0x36B] = r[0x36C] = 0x1B02;                     /* polygons filled */
    for (i = 0; i < 8; i++) r[0x680 + i] = 0x11111111;
    r[0x56E] = 1;
    r[0x556] = 1;
    r[0x651] = 0;
}
static void channel_reset(GpuChannel* ch) {
    static const u32 classes[8] = {0xB197, 0xB1C0, 0xA140, 0x902D, 0xB0B5, 0, 0, 0};
    memcpy(ch->sub_class, classes, sizeof classes);
    reset_3d(ch->r3d);
    memcpy(ch->r3d_shadow, ch->r3d, sizeof ch->r3d_shadow); /* the reset values count as tracked */
    ch->r3d_dirty = ~0u; /* nothing the renderer derived from another channel's registers holds for these */
    ch->r2d[0x83] = ch->r2d[0x8F] = 1; /* the 2D surfaces' depth, as NVIDIA's driver assumes */
}
/* the channel of syncpoint id, made at its first submission. The queue's lock held */
static GpuChannel* channel_get(u32 id) {
    GpuChannel* ch;
    if (id >= 128) fatal("gpu: channel %u is past the 128 the GPU has\n", id);
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
/* a word of the stream: 1 when it is a method call (*sub, *m, *v, *last) */
static int pb_decode(PbBatch* b, u32 w, u32* sub, u32* m, u32* v, int* last) {
    u32 mode, cnt;
    if (b->cnt) {
        *sub = b->sub;
        *m = b->m;
        *v = w;
        *last = b->cnt == 1;
        b->cnt--;
        if (b->mode == 1)
            b->m++;
        else if (b->mode == 5) { /* incrementing once: the first word, then the next method for the rest */
            b->m++;
            b->mode = 3;
        }
        return 1;
    }
    /* a header: the method is 12 bits (open-gpu-doc's NVA16F_DMA_METHOD_ADDRESS 11:0), the subchannel 15:13, the
       count 28:16 and the mode 31:29 */
    mode = w >> 29;
    cnt = (w >> 16) & 0x1FFF;
    *sub = (w >> 13) & 7;
    *m = w & 0xFFF;
    switch (mode) {
        case 1:
        case 3:
        case 5:
            b->mode = mode;
            b->sub = *sub;
            b->m = *m;
            b->cnt = cnt;
            return 0;
        case 4: /* immediate: the count is the data */
            *v = cnt;
            *last = 1;
            return 1;
        default: /* the old modes carry no data words here: a stray one would otherwise eat the methods after it */
            if (w) LOG_LIMIT(8, "gpu: pushbuffer header %08x of mode %u is skipped\n", w, mode);
            return 0;
    }
}
enum { ENG_NONE, ENG_3D, ENG_COMPUTE, ENG_I2M, ENG_2D, ENG_DMA };
static int engine_of(u32 cls) {
    switch (cls & 0xFF) {
        case 0x97: return ENG_3D;      /* MAXWELL_B 0xB197 */
        case 0xC0: return ENG_COMPUTE; /* KEPLER_COMPUTE_B 0xB1C0 */
        case 0x40: return ENG_I2M;     /* KEPLER_INLINE_TO_MEMORY_B 0xA140 */
        case 0x2D: return ENG_2D;      /* FERMI_TWOD_A 0x902D */
        case 0xB5: return ENG_DMA;     /* MAXWELL_DMA_COPY_A 0xB0B5 */
        default: return ENG_NONE;
    }
}
/* the puller's methods (open-gpu-doc's cla16f.h): semaphores, syncpoints, the subchannels' classes */
static void puller_method(GpuChannel* ch, u32 sub, u32 m, u32 v) {
    ch->puller[m] = v;
    switch (m) {
        case 0x00: /* SET_OBJECT */ ch->sub_class[sub] = v & 0xFFFF; break;
        case 0x07: { /* SEMAPHORED: the operation on the semaphore at 4/5 with the payload at 6 */
            u64 a = ((u64)(ch->puller[4] & 0xFF) << 32) | ch->puller[5];
            u32 payload = ch->puller[6];
            switch (v & 0x1F) {
                case 1: gpu_park(ch, GPU_PARK_SEMAPHORE, a, payload, 0); break;
                case 4: gpu_park(ch, GPU_PARK_SEMAPHORE, a, payload, 1); break;
                case 2:
                    if (v & (1u << 24)) { /* RELEASE_SIZE 4 bytes */
                        u8 rel[4];
                        snx_st_le32(rel, payload);
                        gpu_release_write(a, rel, 4);
                    } else { /* 16 bytes: the payload, 0, and the timestamp */
                        u8 rel[16];
                        snx_st_le32(rel, payload);
                        snx_st_le32(rel + 4, 0);
                        snx_st_le64(rel + 8, gpu_ticks());
                        gpu_release_write(a, rel, 16);
                    }
                    break;
                case 8:
                case 16:
                    /* ACQ_AND and REDUCTION aren't performed: the stream goes on as if they were */
                    WARN_LIMIT(4, "gpu: semaphore operation %x at %llx is not performed\n", v & 0x1F,
                               (unsigned long long)a);
                    break;
                default: LOG_LIMIT(4, "gpu: semaphore operation %08x is unknown\n", v); break;
            }
            break;
        }
        case 0x1D: { /* SYNCPOINTB: increment, or wait for the threshold at 0x1C */
            u32 id = (v >> 8) & 0xFFF;
            if (v & 1)
                stream_incr(id);
            else {
                /* results in flight land before the stream waits on something else (the CPU may read them before
                   it increments the syncpoint): a wait already met is none, and stalls nothing */
                gpu_park(ch, GPU_PARK_SYNCPOINT, id, ch->puller[0x1C], 1);
                if (ch->park && g_renderer->before_syncwait) g_renderer->before_syncwait();
            }
            break;
        }
        default: break;
    }
}
static void method_call(GpuChannel* ch, u32 sub, u32 m, u32 v, int last) {
    int eng = m < 0x40 ? ENG_NONE : engine_of(ch->sub_class[sub]);
    if (eng != ENG_3D) { /* the 3D engine's batched constants and merged draw come before what follows */
        r3d_const_flush(ch);
        r3d_pending_flush();
    }
    if (m < 0x40) {
        puller_method(ch, sub, m, v);
        return;
    }
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
}

/* ------------------------------------------------------------ waits */
#define WAIT_POLL_NS 500000ull /* a release the CPU writes has no wake: semaphore waits look again this often */
static int park_met(const GpuChannel* ch) {
    if (ch->park == GPU_PARK_SYNCPOINT || ch->park == GPU_PARK_FENCE)
        return (i32)(syncpt_read((u32)ch->park_addr) - ch->park_val) >= 0;
    {
        u32 cur = gmem_r32(ch->park_addr);
        return ch->park_geq ? (i32)(cur - ch->park_val) >= 0 : cur == ch->park_val;
    }
}
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq) {
    if ((kind == GPU_PARK_SYNCPOINT || kind == GPU_PARK_FENCE) && addr >= SYNCPT_IDS) {
        WARN_LIMIT(8, "gpu: a wait for syncpoint %llu, past the %u there are, doesn't wait\n", (unsigned long long)addr,
                   SYNCPT_IDS);
        return;
    }
    ch->park = kind;
    ch->park_addr = addr;
    ch->park_val = val;
    ch->park_geq = geq;
    /* what the CPU wrote before releasing the wait is for the stream to see, met already or not */
    if (park_met(ch))
        ch->park = 0;
    else /* the console raises an interrupt for a wait nothing ends; here the stream goes on after a while */
        ch->park_end = plat_ns() + (kind == GPU_PARK_REPORT ? 1000000000ull : 2000000000ull);
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
/* 1 when the channel's wait is over (met, or timed out); otherwise the ns to look again in, or -1 for a wake */
static int park_over(GpuChannel* ch, i64* again) {
    u64 now;
    if (park_met(ch)) {
        ch->park = 0;
        may_have_changed();
        return 1;
    }
    now = plat_ns();
    if (now >= ch->park_end) {
        WARN_LIMIT(16, "gpu: channel %u waited %s s for %s %llx to %s %u (it is %u), and goes on\n", ch->id,
                   ch->park == GPU_PARK_REPORT ? "1" : "2", park_name(ch->park), (unsigned long long)ch->park_addr,
                   ch->park_geq ? "reach" : "be", ch->park_val,
                   ch->park == GPU_PARK_SYNCPOINT || ch->park == GPU_PARK_FENCE ? syncpt_read((u32)ch->park_addr)
                                                                               : gmem_r32(ch->park_addr));
        ch->park = 0;
        may_have_changed();
        return 1;
    }
    *again = (i64)(ch->park_end - now);
    if ((ch->park == GPU_PARK_SEMAPHORE || ch->park == GPU_PARK_REPORT) && *again > (i64)WAIT_POLL_NS)
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
    void (*fn)(void*); /* set for a call */
    void* arg;
    u32 chans[4], nchans;
    u64 id; /* a job's */
    u32 chan, incs, wait_id, wait_val, expect;
    u32 n, pf;        /* the entries, and those before the first SYNC entry, fetched at submission into words */
    u32 entry, word;  /* where the stream goes on */
    int started;
    u32* late;        /* the entries the GPU thread fetched: [late_first, late_end) */
    u32 late_cap, late_first, late_end;
};
#define QUEUE_ROOM 256 /* runnable jobs at most: jobs of a waiting channel don't count */
static PMutex g_qlk = PMUTEX_INIT;
static PCond g_qcv;
static int g_qcv_ready, g_qthread;
static GpuItem *g_qhead, *g_qtail;
static u64 g_qgen;           /* bumped by each new item and kick: a pass that saw an older one looks again */
static u32 g_qjobs[128];     /* each channel's queued jobs */
static u32 g_qtotal;
static u8 g_qparked[128];    /* the channel's oldest job waits (gpu_park) */
static u8 g_scan_stale[128]; /* the channel closed: its submission scan starts again from reset */
static u64 g_job_ids;
static int g_hold_want, g_hold_at; /* a game pause asked the GPU thread to hold, and it holds (gpu_hold) */
/* for the stall report: whether this is the GPU thread, the channel whose job runs (its id + 1, 0 between jobs) and
   since when, and the blocking wait it is in (gpu_wait_note) */
static _Thread_local int t_gpu;
static u32 g_run_chan;
static u64 g_run_since;
static const char* g_wait_what;
/* where the running job's stream is, for the stall report: the phase between methods, and the method begun last.
   Read unlocked, each field on its own; the values stay at the last job's when between jobs */
enum { GPU_AT_BETWEEN, GPU_AT_METHOD, GPU_AT_FETCH, GPU_AT_JOB_END, GPU_AT_STREAM_STOP, GPU_AT_SYNCWAIT };
static u32 g_run_at;
static u32 g_run_sub, g_run_method, g_run_class, g_run_entry, g_run_word;
static u64 g_wait_value, g_wait_since;

/* a job's block after the item: its entries, each fetched entry's first word (in the words fetched at submission,
   or in late), then the words */
static u64* job_ent(const GpuItem* j) { return (u64*)(void*)(j + 1); }
static u32* job_off(const GpuItem* j) { return (u32*)(void*)(job_ent(j) + j->n); }
static u32* job_prefetched(const GpuItem* j) { return job_off(j) + j->n; }
static u32 entry_words(u64 e) { return (u32)((e >> 42) & 0x1FFFFF); }
static u64 entry_addr(u64 e) { return e & 0xFFFFFFFFFCull; }
static const u32* job_words(const GpuItem* j, u32 e) {
    return (e < j->pf ? job_prefetched(j) : j->late) + job_off(j)[e];
}
/* the entry e the GPU thread reached was not fetched at submission: it and those after it up to the next SYNC entry
   are fetched now, before any of their methods can hand memory back to the CPU */
static int fetch_late(GpuItem* j, u32 e) {
    u32 end = e + 1, k;
    u64 total = entry_words(job_ent(j)[e]);
    SNX_ZONE_V(z, "gpu fetch late", e);
    while (end < j->n && !(job_ent(j)[end] >> 63)) total += entry_words(job_ent(j)[end++]);
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
    SNX_ZONE(z, "gpu stream stop");
    r3d_const_flush(ch);
    r3d_pending_flush();
    if (g_renderer->stream_end) g_renderer->stream_end();
}
static void job_end(const GpuItem* j, GpuChannel* ch) {
    u32 i, v;
    SNX_ZONE_V(z, "gpu job end", j->chan);
    r3d_const_flush(ch);
    r3d_pending_flush();
    /* without a renderer that queues the increments, the work's results land before the game can see its fence. A
       renderer's queue lands them itself before each increment (release.c), so landing every pending result here
       was a full GPU wait at the end of each submission, about 2 ms a frame in one game */
    if (!g_renderer->stream_incr) gpu_signal_point();
    dma_snap_finish(j->chan, j->id);
    for (i = 0; i < j->incs; i++) stream_incr(j->chan);
    if (g_renderer->stream_end) g_renderer->stream_end();
    v = g_renderer->release_value ? g_renderer->release_value(j->chan) : syncpt_read(j->chan);
    if ((i32)(v - j->expect) < 0)
        WARN_LIMIT(8, "gpu: channel %u ended a submission at syncpoint value %u, short of the %u it promised\n",
                   j->chan, v, j->expect);
}
/* runs a job on the GPU thread, the queue unlocked: 1 when it ran to its end, 0 when it waits */
static int run_job(GpuItem* j) {
    GpuChannel* ch = g_gpu.chan[j->chan];
    ch->dma_job_id = j->id;
    if (!j->started) {
        j->started = 1;
        ch->dma_launch_index = 0;
        if (j->wait_id != ~0u) { /* the fence is the stream's first wait, as nvgpu puts a syncpoint wait first */
            gpu_park(ch, GPU_PARK_FENCE, j->wait_id, j->wait_val, 1);
            if (ch->park) {
                __atomic_store_n(&g_run_at, GPU_AT_SYNCWAIT, __ATOMIC_RELAXED);
                if (g_renderer->before_syncwait) g_renderer->before_syncwait(); /* as at SYNCPOINTB */
                __atomic_store_n(&g_run_at, GPU_AT_BETWEEN, __ATOMIC_RELAXED);
            }
            if (ch->park) {
                stream_stop(ch);
                return 0;
            }
        }
    }
    if (ch->mme.running) {
        mme_resume(ch);
        if (ch->park) {
            stream_stop(ch);
            return 0;
        }
    }
    while (j->entry < j->n) {
        u32 e = j->entry, len = entry_words(job_ent(j)[e]);
        const u32* w;
        if (e >= j->pf && (e < j->late_first || e >= j->late_end)) {
            __atomic_store_n(&g_run_at, GPU_AT_FETCH, __ATOMIC_RELAXED);
            int got = fetch_late(j, e);
            __atomic_store_n(&g_run_at, GPU_AT_BETWEEN, __ATOMIC_RELAXED);
            if (!got) len = 0;
        }
        w = len ? job_words(j, e) : NULL;
        while (j->word < len) {
            u32 sub, m, v;
            int last;
            if (pb_decode(&ch->pb, w[j->word++], &sub, &m, &v, &last)) {
                __atomic_store_n(&g_run_sub, sub, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_method, m, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_class, ch->sub_class[sub], __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_entry, e, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_word, j->word, __ATOMIC_RELAXED);
                __atomic_store_n(&g_run_at, GPU_AT_METHOD, __ATOMIC_RELAXED);
                method_call(ch, sub, m, v, last);
                __atomic_store_n(&g_run_at, ch->park ? GPU_AT_STREAM_STOP : GPU_AT_BETWEEN, __ATOMIC_RELAXED);
                if (ch->park) {
                    stream_stop(ch);
                    return 0;
                }
            }
        }
        j->entry++;
        j->word = 0;
    }
    __atomic_store_n(&g_run_at, GPU_AT_JOB_END, __ATOMIC_RELAXED);
    job_end(j, ch);
    __atomic_store_n(&g_run_at, GPU_AT_BETWEEN, __ATOMIC_RELAXED);
    return 1;
}
static void item_free(GpuItem* it) {
    free(it->late);
    free(it);
}
/* the jobs the GPU thread can run now, for the queue's room. The queue's lock held */
static u32 runnable(void) {
    u32 n = g_qtotal, c;
    for (c = 0; c < 128; c++)
        if (g_qparked[c]) n -= g_qjobs[c];
    return n;
}
/* one pass over the queue, its lock held (dropped while an item runs): 1 when something ran. *wake is the time to
   look again at the waits */
static int run_pass(i64* wake) {
    u8 blocked[128]; /* an older job of the channel is still queued */
    int calls_blocked = 0;
    GpuItem *it, *prev = NULL;
    memset(blocked, 0, sizeof blocked);
    for (it = g_qhead; it; prev = it, it = it->next) {
        GpuChannel* ch;
        int done;
        if (it->fn) {
            u32 k;
            int ok = !calls_blocked;
            for (k = 0; k < it->nchans; k++)
                if (it->chans[k] < 128 && blocked[it->chans[k]]) ok = 0;
            if (!ok) { /* calls keep their order */
                calls_blocked = 1;
                continue;
            }
            if (prev)
                prev->next = it->next;
            else
                g_qhead = it->next;
            if (g_qtail == it) g_qtail = prev;
            pm_unlock(&g_qlk);
            SNX_ZONE(z_call, "gpu call");
            it->fn(it->arg);
            SNX_ZONE_END(z_call);
            item_free(it);
            pm_lock(&g_qlk);
            return 1;
        }
        if (blocked[it->chan]) continue;
        blocked[it->chan] = 1;
        ch = g_gpu.chan[it->chan];
        if (g_qparked[it->chan]) {
            i64 again = -1;
            int over;
            pm_unlock(&g_qlk);
            over = park_over(ch, &again);
            pm_lock(&g_qlk);
            if (!over) {
                if (again >= 0 && (*wake < 0 || again < *wake)) *wake = again;
                continue;
            }
            g_qparked[it->chan] = 0;
        }
        pm_unlock(&g_qlk);
        __atomic_store_n(&g_run_since, plat_ns(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_run_chan, it->chan + 1, __ATOMIC_RELEASE);
        {
            SNX_ZONE_C(z, "gpu job", 0x4a7fb5);
            SNX_ZONE_VALUE(z, it->chan);
            SNX_ZONE_TEXT(z, "channel %u", it->chan);
            if (g_renderer->stream_run) g_renderer->stream_run(1);
            done = run_job(it);
            if (g_renderer->stream_run) g_renderer->stream_run(0);
            SNX_ZONE_END(z);
        }

        __atomic_store_n(&g_run_chan, 0, __ATOMIC_RELEASE);
        pm_lock(&g_qlk);
        if (!done)
            g_qparked[it->chan] = 1; /* its jobs make room for others */
        else {
            if (prev)
                prev->next = it->next;
            else
                g_qhead = it->next;
            if (g_qtail == it) g_qtail = prev;
            g_qjobs[it->chan]--;
            g_qtotal--;
            item_free(it);
        }
        pc_broadcast(&g_qcv);
        return 1;
    }
    return 0;
}
void gpu_wait_note(const char* what, u64 value) {
    if (!t_gpu) return;
    if (what) {
        __atomic_store_n(&g_wait_value, value, __ATOMIC_RELAXED);
        __atomic_store_n(&g_wait_since, plat_ns(), __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_wait_what, what, __ATOMIC_RELEASE);
}
/* read unlocked from the watchdog's thread: each value on its own, which is enough to say where the GPU thread is */
void gpu_report_stall(void) {
    u64 now = plat_ns();
    const char* what = __atomic_load_n(&g_wait_what, __ATOMIC_ACQUIRE);
    u32 run = __atomic_load_n(&g_run_chan, __ATOMIC_ACQUIRE), c;
    if (!__atomic_load_n(&g_qthread, __ATOMIC_RELAXED)) {
        WARN("gpu: nothing was submitted yet\n");
        return;
    }
    if (run)
        WARN("gpu: the GPU thread runs a job of channel %u, for %llu ms\n", run - 1,
             (unsigned long long)((now - __atomic_load_n(&g_run_since, __ATOMIC_RELAXED)) / 1000000u));
    else
        WARN("gpu: the GPU thread is between jobs\n");
    if (run) {
        static const char* const at[] = {"between methods", "in a method", "fetching an entry's words",
                                         "ending the submission", "stopping the stream at a wait",
                                         "before a stream's wait"};
        u32 ati = __atomic_load_n(&g_run_at, __ATOMIC_RELAXED);
        WARN("gpu: the job is at entry %u word %u, %s; its last method %x on subchannel %u (class %x)\n",
             __atomic_load_n(&g_run_entry, __ATOMIC_RELAXED), __atomic_load_n(&g_run_word, __ATOMIC_RELAXED),
             ati < sizeof at / sizeof *at ? at[ati] : "?", __atomic_load_n(&g_run_method, __ATOMIC_RELAXED),
             __atomic_load_n(&g_run_sub, __ATOMIC_RELAXED), __atomic_load_n(&g_run_class, __ATOMIC_RELAXED));
    }
    if (what)
        WARN("gpu: it has waited %llu ms for %s (%llx)\n",
             (unsigned long long)((now - __atomic_load_n(&g_wait_since, __ATOMIC_RELAXED)) / 1000000u), what,
             (unsigned long long)__atomic_load_n(&g_wait_value, __ATOMIC_RELAXED));
    for (c = 0; c < 128; c++) {
        u32 jobs = __atomic_load_n(&g_qjobs[c], __ATOMIC_RELAXED), park;
        const GpuChannel* ch = g_gpu.chan[c];
        if (!jobs || !ch) continue;
        park = __atomic_load_n(&ch->park, __ATOMIC_RELAXED);
        if (!park || !__atomic_load_n(&g_qparked[c], __ATOMIC_RELAXED)) {
            WARN("gpu: channel %u has %u jobs queued\n", c, jobs);
            continue;
        }
        {
            u64 addr = __atomic_load_n(&ch->park_addr, __ATOMIC_RELAXED);
            u32 val = __atomic_load_n(&ch->park_val, __ATOMIC_RELAXED);
            int sp = park == GPU_PARK_SYNCPOINT || park == GPU_PARK_FENCE;
            WARN("gpu: channel %u has %u jobs queued, and waits for %s %llx to %s %u (it is %u)\n", c, jobs,
                 park_name(park), (unsigned long long)addr,
                 sp || __atomic_load_n(&ch->park_geq, __ATOMIC_RELAXED) ? "reach" : "be", val,
                 sp ? syncpt_read((u32)addr) : gmem_r32(addr));
        }
    }
    if (g_renderer && g_renderer->report_stall) g_renderer->report_stall();
}
static void gpu_thread(void* arg) {
    (void)arg;
    t_gpu = 1;
    SNX_THREAD_NAME("GPU");
    pm_lock(&g_qlk);
    for (;;) {
        u64 gen = g_qgen;
        i64 wake = -1;
        if (run_pass(&wake)) continue;
        /* A whole pass ran nothing: sleep until a new item, a kick, or the next look at a wait. A plot, not a zone:
           this wait can last the rest of the run. */
        SNX_PLOT("gpu idle", 1);
        if (gen == g_qgen) pc_wait(&g_qcv, &g_qlk, wake);
        SNX_PLOT("gpu idle", 0);
    }
}
static void queue_init(void) {
    pm_lock_announce(&g_qlk, "gpu queue"); /* the jobs' handoff, game threads to the GPU thread */
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
    g_qgen++;
    if (!g_qthread) {
        g_qthread = 1;
        if (plat_thread(gpu_thread, NULL, (size_t)16 << 20)) {
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
void gpu_kick(void) {
    pm_lock(&g_qlk);
    g_qgen++;
    if (g_qcv_ready) pc_broadcast(&g_qcv);
    pm_unlock(&g_qlk);
}
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    GpuItem* it = (GpuItem*)calloc(1, sizeof *it);
    if (!it) fatal("gpu: out of memory queueing a call\n");
    it->fn = fn;
    it->arg = arg;
    if (nchans > 4) nchans = 4;
    if (nchans) memcpy(it->chans, chans, nchans * sizeof *chans);
    it->nchans = nchans;
    pm_lock(&g_qlk);
    queue_init();
    queue_append(it);
    pm_unlock(&g_qlk);
}

/* A game pause: the GPU thread gets here once the jobs queued before it have run or wait (a channel waiting on the CPU
   stays parked, its deadline moved on by the time held, as nothing the CPU does can happen meanwhile), the renderer
   finishes what it has in flight, and the thread waits until gpu_release. One that gpu_hold gave up on goes past */
static void hold_here(void* arg) {
    u64 held;
    u32 c;
    (void)arg;
    if (g_renderer->idle) g_renderer->idle();
    pm_lock(&g_qlk);
    if (g_hold_want) {
        g_hold_at = 1;
        pc_broadcast(&g_qcv);
        held = plat_ns();
        while (g_hold_want) pc_wait(&g_qcv, &g_qlk, -1);
        g_hold_at = 0;
        held = plat_ns() - held;
        for (c = 0; c < 128; c++)
            if (g_qparked[c] && g_gpu.chan[c]) g_gpu.chan[c]->park_end += held;
    }
    pm_unlock(&g_qlk);
}
int gpu_hold(int timeout_ms) {
    u64 end = plat_ns() + (u64)timeout_ms * 1000000u;
    int r = 0;
    pm_lock(&g_qlk);
    if (!g_qthread) { /* nothing was ever submitted: there is no GPU thread to hold */
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
    u32 sub_class[8];
    PbBatch pb;
    u32 rdma[0x200];
    int stopped;
} GpuScan;
static GpuScan g_scan[128];
static PMutex g_submit_lk = PMUTEX_INIT; /* the scans in queue order */
/* 0 when the scan stops at this method */
static int scan_method(GpuScan* s, u32 chan, u64 job, u32* launches, u32 sub, u32 m, u32 v) {
    int eng;
    if (m < 0x40) {
        if (m == 0) s->sub_class[sub] = v & 0xFFFF;
        if (m == 7 && ((v & 0x1F) == 1 || (v & 0x1F) == 4 || (v & 0x1F) == 8)) return 0;
        if (m == 0x1D && !(v & 1)) return 0;
        return 1;
    }
    eng = engine_of(s->sub_class[sub]);
    if (eng == ENG_3D && (m >= 0xE00 || m == 0x6C3)) return 0;
    if (eng == ENG_COMPUTE && m == 0x6C3) return 0;
    if (eng == ENG_DMA && m < 0x200) {
        s->rdma[m] = v;
        if (m == 0xC0) dma_snap_launch(chan, job, (*launches)++, s->rdma, v);
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
            if (pb_decode(&s->pb, w[i], &sub, &m, &v, &last) &&
                !scan_method(s, j->chan, j->id, &launches, sub, m, v)) {
                s->stopped = 1;
                break;
            }
        }
    }
    if (j->pf < j->n) s->stopped = 1; /* a SYNC entry: the rest is fetched when the stream gets there */
}
/* a job for the entries, with those before the first SYNC entry fetched now (none when a fence comes first): the
   guest may reuse its command ring once the ioctl returns */
static GpuItem* job_new(const void* entries, u32 n, u32 wait_id) {
    u64 words = 0, size;
    u32 e, pf = 0;
    GpuItem* j;
    const u8* src = (const u8*)entries;
    if (wait_id == ~0u)
        for (pf = 0; pf < n; pf++) {
            u64 en = snx_ld_le64(src + (size_t)pf * 8);
            if (en >> 63) break; /* SYNC: this entry and the rest wait until the stream gets there */
            if (words + entry_words(en) > (64u << 20) / 4) break; /* the rest is fetched when reached */
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
    if (channel >= 128) fatal("gpu: a submission on channel %u, past the 128 the GPU has\n", channel);
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
    idle = !g_qjobs[channel]; /* nothing of the channel is queued or running: its state stands still */
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
    while (runnable() >= QUEUE_ROOM) pc_wait(&g_qcv, &g_qlk, -1);
    g_qjobs[channel]++;
    g_qtotal++;
    queue_append(j);
    pm_unlock(&g_qlk);
    g_gpu_submits++;
    pm_unlock(&g_submit_lk);
}
void gpu_channel_close(u32 channel) {
    GpuChannel* ch;
    if (channel >= 128) return;
    r3d_pending_flush(); /* a draw it merged goes first */
    pm_lock(&g_qlk);
    ch = g_gpu.chan[channel];
    g_gpu.chan[channel] = NULL;
    if (ch) g_gpu.nchan--;
    g_qparked[channel] = 0;
    g_scan_stale[channel] = 1;
    pm_unlock(&g_qlk);
    dma_snap_reset(channel);
    if (ch) {
        if (ch->render_pred) g_renderer->render_predicate_drop(ch->render_pred);
        free(ch->inline_indices);
        free(ch->i2m.buf);
        free(ch);
    }
}
void gpu_release_retire(u32 channel) {
    if (g_renderer->release_retire) g_renderer->release_retire(channel);
}
