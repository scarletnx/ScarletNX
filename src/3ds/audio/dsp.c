/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The HLE DSP's frame loop, pipes and power states. Ported from Azahar audio_core/hle/hle.cpp (GPL-2.0-or-later,
   Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e. The binary pipe's
   AAC decoder follows audio_core/hle/decoder.h and aac_decoder.cpp at azahar-emu/azahar 6e42de8, with FFmpeg's decoder
   in place of FAAD2. The shared memory lives in DSP RAM, which the DSP reads and writes as a bus master
   (snx_ctr_phys_read/write), never through a host pointer. */
#include "3ds/audio/internal.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_io.h"
#include <libavcodec/avcodec.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DSP_RAM_SIZE 0x80000u

typedef struct {
    uint8_t* data;
    size_t size, capacity;
} Pipe;
struct SnxCtrDsp {
    SnxCtrMemory* memory;
    SnxCtrDspInterrupt interrupt;
    void* context;
    SnxCtrDspState state;
    Pipe pipes[SNX_CTR_DSP_PIPES];
    DspSource sources[DSP_SOURCES];
    DspMixers mixers;
    uint8_t* backup; /* DSP RAM across sleep, DSP_RAM_SIZE */
    /* the binary pipe's AAC decoder: all three, or none while aac is NULL */
    AVCodecContext* aac;
    AVPacket* packet;
    AVFrame* decoded;
};

static bool pipe_append(Pipe* pipe, const void* data, size_t size) {
    if (pipe->size + size > pipe->capacity) {
        size_t capacity = pipe->capacity ? pipe->capacity : 64;
        while (capacity < pipe->size + size) capacity *= 2;
        uint8_t* grown = realloc(pipe->data, capacity);
        if (!grown) return false;
        pipe->data = grown;
        pipe->capacity = capacity;
    }
    memcpy(pipe->data + pipe->size, data, size);
    pipe->size += size;
    return true;
}
static void reset_pipes(SnxCtrDsp* dsp) {
    for (unsigned i = 0; i < SNX_CTR_DSP_PIPES; ++i) dsp->pipes[i].size = 0;
}
static uint32_t region(unsigned index) { return SNX_CTR_DSP_RAM + (index ? DSP_REGION1 : DSP_REGION0); }
static bool ram_read(SnxCtrDsp* dsp, uint32_t address, void* out, size_t size) {
    return snx_ctr_phys_read(dsp->memory, address, out, size) == SNX_CTR_MEM_OK;
}
static bool ram_write(SnxCtrDsp* dsp, uint32_t address, const void* data, size_t size) {
    return snx_ctr_phys_write(dsp->memory, address, data, size) == SNX_CTR_MEM_OK;
}
static void ram_clear(SnxCtrDsp* dsp) {
    static const uint8_t zero[0x1000];
    for (uint32_t at = 0; at < DSP_RAM_SIZE; at += sizeof zero) ram_write(dsp, SNX_CTR_DSP_RAM + at, zero, sizeof zero);
}
static void aac_close(SnxCtrDsp* dsp) {
    avcodec_free_context(&dsp->aac);
    av_packet_free(&dsp->packet);
    av_frame_free(&dsp->decoded);
}

SnxCtrDsp* snx_ctr_dsp_new(SnxCtrMemory* memory, SnxCtrDspInterrupt interrupt, void* context) {
    SnxCtrDsp* dsp = calloc(1, sizeof *dsp);
    if (!dsp) return NULL;
    dsp->memory = memory;
    dsp->interrupt = interrupt;
    dsp->context = context;
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_reset(&dsp->sources[i]);
    dsp_mixers_reset(&dsp->mixers);
    ram_clear(dsp);
    return dsp;
}
void snx_ctr_dsp_free(SnxCtrDsp* dsp) {
    if (!dsp) return;
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_free(&dsp->sources[i]);
    dsp_mixers_free(&dsp->mixers);
    for (unsigned i = 0; i < SNX_CTR_DSP_PIPES; ++i) free(dsp->pipes[i].data);
    aac_close(dsp);
    free(dsp->backup);
    free(dsp);
}
SnxCtrDspState snx_ctr_dsp_state(const SnxCtrDsp* dsp) { return dsp->state; }

/* Answers on the audio pipe with the DSP RAM addresses of the region's structures, in DSP words: a count, then the
   15 addresses (see also ConvertProcessAddressFromDspDram) */
static void write_struct_addresses(SnxCtrDsp* dsp) {
    static const uint16_t offsets[15] = {DSP_FRAME_COUNTER, DSP_SOURCE_CONFIGS, DSP_SOURCE_STATUSES, DSP_ADPCM_COEFFS,
                                         DSP_CONFIGURATION, DSP_STATUS,         DSP_FINAL_SAMPLES,   DSP_INTERMEDIATE,
                                         DSP_COMPRESSOR,    DSP_DEBUG,          DSP_UNKNOWN10,       DSP_UNKNOWN11,
                                         DSP_UNKNOWN12,     DSP_UNKNOWN13,      DSP_UNKNOWN14};
    uint8_t words[2 * 16];
    words[0] = 15;
    words[1] = 0;
    for (unsigned i = 0; i < 15; ++i) {
        uint16_t address = (uint16_t)(0x8000 + offsets[i] / 2);
        words[2 + i * 2] = (uint8_t)address;
        words[3 + i * 2] = (uint8_t)(address >> 8);
    }
    (void)pipe_append(&dsp->pipes[SNX_CTR_DSP_PIPE_AUDIO], words, sizeof words);
    dsp->interrupt(dsp->context, SNX_CTR_DSP_PIPE_AUDIO);
}
/* Initialize: not quite what the hardware resets, but a clean, consistent state every time, as Azahar does */
static void initialize(SnxCtrDsp* dsp) {
    aac_close(dsp);
    ram_clear(dsp);
    dsp_mixers_reset(&dsp->mixers);
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_reset(&dsp->sources[i]);
    reset_pipes(dsp);
}
/* Applications save the structures sleep garbles and restore them after wakeup themselves */
static void sleep_state(SnxCtrDsp* dsp) {
    if (!dsp->backup) dsp->backup = malloc(DSP_RAM_SIZE);
    if (dsp->backup && !ram_read(dsp, SNX_CTR_DSP_RAM, dsp->backup, DSP_RAM_SIZE)) memset(dsp->backup, 0, DSP_RAM_SIZE);
    dsp_mixers_sleep(&dsp->mixers);
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_sleep(&dsp->sources[i]);
}
static void wakeup(SnxCtrDsp* dsp) {
    aac_close(dsp);
    if (dsp->backup) {
        ram_write(dsp, SNX_CTR_DSP_RAM, dsp->backup, DSP_RAM_SIZE);
        memset(dsp->backup, 0, DSP_RAM_SIZE);
    }
    dsp_mixers_wakeup(&dsp->mixers);
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_wakeup(&dsp->sources[i]);
    reset_pipes(dsp);
}

/* The binary pipe carries the AAC decoder's requests and answers, 32 bytes each: a codec, a command, a result and six
   words, every field little endian (Azahar audio_core/hle/decoder.h) */
#define BINARY_MESSAGE_SIZE 32u
typedef struct {
    uint16_t codec, command;
    uint32_t result;
    uint32_t argument[6];
} BinaryMessage;
enum { CODEC_AAC = 1 };
enum { AAC_INIT, AAC_DECODE, AAC_SHUTDOWN, AAC_LOAD_STATE, AAC_SAVE_STATE };
enum { AAC_SUCCESS, AAC_ERROR };
/* an AAC-LC frame holds 1024 samples of each channel */
#define AAC_SAMPLES 1024u

static BinaryMessage binary_load(const uint8_t* data) {
    BinaryMessage message = {
        .codec = snx_ld_le16(data), .command = snx_ld_le16(data + 2), .result = snx_ld_le32(data + 4)};
    for (unsigned i = 0; i < 6; ++i) message.argument[i] = snx_ld_le32(data + 8 + 4 * i);
    return message;
}
static void binary_store(uint8_t* data, const BinaryMessage* message) {
    snx_st_le16(data, message->codec);
    snx_st_le16(data + 2, message->command);
    snx_st_le32(data + 4, message->result);
    for (unsigned i = 0; i < 6; ++i) snx_st_le32(data + 8 + 4 * i, message->argument[i]);
}
static bool aac_open(SnxCtrDsp* dsp) {
    if (dsp->aac) return true;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    dsp->aac = codec ? avcodec_alloc_context3(codec) : NULL;
    dsp->packet = av_packet_alloc();
    dsp->decoded = av_frame_alloc();
    if (dsp->aac && dsp->packet && dsp->decoded && avcodec_open2(dsp->aac, codec, NULL) >= 0) return true;
    aac_close(dsp);
    return false;
}
/* The answer names the rate by its index in the nine rates the DSP knows. A stream at another rate is refused, where
   Azahar answers 48 kHz for it and the game would play it at the wrong speed. */
static bool aac_rate(int rate, uint32_t* code) {
    static const int rates[] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    for (uint32_t i = 0; i < sizeof rates / sizeof *rates; ++i)
        if (rates[i] == rate) {
            *code = i;
            return true;
        }
    return false;
}
/* The length of the ADTS frame at data (a sync word, layer 0 and one raw data block), or 0 when it is not one */
static size_t adts_frame(const uint8_t* data, size_t size) {
    if (size < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0 || (data[6] & 0x03) != 0) return 0;
    size_t length = (size_t)(data[3] & 0x03) << 11 | (size_t)data[4] << 3 | (size_t)(data[5] >> 5);
    return length >= 7 && length <= size ? length : 0;
}
/* The number of ADTS frames that make up the whole request, or 0 when a byte is not part of one */
static size_t adts_frames(const uint8_t* data, size_t size) {
    size_t count = 0;
    for (size_t at = 0, length; at < size; at += length, ++count)
        if (!(length = adts_frame(data + at, size - at))) return 0;
    return count;
}
/* the DSP reads and writes guest RAM as a bus master, never VRAM, its own RAM or MMIO */
static bool ram_range(const SnxCtrDsp* dsp, uint32_t address, uint32_t size) {
    const SnxCtrRegion* region = snx_ctr_physical_region(snx_ctr_memory_profile(dsp->memory), address, size);
    return region && region->kind == SNX_CTR_RAM;
}
static int16_t pcm16(float sample) {
    if (isnan(sample)) return 0;
    float scaled = sample * 32767.0f;
    if (scaled >= 32767.0f) return 32767;
    if (scaled <= -32768.0f) return -32768;
    return (int16_t)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}
/* Decodes every frame of the input into pcm, channel 0 first and channel 1 channel_bytes after it. Every frame has to
   decode to one frame of the first frame's rate and channel count. */
static bool aac_frames(SnxCtrDsp* dsp, const uint8_t* input, size_t size, uint8_t* pcm, size_t channel_bytes,
                       BinaryMessage* answer) {
    size_t sample = 0;
    for (size_t at = 0, length; at < size; at += length, sample += AAC_SAMPLES) {
        length = adts_frame(input + at, size - at);
        av_packet_unref(dsp->packet);
        if (av_new_packet(dsp->packet, (int)length) < 0) return false;
        memcpy(dsp->packet->data, input + at, length);
        if (avcodec_send_packet(dsp->aac, dsp->packet) < 0 || avcodec_receive_frame(dsp->aac, dsp->decoded) < 0)
            return false;
        uint32_t rate, channels = (uint32_t)dsp->decoded->ch_layout.nb_channels;
        bool usable = dsp->decoded->format == AV_SAMPLE_FMT_FLTP && dsp->decoded->nb_samples == (int)AAC_SAMPLES &&
                      channels >= 1 && channels <= 2 && aac_rate(dsp->decoded->sample_rate, &rate);
        if (!usable || (sample && (rate != answer->argument[0] || channels != answer->argument[1]))) return false;
        answer->argument[0] = rate;
        answer->argument[1] = channels;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const float* decoded = (const float*)dsp->decoded->extended_data[channel];
            uint8_t* out = pcm + channel * channel_bytes + sample * 2;
            for (unsigned i = 0; i < AAC_SAMPLES; ++i) snx_st_le16(out + 2 * i, (uint16_t)pcm16(decoded[i]));
        }
        av_frame_unref(dsp->decoded);
    }
    return true;
}
/* A request is the source and its size, then the left and right destinations. It decodes every frame before it writes
   a destination, so a request it refuses writes nothing. A mono stream writes only the left destination, as Azahar
   skips the empty right channel. The answer echoes the size and counts the samples of the whole request. Azahar answers
   success with made-up values when a decode fails. This answers the error instead, so the game is not told of samples
   that were never written. */
static bool aac_decode(SnxCtrDsp* dsp, const BinaryMessage* request, BinaryMessage* answer) {
    uint32_t source = request->argument[0], size = request->argument[1];
    uint32_t left = request->argument[2], right = request->argument[3];
    if (!size || !ram_range(dsp, source, size) || !aac_open(dsp)) return false;
    uint8_t* input = malloc(size);
    if (!input) return false;
    bool decoded = false;
    size_t frames = ram_read(dsp, source, input, size) ? adts_frames(input, size) : 0;
    uint64_t channel_bytes = (uint64_t)frames * AAC_SAMPLES * 2u;
    /* the input, then both channels of PCM, in one block */
    uint8_t* block = frames && channel_bytes <= UINT32_MAX && ram_range(dsp, left, (uint32_t)channel_bytes)
                         ? realloc(input, size + 2u * (size_t)channel_bytes)
                         : NULL;
    if (block) {
        input = block;
        uint8_t* pcm = block + size;
        decoded = aac_frames(dsp, input, size, pcm, (size_t)channel_bytes, answer);
        if (decoded && answer->argument[1] == 2)
            decoded = ram_range(dsp, right, (uint32_t)channel_bytes) &&
                      ram_write(dsp, left, pcm, (size_t)channel_bytes) &&
                      ram_write(dsp, right, pcm + channel_bytes, (size_t)channel_bytes);
        else if (decoded)
            decoded = ram_write(dsp, left, pcm, (size_t)channel_bytes);
    }
    free(input);
    if (!decoded) {
        aac_close(dsp); /* the next request starts from a new decoder, with nothing of this stream left in it */
        return false;
    }
    answer->argument[2] = size;
    answer->argument[5] = (uint32_t)(frames * AAC_SAMPLES);
    return true;
}
static void binary_request(SnxCtrDsp* dsp, const uint8_t* data, size_t size) {
    if (size != BINARY_MESSAGE_SIZE) return; /* a message of another size gets no answer, as in Azahar */
    BinaryMessage request = binary_load(data);
    BinaryMessage answer = {.codec = request.codec, .command = request.command, .result = AAC_ERROR};
    bool done = false;
    if (request.codec == CODEC_AAC) {
        switch (request.command) {
            case AAC_INIT:
                done = aac_open(dsp);
                if (done) avcodec_flush_buffers(dsp->aac);
                break;
            case AAC_DECODE: done = aac_decode(dsp, &request, &answer); break;
            case AAC_SHUTDOWN:
                aac_close(dsp);
                done = true;
                break;
            case AAC_LOAD_STATE:
            case AAC_SAVE_STATE: done = true; break; /* answered, with nothing to keep, as Azahar answers them */
            default: break;
        }
    }
    if (done && request.command != AAC_DECODE) answer = request;
    if (done) answer.result = AAC_SUCCESS;
    uint8_t bytes[BINARY_MESSAGE_SIZE];
    binary_store(bytes, &answer);
    dsp->pipes[SNX_CTR_DSP_PIPE_BINARY].size = 0; /* one answer at a time, as Azahar leaves the pipe */
    (void)pipe_append(&dsp->pipes[SNX_CTR_DSP_PIPE_BINARY], bytes, sizeof bytes);
    dsp->interrupt(dsp->context, SNX_CTR_DSP_PIPE_BINARY);
}

const char* snx_ctr_dsp_pipe_write(SnxCtrDsp* dsp, unsigned pipe, const uint8_t* data, size_t size) {
    if (pipe == SNX_CTR_DSP_PIPE_AUDIO) {
        /* the state changes; another length is an application error the DSP ignores */
        if (size != 4) return NULL;
        switch (data[0]) {
            case 0: /* Initialize */
                initialize(dsp);
                write_struct_addresses(dsp);
                dsp->state = SNX_CTR_DSP_ON;
                break;
            case 1: dsp->state = SNX_CTR_DSP_OFF; break; /* Shutdown */
            case 2:                                      /* Wakeup */
                wakeup(dsp);
                write_struct_addresses(dsp);
                dsp->state = SNX_CTR_DSP_ON;
                break;
            case 3: /* Sleep */
                sleep_state(dsp);
                write_struct_addresses(dsp);
                dsp->state = SNX_CTR_DSP_SLEEPING;
                break;
            default: dsp->state = SNX_CTR_DSP_OFF; break;
        }
        return NULL;
    }
    if (pipe == SNX_CTR_DSP_PIPE_BINARY) {
        binary_request(dsp, data, size);
        return NULL;
    }
    return "only the DSP's audio and binary pipes are implemented";
}
size_t snx_ctr_dsp_pipe_readable(const SnxCtrDsp* dsp, unsigned pipe) {
    return pipe < SNX_CTR_DSP_PIPES ? dsp->pipes[pipe].size : 0;
}
size_t snx_ctr_dsp_pipe_read(SnxCtrDsp* dsp, unsigned pipe, uint8_t* out, size_t size) {
    if (pipe >= SNX_CTR_DSP_PIPES) return 0;
    Pipe* p = &dsp->pipes[pipe];
    if (size > p->size) size = p->size;
    memcpy(out, p->data, size);
    memmove(p->data, p->data + size, p->size - size);
    p->size -= size;
    return size;
}

/* The region with the higher frame counter is the one the application finished, unless the counter wrapped */
static unsigned current_region(SnxCtrDsp* dsp) {
    uint16_t counter[2] = {0, 0};
    for (unsigned i = 0; i < 2; ++i) (void)ram_read(dsp, region(i) + DSP_FRAME_COUNTER, &counter[i], 2);
    if (counter[0] == 0xffff && counter[1] != 0xfffe) return 1;
    if (counter[1] == 0xffff && counter[0] != 0xfffe) return 0;
    return counter[0] > counter[1] ? 0 : 1;
}

bool snx_ctr_dsp_tick(SnxCtrDsp* dsp, int16_t frame[SNX_CTR_DSP_SAMPLES][2]) {
    if (dsp->state != SNX_CTR_DSP_ON) return false;
    unsigned current = current_region(dsp);
    uint32_t read = region(current), write = region(!current);
    /* about 24 KiB of the region, copied in and out */
    DspSourceConfig configs[DSP_SOURCES];
    int16_t coeffs[DSP_SOURCES][16];
    DspSourceStatus statuses[DSP_SOURCES];
    DspIntermediate read_samples, write_samples;
    DspConfig config;
    (void)ram_read(dsp, read + DSP_SOURCE_CONFIGS, configs, sizeof configs);
    (void)ram_read(dsp, read + DSP_ADPCM_COEFFS, coeffs, sizeof coeffs);
    (void)ram_read(dsp, read + DSP_CONFIGURATION, &config, sizeof config);
    (void)ram_read(dsp, read + DSP_INTERMEDIATE, &read_samples, sizeof read_samples);
    (void)ram_read(dsp, write + DSP_INTERMEDIATE, &write_samples, sizeof write_samples);
    /* the intermediate mixes: every source's frame mixed into each with its gains */
    DspQuadFrame mixes[3] = {0};
    for (unsigned i = 0; i < DSP_SOURCES; i++) {
        uint32_t dirty = configs[i].dirty;
        uint16_t buffers_dirty = configs[i].buffers_dirty;
        statuses[i] = dsp_source_tick(&dsp->sources[i], dsp->memory, &configs[i], coeffs[i]);
        for (unsigned mix = 0; mix < 3; mix++) {
            dsp_source_mix(&dsp->sources[i], mixes[mix], mix);
            dsp_source_mix_done(&dsp->sources[i], mix);
        }
        /* the dirty bits it took are cleared in the application's region, as the firmware clears them */
        uint32_t at = read + DSP_SOURCE_CONFIGS + i * (uint32_t)sizeof(DspSourceConfig);
        if (dirty != configs[i].dirty) ram_write(dsp, at, &configs[i].dirty, 4);
        if (buffers_dirty != configs[i].buffers_dirty)
            ram_write(dsp, at + offsetof(DspSourceConfig, buffers_dirty), &configs[i].buffers_dirty, 2);
    }
    ram_write(dsp, write + DSP_SOURCE_STATUSES, statuses, sizeof statuses);
    /* the final mix */
    DspConfig taken = config;
    if (dsp_mixers_tick(&dsp->mixers, &config, &read_samples, &write_samples, mixes))
        ram_write(dsp, write + DSP_INTERMEDIATE, &write_samples, sizeof write_samples);
    if (taken.dirty != config.dirty) ram_write(dsp, read + DSP_CONFIGURATION, &config.dirty, 4);
    for (unsigned i = 0; i < 2; ++i)
        if (taken.delay[i].dirty != config.delay[i].dirty)
            ram_write(dsp, read + DSP_CONFIGURATION + (uint32_t)offsetof(DspConfig, delay) + i * 20u,
                      &config.delay[i].dirty, 2);
    const uint16_t status[2] = {0, 0}; /* unknown, dropped_frames */
    ram_write(dsp, write + DSP_STATUS, status, sizeof status);
    memcpy(frame, dsp->mixers.frame, sizeof dsp->mixers.frame);
    ram_write(dsp, write + DSP_FINAL_SAMPLES, dsp->mixers.frame, sizeof dsp->mixers.frame);
    dsp->interrupt(dsp->context, SNX_CTR_DSP_PIPE_AUDIO);
    return true;
}

/* ---- save states (runtime/state.c) ---- */

#define STATE_PIPE_MOST   (1u << 20)              /* bytes a pipe holds, past which a state is damaged */
#define STATE_QUEUE_MOST  4096u                   /* buffers a source queues */
#define STATE_BUFFER_MOST (1u << 24)              /* a decoded buffer's frames */
#define STATE_DELAY_MOST  (4u * SNX_CTR_DSP_RATE) /* mixers.c's MAX_DELAY_SAMPLES */
uint64_t snx_ctr_dsp_state_schema(void) {
    static const SnxCtrDsp dsp;
    uint64_t h = SNX_FNV_BASIS ^ sizeof dsp;
    __builtin_dump_struct(&dsp, snx_state_schema_out, &h);
    __builtin_dump_struct(&dsp.pipes[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&dsp.sources[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&dsp.sources[0].state.filters, snx_state_schema_out, &h);
    __builtin_dump_struct(&dsp.mixers, snx_state_schema_out, &h);
    __builtin_dump_struct(&dsp.mixers.state.delay[0], snx_state_schema_out, &h);
    static const DspQueued queued;
    __builtin_dump_struct(&queued, snx_state_schema_out, &h);
    return h;
}
/* a source's state's heap parts, after its bytes: the queue, then whether it has a current buffer and its frames */
static int source_put(StateIo* io, const DspSourceState* s) {
    return io_put(io, s->queue, s->queued * sizeof *s->queue) || io_put32(io, s->current != NULL) ||
           io_put(io, s->current, s->current ? s->current_count * sizeof *s->current : 0);
}
static int source_get(StateIo* io, DspSourceState* s) {
    uint32_t current = 0;
    s->queue = NULL;
    s->current = NULL;
    if (s->queued > STATE_QUEUE_MOST || s->current_count > STATE_BUFFER_MOST) return -1;
    s->queue_capacity = s->queued;
    if (s->queued &&
        (!(s->queue = malloc(s->queued * sizeof *s->queue)) || io_get(io, s->queue, s->queued * sizeof *s->queue)))
        return -1;
    if (io_get32(io, &current)) return -1;
    if (current) {
        s->current = malloc((s->current_count ? s->current_count : 1) * sizeof *s->current);
        if (!s->current || io_get(io, s->current, s->current_count * sizeof *s->current)) return -1;
    }
    return 0;
}
static int delay_put(StateIo* io, const DspDelay* d) {
    return io_put32(io, d->delay_output != NULL) ||
           io_put(io, d->delay_output, d->delay_output ? (size_t)d->delay_samples * 4 * sizeof *d->delay_output : 0);
}
static int delay_get(StateIo* io, DspDelay* d) {
    uint32_t has = 0;
    d->delay_output = NULL;
    if (io_get32(io, &has) || d->delay_samples > STATE_DELAY_MOST || (has && !d->delay_samples)) return -1;
    if (!has) return 0;
    size_t bytes = (size_t)d->delay_samples * 4 * sizeof *d->delay_output;
    return (d->delay_output = malloc(bytes)) && !io_get(io, d->delay_output, bytes) ? 0 : -1;
}
int snx_ctr_dsp_state_save(const SnxCtrDsp* dsp, StateIo* io) {
    SnxCtrDsp copy;
    memcpy(&copy, dsp, sizeof copy); /* its padding too, which goes in the file */
    copy.memory = NULL;
    copy.interrupt = NULL;
    copy.context = copy.aac = NULL;
    copy.packet = NULL;
    copy.decoded = NULL;
    copy.backup = NULL;
    /* the allocations' sizes are the running process's, not the DSP's: a load allocates what it reads */
    for (unsigned i = 0; i < SNX_CTR_DSP_PIPES; ++i) copy.pipes[i].data = NULL, copy.pipes[i].capacity = 0;
    for (unsigned i = 0; i < DSP_SOURCES; ++i) {
        copy.sources[i].state.queue_capacity = copy.sources[i].backup.queue_capacity = 0;
        copy.sources[i].state.queue = copy.sources[i].backup.queue = NULL;
        copy.sources[i].state.current = copy.sources[i].backup.current = NULL;
    }
    for (unsigned i = 0; i < 2; ++i)
        copy.mixers.state.delay[i].delay_output = copy.mixers.backup.delay[i].delay_output = NULL;
    int failed = io_put(io, &copy, sizeof copy) || io_put32(io, dsp->aac != NULL) ||
                 io_put32(io, dsp->backup != NULL) || io_put(io, dsp->backup, dsp->backup ? DSP_RAM_SIZE : 0);
    for (unsigned i = 0; !failed && i < SNX_CTR_DSP_PIPES; ++i)
        failed = io_put(io, dsp->pipes[i].data, dsp->pipes[i].size);
    for (unsigned i = 0; !failed && i < DSP_SOURCES; ++i)
        failed = source_put(io, &dsp->sources[i].state) || source_put(io, &dsp->sources[i].backup);
    for (unsigned i = 0; !failed && i < 2; ++i)
        failed = delay_put(io, &dsp->mixers.state.delay[i]) || delay_put(io, &dsp->mixers.backup.delay[i]);
    return failed ? -1 : 0;
}
SnxCtrDsp* snx_ctr_dsp_state_parse(SnxCtrMemory* memory, SnxCtrDspInterrupt interrupt, void* context, StateIo* io,
                                   char* why, size_t whysize) {
    SnxCtrDsp* dsp = calloc(1, sizeof *dsp);
    uint32_t aac = 0, backup = 0;
    if (!dsp || io_get(io, dsp, sizeof *dsp)) {
        free(dsp);
        snprintf(why, whysize, "its DSP section is damaged or cut short");
        return NULL;
    }
    /* every pointer the bytes held is the save's: none of them is freed */
    dsp->memory = memory;
    dsp->interrupt = interrupt;
    dsp->context = context;
    dsp->aac = NULL;
    dsp->packet = NULL;
    dsp->decoded = NULL;
    dsp->backup = NULL;
    for (unsigned i = 0; i < SNX_CTR_DSP_PIPES; ++i) dsp->pipes[i].data = NULL;
    for (unsigned i = 0; i < DSP_SOURCES; ++i) {
        dsp->sources[i].state.queue = dsp->sources[i].backup.queue = NULL;
        dsp->sources[i].state.current = dsp->sources[i].backup.current = NULL;
    }
    for (unsigned i = 0; i < 2; ++i)
        dsp->mixers.state.delay[i].delay_output = dsp->mixers.backup.delay[i].delay_output = NULL;
    bool failed = io_get32(io, &aac) || io_get32(io, &backup);
    if (!failed && backup) failed = !(dsp->backup = malloc(DSP_RAM_SIZE)) || io_get(io, dsp->backup, DSP_RAM_SIZE);
    for (unsigned i = 0; !failed && i < SNX_CTR_DSP_PIPES; ++i) {
        Pipe* pipe = &dsp->pipes[i];
        pipe->capacity = pipe->size;
        failed = pipe->size > STATE_PIPE_MOST ||
                 (pipe->size && (!(pipe->data = malloc(pipe->size)) || io_get(io, pipe->data, pipe->size)));
    }
    for (unsigned i = 0; !failed && i < DSP_SOURCES; ++i)
        failed = source_get(io, &dsp->sources[i].state) || source_get(io, &dsp->sources[i].backup);
    for (unsigned i = 0; !failed && i < 2; ++i)
        failed = delay_get(io, &dsp->mixers.state.delay[i]) || delay_get(io, &dsp->mixers.backup.delay[i]);
    if (failed) {
        snx_ctr_dsp_free(dsp);
        snprintf(why, whysize, "its DSP section is damaged or cut short");
        return NULL;
    }
    /* the AAC decoder opened again as it was opened: what it kept of the stream's last frames is gone */
    if (aac && !aac_open(dsp)) {
        snx_ctr_dsp_free(dsp);
        snprintf(why, whysize, "the DSP's AAC decoder can't be opened again (FFmpeg has no AAC decoder)");
        return NULL;
    }
    return dsp;
}
