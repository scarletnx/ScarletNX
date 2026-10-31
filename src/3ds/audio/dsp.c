/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The HLE DSP's frame loop, pipes and power states. Ported from Azahar audio_core/hle/hle.cpp (GPL-2.0-or-later,
   Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e. The shared
   memory lives in DSP RAM, which the DSP reads and writes as a bus master (snx_ctr_phys_read/write), never through
   a host pointer. */
#include "3ds/audio/aac.h"
#include "3ds/audio/internal.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

#define DSP_RAM_SIZE 0x80000u

typedef struct {
    uint8_t* data;
    size_t size, capacity;
} Pipe;
struct SnxCtrDsp {
    SnxCtrMemory* memory;
    SnxCtrDspInterrupt interrupt;
    void* context;
    SnxCtrDspState state;
    Pipe pipes[SNX_CTR_DSP_PIPES];
    DspSource sources[DSP_SOURCES];
    DspMixers mixers;
    uint8_t* backup; /* DSP RAM across sleep, DSP_RAM_SIZE */
    SnxCtrAac* aac;   /* the binary pipe's AAC decoder, made at its first message */
};

static bool pipe_append(Pipe* pipe, const void* data, size_t size) {
    if (pipe->size + size > pipe->capacity) {
        size_t capacity = pipe->capacity ? pipe->capacity : 64;
        while (capacity < pipe->size + size) capacity *= 2;
        uint8_t* grown = realloc(pipe->data, capacity);
        if (!grown) return false;
        pipe->data = grown;
        pipe->capacity = capacity;
    }
    memcpy(pipe->data + pipe->size, data, size);
    pipe->size += size;
    return true;
}
static void reset_pipes(SnxCtrDsp* dsp) {
    for (unsigned i = 0; i < SNX_CTR_DSP_PIPES; ++i) dsp->pipes[i].size = 0;
}
static uint32_t region(unsigned index) { return SNX_CTR_DSP_RAM + (index ? DSP_REGION1 : DSP_REGION0); }
static bool ram_read(SnxCtrDsp* dsp, uint32_t address, void* out, size_t size) {
    return snx_ctr_phys_read(dsp->memory, address, out, size) == SNX_CTR_MEM_OK;
}
static void ram_write(SnxCtrDsp* dsp, uint32_t address, const void* data, size_t size) {
    (void)snx_ctr_phys_write(dsp->memory, address, data, size);
}
static void ram_clear(SnxCtrDsp* dsp) {
    static const uint8_t zero[0x1000];
    for (uint32_t at = 0; at < DSP_RAM_SIZE; at += sizeof zero) ram_write(dsp, SNX_CTR_DSP_RAM + at, zero, sizeof zero);
}

SnxCtrDsp* snx_ctr_dsp_new(SnxCtrMemory* memory, SnxCtrDspInterrupt interrupt, void* context) {
    SnxCtrDsp* dsp = calloc(1, sizeof *dsp);
    if (!dsp) return NULL;
    dsp->memory = memory;
    dsp->interrupt = interrupt;
    dsp->context = context;
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_reset(&dsp->sources[i]);
    dsp_mixers_reset(&dsp->mixers);
    ram_clear(dsp);
    return dsp;
}
void snx_ctr_dsp_free(SnxCtrDsp* dsp) {
    if (!dsp) return;
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_free(&dsp->sources[i]);
    dsp_mixers_free(&dsp->mixers);
    for (unsigned i = 0; i < SNX_CTR_DSP_PIPES; ++i) free(dsp->pipes[i].data);
    snx_ctr_aac_free(dsp->aac);
    free(dsp->backup);
    free(dsp);
}
SnxCtrDspState snx_ctr_dsp_state(const SnxCtrDsp* dsp) { return dsp->state; }

/* Answers on the audio pipe with the DSP RAM addresses of the region's structures, in DSP words: a count, then the
   15 addresses (see also ConvertProcessAddressFromDspDram) */
static void write_struct_addresses(SnxCtrDsp* dsp) {
    static const uint16_t offsets[15] = {DSP_FRAME_COUNTER, DSP_SOURCE_CONFIGS, DSP_SOURCE_STATUSES, DSP_ADPCM_COEFFS,
                                         DSP_CONFIGURATION, DSP_STATUS,         DSP_FINAL_SAMPLES,   DSP_INTERMEDIATE,
                                         DSP_COMPRESSOR,    DSP_DEBUG,          DSP_UNKNOWN10,       DSP_UNKNOWN11,
                                         DSP_UNKNOWN12,     DSP_UNKNOWN13,      DSP_UNKNOWN14};
    uint8_t words[2 * 16];
    words[0] = 15;
    words[1] = 0;
    for (unsigned i = 0; i < 15; ++i) {
        uint16_t address = (uint16_t)(0x8000 + offsets[i] / 2);
        words[2 + i * 2] = (uint8_t)address;
        words[3 + i * 2] = (uint8_t)(address >> 8);
    }
    (void)pipe_append(&dsp->pipes[SNX_CTR_DSP_PIPE_AUDIO], words, sizeof words);
    dsp->interrupt(dsp->context, SNX_CTR_DSP_PIPE_AUDIO);
}
/* Initialize: not quite what the hardware resets, but a clean, consistent state every time, as Azahar does */
static void initialize(SnxCtrDsp* dsp) {
    ram_clear(dsp);
    dsp_mixers_reset(&dsp->mixers);
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_reset(&dsp->sources[i]);
    reset_pipes(dsp);
}
/* Applications save the structures sleep garbles and restore them after wakeup themselves */
static void sleep_state(SnxCtrDsp* dsp) {
    if (!dsp->backup) dsp->backup = malloc(DSP_RAM_SIZE);
    if (dsp->backup && !ram_read(dsp, SNX_CTR_DSP_RAM, dsp->backup, DSP_RAM_SIZE)) memset(dsp->backup, 0, DSP_RAM_SIZE);
    dsp_mixers_sleep(&dsp->mixers);
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_sleep(&dsp->sources[i]);
}
static void wakeup(SnxCtrDsp* dsp) {
    if (dsp->backup) {
        ram_write(dsp, SNX_CTR_DSP_RAM, dsp->backup, DSP_RAM_SIZE);
        memset(dsp->backup, 0, DSP_RAM_SIZE);
    }
    dsp_mixers_wakeup(&dsp->mixers);
    for (unsigned i = 0; i < DSP_SOURCES; ++i) dsp_source_wakeup(&dsp->sources[i]);
    reset_pipes(dsp);
}

const char* snx_ctr_dsp_pipe_write(SnxCtrDsp* dsp, unsigned pipe, const uint8_t* data, size_t size) {
    if (pipe == SNX_CTR_DSP_PIPE_AUDIO) {
        /* the state changes; another length is an application error the DSP ignores */
        if (size != 4) return NULL;
        switch (data[0]) {
            case 0: /* Initialize */
                initialize(dsp);
                write_struct_addresses(dsp);
                dsp->state = SNX_CTR_DSP_ON;
                break;
            case 1: dsp->state = SNX_CTR_DSP_OFF; break; /* Shutdown */
            case 2:                                      /* Wakeup */
                wakeup(dsp);
                write_struct_addresses(dsp);
                dsp->state = SNX_CTR_DSP_ON;
                break;
            case 3: /* Sleep */
                sleep_state(dsp);
                write_struct_addresses(dsp);
                dsp->state = SNX_CTR_DSP_SLEEPING;
                break;
            default: dsp->state = SNX_CTR_DSP_OFF; break;
        }
        return NULL;
    }
    if (pipe == SNX_CTR_DSP_PIPE_BINARY) {
        /* Azahar takes only the decoder's message here (it asserts on any other size or codec). The answer replaces
           what the pipe held, and its interrupt is raised before the write returns */
        if (size != SNX_CTR_AAC_MESSAGE) return "the DSP's binary pipe takes the 32-byte message of its AAC decoder";
        if (snx_ld_le16(data) != 1) return "the DSP's binary pipe takes only AAC decoding (codec 1), not the AAC encoder";
        if (!dsp->aac && !(dsp->aac = snx_ctr_aac_new(dsp->memory))) return "the DSP's AAC decoder has no memory";
        uint8_t response[SNX_CTR_AAC_MESSAGE];
        snx_ctr_aac_request(dsp->aac, data, response);
        dsp->pipes[SNX_CTR_DSP_PIPE_BINARY].size = 0;
        if (!pipe_append(&dsp->pipes[SNX_CTR_DSP_PIPE_BINARY], response, sizeof response))
            return "the DSP's AAC decoder has no memory";
        dsp->interrupt(dsp->context, SNX_CTR_DSP_PIPE_BINARY);
        return NULL;
    }
    return "only the DSP's audio and binary pipes are implemented";
}
size_t snx_ctr_dsp_pipe_readable(const SnxCtrDsp* dsp, unsigned pipe) {
    return pipe < SNX_CTR_DSP_PIPES ? dsp->pipes[pipe].size : 0;
}
size_t snx_ctr_dsp_pipe_read(SnxCtrDsp* dsp, unsigned pipe, uint8_t* out, size_t size) {
    if (pipe >= SNX_CTR_DSP_PIPES) return 0;
    Pipe* p = &dsp->pipes[pipe];
    if (size > p->size) size = p->size;
    memcpy(out, p->data, size);
    memmove(p->data, p->data + size, p->size - size);
    p->size -= size;
    return size;
}

/* The region with the higher frame counter is the one the application finished, unless the counter wrapped */
static unsigned current_region(SnxCtrDsp* dsp) {
    uint16_t counter[2] = {0, 0};
    for (unsigned i = 0; i < 2; ++i) (void)ram_read(dsp, region(i) + DSP_FRAME_COUNTER, &counter[i], 2);
    if (counter[0] == 0xffff && counter[1] != 0xfffe) return 1;
    if (counter[1] == 0xffff && counter[0] != 0xfffe) return 0;
    return counter[0] > counter[1] ? 0 : 1;
}

bool snx_ctr_dsp_tick(SnxCtrDsp* dsp, int16_t frame[SNX_CTR_DSP_SAMPLES][2]) {
    if (dsp->state != SNX_CTR_DSP_ON) return false;
    unsigned current = current_region(dsp);
    uint32_t read = region(current), write = region(!current);
    /* about 24 KiB of the region, copied in and out */
    DspSourceConfig configs[DSP_SOURCES];
    int16_t coeffs[DSP_SOURCES][16];
    DspSourceStatus statuses[DSP_SOURCES];
    DspIntermediate read_samples, write_samples;
    DspConfig config;
    (void)ram_read(dsp, read + DSP_SOURCE_CONFIGS, configs, sizeof configs);
    (void)ram_read(dsp, read + DSP_ADPCM_COEFFS, coeffs, sizeof coeffs);
    (void)ram_read(dsp, read + DSP_CONFIGURATION, &config, sizeof config);
    (void)ram_read(dsp, read + DSP_INTERMEDIATE, &read_samples, sizeof read_samples);
    (void)ram_read(dsp, write + DSP_INTERMEDIATE, &write_samples, sizeof write_samples);
    /* the intermediate mixes: every source's frame mixed into each with its gains */
    DspQuadFrame mixes[3] = {0};
    for (unsigned i = 0; i < DSP_SOURCES; i++) {
        uint32_t dirty = configs[i].dirty;
        uint16_t buffers_dirty = configs[i].buffers_dirty;
        statuses[i] = dsp_source_tick(&dsp->sources[i], dsp->memory, &configs[i], coeffs[i]);
        for (unsigned mix = 0; mix < 3; mix++) {
            dsp_source_mix(&dsp->sources[i], mixes[mix], mix);
            dsp_source_mix_done(&dsp->sources[i], mix);
        }
        /* the dirty bits it took are cleared in the application's region, as the firmware clears them */
        uint32_t at = read + DSP_SOURCE_CONFIGS + i * (uint32_t)sizeof(DspSourceConfig);
        if (dirty != configs[i].dirty) ram_write(dsp, at, &configs[i].dirty, 4);
        if (buffers_dirty != configs[i].buffers_dirty)
            ram_write(dsp, at + offsetof(DspSourceConfig, buffers_dirty), &configs[i].buffers_dirty, 2);
    }
    ram_write(dsp, write + DSP_SOURCE_STATUSES, statuses, sizeof statuses);
    /* the final mix */
    DspConfig taken = config;
    if (dsp_mixers_tick(&dsp->mixers, &config, &read_samples, &write_samples, mixes))
        ram_write(dsp, write + DSP_INTERMEDIATE, &write_samples, sizeof write_samples);
    if (taken.dirty != config.dirty) ram_write(dsp, read + DSP_CONFIGURATION, &config.dirty, 4);
    for (unsigned i = 0; i < 2; ++i)
        if (taken.delay[i].dirty != config.delay[i].dirty)
            ram_write(dsp, read + DSP_CONFIGURATION + (uint32_t)offsetof(DspConfig, delay) + i * 20u,
                      &config.delay[i].dirty, 2);
    const uint16_t status[2] = {0, 0}; /* unknown, dropped_frames */
    ram_write(dsp, write + DSP_STATUS, status, sizeof status);
    memcpy(frame, dsp->mixers.frame, sizeof dsp->mixers.frame);
    ram_write(dsp, write + DSP_FINAL_SAMPLES, dsp->mixers.frame, sizeof dsp->mixers.frame);
    dsp->interrupt(dsp->context, SNX_CTR_DSP_PIPE_AUDIO);
    return true;
}
