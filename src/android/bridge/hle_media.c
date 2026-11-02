/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libmediandk (AMediaCodec, AMediaExtractor, AMediaFormat) as HLE: its format keys are objects, each a pointer to the
   key's string, whose values are Android's. The codecs are not there yet (FFmpeg is to come): a codec that is asked
   for is not found, as on a device without it */
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"

#define KEYS(F)                                                                                                        \
    F(MIME, "mime") F(SAMPLE_RATE, "sample-rate") F(CHANNEL_COUNT, "channel-count") F(IS_ADTS, "is-adts")             \
    F(WIDTH, "width") F(HEIGHT, "height") F(DURATION, "durationUs") F(MAX_INPUT_SIZE, "max-input-size")               \
    F(BIT_RATE, "bitrate") F(COLOR_FORMAT, "color-format") F(FRAME_RATE, "frame-rate") F(STRIDE, "stride")            \
    F(CSD_0, "csd-0") F(CSD_1, "csd-1") F(AAC_PROFILE, "aac-profile") F(LANGUAGE, "language")
#define KEY_OBJECT(name, text) static const char* const key_##name = text;
KEYS(KEY_OBJECT)
#define KEY_ENTRY(name, text) {"AMEDIAFORMAT_KEY_" #name, NULL, (void*)&key_##name},

/* AMediaCodec_createDecoderByType and kin: NULL, no codec of the type */
static void no_codec(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}

const HleSymbol hle_media[] = {
    KEYS(KEY_ENTRY)
    {"AMediaCodec_createDecoderByType", no_codec, NULL},
    {"AMediaCodec_createEncoderByType", no_codec, NULL},
    {"AMediaCodec_createCodecByName", no_codec, NULL},
    {"AMediaExtractor_new", no_codec, NULL},
    {"", NULL, NULL},
};
