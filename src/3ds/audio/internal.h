/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The DSP's shared memory as the application sees it, and the state of the sources and mixers that process it.
   Layouts from Azahar audio_core/hle/shared_memory.h (GPL-2.0-or-later, Copyright 2016-2026 Citra Emulator Project /
   Azahar Emulator Project) at a4b335e. The DSP is a 16-bit machine: a 32-bit field it reads has its 16-bit halves
   swapped against the ARM11's little-endian order (dsp32), floats and the intermediate mix samples do not. */
#ifndef SNX_CTR_DSP_INTERNAL_H
#define SNX_CTR_DSP_INTERNAL_H
#include "3ds/audio/dsp.h"
#include "common/utilities/bits.h"
#include <stddef.h>
#include <stdint.h>

#define DSP_SOURCES         24u
#define DSP_REGION0         0x50000u /* the two SharedMemory regions, offsets into DSP RAM */
#define DSP_REGION1         0x70000u
/* SharedMemory, 0x8000 bytes: where each structure sits in a region */
#define DSP_STATUS          0x0800u
#define DSP_DEBUG           0x0820u
#define DSP_FINAL_SAMPLES   0x0a80u
#define DSP_SOURCE_STATUSES 0x0d00u
#define DSP_COMPRESSOR      0x0e20u
#define DSP_CONFIGURATION   0x2860u
#define DSP_INTERMEDIATE    0x2924u
#define DSP_SOURCE_CONFIGS  0x3d24u
#define DSP_ADPCM_COEFFS    0x4f24u
#define DSP_UNKNOWN10       0x5224u
#define DSP_UNKNOWN11       0x5424u
#define DSP_UNKNOWN12       0x55a4u
#define DSP_UNKNOWN13       0x58a4u
#define DSP_UNKNOWN14       0x58b8u
#define DSP_FRAME_COUNTER   0x7ffeu

static inline uint32_t dsp32(uint32_t value) { return value << 16 | value >> 16; }

/* SourceConfiguration::Configuration */
typedef struct {
    uint32_t physical_address, length; /* dsp32 */
    uint16_t adpcm_ps, adpcm_yn[2];
    uint8_t adpcm_dirty, is_looping;
    uint16_t buffer_id, padding;
} DspBuffer;
typedef struct {
    uint32_t dirty;
    float gain[3][4];
    float rate_multiplier;
    uint8_t interpolation_mode, padding0;
    uint16_t filters_enabled;
    int16_t simple_b0, simple_a1;
    int16_t biquad_a2, biquad_a1, biquad_b2, biquad_b1, biquad_b0;
    uint16_t buffers_dirty;
    DspBuffer buffers[4];
    uint32_t loop_related; /* dsp32 */
    uint8_t enable, padding1;
    uint16_t sync_count;
    uint32_t play_position; /* dsp32 */
    uint16_t padding2[2];
    uint32_t physical_address, length; /* dsp32 */
    uint16_t flags1;                   /* bits 0-1 mono (1) or stereo (2), 2-3 format, 5 fade in */
    uint16_t adpcm_ps, adpcm_yn[2];
    uint16_t flags2; /* bit 0 adpcm_dirty, bit 1 is_looping */
    uint16_t buffer_id;
} DspSourceConfig;
_Static_assert(sizeof(DspBuffer) == 20, "DSP buffer layout");
_Static_assert(sizeof(DspSourceConfig) == 192, "DSP source configuration layout");
_Static_assert(offsetof(DspSourceConfig, buffers) == 0x4c && offsetof(DspSourceConfig, buffer_id) == 0xbe,
               "DSP source configuration layout");
/* Configuration.dirty */
enum {
    DSP_FORMAT_DIRTY = 1u << 0,
    DSP_MONO_OR_STEREO_DIRTY = 1u << 1,
    DSP_ADPCM_COEFFICIENTS_DIRTY = 1u << 2,
    DSP_PARTIAL_EMBEDDED_BUFFER_DIRTY = 1u << 3,
    DSP_PARTIAL_RESET = 1u << 4,
    DSP_ENABLE_DIRTY = 1u << 16,
    DSP_INTERPOLATION_DIRTY = 1u << 17,
    DSP_RATE_MULTIPLIER_DIRTY = 1u << 18,
    DSP_BUFFER_QUEUE_DIRTY = 1u << 19,
    DSP_LOOP_RELATED_DIRTY = 1u << 20,
    DSP_PLAY_POSITION_DIRTY = 1u << 21,
    DSP_FILTERS_ENABLED_DIRTY = 1u << 22,
    DSP_SIMPLE_FILTER_DIRTY = 1u << 23,
    DSP_BIQUAD_FILTER_DIRTY = 1u << 24,
    DSP_GAIN_0_DIRTY = 1u << 25,
    DSP_SYNC_COUNT_DIRTY = 1u << 28,
    DSP_RESET = 1u << 29,
    DSP_EMBEDDED_BUFFER_DIRTY = 1u << 30
};
enum { DSP_PCM8, DSP_PCM16, DSP_ADPCM };
enum { DSP_MONO = 1, DSP_STEREO = 2 };
enum { DSP_POLYPHASE, DSP_LINEAR, DSP_NONE };

/* SourceStatus::Status */
typedef struct {
    uint8_t is_enabled, current_buffer_id_dirty;
    uint16_t sync_count;
    uint32_t buffer_position; /* dsp32 */
    uint16_t current_buffer_id, last_buffer_id;
} DspSourceStatus;
_Static_assert(sizeof(DspSourceStatus) == 12, "DSP source status layout");

/* DspConfiguration */
typedef struct {
    uint16_t dirty, enable, padding, outputs;
    uint32_t work_buffer_address; /* dsp32 */
    uint16_t frame_count;
    int16_t g, a, b; /* 7 fractional bits */
} DspDelayConfig;
typedef struct {
    uint32_t dirty;
    float master_volume, aux_return_volume[2];
    uint16_t output_buffer_count, padding0[2], output_format, clipping_mode, headphones_connected, surround_depth,
        surround_speaker_position, padding1, rear_ratio, aux_front_bypass[2], aux_bus_enable[2];
    DspDelayConfig delay[2];
    uint16_t reverb[2][26];
    uint16_t sync_mode, padding2;
    uint32_t dirty2;
} DspConfig;
_Static_assert(sizeof(DspDelayConfig) == 20, "DSP delay effect layout");
_Static_assert(sizeof(DspConfig) == 196 && offsetof(DspConfig, delay) == 0x2c && offsetof(DspConfig, dirty2) == 0xc0,
               "DSP configuration layout");
/* DspConfiguration.dirty */
enum {
    DSP_AUX_BUS_ENABLE_0_DIRTY = 1u << 8,
    DSP_DELAY_EFFECT_0_DIRTY = 1u << 10,
    DSP_REVERB_EFFECT_0_DIRTY = 1u << 12,
    DSP_MASTER_VOLUME_DIRTY = 1u << 16,
    DSP_AUX_RETURN_VOLUME_0_DIRTY = 1u << 24,
    DSP_OUTPUT_FORMAT_DIRTY = 1u << 26,
    DSP_HEADPHONES_CONNECTED_DIRTY = 1u << 28
};
enum { DSP_OUT_MONO, DSP_OUT_STEREO, DSP_OUT_SURROUND };

/* IntermediateMixSamples: two aux buses, channel-major, little-endian */
typedef struct {
    int32_t mix[2][4][SNX_CTR_DSP_SAMPLES];
} DspIntermediate;
_Static_assert(sizeof(DspIntermediate) == 5120, "DSP intermediate mix layout");

typedef int16_t DspFrame[SNX_CTR_DSP_SAMPLES][2];
typedef int32_t DspQuadFrame[SNX_CTR_DSP_SAMPLES][4];

/* SourceFilters */
typedef struct {
    bool simple_enabled, biquad_enabled;
    int32_t simple_a1, simple_b0;
    int16_t simple_y1[2];
    int32_t a1, a2, b0, b1, b2;
    int16_t x1[2], x2[2], y1[2], y2[2];
} DspFilters;

/* A buffer waiting in a source's queue */
typedef struct {
    uint32_t physical_address, length;
    uint16_t adpcm_yn[2], buffer_id;
    bool adpcm_dirty, is_looping, from_queue, has_played;
    uint8_t mono_or_stereo, format;
    uint32_t play_position;
} DspQueued;
typedef struct {
    bool enabled;
    uint16_t sync_count;
    float gain[3][4], gain_ramp_start[3][4];
    bool gain_ramp_active[3];
    DspQueued* queue; /* a priority queue on buffer_id, the lowest first */
    size_t queued, queue_capacity;
    uint8_t mono_or_stereo, format;
    uint32_t current_sample_number, current_buffer_physical_address, current_buffer_length;
    uint8_t current_buffer_mono_or_stereo, current_buffer_format;
    bool current_buffer_is_looping;
    /* the decoded current buffer, consumed from [position] on (Azahar's StereoBuffer16 deque) */
    int16_t (*current)[2];
    size_t current_count, current_position;
    bool buffer_update;
    uint16_t last_buffer_id, current_buffer_id;
    int16_t adpcm_coeffs[16], adpcm_yn1, adpcm_yn2;
    float rate_multiplier;
    uint8_t interpolation_mode;
    int16_t interp_xn1[2], interp_xn2[2];
    uint64_t interp_fposition;
    DspFilters filters;
} DspSourceState;
typedef struct {
    DspSourceState state, backup;
    DspFrame frame, backup_frame;
} DspSource;

void dsp_source_reset(DspSource* source);
void dsp_source_free(DspSource* source);
void dsp_source_sleep(DspSource* source);
void dsp_source_wakeup(DspSource* source);
/* Source::Tick: config's dirty bits are cleared as they are taken, as the firmware does in shared memory */
DspSourceStatus dsp_source_tick(DspSource* source, SnxCtrMemory* memory, DspSourceConfig* config,
                                const int16_t adpcm_coeffs[16]);
void dsp_source_mix(const DspSource* source, DspQuadFrame dest, unsigned mix);
void dsp_source_mix_done(DspSource* source, unsigned mix);

/* DelayEffect */
typedef struct {
    bool enabled;
    uint32_t delay_samples;
    float g, a, b;
    float* delay_output; /* 4 channels of delay_samples */
    float last_output[4];
    size_t delay_pos;
} DspDelay;
typedef struct {
    float intermediate_mixer_volume[3];
    bool aux_bus_enable[2];
    DspQuadFrame intermediate_mix_buffer[3];
    DspDelay delay[2];
    uint16_t output_format;
} DspMixerState;
typedef struct {
    DspMixerState state, backup;
    DspFrame frame, backup_frame;
} DspMixers;

void dsp_mixers_reset(DspMixers* mixers);
void dsp_mixers_free(DspMixers* mixers);
void dsp_mixers_sleep(DspMixers* mixers);
void dsp_mixers_wakeup(DspMixers* mixers);
/* Mixers::Tick. Returns whether write_samples was written (an aux bus is enabled) */
bool dsp_mixers_tick(DspMixers* mixers, DspConfig* config, const DspIntermediate* read_samples,
                     DspIntermediate* write_samples, DspQuadFrame input[3]);
#endif
