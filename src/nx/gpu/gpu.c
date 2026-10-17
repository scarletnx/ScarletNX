/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* GM20B front end: guest memory through the GPU page table, GPFIFO, the puller (semaphores, syncpoints), I2M, the
   method dispatch to the engines (engines.c, maxwell3d.c, compute.c, fermi2d.c, dma.c), and the GPU thread's job
   queue, which keeps each channel's jobs in order and lets a channel whose stream waits (gpu_park) stand aside while
   the others run. Drawing is the renderer's job. */
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"

GpuState g_gpu;
static PMutex g_gpulock; /* the channels and the engines' state: the GPU thread holds it around each job, and
                            gpu_channel_close takes it. The queue has its own lock (g_qlock) */

/* ============================================================ GPU memory */
int g_gpu_releases;
void gmem_need(u64 gva, u64 size) {
    if (__atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE)) g_renderer->release_need(gva, size);
    if (g_cs_any) g_renderer->cs_mem_need(gva, size);
    if (g_gfx_any || g_gfx_img_any) g_renderer->gfx_mem_need(gva, size);
}
void gmem_read_raw(u64 gva, void* dst, u64 size) {
    u8* d = dst;
    while (size) {
        u64 cpu = gmmu_translate(gva);
        u64 n = 0x1000 - (gva & 0xFFF);
        if (n > size) n = size;
        if (!cpu) {
            memset(d, 0, n);
        } else
            memcpy(d, GP(cpu), n);
        d += n;
        gva += n;
        size -= n;
    }
}
int gmem_read(u64 gva, void* dst, u64 size) {
    gmem_need(gva, size);
    gmem_read_raw(gva, dst, size);
    return 0;
}
void (*g_gpu_mem_changed)(u64 cpu_addr, u64 len);
void gpu_mem_changed(u64 cpu_addr, u64 len) {
    if (g_gpu_mem_changed && len) g_gpu_mem_changed(cpu_addr, len);
}
/* a shader program, read up to size bytes: the hardware fetches instructions only up to the program's end, but its
   length isn't known before reading it. A program near the end of guest memory the game committed (Tears of the
   Kingdom's compute programs end close to an uncommitted alias page) would fault a fixed-size read, so the read stops
   at the first page with no guest memory behind it and the rest is zeros */
void gmem_read_code(u64 gva, void* dst, u64 size) {
    u64 n = 0, lo = 0, hi = 0; /* [lo, hi): the committed block the last page was in */
    while (n < size) {
        u64 cpu = gmmu_translate(gva + n);
        if (!cpu) break;
        if (cpu < lo || cpu >= hi) {
            MemBlock b;
            mem_query(cpu, &b);
            if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) break;
            lo = b.addr;
            hi = b.addr + b.size;
        }
        u64 run = hi - cpu; /* committed from here */
        u64 page = 0x1000 - ((gva + n) & 0xFFF);
        n += run < page ? run : page;
    }
    if (n > size) n = size;
    gmem_read(gva, dst, n);
    memset((u8*)dst + n, 0, size - n);
}
/* the renderer lands pending shader results before the stream goes on past them: before a write of
   memory those results overlap (size > 0), and before anything the game can observe, like a semaphore release or a
   syncpoint (gpu_signal_point, size 0) */
void (*g_gpu_before_write)(u64 gva, u64 size);
void (*g_gpu_mapping_begin)(u64 gva, u64 size, int full);
void (*g_gpu_mapping_end)(void);
void gpu_signal_point(void) {
    if (g_gpu_before_write) g_gpu_before_write(0, 0);
}
void gmem_before_write(u64 gva, u64 size) {
    if (size && __atomic_load_n(&g_gpu_releases, __ATOMIC_ACQUIRE)) g_renderer->release_need(gva, size);
    if (g_gpu_before_write && size) g_gpu_before_write(gva, size);
}
void gpu_release_write(u64 gva, const void* data, u32 len) {
    if (g_renderer_up && g_renderer->release_write)
        g_renderer->release_write(gva, data, len);
    else {
        gpu_signal_point();
        gmem_write(gva, data, len);
    }
}
/* the bytes into guest memory, without landing pending shader results first or logging the write */
void gmem_write_raw(u64 gva, const void* src, u64 size) {
    const u8* s = src;
    while (size) {
        u64 cpu = gmmu_translate(gva);
        u64 n = 0x1000 - (gva & 0xFFF);
        if (n > size) n = size;
        if (cpu) memcpy(GP(cpu), s, n);
        s += n;
        gva += n;
        size -= n;
    }
}
int gmem_write(u64 gva, const void* src, u64 size) {
    gmem_before_write(gva, size);
    gmem_write_raw(gva, src, size);
    gpu_mem_written(gva, size);
    return 0;
}
u8* gmem_ptr(u64 gva, u64 size) {
    /* host pointer, only if the whole range is contiguous */
    gmem_need(gva, size);
    return gmem_ptr_untracked(gva, size);
}
u8* gmem_ptr_untracked(u64 gva, u64 size) {
    u64 c0 = gmmu_translate(gva);
    if (!c0) return NULL;
    for (u64 o = (0x1000 - (gva & 0xFFF)); o < size; o += 0x1000) {
        if (gmmu_translate(gva + o) != c0 + o) return NULL;
    }
    return GP(c0);
}
u32 gmem_r32(u64 gva) {
    u32 v = 0;
    gmem_read(gva, &v, 4);
    return v;
}
void gmem_w32(u64 gva, u32 v) { gmem_write(gva, &v, 4); }

/* bytes per pixel of a color format. The 3D engine's render target formats and the 2D engine's surface formats are
   one list (Mesa's nv50 surface formats). These are the guest's bytes: gpu/vk/format.c may store a format wider */
u32 gpu_color_bpp(u32 f) {
    if (f >= 0xC0 && f <= 0xC5) return 16;             /* RGBA32, RGBX32 */
    if (f >= 0xC6 && f <= 0xCE) return 8;              /* RGBA16, RG32, RGBX16 */
    if (f >= 0xE8 && f <= 0xF2) return 2;              /* B5G6R5, BGR5A1, RG8, R16 */
    if (f >= 0xF3 && f <= 0xF7) return 1;              /* R8, A8 */
    if (f == 0xF8 || f == 0xFB || f == 0xFC) return 2; /* BGR5X1 */
    return 4; /* BGRA8, RGBA8, RGB10A2, RG16, R11G11B10, R32, BGRX8, RGBX8, and what isn't known */
}

/* ============================================================ block linear */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    u32 gob_w = (width_bytes + 63) / 64;
    u32 bh = 1u << bh_log2, bd = 1u << bd_log2;
    u32 blocks_h = ((height + 7) / 8 + bh - 1) / bh;
    u32 gx = x_bytes / 64, gy = y / 8;
    u32 block_y = gy / bh, gob_in = gy % bh;
    u32 block_z = z / bd, z_in = z % bd;
    u64 block_size = 512ull * bh * bd;
    u64 off =
        ((u64)block_z * blocks_h * gob_w + (u64)block_y * gob_w + gx) * block_size + ((u64)z_in * bh + gob_in) * 512;
    u32 xb = x_bytes % 64, yb = y % 8;
    off += ((xb % 64) / 32) * 256 + (yb / 2) * 64 + ((xb % 32) / 16) * 32 + (yb % 2) * 16 + (xb % 16);
    return off;
}

void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2) {
    for (u32 y = 0; y < h; y++) {
        u8* d = dst + (u64)y * dst_pitch;
        for (u32 x = 0; x < w_bytes; x += 16) {
            u64 o = bl_offset(x, y, 0, w_bytes, h, bh_log2, 0);
            u32 n = w_bytes - x < 16 ? w_bytes - x : 16;
            memcpy(d + x, src + o, n);
        }
    }
}
void swizzle(u8* dst, const u8* src, u32 src_pitch, u32 w_bytes, u32 h, u32 bh_log2) {
    for (u32 y = 0; y < h; y++) {
        const u8* s = src + (u64)y * src_pitch;
        for (u32 x = 0; x < w_bytes; x += 16) {
            u64 o = bl_offset(x, y, 0, w_bytes, h, bh_log2, 0);
            u32 n = w_bytes - x < 16 ? w_bytes - x : 16;
            memcpy(dst + o, s + x, n);
        }
    }
}

/* ============================================================ puller */
static void puller_method(GpuChannel* ch, u32 m, u32 v) {
    u32* r = ch->puller;
    r[m] = v;
    switch (m) {
        case 0x00: break;
        case 0x07: { /* SemaphoreOperation */
            u64 addr = ((u64)(r[4] & 0xFF) << 32) | r[5];
            u32 op = v & 0x1F;
            if (op == 2) { /* release */
                if (v & (1u << 24)) {
                    gpu_release_write(addr, &r[6], 4);
                } /* 4 bytes only */
                else {
                    u32 q[4] = {r[6], 0, 0, 0};
                    u64 t = gpu_ticks();
                    memcpy(&q[2], &t, 8);
                    gpu_release_write(addr, q, 16);
                }
            } else if (op == 1 || op == 4) { /* acquire (equal) or AcqGeq */
                gpu_park(ch, GPU_PARK_SEMAPHORE, addr, r[6], op == 4);
            } else if (op == 8 || op == 16) { /* AcqAnd and Reduction aren't done: the stream goes on as if they were */
                WARN_LIMIT(4, "gpu: semaphore operation %x at %llx not done\n", op, (unsigned long long)addr);
            }
            break;
        }
        case 0x1D: { /* Syncpoint */
            u32 id = (v >> 8) & 0xFFF;
            if (v & 1) {
                g_renderer->stream_incr(id);
            } else {
                g_renderer->before_syncwait();
                gpu_park(ch, GPU_PARK_SYNCPOINT, id, r[0x1C], 1);
            }
            break;
        }
        default: break;
    }
}

/* ============================================================ inline to memory */
void i2m_launch(GpuChannel* ch, u32* regs, int base) {
    u32 line_len = regs[base + 0], lines = regs[base + 1];
    u64 addr = ((u64)regs[base + 2] << 32) | regs[base + 3];
    u64 len = (u64)line_len * (lines ? lines : 1);
    if (len > (64u << 20)) { /* the data comes inline in the pushbuffer, so this is a bad register value */
        WARN("gpu: inline-to-memory of %llx bytes (%u lines of %u) ignored\n", (unsigned long long)len, lines,
             line_len);
        len = 0;
    }
    ch->i2m_len = (u32)len;
    ch->i2m_pos = 0;
    ch->i2m_addr = addr;
    ch->i2m_linear = (regs[base + 0xC] & 1);
    ch->i2m_pitch = regs[base + 4];
    ch->i2m_w = line_len;
    ch->i2m_lines = lines;
    /* block linear dst. 0x65 = block dims (w 3:0, h 7:4, d 11:8), 0x66..0x68 size, 0x69 z, 0x6A x, 0x6B y.
     note the width field is bits 3:0, height is 7:4 */
    ch->i2m_bh = (regs[base + 5] >> 4) & 0xF;
    ch->i2m_bd = (regs[base + 5] >> 8) & 0xF;
    if (ch->i2m_bh > 5 || ch->i2m_bd > 5) { /* blocks are at most 32 GOBs (log2 5) high and deep */
        WARN_LIMIT(4, "gpu: inline-to-memory block height %u and depth %u (log2) taken as at most 5\n", ch->i2m_bh,
                   ch->i2m_bd);
        if (ch->i2m_bh > 5) ch->i2m_bh = 5;
        if (ch->i2m_bd > 5) ch->i2m_bd = 5;
    }
    ch->i2m_dw = regs[base + 6];
    ch->i2m_dh = regs[base + 7];
    ch->i2m_z = regs[base + 9];
    ch->i2m_x = regs[base + 0xA];
    ch->i2m_y = regs[base + 0xB];
    if (ch->i2m_buf_cap < ch->i2m_len) {
        u8* b = realloc(ch->i2m_buf, (size_t)ch->i2m_len + 4);
        if (!b) {
            WARN("gpu: no memory for %u bytes of inline-to-memory data, ignored\n", ch->i2m_len);
            ch->i2m_len = 0;
            return;
        }
        ch->i2m_buf = b;
        ch->i2m_buf_cap = ch->i2m_len;
    }
}
void i2m_data(GpuChannel* ch, u32 v) {
    if (ch->i2m_pos >= ch->i2m_len) return;
    u32 n = ch->i2m_len - ch->i2m_pos;
    if (n > 4) n = 4;
    memcpy(ch->i2m_buf + ch->i2m_pos, &v, n);
    ch->i2m_pos += n;
    if (ch->i2m_pos >= ch->i2m_len) {
        if (ch->i2m_linear || (ch->i2m_lines <= 1 && !ch->i2m_dw)) { /* single line, no surface: plain copy */
            if (ch->i2m_lines <= 1 || !ch->i2m_pitch || ch->i2m_pitch == ch->i2m_w)
                gmem_write(ch->i2m_addr, ch->i2m_buf, ch->i2m_len);
            else
                for (u32 y = 0; y < ch->i2m_lines; y++)
                    gmem_write(ch->i2m_addr + (u64)y * ch->i2m_pitch, ch->i2m_buf + (u64)y * ch->i2m_w, ch->i2m_w);
        } else { /* bl dst: written 16 bytes at a time, results landed and the write logged once for the range */
            u32 w = ch->i2m_w, h = ch->i2m_lines ? ch->i2m_lines : 1;
            u32 wb = ch->i2m_dw ? ch->i2m_dw : w, hh = ch->i2m_dh ? ch->i2m_dh : h;
            u64 hi = 0;
            for (int pass = 0; pass < 2; pass++) {
                if (pass && hi) {
                    gmem_need(ch->i2m_addr, hi);
                    if (g_gpu_before_write) g_gpu_before_write(ch->i2m_addr, hi);
                }
                for (u32 y = 0; y < h; y++)
                    for (u32 x = 0; x < w;) {
                        u32 xb = ch->i2m_x + x, n = 16 - (xb & 15);
                        if (n > w - x) n = w - x;
                        u64 o = bl_offset(xb, ch->i2m_y + y, ch->i2m_z, wb, hh, ch->i2m_bh, ch->i2m_bd);
                        if (pass)
                            gmem_write_raw(ch->i2m_addr + o, ch->i2m_buf + (u64)y * w + x, n);
                        else if (o + n > hi)
                            hi = o + n;
                        x += n;
                    }
            }
            gpu_mem_written(ch->i2m_addr, hi);
        }
    }
}

/* ============================================================ dispatch */
static void call_method(GpuChannel* ch, u32 sub, u32 m, u32 v, int last) {
    if (m < 0x40) {
        r3d_const_flush(ch);
        if (r3d_pending_on()) r3d_pending_flush();
        puller_method(ch, m, v);
        if (m == 0) { ch->sub_class[sub] = v & 0xFFFF; }
        return;
    }
    u32 cls = ch->sub_class[sub];
    if (r3d_pending_on() &&
        !(cls == 0xB197 || (sub == 0 && cls != 0xB1C0 && cls != 0xA140 && cls != 0x902D && cls != 0xB0B5)))
        r3d_pending_flush(); /* switching engines: flush pending draws */
    if (ch->cb_n && !(cls == 0xB197 || (sub == 0 && cls != 0xB1C0 && cls != 0xA140 && cls != 0x902D && cls != 0xB0B5)))
        r3d_const_flush(ch); /* and the constant buffer data the 3D engine holds */
    switch (cls) {
        case 0xB197: maxwell3d_method(ch, m, v, last); break;
        case 0xB1C0: compute_method(ch, m, v, last); break;
        case 0xA140: kepler_i2m_method(ch, m, v); break;
        case 0x902D: fermi2d_method(ch, m, v); break;
        case 0xB0B5: dma_method(ch, m, v); break;
        default:
            if (sub == 0)
                maxwell3d_method(ch, m, v, last);
            else if (sub == 1)
                compute_method(ch, m, v, last);
            else if (sub == 2)
                kepler_i2m_method(ch, m, v);
            else if (sub == 3)
                fermi2d_method(ch, m, v);
            else if (sub == 4)
                dma_method(ch, m, v);
            break;
    }
}

typedef int PbMethod(void* ctx, u32 sub, u32 m, u32 v, int last);
/* calls fn for each method of the pushbuffer words w[0, n) of channel chan (for the log), log: say what looks wrong.
   Method batches can spill over into the next GPFIFO entry (hw does this, so does yuzu's DmaPusher): b keeps the
   batch state across entries or the leftover data gets parsed as headers */
static inline u32 pb_walk(PbBatch* b, const u32* w, u32 i, u32 n, u32 chan, int log, PbMethod* fn, void* ctx) {
    for (;;) {
        if (b->k < b->cnt) {
            u32 mode = b->mode, sub = b->sub, m = b->m;
            while (b->k < b->cnt && i < n) {
                u32 k = b->k++;
                u32 mm = mode == 1 ? m + k : mode == 5 ? (k == 0 ? m : m + 1) : m;
                if (fn(ctx, sub, mm, w[i++], b->k == b->cnt)) return i;
            }
            if (b->k < b->cnt) {
                if (log)
                    LOG_LIMIT(
                        6,
                        "gpu: method batch continues in the next GPFIFO entry (sub %u method %x mode %u, %u of %u "
                        "words done)\n",
                        b->sub, b->m, b->mode, b->k, b->cnt);
                return i;
            } /* rest comes in the next entry */
        }
        if (i >= n) return i;
        u32 h = w[i++];
        u32 mode = h >> 29, sub = (h >> 13) & 7, m = h & 0xFFF, cnt = (h >> 16) & 0x1FFF;
        if (log && h && (mode == 0 || mode == 2 || mode >= 6)) { /* shouldn't happen in a sane stream, log it */
            static int said;
            if (said < 6) {
                said++;
                LOG("gpu: odd command header %08x (mode %u) at word %u of %u in channel %u; words around:", h, mode,
                    i - 1, n, chan);
                for (u32 j = i > 5 ? i - 5 : 0; j < i + 6 && j < n; j++) LOG(" %s%08x", j == i - 1 ? "*" : "", w[j]);
                LOG("\n");
            }
        }
        switch (mode) {
            case 1:
            case 3:
            case 5:
                b->mode = mode;
                b->sub = sub;
                b->m = m;
                b->cnt = cnt;
                b->k = 0;
                break;
            /* modes 0/2 (old Fermi), 6, 7 carry no data words (same as DmaPusher). don't treat them as batches,
         a stray one would eat the following methods incl. syncpoint incrs */
            case 4:
                if (fn(ctx, sub, m, cnt, 1)) return i;
                break;
            default: break;
        }
    }
}
static int run_method(void* ctx, u32 sub, u32 m, u32 v, int last) {
    GpuChannel* ch = ctx;
    call_method(ch, sub, m, v, last);
    return ch->park != 0;
}
static u32 run_pushbuffer(GpuChannel* ch, const u32* w, u32 i, u32 n) {
    return pb_walk(&ch->pb, w, i, n, ch->id, 1, run_method, ch);
}

/* Each channel's stream as submissions are queued, in nvdrv's submit order (the GPU queue's), followed only as far as
   the copy engine: its launches with a linear source have the source read now (dma_snap_launch, which says why). A
   stream that waits first, on a semaphore acquire or a syncpoint, gets none for the rest of that submission: the CPU
   may write the source after the submit and then release the wait */
typedef struct {
    PbBatch pb;
    u32 id, sub_class[8];
    int waits;
    u32 rdma[0x200];
    u64 job_id;
    u32 launch_index;
} SubmitScan;
static SubmitScan* g_scan[128]; /* by channel (syncpoint id), made at its first submission */
static int scan_method(void* ctx, u32 sub, u32 m, u32 v, int last) {
    SubmitScan* s = ctx;
    u32 cls;
    (void)last;
    if (m < 0x40) { /* puller methods, as puller_method and call_method read them */
        if (m == 0)
            s->sub_class[sub] = v & 0xFFFF;
        else if ((m == 0x07 && ((v & 0x1F) == 1 || (v & 0x1F) == 4 || (v & 0x1F) == 8)) || (m == 0x1D && !(v & 1)))
            s->waits = 1;
        return s->waits;
    }
    cls = s->sub_class[sub];
    if ((cls == 0xB197 && m >= 0xE00) || ((cls == 0xB197 || cls == 0xB1C0) && m == 0x6C3)) {
        s->waits = 1; /* macros and report acquires can wait before later DMA reads */
        return 1;
    }
    if (cls != 0xB0B5 && (sub != 4 || cls == 0xB197 || cls == 0xB1C0 || cls == 0xA140 || cls == 0x902D)) return 0;
    s->rdma[m & 0x1FF] = v; /* dma_method's registers */
    if ((m & 0x1FF) == 0xC0) dma_snap_launch(s->id, s->job_id, s->launch_index++, s->rdma, v);
    return 0;
}
static void scan_submission(const void* entries, u32 n, u32 channel, u64 job_id, const u32* prefetched, int deferred) {
    SubmitScan* s = g_scan[channel & 127];
    u32 k;
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) fatal("gpu: out of memory for a channel\n");
        s->id = channel;
        s->sub_class[0] = 0xB197; /* chan_get's */
        s->sub_class[1] = 0xB1C0;
        s->sub_class[2] = 0xA140;
        s->sub_class[3] = 0x902D;
        s->sub_class[4] = 0xB0B5;
        g_scan[channel & 127] = s;
    }
    s->job_id = job_id;
    s->launch_index = 0;
    u64 at = 0;
    for (k = 0; k < n; k++) {
        u64 entry;
        memcpy(&entry, (const u8*)entries + (u64)k * 8, 8);
        u32 words = (u32)((entry >> 42) & 0x1FFFFF);
        if (s->waits || !prefetched || (entry >> 63)) break;
        pb_walk(&s->pb, prefetched + at, 0, words, channel, 0, scan_method, s);
        at += words;
    }
    if (k < n || deferred) s->waits = 1;
}

static GpuChannel* chan_get(u32 id) {
    for (int i = 0; i < g_gpu.nchan; i++)
        if (g_gpu.chan[i]->id == id) return g_gpu.chan[i];
    if (g_gpu.nchan == (int)(sizeof g_gpu.chan / sizeof g_gpu.chan[0])) fatal("gpu: too many channels\n");
    GpuChannel* c = calloc(1, sizeof *c);
    if (!c) fatal("gpu: out of memory for a channel\n");
    c->id = id;
    c->sub_class[0] = 0xB197;
    c->sub_class[1] = 0xB1C0;
    c->sub_class[2] = 0xA140;
    c->sub_class[3] = 0x902D;
    c->sub_class[4] = 0xB0B5;
    /* hw reset values that NVN/deko3d rely on and never write. sRGB writes default to on,
     otherwise sRGB targets are stored linear and come out too dark */
    c->r3d[0x56E] = 1;
    c->r3d[0x556] = 1; /* rendering starts enabled, until SetRenderEnable supplies a condition */
    /* Leave render-enable override at UseRenderEnable (0). NVN's conditional target initialization clears
       must stop once their metadata matches; AlwaysRender would erase a target again after drawing it. */
    /* the color write masks default to enabled: games that never set them draw nothing at all
       (Eden defaults these for Sonic Mania; Unicorn Overlord and Fire Emblem need it too) */
    memset(&c->r3d[0x680], 0x11, 8 * sizeof c->r3d[0]);
    memcpy(c->r3d_shadow, c->r3d, sizeof c->r3d_shadow);
    g_gpu.chan[g_gpu.nchan++] = c;
    return c;
}

void gpu_channel_close(u32 channel) {
    pm_lock(&g_gpulock);
    for (int i = 0; i < g_gpu.nchan; i++)
        if (g_gpu.chan[i]->id == channel) {
            GpuChannel* c = g_gpu.chan[i];
            g_gpu.chan[i] = g_gpu.chan[--g_gpu.nchan];
            free(c->i2m_buf);
            free(c->inline_indices);
            free(c);
            break;
        }
    /* the channel's id comes back only after this (gpu_channel_retire frees the syncpoint next), so no submission
       of a new channel was scanned yet */
    free(g_scan[channel & 127]);
    g_scan[channel & 127] = NULL;
    dma_snap_reset(channel);
    pm_unlock(&g_gpulock);
}
void gpu_release_retire(u32 channel) {
    if (g_renderer_up && g_renderer->release_retire) g_renderer->release_retire(channel);
}

/* ============================================================ waits */
/* how long a stream waits before it goes on anyway, so a bad value can't stop its channel for good (the console
   raises an interrupt) */
static u64 park_timeout(u32 kind) { return kind == GPU_PARK_REPORT ? 1000000000ull : 2000000000ull; }
/* how soon a semaphore wait reads memory again when nothing woke the GPU thread. A release from another channel's
   stream, a syncpoint increment or a new submission makes it look again at once, but a release the game's CPU code
   writes comes with no wake: it is polled, as Host polls a failed acquire every ACQUIRE_RETRY cycles (on Windows the
   wait rounds up to the timer's millisecond) */
#define GPU_ACQUIRE_RETRY_NS 500000ull
static int park_on_syncpt(const GpuChannel* ch) { return ch->park == GPU_PARK_SYNCPOINT || ch->park == GPU_PARK_FENCE; }
static int park_met(const GpuChannel* ch) {
    if (park_on_syncpt(ch)) return (i32)(syncpt_read((u32)ch->park_addr) - ch->park_val) >= 0;
    u32 cur = gmem_r32(ch->park_addr);
    return ch->park_geq ? (i32)(cur - ch->park_val) >= 0 : cur == ch->park_val;
}
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq) {
    /* the stream reads past a release: what the CPU or host1x wrote before releasing is the stream's to see, even
       when the release came before the stream got here */
    g_renderer->mem_may_have_changed();
    if ((kind == GPU_PARK_SYNCPOINT || kind == GPU_PARK_FENCE) && addr >= SYNCPT_IDS) {
        WARN_LIMIT(4, "gpu: channel %u's stream waits for syncpoint %llu, past the %u there are, and goes on\n", ch->id,
                   (unsigned long long)addr, SYNCPT_IDS);
        return;
    }
    ch->park = kind;
    ch->park_addr = addr;
    ch->park_val = val;
    ch->park_geq = geq;
    if (park_met(ch))
        ch->park = 0;
    else
        ch->park_end = plat_ns() + park_timeout(kind);
}
/* 1 when the channel can go on: it waits for nothing, what it waits for came, or it waited past its time. Otherwise
   *wake moves up to when to look again */
static int park_over(GpuChannel* ch, u64* wake) {
    if (!ch->park) return 1;
    if (!park_met(ch)) {
        u64 now = plat_ns();
        if (now < ch->park_end) {
            u64 t = ch->park_end;
            if (!park_on_syncpt(ch) && now + GPU_ACQUIRE_RETRY_NS < t) t = now + GPU_ACQUIRE_RETRY_NS;
            if (t < *wake) *wake = t;
            return 0;
        }
        u32 ms = (u32)(park_timeout(ch->park) / 1000000);
        if (ch->park == GPU_PARK_FENCE)
            WARN_LIMIT(16,
                       "gpu: a submission on channel %u waited %u ms for its fence, syncpoint %u to reach %u (it is at "
                       "%u), and runs anyway\n",
                       ch->id, ms, (u32)ch->park_addr, ch->park_val, syncpt_read((u32)ch->park_addr));
        else if (ch->park == GPU_PARK_SYNCPOINT)
            WARN_LIMIT(16, "gpu: channel %u waited %u ms for syncpoint %u to reach %u (it is at %u), and goes on\n",
                       ch->id, ms, (u32)ch->park_addr, ch->park_val, syncpt_read((u32)ch->park_addr));
        else
            WARN_LIMIT(16, "gpu: channel %u waited %u ms for %s %llx to %s %x (it has %x), and goes on\n", ch->id, ms,
                       ch->park == GPU_PARK_REPORT ? "report semaphore" : "semaphore",
                       (unsigned long long)ch->park_addr, ch->park_geq ? "reach" : "equal", ch->park_val,
                       gmem_r32(ch->park_addr));
    }
    ch->park = 0;
    /* what ended the wait may be the game's CPU code, which can have written the data the stream reads next */
    g_renderer->mem_may_have_changed();
    return 1;
}

/* Submissions run on a thread of their own, as on the console: the ioctl only queues them, and the fence
   it returns (the channel's syncpoint, incremented once the job is done) tells the game when the work is complete.
   Processed inside the ioctl, the work held the game's submit lock for most of each frame: Tears of the Kingdom's
   main thread waited on it nearly all the time and loading took minutes.
   Each channel's jobs run in order. A channel whose stream waits (gpu_park) holds up only its own later jobs and the
   calls (gpu_call) that wait for it, and the GPU thread goes on with the others', which may be what it waits for:
   NVN and deko3d wait on one queue for a semaphore another queue's stream releases, and the waiting queue can be
   flushed first. The jobs are a list, oldest first: game threads add at the end, the GPU thread removes any job that
   is done, and reads only the jobs that were there when it last looked under the lock. A list of nodes, not an
   array with index links: the GPU thread reads a job without the lock while game threads add, so a job must never
   move, and jobs held behind a wait have no bound to size an array by */
typedef struct GpuJob {
    struct GpuJob* next;
    u32 channel, incs;
    u32 wait_id, wait_val; /* a fence to wait for first (wait_id ~0: none, or the wait is asked for) */
    u32 expect;            /* the syncpoint value the job's fence promised */
    void (*fn)(void*);     /* or a call (gpu_call), made once the jobs before it on chans[0..nchans) are done */
    void* arg;
    u32 chans[4], nchans;
    u32 k, i; /* where the job goes on: word i of entry k */
    u64 id;
    u32 dma_launch_index;
    u32 n; /* GPFIFO entries */
    u32* prefetched;
    u32* current; /* fetched commands up to the next SYNC entry, retained while parked */
    struct {
        u64 value;
        const u32* words;
    } e[];
} GpuJob;
/* how many jobs that can run may wait in the queue before a submission waits for room. Jobs held behind a wait
   don't count: the submission that releases it must get in */
#define GPU_QUEUE 256
static GpuJob *g_qfirst, *g_qlast;
static u32 g_qn;    /* jobs queued and not done */
static u32 g_qheld; /* of those, the jobs the GPU thread's last look couldn't run */
static u32 g_qgen;  /* bumped by every job queued and every gpu_kick: the GPU thread sleeps only on the latest */
static PMutex g_qlock = PMUTEX_INIT; /* gpu_kick may come before gpu_init */
static PCond g_qcv;
static int g_qthread;
static u64 g_job_id;

/* runs a job as far as it goes, and returns 1 when it's done. *ran is set when any of it ran */
static int gpu_job_run(GpuJob* j, u64* wake, int* ran) {
    pm_lock(&g_gpulock);
    GpuChannel* ch = chan_get(j->channel);
    ch->dma_job_id = j->id;
    ch->dma_launch_index = j->dma_launch_index;
    if (j->wait_id != ~0u) { /* the fence is the stream's first wait */
        gpu_park(ch, GPU_PARK_FENCE, j->wait_id, j->wait_val, 1);
        j->wait_id = ~0u;
    }
    if (!park_over(ch, wake)) {
        pm_unlock(&g_gpulock);
        return 0;
    }
    *ran = 1;
    if (ch->mme.running) mme_resume(ch); /* a macro a wait stopped goes on before the stream's next word */
    while (j->k < j->n && !ch->park) {
        u64 ent = j->e[j->k].value;
        u32 words = (u32)((ent >> 42) & 0x1FFFFF);
        if (j->i < words) {
            const u32* buf = j->e[j->k].words;
            if (!buf) {
                /* SYNC stops command prefetch until the preceding work has executed. Once reached, fetch
                   this region through (but not including) the next SYNC before any of its commands can release
                   memory back to the CPU. Fetching one entry at a time lets those releases invalidate later
                   control commands in the same region. A parked stream keeps the fetched region intact. */
                u32 end = j->k + 1;
                u64 total = words;
                while (end < j->n && !(j->e[end].value >> 63)) {
                    total += (j->e[end].value >> 42) & 0x1fffff;
                    end++;
                }
                free(j->current);
                j->current = malloc(total * 4);
                if (!j->current) fatal("Out of memory fetching GPU commands");
                u64 at = 0;
                for (u32 k = j->k; k < end; k++) {
                    u64 entry = j->e[k].value;
                    u32 nw = (entry >> 42) & 0x1fffff;
                    j->e[k].words = j->current + at;
                    gmem_read(entry & 0xfffffffffcull, j->current + at, (u64)nw * 4);
                    at += nw;
                }
                buf = j->e[j->k].words;
            }
            j->i = run_pushbuffer(ch, buf, j->i, words);
            if (ch->park) break;
        }
        j->k++;
        j->i = 0;
    }
    int parked = ch->park != 0;
    j->dma_launch_index = ch->dma_launch_index;
    r3d_const_flush(ch);
    r3d_pending_flush();
    pm_unlock(&g_gpulock);
    if (parked) return 0;
    dma_snap_finish(j->channel, j->id);
    /* the submission's own increments, after its work like the stream's (nothing else of a job's end is seen) */
    for (u32 i = 0; i < j->incs; i++) g_renderer->stream_incr(j->channel);
    /* a stream that increments less than its fence promised leaves whoever waits for the fence waiting 2 s. Increments
       the renderer still holds count: they happen once the work before them lands */
    u32 value = g_renderer->release_value ? g_renderer->release_value(j->channel) : syncpt_read(j->channel);
    if ((i32)(value - j->expect) < 0)
        WARN_LIMIT(16, "gpu: channel %u's submission left its syncpoint at %u, short of the %u its fence promised\n",
                   j->channel, syncpt_read(j->channel), j->expect);
    return 1;
}
static int chan_in(const u32* set, u32 n, u32 c) {
    for (u32 i = 0; i < n; i++)
        if (set[i] == c) return 1;
    return 0;
}
/* one look at the queue, oldest first: a job goes on unless an older job of its channel isn't done, and a call once
   no older job of the channels it names is left and no older call is. Returns 1 when anything ran */
static int gpu_pass(u64* wake) {
    u32 held[sizeof g_gpu.chan / sizeof g_gpu.chan[0]]; /* the channels with a job not done so far: their later jobs
                                                          wait behind it */
    u32 nheld = 0, skipped = 0;
    int ran = 0, call_held = 0;
    pm_lock(&g_qlock);
    GpuJob* last = g_qlast; /* the jobs after it were added since: their links aren't read */
    GpuJob *prev = NULL, *j = g_qfirst;
    pm_unlock(&g_qlock);
    while (j) {
        int is_last = j == last, done = 0;
        if (j->fn) {
            u32 c = 0;
            while (c < j->nchans && !chan_in(held, nheld, j->chans[c])) c++;
            if (!call_held && c == j->nchans) {
                j->fn(j->arg);
                done = ran = 1;
            } else
                call_held = 1; /* calls stay in order */
        } else if (!chan_in(held, nheld, j->channel)) {
            done = gpu_job_run(j, wake, &ran);
            if (!done && nheld < sizeof held / sizeof held[0]) held[nheld++] = j->channel;
        }
        GpuJob* next = is_last ? NULL : j->next;
        if (done) {
            pm_lock(&g_qlock);
            if (prev)
                prev->next = j->next;
            else
                g_qfirst = j->next;
            if (g_qlast == j) g_qlast = prev;
            g_qn--;
            pc_broadcast(&g_qcv); /* room for a gpu_queue waiting */
            pm_unlock(&g_qlock);
            free(j->prefetched);
            free(j->current);
            free(j);
        } else {
            skipped++;
            prev = j;
        }
        j = next;
    }
    pm_lock(&g_qlock);
    g_qheld = skipped;
    pc_broadcast(&g_qcv);
    pm_unlock(&g_qlock);
    return ran;
}
static void gpu_thread(void* p) {
    (void)p;
    for (;;) {
        pm_lock(&g_qlock);
        while (!g_qn) pc_wait(&g_qcv, &g_qlock, -1);
        u32 gen = g_qgen;
        pm_unlock(&g_qlock);
        u64 wake = ~0ull;
        if (gpu_pass(&wake)) continue;
        /* every job waits: until a job is queued, a kick, or a semaphore wait is due to read memory again. A wake that
           is already past (wake <= now) looks again at once */
        pm_lock(&g_qlock);
        if (g_qgen == gen) {
            u64 now = plat_ns();
            if (wake == ~0ull)
                pc_wait(&g_qcv, &g_qlock, -1);
            else if (wake > now)
                pc_wait(&g_qcv, &g_qlock, (i64)(wake - now));
        }
        pm_unlock(&g_qlock);
    }
}
void gpu_kick(void) {
    pm_lock(&g_qlock);
    g_qgen++;
    if (g_qthread) pc_broadcast(&g_qcv);
    pm_unlock(&g_qlock);
}

void gpu_init(void) {
    pm_init(&g_gpulock);
    pc_init(&g_qcv);
}

static void gpu_queue(GpuJob* j) {
    pm_lock(&g_qlock);
    if (!g_qthread) {
        g_qthread = 1;
        if (plat_thread(gpu_thread, NULL, (size_t)16 << 20)) fatal("could not start the GPU thread");
    }
    /* signed: a pass removing jobs the last one held leaves g_qn below g_qheld until it ends */
    while ((i64)g_qn - (i64)g_qheld >= GPU_QUEUE) pc_wait(&g_qcv, &g_qlock, -1);
    j->next = NULL;
    if (g_qlast)
        g_qlast->next = j;
    else
        g_qfirst = j;
    g_qlast = j;
    g_qn++;
    g_qgen++;
    pc_broadcast(&g_qcv);
    pm_unlock(&g_qlock);
}
volatile u64 g_gpu_submits;
void gpu_submit(const void* e, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect) {
    g_gpu_submits++;
    g_renderer->mem_may_have_changed();
    GpuJob* j = calloc(1, sizeof *j + (u64)n * sizeof j->e[0]);
    if (!j) fatal("Out of memory queueing a GPU submission");
    j->n = n;
    j->channel = channel;
    j->incs = incs;
    j->wait_id = wait_id;
    j->wait_val = wait_val;
    j->expect = expect;
    j->id = __atomic_add_fetch(&g_job_id, 1, __ATOMIC_RELAXED);
    /* SYNC (bit 63) postpones prefetch until the GPU reaches that entry. A submission fence also postpones reads.
       Copy the earlier commands before returning to the guest, which may reuse its command ring. */
    u64 total = 0;
    u32 prefetch_end = n;
    for (u32 k = 0; k < n; k++) {
        memcpy(&j->e[k].value, (const u8*)e + (u64)k * 8, 8);
        if (k < prefetch_end && (wait_id != ~0u || (j->e[k].value >> 63))) prefetch_end = k;
        if (k < prefetch_end) total += (j->e[k].value >> 42) & 0x1fffff;
    }
    if (total) {
        j->prefetched = malloc(total * 4);
        if (!j->prefetched) fatal("Out of memory prefetching GPU commands");
        u64 at = 0;
        for (u32 k = 0; k < prefetch_end; k++) {
            u64 entry = j->e[k].value;
            u32 words = (entry >> 42) & 0x1fffff;
            j->e[k].words = j->prefetched + at;
            gmem_read_raw(entry & 0xfffffffffcull, j->prefetched + at, (u64)words * 4);
            at += words;
        }
    }
    scan_submission(e, prefetch_end, channel, j->id, j->prefetched, prefetch_end != n);
    gpu_queue(j);
}
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    GpuJob* j = calloc(1, sizeof *j);
    if (!j) fatal("Out of memory queueing a GPU call");
    if (nchans > 4) fatal("gpu_call: %u channels, 4 at most", nchans);
    j->fn = fn;
    j->arg = arg;
    j->wait_id = ~0u;
    if (nchans) memcpy(j->chans, chans, nchans * sizeof *chans);
    j->nchans = nchans;
    gpu_queue(j);
}
