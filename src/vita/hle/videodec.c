/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceVideodecUser: sceVideodec and sceAvcdec, H.264 decoded with FFmpeg's libavcodec, each picture laid out in the
   game's buffer on the GPU (vita_render_picture). The decoder takes Annex B access units with their timestamps and
   gives pictures in display order with the timestamp each came with, one a call at most: as the console's, it holds
   some back (B frames), which sceAvcdecDecodeStop hands out at the end. As the vitasdk headers (MIT) declare and the
   SDK's SceAvPlayer uses them; Vita3K's (GPL-2.0-or-later) checks of the sizes */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "vita/gpu/render.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_VIDEODEC_TYPE_HW_AVCDEC 0x1001u
#define SCE_AVCDEC_ERROR_INVALID_PARAM   0x80620002u
#define SCE_AVCDEC_ERROR_OUT_OF_MEMORY   0x80620003u
#define SCE_AVCDEC_ERROR_INVALID_STATE   0x80620004u
#define SCE_AVCDEC_ERROR_INVALID_POINTER 0x80620009u
#define DECODERS 4u
#define HELD     8u /* pictures FFmpeg gave that weren't handed out yet */

typedef struct {
    bool used, draining;
    uint32_t width, height;
    AVCodecContext* codec;
    AVPacket* packet;
    AVFrame* held[HELD]; /* in display order, the first next */
    unsigned held_count;
} Decoder;
struct VitaVideodec {
    Decoder decoders[DECODERS];
};

static Decoder* decoder_of(VitaProcess* p, uint32_t ctrl) {
    uint32_t handle = ctrl ? vita_read32(p, ctrl) : 0;
    return p->videodec && handle >= 1 && handle <= DECODERS && p->videodec->decoders[handle - 1].used
               ? &p->videodec->decoders[handle - 1]
               : NULL;
}
static void drop_held(Decoder* d) {
    for (unsigned i = 0; i < d->held_count; ++i) av_frame_free(&d->held[i]);
    d->held_count = 0;
}
static void release(Decoder* d) {
    drop_held(d);
    avcodec_free_context(&d->codec);
    av_packet_free(&d->packet);
    memset(d, 0, sizeof *d);
}
/* the pictures FFmpeg has ready, held: false when there was no room for one (it stays in FFmpeg) */
static bool take_ready(Decoder* d) {
    while (d->held_count < HELD) {
        AVFrame* f = av_frame_alloc();
        if (!f) return false;
        if (avcodec_receive_frame(d->codec, f) < 0) {
            av_frame_free(&f);
            return true;
        }
        d->held[d->held_count++] = f;
    }
    return false;
}
void vita_videodec_free(VitaProcess* p) {
    if (!p->videodec) return;
    for (unsigned i = 0; i < DECODERS; ++i)
        if (p->videodec->decoders[i].used) release(&p->videodec->decoders[i]);
    free(p->videodec);
    p->videodec = NULL;
}
static bool size_valid(VitaProcess* p, uint32_t query) {
    uint32_t w = vita_read32(p, query), h = vita_read32(p, query + 4), refs = vita_read32(p, query + 8);
    return w && h && w <= 1920 && h <= 1088 && w * h <= 1280 * 720 && refs <= 16;
}
static uint32_t frame_bytes(uint32_t w, uint32_t h) { return w * h * 3 / 2; }

uint32_t vita_videodec_ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
/* sceVideodecQueryMemSize(type, query, u32* size): the library's own memory, none here but some for it to carve */
uint32_t vita_videodec_query_mem_size(VitaProcess* p, VitaThread* t, uint32_t type, uint32_t query, uint32_t size) {
    (void)t, (void)query;
    if (type != SCE_VIDEODEC_TYPE_HW_AVCDEC) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    if (!size) return SCE_AVCDEC_ERROR_INVALID_POINTER;
    vita_write32(p, size, 0x40000);
    return 0;
}
/* sceAvcdecQueryDecoderMemSize(type, {width, height, references}, u32* frameMemSize): the references' frames, in
   256 KiB units */
uint32_t vita_videodec_query_decoder_mem_size(VitaProcess* p, VitaThread* t, uint32_t type, uint32_t query,
                                              uint32_t out, uint32_t references, uint32_t frame_mem_size) {
    (void)t, (void)references, (void)frame_mem_size;
    if (type != SCE_VIDEODEC_TYPE_HW_AVCDEC) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    if (!query || !out) return SCE_AVCDEC_ERROR_INVALID_POINTER;
    if (!size_valid(p, query)) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    uint32_t refs = vita_read32(p, query + 8) ? vita_read32(p, query + 8) : 1;
    uint32_t size = frame_bytes(vita_read32(p, query), vita_read32(p, query + 4)) * refs;
    vita_write32(p, out, (size + 0x3ffffu) & ~0x3ffffu);
    return 0;
}
/* sceAvcdecCreateDecoder(type, SceAvcdecCtrl* ctrl, query): the handle into ctrl */
uint32_t vita_videodec_create_decoder(VitaProcess* p, VitaThread* t, uint32_t type, uint32_t ctrl, uint32_t query) {
    (void)t;
    if (type != SCE_VIDEODEC_TYPE_HW_AVCDEC) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    if (!ctrl || !query) return SCE_AVCDEC_ERROR_INVALID_POINTER;
    if (!size_valid(p, query)) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    /* the Vita's pictures will decode with Vulkan Video too: until then the setting stops the game */
    if (g_settings.vulkan_video_decode) {
        printf("vita: Vulkan Video decode (the vulkan_video setting) is not implemented for the Vita yet\n");
        p->exited = true, p->exit_code = -1;
        return SCE_AVCDEC_ERROR_INVALID_PARAM;
    }
    if (!p->videodec && !(p->videodec = calloc(1, sizeof *p->videodec))) return SCE_AVCDEC_ERROR_OUT_OF_MEMORY;
    Decoder* d = NULL;
    for (unsigned i = 0; i < DECODERS && !d; ++i)
        if (!p->videodec->decoders[i].used) d = &p->videodec->decoders[i];
    const AVCodec* h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!d || !h264) {
        if (!h264) printf("vita: FFmpeg has no H.264 decoder, so the movies stay black\n");
        return SCE_AVCDEC_ERROR_OUT_OF_MEMORY;
    }
    d->used = true;
    d->width = vita_read32(p, query), d->height = vita_read32(p, query + 4);
    d->codec = avcodec_alloc_context3(h264);
    d->packet = av_packet_alloc();
    if (!d->codec || !d->packet || avcodec_open2(d->codec, h264, NULL) < 0) {
        release(d);
        return SCE_AVCDEC_ERROR_OUT_OF_MEMORY;
    }
    vita_write32(p, ctrl, (uint32_t)(d - p->videodec->decoders) + 1);
    return 0;
}
uint32_t vita_videodec_delete_decoder(VitaProcess* p, VitaThread* t, uint32_t decoder_id) {
    (void)t;
    Decoder* d = decoder_of(p, decoder_id);
    if (!d) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    release(d);
    return 0;
}
/* the largest access unit the decoder takes */
uint32_t vita_videodec_decode_available_size(VitaProcess* p, VitaThread* t, uint32_t decoder_id) {
    (void)t;
    Decoder* d = decoder_of(p, decoder_id);
    return d ? frame_bytes(d->width, d->height) : SCE_AVCDEC_ERROR_INVALID_PARAM;
}
/* A picture FFmpeg gave into the array's first SceAvcdecPicture (0x64 bytes: its frame at 4, the frame's pixel type,
   pitch, size, then its pPicture[0] at 0x3c; its info's pts at 0x5c): its picture laid out there, its pts the
   unit's. The array's numOfOutput says whether there was one */
static void output(VitaProcess* p, Decoder* d, uint32_t array) {
    uint32_t list = vita_read32(p, array + 8), picture = list ? vita_read32(p, list) : 0;
    vita_write32(p, array, 0);
    if (!d->held_count || !picture || vita_read32(p, array + 4) < 1) return;
    uint32_t pixel_type = vita_read32(p, picture + 4), pitch = vita_read32(p, picture + 8);
    uint32_t width = vita_read32(p, picture + 0xc), height = vita_read32(p, picture + 0x10);
    uint32_t buffer = vita_read32(p, picture + 0x3c);
    AVFrame* f = d->held[0];
    uint32_t w = (uint32_t)f->width < width ? (uint32_t)f->width : width;
    uint32_t h = (uint32_t)f->height < height ? (uint32_t)f->height : height;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
        printf("vita: a decoded picture of format %d isn't laid out (8-bit 4:2:0 only)\n", f->format);
    else if (!vita_render_picture(p, buffer, pitch, pixel_type, w & ~1u, h & ~1u, (const uint8_t* const*)f->data,
                                  f->linesize))
        printf("vita: a decoded picture (pixel type 0x%" PRIx32 ") isn't laid out\n", pixel_type);
    vita_write32(p, picture + 0x14, (uint32_t)f->width), vita_write32(p, picture + 0x18, (uint32_t)f->height);
    uint64_t pts = (uint64_t)f->pts;
    vita_write32(p, picture + 0x5c, (uint32_t)(pts >> 32)), vita_write32(p, picture + 0x60, (uint32_t)pts);
    vita_write32(p, array, 1);
    av_frame_free(&d->held[0]);
    memmove(d->held, d->held + 1, --d->held_count * sizeof *d->held);
}
/* sceAvcdecDecode(ctrl, SceAvcdecAu* au: pts, dts {upper, lower}, es {pBuf, size}, array) */
uint32_t vita_videodec_decode(VitaProcess* p, VitaThread* t, uint32_t ctrl, uint32_t au, uint32_t array) {
    (void)t;
    Decoder* d = decoder_of(p, ctrl);
    if (!d) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    if (!au || !array) return SCE_AVCDEC_ERROR_INVALID_POINTER;
    vita_write32(p, array, 0);
    uint32_t data = vita_read32(p, au + 16), size = vita_read32(p, au + 20);
    if (!data || !size || !vita_mapped(p, data, size)) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    if (d->draining) { /* decoding again after a stop: from a clean state */
        avcodec_flush_buffers(d->codec);
        drop_held(d);
        d->draining = false;
    }
    if (av_new_packet(d->packet, (int)size) < 0) return SCE_AVCDEC_ERROR_OUT_OF_MEMORY;
    memcpy(d->packet->data, vita_ptr(p, data), size);
    d->packet->pts = (int64_t)((uint64_t)vita_read32(p, au) << 32 | vita_read32(p, au + 4));
    d->packet->dts = (int64_t)((uint64_t)vita_read32(p, au + 8) << 32 | vita_read32(p, au + 12));
    /* the unit in, the pictures FFmpeg has ready taken out first when it is full */
    int sent = avcodec_send_packet(d->codec, d->packet);
    while (sent == AVERROR(EAGAIN) && take_ready(d)) sent = avcodec_send_packet(d->codec, d->packet);
    av_packet_unref(d->packet);
    if (sent < 0) printf("vita: the H.264 decoder refused a unit (%d)\n", sent);
    take_ready(d);
    output(p, d, array);
    return sent == AVERROR(EAGAIN) ? 0x8062000au /* SCE_AVCDEC_ERROR_ES_BUFFER_FULL */ : 0;
}
/* sceAvcdecDecodeStop(ctrl, array): a picture the decoder held back, each call, until none is left (numOfOutput 0) */
uint32_t vita_videodec_decode_stop(VitaProcess* p, VitaThread* t, uint32_t ctrl, uint32_t array) {
    (void)t;
    Decoder* d = decoder_of(p, ctrl);
    if (!d) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    if (!array) return SCE_AVCDEC_ERROR_INVALID_POINTER;
    vita_write32(p, array, 0);
    if (!d->draining) {
        avcodec_send_packet(d->codec, NULL);
        d->draining = true;
    }
    take_ready(d);
    output(p, d, array);
    return 0;
}
/* sceAvcdecDecodeFlush(ctrl): what it holds dropped (a seek) */
uint32_t vita_videodec_decode_flush(VitaProcess* p, VitaThread* t, uint32_t ctrl) {
    (void)t;
    Decoder* d = decoder_of(p, ctrl);
    if (!d) return SCE_AVCDEC_ERROR_INVALID_PARAM;
    avcodec_flush_buffers(d->codec);
    drop_held(d);
    d->draining = false;
    return 0;
}
