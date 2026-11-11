/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A native video codec's decoder (native_video.h): FFmpeg's software decoder for VP9, VP8 and H.264, and for AV1 one
   of three ways. With Vulkan Video on the renderer's GPU, where the renderer gave a device for it (the vulkan_video
   setting, on a GPU with an AV1 decode queue); else on the CPU with dav1d; else, in a build without dav1d, as a black
   picture for each input, with the input's timestamp, so that the game plays the movie through and goes on.

   FFmpeg's own AV1 decoder only parses the stream for the GPU's decoder. It refuses what that one doesn't take (a
   profile or a size the driver has not, a monochrome stream, pictures that are not 8 bit 4:2:0, the only ones the
   renderer writes out of a GPU image) at the stream's first picture or at a later sequence header, and the packet it
   refused is gone by then. So the packets the Vulkan decoder took since its last picture are kept (replay), and go to
   the CPU's decoder, or the stand-in, when it takes the stream over: for the rest of the stream, as a decoder can't
   start in the middle of one */
#include "nx/hle/services/native_video.h"
#include "nx/runtime/platform/log.h"
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <stdio.h>
#include <string.h>

#define NVIDEO_AV1_PARSER "av1"      /* FFmpeg's own AV1 decoder, which decodes through a hardware one alone */
#define NVIDEO_AV1_CPU    "libdav1d" /* dav1d's, in a build that has it (cmake/dav1d.cmake) */

/* the Vulkan decoder's pictures in a GPU image when the stream's are 8 bit 4:2:0, which is what the renderer copies out
   of one (vk/nvdec.c); else FFmpeg's software format, the list's last, with which its AV1 decoder refuses the stream */
static enum AVPixelFormat av1_format(AVCodecContext* c, const enum AVPixelFormat* list) {
    const NativeVideoDecoder* d = c->opaque;
    const enum AVPixelFormat* last = list;
    int vulkan = 0;
    for (const enum AVPixelFormat* f = list; *f != AV_PIX_FMT_NONE; f++) {
        if (*f == AV_PIX_FMT_VULKAN) vulkan = 1;
        last = f;
    }
    if (vulkan && c->sw_pix_fmt == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_VULKAN;
    if (vulkan)
        LOG("gsv: video codec %u: the AV1 stream's pictures are %s, which the renderer doesn't write out of a GPU "
            "image (yuv420p only)\n",
            d->index, av_get_pix_fmt_name(c->sw_pix_fmt));
    return *last;
}
/* codec opened, with the renderer's device hw (taken over, NULL for none) and H.264's configuration: NULL when it
   couldn't be. Slice threads only: with frame threads a packet's picture would come out packets later */
static AVCodecContext* context_open(NativeVideoDecoder* d, const AVCodec* codec, AVBufferRef* hw, const u8* config,
                                    u64 config_size) {
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c) {
        av_buffer_unref(&hw);
        return NULL;
    }
    c->thread_count = 4;
    c->thread_type = FF_THREAD_SLICE;
    if (codec->id == AV_CODEC_ID_AV1) c->flags |= AV_CODEC_FLAG_LOW_DELAY; /* dav1d's frame delay 1, for the same */
    c->pkt_timebase = (AVRational){1, 1000000};
    if (hw) {
        c->hw_device_ctx = hw;
        c->opaque = d;
        c->get_format = av1_format;
    }
    if (config && (c->extradata = av_mallocz((size_t)config_size + AV_INPUT_BUFFER_PADDING_SIZE))) {
        memcpy(c->extradata, config, (size_t)config_size);
        c->extradata_size = (int)config_size;
    }
    if (avcodec_open2(c, codec, NULL) < 0) avcodec_free_context(&c);
    return c;
}
/* the stand-in's picture: limited range black, of the configured size */
static int black_open(NativeVideoDecoder* d) {
    AVFrame* f = d->black = av_frame_alloc();
    if (!f) return 0;
    f->format = AV_PIX_FMT_YUV420P;
    f->width = (int)d->width;
    f->height = (int)d->height;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&d->black);
        return 0;
    }
    for (int i = 0; i < 3; i++)
        memset(f->data[i], i ? 128 : 16, (size_t)f->linesize[i] * (size_t)(i ? (f->height + 1) / 2 : f->height));
    return 1;
}
/* AV1 without Vulkan Video, why says why not: dav1d, or the stand-in in a build without it. 0 when neither opened */
static int av1_cpu_open(NativeVideoDecoder* d, const char* why) {
    const AVCodec* codec = avcodec_find_decoder_by_name(NVIDEO_AV1_CPU);
    if (codec) {
        if (!(d->ctx = context_open(d, codec, NULL, NULL, 0))) return 0;
        d->way = NVIDEO_DECODER_CPU;
        LOG("gsv: video codec %u: AV1 decodes on the CPU (%s): %s\n", d->index, codec->name, why);
        return 1;
    }
    if (!black_open(d)) return 0;
    d->way = NVIDEO_DECODER_BLACK;
    WARN("gsv: video codec %u: AV1 pictures come out black: %s, and this build has no CPU decoder for AV1 (%s)\n",
         d->index, why, NVIDEO_AV1_CPU);
    return 1;
}
int nvideo_decoder_open(NativeVideoDecoder* d, u32 index, u32 mime, u32 width, u32 height, const u8* config,
                        u64 config_size, void* hw_device, const char* no_hw) {
    AVBufferRef* hw = hw_device;
    *d = (NativeVideoDecoder){.index = index, .width = width, .height = height};
    if (mime != NVIDEO_MIME_AV1) {
        const AVCodec* codec = avcodec_find_decoder(mime == NVIDEO_MIME_VP9    ? AV_CODEC_ID_VP9
                                                    : mime == NVIDEO_MIME_VP8  ? AV_CODEC_ID_VP8
                                                    : mime == NVIDEO_MIME_H264 ? AV_CODEC_ID_H264
                                                                               : AV_CODEC_ID_NONE);
        av_buffer_unref(&hw);
        if (!codec || !(d->ctx = context_open(d, codec, NULL, config, config_size))) return 0;
        d->way = NVIDEO_DECODER_CPU;
        return 1;
    }
    if (hw) {
        const AVCodec* codec = avcodec_find_decoder_by_name(NVIDEO_AV1_PARSER);
        if (!codec) av_buffer_unref(&hw);
        if (codec && (d->ctx = context_open(d, codec, hw, NULL, 0))) {
            d->way = NVIDEO_DECODER_VULKAN;
            LOG("gsv: video codec %u: AV1 decodes with Vulkan Video (experimental)\n", d->index);
            return 1;
        }
        no_hw = "FFmpeg's AV1 decoder for Vulkan Video could not be opened";
    }
    return av1_cpu_open(d, no_hw);
}
static void replay_clear(NativeVideoDecoder* d) {
    for (u32 i = 0; i < d->nreplay; i++) av_packet_free(&d->replay[i]);
    d->nreplay = d->replayed = 0;
}
void nvideo_decoder_close(NativeVideoDecoder* d) {
    avcodec_free_context(&d->ctx);
    replay_clear(d);
    av_frame_free(&d->black);
    *d = (NativeVideoDecoder){0};
}
void nvideo_decoder_flush(NativeVideoDecoder* d) {
    if (d->ctx) avcodec_flush_buffers(d->ctx);
    replay_clear(d);
    d->pictures = d->black_due = d->black_end = 0;
}
const char* nvideo_decoder_name(const NativeVideoDecoder* d) {
    return d->way == NVIDEO_DECODER_VULKAN  ? "av1 (Vulkan Video)"
           : d->way == NVIDEO_DECODER_BLACK ? "av1 (black stand-in)"
           : d->ctx                         ? d->ctx->codec->name
                                            : "none";
}
/* the Vulkan decoder's error r hands the stream over: its refusal (ENOSYS) anywhere, and any failure before the
   stream's first picture, where the CPU's decoder loses nothing by starting */
static int av1_refused(const NativeVideoDecoder* d, int r) { return r == AVERROR(ENOSYS) || !d->pictures; }
/* the stream from the Vulkan decoder to the CPU's, or the stand-in, which the packets kept go to first. 0 when
   neither opened: the codec then decodes nothing more */
static int av1_take_over(NativeVideoDecoder* d, const char* why) {
    char line[160];
    snprintf(line, sizeof line, "Vulkan Video gave the stream up after %u pictures (%s), packets kept for it: %u",
             d->pictures, why, d->nreplay);
    avcodec_free_context(&d->ctx);
    d->way = NVIDEO_DECODER_NONE;
    d->replayed = 0;
    if (av1_cpu_open(d, line)) return 1;
    WARN("gsv: video codec %u: %s, but no decoder for the CPU could be opened: the movie stops\n", d->index, line);
    replay_clear(d);
    return 0;
}
static int av1_take_over_error(NativeVideoDecoder* d, int r) {
    char msg[AV_ERROR_MAX_STRING_SIZE] = "";
    av_strerror(r, msg, sizeof msg);
    return av1_take_over(d, msg);
}
/* the decoder in use, the stand-in among them: avcodec_send_packet's and avcodec_receive_frame's results */
static int way_send(NativeVideoDecoder* d, const AVPacket* pkt) {
    if (d->ctx) return avcodec_send_packet(d->ctx, pkt);
    if (d->way != NVIDEO_DECODER_BLACK) return AVERROR(EINVAL);
    if (d->black_end) return AVERROR_EOF;
    if (d->black_due) return AVERROR(EAGAIN);
    if (pkt) {
        d->black_pts = pkt->pts;
        d->black_due = 1;
    } else
        d->black_end = 1;
    return 0;
}
static int way_receive(NativeVideoDecoder* d, AVFrame* frame) {
    int r;
    if (d->ctx) return avcodec_receive_frame(d->ctx, frame);
    if (d->way != NVIDEO_DECODER_BLACK) return AVERROR(EINVAL);
    if (!d->black_due) return d->black_end ? AVERROR_EOF : AVERROR(EAGAIN);
    if ((r = av_frame_ref(frame, d->black)) < 0) return r;
    frame->pts = d->black_pts;
    d->black_due = 0;
    return 0;
}
int nvideo_decoder_send(NativeVideoDecoder* d, const AVPacket* pkt) {
    AVPacket* keep;
    int r;
    if (d->way != NVIDEO_DECODER_VULKAN) return d->replayed < d->nreplay ? AVERROR(EAGAIN) : way_send(d, pkt);
    if (!pkt) return way_send(d, NULL);
    if (d->nreplay == NVIDEO_REPLAY) return AVERROR(EAGAIN); /* nvideo_decoder_receive hands the stream over first */
    if (!(keep = av_packet_clone(pkt))) return AVERROR(ENOMEM);
    r = way_send(d, pkt);
    if (r == AVERROR(EAGAIN)) {
        av_packet_free(&keep);
        return r;
    }
    d->replay[d->nreplay++] = keep;
    if (r >= 0 || r == AVERROR_EOF || !av1_refused(d, r)) return r;
    return av1_take_over_error(d, r) ? 0 : r; /* taken: it is among the packets kept */
}
int nvideo_decoder_receive(NativeVideoDecoder* d, AVFrame* frame) {
    for (;;) {
        int r = way_receive(d, frame);
        if (d->way == NVIDEO_DECODER_VULKAN) {
            if (!r) {
                replay_clear(d);
                d->pictures++;
                return 0;
            }
            if (r == AVERROR_EOF) return r;
            if (r == AVERROR(EAGAIN)) { /* a stream that shows nothing for that long: none to keep packets for */
                if (d->nreplay < NVIDEO_REPLAY) return r;
                if (!av1_take_over(d, "no picture in the packets kept, which are all it keeps")) return r;
            } else if (!av1_refused(d, r) || !av1_take_over_error(d, r))
                return r;
            continue;
        }
        if (!r) d->pictures++;
        if (r != AVERROR(EAGAIN) || d->replayed == d->nreplay) return r;
        /* the next of the packets the Vulkan decoder took, which asked for one by that EAGAIN */
        r = way_send(d, d->replay[d->replayed++]);
        if (d->replayed == d->nreplay) replay_clear(d);
        if (r < 0) {
            char msg[AV_ERROR_MAX_STRING_SIZE] = "";
            av_strerror(r, msg, sizeof msg);
            WARN_LIMIT(8, "gsv: video codec %u: %s refused a packet the Vulkan decoder had taken (%s)\n", d->index,
                       nvideo_decoder_name(d), msg);
        }
    }
}
