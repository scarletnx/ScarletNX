/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/apu/apu.h"
#include "agb/board/board.h"
#include "common/utilities/endian.h"
#include <string.h>

/* The PSG runs at 4 MiHz and the mixer samples it every 512 cycles, 32768 times a second. The frame
   sequencer clocks the length counters at 256 Hz, the sweep at 128 Hz and the envelopes at 64 Hz. */
#define CYCLES_PER_SAMPLE 512
#define LENGTH_TICK       128
#define SWEEP_TICK        256
#define ENVELOPE_TICK     512

static const uint8_t duty_steps[4] = {1, 2, 4, 6}; /* how many of a duty cycle's eight positions are high */

/* A channel's DAC is on while its envelope starts above 0 or rises (bits 11 to 15 of its envelope), the wave's while
   SOUND3CNT_L's bit 7 is set. With it off a trigger does not start the channel, and turning it off stops the channel
   (the Game Boy's sound, which the GBA keeps: SOUNDCNT_X's status bits in mGBA's test suite, io-read.c) */
static int dac_on(const AgbApu* apu, int channel) {
    return channel == 2 ? (apu->cnt_l[2] & 0x80) != 0 : (apu->cnt_h[channel] & 0xF800) != 0;
}

static void trigger(AgbBoard* board, int channel) {
    AgbApu* apu = &board->apu;
    apu->enabled[channel] = dac_on(apu, channel);
    apu->phase[channel] = 0;
    if (channel < 2) apu->duty_position[channel] = 0;
    if (channel == 2) apu->wave_position = 0;
    if (channel == 3) apu->noise_lfsr = 0x7FFF;
    if (channel == 0) {
        apu->sweep_shadow[0] = apu->cnt_x[0] & 0x7FF;
        apu->sweep_timer[0] = (apu->cnt_l[0] >> 4) & 7;
    }
    /* the envelope reloads its volume and timer */
    apu->envelope_volume[channel] = (int)(apu->cnt_h[channel] >> 12);
    apu->envelope_direction[channel] = apu->cnt_h[channel] & 0x800 ? 1 : -1;
    apu->envelope_period[channel] = (apu->cnt_h[channel] >> 8) & 7;
    apu->envelope_timer[channel] = apu->envelope_period[channel];
    if (!apu->length[channel]) apu->length[channel] = channel == 2 ? 256 : 64;
}

/* ---- registers ---- */

/* SOUND3CNT_L's bit 6 names the wave RAM bank the channel plays (bit 5 makes it 64 digits, that bank then the other);
   the registers at 0x04000090 reach the other bank, while the channel plays too */
static uint32_t cpu_bank(const AgbApu* apu) { return apu->cnt_l[2] & 0x40 ? 0 : 1; }

uint16_t agb_apu_read(AgbBoard* board, uint32_t offset) {
    AgbApu* apu = &board->apu;
    switch (offset) {
        case 0x060: return apu->cnt_l[0] & 0x7F;
        case 0x062: return apu->cnt_h[0];
        case 0x068: return apu->cnt_h[1];
        case 0x064: return (uint16_t)(apu->cnt_x[0] & 0x4000);
        case 0x06C: return (uint16_t)(apu->cnt_x[1] & 0x4000);
        case 0x070: return apu->cnt_l[2] & 0xE0;
        case 0x072: return apu->cnt_h[2] & 0xE0FF;
        case 0x074: return (uint16_t)(apu->cnt_x[2] & 0x4000);
        case 0x078: return apu->cnt_h[3] & 0xFFC0;
        case 0x07C: return (uint16_t)(apu->cnt_x[3] & 0x40FF);
        case 0x080: return apu->soundcnt_l;
        case 0x082: return apu->soundcnt_h;
        case 0x084: {
            uint16_t status = apu->soundcnt_x & 0x80;
            for (int channel = 0; channel < 4; ++channel)
                if (apu->enabled[channel]) status |= 1 << channel;
            return status;
        }
        case 0x088: return apu->soundbias;
        default: break;
    }
    if (offset >= 0x090 && offset <= 0x09E) return snx_ld_le16(&apu->wave_ram[cpu_bank(apu)][offset & 15]);
    return 0;
}

uint16_t agb_apu_written(AgbBoard* board, uint32_t offset) {
    const AgbApu* apu = &board->apu;
    switch (offset) {
        case 0x060: return apu->cnt_l[0];
        case 0x062: return apu->cnt_h[0];
        case 0x068: return apu->cnt_h[1];
        case 0x064: return apu->cnt_x[0];
        case 0x06C: return apu->cnt_x[1];
        case 0x074: return apu->cnt_x[2];
        case 0x07C: return apu->cnt_x[3] & 0x7FFF; /* the noise keeps its trigger bit */
        case 0x070: return apu->cnt_l[2];
        case 0x072: return apu->cnt_h[2];
        case 0x078: return apu->cnt_h[3];
        case 0x080: return apu->soundcnt_l;
        case 0x082: return apu->soundcnt_h;
        case 0x084: return apu->soundcnt_x;
        case 0x088: return apu->soundbias;
        default: break;
    }
    if (offset >= 0x090 && offset <= 0x09E) return snx_ld_le16(&apu->wave_ram[cpu_bank(apu)][offset & 15]);
    return 0;
}

void agb_apu_write(AgbBoard* board, uint32_t offset, uint16_t value) {
    AgbApu* apu = &board->apu;
    int channel;
    switch (offset) {
        case 0x060: apu->cnt_l[0] = value & 0x7F; return;
        /* length, duty and envelope (the wave's volume): the trigger and the length switch are the next register's */
        case 0x062:
        case 0x068:
            channel = offset == 0x062 ? 0 : 1;
            apu->cnt_h[channel] = value;
            apu->length[channel] = 64 - (value & 63);
            if (!dac_on(apu, channel)) apu->enabled[channel] = 0;
            return;
        case 0x072:
            apu->cnt_h[2] = value & 0xE0FF;
            apu->length[2] = 256 - (value & 0xFF);
            return;
        case 0x078:
            apu->cnt_h[3] = value;
            apu->length[3] = 64 - (value & 63);
            if (!dac_on(apu, 3)) apu->enabled[3] = 0;
            return;
        case 0x064:
        case 0x06C:
        case 0x074:
            channel = offset == 0x064 ? 0 : offset == 0x06C ? 1 : 2;
            apu->cnt_x[channel] = value & 0x47FF;
            apu->length_enabled[channel] = value & 0x4000 ? 1 : 0;
            if (value & 0x8000) trigger(board, channel);
            return;
        case 0x07C:
            apu->cnt_x[3] = value & 0xC0FF;
            apu->length_enabled[3] = value & 0x4000 ? 1 : 0;
            if (value & 0x8000) trigger(board, 3);
            return;
        case 0x070:
            /* bit 7 turns the wave channel on, bit 6 picks the bank it plays and bit 5 makes it play both */
            apu->cnt_l[2] = value & 0xE0;
            if (!(value & 0x80)) apu->enabled[2] = 0;
            return;
        case 0x080: apu->soundcnt_l = value & 0xFF77; return;
        case 0x082: {
            apu->soundcnt_h = value;
            /* bits 11 and 15 clear the FIFOs as the write lands */
            if (value & 0x0800) {
                apu->fifo_count[0] = apu->fifo_read[0] = apu->fifo_write[0] = 0;
                apu->soundcnt_h &= (uint16_t)~0x0800;
            }
            if (value & 0x8000) {
                apu->fifo_count[1] = apu->fifo_read[1] = apu->fifo_write[1] = 0;
                apu->soundcnt_h &= (uint16_t)~0x8000;
            }
            return;
        }
        case 0x084:
            apu->soundcnt_x = value & 0x80;
            if (!(value & 0x80)) {
                memset(apu->enabled, 0, sizeof(apu->enabled));
                memset(apu->length, 0, sizeof(apu->length));
            }
            return;
        case 0x088: apu->soundbias = value & 0xC3FF; return;
        default: break;
    }
    if (offset >= 0x090 && offset <= 0x09E) {
        snx_st_le16(&apu->wave_ram[cpu_bank(apu)][offset & 15], value);
        return;
    }
    if (offset == 0x0A0 || offset == 0x0A2) {
        uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
        agb_apu_fifo_write(board, 0, bytes, 2);
    } else if (offset == 0x0A4 || offset == 0x0A6) {
        uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
        agb_apu_fifo_write(board, 1, bytes, 2);
    }
}

void agb_apu_fifo_write(AgbBoard* board, int fifo, const void* bytes, uint32_t size) {
    AgbApu* apu = &board->apu;
    const uint8_t* p = (const uint8_t*)bytes;
    for (uint32_t i = 0; i < size; ++i) {
        if (apu->fifo_count[fifo] == AGB_FIFO_CAPACITY) break;
        apu->fifo[fifo][apu->fifo_write[fifo]] = (int8_t)p[i];
        apu->fifo_write[fifo] = (uint8_t)((apu->fifo_write[fifo] + 1) % AGB_FIFO_CAPACITY);
        ++apu->fifo_count[fifo];
    }
}

static int fifo_pop(AgbApu* apu, int fifo) {
    if (!apu->fifo_count[fifo]) return 0;
    int8_t sample = apu->fifo[fifo][apu->fifo_read[fifo]];
    apu->fifo_read[fifo] = (uint8_t)((apu->fifo_read[fifo] + 1) % AGB_FIFO_CAPACITY);
    --apu->fifo_count[fifo];
    return sample;
}

/* ---- the frame sequencer ---- */

static void tick_length(AgbApu* apu) {
    for (int channel = 0; channel < 4; ++channel) {
        if (!apu->length_enabled[channel] || !apu->length[channel]) continue;
        if (--apu->length[channel]) continue;
        apu->enabled[channel] = 0;
    }
}

static void tick_envelope(AgbApu* apu) {
    for (int channel = 0; channel < 4; ++channel) {
        if (!apu->envelope_period[channel] || !apu->enabled[channel]) continue;
        if (--apu->envelope_timer[channel]) continue;
        apu->envelope_timer[channel] = apu->envelope_period[channel];
        {
            int volume = apu->envelope_volume[channel] + apu->envelope_direction[channel];
            if (volume >= 0 && volume <= 15) apu->envelope_volume[channel] = volume;
        }
    }
}

static void tick_sweep(AgbApu* apu) {
    uint32_t period = (apu->cnt_l[0] >> 4) & 7;
    if (!apu->enabled[0] || !period) return;
    if (--apu->sweep_timer[0]) return;
    apu->sweep_timer[0] = period;
    uint32_t shift = apu->cnt_l[0] & 7;
    if (!shift) return;
    int shadow = apu->sweep_shadow[0];
    int frequency = apu->cnt_l[0] & 0x8 ? shadow - (shadow >> shift) : shadow + (shadow >> shift);
    if (frequency > 0x7FF) {
        apu->enabled[0] = 0;
        return;
    }
    apu->sweep_shadow[0] = frequency;
    apu->cnt_x[0] = (uint16_t)((apu->cnt_x[0] & ~(uint16_t)0x7FF) | frequency);
}

/* ---- channels ---- */

/* Each channel's DAC centers its levels on zero: a playing square is high or low, the wave channel
   plays its four bit digits, and a stopped channel contributes nothing at all. */
static int channel_sample(AgbBoard* board, int channel) {
    AgbApu* apu = &board->apu;
    if (!apu->enabled[channel] || !(apu->soundcnt_x & 0x80)) return 0;
    if (channel < 2) {
        int position = apu->duty_position[channel] & 7;
        int high = position < duty_steps[apu->cnt_h[channel] >> 6 & 3];
        uint32_t frequency = (uint32_t)(apu->cnt_x[channel] & 0x7FF);
        /* eight duty positions per wave cycle, 131072 Hz at the middle frequency */
        apu->phase[channel] += 0x10000u * 32 / (2048u - frequency);
        apu->duty_position[channel] = (uint8_t)((apu->duty_position[channel] + (apu->phase[channel] >> 16)) & 7);
        apu->phase[channel] &= 0xFFFF;
        return high ? apu->envelope_volume[channel] : -apu->envelope_volume[channel];
    }
    if (channel == 2) {
        /* the played bank's 32 digits, or with bit 5 both banks' 64 from the played one on; each byte's high digit
           first */
        uint32_t digits = apu->cnt_l[2] & 0x20 ? 63 : 31,
                 bank = (apu->cnt_l[2] >> 6 & 1) ^ (apu->wave_position >> 5 & 1);
        uint8_t byte = apu->wave_ram[bank][(apu->wave_position >> 1) & 15];
        int sample = apu->wave_position & 1 ? byte & 15 : byte >> 4;
        uint32_t frequency = (uint32_t)(apu->cnt_x[2] & 0x7FF);
        /* 2097152 / (2048 - n) digits a second: 64 / (2048 - n) of them each output sample */
        apu->phase[2] += 0x10000u * 64 / (2048u - frequency);
        apu->wave_position = (uint8_t)((apu->wave_position + (apu->phase[2] >> 16)) & digits);
        apu->phase[2] &= 0xFFFF;
        int centered = sample * 2 - 15;
        if (apu->cnt_h[2] & 0x8000) return centered * 3 >> 2; /* bit 15 forces 75 percent */
        switch (apu->cnt_h[2] >> 13 & 3) {
            case 0: return 0;
            case 2: return centered >> 1;
            case 3: return centered >> 2;
            default: return centered;
        }
    }
    /* the noise channel clocks a linear feedback register at 524288 Hz / r / 2^(s+1), r 0 counting as a half */
    {
        uint32_t code = (uint32_t)(apu->cnt_x[3] & 7), shift = (uint32_t)(apu->cnt_x[3] >> 4 & 15);
        uint32_t rate = code ? 262144u / (code << shift) : 524288u >> shift;
        apu->phase[3] += rate;
        while (apu->phase[3] >= AGB_APU_RATE) {
            uint32_t bit = (apu->noise_lfsr & 1) ^ ((apu->noise_lfsr >> 1) & 1);
            apu->noise_lfsr = (uint16_t)((apu->noise_lfsr >> 1) | (bit << 14));
            if (apu->cnt_x[3] & 0x80) apu->noise_lfsr = (uint16_t)((apu->noise_lfsr & ~(1u << 6)) | (bit << 6));
            apu->phase[3] -= AGB_APU_RATE;
        }
        int high = !(apu->noise_lfsr & 1);
        return high ? apu->envelope_volume[3] : -apu->envelope_volume[3];
    }
}

/* The mixer, at mGBA's levels: the PSG channels (each twice its level, centered on zero) at their master volume, 1 to 8
   eighths, and SOUNDCNT_H's 25, 50 or 100 percent; the FIFOs at twice or four times their bytes; the sum clipped to
   the 10 bit DAC's range around SOUNDBIAS's level, then scaled to 16 bits as mGBA's full volume scales it */
static void mix_sample(AgbBoard* board, int16_t* left, int16_t* right) {
    AgbApu* apu = &board->apu;
    int psg_left = 0, psg_right = 0, l, r, shift = 4 - (apu->soundcnt_h & 3), bias = apu->soundbias & 0x3FE;
    if (!(apu->soundcnt_x & 0x80)) {
        *left = *right = 0;
        return;
    }
    for (int channel = 0; channel < 4; ++channel) {
        int sample = channel_sample(board, channel);
        if (apu->soundcnt_l & (0x100 << channel)) psg_right += sample;
        if (apu->soundcnt_l & (0x1000 << channel)) psg_left += sample;
    }
    l = psg_left * 4 * (1 + (apu->soundcnt_l >> 4 & 7)) >> shift;
    r = psg_right * 4 * (1 + (apu->soundcnt_l & 7)) >> shift;
    for (int fifo = 0; fifo < 2; ++fifo) {
        int sample = apu->fifo_sample[fifo] * 4;                /* held between its timer's overflows */
        if (!(apu->soundcnt_h & (4 << fifo))) sample >>= 1;     /* the 50 percent volume setting */
        if (apu->soundcnt_h & (0x100 << fifo * 4)) r += sample; /* bits 8 and 12: to the right */
        if (apu->soundcnt_h & (0x200 << fifo * 4)) l += sample; /* bits 9 and 13: to the left */
    }
    l = (l + bias < 0 ? 0 : l + bias > 0x3FF ? 0x3FF : l + bias) - bias;
    r = (r + bias < 0 ? 0 : r + bias > 0x3FF ? 0x3FF : r + bias) - bias;
    *left = (int16_t)(l * 48);
    *right = (int16_t)(r * 48);
}

void agb_apu_timer_overflow(AgbBoard* board, int timer) {
    AgbApu* apu = &board->apu;
    for (int fifo = 0; fifo < 2; ++fifo) {
        if ((int)(apu->soundcnt_h >> (10 + fifo * 4) & 1) != timer) continue; /* bits 10 and 14: its timer */
        apu->fifo_sample[fifo] = (int8_t)fifo_pop(apu, fifo);
        if (apu->fifo_count[fifo] <= 16) agb_dma_event(board, fifo ? AGB_DMA_FIFO_B : AGB_DMA_FIFO_A);
    }
}

uint32_t agb_apu_run(AgbBoard* board, uint32_t cycles, int16_t* stereo, uint32_t capacity) {
    AgbApu* apu = &board->apu;
    uint32_t written = 0;
    apu->cycles += cycles;
    while (apu->cycles >= CYCLES_PER_SAMPLE && written < capacity) {
        apu->cycles -= CYCLES_PER_SAMPLE;
        mix_sample(board, stereo + written * 2, stereo + written * 2 + 1);
        ++written;
        if (++apu->ticks % LENGTH_TICK == 0) tick_length(apu);
        if (apu->ticks % SWEEP_TICK == 0) tick_sweep(apu);
        if (apu->ticks % ENVELOPE_TICK == 0) tick_envelope(apu);
    }
    return written;
}
