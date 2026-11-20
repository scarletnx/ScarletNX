/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The movie library's JPEG decoder and encoder (tegraJPEG), native (tvmr_jpeg.hook): the TVMRJPEG functions, which
   drive NVJPG through /dev/nvhost-nvjpg on the console, done with FFmpeg's mjpeg decoder and encoder. The library's
   NvMMLite image block calls them for OMX.Nvidia.jpeg.decoder, .mjpeg.decoder and .jpeg.encoder. What each function
   does was read in the library of movie 0.12.16 and 1.4.2 (32-bit), 1.5.0, 3.5.1, 4.4.1, 4.5.0 and 7.3.2.

   The caller keeps the decoder and the encoder that Create gives and passes them back: it reads no field of them.
   So each is an object of the runtime's own in guest memory (TvmrJpegObj, by its address), where the decoder's three
   public fields are as the SDK's (+0 the maximum width, +2 the height, +4 the bitstream's bytes, as u16, u16, u32).

   The pictures are in pitch linear surfaces: TVMRVideoSurface {u32 type, width, height; plane objects at +0x10, one
   pointer each}, a plane object {u32 pitch; a CPU pointer the caller doesn't fill; NvRmSurface* at +0x10}, and the
   NvRmSurface {+0 width, +4 height, +0x10 layout (1 pitch linear), +0x14 pitch, +0x18 the nvmap handle, +0x1c the
   offset in it}. A 32-bit game has the plane objects at +0xc and the NvRmSurface at +8. NVJPG takes the nvmap handle
   and the offset, and so does the runtime: the plane is at the handle's memory plus the offset (nvmap_get).

   NVJPG works on its own: the functions hand back a fence that the caller waits for (an NvRmFence {u32 syncpoint,
   u32 value}). Here the work is done when the function returns, so the fence is {~0, 0}, which the caller and the
   SDK's own waits pass over. The fences the caller gives to wait for before the work are waited for (syncpt_wait).

   What NVJPG does that FFmpeg doesn't:
   - The encoder's DC quantizers are 8, as FFmpeg's mjpeg encoder has them, where NVJPG takes the table's first
     entry. Its AC quantizers are the table's. Its Huffman tables are the standard ones, its JPEG has no restart
     interval, and a target size changes nothing: the caller's own tables, restart interval and target are logged
     once and not used.
   - Render writes RGBA (the caller's 0x23, A8B8G8R8). Its other three RGB types are NVJPG's other byte orders,
     which aren't known: those are refused with the status of a format NVJPG doesn't take.
   - Chroma that RenderYUV writes to a surface of other chroma than the JPEG's, and Render's RGB, take the nearest
     chroma sample. Which of the two chroma planes is Cb follows the order NVJPG takes them in, Cb first: a guess */
#include "nx/nn/tvmr_jpeg.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/services_int.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <stdlib.h>
#include <string.h>

/* the SDK's statuses, as its code returns them */
#define ST_OK          0u
#define ST_BAD         1u /* a bad parameter, or a picture larger than its surface */
#define ST_NOT_READY   2u /* the encoder: the frame isn't done */
#define ST_NONE        3u /* the encoder: no frame in flight */
#define ST_FULL        4u /* a bitstream larger than the decoder's, an encoder with every buffer in flight, or room
                             too small for the JPEG */
#define ST_UNSUPPORTED 6u /* a JPEG or a format NVJPG doesn't decode */

#define NO_FENCE   0xFFFFFFFFu /* an NvRmFence's syncpoint when there is nothing to wait for */
#define MAX_FENCES 16u         /* the fences one call waits for, at most */
#define MAX_STREAM (64u << 20) /* the largest bitstream taken */
#define MAX_DIM    16384u      /* JPEG's largest picture side is 65535: NVJPG takes far less */

/* where a game's ABI keeps what the functions read: the movie library of 64-bit games up to 4.x, of 7.x, and of
   32-bit games */
typedef struct {
    u8 ptr;          /* a pointer's bytes */
    u8 planes;       /* the first plane object's pointer in a TVMRVideoSurface (one pointer each after it) */
    u8 out_plane;    /* the plane object's pointer in a TVMROutputSurface (Render's) */
    u8 plane_surf;   /* the NvRmSurface's pointer in a plane object */
    u8 rgba;         /* Render's type for RGBA: the next three are the other byte orders */
    u16 info;        /* GetInfo's info: 6 bytes, 0x108 with the marker segments the parser skipped (movie 7) */
    u8 null_dst_ok;  /* the encoder's GetBits and GetBitsEx with no destination drop the frame (movie 7), where the
                        others refuse it */
} Abi;
static const Abi g_abi64 = {8, 0x10, 0x10, 0x10, 0x23, 6, 0}, g_abi64_7 = {8, 0x10, 0x10, 0x10, 0x26, 0x108, 1},
                 g_abi32 = {4, 0xc, 0xc, 8, 0x23, 6, 0};

/* the NvRmSurface's fields */
#define RM_WIDTH  0x00
#define RM_HEIGHT 0x04
#define RM_LAYOUT 0x10
#define RM_PITCH  0x14
#define RM_HMEM   0x18
#define RM_OFFSET 0x1c
#define RM_PITCH_LINEAR 1u

static u64 ld_ptr(const CPU* c, const Abi* a, u64 addr) {
    return a->ptr == 8 ? snx_ld_le64(hook_host(c, addr)) : snx_ld_le32(hook_host(c, addr));
}
static int readable(u64 addr, u64 size) { return addr && snx_audio_guest_ok(addr, size, PERM_R); }
static int writable(u64 addr, u64 size) { return addr && snx_audio_guest_ok(addr, size, PERM_W); }

/* ------------------------------------------------------------ the objects */
/* the object of kind at guest address addr, or NULL; under g_host.tvmr_jpeg.lk */
static TvmrJpegObj* obj_of(u64 addr, int kind) {
    const HleTvmrJpeg* s = &g_hle.tvmr_jpeg;
    u64 off = addr - s->base;
    if (!s->base || addr < s->base || off >= (u64)TVMRJPEG_OBJECTS * TVMRJPEG_OBJECT || off % TVMRJPEG_OBJECT)
        return NULL;
    TvmrJpegObj* o = &g_hle.tvmr_jpeg.obj[off / TVMRJPEG_OBJECT];
    return o->kind == kind ? o : NULL;
}
static u32 obj_index(const TvmrJpegObj* o) { return POOL_INDEX(g_hle.tvmr_jpeg.obj, o); }
static u64 obj_addr(const TvmrJpegObj* o) { return g_hle.tvmr_jpeg.base + (u64)obj_index(o) * TVMRJPEG_OBJECT; }

/* a new object of kind, its guest bytes zero; NULL with all taken (the SDK's Create gives NULL when it can't make
   one). Under the lock */
static TvmrJpegObj* obj_new(CPU* c, int kind) {
    HleTvmrJpeg* s = &g_hle.tvmr_jpeg;
    if (!s->base) s->base = kmem_alloc((u64)TVMRJPEG_OBJECTS * TVMRJPEG_OBJECT, MS_KERNEL, PERM_RW);
    TvmrJpegObj* o = POOL_TRY(s->obj, s->next);
    if (!o) {
        WARN_LIMIT(4, "tvmr_jpeg: a %s asked for with all %d open: none given\n",
                   kind == TVMRJPEG_DECODER ? "decoder" : "encoder", TVMRJPEG_OBJECTS);
        return NULL;
    }
    o->kind = (u8)kind;
    memset(hook_host(c, obj_addr(o)), 0, TVMRJPEG_OBJECT);
    return o;
}
static void obj_free(CPU* c, TvmrJpegObj* o) {
    u32 i = obj_index(o);
    for (u32 b = 0; b < TVMRJPEG_BUFS; b++) {
        free(g_host.tvmr_jpeg.jpeg[i][b]);
        g_host.tvmr_jpeg.jpeg[i][b] = NULL;
    }
    memset(hook_host(c, obj_addr(o)), 0, TVMRJPEG_OBJECT);
    POOL_PUT(o);
}
static void lock(void) { pm_lock(&g_host.tvmr_jpeg.lk); }
static void unlock(void) { pm_unlock(&g_host.tvmr_jpeg.lk); }

/* ------------------------------------------------------------ fences and planes */
/* the NULL-terminated list of NvRmFence pointers at list (NULL for none), each waited for. With no lock held: it
   waits in the kernel */
static void wait_fences(CPU* c, const Abi* a, u64 list) {
    for (u32 i = 0; list && i < MAX_FENCES; i++) {
        u64 at = list + (u64)i * a->ptr, f;
        if (!readable(at, a->ptr) || !(f = ld_ptr(c, a, at)) || !readable(f, 8)) return;
        u32 id = snx_ld_le32(hook_host(c, f)), value = snx_ld_le32(hook_host(c, f + 4));
        if (id != NO_FENCE) syncpt_wait(id, value, -1);
    }
}
/* the fence of work that is done, at f (NULL for none) */
static void done_fence(CPU* c, u64 f) {
    if (!writable(f, 8)) return;
    snx_st_le32(hook_host(c, f), NO_FENCE);
    snx_st_le32(hook_host(c, f + 4), 0);
}

typedef struct {
    u8* p; /* the plane's first byte in host memory, NULL when it isn't there */
    u32 pitch, width, height, layout;
} Plane;
/* the NvRmSurface of the plane object at obj */
static u64 plane_surface(const CPU* c, const Abi* a, u64 obj) {
    return readable(obj, (u64)a->plane_surf + a->ptr) ? ld_ptr(c, a, obj + a->plane_surf) : 0;
}
/* the plane whose NvRmSurface is at rm, with rows rows of row_bytes bytes in its memory: what the GPU holds of them
   lands first. p NULL when its memory is not a live nvmap handle's, or too small */
static Plane plane_at(CPU* c, u64 rm, u32 rows, u32 row_bytes) {
    Plane pl = {0};
    u64 base;
    u32 size;
    if (!readable(rm, RM_OFFSET + 4)) return pl;
    pl.width = snx_ld_le32(hook_host(c, rm + RM_WIDTH));
    pl.height = snx_ld_le32(hook_host(c, rm + RM_HEIGHT));
    pl.layout = snx_ld_le32(hook_host(c, rm + RM_LAYOUT));
    pl.pitch = snx_ld_le32(hook_host(c, rm + RM_PITCH));
    u32 h = snx_ld_le32(hook_host(c, rm + RM_HMEM)), off = snx_ld_le32(hook_host(c, rm + RM_OFFSET));
    if (!rows || !row_bytes) return pl;
    u64 need = (u64)(rows - 1) * pl.pitch + row_bytes;
    if (row_bytes > pl.pitch || !nvmap_get(h, &base, &size) || (u64)off + need > size) {
        WARN_LIMIT(8,
                   "tvmr_jpeg: a plane of nvmap handle %lu at %#lx, pitch %lu, has no room for %lu rows of %lu "
                   "bytes\n",
                   (unsigned long)h, (unsigned long)off, (unsigned long)pl.pitch, (unsigned long)rows,
                   (unsigned long)row_bytes);
        return pl;
    }
    if (own_any()) own_acquire(base + off, need);
    pl.p = hook_host(c, base + off);
    return pl;
}

/* ------------------------------------------------------------ the bitstream */
/* the count buffers {pointer, u32 bytes} at bufs, one after another: a buffer of the runtime's with their bytes and
   FFmpeg's padding after them, NULL when one can't be read */
static u8* gather(const CPU* c, const Abi* a, u32 count, u64 bufs, u32* n) {
    u64 total = 0, stride = 2u * a->ptr;
    *n = 0;
    if (!count || !readable(bufs, stride * count)) return NULL;
    for (u32 i = 0; i < count; i++) total += snx_ld_le32(hook_host(c, bufs + i * stride + a->ptr));
    if (total > MAX_STREAM) return NULL;
    u8* s = malloc((size_t)total + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!s) return NULL;
    for (u32 i = 0, at = 0; i < count; i++) {
        u64 p = ld_ptr(c, a, bufs + i * stride);
        u32 bytes = snx_ld_le32(hook_host(c, bufs + i * stride + a->ptr));
        if (bytes && !readable(p, bytes)) {
            free(s);
            return NULL;
        }
        if (bytes) memcpy(s + at, hook_host(c, p), bytes);
        at += bytes;
    }
    memset(s + total, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *n = (u32)total;
    return s;
}

/* what the SDK's parser reads of a JPEG, up to its first scan */
enum { CHROMA_GRAY, CHROMA_420, CHROMA_422, CHROMA_440, CHROMA_444 };
typedef struct {
    u32 w, h, chroma; /* chroma: CHROMA_* */
    u32 mcu_w, mcu_h; /* the picture's size in whole MCUs */
} Jpeg;
/* ST_OK when NVJPG decodes it: baseline or extended sequential (SOF0, SOF1) with 8-bit samples, one component or
   three with 1x1 chroma and luma of at most 2x2, at most two DC and two AC tables (more is a partial decode, which
   the SDK leaves to the CPU), and a size */
static u32 parse(const u8* s, u32 n, Jpeg* j) {
    u32 i = 2, dc = 0, ac = 0, frame = 0;
    *j = (Jpeg){0};
    if (n < 4 || s[0] != 0xFF || s[1] != 0xD8) return ST_UNSUPPORTED;
    while (i + 4 <= n) {
        u32 m, len;
        if (s[i] != 0xFF) return ST_UNSUPPORTED;
        while (i < n && s[i] == 0xFF) i++; /* fill bytes */
        if (i >= n) break;
        m = s[i++];
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue; /* no length */
        if (i + 2 > n) break;
        len = snx_ld_be16(s + i);
        if (len < 2 || i + len > n) return ST_UNSUPPORTED;
        const u8* seg = s + i + 2;
        u32 seg_n = len - 2;
        if (m == 0xC0 || m == 0xC1) {
            if (seg_n < 6 || seg[0] != 8) return ST_UNSUPPORTED;
            j->h = snx_ld_be16(seg + 1);
            j->w = snx_ld_be16(seg + 3);
            u32 nc = seg[5];
            if (!j->w || !j->h || seg_n < 6 + 3 * nc) return ST_UNSUPPORTED;
            u32 hy = seg[7] >> 4, vy = seg[7] & 15;
            if (nc == 1) {
                j->chroma = CHROMA_GRAY;
                hy = vy = 1;
            } else if (nc == 3) {
                if (seg[10] != 0x11 || seg[13] != 0x11 || hy < 1 || hy > 2 || vy < 1 || vy > 2) return ST_UNSUPPORTED;
                j->chroma = hy == 2 ? (vy == 2 ? CHROMA_420 : CHROMA_422) : (vy == 2 ? CHROMA_440 : CHROMA_444);
            } else
                return ST_UNSUPPORTED;
            j->mcu_w = (j->w + 8 * hy - 1) / (8 * hy) * 8 * hy;
            j->mcu_h = (j->h + 8 * vy - 1) / (8 * vy) * 8 * vy;
            frame = 1;
        } else if ((m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC))
            return ST_UNSUPPORTED; /* progressive, lossless, hierarchical, arithmetic */
        else if (m == 0xC4) {      /* each table's class and id */
            for (u32 k = 0; k + 17 <= seg_n;) {
                u32 count = 0;
                for (u32 b = 1; b <= 16; b++) count += seg[k + b];
                if (seg[k] >> 4)
                    ac |= 1u << (seg[k] & 15);
                else
                    dc |= 1u << (seg[k] & 15);
                k += 17 + count;
            }
        } else if (m == 0xDA)
            break;
        i += len;
    }
    if (!frame || snx_popcount64(dc) > 2 || snx_popcount64(ac) > 2) return ST_UNSUPPORTED;
    return ST_OK;
}

/* ------------------------------------------------------------ FFmpeg */
/* the picture of the JPEG s (with FFmpeg's padding after its n bytes), its sides shrunk by 1 << lowres; NULL when
   FFmpeg can't decode it or it is no 8-bit YUV or gray */
static AVFrame* decode(const u8* s, u32 n, u32 lowres) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    int r = -1;
    if (ctx && pkt && f) {
        ctx->lowres = (int)lowres;
        pkt->data = (u8*)s;
        pkt->size = (int)n;
        if (avcodec_open2(ctx, codec, NULL) >= 0 && avcodec_send_packet(ctx, pkt) >= 0 &&
            (r = avcodec_receive_frame(ctx, f)) == AVERROR(EAGAIN) && avcodec_send_packet(ctx, NULL) >= 0)
            r = avcodec_receive_frame(ctx, f);
    }
    const AVPixFmtDescriptor* d = r >= 0 ? av_pix_fmt_desc_get(f->format) : NULL;
    if (r >= 0 && (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)) ||
                   !(d->flags & AV_PIX_FMT_FLAG_PLANAR || d->nb_components == 1) || d->comp[0].depth != 8 ||
                   (d->nb_components != 1 && d->nb_components != 3))) {
        WARN_LIMIT(4, "tvmr_jpeg: FFmpeg decoded a JPEG to %s, which isn't 8-bit YUV or gray\n",
                   d ? d->name : "an unknown format");
        r = -1;
    }
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (r < 0) av_frame_free(&f);
    return f;
}

/* the frame's sample of component k (0 Y, 1 Cb, 2 Cr) for luma position (x, y): the nearest chroma one, 128 for the
   chroma of a gray picture */
static u8 sample(const AVFrame* f, const AVPixFmtDescriptor* d, u32 k, u32 x, u32 y) {
    if (!k) return f->data[0][(size_t)y * (size_t)f->linesize[0] + x];
    if (d->nb_components == 1) return 128;
    u32 cx = snx_min_u32(x >> d->log2_chroma_w, (u32)AV_CEIL_RSHIFT(f->width, d->log2_chroma_w) - 1);
    u32 cy = snx_min_u32(y >> d->log2_chroma_h, (u32)AV_CEIL_RSHIFT(f->height, d->log2_chroma_h) - 1);
    return f->data[k][(size_t)cy * (size_t)f->linesize[k] + cx];
}

/* the JPEG of a 4:2:0 picture (Y, then Cb and Cr of half the sides, rounded up, or the two in pairs in cb when cr
   is NULL, NV12's) with the
   quantization tables q (natural order) and the standard Huffman tables: SOI to EOI, without the APPn and COM
   segments FFmpeg writes (NVJPG writes none). 0 when FFmpeg can't */
static u32 encode(const u8 q[2][64], u32 w, u32 h, const Plane* y, const Plane* cb, const Plane* cr, u8** out) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame* f = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    u32 n = 0;
    *out = NULL;
    if (!ctx || !f || !pkt) goto done;
    ctx->width = (int)w;
    ctx->height = (int)h;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base = (AVRational){1, 25};
    /* qscale 8: the encoder quantizes with (matrix * qscale) >> 3, the tables themselves */
    ctx->flags |= AV_CODEC_FLAG_QSCALE | AV_CODEC_FLAG_BITEXACT;
    ctx->global_quality = 8 * FF_QP2LAMBDA;
    ctx->intra_matrix = av_malloc(64 * sizeof *ctx->intra_matrix);
    ctx->chroma_intra_matrix = av_malloc(64 * sizeof *ctx->chroma_intra_matrix);
    if (!ctx->intra_matrix || !ctx->chroma_intra_matrix) goto done;
    for (u32 i = 0; i < 64; i++) {
        ctx->intra_matrix[i] = q[0][i];
        ctx->chroma_intra_matrix[i] = q[1][i];
    }
    if (av_opt_set(ctx->priv_data, "huffman", "default", 0) < 0 || avcodec_open2(ctx, codec, NULL) < 0) goto done;
    f->format = AV_PIX_FMT_YUVJ420P;
    f->width = (int)w;
    f->height = (int)h;
    if (av_frame_get_buffer(f, 0) < 0) goto done;
    f->quality = ctx->global_quality;
    for (u32 r = 0; r < h; r++) memcpy(f->data[0] + (size_t)r * (size_t)f->linesize[0], y->p + (size_t)r * y->pitch, w);
    for (u32 r = 0, cw = (w + 1) / 2; r < (h + 1) / 2; r++) {
        u8* u = f->data[1] + (size_t)r * (size_t)f->linesize[1];
        u8* v = f->data[2] + (size_t)r * (size_t)f->linesize[2];
        const u8* b = cb->p + (size_t)r * cb->pitch;
        if (!cr) /* Cb, Cr pairs */
            for (u32 x = 0; x < cw; x++) {
                u[x] = b[2 * x];
                v[x] = b[2 * x + 1];
            }
        else {
            memcpy(u, b, cw);
            memcpy(v, cr->p + (size_t)r * cr->pitch, cw);
        }
    }
    if (avcodec_send_frame(ctx, f) < 0 || avcodec_send_frame(ctx, NULL) < 0 || avcodec_receive_packet(ctx, pkt) < 0 ||
        pkt->size < 4)
        goto done;
    /* SOI, the segments but APPn and COM, then the scan from its SOS to the end */
    const u8* s = pkt->data;
    u32 len = (u32)pkt->size, i = 2;
    if (!(*out = malloc(len))) goto done;
    memcpy(*out, s, 2);
    n = 2;
    while (i + 4 <= len && s[i] == 0xFF) {
        u32 m = s[i + 1], seg = 2u + snx_ld_be16(s + i + 2);
        if (m == 0xDA || i + seg > len) {
            memcpy(*out + n, s + i, len - i);
            n += len - i;
            break;
        }
        if (!((m >= 0xE0 && m <= 0xEF) || m == 0xFE)) {
            memcpy(*out + n, s + i, seg);
            n += seg;
        }
        i += seg;
    }
    if (n < 4 || (*out)[n - 2] != 0xFF || (*out)[n - 1] != 0xD9) {
        free(*out);
        *out = NULL;
        n = 0;
    }
done:
    if (!n) WARN_LIMIT(4, "tvmr_jpeg: FFmpeg couldn't encode a %lux%lu picture\n", (unsigned long)w, (unsigned long)h);
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&ctx);
    return n;
}

/* ------------------------------------------------------------ the decoder */
static u32 get_info(CPU* c, const Abi* a, u64 info, u32 count, u64 bufs) {
    Jpeg j;
    u32 n;
    if (!writable(info, a->info)) return ST_BAD;
    memset(hook_host(c, info), 0, a->info); /* the SDK's first step: what a failure leaves */
    u8* s = gather(c, a, count, bufs, &n);
    u32 st = s ? parse(s, n, &j) : ST_UNSUPPORTED;
    free(s);
    if (st) return st;
    /* movie 7's marker segments are left out: its caller reads the size alone */
    snx_st_le16(hook_host(c, info), (u16)j.w);
    snx_st_le16(hook_host(c, info + 2), (u16)j.h);
    return ST_OK;
}
u32 tvmrjpeg_get_info64(CPU* c, u64 info, u32 count, u64 bufs) { return get_info(c, &g_abi64, info, count, bufs); }
u32 tvmrjpeg_get_info64_7(CPU* c, u64 info, u32 count, u64 bufs) { return get_info(c, &g_abi64_7, info, count, bufs); }
u32 tvmrjpeg_get_info32(CPU* c, u64 info, u32 count, u64 bufs) { return get_info(c, &g_abi32, info, count, bufs); }

/* the decoder's public fields, as the SDK's */
static void dec_public(CPU* c, const TvmrJpegObj* o) {
    u8* p = hook_host(c, obj_addr(o));
    snx_st_le16(p, o->max_w);
    snx_st_le16(p + 2, o->max_h);
    snx_st_le32(p + 4, o->max_bytes);
}
/* partial: a decoder for the JPEGs NVJPG decodes only in part, which the SDK never makes (and the caller never asks
   for, as GetInfo refuses such JPEGs) */
u64 tvmrjpeg_decoder_create(CPU* c, u16 max_w, u16 max_h, u32 max_bytes, u8 partial) {
    u64 addr = 0;
    if (partial) return 0;
    lock();
    TvmrJpegObj* o = obj_new(c, TVMRJPEG_DECODER);
    if (o) {
        o->max_w = max_w;
        o->max_h = max_h;
        o->max_bytes = max_bytes;
        o->color_standard = 3; /* full range */
        dec_public(c, o);
        addr = obj_addr(o);
    }
    unlock();
    LOG_LIMIT(4, "tvmr_jpeg: decoder %llx for %ux%u, %lu bytes\n", (unsigned long long)addr, (unsigned)max_w,
              (unsigned)max_h, (unsigned long)max_bytes);
    return addr;
}
/* mask 2: the color standard at +0, 1: the alpha at +4 */
void tvmrjpeg_decoder_set_attributes(CPU* c, u64 dec, u32 mask, u64 attr) {
    lock();
    TvmrJpegObj* o = obj_of(dec, TVMRJPEG_DECODER);
    if (o && readable(attr, 8)) {
        if (mask & 2) o->color_standard = snx_ld_le32(hook_host(c, attr));
        if (mask & 1) o->alpha = snx_ld_le32(hook_host(c, attr + 4));
    }
    unlock();
}
u32 tvmrjpeg_decoder_resize(CPU* c, u64 dec, u16 max_w, u16 max_h, u32 max_bytes) {
    lock();
    TvmrJpegObj* o = obj_of(dec, TVMRJPEG_DECODER);
    if (o) {
        o->max_w = max_w;
        o->max_h = max_h;
        o->max_bytes = max_bytes;
        dec_public(c, o);
    }
    unlock();
    return o ? ST_OK : ST_BAD;
}
void tvmrjpeg_decoder_destroy(CPU* c, u64 dec) {
    lock();
    TvmrJpegObj* o = obj_of(dec, TVMRJPEG_DECODER);
    if (o) obj_free(c, o);
    unlock();
}

/* what Render and RenderYUV do first: the decoder's settings (*o), the JPEG parsed and checked against the
   decoder, the fences waited for. *s the bitstream, freed by the caller */
static u32 render_begin(CPU* c, const Abi* a, u64 dec, u32 count, u64 bufs, u64 pre, u32 flags, TvmrJpegObj* o,
                        Jpeg* j, u8** s, u32* n) {
    lock();
    const TvmrJpegObj* d = obj_of(dec, TVMRJPEG_DECODER);
    if (d) *o = *d;
    unlock();
    *s = NULL;
    if (!d) return ST_BAD;
    *s = gather(c, a, count, bufs, n);
    u32 st = *s ? parse(*s, *n, j) : ST_UNSUPPORTED;
    if (st || flags) return ST_UNSUPPORTED;
    wait_fences(c, a, pre);
    return *n > o->max_bytes ? ST_FULL : ST_OK;
}

/* the RGBA of colorStandard: 16.16 luma gain and offset, then Cr to R, Cb and Cr to G, Cb to B */
typedef struct {
    i32 gain, offset, rv, gu, gv, bu;
} Matrix;
static const Matrix g_bt601 = {76284, 16, 104595, -25625, -53281, 132252};
static const Matrix g_bt709 = {76284, 16, 117506, -13959, -34931, 138609};
static const Matrix g_full = {65536, 0, 91881, -22544, -46793, 116130};
static u8 rgb_clamp(i32 v) { return (u8)(v < 0 ? 0 : v >= 255 << 16 ? 255 : (v + 32768) >> 16); }

static u32 render(CPU* c, const Abi* a, u64 dec, u64 dst, u8 ds, u32 count, u64 bufs, u64 pre, u64 post,
                  u32 flags) {
    TvmrJpegObj o;
    Jpeg j;
    u8* s;
    u32 n, st;
    ds = (u8)snx_min_u32(ds, 3);
    if (!readable(dst, (u64)a->out_plane + a->ptr)) return ST_BAD;
    u32 type = snx_ld_le32(hook_host(c, dst));
    u64 rm = plane_surface(c, a, ld_ptr(c, a, dst + a->out_plane));
    if ((st = render_begin(c, a, dec, count, bufs, pre, flags, &o, &j, &s, &n)) != ST_OK) goto done;
    if (type > a->rgba && type <= a->rgba + 3u) {
        WARN_LIMIT(4, "tvmr_jpeg: Render to RGB type %#lx, whose byte order isn't known: refused\n",
                   (unsigned long)type);
        st = ST_UNSUPPORTED;
        goto done;
    }
    Plane pl = plane_at(c, rm, 0, 0); /* its sides */
    if (pl.layout != RM_PITCH_LINEAR) {
        WARN_LIMIT(4, "tvmr_jpeg: Render to a surface of layout %lu, not pitch linear: refused\n",
                   (unsigned long)pl.layout);
        st = ST_UNSUPPORTED;
        goto done;
    }
    if ((j.mcu_w >> ds) > pl.width || (j.mcu_h >> ds) > pl.height) {
        st = ST_BAD;
        goto done;
    }
    AVFrame* f = decode(s, n, ds);
    if (!f) {
        st = ST_UNSUPPORTED;
        goto done;
    }
    u32 w = snx_min_u32((u32)f->width, pl.width), h = snx_min_u32((u32)f->height, pl.height);
    pl = plane_at(c, rm, h, w * 4);
    if (pl.p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f->format);
        const Matrix* m = o.color_standard == 0 ? &g_bt601 : o.color_standard == 1 ? &g_bt709 : &g_full;
        for (u32 y = 0; y < h; y++) {
            u8* row = pl.p + (size_t)y * pl.pitch;
            for (u32 x = 0; x < w; x++) {
                i32 l = m->gain * ((i32)sample(f, d, 0, x, y) - m->offset);
                i32 u = (i32)sample(f, d, 1, x, y) - 128, v = (i32)sample(f, d, 2, x, y) - 128;
                row[4 * x] = rgb_clamp(l + m->rv * v);
                row[4 * x + 1] = rgb_clamp(l + m->gu * u + m->gv * v);
                row[4 * x + 2] = rgb_clamp(l + m->bu * u);
                row[4 * x + 3] = (u8)o.alpha;
            }
        }
        LOG_LIMIT(1, "tvmr_jpeg: the first picture decoded to RGBA, %lux%lu\n", (unsigned long)w, (unsigned long)h);
    } else
        st = ST_BAD;
    av_frame_free(&f);
done:
    free(s);
    if (!st) done_fence(c, post);
    return st;
}
/* the 64-bit ABI's last two arguments, the post fence and the flags, are on the stack */
u32 tvmrjpeg_decoder_render64(CPU* c, u64 dec, u64 dst, u64 src_rect, u64 dst_rect, u8 ds, u32 count, u64 bufs,
                              u64 pre) {
    u64 sp = c->x[31];
    (void)src_rect, (void)dst_rect; /* NVJPG takes neither */
    if (!readable(sp, 12)) return ST_BAD;
    return render(c, &g_abi64, dec, dst, ds, count, bufs, pre, snx_ld_le64(hook_host(c, sp)),
                  snx_ld_le32(hook_host(c, sp + 8)));
}
u32 tvmrjpeg_decoder_render64_7(CPU* c, u64 dec, u64 dst, u64 src_rect, u64 dst_rect, u8 ds, u32 count, u64 bufs,
                                u64 pre) {
    u64 sp = c->x[31];
    (void)src_rect, (void)dst_rect;
    if (!readable(sp, 12)) return ST_BAD;
    return render(c, &g_abi64_7, dec, dst, ds, count, bufs, pre, snx_ld_le64(hook_host(c, sp)),
                  snx_ld_le32(hook_host(c, sp + 8)));
}
u32 tvmrjpeg_decoder_render32(CPU* c, u64 dec, u64 dst, u64 src_rect, u64 dst_rect, u8 ds, u32 count, u64 bufs,
                              u64 pre, u64 post, u32 flags) {
    (void)src_rect, (void)dst_rect;
    return render(c, &g_abi32, dec, dst, ds, count, bufs, pre, post, flags);
}

/* a TVMRVideoSurface type RenderYUV writes: its chroma's subsampling (log2, across and down) and how it lays the
   planes out */
enum { LAY_PLANAR, LAY_NV12, LAY_YUYV };
typedef struct {
    u8 sub_w, sub_h, lay, chroma; /* chroma: CHROMA_* */
} YuvType;
/* types 0 to 12: 0 to 3 planar 4:2:0, 4:2:2, 4:4:0 and 4:4:4, 6 NV12, 12 YUYV. The rest NVJPG doesn't write */
static int yuv_type(u32 type, YuvType* t) {
    static const YuvType types[] = {{1, 1, LAY_PLANAR, CHROMA_420}, {1, 0, LAY_PLANAR, CHROMA_422},
                                    {0, 1, LAY_PLANAR, CHROMA_440}, {0, 0, LAY_PLANAR, CHROMA_444}};
    if (type <= 3)
        *t = types[type];
    else if (type == 6)
        *t = (YuvType){1, 1, LAY_NV12, CHROMA_420};
    else if (type == 12)
        *t = (YuvType){1, 0, LAY_YUYV, CHROMA_422};
    else
        return 0;
    return 1;
}

static u32 render_yuv(CPU* c, const Abi* a, u64 dec, u64 dst, u8 ds, u32 count, u64 bufs, u64 pre, u64 post,
                      u32 flags) {
    TvmrJpegObj o;
    YuvType t;
    Jpeg j;
    u8* s = NULL;
    u32 n, st;
    if (!readable(dst, (u64)a->planes + 3u * a->ptr)) return ST_BAD;
    u32 type = snx_ld_le32(hook_host(c, dst));
    if (type > 12) return ST_BAD;
    if (!yuv_type(type, &t)) return ST_UNSUPPORTED;
    u64 rm[3];
    for (u32 i = 0; i < 3; i++) rm[i] = plane_surface(c, a, ld_ptr(c, a, dst + a->planes + (u64)i * a->ptr));
    Plane py = plane_at(c, rm[0], 0, 0);
    if (py.layout != RM_PITCH_LINEAR) return ST_UNSUPPORTED;
    ds = (u8)snx_min_u32(ds, 3);
    if ((st = render_begin(c, a, dec, count, bufs, pre, flags, &o, &j, &s, &n)) != ST_OK) goto done;
    /* NVJPG converts the chroma only to 4:2:0 and 4:2:2, at full size */
    if (j.chroma != CHROMA_GRAY && j.chroma != t.chroma &&
        ((t.chroma != CHROMA_420 && t.chroma != CHROMA_422) || ds)) {
        st = ST_UNSUPPORTED;
        goto done;
    }
    if ((j.mcu_w >> ds) > py.width || (j.mcu_h >> ds) > py.height) {
        st = ST_BAD;
        goto done;
    }
    lock();
    TvmrJpegObj* live = obj_of(dec, TVMRJPEG_DECODER);
    if (live) live->alpha = 0; /* as the SDK's: a later Render writes alpha 0 */
    unlock();
    AVFrame* f = decode(s, n, ds);
    if (!f) {
        st = ST_UNSUPPORTED;
        goto done;
    }
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f->format);
    u32 w = snx_min_u32((u32)f->width, py.width), h = snx_min_u32((u32)f->height, py.height);
    u32 cw = (w + (1u << t.sub_w) - 1) >> t.sub_w, ch = (h + (1u << t.sub_h) - 1) >> t.sub_h;
    int gray = j.chroma == CHROMA_GRAY; /* luma alone, as NVJPG writes it */
    if (t.lay == LAY_YUYV) {
        Plane p = plane_at(c, rm[0], h, 2 * cw * 2);
        if (!p.p) st = ST_BAD;
        for (u32 y = 0; p.p && y < h; y++)
            for (u32 x = 0; x < w; x++) {
                u8* q = p.p + (size_t)y * p.pitch + 2 * x;
                q[0] = sample(f, d, 0, x, y);
                if (!gray) q[1] = sample(f, d, x & 1 ? 2 : 1, x & ~1u, y);
            }
    } else {
        py = plane_at(c, rm[0], h, w);
        if (!py.p) st = ST_BAD;
        for (u32 y = 0; py.p && y < h; y++)
            for (u32 x = 0; x < w; x++) py.p[(size_t)y * py.pitch + x] = sample(f, d, 0, x, y);
        if (!gray && t.lay == LAY_NV12) {
            Plane p = plane_at(c, rm[1], ch, 2 * cw);
            if (!p.p) st = ST_BAD;
            for (u32 y = 0; p.p && y < ch; y++)
                for (u32 x = 0; x < cw; x++) {
                    u8* q = p.p + (size_t)y * p.pitch + 2 * x;
                    q[0] = sample(f, d, 1, x << t.sub_w, y << t.sub_h);
                    q[1] = sample(f, d, 2, x << t.sub_w, y << t.sub_h);
                }
        } else if (!gray) {
            /* NVJPG's second plane (Cb) is the surface's third, its third (Cr) the surface's second */
            Plane pb = plane_at(c, rm[2], ch, cw), pr = plane_at(c, rm[1], ch, cw);
            if (!pb.p || !pr.p) st = ST_BAD;
            for (u32 y = 0; pb.p && pr.p && y < ch; y++)
                for (u32 x = 0; x < cw; x++) {
                    pb.p[(size_t)y * pb.pitch + x] = sample(f, d, 1, x << t.sub_w, y << t.sub_h);
                    pr.p[(size_t)y * pr.pitch + x] = sample(f, d, 2, x << t.sub_w, y << t.sub_h);
                }
        }
    }
    if (!st)
        LOG_LIMIT(1, "tvmr_jpeg: the first picture decoded to YUV type %lu, %lux%lu\n", (unsigned long)type,
                  (unsigned long)w, (unsigned long)h);
    av_frame_free(&f);
done:
    free(s);
    if (!st) done_fence(c, post);
    return st;
}
u32 tvmrjpeg_decoder_render_yuv64(CPU* c, u64 dec, u64 dst, u8 ds, u32 count, u64 bufs, u64 pre, u64 post,
                                  u32 flags) {
    return render_yuv(c, &g_abi64, dec, dst, ds, count, bufs, pre, post, flags);
}
u32 tvmrjpeg_decoder_render_yuv64_7(CPU* c, u64 dec, u64 dst, u8 ds, u32 count, u64 bufs, u64 pre, u64 post,
                                    u32 flags) {
    return render_yuv(c, &g_abi64_7, dec, dst, ds, count, bufs, pre, post, flags);
}
u32 tvmrjpeg_decoder_render_yuv32(CPU* c, u64 dec, u64 dst, u8 ds, u32 count, u64 bufs, u64 pre, u64 post,
                                  u32 flags) {
    return render_yuv(c, &g_abi32, dec, dst, ds, count, bufs, pre, post, flags);
}

/* ------------------------------------------------------------ the encoder */
/* JPEG's example tables (ITU T.81 Annex K), natural order */
static const u8 g_base[2][64] = {
    {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24, 40,  57,
     69, 56, 14, 17, 22,  29,  51,  87,  80, 62, 18, 22, 37,  56,  68,  109, 103, 77, 24, 35, 55, 64,
     81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99},
    {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
     99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};
/* the tables of quality q (1 to 100), as the SDK scales them */
static void quality_tables(u8 q[2][64], u32 quality) {
    u32 qq = quality ? snx_min_u32(quality, 100) : 1, scale = qq < 50 ? 6400 / qq : (100 - qq) * 256 / 100;
    for (u32 t = 0; t < 2; t++)
        for (u32 i = 0; i < 64; i++) {
            u32 v = (g_base[t][i] * scale + 64) >> 7;
            q[t][i] = (u8)(v < 1 ? 1 : v > 255 ? 255 : v);
        }
}
/* the encoder's first frame not read yet: its bytes, NULL when they were lost (fed before a save state's load) */
static const u8* frame_bytes(const TvmrJpegObj* o) {
    u32 i = obj_index(o);
    return g_host.tvmr_jpeg.serial[i][o->rd] == o->serial[o->rd] ? g_host.tvmr_jpeg.jpeg[i][o->rd] : NULL;
}
static void frame_pop(TvmrJpegObj* o) {
    u32 i = obj_index(o);
    free(g_host.tvmr_jpeg.jpeg[i][o->rd]);
    g_host.tvmr_jpeg.jpeg[i][o->rd] = NULL;
    o->rd = (u8)((o->rd + 1u) % o->nbufs);
    o->inflight--;
}

u64 tvmrjpeg_encoder_create(CPU* c, u8 nbufs, u32 max_bytes) {
    u64 addr = 0;
    if (max_bytes < 0x250) return 0; /* the SDK's least: room for its default header */
    lock();
    TvmrJpegObj* o = obj_new(c, TVMRJPEG_ENCODER);
    if (o) {
        o->nbufs = (u8)(nbufs ? snx_min_u32(nbufs, TVMRJPEG_BUFS) : 1);
        o->cap = max_bytes - 0x24f;
        o->target = 0xFFFFFFFFu;
        quality_tables(o->quant, 50);
        addr = obj_addr(o);
    }
    unlock();
    LOG_LIMIT(4, "tvmr_jpeg: encoder %llx with %u buffers of %lu bytes\n", (unsigned long long)addr, (unsigned)nbufs,
              (unsigned long)max_bytes);
    return addr;
}
void tvmrjpeg_encoder_destroy(CPU* c, u64 enc) {
    lock();
    TvmrJpegObj* o = obj_of(enc, TVMRJPEG_ENCODER);
    if (o) obj_free(c, o);
    unlock();
}

/* the attributes at attr by mask: 1 the quality (u8 at +0), 2 the restart interval (u16 at +2), 4 the target size
   (u32 at +4, kept when 0), 8 the luma and chroma tables (64 bytes each at +8 and +0x48, natural order), 16 the
   Huffman tables (four pointers at +0x88, NULL for the standard one). Under the lock */
static void set_attributes(CPU* c, const Abi* a, TvmrJpegObj* o, u32 mask, u64 attr) {
    if (!readable(attr, 0x88u + 4u * a->ptr)) return;
    const u8* p = hook_host(c, attr);
    if (mask & 1) quality_tables(o->quant, p[0]);
    if (mask & 2) o->restart = snx_ld_le16(p + 2);
    if (mask & 4 && snx_ld_le32(p + 4)) o->target = snx_ld_le32(p + 4);
    if (mask & 8) {
        memcpy(o->quant[0], p + 8, 64);
        memcpy(o->quant[1], p + 0x48, 64);
    }
    for (u32 i = 0; mask & 16 && i < 4; i++)
        if (ld_ptr(c, a, attr + 0x88 + (u64)i * a->ptr))
            LOG_LIMIT(1, "tvmr_jpeg: an encoder's own Huffman tables aren't used: its JPEGs take the standard ones\n");
}
static void encoder_set_attributes(CPU* c, const Abi* a, u64 enc, u32 mask, u64 attr) {
    lock();
    TvmrJpegObj* o = obj_of(enc, TVMRJPEG_ENCODER);
    if (o && attr) set_attributes(c, a, o, mask, attr);
    unlock();
}
void tvmrjpeg_encoder_set_attributes64(CPU* c, u64 enc, u32 mask, u64 attr) {
    encoder_set_attributes(c, &g_abi64, enc, mask, attr);
}
void tvmrjpeg_encoder_set_attributes32(CPU* c, u64 enc, u32 mask, u64 attr) {
    encoder_set_attributes(c, &g_abi32, enc, mask, attr);
}

/* EncFeedFrame: the surface's picture (planar 4:2:0, type 0, or NV12, type 6) encoded into the encoder's next
   buffer. The settings are the object's, as the caller left them, or *with when it isn't NULL (the FeedFrame
   functions', which the SDK stores in the object first) */
static u32 feed(CPU* c, const Abi* a, u64 enc, u64 surf, u64 pre, u64 out, const TvmrJpegObj* with) {
    TvmrJpegObj o;
    if (!enc) return ST_FULL;
    if (!readable(surf, (u64)a->planes + 3u * a->ptr)) return ST_BAD;
    u32 type = snx_ld_le32(hook_host(c, surf)), w = snx_ld_le32(hook_host(c, surf + 4)),
        h = snx_ld_le32(hook_host(c, surf + 8));
    lock();
    TvmrJpegObj* live = obj_of(enc, TVMRJPEG_ENCODER);
    if (live && with) {
        memcpy(live->quant, with->quant, sizeof live->quant);
        live->target = with->target;
    }
    if (live) o = *live;
    unlock();
    if (!live || (type != 0 && type != 6)) return live ? ST_BAD : ST_FULL;
    if (o.inflight == o.nbufs) return ST_FULL;
    if (!w || !h || w > MAX_DIM || h > MAX_DIM) return ST_BAD;
    if (o.restart)
        LOG_LIMIT(1, "tvmr_jpeg: an encoder's restart interval %lu isn't used: its JPEGs have none\n",
                  (unsigned long)o.restart);
    if (o.target != 0xFFFFFFFFu)
        LOG_LIMIT(1, "tvmr_jpeg: an encoder's target of %lu bytes isn't used: its tables set the size\n",
                  (unsigned long)o.target);
    wait_fences(c, a, pre);
    u32 cw = (w + 1) / 2, ch = (h + 1) / 2;
    u64 rm[3];
    for (u32 i = 0; i < 3; i++) rm[i] = plane_surface(c, a, ld_ptr(c, a, surf + a->planes + (u64)i * a->ptr));
    Plane py = plane_at(c, rm[0], h, w), pb = plane_at(c, rm[1], ch, type ? 2 * cw : cw), pr = {0};
    if (!type) pr = plane_at(c, rm[2], ch, cw);
    if (!py.p || !pb.p || (!type && !pr.p)) return ST_BAD;
    u8* jpeg;
    u32 n = encode((const u8(*)[64])o.quant, w, h, &py, &pb, type ? NULL : &pr, &jpeg);
    if (!n) return ST_FULL;
    if (n - 4 > o.cap) /* NVJPG cuts what overflows its buffer: a larger one would have to be asked for */
        WARN_LIMIT(4, "tvmr_jpeg: a JPEG of %lu bytes for an encoder whose buffer holds %lu: kept whole\n",
                   (unsigned long)n, (unsigned long)o.cap + 4);
    lock();
    live = obj_of(enc, TVMRJPEG_ENCODER);
    if (!live || live->inflight == live->nbufs) {
        unlock();
        free(jpeg);
        return ST_FULL;
    }
    u32 i = obj_index(live), b = live->wr;
    free(g_host.tvmr_jpeg.jpeg[i][b]);
    g_host.tvmr_jpeg.jpeg[i][b] = jpeg;
    g_host.tvmr_jpeg.serial[i][b] = live->serial[b] = ++g_hle.tvmr_jpeg.frames;
    live->size[b] = n;
    live->wr = (u8)((b + 1u) % live->nbufs);
    live->inflight++;
    unlock();
    LOG_LIMIT(1, "tvmr_jpeg: the first picture encoded, %lux%lu into %lu bytes\n", (unsigned long)w,
              (unsigned long)h, (unsigned long)n);
    done_fence(c, out);
    return ST_OK;
}
static u32 enc_feed(CPU* c, const Abi* a, u64 enc, u64 surf, u64 pre, u64 out) {
    return feed(c, a, enc, surf, pre, out, NULL);
}
u32 tvmrjpeg_enc_feed_frame64(CPU* c, u64 enc, u64 surf, u64 pre, u64 out) {
    return enc_feed(c, &g_abi64, enc, surf, pre, out);
}
u32 tvmrjpeg_enc_feed_frame32(CPU* c, u64 enc, u64 surf, u64 pre, u64 out) {
    return enc_feed(c, &g_abi32, enc, surf, pre, out);
}
/* the tables of quality, and no target */
static u32 feed_quality(CPU* c, const Abi* a, u64 enc, u64 surf, u64 pre, u64 out, u8 quality) {
    TvmrJpegObj with = {0};
    quality_tables(with.quant, quality);
    with.target = 0xFFFFFFFFu;
    return feed(c, a, enc, surf, pre, out, &with);
}
u32 tvmrjpeg_encoder_feed_frame64(CPU* c, u64 enc, u64 surf, u64 pre, u64 out, u8 quality) {
    return feed_quality(c, &g_abi64, enc, surf, pre, out, quality);
}
u32 tvmrjpeg_encoder_feed_frame32(CPU* c, u64 enc, u64 surf, u64 pre, u64 out, u8 quality) {
    return feed_quality(c, &g_abi32, enc, surf, pre, out, quality);
}
/* the caller's tables (natural order), and target (none for ~0, the old one for 0) */
static u32 feed_tables(CPU* c, const Abi* a, u64 enc, u64 surf, u64 pre, u64 out, u64 luma, u64 chroma, u32 target) {
    TvmrJpegObj with = {0};
    if (!enc) return ST_FULL;
    if (!readable(luma, 64) || !readable(chroma, 64)) return ST_BAD;
    memcpy(with.quant[0], hook_host(c, luma), 64);
    memcpy(with.quant[1], hook_host(c, chroma), 64);
    lock();
    const TvmrJpegObj* o = obj_of(enc, TVMRJPEG_ENCODER);
    with.target = target ? target : o ? o->target : 0xFFFFFFFFu;
    unlock();
    return feed(c, a, enc, surf, pre, out, &with);
}
u32 tvmrjpeg_encoder_feed_frame_quant64(CPU* c, u64 enc, u64 surf, u64 pre, u64 out, u64 luma, u64 chroma) {
    return feed_tables(c, &g_abi64, enc, surf, pre, out, luma, chroma, 0xFFFFFFFFu);
}
u32 tvmrjpeg_encoder_feed_frame_quant32(CPU* c, u64 enc, u64 surf, u64 pre, u64 out, u64 luma, u64 chroma) {
    return feed_tables(c, &g_abi32, enc, surf, pre, out, luma, chroma, 0xFFFFFFFFu);
}
u32 tvmrjpeg_encoder_feed_frame_rate_control64(CPU* c, u64 enc, u64 surf, u64 pre, u64 out, u64 luma, u64 chroma,
                                               u32 target) {
    return feed_tables(c, &g_abi64, enc, surf, pre, out, luma, chroma, target);
}
u32 tvmrjpeg_encoder_feed_frame_rate_control32(CPU* c, u64 enc, u64 surf, u64 pre, u64 out, u64 luma, u64 chroma,
                                               u32 target) {
    return feed_tables(c, &g_abi32, enc, surf, pre, out, luma, chroma, target);
}

/* the size of the first frame not read, SOI to EOI, at *bytes: every frame fed is done. mode 2 waits for a frame
   to be fed when none is: here, as no other thread feeds one, none is the answer */
u32 tvmrjpeg_encoder_bits_available(CPU* c, u64 enc, u64 bytes, u32 mode, u32 timeout_ms) {
    (void)mode, (void)timeout_ms;
    if (!enc || !writable(bytes, 4)) return ST_FULL;
    lock();
    const TvmrJpegObj* o = obj_of(enc, TVMRJPEG_ENCODER);
    u32 st = !o || !o->inflight ? ST_NONE : ST_OK;
    if (!st) snx_st_le32(hook_host(c, bytes), o->size[o->rd]);
    unlock();
    return st;
}

/* the first frame not read, into the nbufs buffers {pointer, u32 room} at bufs one after another (dst alone, with
   room for it all, when bufs is 0), without its SOI when flags has bit 0; its size at *bytes. A frame the buffers
   can't hold stays. Movie 7's drop the frame when there is no destination */
static u32 get_bits(CPU* c, const Abi* a, u64 enc, u64 bytes, u32 nbufs, u64 bufs, u64 dst, u32 flags) {
    u32 st = ST_OK;
    lock();
    TvmrJpegObj* o = obj_of(enc, TVMRJPEG_ENCODER);
    if (!o || !o->inflight) {
        unlock();
        return ST_NONE;
    }
    u32 skip = flags & 1 ? 2 : 0, n = o->size[o->rd] - skip, left = n;
    const u8* s = frame_bytes(o);
    snx_st_le32(hook_host(c, bytes), n);
    if (!s) {
        WARN_LIMIT(4, "tvmr_jpeg: an encoder's frame fed before the save state's load is lost\n");
        frame_pop(o);
        unlock();
        return ST_FULL;
    }
    s += skip;
    if (!bufs && !dst) { /* movie 7's drop */
        frame_pop(o);
        unlock();
        return ST_OK;
    }
    if (!bufs) {
        if (!writable(dst, n)) st = ST_FULL;
        else
            memcpy(hook_host(c, dst), s, n), left = 0;
    } else {
        u64 stride = 2u * a->ptr;
        for (u32 i = 0; left && i < nbufs && readable(bufs + i * stride, stride); i++) {
            u64 p = ld_ptr(c, a, bufs + i * stride);
            u32 room = snx_min_u32(snx_ld_le32(hook_host(c, bufs + i * stride + a->ptr)), left);
            if (!room) continue;
            if (!writable(p, room)) break;
            memcpy(hook_host(c, p), s + (n - left), room);
            left -= room;
        }
        if (left) st = ST_FULL;
    }
    if (!st) frame_pop(o);
    unlock();
    return st;
}
static u32 getbits(CPU* c, const Abi* a, u64 enc, u64 bytes, u64 dst, u32 flags) {
    if (!enc || !bytes || (!dst && !a->null_dst_ok)) return ST_FULL;
    if (!writable(bytes, 4)) return ST_BAD;
    return get_bits(c, a, enc, bytes, 0, 0, dst, flags);
}
u32 tvmrjpeg_encoder_get_bits64(CPU* c, u64 enc, u64 bytes, u64 dst, u32 flags) {
    return getbits(c, &g_abi64, enc, bytes, dst, flags);
}
u32 tvmrjpeg_encoder_get_bits64_7(CPU* c, u64 enc, u64 bytes, u64 dst, u32 flags) {
    return getbits(c, &g_abi64_7, enc, bytes, dst, flags);
}
u32 tvmrjpeg_encoder_get_bits32(CPU* c, u64 enc, u64 bytes, u64 dst, u32 flags) {
    return getbits(c, &g_abi32, enc, bytes, dst, flags);
}
static u32 getbits_ex(CPU* c, const Abi* a, u64 enc, u64 bytes, u32 nbufs, u64 bufs, u32 flags) {
    if (!enc || !bytes || (!a->null_dst_ok && (!nbufs || !bufs))) return ST_FULL;
    if (!writable(bytes, 4)) return ST_BAD;
    if (!bufs) return get_bits(c, a, enc, bytes, 0, 0, 0, flags);
    return get_bits(c, a, enc, bytes, nbufs, bufs, 0, flags);
}
u32 tvmrjpeg_encoder_get_bits_ex64(CPU* c, u64 enc, u64 bytes, u32 nbufs, u64 bufs, u32 flags) {
    return getbits_ex(c, &g_abi64, enc, bytes, nbufs, bufs, flags);
}
u32 tvmrjpeg_encoder_get_bits_ex64_7(CPU* c, u64 enc, u64 bytes, u32 nbufs, u64 bufs, u32 flags) {
    return getbits_ex(c, &g_abi64_7, enc, bytes, nbufs, bufs, flags);
}
u32 tvmrjpeg_encoder_get_bits_ex32(CPU* c, u64 enc, u64 bytes, u32 nbufs, u64 bufs, u32 flags) {
    return getbits_ex(c, &g_abi32, enc, bytes, nbufs, bufs, flags);
}
