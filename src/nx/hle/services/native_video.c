/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Protocol ported from eden-ns2's native_video_decoder.cpp, native_video_layout.h and
   graphics_supervisor_protocol.h, copyright 2026 Eden Emulator Project, GPL-3.0-or-later.
   Pixel interleave and layout conversion use ScarletNX's GPU path, not the reference's CPU conversion. */
#include "nx/hle/hle.h"
#include "nx/hle/services/native_video.h"
#include "nx/gpu/renderer.h"
#include "common/utilities/endian.h"
#include <libavcodec/avcodec.h>
#include <limits.h>

static int video_context_dispatch(SvcObj* self, IpcCtx* c);
static int video_codec_dispatch(SvcObj* self, IpcCtx* c);
static void video_codec_destroy(SvcObj* self);
#include "hle/services/native_video.swipc.inc"

#define VIDEO_EMPTY RC(111, 3604)
#define VIDEO_FULL  RC(111, 3603)

static int video_buffer_mapped(IpcBuf b) {
    return b.addr && b.addr < AS_SIZE && b.size <= AS_SIZE - b.addr && mem_mapped_run(b.addr, b.size) == b.size;
}

static NativeVideoPool* video_pool(u32 image) {
    for (u32 i = 0; i < NVIDEO_POOLS; ++i) {
        NativeVideoPool* p = &g_hle.native_video.pools[i];
        if (p->first && image >= p->first && image - p->first < p->count) return p;
    }
    return NULL;
}
static int video_pool_used(const NativeVideoPool* p) {
    for (u32 c = 0; c < NVIDEO_CODECS; ++c)
        for (u32 i = 0; i < NVIDEO_IMAGES; ++i) {
            u32 id = g_hle.native_video.codecs[c].images[i].id;
            if (id >= p->first && id - p->first < p->count) return 1;
        }
    for (u32 i = 0; i < NVIDEO_MAPS; ++i) {
        u32 id = g_hle.native_video.maps[i].image;
        if (id >= p->first && id - p->first < p->count) return 1;
    }
    return 0;
}
/* A retired pool's record goes when no registered image or GPU mapping names it. Its native image went at 912 */
static void video_collect_pools(void) {
    for (u32 i = 0; i < NVIDEO_POOLS; ++i) {
        NativeVideoPool* p = &g_hle.native_video.pools[i];
        if (p->first && p->retired && !video_pool_used(p)) *p = (NativeVideoPool){0};
    }
}
static int video_layout(const u32 pref[16], u32 count, NativeVideoPool* p, u64* size) {
    R_UNLESS(!pref[0] && pref[1] == 1 && count && count <= 1024, RC(1, 33));
    for (u32 i = 3; i < 16; ++i) R_UNLESS(!pref[i], RC(1, 33));
    u32 w = (pref[2] & 0xffff) + 1, h = (pref[2] >> 16) + 1;
    R_UNLESS(w >= 2 && h >= 2 && !((w | h) & 1), RC(1, 101));
    u64 pitch = (w + 255u) & ~255u;
    u64 chroma = pitch * ((h + 3u) & ~3u);
    u64 bytes = chroma + pitch * (((h / 2) + 3u) & ~3u);
    if (bytes < 4096) bytes = 4096;
    R_UNLESS(bytes <= UINT32_MAX / count, RC(1, 101));
    *p = (NativeVideoPool){.count = count,
                           .extent = pref[2],
                           .pitch = (u32)pitch,
                           .chroma_offset = (u32)chroma,
                           .image_bytes = (u32)bytes,
                           .luma_bytes = (u32)(pitch * h),
                           .chroma_bytes = (u32)(pitch * (h / 2))};
    *size = bytes * count;
    R_SUCCEED();
}
int native_video_service(int encoder, int enable) {
    pm_lock(&g_host.native_video.lock);
    u32* ready = encoder ? &g_hle.native_video.encoder_ready : &g_hle.native_video.decoder_ready;
    int rc = !enable && !*ready ? RC(1, 125) : 0;
    if (!rc) *ready = enable != 0;
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_requirements(const u32 pref[16], u32 count, u64* size, u64* alignment) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool p;
    int rc = g_hle.native_video.decoder_ready ? video_layout(pref, count, &p, size) : RC(1, 125);
    if (!rc) *alignment = 4096;
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_pool_create(const u32 pref[16], u32 count, u32 memory, u32* image) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool p, *slot = NULL;
    u64 size = 0;
    int rc = g_hle.native_video.decoder_ready ? video_layout(pref, count, &p, &size) : RC(1, 125);
    for (u32 i = 0; !rc && i < NVIDEO_POOLS && !slot; ++i)
        if (!g_hle.native_video.pools[i].first) slot = &g_hle.native_video.pools[i];
    if (!rc && !slot) rc = RC(1, 103);
    if (!rc) {
        NvGsvImageShape shape = {.extent = pref[2], .pixel_format = 0xa};
        rc = nv_native_image_create(memory, size, count, &shape, image);
        if (!rc) {
            p.first = *image;
            p.memory = memory;
            u32 memory_size;
            rc = nvmap_get(memory, &p.cpu, &memory_size) ? 0 : RC(1, 121);
            if (rc)
                nv_native_image_destroy(*image);
            else
                *slot = p;
        }
    }
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
/* The SDK destroys the native memory right after 912, with images still registered and mapped. The native image and
   its reference on the memory go now. A mapping pins the backing until it is unmapped, and a registered image keeps
   the retired pool's layout */
int native_video_pool_destroy(u32 image) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = video_pool(image);
    int rc = !p || p->first != image || p->retired ? RC(1, 121) : nv_native_image_destroy(image);
    if (!rc) {
        p->retired = 1;
        video_collect_pools();
    }
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_query(u32 image, u64 d[16]) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = video_pool(image);
    int rc = p && !p->retired ? 0 : RC(1, 121);
    if (!rc) {
        u64 off = (u64)(image - p->first) * p->image_bytes;
        d[0] = (u64)p->extent << 32 | 0xa;
        d[1] = (u64)p->luma_bytes << 32 | off;
        d[2] = p->pitch;
        d[3] = (u64)p->chroma_bytes << 32 | (off + p->chroma_offset);
        d[4] = p->pitch;
    }
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
static void video_gpu_descriptor(const NativeVideoPool* p, u32 image, u64 d[16]) {
    d[0] = 1;
    d[1] = p->extent;
    d[4] = 0x101000001ull;
    d[5] = (u64)p->image_bytes << 32 | (u64)(image - p->first) * p->image_bytes;
}
int native_video_query_gpu(u32 image, u64 d[16]) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = video_pool(image);
    int rc = p && !p->retired ? 0 : RC(1, 121);
    if (!rc) video_gpu_descriptor(p, image, d);
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_map(u32 image, u32 as, u64* address, u64* last, u64 d[16]) {
    pm_lock(&g_host.native_video.lock);
    NativeVideoPool* p = video_pool(image);
    NativeVideoMap* m = NULL;
    for (u32 i = 0; i < NVIDEO_MAPS; ++i)
        if (g_hle.native_video.maps[i].image == image && g_hle.native_video.maps[i].address_space == as)
            m = &g_hle.native_video.maps[i];
    int rc = p && !p->retired ? 0 : RC(1, 121);
    if (!rc && m) {
        if (m->refs == UINT32_MAX)
            rc = RC(1, 103);
        else
            ++m->refs;
    } else if (!rc) {
        for (u32 i = 0; i < NVIDEO_MAPS && !m; ++i)
            if (!g_hle.native_video.maps[i].image) m = &g_hle.native_video.maps[i];
        if (!m)
            rc = RC(1, 103);
        else {
            u64 gva = 0;
            rc = nv_native_image_map(as, p->memory, (u64)(image - p->first) * p->image_bytes, p->image_bytes, &gva);
            if (!rc) *m = (NativeVideoMap){image, as, 1, gva, p->image_bytes};
        }
    }
    if (!rc) {
        *address = m->address;
        *last = m->address + m->size - 1;
        video_gpu_descriptor(p, image, d);
        d[5] &= 0xffffffff00000000ull;
    }
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
int native_video_unmap(u32 image, u32 as, u64 address, u64 last) {
    pm_lock(&g_host.native_video.lock);
    int rc = RC(1, 121);
    for (u32 i = 0; i < NVIDEO_MAPS; ++i) {
        NativeVideoMap* m = &g_hle.native_video.maps[i];
        if (m->image != image || m->address_space != as || m->address != address || last < address ||
            last - address != m->size - 1)
            continue;
        rc = 0;
        if (m->refs == 1) {
            rc = nv_native_image_unmap(address, m->size);
            if (!rc) *m = (NativeVideoMap){0};
        } else
            --m->refs;
        break;
    }
    if (!rc) video_collect_pools();
    pm_unlock(&g_host.native_video.lock);
    return rc;
}

static NativeVideoHostCodec* video_host(NativeVideoCodec* s) {
    return &g_host.native_video.codecs[s - g_hle.native_video.codecs];
}
static void video_host_free(NativeVideoHostCodec* h) {
    avcodec_free_context(&h->decoder);
    av_frame_free(&h->frame);
    for (u32 i = 0; i < NVIDEO_INPUTS; ++i) av_packet_free(&h->packets[i]);
}
static void video_reset(NativeVideoCodec* s) {
    if (s->syncpoint) {
        if (g_renderer_up && g_renderer->release_retire) g_renderer->release_retire(s->syncpoint);
        syncpt_free(s->syncpoint);
    }
    video_host_free(video_host(s));
    kobj_unref(kobj_at(s->tmem));
    kobj_unref(kobj_at(s->event));
    *s = (NativeVideoCodec){.obj = s->obj, .encoder = s->encoder};
    video_collect_pools();
}
static void video_submission_event(NativeVideoCodec* s) {
    if (s->started && s->input_count < NVIDEO_INPUTS && s->input_bytes < ((KTmem*)kobj_at(s->tmem))->size && !s->eos)
        ev_signal(s->event);
    else
        ev_clear(s->event);
}
/* Runs independently of parked graphics channels. The packet frame stays alive until its CPU rows have been staged.
   The renderer's release queue signals the acquisition fence only after the actual GPU writes finish. */
typedef struct {
    NvdecPic picture;
    u32 syncpoint, increments, done;
    int ok;
} VideoUpload;
static void video_upload(void* arg) {
    VideoUpload* u = arg;
    u->ok = !u->picture.w || g_renderer->nvdec_write(&u->picture);
    if (u->ok) {
        for (u32 i = 0; i < u->increments; ++i) g_renderer->stream_incr(u->syncpoint);
        g_renderer->stream_end();
    }
    __atomic_store_n(&u->done, 1, __ATOMIC_RELEASE);
    plat_wake_word(&u->done);
}
static int video_output(NativeVideoCodec* s, NativeVideoImage* image, const AVFrame* f, u64 timestamp) {
    NativeVideoPool* p = video_pool(image->id);
    R_UNLESS(p && g_renderer_up && g_renderer->nvdec_write && g_renderer->stream_incr && g_renderer->stream_end,
             RC(1, 125));
    VideoUpload u = {.syncpoint = s->syncpoint, .increments = image->fence - s->issued};
    if (f) {
        R_UNLESS((f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) && f->width > 0 &&
                     f->height > 0 && (u32)f->width <= (p->extent & 0xffff) + 1 &&
                     (u32)f->height <= (p->extent >> 16) + 1,
                 RC(1, 33));
        u64 cpu = p->cpu + (u64)(image->id - p->first) * p->image_bytes;
        u.picture = (NvdecPic){.w = (u32)f->width,
                               .h = (u32)f->height,
                               .luma = cpu,
                               .chroma = cpu + p->chroma_offset,
                               .ext_y = p->luma_bytes,
                               .ext_c = p->chroma_bytes,
                               .pitch_y = p->pitch,
                               .pitch_c = p->pitch,
                               .pitch_linear = 1};
        for (u32 i = 0; i < 3; ++i) {
            R_UNLESS(f->linesize[i] > 0, RC(1, 33));
            u.picture.plane[i] = f->data[i];
            u.picture.stride[i] = f->linesize[i];
        }
    }
    gpu_call(video_upload, &u, NULL, 0);
    while (!__atomic_load_n(&u.done, __ATOMIC_ACQUIRE)) plat_wait_word(&u.done, 0, -1);
    R_UNLESS(u.ok, RC(1, 103));
    s->issued = image->fence;
    image->available = 0;
    u32* out = s->output[(s->output_head + s->output_count++) % NVIDEO_IMAGES];
    u32 props[16] = {image->id,
                     s->width,
                     s->height,
                     f ? (u32)f->width : 0,
                     f ? (u32)f->height : 0,
                     0,
                     (u32)(timestamp * 1000),
                     (u32)((timestamp * 1000) >> 32),
                     f ? 0u : 1u};
    memcpy(out, props, sizeof props);
    LOGV("video: native output image %u, %ux%u, timestamp %llu ns, eos %u\n", image->id, props[3], props[4],
         (unsigned long long)(timestamp * 1000), props[8]);
    R_SUCCEED();
}
static int video_pump(NativeVideoCodec* s) {
    NativeVideoHostCodec* h = video_host(s);
    while (s->started && s->output_count < NVIDEO_IMAGES) {
        NativeVideoImage* image = NULL;
        for (u32 i = 0; i < NVIDEO_IMAGES; ++i) {
            NativeVideoImage* q = &s->images[i];
            if (q->id && q->available && (!image || q->fence - s->issued < image->fence - s->issued)) image = q;
        }
        if (!image) break;
        int r = avcodec_receive_frame(h->decoder, h->frame);
        if (!r) {
            R_TRY(video_output(s, image, h->frame, (u64)h->frame->pts));
            av_frame_unref(h->frame);
            continue;
        }
        if (r == AVERROR_EOF && s->eos == 2) {
            R_TRY(video_output(s, image, NULL, s->eos_timestamp));
            /* EOS finishes one stream. The same configured codec can start another, including a tutorial loop.
               A normal pause Flush does not take this path and keeps the VP9 reference pictures. */
            avcodec_flush_buffers(h->decoder);
            s->eos = 0;
            continue;
        }
        R_UNLESS(r == AVERROR(EAGAIN), RC(1, 125));
        if (!s->input_count) break;
        AVPacket* pkt = h->packets[s->input_head];
        r = avcodec_send_packet(h->decoder, pkt->size ? pkt : NULL);
        if (r < 0) {
            WARN("video: native VP9 packet rejected (%d)\n", r);
            return RC(1, 125);
        }
        if (!pkt->size) {
            s->eos = 2;
            s->eos_timestamp = (u64)pkt->pts;
        }
        s->input_bytes -= (u32)pkt->size;
        av_packet_unref(pkt);
        s->input_head = (s->input_head + 1) % NVIDEO_INPUTS;
        --s->input_count;
    }
    video_submission_event(s);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_Initialize201(SvcObj* self, u32 memory, const u8* size, u32 codec, u32 input_format,
                                                  u32 output_format, u32 output_mode) {
    NativeVideoCodec* s = self->data;
    /* Input format 1 came with a 640x360 movie and 3 with a 2560x1440 one. Both submit plain VP9 frames */
    R_UNLESS(!s->encoder && !codec && (input_format == 1 || input_format == 3) && output_format == 3 &&
                 output_mode == 1,
             RC(1, 33));
    R_UNLESS(!s->tmem, RC(1, 125));
    KTmem* m = (KTmem*)h_get_ref(memory, KO_TMEM);
    R_UNLESS(m, RC(1, 114));
    if (!snx_ld_le64(size) || snx_ld_le64(size) != m->size) {
        kobj_unref(&m->o);
        return RC(1, 101);
    }
    s->tmem = kobj_id(&m->o);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_Initialize200(SvcObj* self, u32 memory, const u8* size, u32 codec, u32 input_format,
                                                  u32 output_format) {
    R_UNLESS(codec == 1, RC(1, 33));
    return nn_gsv_INativeVideoCodec_Initialize201(self, memory, size, 0, input_format, output_format, 1);
}
static int nn_gsv_INativeVideoCodec_Configure(SvcObj* self, IpcBuf mime, const nn_gsv_VideoConfiguration* cfg,
                                              u64* reserved) {
    (void)reserved;
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->tmem && !s->width, RC(1, 125));
    const char name[] = "video/vp9";
    R_UNLESS(mime.size == sizeof name && video_buffer_mapped(mime) && !memcmp(GP(mime.addr), name, sizeof name),
             RC(1, 33));
    R_UNLESS(cfg->width && cfg->height && cfg->width <= 65535 && cfg->height <= 65535 && cfg->frame_rate &&
                 cfg->frame_rate <= 60 && cfg->input_capacity &&
                 cfg->input_capacity <= ((KTmem*)kobj_at(s->tmem))->size,
             RC(1, 101));
    for (u32 i = 0; i < 4; ++i) R_UNLESS(!cfg->reserved[i], RC(1, 33));
    NativeVideoHostCodec* h = video_host(s);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
    h->decoder = codec ? avcodec_alloc_context3(codec) : NULL;
    h->frame = av_frame_alloc();
    int ok = h->decoder && h->frame;
    for (u32 i = 0; i < NVIDEO_INPUTS; ++i) {
        h->packets[i] = av_packet_alloc();
        ok = ok && h->packets[i];
    }
    if (ok) {
        h->decoder->thread_count = 4;
        h->decoder->thread_type = FF_THREAD_SLICE;
        h->decoder->pkt_timebase = (AVRational){1, 1000000};
        ok = avcodec_open2(h->decoder, codec, NULL) >= 0;
    }
    KEvent* event = ok ? kevent_new("NativeVideoSubmissionReady") : NULL;
    if (!event) {
        video_host_free(h);
        return RC(1, 103);
    }
    s->event = kobj_id(&event->o);
    s->width = cfg->width;
    s->height = cfg->height;
    s->capacity = cfg->input_capacity;
    kevent_signal(event);
    LOG("video: native VP9 decoder configured for %ux%u at %u FPS\n", s->width, s->height, cfg->frame_rate);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_GetSubmissionEvent(SvcObj* self, u32* event) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->event, RC(1, 125));
    *event = h_new(&kev(s->event)->o);
    return *event ? 0 : RC(1, 105);
}
static void video_fence(NativeVideoCodec* s, NativeVideoImage* image, nn_gsv_VideoFence* fence) {
    *fence = (nn_gsv_VideoFence){.encoded_id = ~s->syncpoint, .value = image->fence};
}
static int nn_gsv_INativeVideoCodec_RegisterImage(SvcObj* self, u32 id, const u32* reserved, nn_gsv_VideoFence* fence) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->width && !s->started, RC(1, 125));
    for (u32 i = 0; i < 3; ++i) R_UNLESS(!reserved[i], RC(1, 33));
    NativeVideoPool* p = video_pool(id);
    R_UNLESS(p && !p->retired, RC(1, 121));
    R_UNLESS((p->extent & 0xffff) + 1 >= s->width && (p->extent >> 16) + 1 >= s->height, RC(1, 101));
    NativeVideoImage* slot = NULL;
    for (u32 i = 0; i < NVIDEO_IMAGES; ++i) {
        R_UNLESS(s->images[i].id != id, RC(1, 125));
        if (!s->images[i].id && !slot) slot = &s->images[i];
    }
    R_UNLESS(slot, RC(1, 103));
    if (!s->syncpoint) {
        s->syncpoint = syncpt_alloc();
        s->issued = s->next_fence = syncpt_read(s->syncpoint);
    }
    *slot = (NativeVideoImage){id, ++s->next_fence, 1};
    video_fence(s, slot, fence);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_ReleaseImage(SvcObj* self, u32 id, u32 reserved, const u8* release,
                                                 nn_gsv_VideoFence* fence) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->width && !reserved, RC(1, 125));
    for (u32 i = 0; i < 24; ++i) R_UNLESS(!release[i], RC(1, 33));
    for (u32 i = 0; i < NVIDEO_IMAGES; ++i) {
        NativeVideoImage* image = &s->images[i];
        if (image->id != id) continue;
        if (!image->available) {
            image->available = 1;
            image->fence = ++s->next_fence;
        }
        video_fence(s, image, fence);
        return video_pump(s);
    }
    return RC(1, 121);
}
static int nn_gsv_INativeVideoCodec_UnregisterImage(SvcObj* self, u32 id, const u32* reserved) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(!s->started, RC(1, 125));
    for (u32 i = 0; i < 7; ++i) R_UNLESS(!reserved[i], RC(1, 33));
    for (u32 i = 0; i < NVIDEO_IMAGES; ++i)
        if (s->images[i].id == id) {
            s->images[i] = (NativeVideoImage){0};
            video_collect_pools();
            R_SUCCEED();
        }
    return RC(1, 121);
}
static int nn_gsv_INativeVideoCodec_Start(SvcObj* self) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->width && s->syncpoint, RC(1, 125));
    s->started = 1;
    video_submission_event(s);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_Submit(SvcObj* self, IpcBuf input, const u8* timestamp, u8 eos, u64* consumed) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->started && !s->eos, RC(1, 125));
    R_UNLESS(eos <= 1 && (input.size == 0) == (eos != 0) && input.size <= s->capacity && input.size <= INT_MAX,
             RC(1, 101));
    KTmem* tmem = (KTmem*)kobj_at(s->tmem);
    if (s->input_count == NVIDEO_INPUTS || input.size > tmem->size - s->input_bytes) {
        ev_clear(s->event);
        return VIDEO_FULL;
    }
    R_UNLESS(!input.size || video_buffer_mapped(input), RC(1, 102));
    NativeVideoHostCodec* h = video_host(s);
    AVPacket* pkt = h->packets[(s->input_head + s->input_count) % NVIDEO_INPUTS];
    R_UNLESS(av_new_packet(pkt, (int)input.size) >= 0, RC(1, 103));
    if (input.size) memcpy(pkt->data, GP(input.addr), (size_t)input.size);
    pkt->pts = (i64)snx_ld_le64(timestamp);
    s->input_bytes += input.size;
    ++s->input_count;
    if (eos) s->eos = 1;
    R_TRY(video_pump(s));
    *consumed = input.size;
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_GetOutputProperties(SvcObj* self, u32* properties) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->width, RC(1, 125));
    R_TRY(video_pump(s));
    R_UNLESS(s->output_count, VIDEO_EMPTY);
    memcpy(properties, s->output[s->output_head], sizeof s->output[0]);
    s->output_head = (s->output_head + 1) % NVIDEO_IMAGES;
    --s->output_count;
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_Flush(SvcObj* self) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->started && s->width, RC(1, 125));
    /* A pause flush does not end a VP9 stream. Its following inter frames still need the reference pictures. */
    return video_pump(s);
}
static int nn_gsv_INativeVideoCodec_Stop(SvcObj* self) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->width, RC(1, 125));
    if (s->syncpoint && g_renderer_up && g_renderer->release_retire) g_renderer->release_retire(s->syncpoint);
    NativeVideoHostCodec* h = video_host(s);
    avcodec_flush_buffers(h->decoder);
    av_frame_unref(h->frame);
    for (u32 i = 0; i < NVIDEO_INPUTS; ++i) av_packet_unref(h->packets[i]);
    s->started = s->input_count = s->input_head = s->output_count = s->output_head = s->eos = 0;
    s->input_bytes = 0;
    ev_clear(s->event);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_Finalize(SvcObj* self) {
    video_reset(self->data);
    R_SUCCEED();
}
static int video_codec_dispatch(SvcObj* self, IpcCtx* c) {
    R_UNLESS(c->tipc, RC(1, 33));
    pm_lock(&g_host.native_video.lock);
    int rc = nn_gsv_INativeVideoCodec_dispatch(self, c);
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
static void video_codec_destroy(SvcObj* self) {
    pm_lock(&g_host.native_video.lock);
    video_reset(self->data);
    pm_unlock(&g_host.native_video.lock);
}
static int nn_gsv_INativeVideoContext_CreateCodec(SvcObj* self, u32* codec) {
    NativeVideoContext* context = self->data;
    SvcObj* obj = SVCOBJ_NEW_DATA("nn::gsv::INativeVideoCodec", video_codec_dispatch, g_hle.native_video.codecs,
                                  g_hle.native_video.codec_next, "native video codecs");
    ((NativeVideoCodec*)obj->data)->encoder = context->encoder;
    obj->destroy = video_codec_destroy;
    *codec = session_new(obj);
    return *codec ? 0 : RC(1, 103);
}
static int video_context_dispatch(SvcObj* self, IpcCtx* c) {
    R_UNLESS(c->tipc, RC(1, 33));
    c->tipc_error_moves = c->cmd == 1;
    pm_lock(&g_host.native_video.lock);
    int rc = nn_gsv_INativeVideoContext_dispatch(self, c);
    pm_unlock(&g_host.native_video.lock);
    return rc;
}
SvcObj* native_video_context_new(int encoder) {
    pm_lock(&g_host.native_video.lock);
    if (!(encoder ? g_hle.native_video.encoder_ready : g_hle.native_video.decoder_ready)) {
        pm_unlock(&g_host.native_video.lock);
        return NULL;
    }
    SvcObj* obj = SVCOBJ_NEW_DATA("nn::gsv::INativeVideoContext", video_context_dispatch, g_hle.native_video.contexts,
                                  g_hle.native_video.context_next, "native video contexts");
    ((NativeVideoContext*)obj->data)->encoder = encoder != 0;
    pm_unlock(&g_host.native_video.lock);
    return obj;
}
int native_video_busy(const HleNativeVideo* state) {
    for (u32 i = 0; i < NVIDEO_CODECS; ++i) {
        NativeVideoCodec idle = {.obj = state->codecs[i].obj, .encoder = state->codecs[i].encoder};
        if (memcmp(&idle, &state->codecs[i], sizeof idle)) return 1;
    }
    for (u32 i = 0; i < NVIDEO_POOLS; ++i)
        if (state->pools[i].first) return 1;
    for (u32 i = 0; i < NVIDEO_MAPS; ++i)
        if (state->maps[i].image) return 1;
    return 0;
}
void native_video_state_drop(void) {
    for (u32 i = 0; i < NVIDEO_CODECS; ++i) video_host_free(&g_host.native_video.codecs[i]);
}
STATE_FN_TABLE(g_native_video_state_fns, STATE_FN("native_video", video_context_dispatch),
               STATE_FN("native_video", video_codec_dispatch), STATE_FN("native_video", video_codec_destroy));
