/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* CSND's frames mixed into the DSP's, as Azahar's DspInterface mixes them (audio_core/dsp_interface.cpp at
   azahar-emu/azahar 6e42de8: OutputCsndFrame and MixCsnd) */
#include "3ds/audio/output.h"
#include <string.h>

#define PENDING (SNX_CTR_DSP_SAMPLES * 4u) /* Azahar's MaxCsndPendingSamples */

static struct {
    int16_t pending[PENDING][2]; /* the CSND samples not mixed yet, oldest first: count of them */
    uint32_t count, dsp_since_csnd, csnd_without_dsp;
    bool alone; /* CSND's own frame going out, not the DSP's */
} g_mix;

static int16_t clamp16(int32_t v) { return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v); }
void snx_ctr_audio_mix_csnd(const int16_t frame[SNX_CTR_DSP_SAMPLES][2], int16_t out[SNX_CTR_DSP_SAMPLES][2]) {
    uint32_t n = g_mix.count < SNX_CTR_DSP_SAMPLES ? g_mix.count : SNX_CTR_DSP_SAMPLES;
    for (unsigned i = 0; i < SNX_CTR_DSP_SAMPLES; ++i)
        for (unsigned side = 0; side < 2; ++side)
            out[i][side] = i < n ? clamp16(frame[i][side] + g_mix.pending[i][side]) : frame[i][side];
    memmove(g_mix.pending, g_mix.pending + n, (g_mix.count - n) * sizeof *g_mix.pending);
    g_mix.count -= n;
    if (!g_mix.alone) g_mix.dsp_since_csnd += SNX_CTR_DSP_SAMPLES;
}
void snx_ctr_audio_csnd_frame(const int16_t frame[SNX_CTR_DSP_SAMPLES][2]) {
    g_mix.csnd_without_dsp = g_mix.dsp_since_csnd ? 0 : g_mix.csnd_without_dsp + 1;
    g_mix.dsp_since_csnd = 0;
    /* the oldest go first when the two clocks drift apart */
    if (g_mix.count + SNX_CTR_DSP_SAMPLES > PENDING) {
        uint32_t drop = g_mix.count + SNX_CTR_DSP_SAMPLES - PENDING;
        memmove(g_mix.pending, g_mix.pending + drop, (g_mix.count - drop) * sizeof *g_mix.pending);
        g_mix.count -= drop;
    }
    memcpy(g_mix.pending + g_mix.count, frame, SNX_CTR_DSP_SAMPLES * sizeof *g_mix.pending);
    g_mix.count += SNX_CTR_DSP_SAMPLES;
    if (g_mix.csnd_without_dsp < 2) return;
    /* the DSP is idle: CSND plays alone, its samples mixed into silence */
    static const int16_t silence[SNX_CTR_DSP_SAMPLES][2];
    g_mix.alone = true;
    while (g_mix.count) snx_ctr_audio_frame(silence);
    g_mix.alone = false;
}
void snx_ctr_audio_mix_reset(void) { memset(&g_mix, 0, sizeof g_mix); }
