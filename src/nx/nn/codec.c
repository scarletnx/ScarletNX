/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/audio/hardware_opus_decoder_manager.cpp, hardware_opus_decoder.cpp,
   errors.h; src/audio_core/opus/decoder.cpp, decoder_manager.cpp):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018-2024 yuzu Emulator Project (GPL-2.0-or-later) */
/* nn::codec's hardware Opus decoder, native (codec.hook), with no hwopus session: the functions that open a decoder,
   decode with it, close it and size its work buffer. The decoding itself is hle/services/audio.c's, on FFmpeg, as
   the service's port had it: a packet behind its 8 byte header in, exactly its duration of 16-bit samples out.

   The SDK keeps a decoder's rate, channels and stream counts in the game's own object and reads them there
   (GetHardwareOpusDecoderSampleRate and the like, which stay its own code), so the open fills them as the SDK does.
   The object is one of three shapes, each the same in every SDK read that has it:
   - the HardwareOpusDecoder class up to SDK 4 (0.12.14 to 4.5.0): a flag that it is initialized at 8, the rate at
     0xc, the channels at 0x10 and, in the multistream class, the stream counts at 0x14 and 0x18;
   - the HardwareOpusDecoderType struct from SDK 5 (5.4.180 to 22.2.8): the session's object at 0, the
     rate at 0x18, the channels at 0x1c; from SDK 6 the last decode's time in microseconds at 0x20 and, at 0x28, the
     flag the SDK's own ResetHardwareOpusDecoder sets for the next decode to start the stream again. The multistream
     struct has the stream counts at 0x20 and 0x24, and the time and the flag 8 bytes later;
   - the class from SDK 5 to 14, which is that struct behind its vtable.
   A decoder is found by the address of that object (g_hle.audio.opus, which a save state holds), and the word where
   the SDK keeps its session's object holds the decoder's place in the pool, so the SDK's own IsInitialized says yes.

   The results are the SDK's own OpusResult, as its code gives them: 6 for a packet shorter than its header says, 7
   for an output with no room for the packet's samples, 4 for a packet the decoder refuses, and 8 from an open when
   every decoder is taken (the firmware has 24), which sends a game to the SDK's software decoder */
#include "nx/nn/codec.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/services_int.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/time.h"

#define OPUS_BROKEN_DATA     4u /* nn::codec::OpusResult */
#define OPUS_SHORT_INPUT     6u
#define OPUS_SHORT_OUTPUT    7u
#define OPUS_OUT_OF_RESOURCE 8u
#define OPUS_HEADER          8u /* a packet's: its size and the encoder's final range, big-endian */

/* where an object keeps what the SDK's own code reads; 0 for a field the shape doesn't have */
typedef struct {
    u8 object; /* the session's object (8 bytes), or the initialized flag (1 byte) of the class up to SDK 4 */
    u8 flag;   /* object is that flag */
    u8 rate, channels, streams, stereo_streams;
    u8 time, reset; /* from SDK 6 */
} Shape;
static const Shape g_class = {8, 1, 0xc, 0x10, 0, 0, 0, 0}, g_class_multi = {8, 1, 0xc, 0x10, 0x14, 0x18, 0, 0};
static const Shape g_type = {0, 0, 0x18, 0x1c, 0, 0, 0, 0}, g_type_multi = {0, 0, 0x18, 0x1c, 0x20, 0x24, 0, 0};
static const Shape g_timed = {0, 0, 0x18, 0x1c, 0, 0, 0x20, 0x28},
                   g_timed_multi = {0, 0, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x30};
#define CLASS_TYPE 8 /* the struct in the class from SDK 5: behind the vtable */

/* the decoder of the game's object at guest, under g_host.audio.lk, which the caller holds from here whether there is
   one or not */
static HwOpus* decoder_lock(u64 guest) {
    u32 i;
    pm_lock(&g_host.audio.lk);
    for (i = 0; guest && i < HWOPUS_POOL; i++)
        if (g_hle.audio.opus[i].guest == guest) return &g_hle.audio.opus[i];
    return NULL;
}

/* ------------------------------------------------------------ opening and closing */
static u32 open_decoder(CPU* c, const Shape* s, u64 guest, u32 rate, u32 channels, u32 streams, u32 stereo_streams,
                        const u8* mapping) {
    HwOpus* h = decoder_lock(guest);
    u32 place;
    if (h) { /* initialized twice: the SDK opens another session over the first, which is the one a decode then uses */
        hwopus_free(h);
        POOL_PUT(h);
    }
    h = POOL_TRY(g_hle.audio.opus, g_hle.audio.opus_next);
    if (!h) {
        pm_unlock(&g_host.audio.lk);
        WARN_LIMIT(4, "hwopus: a decoder asked for with all %d open: the SDK's result for none left (%u)\n",
                   SNX_HWOPUS_MAX_DECODERS, OPUS_OUT_OF_RESOURCE);
        return OPUS_OUT_OF_RESOURCE;
    }
    h->guest = guest; /* taken: the open runs outside the lock, as FFmpeg's may take long */
    place = POOL_INDEX(g_hle.audio.opus, h) + 1;
    pm_unlock(&g_host.audio.lk);
    if (!hwopus_open(h, rate, channels, streams, stereo_streams, mapping)) {
        WARN_LIMIT(4, "hwopus: can't open a decoder of %lu channels at %lu Hz: the SDK's result for none left (%u)\n",
                   (unsigned long)channels, (unsigned long)rate, OPUS_OUT_OF_RESOURCE);
        pm_lock(&g_host.audio.lk);
        hwopus_free(h);
        POOL_PUT(h);
        pm_unlock(&g_host.audio.lk);
        return OPUS_OUT_OF_RESOURCE;
    }
    snx_st_le32(hook_host(c, guest + s->rate), rate);
    snx_st_le32(hook_host(c, guest + s->channels), channels);
    if (s->streams) {
        snx_st_le32(hook_host(c, guest + s->streams), streams);
        snx_st_le32(hook_host(c, guest + s->stereo_streams), stereo_streams);
    }
    if (s->time) {
        snx_st_le64(hook_host(c, guest + s->time), 0);
        *hook_host(c, guest + s->reset) = 0;
    }
    if (s->flag)
        *hook_host(c, guest + s->object) = 1;
    else
        snx_st_le64(hook_host(c, guest + s->object), place);
    LOG("hwopus: decoder %llx: %lu channels in %lu streams at %lu Hz\n", (unsigned long long)guest,
        (unsigned long)channels, (unsigned long)streams, (unsigned long)rate);
    return 0;
}
/* a mapping is a byte a channel; one that can't be read opens no decoder */
static u32 open_multi(CPU* c, const Shape* s, u64 guest, i32 rate, i32 channels, i32 streams, i32 stereo_streams,
                      u64 mapping) {
    if (channels < 1 || channels > 255 || !snx_audio_guest_ok(mapping, (u64)channels, PERM_R)) {
        WARN_LIMIT(4,
                   "hwopus: a multistream decoder of %ld channels with its mapping at %#llx: the SDK's result for "
                   "none left (%u)\n",
                   (long)channels, (unsigned long long)mapping, OPUS_OUT_OF_RESOURCE);
        return OPUS_OUT_OF_RESOURCE;
    }
    return open_decoder(c, s, guest, (u32)rate, (u32)channels, (u32)streams, (u32)stereo_streams,
                        hook_host(c, mapping));
}
/* the SDK lets its session go and clears what it kept: an object that was never opened has nothing to let go */
static void close_decoder(CPU* c, const Shape* s, u64 guest) {
    HwOpus* h = decoder_lock(guest);
    if (h) {
        hwopus_free(h);
        POOL_PUT(h);
    }
    pm_unlock(&g_host.audio.lk);
    if (!h) return;
    if (s->flag)
        *hook_host(c, guest + s->object) = 0;
    else { /* the struct's first four words, the rate and the channels among them */
        memset(hook_host(c, guest), 0, 0x20);
        if (s->streams) snx_st_le64(hook_host(c, guest + s->streams), 0);
    }
}

u32 nncodec_hwopus_class_init(CPU* c, u64 self, i32 rate, i32 channels, u64 work, u64 size) {
    (void)work, (void)size; /* the decoder's state is the runtime's: the game's work buffer stays untouched */
    return open_decoder(c, &g_class, self, (u32)rate, (u32)channels, 1, (u32)channels - 1, NULL);
}
u32 nncodec_hwopus_class_multi_init(CPU* c, u64 self, i32 rate, i32 channels, i32 streams, i32 stereo_streams,
                                    u64 mapping, u64 work, u64 size) {
    (void)work, (void)size;
    return open_multi(c, &g_class_multi, self, rate, channels, streams, stereo_streams, mapping);
}
u32 nncodec_hwopus_init(CPU* c, u64 decoder, i32 rate, i32 channels, u64 work, u64 size, i32 option) {
    (void)work, (void)size, (void)option;
    return open_decoder(c, &g_type, decoder, (u32)rate, (u32)channels, 1, (u32)channels - 1, NULL);
}
u32 nncodec_hwopus_init_timed(CPU* c, u64 decoder, i32 rate, i32 channels, u64 work, u64 size, i32 option) {
    (void)work, (void)size, (void)option;
    return open_decoder(c, &g_timed, decoder, (u32)rate, (u32)channels, 1, (u32)channels - 1, NULL);
}
u32 nncodec_hwopus_multi_init(CPU* c, u64 decoder, i32 rate, i32 channels, i32 streams, i32 stereo_streams, u64 mapping,
                              u64 work, u64 size) {
    (void)work, (void)size;
    return open_multi(c, &g_type_multi, decoder, rate, channels, streams, stereo_streams, mapping);
}
u32 nncodec_hwopus_multi_init_timed(CPU* c, u64 decoder, i32 rate, i32 channels, i32 streams, i32 stereo_streams,
                                    u64 mapping, u64 work, u64 size) {
    (void)work, (void)size;
    return open_multi(c, &g_timed_multi, decoder, rate, channels, streams, stereo_streams, mapping);
}

void nncodec_hwopus_class_finalize(CPU* c, u64 self) { close_decoder(c, &g_class, self); }
void nncodec_hwopus_class_multi_finalize(CPU* c, u64 self) { close_decoder(c, &g_class_multi, self); }
/* the class from SDK 5, whose Finalize is its struct's FinalizeHardwareOpusDecoder again */
void nncodec_hwopus_type_class_finalize(CPU* c, u64 self) { close_decoder(c, &g_type, self + CLASS_TYPE); }
void nncodec_hwopus_type_class_multi_finalize(CPU* c, u64 self) { close_decoder(c, &g_type_multi, self + CLASS_TYPE); }
void nncodec_hwopus_finalize(CPU* c, u64 decoder) { close_decoder(c, &g_type, decoder); }
void nncodec_hwopus_multi_finalize(CPU* c, u64 decoder) { close_decoder(c, &g_type_multi, decoder); }
/* ResetHardwareOpusDecoder, the SDK's own three instructions: the flag the next decode reads. Only a struct of SDK 6
   on has the function */
void nncodec_hwopus_reset(CPU* c, u64 decoder) { *hook_host(c, decoder + g_timed.reset) = 1; }
/* SetContext, the class's up to SDK 4: the session took the context and did nothing with it */
u32 nncodec_hwopus_class_set_context(CPU* c, u64 self, u64 context, u64 size) {
    (void)c, (void)self, (void)context, (void)size;
    return 0;
}

/* ------------------------------------------------------------ decoding */
/* room: the SDK checks the output's room itself before it asks (every SDK read but 0.12, where the session refuses
   the packet) */
static u32 decode(CPU* c, const Shape* s, u64 guest, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in,
                  u64 in_size, int room) {
    HwOpus* h;
    u64 t = plat_ns();
    u32 size, want, used = 0, got = 0;
    int reset, rc;
    if (in_size < OPUS_HEADER || !snx_audio_guest_ok(in, in_size, PERM_R)) return OPUS_SHORT_INPUT;
    size = snx_ld_be32(hook_host(c, in));
    if ((u64)size + OPUS_HEADER > in_size) return OPUS_SHORT_INPUT;
    h = decoder_lock(guest);
    pm_unlock(&g_host.audio.lk); /* a decoder is closed by the thread that decodes with it, or after it */
    if (!h) {
        WARN_LIMIT(8,
                   "hwopus: a decode on the decoder at %#llx, which isn't open: the SDK's result for a packet the "
                   "decoder refuses (%u)\n",
                   (unsigned long long)guest, OPUS_BROKEN_DATA);
        return OPUS_BROKEN_DATA;
    }
    want = hwopus_packet_samples(h, hook_host(c, in + OPUS_HEADER), size);
    if (!want) return OPUS_BROKEN_DATA;
    if ((u64)want * h->channels * 2 > out_size) return room ? OPUS_SHORT_OUTPUT : OPUS_BROKEN_DATA;
    if (!snx_audio_guest_ok(out, out_size, PERM_W)) return OPUS_BROKEN_DATA;
    reset = s->reset ? *hook_host(c, guest + s->reset) != 0 : 0;
    if (s->reset) *hook_host(c, guest + s->reset) = 0;
    rc = hwopus_decode_packet(h, hook_host(c, in), (u64)size + OPUS_HEADER, (i16*)hook_host(c, out), out_size, reset,
                              &used, &got);
    if (rc) return OPUS_BROKEN_DATA;
    LOG_LIMIT(1, "hwopus: the first packet decoded, %lu bytes into %lu samples\n", (unsigned long)used,
              (unsigned long)got);
    snx_st_le64(hook_host(c, consumed), used);
    snx_st_le32(hook_host(c, samples), got);
    if (s->time) snx_st_le64(hook_host(c, guest + s->time), (plat_ns() - t) / 1000);
    return 0;
}
u32 nncodec_hwopus_class_decode_first(CPU* c, u64 self, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in,
                                      u64 in_size) {
    return decode(c, &g_class, self, consumed, samples, out, out_size, in, in_size, 0);
}
u32 nncodec_hwopus_class_decode(CPU* c, u64 self, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in,
                                u64 in_size) {
    return decode(c, &g_class, self, consumed, samples, out, out_size, in, in_size, 1);
}
u32 nncodec_hwopus_class_multi_decode(CPU* c, u64 self, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in,
                                      u64 in_size) {
    return decode(c, &g_class_multi, self, consumed, samples, out, out_size, in, in_size, 1);
}
u32 nncodec_hwopus_decode(CPU* c, u64 decoder, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in, u64 in_size) {
    return decode(c, &g_type, decoder, consumed, samples, out, out_size, in, in_size, 1);
}
u32 nncodec_hwopus_decode_timed(CPU* c, u64 decoder, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in,
                                u64 in_size) {
    return decode(c, &g_timed, decoder, consumed, samples, out, out_size, in, in_size, 1);
}
u32 nncodec_hwopus_multi_decode(CPU* c, u64 decoder, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in,
                                u64 in_size) {
    return decode(c, &g_type_multi, decoder, consumed, samples, out, out_size, in, in_size, 1);
}
u32 nncodec_hwopus_multi_decode_timed(CPU* c, u64 decoder, u64 consumed, u64 samples, u64 out, u64 out_size, u64 in,
                                      u64 in_size) {
    return decode(c, &g_timed_multi, decoder, consumed, samples, out, out_size, in, in_size, 1);
}

/* ------------------------------------------------------------ the work buffer's size */
/* What the session answered, which a game gives its decoder as a work buffer: the session's three commands size it
   differently, and each SDK asks one of them (codec.hook says which). The SDK stops the game on a rate or a channel
   count the session refuses: here the log says so and the size is 0 */
static u64 work_size(i32 rate, i32 channels, i32 streams, i32 stereo_streams, int multi, i32 option, int dsp) {
    u32 size = hwopus_work_size((u32)rate, (u32)channels, (u32)streams, (u32)stereo_streams, multi, (u32)option, dsp);
    if (!size)
        WARN_LIMIT(4,
                   "hwopus: no work buffer size for %ld channels in %ld streams (%ld of two) at %ld Hz, which no "
                   "decoder takes: 0\n",
                   (long)channels, (long)streams, (long)stereo_streams, (long)rate);
    return size;
}
u64 nncodec_hwopus_class_work_size(CPU* c, u64 self, i32 rate, i32 channels) {
    (void)c, (void)self;
    return work_size(rate, channels, 1, 0, 0, 0, 0);
}
u64 nncodec_hwopus_work_size(CPU* c, i32 rate, i32 channels) {
    (void)c;
    return work_size(rate, channels, 1, 0, 0, 0, 0);
}
u64 nncodec_hwopus_work_size_option(CPU* c, i32 rate, i32 channels, i32 option) {
    (void)c;
    return work_size(rate, channels, 1, 0, 0, option, 0);
}
u64 nncodec_hwopus_work_size_dsp(CPU* c, i32 rate, i32 channels, i32 option) {
    (void)c;
    return work_size(rate, channels, 1, 0, 0, option, 1);
}
u64 nncodec_hwopus_class_multi_work_size(CPU* c, u64 self, i32 rate, i32 channels, i32 streams, i32 stereo_streams) {
    (void)c, (void)self;
    return work_size(rate, channels, streams, stereo_streams, 1, 0, 0);
}
/* the class of SDK 12 to 14 keeps the option it was made with at 0x40 */
u64 nncodec_hwopus_class_multi_work_size_option(CPU* c, u64 self, i32 rate, i32 channels, i32 streams,
                                                i32 stereo_streams) {
    return work_size(rate, channels, streams, stereo_streams, 1, (i32)snx_ld_le32(hook_host(c, self + 0x40)), 0);
}
u64 nncodec_hwopus_multi_work_size(CPU* c, i32 rate, i32 channels, i32 streams, i32 stereo_streams) {
    (void)c;
    return work_size(rate, channels, streams, stereo_streams, 1, 0, 0);
}
u64 nncodec_hwopus_multi_work_size_dsp(CPU* c, i32 rate, i32 channels, i32 streams, i32 stereo_streams) {
    (void)c;
    return work_size(rate, channels, streams, stereo_streams, 1, 0, 1);
}
u64 nncodec_hwopus_multi_work_size_option(CPU* c, i32 rate, i32 channels, i32 streams, i32 stereo_streams, i32 option) {
    (void)c;
    return work_size(rate, channels, streams, stereo_streams, 1, option, 0);
}
u64 nncodec_hwopus_multi_work_size_option_dsp(CPU* c, i32 rate, i32 channels, i32 streams, i32 stereo_streams,
                                              i32 option) {
    (void)c;
    return work_size(rate, channels, streams, stereo_streams, 1, option, 1);
}
