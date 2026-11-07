/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The CSND sound hardware, ported from Azahar's audio_core/csnd.cpp at azahar-emu/azahar 6e42de8 (csnd.h) */
#include "3ds/audio/csnd.h"

#define TICKS_PER_SAMPLE 2048u /* the timer's units an output sample takes */
#define MIN_PERIOD       0x42u
#define MAX_VOLUME       0x8000u
enum { PCM8, PCM16, IMA_ADPCM, PSG };
enum { MANUAL, NORMAL, ONE_SHOT, NORMAL_NO_RELOAD };

static const int16_t IMA_STEPS[89] = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t IMA_INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static unsigned format_of(uint16_t cnt) { return (cnt & SNX_CTR_CSND_FORMAT_MASK) >> SNX_CTR_CSND_FORMAT_SHIFT; }
static unsigned loop_of(uint16_t cnt) { return (cnt & SNX_CTR_CSND_LOOP_MASK) >> SNX_CTR_CSND_LOOP_SHIFT; }
static uint16_t clamp_volume(uint16_t v) { return v < MAX_VOLUME ? v : MAX_VOLUME; }
static uint8_t index_of(uint32_t state) { return (uint8_t)((state >> 16 & 0x7f) < 88 ? state >> 16 & 0x7f : 88); }
static uint32_t period_of(const SnxCtrCsndChannel* ch) {
    uint32_t period = 0x10000u - ch->timer;
    return period > MIN_PERIOD ? period : MIN_PERIOD;
}

static void start(SnxCtrCsndChannel* ch) {
    ch->block = ch->start;
    ch->block_size = ch->size;
    ch->position = ch->counter = 0;
    ch->high_nibble = ch->holding = 0;
    ch->previous = ch->current = 0;
    ch->predictor = (int16_t)(ch->adpcm_state & 0xffff);
    ch->index = index_of(ch->adpcm_state);
}
static void stop(SnxCtrCsndChannel* ch) {
    ch->adpcm_state = (uint16_t)ch->predictor | (uint32_t)ch->index << 16;
    ch->cnt &= (uint16_t)~SNX_CTR_CSND_ENABLE;
}
void snx_ctr_csnd_set_cnt(SnxCtrCsndHw* hw, unsigned channel, uint16_t cnt) {
    SnxCtrCsndChannel* ch = &hw->channel[channel];
    bool was = ch->cnt & SNX_CTR_CSND_ENABLE;
    ch->cnt = cnt;
    if (!was && (cnt & SNX_CTR_CSND_ENABLE))
        start(ch);
    else if (was && !(cnt & SNX_CTR_CSND_ENABLE)) {
        stop(ch);
        ch->holding = 0;
    }
    if (!(cnt & SNX_CTR_CSND_HOLD)) ch->holding = 0;
}
void snx_ctr_csnd_set_volume(SnxCtrCsndHw* hw, unsigned channel, uint16_t left, uint16_t right) {
    hw->channel[channel].volume[0] = clamp_volume(left);
    hw->channel[channel].volume[1] = clamp_volume(right);
}
void snx_ctr_csnd_set_capture_volume(SnxCtrCsndHw* hw, unsigned channel, uint16_t left, uint16_t right) {
    hw->channel[channel].capture_volume[0] = clamp_volume(left);
    hw->channel[channel].capture_volume[1] = clamp_volume(right);
}
uint32_t snx_ctr_csnd_adpcm_state(const SnxCtrCsndHw* hw, unsigned channel) {
    const SnxCtrCsndChannel* ch = &hw->channel[channel];
    if (ch->cnt & SNX_CTR_CSND_ENABLE) return (uint16_t)ch->predictor | (uint32_t)ch->index << 16;
    return ch->adpcm_state;
}
bool snx_ctr_csnd_busy(const SnxCtrCsndHw* hw) {
    for (unsigned i = 0; i < SNX_CTR_CSND_CHANNELS; ++i)
        if (hw->channel[i].cnt & SNX_CTR_CSND_ENABLE) return true;
    return false;
}
/* a byte of physical memory, 0 where nothing is mapped (Azahar's null pointer) */
static uint8_t byte_at(SnxCtrMemory* memory, uint32_t pa, bool* mapped) {
    uint8_t b = 0;
    *mapped = snx_ctr_phys_read(memory, pa, &b, 1) == SNX_CTR_MEM_OK;
    return b;
}
static int16_t fetch(SnxCtrCsndChannel* ch, SnxCtrMemory* memory) {
    uint32_t address = ch->block + ch->position;
    bool mapped;
    switch (format_of(ch->cnt)) {
        case PCM8: {
            uint8_t b = byte_at(memory, address, &mapped);
            ch->position++;
            return mapped ? (int16_t)((int8_t)b * 256) : 0;
        }
        case PCM16: {
            uint8_t low = byte_at(memory, address, &mapped);
            bool high_mapped;
            uint8_t high = byte_at(memory, address + 1, &high_mapped);
            ch->position += 2;
            return mapped ? (int16_t)(uint16_t)(low | high << 8) : 0;
        }
        case IMA_ADPCM: {
            uint8_t b = byte_at(memory, address, &mapped);
            uint8_t nibble = ch->high_nibble ? b >> 4 : b & 0xf;
            if (ch->high_nibble) ch->position++;
            ch->high_nibble = !ch->high_nibble;
            int32_t step = IMA_STEPS[ch->index], diff = step >> 3;
            if (nibble & 1) diff += step >> 2;
            if (nibble & 2) diff += step >> 1;
            if (nibble & 4) diff += step;
            int32_t predictor = ch->predictor;
            if (nibble & 8)
                predictor = predictor - diff > -0x7fff ? predictor - diff : -0x7fff;
            else
                predictor = predictor + diff < 0x7fff ? predictor + diff : 0x7fff;
            ch->predictor = (int16_t)predictor;
            int index = ch->index + IMA_INDEX[nibble & 7];
            ch->index = (uint8_t)(index < 0 ? 0 : index > 88 ? 88 : index);
            return ch->predictor;
        }
        default: return 0; /* PSG and noise, not emulated */
    }
}
static bool block_end(const SnxCtrCsndChannel* ch) {
    if (format_of(ch->cnt) == PSG || loop_of(ch->cnt) == MANUAL) return false;
    return ch->position >= ch->block_size;
}
static void step(SnxCtrCsndChannel* ch, SnxCtrMemory* memory) {
    if (block_end(ch)) {
        unsigned loop = loop_of(ch->cnt);
        if (loop == ONE_SHOT) {
            stop(ch);
            ch->holding = (ch->cnt & SNX_CTR_CSND_HOLD) != 0;
            return;
        }
        ch->block = ch->loop;
        if (loop == NORMAL) ch->block_size = ch->size;
        ch->position = 0;
        ch->high_nibble = 0;
        if (ch->loop_adpcm_state & SNX_CTR_CSND_LOOP_RELOAD) {
            ch->predictor = (int16_t)(ch->loop_adpcm_state & 0xffff);
            ch->index = index_of(ch->loop_adpcm_state);
        }
        if (!ch->block_size) {
            stop(ch);
            return;
        }
    }
    ch->previous = ch->current;
    ch->current = fetch(ch, memory);
}
static int32_t interpolated(const SnxCtrCsndChannel* ch) {
    if (!(ch->cnt & SNX_CTR_CSND_INTERPOLATE)) return ch->current;
    int32_t delta = ch->current - ch->previous;
    return (int16_t)(ch->previous + delta * (int32_t)ch->counter / (int32_t)period_of(ch));
}
static int16_t clamp16(int64_t v) { return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v); }
void snx_ctr_csnd_render(SnxCtrCsndHw* hw, SnxCtrMemory* memory, int16_t (*out)[2], unsigned count) {
    for (unsigned s = 0; s < count; ++s) {
        int32_t left = 0, right = 0;
        for (unsigned i = 0; i < SNX_CTR_CSND_CHANNELS; ++i) {
            SnxCtrCsndChannel* ch = &hw->channel[i];
            if ((ch->cnt & SNX_CTR_CSND_ENABLE) && (ch->cnt & SNX_CTR_CSND_RUN)) {
                ch->counter += TICKS_PER_SAMPLE;
                while (ch->counter >= period_of(ch) && (ch->cnt & SNX_CTR_CSND_ENABLE)) {
                    ch->counter -= period_of(ch);
                    step(ch, memory);
                }
            }
            if (!(ch->cnt & SNX_CTR_CSND_ENABLE)) {
                if (ch->holding) {
                    left += ch->current * ch->volume[0] >> 15;
                    right += ch->current * ch->volume[1] >> 15;
                }
                continue;
            }
            int32_t sample = interpolated(ch);
            left += sample * ch->volume[0] >> 15;
            right += sample * ch->volume[1] >> 15;
        }
        /* the master volume, 0x8000 (unity) */
        out[s][0] = clamp16(left);
        out[s][1] = clamp16(right);
    }
}
