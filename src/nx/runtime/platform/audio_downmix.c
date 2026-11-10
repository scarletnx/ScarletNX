/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/audio_downmix.h"
#include "common/utilities/endian.h"

static int16_t downmix_sample(int32_t front, int32_t shared, int32_t back) {
    int32_t sum = shared + back * SNX_AUDIO_DOWNMIX_BACK; /* under 2^31: 32768 * (23167 + 8225 + 23167) */
    sum = (sum < 0 ? sum - 16384 : sum + 16384) / 32768 + front;
    return (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
}

void snx_audio_downmix_51(void* stereo, const void* surround, uint32_t frames) {
    const unsigned char* in = surround;
    unsigned char* out = stereo;
    uint32_t i;
    for (i = 0; i < frames; i++, in += 12, out += 4) {
        int16_t s[6];
        int32_t shared;
        int c;
        for (c = 0; c < 6; c++) s[c] = (int16_t)snx_ld_le16(in + c * 2);
        shared = s[2] * SNX_AUDIO_DOWNMIX_CENTRE + s[3] * SNX_AUDIO_DOWNMIX_LFE;
        snx_st_le16(out, (uint16_t)downmix_sample(s[0], shared, s[4]));
        snx_st_le16(out + 2, (uint16_t)downmix_sample(s[1], shared, s[5]));
    }
}
