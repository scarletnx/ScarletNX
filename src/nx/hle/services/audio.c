/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/audio/audio_renderer_manager.cpp, audio_renderer.cpp, audio_device.cpp,
   hardware_opus_decoder.cpp, hardware_opus_decoder_manager.cpp, errors.h; src/audio_core/opus/decoder_manager.cpp,
   opus/decoder.cpp, renderer/system.cpp, common/feature_support.h):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018-2024 yuzu Emulator Project (GPL-2.0-or-later)
   The hwopus work buffer sizes, from Ryujinx (src/Ryujinx.Horizon/Sdk/Codec/Detail/HardwareOpusDecoderManager.cs):
   SPDX-FileCopyrightText: Ryujinx Team and Contributors (MIT) */
/* The hardware Opus decoding (the native nn::codec's, nn/codec.c), audin:u and aud:u, with the audio clock that
   paces the native audio outs (nn/audio.c) and renderers (nn/audio_renderer.c, audren.c): every 5 ms it renders a
   frame of each started renderer, signals the renderer event, and plays and releases the audio outs' buffers */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/hle/services/services.h"
#include "nx/nn/audio.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/tracy.h"
#include <math.h>
#ifndef SNX_NO_H264
#include "nx/gpu/video/host1x.h"
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#endif

#include "hle/services/audio.swipc.inc"
#include "common/utilities/bits.h"

#define AUDIO_RATE      48000u  /* the DSP's */
#define AUDIO_FRAME_NS  5000000 /* the DSP's frame */

/* the objects' data (AudRen, HwOpus) and their pools: g_hle.audio (services_int.h) */

/* ============================================================ helpers */
/* whether [addr, addr + size) is guest memory with perm, block by block */
int snx_audio_guest_ok(u64 addr, u64 size, u32 perm) {
    u64 end = addr + size;
    MemBlock b;
    if (end < addr || end > AS_SIZE) return 0;
    while (addr < end) {
        mem_query(addr, &b);
        if ((b.perm & perm) != perm || b.addr + b.size <= addr) return 0;
        addr = b.addr + b.size;
    }
    return 1;
}

/* A part of nn::audio the runtime answers only natively: a game that asks service for what got here through an SDK
   function that is not native yet, as one that opens a native-only service did (sm.c), and stops the same way */
static _Noreturn void native_only(const char* service, const char* what) {
    WARN("\n*** The program asked %s for %s, which the runtime answers only natively ***\n", service, what);
    kernel_dump_threads();
    fatal("%s: the program asked for %s, which the runtime answers only in the SDK's own functions, nn::audio "
          "(nn/audio.c). A game got here through an SDK function that is not native yet (the calls of its thread, "
          "in the log's thread list before this, name it)",
          service, what);
}

/* ============================================================ the native audio manager (aud:u) */
/* The Ounce service that hands back the managers the older services serve. The audio in manager is the one still
   served here */
static int nn_audio_detail_INativeAudioManager_GetAudioInManager(SvcObj* self, SvcObj** manager) {
    (void)self;
    *manager = new_audin();
    R_SUCCEED();
}
/* the audio outs are answered in the SDK's own functions (nn/audio.c), with no manager to hand out */
static int nn_audio_detail_INativeAudioManager_GetAudioOutManager(SvcObj* self, SvcObj** manager) {
    (void)self, (void)manager;
    native_only("aud:u", "the audio out manager");
}
/* nor a renderer manager: the audio renderer is native too (nn/audio_renderer.c) */
static int nn_audio_detail_INativeAudioManager_GetAudioRendererManager(SvcObj* self, SvcObj** manager) {
    (void)self, (void)manager;
    native_only("aud:u", "the audio renderer manager");
}

SvcObj* new_aud(void) { return nn_audio_detail_INativeAudioManager_new(); }

/* ============================================================ audin: no microphone to open */
static int nn_audio_detail_IAudioInManager_OpenAudioIn(SvcObj* self, const nn_audio_AudioInParameter* params,
                                                       u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                       u32* sample_rate, u32* channel_count, u32* sample_format,
                                                       u32* state, SvcObj** in) {
    (void)self, (void)params, (void)aruid, (void)process, (void)name, (void)name_out;
    (void)sample_rate, (void)channel_count, (void)sample_format, (void)state, (void)in;
    R_THROW(SNX_RC_AUDIO_NOT_FOUND);
}
static int nn_audio_detail_IAudioInManager_OpenAudioInAuto(SvcObj* self, const nn_audio_AudioInParameter* params,
                                                           u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                           u32* sample_rate, u32* channel_count, u32* sample_format,
                                                           u32* state, SvcObj** in) {
    return nn_audio_detail_IAudioInManager_OpenAudioIn(self, params, aruid, process, name, name_out, sample_rate,
                                                       channel_count, sample_format, state, in);
}
static int nn_audio_detail_IAudioInManager_OpenAudioInProtocolSpecified(
    SvcObj* self, const nn_audio_AudioInProtocol* protocol, const nn_audio_AudioInParameter* params, u64 aruid,
    u32 process, IpcBuf name, IpcBuf name_out, u32* sample_rate, u32* channel_count, u32* sample_format, u32* state,
    SvcObj** in) {
    (void)protocol;
    return nn_audio_detail_IAudioInManager_OpenAudioIn(self, params, aruid, process, name, name_out, sample_rate,
                                                       channel_count, sample_format, state, in);
}
SvcObj* new_audin(void) { return nn_audio_detail_IAudioInManager_new(); }

/* ============================================================ the clock */
void audio_init(void) {
    g_hle.audio.ev = owned_event("IAudioRenderer");
    pc_init(&g_host.audio.hold_cv);
}

/* every 5 ms of host time: the renderers' frames, their event, the audio outs' buffers. Behind by more than a few
   frames (the host stalled), it starts over from now instead of rendering the backlog at once. The audio outs play in
   guest time, which a game pause stops: the clock itself stops at the top of its turn while one is in force */
void audio_clock(void* arg) {
    u64 next = plat_ns(), now;
    (void)arg;
    SNX_THREAD_NAME("audio");
    for (;;) {
        pm_lock(&g_host.audio.lk);
        if (g_hle.audio.hold) {
            g_hle.audio.held = 1;
            pc_broadcast(&g_host.audio.hold_cv);
            while (g_hle.audio.hold) pc_wait(&g_host.audio.hold_cv, &g_host.audio.lk, -1);
            g_hle.audio.held = 0;
            next = plat_ns();
        }
        pm_unlock(&g_host.audio.lk);
        SNX_ZONE(z_audio, "audio tick");
        audren_tick();
        ev_signal(g_hle.audio.ev);
        now = plat_ns();
        SNX_ZONE_VALUE(z_audio, now > next ? (u64)(now - next) / AUDIO_FRAME_NS : 0);
        nnaudio_out_tick(guest_ns());
        outstream_tick(guest_ns());
        SNX_ZONE_END(z_audio);
        next += AUDIO_FRAME_NS;
        if (now > next + 10 * (u64)AUDIO_FRAME_NS) next = now + AUDIO_FRAME_NS;
        if (next > now) plat_sleep_ns((i64)(next - now));
    }
}

void audio_hold(void) {
    pm_lock(&g_host.audio.lk);
    g_hle.audio.hold = 1;
    while (!g_hle.audio.held) pc_wait(&g_host.audio.hold_cv, &g_host.audio.lk, -1);
    pm_unlock(&g_host.audio.lk);
}
void audio_release(void) {
    pm_lock(&g_host.audio.lk);
    g_hle.audio.hold = 0;
    pc_broadcast(&g_host.audio.hold_cv);
    pm_unlock(&g_host.audio.lk);
}

/* ============================================================ hwopus */
/* a decoder: FFmpeg's, at 48 kHz, its output brought down to the decoder's rate through a low-pass filter keeping
   every step-th sample, and kept in a queue so that every reply is exactly the packet's duration (HwOpus,
   services_int.h) */
/* the queue starts with this much silence at 48 kHz: what FFmpeg's resampler may hold back of the first packets
   (libswresample's most), so that later replies come from the queue instead of silence put in front */
#define HWOPUS_LEAD 24

static int opus_rate_ok(u32 rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

/* the samples per channel at 48 kHz an Opus packet holds, from its TOC byte and frame count (RFC 6716 3.1),
   0 for a packet that isn't one or holds more than 120 ms */
static u32 opus_packet_48k(const u8* p, u32 len) {
    static const u16 silk[4] = {480, 960, 1920, 2880}, hybrid[2] = {480, 960}, celt[4] = {120, 240, 480, 960};
    u32 config, frame, frames;
    if (!len) return 0;
    config = p[0] >> 3;
    frame = config < 12 ? silk[config & 3] : config < 16 ? hybrid[config & 1] : celt[config & 3];
    switch (p[0] & 3) {
        case 0: frames = 1; break;
        case 1:
        case 2: frames = 2; break;
        default:
            if (len < 2) return 0;
            frames = p[1] & 0x3F;
    }
    frames *= frame;
    return frames > 5760 ? 0 : frames;
}

void hwopus_free(HwOpus* h) {
#ifndef SNX_NO_H264
    avcodec_free_context(&h->dec);
    av_packet_free(&h->pkt);
    av_frame_free(&h->frame);
#endif
    free(h->queue);
    h->queue = NULL;
    h->taps = h->hist = NULL;
}

#ifndef SNX_NO_H264
/* the decoder as it starts: the filter's history zero and the queue's lead of silence */
static void hwopus_restart(HwOpus* h) {
    h->phase = h->hpos = 0;
    if (h->ntaps) memset(h->hist, 0, (size_t)h->ntaps * h->channels * sizeof *h->hist);
    h->queued = HWOPUS_LEAD / h->step;
    memset(h->queue, 0, (size_t)h->queued * h->channels * sizeof *h->queue);
}

/* a Blackman-windowed sinc low-pass of 24 * step + 1 taps cutting at 0.45 of the lower rate's half, its gain one */
static void hwopus_filter(HwOpus* h) {
    const double pi = 3.14159265358979323846, fc = 0.45 / h->step;
    double sum = 0.0, m = (double)(h->ntaps - 1), x, w, v;
    u32 i;
    for (i = 0; i < h->ntaps; i++) {
        x = (double)i - m / 2.0;
        w = 0.42 - 0.5 * cos(2.0 * pi * i / m) + 0.08 * cos(4.0 * pi * i / m);
        v = x == 0.0 ? 2.0 * fc : sin(2.0 * pi * fc * x) / (pi * x);
        h->taps[i] = (float)(v * w);
        sum += v * w;
    }
    for (i = 0; i < h->ntaps; i++) h->taps[i] = (float)(h->taps[i] / sum);
}
#endif

/* opens h for rate and channels: one stream (mapping NULL), or streams of them with stereo_streams coupled and
   mapping placing their channels. 0 when it can't be */
int hwopus_open(HwOpus* h, u32 rate, u32 channels, u32 streams, u32 stereo_streams, const u8* mapping) {
#ifndef SNX_NO_H264
    const AVCodec* codec;
    u8* extra;
    size_t qsize, fsize;
    if (!opus_rate_ok(rate) || !channels || channels > 255) return 0;
    if (!mapping ? channels > 2 : !streams || stereo_streams > streams || streams + stereo_streams > 255) return 0;
    h->streams = streams;
    h->stereo_streams = stereo_streams;
    h->mapped = mapping != NULL;
    if (mapping) memcpy(h->mapping, mapping, channels);
    ffmpeg_log_init();
    codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return 0;
    h->rate = rate;
    h->channels = channels;
    h->step = AUDIO_RATE / rate;
    h->cap = 3 * 5760 / h->step; /* a packet, and what FFmpeg's resampler may give back with the next */
    h->ntaps = h->step > 1 ? 24 * h->step + 1 : 0;
    qsize = (size_t)h->cap * channels * sizeof *h->queue; /* even: the floats after it are aligned */
    fsize = (size_t)h->ntaps * (channels + 1) * sizeof *h->taps;
    h->queue = malloc(qsize + fsize + 1);
    h->dec = avcodec_alloc_context3(codec);
    h->pkt = av_packet_alloc();
    h->frame = av_frame_alloc();
    /* an OpusHead (RFC 7845 5.1): mapping family 0 for a stream of one or two channels, 255 (no layout) for more */
    extra = h->dec ? av_mallocz(21 + 255 + AV_INPUT_BUFFER_PADDING_SIZE) : NULL;
    if (!h->queue || !h->dec || !h->pkt || !h->frame || !extra) {
        av_free(extra);
        hwopus_free(h);
        return 0;
    }
    h->taps = (float*)((u8*)h->queue + qsize);
    h->hist = h->taps + h->ntaps;
    if (h->ntaps) hwopus_filter(h);
    hwopus_restart(h);
    memcpy(extra, "OpusHead", 8);
    extra[8] = 1;
    extra[9] = (u8)channels;
    extra[12] = (u8)AUDIO_RATE, extra[13] = (u8)(AUDIO_RATE >> 8), extra[14] = (u8)(AUDIO_RATE >> 16);
    extra[18] = mapping ? 255 : 0;
    if (mapping) {
        extra[19] = (u8)streams;
        extra[20] = (u8)stereo_streams;
        memcpy(extra + 21, mapping, channels);
    }
    h->dec->extradata = extra;
    h->dec->extradata_size = mapping ? 21 + (int)channels : 19;
    h->dec->sample_rate = AUDIO_RATE;
    av_channel_layout_default(&h->dec->ch_layout, (int)channels);
    if (avcodec_open2(h->dec, codec, NULL) < 0) {
        hwopus_free(h);
        return 0;
    }
    pm_init(&h->lk);
    return 1;
#else
    (void)h, (void)rate, (void)channels, (void)streams, (void)stereo_streams, (void)mapping;
    return 0; /* a build without FFmpeg has no Opus decoder */
#endif
}

#ifndef SNX_NO_H264
static float sample_at(const AVFrame* f, u32 ch, u32 channels, u32 i) {
    switch (f->format) {
        case AV_SAMPLE_FMT_FLTP: return ((const float*)f->extended_data[ch])[i] * 32768.0f;
        case AV_SAMPLE_FMT_FLT: return ((const float*)f->extended_data[0])[i * channels + ch] * 32768.0f;
        case AV_SAMPLE_FMT_S16P: return ((const i16*)f->extended_data[ch])[i];
        case AV_SAMPLE_FMT_S16: return ((const i16*)f->extended_data[0])[i * channels + ch];
        default: return 0.0f;
    }
}

/* a sample as s16: rounded, and clipped */
static i16 to_s16(float v) {
    v = (float)floor(v + 0.5f);
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (i16)v;
}
/* a decoded frame into the queue, filtered down to every step-th sample; the oldest go when it is full */
static void hwopus_take(HwOpus* h, const AVFrame* f) {
    u32 i, ch, k, n = (u32)f->nb_samples, fch = (u32)f->ch_layout.nb_channels;
    for (i = 0; i < n; i++) {
        if (h->ntaps) {
            for (ch = 0; ch < h->channels; ch++)
                h->hist[ch * h->ntaps + h->hpos] = ch < fch ? sample_at(f, ch, fch, i) : 0.0f;
            h->hpos = (h->hpos + 1) % h->ntaps;
        }
        if (++h->phase < h->step) continue;
        h->phase = 0;
        if (h->queued == h->cap) {
            memmove(h->queue, h->queue + h->channels, (size_t)(h->cap - 1) * h->channels * sizeof *h->queue);
            h->queued--;
        }
        for (ch = 0; ch < h->channels; ch++) {
            float v = 0.0f;
            if (!h->ntaps)
                v = ch < fch ? sample_at(f, ch, fch, i) : 0.0f;
            else /* the newest sample is the one before hpos */
                for (k = 0; k < h->ntaps; k++)
                    v += h->taps[k] * h->hist[ch * h->ntaps + (h->hpos + h->ntaps - 1 - k) % h->ntaps];
            h->queue[h->queued * h->channels + ch] = to_s16(v);
        }
        h->queued++;
    }
}
#endif

/* one packet behind its 8 byte header (a big-endian size and the encoder's final range) into out: exactly the
   packet's duration at the decoder's rate, from the queue, silence first when the decoder has given less so far */
static int hwopus_decode(HwOpus* h, const u8* in, u64 in_size, i16* out, u64 out_size, int reset, u32* consumed,
                         u32* samples) {
    u32 size, want;
    R_UNLESS(in_size >= 8, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
    size = (u32)in[0] << 24 | (u32)in[1] << 16 | (u32)in[2] << 8 | in[3];
    R_UNLESS(size && size <= in_size - 8, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
    want = opus_packet_48k(in + 8, size) / h->step;
    R_UNLESS(want && (u64)want * h->channels * 2 <= out_size, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
#ifndef SNX_NO_H264
    {
        u32 pad;
        int err;
        if (reset) {
            avcodec_flush_buffers(h->dec);
            hwopus_restart(h);
        }
        R_UNLESS(av_new_packet(h->pkt, (int)size) >= 0, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
        memcpy(h->pkt->data, in + 8, size);
        err = avcodec_send_packet(h->dec, h->pkt);
        av_packet_unref(h->pkt);
        R_UNLESS(err >= 0, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
        while (avcodec_receive_frame(h->dec, h->frame) >= 0) {
            hwopus_take(h, h->frame);
            av_frame_unref(h->frame);
        }
        pad = want > h->queued ? want - h->queued : 0;
        memset(out, 0, (size_t)pad * h->channels * sizeof *out);
        memcpy(out + (size_t)pad * h->channels, h->queue, (size_t)(want - pad) * h->channels * sizeof *out);
        snx_le16_array(out, (size_t)want * h->channels); /* the guest's samples are little-endian */
        h->queued -= want - pad;
        memmove(h->queue, h->queue + (size_t)(want - pad) * h->channels, (size_t)h->queued * h->channels * sizeof *out);
    }
#else
    (void)reset;
    (void)out;
#endif
    *consumed = 8 + size;
    *samples = want;
    R_SUCCEED();
}

/* the samples per channel a packet's payload gives at h's rate, 0 for one that isn't an Opus packet */
u32 hwopus_packet_samples(const HwOpus* h, const u8* payload, u32 size) {
    return opus_packet_48k(payload, size) / h->step;
}

/* An Ounce game's hardware Opus decoders (hle/hook/codec.c), which its SDK runs on the audio DSP and the hooks run
   here: the same decoding, outside the pool's decoders and their save states */
HwOpus* hwopus_new(u32 rate, u32 channels) {
    HwOpus* h = calloc(1, sizeof *h);
    if (h && !hwopus_open(h, rate, channels, 1, channels - 1, NULL)) {
        free(h);
        return NULL;
    }
    return h;
}
void hwopus_delete(HwOpus* h) {
    if (!h) return;
    hwopus_free(h);
    free(h);
}
int hwopus_decode_packet(HwOpus* h, const u8* in, u64 in_size, i16* out, u64 out_size, int reset, u32* consumed,
                         u32* samples) {
    int rc;
    pm_lock(&h->lk);
    rc = hwopus_decode(h, in, in_size, out, out_size, reset, consumed, samples);
    pm_unlock(&h->lk);
    return rc;
}

/* ------------------------------------------------------------ hwopus */
/* The work buffer sizes of the console's hwopus, from Ryujinx: libopus's decoder state, then the stream and frame
   buffers. Ryujinx gives the ExEx commands (8 and 9) no decoder state, as a placeholder: the console's value is not
   known. One game sizes its sound memory from command 9, and with the state in that size its sound init failed. The
   runtime decodes in host memory and never uses the work buffer, so the opens don't check its size */
enum {
    OPUS_STATE_1CH = 0x9564,        /* the decoder (0x5050), SILK (0x2160) and CELT (0x23b4) for one channel */
    OPUS_STATE_2CH = 0xbf74,        /* 0x5850, 0x2160 and 0x45c4 for two */
    OPUS_STREAM_1CH = 0x4564,       /* a multistream decoder's mono stream: the state less its 0x5000 decoder */
    OPUS_STREAM_2CH = 0x6774,       /* a stereo stream: the state less its 0x5800 decoder */
    OPUS_MULTISTREAM_BASE = 0xb920, /* the multistream decoder itself */
};
/* flags: bit 0 asks for frames of 120 ms, not 40 ms. dsp: the size the session's ExEx commands gave. 0 for a rate,
   a channel count or stream counts the session refused */
u32 hwopus_work_size(u32 rate, u32 channels, u32 streams, u32 stereo_streams, int multi, u32 flags, int dsp) {
    if (multi ? !channels || channels > 255 : channels != 1 && channels != 2) return 0;
    if (!opus_rate_ok(rate)) return 0;
    if (multi && (!streams || stereo_streams > streams || streams + stereo_streams > channels)) return 0;
    u32 frame = (u32)snx_align_up(channels * (flags & 1 ? 5760u : 1920u) / (48000u / rate), 64);
    u32 state = channels == 2 ? OPUS_STATE_2CH : OPUS_STATE_1CH;
    if (multi)
        state = OPUS_STREAM_1CH * (streams - stereo_streams) + OPUS_STREAM_2CH * stereo_streams + OPUS_MULTISTREAM_BASE;
    return (dsp ? 0 : state) + (multi ? (u32)snx_align_up(1500u * streams, 64) : 0x600u) + frame;
}

/* ============================================================ save states */
void audio_state_drop(void) {
    u32 i;
    nnaudio_out_state_drop();
    for (i = 0; i < HWOPUS_POOL; i++)
        if (g_hle.audio.opus[i].guest) hwopus_free(&g_hle.audio.opus[i]);
}
void audio_state_rebuild(void) {
    u32 i;
    nnaudio_out_state_rebuild();
    for (i = 0; i < HWOPUS_POOL; i++) {
        HwOpus* h = &g_hle.audio.opus[i];
        u8 mapping[255];
        if (!h->guest) continue;
        memcpy(mapping, h->mapping, sizeof mapping);
#ifndef SNX_NO_H264
        h->dec = NULL;
        h->pkt = NULL;
        h->frame = NULL;
#endif
        h->queue = NULL;
        if (!hwopus_open(h, h->rate, h->channels, h->streams, h->stereo_streams, h->mapped ? mapping : NULL))
            WARN("state: an Opus decoder (%u Hz, %u channels) couldn't be made again\n", h->rate, h->channels);
    }
}
