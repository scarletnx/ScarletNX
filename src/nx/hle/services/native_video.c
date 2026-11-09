/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Ounce's native video service (native_video.h): the decoder contexts the graphics supervisor opens, their codec
   sessions, and the video image pools. FFmpeg decodes on the CPU, and the GPU writes a picture into the image the
   client acquires next, in the order of the fences RegisterImage and ReleaseImage gave. Then the codec's
   syncpoint reaches the image's fence, and the game reads the image (ReadDecoderOutputImageNv12, or its GPU mapping).
   Every command takes g_host.native_video.lock. The structure and the command set follow closed PR #1009, which
   implemented the same protocol in another layout of the runtime, checked against the game's SDK */
#include "nx/hle/hle.h"
#include "nx/hle/services/native_video.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/video/vic_regs.h"
#include <libavcodec/avcodec.h>
#include <limits.h>
#include <math.h>

static int nvideo_context_dispatch(SvcObj* self, IpcCtx* c);
static int nvideo_codec_dispatch(SvcObj* self, IpcCtx* c);
static void nvideo_codec_destroy(SvcObj* self);
#include "hle/services/native_video.swipc.inc"

/* the supervisor's partition of the GPU address space (gsv.c's GetGsvPartitionGpuAddress), which the game's own
   mappings never use: 426 maps the images there */
#define NVIDEO_VA_FIRST 0xc000000000ull
#define NVIDEO_VA_END   0x10000000000ull

#define NV (&g_hle.native_video)

int native_video_service(int encoder, int enable) {
    pm_lock(&g_host.native_video.lock);
    if (encoder)
        NV->encoder_ready = enable != 0;
    else
        NV->decoder_ready = enable != 0;
    pm_unlock(&g_host.native_video.lock);
    LOG("gsv: video %s %s\n", encoder ? "encoder" : "decoder", enable ? "initialized" : "finalized");
    R_SUCCEED();
}
int native_video_requirements(const u32 pref[16], u32 count, u64* size, u64* alignment) {
    NativeVideoLayout l;
    R_TRY(nvideo_layout(pref, count, &l, size));
    *alignment = 4096;
    LOG("gsv: video images %ux%u kind %u, %u of them: %#llx bytes\n", (pref[2] & 0xffff) + 1, (pref[2] >> 16) + 1,
        pref[1], count, (unsigned long long)*size);
    R_SUCCEED();
}
int native_video_pool_create(const u32 pref[16], u32 count, u32 memory, u32* image) {
    NativeVideoPool p = {.count = count, .memory = memory};
    u64 size = 0, mem_size = 0;
    R_TRY(nvideo_layout(pref, count, &p.layout, &size));
    R_UNLESS(gsv_native_memory_cpu(memory, &p.cpu, &mem_size), NVIDEO_NOT_FOUND);
    R_UNLESS(mem_size >= size, NVIDEO_BAD_ARGUMENT);
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* slot = NULL;
    for (u32 i = 0; i < NVIDEO_POOLS && !slot; i++)
        if (!NV->pools[i].first) slot = &NV->pools[i];
    p.first = slot ? gsv_native_image_reserve(count) : 0;
    if (p.first) *slot = p;
    pm_unlock(&g_host.native_video.lock);
    R_UNLESS(p.first, NVIDEO_NO_ROOM);
    *image = p.first;
    LOG("gsv: video pool %u..%u in memory %u at %#llx, %u bytes an image\n", p.first, p.first + count - 1, memory,
        (unsigned long long)p.cpu, p.layout.image_bytes);
    R_SUCCEED();
}
u32 native_video_memory_pitch(u32 memory) {
    u32 pitch = 0;
    pm_lock(&g_host.native_video.lock);
    for (u32 i = 0; i < NVIDEO_POOLS && !pitch; i++)
        if (NV->pools[i].first && NV->pools[i].memory == memory && !nvideo_kind_nv12(NV->pools[i].layout.kind))
            pitch = NV->pools[i].layout.pitch;
    pm_unlock(&g_host.native_video.lock);
    return pitch;
}
/* The SDK destroys the native memory right after, with the images maybe still mapped for the GPU: the mappings stay
   until 427, and a codec skips an image whose pool went */
int native_video_pool_destroy(u32 image) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = nvideo_pool_find(NV->pools, NVIDEO_POOLS, image);
    int rc = p && p->first == image ? 0 : NVIDEO_NOT_FOUND;
    if (!rc) *p = (NativeVideoPool){0};
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_query(u32 image, u64 d[16]) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = nvideo_pool_find(NV->pools, NVIDEO_POOLS, image);
    if (p) nvideo_cpu_descriptor(p, image, d);
    pm_unlock(&g_host.native_video.lock);
    return p ? 0 : NVIDEO_NOT_FOUND;
}
int native_video_gpu_query(u32 image, u64 d[16]) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = nvideo_pool_find(NV->pools, NVIDEO_POOLS, image);
    if (p) nvideo_gpu_descriptor(p, image, d);
    pm_unlock(&g_host.native_video.lock);
    return p ? 0 : NVIDEO_NOT_FOUND;
}
int native_video_gpu_map(u32 image, u32 address_space, u64* first, u64* last, u64 d[16]) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = nvideo_pool_find(NV->pools, NVIDEO_POOLS, image);
    NativeVideoMap *m = NULL, *free_slot = NULL;
    int rc = p ? 0 : NVIDEO_NOT_FOUND;
    for (u32 i = 0; i < NVIDEO_MAPS && !rc; i++) {
        NativeVideoMap* q = &NV->maps[i];
        if (q->image == image && q->address_space == address_space) m = q;
        if (!q->image && !free_slot) free_slot = q;
    }
    if (!rc && m)
        m->refs++;
    else if (!rc) {
        u64 bytes = p->layout.image_bytes, cpu = p->cpu + (u64)(image - p->first) * bytes;
        if (!NV->va_next) NV->va_next = NVIDEO_VA_FIRST;
        if (!free_slot || NV->va_next + bytes > NVIDEO_VA_END)
            rc = NVIDEO_NO_ROOM;
        else {
            m = free_slot;
            *m = (NativeVideoMap){image, address_space, 1, NV->va_next, bytes};
            NV->va_next += (bytes + 0xffff) & ~0xffffull;
            gmmu_space_map(m->address, cpu, bytes);
        }
    }
    if (!rc) {
        *first = m->address;
        *last = m->address + m->size - 1;
        nvideo_gpu_descriptor(p, image, d);
        d[5] &= 0xffffffff00000000ull; /* the mapping starts at the image */
    }
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_gpu_unmap(u32 image, u32 address_space, u64 first, u64 last) {
    pm_lock(&g_host.native_video.lock);
    int rc = NVIDEO_NOT_FOUND;
    for (u32 i = 0; i < NVIDEO_MAPS && rc; i++) {
        NativeVideoMap* m = &NV->maps[i];
        if (m->image != image || m->address_space != address_space || m->address != first ||
            last != m->address + m->size - 1)
            continue;
        rc = 0;
        if (!--m->refs) {
            gmmu_space_unmap(m->address, m->size);
            *m = (NativeVideoMap){0};
        }
    }
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_busy(void) {
    for (u32 i = 0; i < NVIDEO_CODECS; i++)
        if (NV->codecs[i].obj && NV->codecs[i].state != NVIDEO_NEW) return 1;
    return 0;
}

/* ============================================================ the codec */
static u32 codec_index(const NativeVideoCodec* s) { return (u32)(s - NV->codecs); }
#define HOST(s) (&g_host.native_video.codecs[codec_index(s)])

static void host_free(NativeVideoCodec* s) {
    avcodec_free_context(&HOST(s)->decoder);
    av_frame_free(&HOST(s)->frame);
    for (u32 i = 0; i < NVIDEO_INPUTS; i++) av_packet_free(&HOST(s)->packets[i]);
    av_frame_free(&HOST(s)->black);
    HOST(s)->nblack = 0;
}
/* back to a codec that was just made: its decoder, syncpoint, memory and event let go */
static void codec_reset(NativeVideoCodec* s) {
    host_free(s);
    if (s->syncpoint) syncpt_free(s->syncpoint);
    if (s->tmem) kobj_unref(kobj_at(s->tmem));
    if (s->event) kobj_unref(kobj_at(s->event));
    *s = (NativeVideoCodec){.obj = s->obj, .encoder = s->encoder};
}
/* signaled while the input ring has room */
static void submission_event(NativeVideoCodec* s) {
    if (!s->event) return;
    if (s->input_count < NVIDEO_INPUTS)
        ev_signal(s->event);
    else
        ev_clear(s->event);
}
/* VIC's colour matrix from a picture's YUV to RGB, by the colour space and range FFmpeg read: each channel is
   ((Y*c0 + U*c1 + V*c2) >> shift + c3) >> 8, of samples of 10 bits (gpu/video/vic.c) */
static void rgb_matrix(VicSlot* s, const AVFrame* f) {
    double kr = 0.2126, kb = 0.0722; /* BT.709, also when the stream doesn't say */
    if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M)
        kr = 0.299, kb = 0.114;
    else if (f->colorspace == AVCOL_SPC_BT2020_NCL)
        kr = 0.2627, kb = 0.0593;
    int full = f->color_range == AVCOL_RANGE_JPEG;
    double kg = 1 - kr - kb, ys = full ? 1 : 255.0 / 219, cs = full ? 1 : 255.0 / 224, y0 = full ? 0 : 64;
    const double m[3][3] = {{ys, 0, cs * 2 * (1 - kr)},
                            {ys, -cs * 2 * kb * (1 - kb) / kg, -cs * 2 * kr * (1 - kr) / kg},
                            {ys, cs * 2 * (1 - kb), 0}};
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) s->mat[i][k] = (int)lround(m[i][k] * 256);
        s->mat[i][3] = (int)lround(-(ys * y0 + (m[i][1] + m[i][2]) * 512) * 256);
    }
    s->mat_shift = 0;
    s->mat_enable = 1;
    s->alpha = 1023;
    s->clamp_lo = 0;
    s->clamp_hi = 1023;
}
/* the picture written into its image on the GPU, as NVDEC's pictures are (the renderer's nvdec_write, pitch linear):
   FFmpeg's planar rows are staged, the GPU interleaves the chroma, and the image is published over guest memory, which
   the game's CPU reads (ReadDecoderOutputImageNv12) land. 1 when written */
static int image_write(const NativeVideoPool* p, u32 id, const AVFrame* f) {
    const NativeVideoLayout* l = &p->layout;
    u32 w = (u32)f->width, h = (u32)f->height;
    u64 cpu = p->cpu + (u64)(id - p->first) * l->image_bytes;
    /* an image of four bytes a pixel takes its picture through VIC, as NV12 in its own first bytes, where it fits: VIC
       copies its inputs before it composes (vk/vic.c), so composing over them is safe */
    int nv12 = nvideo_kind_nv12(l->kind);
    u32 pitch = nv12 ? l->pitch : (u32)snx_align_up(w, 256);
    u64 ext_y = (u64)pitch * l->rows, ext_c = ext_y / 2;
    NvdecPic pic = {.w = w,
                    .h = h,
                    .luma = cpu,
                    .chroma = cpu + (nv12 ? l->chroma_offset : ext_y),
                    .ext_y = ext_y,
                    .ext_c = nv12 ? (u64)l->pitch * l->chroma_rows : ext_c,
                    .pitch_y = pitch,
                    .pitch_c = pitch,
                    .surf_h = l->rows,
                    .pitch_linear = 1};
    if ((f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) ||
        (!nv12 && ext_y + ext_c > l->image_bytes) || w > (l->extent & 0xffff) + 1u || h > (l->extent >> 16) + 1u) {
        WARN_LIMIT(4, "gsv: a %ux%u picture (format %d) doesn't fit video image %u (kind %u, extent %#x)\n", w, h,
                   f->format, id, l->kind, l->extent);
        return 0;
    }
    if (!cpu || mem_mapped_run(cpu, l->image_bytes) != l->image_bytes) {
        WARN_LIMIT(4, "gsv: video image %u at %#llx isn't mapped\n", id, (unsigned long long)cpu);
        return 0;
    }
    if (!g_renderer_up || !g_renderer->nvdec_write) {
        WARN_LIMIT(4, "gsv: a video picture before the renderer started, dropped\n");
        return 0;
    }
    for (int i = 0; i < 3; i++) { /* Y, U, V */
        pic.plane[i] = f->data[i];
        pic.stride[i] = f->linesize[i];
    }
    if (!g_renderer->nvdec_write(&pic)) return 0;
    if (nv12) return 1;
    if (!g_renderer->vic_compose) {
        WARN_LIMIT(4, "gsv: this renderer can't compose video image %u\n", id);
        return 0;
    }
    /* kinds 2 and 5 have 0x9 in their CPU descriptor (nvideo_cpu_descriptor), 3 and 6 0x8. Which channel order each
       means is read from the SDK's names only, and not checked against a picture on the console */
    VicJob j = {.fmt = l->kind == 2 || l->kind == 5 ? VIC_PIXEL_A8B8G8R8 : VIC_PIXEL_A8R8G8B8,
                .sw = w,
                .sh = h,
                .tw = w,
                .th = h,
                .bg = {0, 0, 0, 1023}};
    j.oy = (VicPlane){.p = GP(cpu), .lim = l->image_bytes, .pitch = l->pitch, .h = l->rows};
    VicSlot* s = &j.slots[0];
    s->on = 1;
    s->w = w;
    s->h = h;
    s->iy = (VicPlane){.p = GP(cpu), .lim = ext_y, .pitch = pitch, .h = l->rows};
    s->ic = (VicPlane){.p = GP(cpu + ext_y), .lim = ext_c, .pitch = pitch, .h = l->rows / 2};
    rgb_matrix(s, f);
    return g_renderer->vic_compose(&j);
}
/* whether the stand-in painted the image black already: the picture never changes, and painting it again on the GPU
   would have every frame's CPU read of it wait for it to land */
static int painted_black(const NativeVideoCodec* s, u32 id) {
    for (u32 i = 0; i < HOST(s)->nblack; i++)
        if (HOST(s)->black_images[i] == id) return 1;
    return 0;
}
/* the image filled with the frame (NULL: the end of the stream, no picture), its output queued and its fence reached */
static void codec_output(NativeVideoCodec* s, NativeVideoImage* image, const AVFrame* f, u64 timestamp) {
    NativeVideoPool* p = nvideo_pool_find(NV->pools, NVIDEO_POOLS, image->id);
    u32 w = f ? (u32)f->width : 0, h = f ? (u32)f->height : 0;
    if (image->wait_id < SYNCPT_IDS && !syncpt_wait(image->wait_id, image->wait_value, 200000000))
        WARN_LIMIT(4, "gsv: video image %u: the game's fence %u on syncpoint %u isn't reached, written anyway\n",
                   image->id, image->wait_value, image->wait_id);
    if (f && p && !(f == HOST(s)->black && painted_black(s, image->id))) {
        if (image_write(p, image->id, f) && f == HOST(s)->black && HOST(s)->nblack < NVIDEO_IMAGES)
            HOST(s)->black_images[HOST(s)->nblack++] = image->id;
    } else if (f && !p)
        WARN_LIMIT(4, "gsv: video image %u has no pool: its picture is dropped\n", image->id);
    u32 props[NVIDEO_PROPS];
    nvideo_props(props, image->id, s->width, s->height, w ? w : s->width, h ? h : s->height, timestamp, !f);
    if (!(s->frames++ % 120) || !f)
        LOG("gsv: video output %u: image %u, %ux%u, %llu us%s\n", s->frames, image->id, w, h,
            (unsigned long long)timestamp, f ? "" : ", the end of the stream");
    /* as host1x's NVDEC jobs: the increments after the picture's GPU work is recorded, which the game's GPU waits
       and CPU reads of the image then follow */
    syncpt_add(s->syncpoint, nvideo_image_fill(s, image, props));
}
/* An AV1 stream in a build without dav1d (cmake/dav1d.cmake: not built for its target, or turned off), whose FFmpeg has
   no AV1 decoder that decodes in software: each input comes out as a black picture with the input's timestamp, and the
   end of the stream as it is, so the game plays the movie through, black, and goes on */
static void standin_pump(NativeVideoCodec* s) {
    NativeVideoImage* image;
    while (s->started && s->input_count && (image = nvideo_image_next(s))) {
        NativeVideoInput* in = &s->inputs[s->input_head];
        codec_output(s, image, in->eos ? NULL : HOST(s)->black, in->timestamp);
        av_packet_unref(HOST(s)->packets[s->input_head]);
        nvideo_input_pop(s);
    }
}
/* the decoder fed and its pictures written while images are queued */
static void codec_pump(NativeVideoCodec* s) {
    AVCodecContext* dec = HOST(s)->decoder;
    AVFrame* frame = HOST(s)->frame;
    if (HOST(s)->black) {
        standin_pump(s);
        submission_event(s);
        return;
    }
    while (dec && s->started) {
        NativeVideoImage* image;
        if (frame->buf[0]) {
            if (!(image = nvideo_image_next(s))) break;
            codec_output(s, image, frame, frame->pts == AV_NOPTS_VALUE ? 0 : (u64)frame->pts);
            av_frame_unref(frame);
            continue;
        }
        if (s->eos == 2) {
            if (!(image = nvideo_image_next(s))) break;
            codec_output(s, image, NULL, s->eos_timestamp);
            /* the stream ended: the decoder starts the next one (a movie that loops) from nothing */
            avcodec_flush_buffers(dec);
            s->eos = 0;
            continue;
        }
        int r = avcodec_receive_frame(dec, frame);
        if (!r) continue;
        if (r == AVERROR_EOF) {
            if (s->eos == 1) {
                s->eos = 2;
                continue;
            }
            avcodec_flush_buffers(dec);
            break;
        }
        if (r != AVERROR(EAGAIN)) {
            WARN_LIMIT(8, "gsv: the video decoder failed (%d)\n", r);
            break;
        }
        if (!s->input_count) break;
        NativeVideoInput* in = &s->inputs[s->input_head];
        AVPacket* pkt = HOST(s)->packets[s->input_head];
        r = avcodec_send_packet(dec, in->eos ? NULL : pkt);
        if (r == AVERROR(EAGAIN)) break;
        if (r < 0 && r != AVERROR_EOF) WARN_LIMIT(8, "gsv: the video decoder refused a packet (%d)\n", r);
        if (in->eos) {
            s->eos = 1;
            s->eos_timestamp = in->timestamp;
        }
        av_packet_unref(pkt);
        nvideo_input_pop(s);
    }
    submission_event(s);
}

static int codec_initialize(NativeVideoCodec* s, u32 tmem, u64 size, u32 codec, u32 input_format,
                            u32 output_format) {
    LOG("gsv: video codec %u initialized: work memory %#llx bytes%s, codec %u, input format %u, output format %u\n",
        codec_index(s), (unsigned long long)size, tmem ? " (transfer memory)" : "", codec, input_format,
        output_format);
    if (s->state != NVIDEO_NEW) codec_reset(s);
    s->state = NVIDEO_INITIALIZED;
    s->tmem = tmem;
    s->work_size = size;
    s->codec = codec;
    s->input_format = input_format;
    s->output_format = output_format;
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_InitializeWithTransferMemory(SvcObj* self, u32 memory, u64 size, u32 codec,
                                                                 u32 input_format, u32 output_format) {
    NativeVideoCodec* s = self->data;
    KTmem* m = (KTmem*)h_get_ref(memory, KO_TMEM);
    R_UNLESS(m, SNX_RC_SVC_INVALID_HANDLE);
    if (size > m->size) {
        kobj_unref(&m->o);
        R_THROW(NVIDEO_BAD_ARGUMENT);
    }
    return codec_initialize(s, kobj_id(&m->o), size, codec, input_format, output_format);
}
static int nn_gsv_INativeVideoCodec_InitializeH264WithTransferMemory(SvcObj* self, u32 memory, u64 size, u32 codec,
                                                                     u32 input_format, u32 output_format, u32 extra) {
    (void)extra;
    return nn_gsv_INativeVideoCodec_InitializeWithTransferMemory(self, memory, size, codec, input_format,
                                                                 output_format);
}
static int nn_gsv_INativeVideoCodec_InitializeWithProcess(SvcObj* self, u32 process, u64 size, u32 codec,
                                                          u32 input_format, u32 output_format) {
    (void)process;
    return codec_initialize(self->data, 0, size, codec, input_format, output_format);
}
static int nn_gsv_INativeVideoCodec_Configure(SvcObj* self, IpcBuf mime, const nn_gsv_VideoConfiguration* cfg,
                                              u64* reserved) {
    NativeVideoCodec* s = self->data;
    (void)reserved;
    R_UNLESS(s->state == NVIDEO_INITIALIZED && !s->encoder, NVIDEO_BAD_STATE);
    R_UNLESS(mime.addr && mime.size && mime.size <= 64 && mem_mapped_run(mime.addr, mime.size) == mime.size,
             NVIDEO_BAD_ARGUMENT);
    u32 kind = nvideo_mime(GP(mime.addr), mime.size);
    enum AVCodecID id = kind == NVIDEO_MIME_VP9    ? AV_CODEC_ID_VP9
                        : kind == NVIDEO_MIME_VP8  ? AV_CODEC_ID_VP8
                        : kind == NVIDEO_MIME_H264 ? AV_CODEC_ID_H264
                        : kind == NVIDEO_MIME_AV1  ? AV_CODEC_ID_AV1
                                                   : AV_CODEC_ID_NONE;
    /* The SDK's H.264 decoder (command 201, codec 0) passes down its caller's buffer here, not a fixed MIME type: read
       as the stream's configuration, which starts with a NUL where it has start codes */
    int h264_config = id == AV_CODEC_ID_NONE && s->codec == 0;
    if (h264_config) {
        const u8* b = GP(mime.addr);
        LOG("gsv: video codec %u: H.264 configuration of %llu bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            codec_index(s), (unsigned long long)mime.size, b[0], mime.size > 1 ? b[1] : 0, mime.size > 2 ? b[2] : 0,
            mime.size > 3 ? b[3] : 0, mime.size > 4 ? b[4] : 0, mime.size > 5 ? b[5] : 0, mime.size > 6 ? b[6] : 0,
            mime.size > 7 ? b[7] : 0);
        id = AV_CODEC_ID_H264;
    }
    if (id == AV_CODEC_ID_NONE) {
        char name[65] = {0};
        memcpy(name, GP(mime.addr), (size_t)mime.size);
        WARN("gsv: video codec %u: no decoder for \"%s\"\n", codec_index(s), name);
        R_THROW(NVIDEO_BAD_ARGUMENT);
    }
    R_UNLESS(cfg->width && cfg->height && cfg->width <= 8192 && cfg->height <= 8192, NVIDEO_BAD_ARGUMENT);
    /* AV1: dav1d's decoder, the only one this FFmpeg has for it, or the black stand-in in a build without it */
    const AVCodec* codec = avcodec_find_decoder(id);
    int av1 = id == AV_CODEC_ID_AV1 && !codec;
    AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : NULL;
    int ok = dec != NULL || av1;
    if (av1 && (HOST(s)->black = av_frame_alloc())) { /* the stand-in's picture: limited range black */
        AVFrame* f = HOST(s)->black;
        f->format = AV_PIX_FMT_YUV420P;
        f->width = (int)cfg->width;
        f->height = (int)cfg->height;
        ok = av_frame_get_buffer(f, 0) >= 0;
        for (int i = 0; i < 3 && ok; i++)
            memset(f->data[i], i ? 128 : 16, (size_t)f->linesize[i] * (size_t)(i ? (f->height + 1) / 2 : f->height));
        if (ok)
            WARN("gsv: video codec %u: an AV1 stream, which this build has no decoder for: its pictures come out black\n",
                 codec_index(s));
    } else if (av1)
        ok = 0;
    if (dec) {
        dec->thread_count = 4;
        dec->thread_type = FF_THREAD_SLICE; /* no frame threads: each packet's picture comes out at once */
        if (id == AV_CODEC_ID_AV1) dec->flags |= AV_CODEC_FLAG_LOW_DELAY; /* dav1d's frame delay 1, for the same */
        dec->pkt_timebase = (AVRational){1, 1000000};
        if (h264_config && (dec->extradata = av_mallocz((size_t)mime.size + AV_INPUT_BUFFER_PADDING_SIZE))) {
            memcpy(dec->extradata, GP(mime.addr), (size_t)mime.size);
            dec->extradata_size = (int)mime.size;
        }
        ok = avcodec_open2(dec, codec, NULL) >= 0;
    }
    HOST(s)->decoder = dec;
    HOST(s)->frame = ok ? av_frame_alloc() : NULL;
    ok = ok && HOST(s)->frame;
    for (u32 i = 0; i < NVIDEO_INPUTS && ok; i++) ok = (HOST(s)->packets[i] = av_packet_alloc()) != NULL;
    if (!ok) {
        WARN("gsv: video codec %u: FFmpeg's decoder couldn't be opened\n", codec_index(s));
        host_free(s);
        R_THROW(NVIDEO_NO_ROOM);
    }
    s->event = owned_event("gsv:video submission");
    s->mime = kind;
    s->width = cfg->width;
    s->height = cfg->height;
    s->frame_rate = cfg->frame_rate;
    s->capacity = cfg->input_capacity;
    s->state = NVIDEO_CONFIGURED;
    submission_event(s);
    LOG("gsv: video codec %u configured: %s %ux%u at %u FPS, inputs of %u bytes at most\n", codec_index(s),
        codec ? codec->name : "av1 (black stand-in)", s->width, s->height, s->frame_rate, s->capacity);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_GetSubmissionEvent(SvcObj* self, u32* event) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->event, NVIDEO_BAD_STATE);
    *event = ev_handle(s->event);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static void video_fence(const NativeVideoCodec* s, u32 value, nn_gsv_VideoFence* fence) {
    *fence = (nn_gsv_VideoFence){.encoded_id = ~s->syncpoint, .value = value};
}
static int nn_gsv_INativeVideoCodec_RegisterImage(SvcObj* self, u32 image, const u32* reserved,
                                                  nn_gsv_VideoFence* fence) {
    NativeVideoCodec* s = self->data;
    (void)reserved;
    R_UNLESS(s->state != NVIDEO_NEW, NVIDEO_BAD_STATE);
    R_UNLESS(nvideo_pool_find(NV->pools, NVIDEO_POOLS, image), NVIDEO_NOT_FOUND);
    if (!s->syncpoint) {
        s->syncpoint = syncpt_alloc();
        s->issued = s->next_fence = syncpt_read(s->syncpoint);
    }
    u32 value;
    R_TRY(nvideo_image_register(s, image, &value));
    LOG("gsv: video codec %u: image %u registered, fence %u on syncpoint %u\n", codec_index(s), image, value,
        s->syncpoint);
    video_fence(s, value, fence);
    codec_pump(s);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_ReleaseImage(SvcObj* self, u32 image, u32 reserved,
                                                 const nn_gsv_NativeSyncHandle* wait, u64 reserved2,
                                                 nn_gsv_VideoFence* fence) {
    NativeVideoCodec* s = self->data;
    (void)reserved, (void)reserved2;
    u32 wait_id = ~wait->encoded_id, value;
    if (wait_id < SYNCPT_IDS) LOGV("gsv: video image %u released after syncpoint %u reaches %u\n", image, wait_id,
                                   (u32)wait->value);
    R_TRY(nvideo_image_release(s, image, wait_id < SYNCPT_IDS ? wait_id : ~0u, (u32)wait->value, &value));
    video_fence(s, value, fence);
    codec_pump(s);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_UnregisterImage(SvcObj* self, u32 image, const u32* reserved) {
    (void)reserved;
    LOG("gsv: video codec %u: image %u unregistered\n", codec_index(self->data), image);
    return nvideo_image_unregister(self->data, image);
}
static int nn_gsv_INativeVideoCodec_Start(SvcObj* self) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->state == NVIDEO_CONFIGURED, NVIDEO_BAD_STATE);
    s->started = 1;
    LOG("gsv: video codec %u started\n", codec_index(s));
    codec_pump(s);
    R_SUCCEED();
}
/* the inputs queued go to the decoder, and their pictures out as images come free: the decoder keeps its reference
   pictures, as the movie library's pause flushes in the middle of a stream */
static int nn_gsv_INativeVideoCodec_Flush(SvcObj* self) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->state == NVIDEO_CONFIGURED, NVIDEO_BAD_STATE);
    codec_pump(s);
    R_SUCCEED();
}
/* the inputs and the decoder's pictures dropped. The outputs already written stay for GetOutputProperties: the
   client's queue holds their images */
static int nn_gsv_INativeVideoCodec_Stop(SvcObj* self) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->state == NVIDEO_CONFIGURED, NVIDEO_BAD_STATE);
    if (HOST(s)->decoder) avcodec_flush_buffers(HOST(s)->decoder);
    av_frame_unref(HOST(s)->frame);
    while (s->input_count) {
        av_packet_unref(HOST(s)->packets[s->input_head]);
        nvideo_input_pop(s);
    }
    s->started = 0;
    s->eos = 0;
    submission_event(s);
    LOG("gsv: video codec %u stopped after %u outputs\n", codec_index(s), s->frames);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_Submit(SvcObj* self, IpcBuf input, u64 timestamp, u8 eos, u64* consumed) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->state == NVIDEO_CONFIGURED, NVIDEO_BAD_STATE);
    R_UNLESS(input.size <= INT_MAX && (!input.size || mem_mapped_run(input.addr, input.size) == input.size),
             NVIDEO_BAD_ARGUMENT);
    u32 need = (input.size != 0) + (eos != 0);
    if (s->input_count + need > NVIDEO_INPUTS) {
        codec_pump(s);
        if (s->input_count + need > NVIDEO_INPUTS) {
            ev_clear(s->event);
            R_THROW(NVIDEO_INPUT_FULL);
        }
    }
    if (input.size) {
        int slot = nvideo_input_push(s, (u32)input.size, timestamp, 0);
        AVPacket* pkt = HOST(s)->packets[slot];
        if (av_new_packet(pkt, (int)input.size) < 0) {
            s->input_count--;
            R_THROW(NVIDEO_NO_ROOM);
        }
        memcpy(pkt->data, GP(input.addr), (size_t)input.size);
        pkt->pts = (i64)timestamp;
    }
    if (eos) nvideo_input_push(s, 0, timestamp, 1);
    codec_pump(s);
    *consumed = input.size;
    R_SUCCEED();
}
/* The SDK asks after the image's fence (RequestSyncPointEvent), which the output's fill reaches: an output waits then.
   Its loop (GetVp9DecoderOutputBufferProperties) passes any failure up, and the game aborts on it */
static int nn_gsv_INativeVideoCodec_GetOutputProperties(SvcObj* self, u32* properties) {
    NativeVideoCodec* s = self->data;
    codec_pump(s);
    int rc = nvideo_output_pop(s, properties);
    if (rc)
        WARN_LIMIT(4,
                   "gsv: video codec %u: output properties asked before an output (%s image for the decoder, %u "
                   "inputs)\n",
                   codec_index(s), nvideo_image_next(s) ? "an" : "no", s->input_count);
    return rc;
}
static int nn_gsv_INativeVideoCodec_Finalize(SvcObj* self) {
    LOG("gsv: video codec %u finalized\n", codec_index(self->data));
    codec_reset(self->data);
    R_SUCCEED();
}
static int nvideo_codec_dispatch(SvcObj* self, IpcCtx* c) {
    pm_lock(&g_host.native_video.lock);
    int rc = nn_gsv_INativeVideoCodec_dispatch(self, c);
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
static void nvideo_codec_destroy(SvcObj* self) {
    pm_lock(&g_host.native_video.lock);
    codec_reset(self->data);
    pm_unlock(&g_host.native_video.lock);
}

/* ============================================================ the context */
static int nn_gsv_INativeVideoContext_CreateCodec(SvcObj* self, u32* codec) {
    const NativeVideoContext* context = self->data;
    SvcObj* obj = SVCOBJ_NEW_DATA("nn::gsv::INativeVideoCodec", nvideo_codec_dispatch, NV->codecs, NV->codec_next,
                                  "native video codecs");
    ((NativeVideoCodec*)obj->data)->encoder = context->encoder;
    obj->destroy = nvideo_codec_destroy;
    *codec = session_new(obj);
    R_UNLESS(*codec, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nvideo_context_dispatch(SvcObj* self, IpcCtx* c) {
    pm_lock(&g_host.native_video.lock);
    int rc = nn_gsv_INativeVideoContext_dispatch(self, c);
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_open(int encoder, u32* session) {
    SvcObj* obj = SVCOBJ_NEW_DATA("nn::gsv::INativeVideoContext", nvideo_context_dispatch, NV->contexts,
                                  NV->context_next, "native video contexts");
    ((NativeVideoContext*)obj->data)->encoder = encoder != 0;
    *session = session_new(obj);
    R_UNLESS(*session, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}

/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_native_video_state_fns, STATE_FN("native_video", nvideo_context_dispatch),
               STATE_FN("native_video", nvideo_codec_dispatch), STATE_FN("native_video", nvideo_codec_destroy));
