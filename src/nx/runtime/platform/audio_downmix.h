/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's 5.1 PCM mixed to stereo for a host output that has no more than two channels */
#ifndef SNX_AUDIO_DOWNMIX_H
#define SNX_AUDIO_DOWNMIX_H
#include <stdint.h>

/* the console's default downmix, the one audren's device sink uses: 1 for the fronts, 0.707 for the centre and the
   backs, 0.251 for the LFE, in 1/32768ths */
#define SNX_AUDIO_DOWNMIX_CENTRE 23167
#define SNX_AUDIO_DOWNMIX_LFE    8225
#define SNX_AUDIO_DOWNMIX_BACK   23167

/* frames of front left, front right, centre, LFE, back left, back right (signed 16-bit, little-endian, interleaved)
   to as many of left, right. The sums saturate: a game's 5.1 mix leaves the headroom its own peaks need, and scaling
   every sample down for the worst case (all six at full scale, 2.665) would play the whole game 8.5 dB lower */
void snx_audio_downmix_51(void* stereo, const void* surround, uint32_t frames);
#endif
