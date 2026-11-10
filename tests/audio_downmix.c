/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's 5.1 PCM mixed to stereo (runtime/platform/audio_downmix.c): each channel alone, sample by sample, the
   saturation at both ends, and the bytes' order */
#include "nx/runtime/platform/audio_downmix.h"
#include <assert.h>
#include <string.h>

/* one frame of front left, front right, centre, LFE, back left, back right in; left and right out */
static void mix(int fl, int fr, int fc, int lfe, int bl, int br, int* l, int* r) {
    const int in[6] = {fl, fr, fc, lfe, bl, br};
    unsigned char surround[12], stereo[4];
    int c;
    for (c = 0; c < 6; c++) {
        surround[c * 2] = (unsigned char)(in[c] & 0xFF);
        surround[c * 2 + 1] = (unsigned char)((in[c] >> 8) & 0xFF);
    }
    snx_audio_downmix_51(stereo, surround, 1);
    *l = (int16_t)(stereo[0] | stereo[1] << 8);
    *r = (int16_t)(stereo[2] | stereo[3] << 8);
}

int main(void) {
    int l, r;
    /* the fronts pass as they are, each to its own side */
    mix(-12345, 321, 0, 0, 0, 0, &l, &r);
    assert(l == -12345 && r == 321);
    /* the centre alone, the voices: 0.707 of it on both sides */
    mix(0, 0, 10000, 0, 0, 0, &l, &r);
    assert(l == 7070 && r == 7070);
    mix(0, 0, -10000, 0, 0, 0, &l, &r);
    assert(l == -7070 && r == -7070);
    /* a back channel alone: 0.707 of it on its side, nothing on the other */
    mix(0, 0, 0, 0, 10000, 0, &l, &r);
    assert(l == 7070 && r == 0);
    mix(0, 0, 0, 0, 0, -10000, &l, &r);
    assert(l == 0 && r == -7070);
    /* the LFE alone: 0.251 of it on both sides */
    mix(0, 0, 0, 10000, 0, 0, &l, &r);
    assert(l == 2510 && r == 2510);
    /* every channel at once is their sum */
    mix(1000, -2000, 10000, 10000, 10000, -10000, &l, &r);
    assert(l == 1000 + 7070 + 2510 + 7070 && r == -2000 + 7070 + 2510 - 7070);
    /* a sum past 16 bits stays at the end it passed, never wrapping to the other */
    mix(30000, -30000, 30000, 0, 0, -32768, &l, &r);
    assert(l == 32767 && r == -30000 + 21210 - 23167);
    mix(32767, 32767, 32767, 32767, 32767, 32767, &l, &r);
    assert(l == 32767 && r == 32767);
    mix(-32768, -32768, -32768, -32768, -32768, -32768, &l, &r);
    assert(l == -32768 && r == -32768);
    {
        /* only the frames asked for are read and written, in order */
        unsigned char surround[3 * 12], stereo[3 * 4];
        static const unsigned char want[8] = {0x34, 0x12, 0, 0, 0, 0, 0xCC, 0xED}; /* 0x1234, 0; 0, -0x1234 */
        memset(surround, 0, sizeof surround);
        surround[0] = 0x34, surround[1] = 0x12;           /* frame 0: front left 0x1234 */
        surround[12 + 2] = 0xCC, surround[12 + 3] = 0xED; /* frame 1: front right -0x1234 */
        memset(surround + 24, 0x7F, 12);                  /* frame 2: not asked for */
        memset(stereo, 0xAA, sizeof stereo);
        snx_audio_downmix_51(stereo, surround, 2);
        assert(!memcmp(stereo, want, 8));
        assert(stereo[8] == 0xAA && stereo[9] == 0xAA && stereo[10] == 0xAA && stereo[11] == 0xAA);
    }
    return 0;
}
