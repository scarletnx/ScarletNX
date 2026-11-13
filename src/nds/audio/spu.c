/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2016-2026 melonDS team
   Copyright 2026 the ScarletNX contributors */
/* The channels and the mixer after melonDS's (SPU.cpp at 497aa6c, GPL-3.0-or-later): each channel's timer and
   formats, the ADPCM tables, the pan and volume arithmetic, the master volume. A channel reads its samples from main
   memory where they lie. The capture units keep their registers and capture nothing yet. */
#include "nds/audio/spu.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

static const int8_t adpcm_index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const uint16_t adpcm_table[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015, 0x0017,
    0x0019, 0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042, 0x0049, 0x0050,
    0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117,
    0x0133, 0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292, 0x02D4, 0x031C, 0x036C, 0x03C3,
    0x0424, 0x048E, 0x0502, 0x0583, 0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF,
    0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954, 0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF,
    0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E, 0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF};
/* a square wave's eight steps, by its duty */
static const uint8_t psg_high[8] = {7, 6, 5, 4, 3, 2, 1, 8};

typedef struct {
    uint32_t cnt, source, loop_pos, length;
    uint16_t timer_reload;
    uint32_t timer;
    int32_t pos;
    int16_t current;
    uint16_t noise;
    int32_t adpcm_value, adpcm_index, adpcm_loop_value, adpcm_loop_index;
    uint8_t adpcm_byte, key_on, volume, volume_shift, pan;
} Channel;

#define KEPT 8192u
typedef struct NdsSpu {
    Channel channel[16];
    uint16_t cnt, bias;
    uint8_t capture_cnt[2];
    uint32_t capture_to[2], capture_length[2];
    uint64_t mixed_to; /* the bus cycle the mix reached */
    int16_t kept[KEPT * 2];
    uint32_t kept_at, kept_count;
} NdsSpu;

static NdsSpu* spu_of(NdsBoard* board) { return (NdsSpu*)board->spu; }

bool nds_spu_init(NdsBoard* board) {
    board->spu = calloc(1, sizeof(NdsSpu));
    if (!board->spu) return false;
    spu_of(board)->bias = 0x200;
    return true;
}

void nds_spu_free(NdsBoard* board) {
    free(board->spu);
    board->spu = NULL;
}

static uint8_t sample8(const NdsBoard* board, uint32_t address) {
    return board->mem.main.view[address & (NDS_MAIN_RAM_SIZE - 1)];
}

static void start(Channel* c) {
    c->timer = c->timer_reload;
    c->pos = ((c->cnt >> 29) & 3) == 3 ? -1 : -3;
    c->noise = 0x7FFF;
    c->current = 0;
}

/* the channel's next sample, by its format. A channel that plays once stops at its end */
static void next_sample(const NdsBoard* board, Channel* c, unsigned format, unsigned index) {
    unsigned repeat = (c->cnt >> 27) & 3;
    uint32_t end = c->loop_pos + c->length;
    ++c->pos;
    if (format == 3) {
        if (index >= 14) { /* noise */
            --c->pos;
            if (c->noise & 1) {
                c->noise = (uint16_t)((c->noise >> 1) ^ 0x6000);
                c->current = -0x7FFF;
            } else {
                c->noise >>= 1;
                c->current = 0x7FFF;
            }
        } else if (index >= 8) {
            unsigned duty = (c->cnt >> 24) & 7;
            c->current = (unsigned)(c->pos & 7) >= psg_high[duty] ? 0x7FFF : -0x7FFF;
        } else
            c->current = 0;
        return;
    }
    if (format < 2) {
        if (c->pos < 0) return;
        uint32_t at = format ? (uint32_t)c->pos << 1 : (uint32_t)c->pos;
        if (at >= end) {
            if (repeat & 1)
                c->pos = (int32_t)(format ? c->loop_pos >> 1 : c->loop_pos);
            else if (repeat & 2) {
                c->current = 0;
                c->cnt &= ~(1u << 31);
                return;
            }
        }
        if (format) {
            uint32_t address = c->source + ((uint32_t)c->pos << 1);
            c->current = (int16_t)(sample8(board, address) | sample8(board, address + 1) << 8);
        } else
            c->current = (int16_t)((int8_t)sample8(board, c->source + (uint32_t)c->pos) * 256);
        return;
    }
    /* ADPCM: a header of the first value and step, then a nibble a sample */
    if (c->pos < 8) {
        if (c->pos == 0) {
            c->adpcm_value = (int16_t)(sample8(board, c->source) | sample8(board, c->source + 1) << 8);
            c->adpcm_index = sample8(board, c->source + 2) & 0x7F;
            if (c->adpcm_index > 88) c->adpcm_index = 88;
            c->adpcm_loop_value = c->adpcm_value;
            c->adpcm_loop_index = c->adpcm_index;
        }
        return;
    }
    if (((uint32_t)c->pos >> 1) >= end) {
        if (repeat & 1) {
            c->pos = (int32_t)(c->loop_pos << 1);
            c->adpcm_value = c->adpcm_loop_value;
            c->adpcm_index = c->adpcm_loop_index;
            c->adpcm_byte = sample8(board, c->source + c->loop_pos);
        } else if (repeat & 2) {
            c->current = 0;
            c->cnt &= ~(1u << 31);
            return;
        }
    } else {
        if (!(c->pos & 1))
            c->adpcm_byte = sample8(board, c->source + ((uint32_t)c->pos >> 1));
        else
            c->adpcm_byte >>= 4;
        int32_t step = adpcm_table[c->adpcm_index], difference = step >> 3;
        if (c->adpcm_byte & 1) difference += step >> 2;
        if (c->adpcm_byte & 2) difference += step >> 1;
        if (c->adpcm_byte & 4) difference += step;
        if (c->adpcm_byte & 8) {
            c->adpcm_value -= difference;
            if (c->adpcm_value < -0x7FFF) c->adpcm_value = -0x7FFF;
        } else {
            c->adpcm_value += difference;
            if (c->adpcm_value > 0x7FFF) c->adpcm_value = 0x7FFF;
        }
        c->adpcm_index += adpcm_index_table[c->adpcm_byte & 7];
        if (c->adpcm_index < 0) c->adpcm_index = 0;
        if (c->adpcm_index > 88) c->adpcm_index = 88;
        if ((uint32_t)c->pos == c->loop_pos << 1) {
            c->adpcm_loop_value = c->adpcm_value;
            c->adpcm_loop_index = c->adpcm_index;
        }
    }
    c->current = (int16_t)c->adpcm_value;
}

/* a channel's output for one sample of the mix: 512 of its timer's cycles */
static int32_t run(const NdsBoard* board, Channel* c, unsigned index) {
    if (!(c->cnt >> 31)) return 0;
    unsigned format = (c->cnt >> 29) & 3;
    if (format < 3 && c->length + c->loop_pos < 16) return 0;
    if (c->key_on) {
        start(c);
        c->key_on = 0;
    }
    c->timer += 512;
    while (c->timer >> 16) {
        c->timer = c->timer_reload + (c->timer - 0x10000);
        next_sample(board, c, format, index);
        if (!(c->cnt >> 31)) break;
    }
    return ((int32_t)c->current << c->volume_shift) * c->volume;
}

void nds_spu_run(NdsBoard* board) {
    NdsSpu* s = spu_of(board);
    while (board->cycles - s->mixed_to >= NDS_SPU_SAMPLE_CYCLES) {
        s->mixed_to += NDS_SPU_SAMPLE_CYCLES;
        int64_t left = 0, right = 0;
        if (s->cnt & 0x8000) {
            for (unsigned i = 0; i < 16; ++i) {
                Channel* c = &s->channel[i];
                int32_t value = run(board, c, i);
                /* channels 1 and 3 can be kept out of the mixer, for the capture units */
                if ((i == 1 && (s->cnt & 0x1000)) || (i == 3 && (s->cnt & 0x2000))) continue;
                left += ((int64_t)value * (128 - c->pan)) >> 10;
                right += ((int64_t)value * c->pan) >> 10;
            }
        }
        unsigned master = s->cnt & 0x7F;
        if (master == 127) master = 128;
        left = ((left * master) >> 7) >> 8;
        right = ((right * master) >> 7) >> 8;
        if (left < -0x8000) left = -0x8000;
        if (left > 0x7FFF) left = 0x7FFF;
        if (right < -0x8000) right = -0x8000;
        if (right > 0x7FFF) right = 0x7FFF;
        if (s->kept_count == KEPT) { /* the host took none for a while: the oldest goes */
            s->kept_at = (s->kept_at + 1) % KEPT;
            --s->kept_count;
        }
        uint32_t at = (s->kept_at + s->kept_count++) % KEPT;
        s->kept[at * 2] = (int16_t)left, s->kept[at * 2 + 1] = (int16_t)right;
    }
}

uint32_t nds_spu_take(NdsBoard* board, int16_t* out, uint32_t most) {
    NdsSpu* s = spu_of(board);
    uint32_t count = s->kept_count < most ? s->kept_count : most;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = (s->kept_at + i) % KEPT;
        out[i * 2] = s->kept[at * 2], out[i * 2 + 1] = s->kept[at * 2 + 1];
    }
    s->kept_at = (s->kept_at + count) % KEPT;
    s->kept_count -= count;
    return count;
}

static void set_cnt(Channel* c, uint32_t value) {
    static const uint8_t shift[4] = {4, 3, 2, 0};
    uint32_t old = c->cnt;
    c->cnt = value & 0xFF7F837Fu;
    c->volume = c->cnt & 0x7F;
    if (c->volume == 127) c->volume = 128;
    c->volume_shift = shift[(c->cnt >> 8) & 3];
    c->pan = (c->cnt >> 16) & 0x7F;
    if (c->pan == 127) c->pan = 128;
    if ((value >> 31) && !(old >> 31)) c->key_on = 1;
}

uint32_t nds_spu_read(NdsBoard* board, uint32_t address, unsigned bytes) {
    NdsSpu* s = spu_of(board);
    uint32_t offset = address & 0xFFF, value = 0;
    if (offset >= 0x400 && offset < 0x500) {
        const Channel* c = &s->channel[(offset >> 4) & 15];
        if ((offset & 0xC) == 0) value = c->cnt;
    } else if ((offset & ~3u) == 0x500)
        value = s->cnt;
    else if ((offset & ~3u) == 0x504)
        value = s->bias;
    else if ((offset & ~3u) == 0x508)
        value = s->capture_cnt[0] | (uint32_t)s->capture_cnt[1] << 8;
    else if ((offset & ~3u) == 0x510 || (offset & ~3u) == 0x518)
        value = s->capture_to[(offset >> 3) & 1];
    value >>= (offset & 3) * 8;
    return bytes == 4 ? value : bytes == 2 ? value & 0xFFFF : value & 0xFF;
}

void nds_spu_write(NdsBoard* board, uint32_t address, unsigned bytes, uint32_t value) {
    NdsSpu* s = spu_of(board);
    uint32_t offset = address & 0xFFF, shift = (offset & 3) * 8;
    uint32_t mask = (bytes == 4 ? 0xFFFFFFFFu : bytes == 2 ? 0xFFFFu : 0xFFu) << shift;
    value = (value << shift) & mask;
    nds_spu_run(board); /* what was playing up to now is mixed as it was */
    if (offset >= 0x400 && offset < 0x500) {
        Channel* c = &s->channel[(offset >> 4) & 15];
        switch (offset & 0xC) {
            case 0x0: set_cnt(c, (c->cnt & ~mask) | value); break;
            case 0x4: c->source = ((c->source & ~mask) | value) & 0x07FFFFFCu; break;
            case 0x8: { /* the timer in the low half, the loop's start, in words, in the high half */
                uint32_t both = (((c->loop_pos >> 2) << 16 | c->timer_reload) & ~mask) | value;
                c->timer_reload = (uint16_t)both;
                c->loop_pos = (both >> 16) << 2;
                break;
            }
            default: c->length = ((((c->length >> 2) & ~mask) | value) & 0x003FFFFFu) << 2; break;
        }
        return;
    }
    switch (offset & ~3u) {
        case 0x500: s->cnt = (uint16_t)(((s->cnt & ~mask) | value) & 0xBF7F); break;
        case 0x504: s->bias = (uint16_t)(((s->bias & ~mask) | value) & 0x3FF); break;
        case 0x508:
            if (mask & 0xFF) s->capture_cnt[0] = value & 0x8F;
            if (mask & 0xFF00) s->capture_cnt[1] = (value >> 8) & 0x8F;
            break;
        case 0x510:
        case 0x518: s->capture_to[(offset >> 3) & 1] = ((s->capture_to[(offset >> 3) & 1] & ~mask) | value) & 0x07FFFFFCu; break;
        case 0x514:
        case 0x51C: s->capture_length[(offset >> 3) & 1] = value & 0xFFFF; break;
        default: break;
    }
}
