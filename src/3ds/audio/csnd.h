/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The CSND sound hardware (csnd.c), as Azahar models it (audio_core/csnd.cpp and csnd.h at azahar-emu/azahar
   6e42de8): 32 channels that play PCM8, PCM16 or IMA-ADPCM from physical memory at their timer's rate, one-shot or
   looping, held or not, with linear interpolation, each at its own volume, mixed at the master volume into the DSP's
   32728 Hz stereo. PSG and noise play silence, as in Azahar. Capture units keep their registers and capture nothing.
   The master volume stays at its 0x8000, unity, as csnd:SND never sets it */
#ifndef SNX_CTR_CSND_HW_H
#define SNX_CTR_CSND_HW_H
#include "3ds/memory/ctr_memory.h"
#include <stdbool.h>
#include <stdint.h>

#define SNX_CTR_CSND_CHANNELS 32
#define SNX_CTR_CSND_CAPTURES 2
/* a channel's control register (CNT) */
enum {
    SNX_CTR_CSND_INTERPOLATE = 1u << 6, /* linear interpolation between two samples */
    SNX_CTR_CSND_HOLD = 1u << 7,        /* the last sample held after a one-shot block */
    SNX_CTR_CSND_RUN = 1u << 14,        /* runs (0: paused) */
    SNX_CTR_CSND_ENABLE = 1u << 15,     /* started, busy */
    SNX_CTR_CSND_LOOP_SHIFT = 10,       /* bits 10-11: 0 manual, 1 normal, 2 one-shot, 3 normal without reload */
    SNX_CTR_CSND_FORMAT_SHIFT = 12      /* bits 12-13: 0 PCM8, 1 PCM16, 2 IMA-ADPCM, 3 PSG or noise */
};
#define SNX_CTR_CSND_LOOP_MASK      (3u << SNX_CTR_CSND_LOOP_SHIFT)
#define SNX_CTR_CSND_FORMAT_MASK    (3u << SNX_CTR_CSND_FORMAT_SHIFT)
#define SNX_CTR_CSND_LOOP_RELOAD    (1u << 31) /* a loop ADPCM state reloaded at the loop */
#define SNX_CTR_CSND_CAPTURE_ENABLE (1u << 15)
typedef struct {
    uint16_t cnt, timer, volume[2], capture_volume[2];
    uint32_t start, size, loop, adpcm_state, loop_adpcm_state;
    /* the block playing: its address and the size latched for it, the byte reached and the ADPCM nibble */
    uint32_t block, block_size, position, counter; /* counter: the timer in 1/2048 output samples */
    int16_t previous, current, predictor;
    uint8_t index, high_nibble, holding; /* holding: a one-shot ended with HOLD set, its last sample out */
} SnxCtrCsndChannel;
typedef struct {
    SnxCtrCsndChannel channel[SNX_CTR_CSND_CHANNELS];
    struct {
        uint16_t cnt, timer;
        uint32_t address, size;
    } capture[SNX_CTR_CSND_CAPTURES];
    uint32_t channel_mask;
    uint8_t capture_mask;
} SnxCtrCsndHw;

/* CNT: a channel enabled starts its block from its start address and ADPCM state, one disabled stops (its ADPCM
   state kept), and HOLD cleared lets a held sample go */
void snx_ctr_csnd_set_cnt(SnxCtrCsndHw* hw, unsigned channel, uint16_t cnt);
void snx_ctr_csnd_set_volume(SnxCtrCsndHw* hw, unsigned channel, uint16_t left, uint16_t right);
void snx_ctr_csnd_set_capture_volume(SnxCtrCsndHw* hw, unsigned channel, uint16_t left, uint16_t right);
/* the ADPCM state: predictor in bits 0-15, step index in 16-22; while the channel plays, the decoder's own */
uint32_t snx_ctr_csnd_adpcm_state(const SnxCtrCsndHw* hw, unsigned channel);
bool snx_ctr_csnd_busy(const SnxCtrCsndHw* hw);
/* count stereo samples of the mix, the channels' samples read from memory */
void snx_ctr_csnd_render(SnxCtrCsndHw* hw, SnxCtrMemory* memory, int16_t (*out)[2], unsigned count);
#endif
