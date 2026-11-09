/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The DSP's 24 sources: buffer queues, decoding (PCM8, PCM16, GC-ADPCM), resampling, the per-source filters and gains.
   Ported from Azahar audio_core/hle/{source,filter}.cpp and audio_core/{codec,interpolate}.cpp (GPL-2.0-or-later,
   Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e. */
#include "3ds/audio/internal.h"
#include <stdlib.h>
#include <string.h>

/* ---- Codec ---- */

/* GC-ADPCM with a scale factor and variable coefficients: 8-byte frames of 14 four-bit samples. The result is
   sample_count rounded up to even */
static int16_t (*decode_adpcm(const uint8_t* data, size_t sample_count, const int16_t coeffs[16], int16_t* yn1_state,
                              int16_t* yn2_state, size_t* count))[2] {
    static const int signed_nibbles[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};
    size_t size = sample_count % 2 == 0 ? sample_count : sample_count + 1;
    int16_t (*ret)[2] = calloc(size ? size : 1, sizeof *ret);
    if (!ret) return NULL;
    int yn1 = *yn1_state, yn2 = *yn2_state;
    size_t frames = (sample_count + 13) / 14;
    for (size_t framei = 0; framei < frames; framei++) {
        int header = data[framei * 8];
        int scale = 1 << (header & 0xf), index = (header >> 4) & 7;
        /* coefficients have 11 fractional bits */
        int coef1 = coeffs[index * 2], coef2 = coeffs[index * 2 + 1];
        size_t outputi = framei * 14, datai = framei * 8 + 1;
        for (size_t i = 0; i < 14 && outputi < sample_count; i += 2) {
            for (unsigned half = 0; half < 2; ++half) {
                int nibble = signed_nibbles[half ? data[datai] & 0xf : data[datai] >> 4];
                /* y[n] = x[n] + 0.5 + c1 * y[n-1] + c2 * y[n-2], in 11-bit fixed point */
                int val = (nibble * scale * 2048 + 0x400 + coef1 * yn1 + coef2 * yn2) >> 11;
                val = val < -32768 ? -32768 : val > 32767 ? 32767 : val;
                yn2 = yn1;
                yn1 = val;
                ret[outputi][0] = ret[outputi][1] = (int16_t)val;
                outputi++;
            }
            datai++;
        }
    }
    *yn1_state = (int16_t)yn1;
    *yn2_state = (int16_t)yn2;
    *count = size;
    return ret;
}
static int16_t (*decode_pcm(unsigned format, unsigned channels, const uint8_t* data, size_t sample_count))[2] {
    int16_t (*ret)[2] = calloc(sample_count ? sample_count : 1, sizeof *ret);
    if (!ret) return NULL;
    for (size_t i = 0; i < sample_count; i++)
        for (unsigned c = 0; c < 2; ++c) {
            size_t at = i * channels + (channels == 2 ? c : 0);
            if (format == DSP_PCM8)
                ret[i][c] = (int16_t)((uint16_t)data[at] << 8);
            else
                ret[i][c] = (int16_t)(uint16_t)(data[at * 2] | data[at * 2 + 1] << 8);
        }
    return ret;
}
static size_t encoded_bytes(unsigned format, unsigned channels, size_t samples) {
    return format == DSP_PCM8    ? samples * channels
           : format == DSP_PCM16 ? samples * channels * 2
                                 : (samples + 13) / 14 * 8;
}
/* Decodes length samples at the physical address (masked to a word, as the firmware's DMA reads it) into the
   current buffer. False when the address is not RAM */
static bool decode_current(DspSourceState* s, SnxCtrMemory* memory, uint32_t address, uint32_t length, unsigned format,
                           unsigned mono_or_stereo) {
    unsigned channels = mono_or_stereo == DSP_STEREO ? 2 : 1;
    size_t bytes = encoded_bytes(format, channels, length);
    uint8_t* data = malloc(bytes ? bytes : 1);
    if (!data) return false;
    if (bytes && snx_ctr_phys_read(memory, address & ~3u, data, bytes) != SNX_CTR_MEM_OK) {
        free(data);
        return false;
    }
    size_t count = length;
    int16_t (*decoded)[2] = format == DSP_ADPCM
                                ? decode_adpcm(data, length, s->adpcm_coeffs, &s->adpcm_yn1, &s->adpcm_yn2, &count)
                                : decode_pcm(format, channels, data, length);
    free(data);
    if (!decoded) return false;
    free(s->current);
    s->current = decoded;
    s->current_count = count;
    s->current_position = 0;
    return true;
}
static size_t current_size(const DspSourceState* s) { return s->current_count - s->current_position; }
static void current_clear(DspSourceState* s) {
    free(s->current);
    s->current = NULL;
    s->current_count = s->current_position = 0;
}
/* drops n samples from the front, at most what is left */
static void current_erase(DspSourceState* s, size_t n) {
    s->current_position += n < current_size(s) ? n : current_size(s);
}

/* ---- Interpolation: 24 fractional bits, two samples of history (a two-sample predelay) ---- */

#define SCALE ((uint64_t)1 << 24)
#define MASK  (SCALE - 1)
static void step_over_samples(DspSourceState* s, float rate, DspFrame output, size_t* outputi, bool linear) {
    size_t n = current_size(s);
    if (!n) return;
    /* the input is xn2, xn1, then the current buffer */
    size_t size = n + 2;
#define INPUT(i) ((i) == 0 ? s->interp_xn2 : (i) == 1 ? s->interp_xn1 : s->current[s->current_position + (i) - 2])
    uint64_t step = (uint64_t)(rate * (float)SCALE), fposition = s->interp_fposition;
    size_t inputi = 0;
    while (*outputi < SNX_CTR_DSP_SAMPLES) {
        inputi = (size_t)(fposition / SCALE);
        if (inputi + 2 >= size) {
            inputi = size - 2;
            break;
        }
        uint64_t fraction = fposition & MASK;
        const int16_t *x0 = INPUT(inputi), *x1 = INPUT(inputi + 1);
        for (unsigned c = 0; c < 2; ++c) {
            if (!linear) {
                output[*outputi][c] = x0[c];
                continue;
            }
            /* a saturated difference (verified by black-box fuzzing); the product is unsigned, as Azahar computes it,
               which rounds a negative step down */
            int64_t delta = snx_clamp_s16((int64_t)x1[c] - x0[c]);
            output[*outputi][c] = (int16_t)(x0[c] + fraction * delta / SCALE);
        }
        ++*outputi;
        fposition += step;
    }
    int16_t xn2[2], xn1[2];
    memcpy(xn2, INPUT(inputi), sizeof xn2);
    memcpy(xn1, INPUT(inputi + 1), sizeof xn1);
#undef INPUT
    memcpy(s->interp_xn2, xn2, sizeof xn2);
    memcpy(s->interp_xn1, xn1, sizeof xn1);
    s->interp_fposition = fposition - inputi * SCALE;
    /* erases inputi + 2 of the input: the two history samples and inputi of the buffer */
    current_erase(s, inputi);
}

/* ---- Filters ---- */

static void filters_enable(DspFilters* f, bool simple, bool biquad) {
    f->simple_enabled = simple;
    f->biquad_enabled = biquad;
    if (!simple) { /* passthrough */
        memset(f->simple_y1, 0, sizeof f->simple_y1);
        f->simple_a1 = 0;
        f->simple_b0 = 1 << 15;
    }
    if (!biquad) {
        memset(f->x1, 0, sizeof f->x1);
        memset(f->x2, 0, sizeof f->x2);
        memset(f->y1, 0, sizeof f->y1);
        memset(f->y2, 0, sizeof f->y2);
        f->a1 = f->a2 = f->b1 = f->b2 = 0;
        f->b0 = 1 << 14;
    }
}
static void filters_process(DspFilters* f, DspFrame frame) {
    if (f->simple_enabled)
        for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; ++n) {
            /* H(z) = b0 / (1 - a1 z^-1), 15 fractional bits */
            for (unsigned c = 0; c < 2; ++c)
                frame[n][c] = snx_clamp_s16((f->simple_b0 * frame[n][c] + f->simple_a1 * f->simple_y1[c]) >> 15);
            memcpy(f->simple_y1, frame[n], sizeof f->simple_y1);
        }
    if (f->biquad_enabled)
        for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; ++n) {
            int16_t y0[2];
            /* H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 - a1 z^-1 - a2 z^-2), 14 fractional bits */
            for (unsigned c = 0; c < 2; ++c)
                y0[c] = snx_clamp_s16(
                    (f->b0 * frame[n][c] + f->b1 * f->x1[c] + f->b2 * f->x2[c] + f->a1 * f->y1[c] + f->a2 * f->y2[c]) >>
                    14);
            memcpy(f->x2, f->x1, sizeof f->x2);
            memcpy(f->x1, frame[n], sizeof f->x1);
            memcpy(f->y2, f->y1, sizeof f->y2);
            memcpy(f->y1, y0, sizeof f->y1);
            memcpy(frame[n], y0, sizeof y0);
        }
}

/* ---- Source ---- */

static void state_init(DspSourceState* s) {
    *s = (DspSourceState){.mono_or_stereo = DSP_MONO,
                          .format = DSP_ADPCM,
                          .current_buffer_mono_or_stereo = DSP_MONO,
                          .current_buffer_format = DSP_ADPCM,
                          .rate_multiplier = 1.0f,
                          .interpolation_mode = DSP_POLYPHASE};
    filters_enable(&s->filters, false, false);
}
static void state_free(DspSourceState* s) {
    free(s->queue);
    free(s->current);
}
/* a deep copy of the queue and the current buffer, for sleep */
static bool state_copy(DspSourceState* to, const DspSourceState* from) {
    *to = *from;
    to->queue = NULL;
    to->current = NULL;
    if (from->queue_capacity) {
        to->queue = malloc(from->queue_capacity * sizeof *to->queue);
        if (!to->queue) return false;
        memcpy(to->queue, from->queue, from->queued * sizeof *to->queue);
    }
    if (from->current) {
        to->current = malloc(from->current_count * sizeof *to->current);
        if (!to->current) {
            free(to->queue);
            to->queue = NULL;
            return false;
        }
        memcpy(to->current, from->current, from->current_count * sizeof *to->current);
    }
    return true;
}
void dsp_source_reset(DspSource* source) {
    state_free(&source->state);
    state_init(&source->state);
    memset(source->frame, 0, sizeof source->frame);
}
void dsp_source_free(DspSource* source) {
    state_free(&source->state);
    state_free(&source->backup);
    source->state = (DspSourceState){0};
    source->backup = (DspSourceState){0};
}
void dsp_source_sleep(DspSource* source) {
    state_free(&source->backup);
    if (!state_copy(&source->backup, &source->state)) state_init(&source->backup);
    memcpy(source->backup_frame, source->frame, sizeof source->frame);
}
void dsp_source_wakeup(DspSource* source) {
    state_free(&source->state);
    source->state = source->backup;
    memcpy(source->frame, source->backup_frame, sizeof source->frame);
    state_init(&source->backup);
    memset(source->backup_frame, 0, sizeof source->backup_frame);
}

static bool queue_push(DspSourceState* s, DspQueued buffer) {
    if (s->queued == s->queue_capacity) {
        size_t capacity = s->queue_capacity ? s->queue_capacity * 2 : 8;
        DspQueued* queue = realloc(s->queue, capacity * sizeof *queue);
        if (!queue) return false;
        s->queue = queue;
        s->queue_capacity = capacity;
    }
    s->queue[s->queued++] = buffer;
    return true;
}
/* the queued buffer with the lowest buffer_id */
static bool queue_pop(DspSourceState* s, DspQueued* out) {
    if (!s->queued) return false;
    size_t best = 0;
    for (size_t i = 1; i < s->queued; ++i)
        if (s->queue[i].buffer_id < s->queue[best].buffer_id) best = i;
    *out = s->queue[best];
    s->queue[best] = s->queue[--s->queued];
    return true;
}

static bool dequeue_buffer(DspSourceState* s, SnxCtrMemory* memory) {
    DspQueued buf;
    if (!queue_pop(s, &buf)) return false;
    if (buf.adpcm_dirty) {
        s->adpcm_yn1 = (int16_t)buf.adpcm_yn[0];
        s->adpcm_yn2 = (int16_t)buf.adpcm_yn[1];
    }
    if (!decode_current(s, memory, buf.physical_address, buf.length, buf.format, buf.mono_or_stereo)) {
        /* not RAM: this buffer plays as nothing */
        current_clear(s);
        return true;
    }
    /* the first playthrough starts at play_position, loops at the beginning */
    s->current_sample_number = !buf.has_played ? buf.play_position : 0;
    s->current_buffer_physical_address = buf.physical_address;
    s->current_buffer_length = buf.length;
    s->current_buffer_mono_or_stereo = buf.mono_or_stereo;
    s->current_buffer_format = buf.format;
    s->current_buffer_is_looping = buf.is_looping;
    s->current_buffer_id = buf.buffer_id;
    s->last_buffer_id = 0;
    s->buffer_update = buf.from_queue && !buf.has_played;
    if (buf.is_looping) {
        buf.has_played = true;
        (void)queue_push(s, buf);
    }
    current_erase(s, s->current_sample_number);
    return true;
}

static void parse_config(DspSourceState* s, SnxCtrMemory* memory, DspSourceConfig* config,
                         const int16_t adpcm_coeffs[16], DspSource* source) {
    uint32_t dirty = config->dirty;
    if (!dirty) return;
    if (dirty & DSP_RESET) {
        dsp_source_reset(source);
        s = &source->state;
    }
    if (dirty & DSP_PARTIAL_RESET) s->queued = 0;
    if (dirty & DSP_ENABLE_DIRTY) s->enabled = config->enable != 0;
    if (dirty & DSP_SYNC_COUNT_DIRTY) s->sync_count = config->sync_count;
    if (dirty & DSP_RATE_MULTIPLIER_DIRTY) {
        s->rate_multiplier = config->rate_multiplier;
        /* the firmware produces garbage with these */
        if (!(s->rate_multiplier > 0)) s->rate_multiplier = 1.0f;
    }
    if (dirty & DSP_ADPCM_COEFFICIENTS_DIRTY) memcpy(s->adpcm_coeffs, adpcm_coeffs, sizeof s->adpcm_coeffs);
    for (unsigned mix = 0; mix < 3; ++mix)
        if (dirty & (DSP_GAIN_0_DIRTY << mix)) {
            memcpy(s->gain_ramp_start[mix], s->gain[mix], sizeof s->gain[mix]);
            s->gain_ramp_active[mix] = true;
            memcpy(s->gain[mix], config->gain[mix], sizeof s->gain[mix]);
        }
    if (dirty & DSP_FILTERS_ENABLED_DIRTY)
        filters_enable(&s->filters, config->filters_enabled & 1, (config->filters_enabled >> 1) & 1);
    if (dirty & DSP_SIMPLE_FILTER_DIRTY) {
        s->filters.simple_a1 = config->simple_a1;
        s->filters.simple_b0 = config->simple_b0;
    }
    if (dirty & DSP_BIQUAD_FILTER_DIRTY) {
        s->filters.a1 = config->biquad_a1;
        s->filters.a2 = config->biquad_a2;
        s->filters.b0 = config->biquad_b0;
        s->filters.b1 = config->biquad_b1;
        s->filters.b2 = config->biquad_b2;
    }
    if (dirty & DSP_INTERPOLATION_DIRTY) s->interpolation_mode = config->interpolation_mode;
    if (dirty & (DSP_FORMAT_DIRTY | DSP_EMBEDDED_BUFFER_DIRTY)) s->format = (config->flags1 >> 2) & 3;
    if (dirty & (DSP_MONO_OR_STEREO_DIRTY | DSP_EMBEDDED_BUFFER_DIRTY)) s->mono_or_stereo = config->flags1 & 3;
    /* play_position applies only to the embedded buffer, and is 0 without its dirty bit */
    uint32_t play_position = dirty & DSP_PLAY_POSITION_DIRTY ? dsp32(config->play_position) : 0;
    if (dirty & DSP_PARTIAL_EMBEDDED_BUFFER_DIRTY) {
        /* The application extended the embedded buffer: decode it again with the new length, from the address
           latched at dequeue (the configuration's may be stale), and skip what has been played. Only PCM16 is
           handled, as in Azahar. */
        uint32_t length = dsp32(config->length);
        if (s->format == DSP_PCM16 &&
            decode_current(s, memory, s->current_buffer_physical_address, length, s->format, s->mono_or_stereo)) {
            s->current_buffer_length = length;
            s->current_buffer_mono_or_stereo = s->mono_or_stereo;
            s->current_buffer_format = s->format;
            s->current_buffer_is_looping = (config->flags2 >> 1) & 1;
            /* a length shorter than what was played (Tomodachi Life skipping dialog) restarts the buffer */
            if (current_size(s) < s->current_sample_number)
                s->current_sample_number = 0;
            else
                current_erase(s, s->current_sample_number);
        }
    }
    if (dirty & DSP_EMBEDDED_BUFFER_DIRTY) {
        uint32_t length = dsp32(config->length);
        /* an underflowed length (Luigi's Mansion: Dark Moon) is skipped rather than allocated */
        if ((int32_t)length >= 0)
            (void)queue_push(s, (DspQueued){.physical_address = dsp32(config->physical_address),
                                            .length = length,
                                            .adpcm_yn = {config->adpcm_yn[0], config->adpcm_yn[1]},
                                            .adpcm_dirty = config->flags2 & 1,
                                            .is_looping = (config->flags2 >> 1) & 1,
                                            .buffer_id = config->buffer_id,
                                            .mono_or_stereo = s->mono_or_stereo,
                                            .format = s->format,
                                            .play_position = play_position});
    }
    if (dirty & DSP_BUFFER_QUEUE_DIRTY) {
        for (unsigned i = 0; i < 4; i++) {
            if (!(config->buffers_dirty & (1u << i))) continue;
            const DspBuffer* b = &config->buffers[i];
            if ((int32_t)dsp32(b->length) < 0) continue;
            (void)queue_push(s, (DspQueued){.physical_address = dsp32(b->physical_address),
                                            .length = dsp32(b->length),
                                            .adpcm_yn = {b->adpcm_yn[0], b->adpcm_yn[1]},
                                            .adpcm_dirty = b->adpcm_dirty != 0,
                                            .is_looping = b->is_looping != 0,
                                            .buffer_id = b->buffer_id,
                                            .mono_or_stereo = s->mono_or_stereo,
                                            .format = s->format,
                                            .from_queue = true});
        }
        config->buffers_dirty = 0;
    }
    config->dirty = 0;
}

static void generate_frame(DspSourceState* s, DspFrame frame, SnxCtrMemory* memory) {
    memset(frame, 0, sizeof(DspFrame));
    /* The application can rewrite a looping PCM16 buffer while it plays: the part not yet played is read again from
       RAM. current_sample_number counts exactly what the interpolation consumed, so the offset stays in step */
    if (current_size(s) && s->current_buffer_is_looping && s->current_buffer_format == DSP_PCM16 &&
        s->current_buffer_physical_address && s->current_sample_number < s->current_buffer_length) {
        unsigned channels = s->current_buffer_mono_or_stereo == DSP_STEREO ? 2 : 1;
        uint32_t address = (s->current_buffer_physical_address & ~3u) + s->current_sample_number * channels * 2;
        (void)decode_current(s, memory, address, s->current_buffer_length - s->current_sample_number, DSP_PCM16,
                             s->current_buffer_mono_or_stereo);
    }
    if (!current_size(s)) {
        if (dequeue_buffer(s, memory)) return;
        s->enabled = false;
        s->buffer_update = true;
        s->last_buffer_id = s->current_buffer_id;
        s->current_buffer_id = 0;
        return;
    }
    size_t position = 0;
    while (position < SNX_CTR_DSP_SAMPLES) {
        if (!current_size(s) && !dequeue_buffer(s, memory)) break;
        size_t before = current_size(s);
        /* polyphase interpolation is not implemented in Azahar either: it plays as linear */
        step_over_samples(s, s->rate_multiplier, frame, &position, s->interpolation_mode != DSP_NONE);
        s->current_sample_number += (uint32_t)(before - current_size(s));
    }
    filters_process(&s->filters, frame);
}

DspSourceStatus dsp_source_tick(DspSource* source, SnxCtrMemory* memory, DspSourceConfig* config,
                                const int16_t adpcm_coeffs[16]) {
    parse_config(&source->state, memory, config, adpcm_coeffs, source);
    DspSourceState* s = &source->state;
    if (s->enabled) generate_frame(s, source->frame, memory);
    /* applications synchronise audio with video on current_buffer_id_dirty and current_buffer_id */
    DspSourceStatus status = {.is_enabled = s->enabled,
                              .current_buffer_id_dirty = s->buffer_update,
                              .sync_count = s->sync_count,
                              .buffer_position = dsp32(s->current_sample_number),
                              .current_buffer_id = s->current_buffer_id,
                              .last_buffer_id = s->last_buffer_id};
    s->buffer_update = false;
    return status;
}

/* Source::MixInto: the stereo frame into a quadraphonic mix with that mix's gains, ramping from the old gains across
   the frame after a change */
void dsp_source_mix(const DspSource* source, DspQuadFrame dest, unsigned mix) {
    const DspSourceState* s = &source->state;
    if (!s->enabled) return;
    const float *gains = s->gain[mix], *start = s->gain_ramp_start[mix];
    const float ramp_scale = 1.0f / (float)(SNX_CTR_DSP_SAMPLES - 1);
    for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; n++) {
        const float progress = (float)n * ramp_scale;
        float g[4];
        for (unsigned c = 0; c < 4; ++c)
            g[c] = s->gain_ramp_active[mix] ? start[c] + (gains[c] - start[c]) * progress : gains[c];
        dest[n][0] += (int32_t)(g[0] * source->frame[n][0]);
        dest[n][1] += (int32_t)(g[1] * source->frame[n][1]);
        dest[n][2] += (int32_t)(g[2] * source->frame[n][0]);
        dest[n][3] += (int32_t)(g[3] * source->frame[n][1]);
    }
}
void dsp_source_mix_done(DspSource* source, unsigned mix) {
    DspSourceState* s = &source->state;
    if (!s->enabled || s->gain_ramp_active[mix]) {
        memcpy(s->gain_ramp_start[mix], s->gain[mix], sizeof s->gain[mix]);
        s->gain_ramp_active[mix] = false;
    }
}
