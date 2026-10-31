/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2019-2026 Citra Emulator Project / Azahar Emulator Project
   The DSP's AAC decoder on the binary pipe. The messages and their handling are ported from Azahar's
   audio_core/hle/{decoder.h,aac_decoder.cpp} (GPL-2.0-or-later), which decode with FAAD2. This decodes with FFmpeg's
   AAC decoder, which the Vita driver uses too. A message is 32 bytes: the codec (u16, 1 is AAC decoding) and command
   (u16) and the result (u32), then 24 bytes the command gives its meaning to. Decode's are, in a request, the source
   address, the byte size, the destination addresses of the two channels and two unknowns, and in a response the sample
   rate (an enum), the channels, the size, two unknowns and the samples of a channel (all u32, little-endian). */
#include "3ds/audio/aac.h"
#include "common/utilities/endian.h"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CODEC_DECODE_AAC = 1 };
enum { COMMAND_INIT, COMMAND_DECODE, COMMAND_SHUTDOWN, COMMAND_LOAD_STATE, COMMAND_SAVE_STATE };
enum { RESULT_SUCCESS, RESULT_ERROR };
/* the sample rates of the response's enum, from 48000 Hz (0) down to 8000 Hz (8) */
static const uint32_t rates[9] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
#define FCRAM_PADDR 0x20000000u
#define MAX_INPUT   (1u << 20) /* the most bytes of AAC a request is taken to hold */
#define MAX_SAMPLES (1u << 20) /* the most samples of a channel a request is allowed to decode to */
#define ADTS_HEADER 7u
#define FALLBACK_SAMPLES 1024u /* what the DSP answers when a decode fails: a frame of 48 kHz stereo */

struct SnxCtrAac {
    SnxCtrMemory* memory;
    AVCodecContext* context; /* NULL until the Init command (or the first Decode) opens it */
    AVPacket* packet;
    AVFrame* frame;
};
typedef struct {
    int16_t* channel[2];
    size_t samples, capacity;
    uint32_t channels, rate;
} Decoded;

SnxCtrAac* snx_ctr_aac_new(SnxCtrMemory* memory) {
    SnxCtrAac* aac = calloc(1, sizeof *aac);
    if (aac) aac->memory = memory;
    return aac;
}
static void close_decoder(SnxCtrAac* aac) {
    avcodec_free_context(&aac->context);
    av_packet_free(&aac->packet);
    av_frame_free(&aac->frame);
}
void snx_ctr_aac_free(SnxCtrAac* aac) {
    if (!aac) return;
    close_decoder(aac);
    free(aac);
}
/* a new decoder: the stream's rate and channels come from the ADTS headers, as FAAD2's NeAACDecInit reads them */
static bool open_decoder(SnxCtrAac* aac) {
    close_decoder(aac);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    aac->context = codec ? avcodec_alloc_context3(codec) : NULL;
    aac->packet = av_packet_alloc();
    aac->frame = av_frame_alloc();
    if (aac->context && aac->packet && aac->frame && avcodec_open2(aac->context, codec, NULL) >= 0) return true;
    close_decoder(aac);
    return false;
}
static void free_decoded(Decoded* d) {
    free(d->channel[0]);
    free(d->channel[1]);
    *d = (Decoded){0};
}
/* the frame's samples (planar floats) appended to the channels as PCM16: false for a layout the DSP doesn't have */
static bool append(Decoded* d, const AVFrame* frame) {
    int channels = frame->ch_layout.nb_channels;
    if (frame->format != AV_SAMPLE_FMT_FLTP || channels < 1 || channels > 2 || frame->nb_samples < 0) return false;
    size_t n = (size_t)frame->nb_samples;
    if (d->samples + n > MAX_SAMPLES) return false;
    if (d->samples + n > d->capacity) {
        size_t capacity = d->capacity ? d->capacity : 2048;
        while (capacity < d->samples + n) capacity *= 2;
        for (int c = 0; c < channels; ++c) {
            int16_t* grown = realloc(d->channel[c], capacity * sizeof(int16_t));
            if (!grown) return false;
            d->channel[c] = grown;
        }
        d->capacity = capacity;
    }
    for (int c = 0; c < channels; ++c) {
        const float* in = (const float*)frame->extended_data[c];
        for (size_t i = 0; i < n; ++i) {
            float v = nearbyintf(in[i] * 32768.0f);
            d->channel[c][d->samples + i] = (int16_t)(v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v);
        }
    }
    d->samples += n;
    d->channels = (uint32_t)channels;
    d->rate = (uint32_t)frame->sample_rate;
    return true;
}
/* The ADTS stream a request holds, a frame at a time (the length is in the header: 13 bits, the header's own bytes
   included), each decoded and appended. False when a frame fails: nothing is then kept */
static bool decode(SnxCtrAac* aac, const uint8_t* data, size_t size, Decoded* out) {
    while (size) {
        if (size < ADTS_HEADER || data[0] != 0xff || (data[1] & 0xf0) != 0xf0) return false;
        size_t length = (size_t)(data[3] & 3) << 11 | (size_t)data[4] << 3 | data[5] >> 5;
        if (length < ADTS_HEADER || length > size) return false;
        if (av_new_packet(aac->packet, (int)length) < 0) return false;
        memcpy(aac->packet->data, data, length);
        int sent = avcodec_send_packet(aac->context, aac->packet);
        av_packet_unref(aac->packet);
        if (sent < 0) return false;
        int received;
        while ((received = avcodec_receive_frame(aac->context, aac->frame)) >= 0) {
            bool ok = append(out, aac->frame);
            av_frame_unref(aac->frame);
            if (!ok) return false;
        }
        if (received != AVERROR(EAGAIN)) return false;
        data += length;
        size -= length;
    }
    return out->samples != 0;
}
/* the response to a Decode that failed or that the DSP couldn't take: it still says how big the request was */
static void fallback(uint8_t response[SNX_CTR_AAC_MESSAGE], uint32_t size) {
    snx_st_le32(response + 8, 0); /* 48000 Hz */
    snx_st_le32(response + 12, 2);
    snx_st_le32(response + 16, size);
    snx_st_le32(response + 28, FALLBACK_SAMPLES);
}
static void put_pcm(SnxCtrAac* aac, uint32_t address, const int16_t* pcm, size_t samples) {
    uint8_t* bytes = malloc(samples * 2);
    if (!bytes) return;
    for (size_t i = 0; i < samples; ++i) snx_st_le16(bytes + i * 2, (uint16_t)pcm[i]);
    (void)snx_ctr_phys_write(aac->memory, address, bytes, samples * 2);
    free(bytes);
}
static void decode_request(SnxCtrAac* aac, const uint8_t* request, uint8_t* response) {
    uint32_t source = snx_ld_le32(request + 8), size = snx_ld_le32(request + 12);
    uint32_t destination[2] = {snx_ld_le32(request + 16), snx_ld_le32(request + 20)};
    fallback(response, size);
    if (!aac->context && !open_decoder(aac)) return;
    /* the source and the destinations are FCRAM's: an address below it is the application's error, which the DSP
       answers with the fallback. The second channel's matters only for a stream that has one */
    if (source < FCRAM_PADDR || destination[0] < FCRAM_PADDR || !size || size > MAX_INPUT) return;
    uint8_t* data = malloc(size);
    Decoded decoded = {0};
    if (data && snx_ctr_phys_read(aac->memory, source, data, size) == SNX_CTR_MEM_OK &&
        decode(aac, data, size, &decoded) && (decoded.channels < 2 || destination[1] >= FCRAM_PADDR)) {
        put_pcm(aac, destination[0], decoded.channel[0], decoded.samples);
        if (decoded.channels > 1) put_pcm(aac, destination[1], decoded.channel[1], decoded.samples);
        uint32_t rate = 0;
        while (rate < 8 && rates[rate] != decoded.rate) ++rate;
        snx_st_le32(response + 8, rate); /* a rate the enum lacks is the last, as the table's end */
        snx_st_le32(response + 12, decoded.channels);
        snx_st_le32(response + 16, size);
        snx_st_le32(response + 28, (uint32_t)decoded.samples);
    }
    free(data);
    free_decoded(&decoded);
}
void snx_ctr_aac_request(SnxCtrAac* aac, const uint8_t request[SNX_CTR_AAC_MESSAGE],
                         uint8_t response[SNX_CTR_AAC_MESSAGE]) {
    memset(response, 0, SNX_CTR_AAC_MESSAGE);
    memcpy(response, request, 4); /* the codec and the command, back */
    snx_st_le32(response + 4, RESULT_SUCCESS);
    switch (snx_ld_le16(request + 2)) {
        case COMMAND_INIT:
            if (!open_decoder(aac)) snx_st_le32(response + 4, RESULT_ERROR);
            memcpy(response + 8, request + 8, 24);
            break;
        case COMMAND_DECODE: decode_request(aac, request, response); break;
        case COMMAND_SHUTDOWN:
        case COMMAND_LOAD_STATE:
        case COMMAND_SAVE_STATE: memcpy(response + 8, request + 8, 24); break; /* Azahar logs them and echoes */
        default:
            snx_st_le32(response + 4, RESULT_ERROR);
            memcpy(response + 8, request + 8, 24);
            break;
    }
}
