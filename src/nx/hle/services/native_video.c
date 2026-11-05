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
#include <libavcodec/avcodec.h>
#include <limits.h>

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
/* the picture written into its image on the GPU, as NVDEC's pictures are (the renderer's nvdec_write, pitch linear):
   FFmpeg's planar rows are staged, the GPU interleaves the chroma, and the image is published over guest memory, which
   the game's CPU reads (ReadDecoderOutputImageNv12) land. 1 when written */
static int image_write(const NativeVideoPool* p, u32 id, const AVFrame* f) {
    const NativeVideoLayout* l = &p->layout;
    u32 w = (u32)f->width, h = (u32)f->height;
    u64 cpu = p->cpu + (u64)(id - p->first) * l->image_bytes;
    NvdecPic pic = {.w = w,
                    .h = h,
                    .luma = cpu,
                    .chroma = cpu + l->chroma_offset,
                    .ext_y = (u64)l->pitch * l->rows,
                    .ext_c = (u64)l->pitch * l->chroma_rows,
                    .pitch_y = l->pitch,
                    .pitch_c = l->pitch,
                    .surf_h = l->rows,
                    .pitch_linear = 1};
    if ((f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) || !nvideo_kind_nv12(l->kind) ||
        w > (l->extent & 0xffff) + 1u || h > (l->extent >> 16) + 1u) {
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
    return g_renderer->nvdec_write(&pic);
}
/* the image filled with the frame (NULL: the end of the stream, no picture), its output queued and its fence reached */
static void codec_output(NativeVideoCodec* s, NativeVideoImage* image, const AVFrame* f, u64 timestamp) {
    NativeVideoPool* p = nvideo_pool_find(NV->pools, NVIDEO_POOLS, image->id);
    u32 w = f ? (u32)f->width : 0, h = f ? (u32)f->height : 0;
    if (image->wait_id < SYNCPT_IDS && !syncpt_wait(image->wait_id, image->wait_value, 200000000))
        WARN_LIMIT(4, "gsv: video image %u: the game's fence %u on syncpoint %u isn't reached, written anyway\n",
                   image->id, image->wait_value, image->wait_id);
    if (f && p)
        image_write(p, image->id, f);
    else if (f)
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
/* the decoder fed and its pictures written while images are queued */
static void codec_pump(NativeVideoCodec* s) {
    AVCodecContext* dec = HOST(s)->decoder;
    AVFrame* frame = HOST(s)->frame;
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
    R_UNLESS(m, RC(1, 114)); /* InvalidHandle */
    if (size > m->size) {
        kobj_unref(&m->o);
        R_THROW(NVIDEO_BAD_ARGUMENT);
    }
    return codec_initialize(s, kobj_id(&m->o), size, codec, input_format, output_format);
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
    enum AVCodecID id = kind == NVIDEO_MIME_VP9 ? AV_CODEC_ID_VP9
                        : kind == NVIDEO_MIME_VP8 ? AV_CODEC_ID_VP8
                        : kind == NVIDEO_MIME_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_NONE;
    if (id == AV_CODEC_ID_NONE) {
        char name[65] = {0};
        memcpy(name, GP(mime.addr), (size_t)mime.size);
        WARN("gsv: video codec %u: no decoder for \"%s\"\n", codec_index(s), name);
        R_THROW(NVIDEO_BAD_ARGUMENT);
    }
    R_UNLESS(cfg->width && cfg->height && cfg->width <= 8192 && cfg->height <= 8192, NVIDEO_BAD_ARGUMENT);
    const AVCodec* codec = avcodec_find_decoder(id);
    AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : NULL;
    int ok = dec != NULL;
    if (ok) {
        dec->thread_count = 4;
        dec->thread_type = FF_THREAD_SLICE; /* no frame threads: each packet's picture comes out at once */
        dec->pkt_timebase = (AVRational){1, 1000000};
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
        codec->name, s->width, s->height, s->frame_rate, s->capacity);
    R_SUCCEED();
}
static int nn_gsv_INativeVideoCodec_GetSubmissionEvent(SvcObj* self, u32* event) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->event, NVIDEO_BAD_STATE);
    *event = ev_handle(s->event);
    R_UNLESS(*event, RC(1, 105)); /* OutOfHandles */
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
    return nvideo_image_unregister(self->data, image);
}
static int nn_gsv_INativeVideoCodec_Start(SvcObj* self) {
    NativeVideoCodec* s = self->data;
    R_UNLESS(s->state == NVIDEO_CONFIGURED, NVIDEO_BAD_STATE);
    s->started = 1;
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
    avcodec_flush_buffers(HOST(s)->decoder);
    av_frame_unref(HOST(s)->frame);
    while (s->input_count) {
        av_packet_unref(HOST(s)->packets[s->input_head]);
        nvideo_input_pop(s);
    }
    s->started = 0;
    s->eos = 0;
    submission_event(s);
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
        WARN_LIMIT(4, "gsv: video codec %u: output properties asked before an output (%s image for the decoder, %u inputs)\n",
                   codec_index(s), nvideo_image_next(s) ? "an" : "no", s->input_count);
    return rc;
}
static int nn_gsv_INativeVideoCodec_Finalize(SvcObj* self) {
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
    R_UNLESS(*codec, RC(1, 105)); /* OutOfHandles */
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
    R_UNLESS(*session, RC(1, 105)); /* OutOfHandles */
    R_SUCCEED();
}

/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_native_video_state_fns, STATE_FN("native_video", nvideo_context_dispatch),
               STATE_FN("native_video", nvideo_codec_dispatch), STATE_FN("native_video", nvideo_codec_destroy));
