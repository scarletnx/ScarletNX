/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The DSP's mixers: the three intermediate mixes (dry, and two aux buses the application can process itself), their
   delay effects, and the downmix into the final stereo frame. Ported from Azahar audio_core/hle/{mixers,effects}.cpp
   (GPL-2.0-or-later, Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar
   a4b335e. Reverb is a stub there too, as are the limiter, the compressor and surround output (played as stereo). */
#include "3ds/audio/internal.h"
#include <stdlib.h>
#include <string.h>

#define MAX_DELAY_SAMPLES (4u * SNX_CTR_DSP_RATE)

/* a float into int32 without the undefined conversion of a value out of range */
static int32_t to32(float v) { return v <= -2147483648.0f ? INT32_MIN : v >= 2147483520.0f ? 2147483520 : (int32_t)v; }

/* ---- DelayEffect: H(z) = a z^-N / (1 - b z^-1 + a g z^-N), N = frame_count * 160 ---- */

static void delay_reset(DspDelay* d) {
    free(d->delay_output);
    *d = (DspDelay){0};
}
static void delay_resize(DspDelay* d, uint32_t samples) {
    if (samples == d->delay_samples && d->delay_output) return;
    float* output = calloc((size_t)samples * 4, sizeof *output);
    if (!output) { /* without the memory the effect stays off */
        delay_reset(d);
        return;
    }
    free(d->delay_output);
    d->delay_output = output;
    d->delay_samples = samples;
    d->delay_pos = 0;
    memset(d->last_output, 0, sizeof d->last_output);
}
static void delay_parse(DspDelay* d, DspDelayConfig* config) {
    if (!config->dirty) return;
    bool was_enabled = d->enabled;
    d->enabled = config->enable != 0;
    config->dirty = 0;
    if (!d->enabled) {
        if (was_enabled) delay_reset(d);
        return;
    }
    uint32_t samples = (uint32_t)config->frame_count * SNX_CTR_DSP_SAMPLES;
    /* a delay of nothing would read the sample about to be written: bypassed until it is known what hardware does */
    if (!samples) {
        delay_reset(d);
        return;
    }
    if (samples > MAX_DELAY_SAMPLES) samples = MAX_DELAY_SAMPLES;
    const float q7 = 1.0f / 128.0f;
    d->g = config->g * q7;
    d->a = config->a * q7;
    d->b = config->b * q7;
    /* the application's work buffer is not used: the delay line is kept here */
    delay_resize(d, samples);
}
static void delay_process(DspDelay* d, DspQuadFrame frame) {
    if (!d->enabled || !d->delay_output) return;
    for (size_t n = 0; n < SNX_CTR_DSP_SAMPLES; n++) {
        size_t pos = (d->delay_pos + n) % d->delay_samples;
        for (unsigned c = 0; c < 4; c++) {
            /* y[n] = b y[n-1] + a v[n-N], v[n] = x[n] - g y[n] */
            float x = (float)frame[n][c], *delayed = &d->delay_output[(size_t)c * d->delay_samples + pos];
            float y = d->b * d->last_output[c] + d->a * *delayed;
            *delayed = x - d->g * y;
            d->last_output[c] = y;
            frame[n][c] = to32(y);
        }
    }
    d->delay_pos = (d->delay_pos + SNX_CTR_DSP_SAMPLES) % d->delay_samples;
}
static bool delay_copy(DspDelay* to, const DspDelay* from) {
    *to = *from;
    to->delay_output = NULL;
    if (!from->delay_output) return true;
    size_t bytes = (size_t)from->delay_samples * 4 * sizeof(float);
    to->delay_output = malloc(bytes);
    if (!to->delay_output) {
        *to = (DspDelay){0};
        return false;
    }
    memcpy(to->delay_output, from->delay_output, bytes);
    return true;
}

/* ---- Mixers ---- */

static void state_free(DspMixerState* s) {
    for (unsigned i = 0; i < 2; ++i) delay_reset(&s->delay[i]);
}
static void state_init(DspMixerState* s) { *s = (DspMixerState){.output_format = DSP_OUT_STEREO}; }
void dsp_mixers_reset(DspMixers* m) {
    state_free(&m->state);
    state_init(&m->state);
    memset(m->frame, 0, sizeof m->frame);
}
void dsp_mixers_free(DspMixers* m) {
    state_free(&m->state);
    state_free(&m->backup);
}
void dsp_mixers_sleep(DspMixers* m) {
    state_free(&m->backup);
    m->backup = m->state;
    for (unsigned i = 0; i < 2; ++i) (void)delay_copy(&m->backup.delay[i], &m->state.delay[i]);
    memcpy(m->backup_frame, m->frame, sizeof m->frame);
}
void dsp_mixers_wakeup(DspMixers* m) {
    state_free(&m->state);
    m->state = m->backup;
    memcpy(m->frame, m->backup_frame, sizeof m->frame);
    state_init(&m->backup);
    memset(m->backup_frame, 0, sizeof m->backup_frame);
}

static void parse_config(DspMixerState* s, DspConfig* config) {
    uint32_t dirty = config->dirty;
    if (!dirty) return;
    for (unsigned i = 0; i < 2; ++i) {
        if (dirty & (DSP_AUX_BUS_ENABLE_0_DIRTY << i)) s->aux_bus_enable[i] = config->aux_bus_enable[i] != 0;
        if (dirty & (DSP_AUX_RETURN_VOLUME_0_DIRTY << i))
            s->intermediate_mixer_volume[i + 1] = config->aux_return_volume[i];
        if (dirty & (DSP_DELAY_EFFECT_0_DIRTY << i)) delay_parse(&s->delay[i], &config->delay[i]);
    }
    if (dirty & DSP_MASTER_VOLUME_DIRTY) s->intermediate_mixer_volume[0] = config->master_volume;
    if (dirty & DSP_OUTPUT_FORMAT_DIRTY) s->output_format = config->output_format;
    /* reverb: a stub, as in Azahar. Headphones change the surround coefficients, and surround is not implemented */
    config->dirty = 0;
}
static void downmix(DspFrame frame, uint16_t format, float gain, DspQuadFrame samples) {
    for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; ++n) {
        const int32_t* x = samples[n];
        int16_t left, right;
        if (format == DSP_OUT_MONO)
            left = right = snx_clamp_s16(to32((gain * x[0] + gain * x[1] + gain * x[2] + gain * x[3]) / 2));
        else { /* stereo, and surround, which is not implemented */
            left = snx_clamp_s16(to32(gain * x[0] + gain * x[2]));
            right = snx_clamp_s16(to32(gain * x[1] + gain * x[3]));
        }
        frame[n][0] = snx_clamp_s16((int32_t)frame[n][0] + left);
        frame[n][1] = snx_clamp_s16((int32_t)frame[n][1] + right);
    }
}
bool dsp_mixers_tick(DspMixers* m, DspConfig* config, const DspIntermediate* read_samples,
                     DspIntermediate* write_samples, DspQuadFrame input[3]) {
    DspMixerState* s = &m->state;
    parse_config(s, config);
    /* AuxReturn: what the application processed comes back from the read region (its samples are channel-major) */
    for (unsigned bus = 0; bus < 2; ++bus)
        if (s->aux_bus_enable[bus])
            for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; n++)
                for (unsigned c = 0; c < 4; c++)
                    s->intermediate_mix_buffer[bus + 1][n][c] = read_samples->mix[bus][c][n];
    /* AuxSend: an enabled bus goes out to the application through the write region, else straight on */
    memcpy(s->intermediate_mix_buffer[0], input[0], sizeof(DspQuadFrame));
    bool sent = false;
    for (unsigned bus = 0; bus < 2; ++bus) {
        if (!s->aux_bus_enable[bus]) {
            memcpy(s->intermediate_mix_buffer[bus + 1], input[bus + 1], sizeof(DspQuadFrame));
            continue;
        }
        for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; n++)
            for (unsigned c = 0; c < 4; c++) write_samples->mix[bus][c][n] = input[bus + 1][n][c];
        sent = true;
    }
    for (unsigned i = 0; i < 2; i++) delay_process(&s->delay[i], s->intermediate_mix_buffer[i + 1]);
    /* the first intermediate mixer's volume is the master volume; the limiter and compressor are assumed off */
    memset(m->frame, 0, sizeof m->frame);
    for (unsigned mix = 0; mix < 3; mix++)
        downmix(m->frame, s->output_format, s->intermediate_mixer_volume[mix], s->intermediate_mix_buffer[mix]);
    return sent;
}
