/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceAudiodecUser: ATRAC9 and AAC decoded with FFmpeg's libavcodec into 16-bit interleaved PCM, a frame a call (n with
   DecodeNFrames), and SceCodecEngineUser's memory blocks, which the decoders' callers carve their memory from. As the
   vitasdk headers (MIT) declare them; the sizes a decoder reports are Vita3K's (GPL-2.0-or-later,
   modules/SceAudiodec/SceAudiodecUser.cpp).

   ATRAC9: a superframe of one, two or four frames is the configuration's size (configData's average frame size times
   its frames), and its frames' own sizes vary, so FFmpeg's decoder takes it whole: its first frame decodes it, giving
   the first frame's samples and using the superframe's bytes, and the others give theirs from it, using none. The
   caller's stream goes on a superframe at a time, its padding included, as the console's does. AAC: a raw frame, no
   ADTS, of the size SceMp4 gave the unit at that address (vita_mp4_unit_size: a raw frame says nothing of its length),
   or the most a decoder reads */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_io.h"
#include <libavcodec/avcodec.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TYPE_AT9 0x1003u
#define TYPE_AAC 0x1005u
#define SCE_AUDIODEC_ERROR_API_FAIL           0x807f0000u
#define SCE_AUDIODEC_ERROR_INVALID_TYPE       0x807f0001u
#define SCE_AUDIODEC_ERROR_OUT_OF_MEMORY      0x807f0004u
#define SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE 0x807f0007u
#define SCE_AUDIODEC_ERROR_INVALID_PTR        0x807f0008u
#define SCE_AUDIODEC_ERROR_INVALID_HANDLE     0x807f0009u
#define SCE_AUDIODEC_ERROR_INVALID_SIZE       0x807f000du
#define CTRL_SIZE  0x28u
#define AAC_MOST   1536u /* the most an AAC frame takes */
#define AT9_MOST   1024u
#define DECODERS   32u
#define BLOCKS     16u

typedef struct {
    bool used;
    uint32_t type, channels, frame_bytes, frame_samples;
    /* ATRAC9: the superframe's frames, the next one's index in it, and its samples decoded (interleaved 16-bit, made at
       the decoder's creation for a superframe of more than one frame) */
    uint32_t superframe_frames, superframe_index;
    unsigned char* superframe_pcm;
    AVCodecContext* codec;
    AVPacket* packet;
    AVFrame* frame;
} Decoder;
struct VitaAudiodec {
    Decoder decoders[DECODERS];
    struct {
        uint32_t base, size, used;
    } blocks[BLOCKS]; /* the codec engine's: carved from their start */
};

static struct VitaAudiodec* state(VitaProcess* p) {
    if (!p->audiodec) p->audiodec = calloc(1, sizeof *p->audiodec);
    return p->audiodec;
}
static Decoder* decoder_of(VitaProcess* p, uint32_t ctrl) {
    uint32_t handle = ctrl ? vita_read32(p, ctrl + 4) : 0;
    return p->audiodec && handle >= 1 && handle <= DECODERS && p->audiodec->decoders[handle - 1].used
               ? &p->audiodec->decoders[handle - 1]
               : NULL;
}
static void release(Decoder* d) {
    free(d->superframe_pcm);
    avcodec_free_context(&d->codec);
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    memset(d, 0, sizeof *d);
}
void vita_audiodec_free(VitaProcess* p) {
    if (!p->audiodec) return;
    for (unsigned i = 0; i < DECODERS; ++i)
        if (p->audiodec->decoders[i].used) release(&p->audiodec->decoders[i]);
    free(p->audiodec);
    p->audiodec = NULL;
}
uint32_t vita_audiodec_ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

/* ---- the decoders ---- */
/* sceAudiodecGetContextSize(ctrl, type): the decoder's memory, which its caller allocates */
uint32_t vita_audiodec_get_context_size(VitaProcess* p, VitaThread* t, uint32_t ctrl, uint32_t type) {
    (void)t;
    if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    if (vita_read32(p, ctrl) != CTRL_SIZE) return SCE_AUDIODEC_ERROR_INVALID_SIZE;
    return type == TYPE_AAC ? 0x18000u : type == TYPE_AT9 ? 0xc00u : SCE_AUDIODEC_ERROR_INVALID_TYPE;
}
/* frames' samples by ATRAC9's sample rate index (FFmpeg's at9_tab_sri_frame_log2) */
static const uint8_t at9_frame_log2[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
/* A decoder of type for ctrl (its SceAudiodecInfo at pInfo, 0x24): FFmpeg's, its sizes written back (maxEsSize at
   0x10, maxPcmSize at 0x1c; an ATRAC9 one's channels, bit rate, sample rate, superframe size and frames from its
   configuration) */
static uint32_t create(VitaProcess* p, uint32_t ctrl, uint32_t type) {
    if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    if (vita_read32(p, ctrl) != CTRL_SIZE) return SCE_AUDIODEC_ERROR_INVALID_SIZE;
    uint32_t info = vita_read32(p, ctrl + 0x24);
    if (!info) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    if (type != TYPE_AAC && type != TYPE_AT9) return SCE_AUDIODEC_ERROR_INVALID_TYPE;
    struct VitaAudiodec* s = state(p);
    Decoder* d = NULL;
    for (unsigned i = 0; s && i < DECODERS && !d; ++i)
        if (!s->decoders[i].used) d = &s->decoders[i];
    if (!d) return s ? SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE : SCE_AUDIODEC_ERROR_OUT_OF_MEMORY;
    const AVCodec* codec = avcodec_find_decoder(type == TYPE_AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_ATRAC9);
    if (!codec) return SCE_AUDIODEC_ERROR_API_FAIL;
    d->used = true, d->type = type;
    d->codec = avcodec_alloc_context3(codec), d->packet = av_packet_alloc(), d->frame = av_frame_alloc();
    unsigned char* extra = av_mallocz(12 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->codec || !d->packet || !d->frame || !extra) {
        av_free(extra);
        release(d);
        return SCE_AUDIODEC_ERROR_OUT_OF_MEMORY;
    }
    uint32_t most_es, most_pcm;
    if (type == TYPE_AAC) { /* SceAudiodecInfoAac: size, isAdts, ch, samplingRate, isSbr */
        static const uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                           22050, 16000, 12000, 11025, 8000,  7350};
        uint32_t channels = vita_read32(p, info + 8), rate = vita_read32(p, info + 12), index = 0;
        while (index < 12 && rates[index] != rate) ++index;
        /* the AudioSpecificConfig a raw frame needs: AAC LC (2), the rate's index, the channels */
        extra[0] = (unsigned char)(2 << 3 | index >> 1), extra[1] = (unsigned char)((index & 1) << 7 | (channels & 15) << 3);
        d->codec->extradata = extra, d->codec->extradata_size = 2;
        d->channels = channels, d->frame_samples = 1024;
        most_es = AAC_MOST + (vita_read32(p, info + 4) ? 0x100 : 0);
        most_pcm = channels * 2048 * (vita_read32(p, info + 16) ? 2 : 1);
    } else { /* SceAudiodecInfoAt9: size, configData[4], ch, bitRate, samplingRate, superFrameSize, frames */
        const unsigned char* config = vita_ptr(p, info + 4);
        extra[0] = 2; /* FFmpeg's extradata: a version, then the configuration */
        memcpy(extra + 4, config, 4);
        uint32_t bits = snx_ld_be32(config);
        /* the configuration word: 0xfe, the sample rate's index (4 bits), the channels' layout (3), a zero bit, the
           frame's bytes less one (11), the superframe's frames as a power of two (2) */
        d->frame_bytes = ((bits >> 5) & 0x7ff) + 1;
        uint32_t frames = 1u << ((bits >> 3) & 3);
        d->frame_samples = 1u << at9_frame_log2[(bits >> 20) & 15];
        d->codec->extradata = extra, d->codec->extradata_size = 12;
        d->codec->block_align = (int)(d->frame_bytes * frames);
        if (avcodec_open2(d->codec, codec, NULL) < 0) {
            release(d);
            return SCE_AUDIODEC_ERROR_API_FAIL;
        }
        d->channels = (uint32_t)d->codec->ch_layout.nb_channels;
        d->superframe_frames = frames;
        if (frames > 1 && !(d->superframe_pcm = malloc((size_t)frames * d->frame_samples * d->channels * 2))) {
            release(d);
            return SCE_AUDIODEC_ERROR_OUT_OF_MEMORY;
        }
        uint32_t rate = (uint32_t)d->codec->sample_rate, superframe = d->frame_bytes * frames;
        vita_write32(p, info + 8, d->channels);
        vita_write32(p, info + 12, (uint32_t)((uint64_t)superframe * 8 * rate / (d->frame_samples * frames)));
        vita_write32(p, info + 16, rate);
        vita_write32(p, info + 20, superframe);
        vita_write32(p, info + 24, frames);
        most_es = superframe < AT9_MOST ? superframe : AT9_MOST;
        most_pcm = d->frame_samples * d->channels * 2;
    }
    if (type == TYPE_AAC && avcodec_open2(d->codec, codec, NULL) < 0) {
        release(d);
        return SCE_AUDIODEC_ERROR_API_FAIL;
    }
    vita_write32(p, ctrl + 4, (uint32_t)(d - s->decoders) + 1);
    vita_write32(p, ctrl + 0x10, most_es), vita_write32(p, ctrl + 0x1c, most_pcm);
    return 0;
}
/* sceAudiodecCreateDecoder(ctrl, type), and its External form (ctrl, type, memory, size), whose memory is unused */
uint32_t vita_audiodec_create_decoder(VitaProcess* p, VitaThread* t, uint32_t ctrl, uint32_t type) {
    (void)t;
    return create(p, ctrl, type);
}
uint32_t vita_audiodec_delete_decoder(VitaProcess* p, VitaThread* t, uint32_t ctrl) {
    (void)t;
    Decoder* d = decoder_of(p, ctrl);
    if (!d) return SCE_AUDIODEC_ERROR_INVALID_HANDLE;
    release(d);
    vita_write32(p, ctrl + 4, 0);
    return 0;
}
/* samples [first, first + count) of FFmpeg's frame (planar or packed, float or 16-bit) as interleaved 16-bit ones at out */
static void interleave(const AVFrame* f, uint32_t first, uint32_t count, unsigned char* out) {
    uint32_t channels = (uint32_t)f->ch_layout.nb_channels;
    for (uint32_t i = first; i < first + count; ++i)
        for (uint32_t c = 0; c < channels; ++c) {
            int v;
            switch (f->format) {
                case AV_SAMPLE_FMT_FLTP: v = (int)lrintf(((const float*)f->extended_data[c])[i] * 32767.0f); break;
                case AV_SAMPLE_FMT_FLT: v = (int)lrintf(((const float*)f->data[0])[i * channels + c] * 32767.0f); break;
                case AV_SAMPLE_FMT_S16P: v = ((const int16_t*)f->extended_data[c])[i]; break;
                default: v = ((const int16_t*)f->data[0])[i * channels + c]; break;
            }
            v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
            snx_st_le16(out, (uint16_t)v), out += 2;
        }
}
/* a packet of size bytes at es through FFmpeg's decoder into d->frame: false when it can't be decoded */
static bool decode_packet(VitaProcess* p, Decoder* d, uint32_t es, uint32_t size) {
    if (!vita_mapped(p, es, size) || av_new_packet(d->packet, (int)size) < 0) return false;
    memcpy(d->packet->data, vita_ptr(p, es), size);
    int r = avcodec_send_packet(d->codec, d->packet);
    av_packet_unref(d->packet);
    return r >= 0 && avcodec_receive_frame(d->codec, d->frame) >= 0;
}
/* one frame from es into pcm: the bytes of each, false when it can't be decoded */
static bool decode_frame(VitaProcess* p, Decoder* d, uint32_t es, uint32_t pcm, uint32_t most_es, uint32_t* used,
                         uint32_t* given) {
    if (d->superframe_pcm) { /* an ATRAC9 superframe of several frames: decoded at its first */
        uint32_t frame_pcm = d->frame_samples * d->channels * 2, superframe = d->frame_bytes * d->superframe_frames;
        *used = 0;
        if (!d->superframe_index) {
            if (!decode_packet(p, d, es, superframe)) return false;
            AVFrame* f = d->frame;
            bool whole = (uint32_t)f->nb_samples == d->frame_samples * d->superframe_frames &&
                         (uint32_t)f->ch_layout.nb_channels == d->channels;
            if (whole) interleave(f, 0, (uint32_t)f->nb_samples, d->superframe_pcm);
            av_frame_unref(f);
            if (!whole) return false;
            *used = superframe;
        }
        if (!vita_mapped(p, pcm, frame_pcm)) return false;
        memcpy(vita_ptr(p, pcm), d->superframe_pcm + d->superframe_index * frame_pcm, frame_pcm);
        d->superframe_index = (d->superframe_index + 1) % d->superframe_frames;
        *given = frame_pcm;
        return true;
    }
    uint32_t size = d->type == TYPE_AT9 ? d->frame_bytes : vita_mp4_unit_size(p, es);
    if (!size || size > most_es) size = most_es;
    if (!decode_packet(p, d, es, size)) return false;
    AVFrame* f = d->frame;
    uint32_t channels = (uint32_t)f->ch_layout.nb_channels, samples = (uint32_t)f->nb_samples;
    if (!vita_mapped(p, pcm, samples * channels * 2)) {
        av_frame_unref(f);
        return false;
    }
    interleave(f, 0, samples, vita_ptr(p, pcm));
    *used = size, *given = samples * channels * 2;
    av_frame_unref(f);
    return true;
}
/* sceAudiodecDecodeNFrames(ctrl, n): from pEs (0x8) into pPcm (0x14), the bytes each took into inputEsSize (0xc) and
   outputPcmSize (0x18) */
static uint32_t decode_frames(VitaProcess* p, uint32_t ctrl, uint32_t frames) {
    if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    Decoder* d = decoder_of(p, ctrl);
    if (!d) return SCE_AUDIODEC_ERROR_INVALID_HANDLE;
    uint32_t es = vita_read32(p, ctrl + 8), pcm = vita_read32(p, ctrl + 0x14), most = vita_read32(p, ctrl + 0x10);
    uint32_t used = 0, given = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        uint32_t u, g;
        if (!decode_frame(p, d, es + used, pcm + given, most, &u, &g)) {
            vita_write32(p, ctrl + 0xc, used), vita_write32(p, ctrl + 0x18, given);
            return SCE_AUDIODEC_ERROR_API_FAIL;
        }
        used += u, given += g;
    }
    vita_write32(p, ctrl + 0xc, used), vita_write32(p, ctrl + 0x18, given);
    return 0;
}
uint32_t vita_audiodec_decode(VitaProcess* p, VitaThread* t, uint32_t ctrl) {
    (void)t;
    return decode_frames(p, ctrl, 1);
}
uint32_t vita_audiodec_decode_n_frames(VitaProcess* p, VitaThread* t, uint32_t ctrl, uint32_t frames) {
    (void)t;
    return decode_frames(p, ctrl, frames);
}
/* sceAudiodecClearContext(ctrl): the decoder as new, before a seek */
uint32_t vita_audiodec_clear_context(VitaProcess* p, VitaThread* t, uint32_t ctrl) {
    (void)t;
    Decoder* d = decoder_of(p, ctrl);
    if (!d) return SCE_AUDIODEC_ERROR_INVALID_HANDLE;
    avcodec_flush_buffers(d->codec);
    d->superframe_index = 0;
    return 0;
}

/* ---- SceCodecEngine: memory the decoders' callers hand them, carved from blocks the caller allocated ---- */
/* sceCodecEngineOpenUnmapMemBlock(base, size): the block's id */
uint32_t vita_audiodec_open_block(VitaProcess* p, VitaThread* t, uint32_t base, uint32_t size) {
    (void)t;
    struct VitaAudiodec* s = state(p);
    for (unsigned i = 0; s && i < BLOCKS; ++i)
        if (!s->blocks[i].size) {
            s->blocks[i].base = base, s->blocks[i].size = size, s->blocks[i].used = 0;
            return s->blocks[i].size ? i + 1 : 0x80000000u;
        }
    return 0x80000000u;
}
/* sceCodecEngineAllocMemoryFromUnmapMemBlock(id, size, alignment): its address, 0 without room */
uint32_t vita_audiodec_alloc_from_block(VitaProcess* p, VitaThread* t, uint32_t id, uint32_t size, uint32_t align) {
    (void)t;
    struct VitaAudiodec* s = p->audiodec;
    if (!s || id < 1 || id > BLOCKS || !s->blocks[id - 1].size || (align & (align - 1))) return 0;
    if (!align) align = 1;
    uint64_t at = ((uint64_t)s->blocks[id - 1].base + s->blocks[id - 1].used + align - 1) & ~(uint64_t)(align - 1);
    if (at + size > (uint64_t)s->blocks[id - 1].base + s->blocks[id - 1].size) return 0;
    s->blocks[id - 1].used = (uint32_t)(at + size - s->blocks[id - 1].base);
    return (uint32_t)at;
}
uint32_t vita_audiodec_close_block(VitaProcess* p, VitaThread* t, uint32_t id) {
    (void)t;
    if (!p->audiodec || id < 1 || id > BLOCKS) return 0x80000000u;
    memset(&p->audiodec->blocks[id - 1], 0, sizeof p->audiodec->blocks[id - 1]);
    return 0;
}

/* ---- save states (vita/runtime/state.c) ----
   Each decoder as the game made it: its kind and sizes, FFmpeg's configuration of it (the AudioSpecificConfig or
   ATRAC9's), and an ATRAC9 superframe decoded and not all handed out yet; then the codec engine's blocks. A load makes
   each decoder again from its configuration: what FFmpeg's decoder keeps of the frames before (the overlap of their
   transforms) starts empty, so the first frame decoded after a load differs from the console's, the ones after it
   don't */
#define EXTRA_MOST 12u
typedef struct {
    uint32_t used, type, channels, frame_bytes, frame_samples, superframe_frames, superframe_index;
    uint32_t extra_size, block_align;
    unsigned char extra[EXTRA_MOST];
} Saved;
typedef struct {
    Decoder decoders[DECODERS];
    uint32_t blocks[BLOCKS][3];
} AudiodecStaged;

uint64_t vita_audiodec_state_schema(void) {
    static const Saved v; /* only walked */
    uint64_t h = SNX_FNV_BASIS ^ sizeof v ^ (uint64_t)DECODERS << 32 ^ (uint64_t)BLOCKS << 40;
    __builtin_dump_struct(&v, snx_state_schema_out, &h);
    return h;
}
static uint32_t superframe_bytes(const Decoder* d) {
    return d->superframe_pcm ? d->superframe_frames * d->frame_samples * d->channels * 2 : 0;
}
int vita_audiodec_state_save(VitaProcess* p, StateIo* io) {
    struct VitaAudiodec* s = p->audiodec;
    for (unsigned i = 0; i < DECODERS; ++i) {
        const Decoder* d = s ? &s->decoders[i] : NULL;
        Saved v;
        memset(&v, 0, sizeof v); /* its padding is the file's */
        if (d && d->used) {
            v.used = 1, v.type = d->type, v.channels = d->channels, v.frame_bytes = d->frame_bytes;
            v.frame_samples = d->frame_samples, v.superframe_frames = d->superframe_frames;
            v.superframe_index = d->superframe_index;
            v.extra_size = d->codec->extradata_size > 0 && (uint32_t)d->codec->extradata_size <= EXTRA_MOST
                               ? (uint32_t)d->codec->extradata_size
                               : 0;
            v.block_align = (uint32_t)d->codec->block_align;
            if (v.extra_size) memcpy(v.extra, d->codec->extradata, v.extra_size);
        }
        if (io_put(io, &v, sizeof v) ||
            (v.used && superframe_bytes(d) && io_put(io, d->superframe_pcm, superframe_bytes(d))))
            return -1;
    }
    for (unsigned i = 0; i < BLOCKS; ++i) {
        uint32_t b[3] = {s ? s->blocks[i].base : 0, s ? s->blocks[i].size : 0, s ? s->blocks[i].used : 0};
        if (io_put(io, b, sizeof b)) return -1;
    }
    return 0;
}
void vita_audiodec_state_drop(void* staged) {
    AudiodecStaged* s = staged;
    for (unsigned i = 0; s && i < DECODERS; ++i)
        if (s->decoders[i].used) release(&s->decoders[i]);
    free(s);
}
void* vita_audiodec_state_parse(StateIo* io, char* why, size_t whysize) {
    AudiodecStaged* s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(why, whysize, "out of memory for the state's audio decoders (%zu bytes)", sizeof *s);
        return NULL;
    }
    for (unsigned i = 0; i < DECODERS; ++i) {
        Saved v;
        if (io_get(io, &v, sizeof v)) goto damaged;
        if (!v.used) continue;
        Decoder* d = &s->decoders[i];
        if ((v.type != TYPE_AAC && v.type != TYPE_AT9) || v.extra_size > EXTRA_MOST || !v.channels || v.channels > 8 ||
            v.frame_samples > 4096 || v.superframe_frames > 4 ||
            (v.superframe_frames && v.superframe_index >= v.superframe_frames)) {
            snprintf(why, whysize,
                     "its audio decoder %u is damaged: type 0x%x, %u channels, %u samples a frame, %u "
                     "frames a superframe from %u",
                     i + 1, v.type, v.channels, v.frame_samples, v.superframe_frames, v.superframe_index);
            goto refused;
        }
        const AVCodec* codec = avcodec_find_decoder(v.type == TYPE_AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_ATRAC9);
        d->used = true, d->type = v.type, d->channels = v.channels, d->frame_bytes = v.frame_bytes;
        d->frame_samples = v.frame_samples, d->superframe_frames = v.superframe_frames;
        d->superframe_index = v.superframe_index;
        d->codec = codec ? avcodec_alloc_context3(codec) : NULL;
        d->packet = av_packet_alloc(), d->frame = av_frame_alloc();
        unsigned char* extra = av_mallocz(EXTRA_MOST + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!d->codec || !d->packet || !d->frame || !extra) {
            av_free(extra);
            snprintf(why, whysize, "its audio decoder %u (type 0x%x) can't be made again: %s", i + 1, v.type,
                     codec ? "out of memory" : "FFmpeg has no such decoder");
            goto refused;
        }
        memcpy(extra, v.extra, v.extra_size);
        d->codec->extradata = extra, d->codec->extradata_size = (int)v.extra_size;
        d->codec->block_align = (int)v.block_align;
        if (avcodec_open2(d->codec, codec, NULL) < 0) {
            snprintf(why, whysize, "its audio decoder %u (type 0x%x) can't be opened again with its configuration",
                     i + 1, v.type);
            goto refused;
        }
        if (v.type == TYPE_AT9 && v.superframe_frames > 1) {
            if (!(d->superframe_pcm = malloc(superframe_bytes(d) ? superframe_bytes(d) : 1)) ||
                io_get(io, d->superframe_pcm, superframe_bytes(d)))
                goto damaged;
        }
    }
    for (unsigned i = 0; i < BLOCKS; ++i)
        if (io_get(io, s->blocks[i], sizeof s->blocks[i])) goto damaged;
    return s;
damaged:
    snprintf(why, whysize, "its audio decoders' section is damaged or cut short");
refused:
    vita_audiodec_state_drop(s);
    return NULL;
}
void vita_audiodec_state_install(VitaProcess* p, void* staged) {
    AudiodecStaged* s = staged;
    bool any = false;
    for (unsigned i = 0; i < DECODERS; ++i) any = any || s->decoders[i].used;
    for (unsigned i = 0; i < BLOCKS; ++i) any = any || s->blocks[i][1];
    vita_audiodec_free(p);
    struct VitaAudiodec* a = any ? state(p) : NULL;
    if (a) {
        memcpy(a->decoders, s->decoders, sizeof a->decoders); /* the decoders' FFmpeg objects go with them */
        for (unsigned i = 0; i < BLOCKS; ++i)
            a->blocks[i].base = s->blocks[i][0], a->blocks[i].size = s->blocks[i][1],
            a->blocks[i].used = s->blocks[i][2];
        free(s);
    } else
        vita_audiodec_state_drop(s);
}
