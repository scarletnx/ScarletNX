/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The HLE DSP and dsp::DSP: the audio pipe's struct addresses, region selection, a PCM16 source played through the
   gain ramp, mixer and final samples, buffer end statuses, GC-ADPCM decoding and linear interpolation against hand
   computed values, the AAC decoder on the binary pipe, then the service as libctru's ndspInit drives it. */
#include "3ds/audio/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/dsp.h"
#include "common/utilities/endian.h"
#include "support/3ds_guest.h"
#include "support/dsp_aac_fixture.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FCRAM   0x20000000u
#define SAMPLES (FCRAM + 0x100000u)
#define R0      (SNX_CTR_DSP_RAM + DSP_REGION0)
#define R1      (SNX_CTR_DSP_RAM + DSP_REGION1)

static unsigned raised[SNX_CTR_DSP_PIPES];
static void interrupt(void* context, unsigned pipe) {
    (void)context;
    ++raised[pipe];
}
static void put(SnxCtrMemory* m, uint32_t pa, const void* data, size_t size) {
    assert(snx_ctr_phys_write(m, pa, data, size) == SNX_CTR_MEM_OK);
}
static void get(SnxCtrMemory* m, uint32_t pa, void* out, size_t size) {
    assert(snx_ctr_phys_read(m, pa, out, size) == SNX_CTR_MEM_OK);
}
static void counters(SnxCtrMemory* m, uint16_t r0, uint16_t r1) {
    put(m, R0 + DSP_FRAME_COUNTER, &r0, 2);
    put(m, R1 + DSP_FRAME_COUNTER, &r1, 2);
}
static int16_t sample(unsigned k) { return (int16_t)((int)(k * 37 % 2000) - 1000); }
/* a mono source on mix 0 at full gain, no interpolation, its embedded buffer the given one */
static DspSourceConfig source(uint32_t pa, uint32_t length, unsigned format, uint8_t interpolation, float rate) {
    DspSourceConfig c = {0};
    c.dirty = DSP_ENABLE_DIRTY | DSP_INTERPOLATION_DIRTY | DSP_RATE_MULTIPLIER_DIRTY | DSP_GAIN_0_DIRTY |
              DSP_FORMAT_DIRTY | DSP_MONO_OR_STEREO_DIRTY | DSP_EMBEDDED_BUFFER_DIRTY;
    c.enable = 1;
    c.interpolation_mode = interpolation;
    c.rate_multiplier = rate;
    c.gain[0][0] = c.gain[0][1] = 1.0f;
    c.flags1 = (uint16_t)(DSP_MONO | format << 2);
    c.physical_address = dsp32(pa);
    c.length = dsp32(length);
    c.buffer_id = 7;
    return c;
}
static void master(SnxCtrMemory* m, uint32_t region) {
    DspConfig config = {.dirty = DSP_MASTER_VOLUME_DIRTY, .master_volume = 1.0f};
    put(m, region + DSP_CONFIGURATION, &config, sizeof config);
}
static SnxCtrDsp* initialized(SnxCtrMemory* m) {
    memset(raised, 0, sizeof raised);
    SnxCtrDsp* dsp = snx_ctr_dsp_new(m, interrupt, NULL);
    assert(dsp && snx_ctr_dsp_state(dsp) == SNX_CTR_DSP_OFF);
    int16_t frame[SNX_CTR_DSP_SAMPLES][2];
    assert(!snx_ctr_dsp_tick(dsp, frame)); /* off: no frame, no interrupt */
    const uint8_t init[4] = {0, 0, 0, 0};
    assert(!snx_ctr_dsp_pipe_write(dsp, SNX_CTR_DSP_PIPE_AUDIO, init, 4));
    assert(snx_ctr_dsp_state(dsp) == SNX_CTR_DSP_ON && raised[SNX_CTR_DSP_PIPE_AUDIO] == 1);
    return dsp;
}

static void pipes_and_regions(SnxCtrMemory* m) {
    SnxCtrDsp* dsp = initialized(m);
    /* a count, then the 15 structures' DSP-word addresses, frame counter first */
    assert(snx_ctr_dsp_pipe_readable(dsp, SNX_CTR_DSP_PIPE_AUDIO) == 32);
    uint8_t pipe[32];
    assert(snx_ctr_dsp_pipe_read(dsp, SNX_CTR_DSP_PIPE_AUDIO, pipe, 2) == 2 && pipe[0] == 15 && !pipe[1]);
    assert(snx_ctr_dsp_pipe_read(dsp, SNX_CTR_DSP_PIPE_AUDIO, pipe, 64) == 30);
    const uint16_t expect[15] = {0xbfff, 0x9e92, 0x8680, 0xa792, 0x9430, 0x8400, 0x8540, 0x9492,
                                 0x8710, 0x8410, 0xa912, 0xaa12, 0xaad2, 0xac52, 0xac5c};
    for (unsigned i = 0; i < 15; ++i) assert((pipe[i * 2] | pipe[i * 2 + 1] << 8) == expect[i]);
    assert(!snx_ctr_dsp_pipe_readable(dsp, SNX_CTR_DSP_PIPE_AUDIO));
    /* the region with the higher frame counter is read, the other written, unless the counter wrapped */
    int16_t frame[SNX_CTR_DSP_SAMPLES][2];
    const uint16_t pairs[4][2] = {{5, 4}, {4, 5}, {0xffff, 0}, {0, 0xffff}};
    const uint32_t written[4] = {R1, R0, R0, R1}; /* after 0xffff, 0 is the newer */
    for (unsigned i = 0; i < 4; ++i) {
        counters(m, pairs[i][0], pairs[i][1]);
        const uint16_t marker = 0x5a5a, cleared[2] = {0, 0};
        put(m, R0 + DSP_STATUS, &marker, 2);
        put(m, R1 + DSP_STATUS, &marker, 2);
        assert(snx_ctr_dsp_tick(dsp, frame));
        uint16_t status[2];
        get(m, written[i] + DSP_STATUS, status, 4);
        assert(!memcmp(status, cleared, 4));
        get(m, (written[i] == R0 ? R1 : R0) + DSP_STATUS, status, 2);
        assert(status[0] == marker);
    }
    assert(raised[SNX_CTR_DSP_PIPE_AUDIO] == 5);
    /* shut down, the DSP makes no more frames */
    const uint8_t shutdown[4] = {1, 0, 0, 0};
    assert(!snx_ctr_dsp_pipe_write(dsp, SNX_CTR_DSP_PIPE_AUDIO, shutdown, 4));
    assert(snx_ctr_dsp_state(dsp) == SNX_CTR_DSP_OFF && !snx_ctr_dsp_tick(dsp, frame));
    snx_ctr_dsp_free(dsp);
}

static void pcm16_source(SnxCtrMemory* m) {
    SnxCtrDsp* dsp = initialized(m);
    int16_t pcm[400];
    for (unsigned k = 0; k < 400; ++k) pcm[k] = sample(k);
    put(m, SAMPLES, pcm, sizeof pcm);
    counters(m, 2, 1);
    DspSourceConfig config = source(SAMPLES, 400, DSP_PCM16, DSP_NONE, 1.0f);
    put(m, R0 + DSP_SOURCE_CONFIGS, &config, sizeof config);
    master(m, R0);
    int16_t frame[SNX_CTR_DSP_SAMPLES][2], final[SNX_CTR_DSP_SAMPLES][2];
    /* the frame that dequeues the buffer plays nothing yet (its gain ramp from 0 goes with it) */
    assert(snx_ctr_dsp_tick(dsp, frame));
    for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; ++n) assert(!frame[n][0] && !frame[n][1]);
    DspSourceStatus status;
    get(m, R1 + DSP_SOURCE_STATUSES, &status, sizeof status);
    assert(status.is_enabled && !status.current_buffer_id_dirty && !dsp32(status.buffer_position) &&
           status.current_buffer_id == 7 && !status.last_buffer_id);
    /* the dirty bits taken are cleared where the application wrote them */
    DspSourceConfig after;
    DspConfig mixer;
    get(m, R0 + DSP_SOURCE_CONFIGS, &after, sizeof after);
    get(m, R0 + DSP_CONFIGURATION, &mixer, sizeof mixer);
    assert(!after.dirty && !mixer.dirty);
    /* then the samples, after a two-sample predelay, at full gain */
    assert(snx_ctr_dsp_tick(dsp, frame));
    for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; ++n)
        assert(frame[n][0] == (n < 2 ? 0 : pcm[n - 2]) && frame[n][1] == frame[n][0]);
    get(m, R1 + DSP_FINAL_SAMPLES, final, sizeof final);
    assert(!memcmp(final, frame, sizeof final));
    get(m, R1 + DSP_SOURCE_STATUSES, &status, sizeof status);
    /* the interpolation keeps its position a sample behind: 159 of the buffer are consumed */
    assert(dsp32(status.buffer_position) == 159);
    /* a new gain ramps from the old one across the next frame */
    config.dirty = DSP_GAIN_0_DIRTY;
    config.gain[0][0] = config.gain[0][1] = 0.5f;
    put(m, R0 + DSP_SOURCE_CONFIGS, &config, sizeof config);
    assert(snx_ctr_dsp_tick(dsp, frame));
    for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; ++n) {
        float progress = (float)n * (1.0f / 159.0f), gain = 1.0f + (0.5f - 1.0f) * progress;
        int16_t want = (int16_t)(int32_t)(gain * pcm[158 + n]);
        assert(frame[n][0] == want && frame[n][1] == want);
    }
    /* the fourth runs out 80 samples in and is silent after */
    assert(snx_ctr_dsp_tick(dsp, frame));
    for (unsigned n = 0; n < SNX_CTR_DSP_SAMPLES; ++n)
        assert(frame[n][0] == (n < 80 ? (int16_t)(int32_t)(0.5f * pcm[318 + n]) : 0));
    get(m, R1 + DSP_SOURCE_STATUSES, &status, sizeof status);
    assert(status.is_enabled && dsp32(status.buffer_position) == 400);
    /* the fifth finds the queue empty: the source stops, reporting the buffer it finished */
    assert(snx_ctr_dsp_tick(dsp, frame));
    get(m, R1 + DSP_SOURCE_STATUSES, &status, sizeof status);
    assert(!status.is_enabled && status.current_buffer_id_dirty && !status.current_buffer_id &&
           status.last_buffer_id == 7);
    snx_ctr_dsp_free(dsp);
}

static void adpcm_and_linear(SnxCtrMemory* m) {
    SnxCtrDsp* dsp = initialized(m);
    /* one frame: scale 1, coefficients 0 (c1 = 1.0, c2 = 0), nibbles 1, 2, -1, 0, 0, 0. y[n] = (x*2048 + 0x400 +
       2048*y[n-1]) >> 11 gives 1, 3, 2, 2, 2, 2. A buffer's last two samples stay in the interpolation's history */
    const uint8_t adpcm[8] = {0x00, 0x12, 0xf0};
    put(m, SAMPLES, adpcm, sizeof adpcm);
    counters(m, 2, 1);
    DspSourceConfig config = source(SAMPLES, 6, DSP_ADPCM, DSP_NONE, 1.0f);
    config.dirty |= DSP_ADPCM_COEFFICIENTS_DIRTY;
    int16_t coeffs[16] = {2048, 0};
    put(m, R0 + DSP_SOURCE_CONFIGS, &config, sizeof config);
    put(m, R0 + DSP_ADPCM_COEFFS, coeffs, sizeof coeffs);
    master(m, R0);
    int16_t frame[SNX_CTR_DSP_SAMPLES][2];
    assert(snx_ctr_dsp_tick(dsp, frame) && snx_ctr_dsp_tick(dsp, frame)); /* the first dequeues */
    const int16_t decoded[7] = {0, 0, 1, 3, 2, 2, 0};
    for (unsigned n = 0; n < 7; ++n) assert(frame[n][0] == decoded[n]);
    snx_ctr_dsp_free(dsp);

    /* Linear at half rate on the second source: halfway between -1001 and 0 is -501, the unsigned product of
       Azahar's fixed point rounding a negative step down */
    dsp = initialized(m);
    int16_t pcm[4] = {-1001, 1000, 0, 0};
    put(m, SAMPLES, pcm, sizeof pcm);
    counters(m, 2, 1);
    config = source(SAMPLES, 4, DSP_PCM16, DSP_LINEAR, 0.5f);
    config.gain[0][0] = config.gain[0][1] = 0.0f; /* source 0 silent */
    DspSourceConfig second = source(SAMPLES, 4, DSP_PCM16, DSP_LINEAR, 0.5f);
    put(m, R0 + DSP_SOURCE_CONFIGS, &config, sizeof config);
    put(m, R0 + DSP_SOURCE_CONFIGS + sizeof config, &second, sizeof second);
    master(m, R0);
    assert(snx_ctr_dsp_tick(dsp, frame) && snx_ctr_dsp_tick(dsp, frame)); /* the first dequeues */
    DspSourceStatus statuses[2];
    get(m, R1 + DSP_SOURCE_STATUSES, statuses, sizeof statuses);
    assert(statuses[1].is_enabled && statuses[1].current_buffer_id == 7);
    const int16_t want[8] = {0, 0, 0, -501, -1001, -1, 1000, 500};
    for (unsigned n = 0; n < 8; ++n) assert(frame[n][0] == want[n]);
    snx_ctr_dsp_free(dsp);
}

/* ---- the binary pipe's AAC decoder ---- */

#define AAC_INPUT (FCRAM + 0x200000u)
#define AAC_LEFT  (FCRAM + 0x210000u)
#define AAC_RIGHT (FCRAM + 0x220000u)
#define POISON    0x7fu
enum { AAC_SUCCESS, AAC_ERROR };

/* One request on the binary pipe, the AAC codec with the command and its six words, and its one answer */
static void aac_message(SnxCtrDsp* dsp, uint16_t codec, uint16_t command, const uint32_t argument[6],
                        uint8_t answer[32]) {
    uint8_t message[32] = {0};
    snx_st_le16(message, codec);
    snx_st_le16(message + 2, command);
    for (unsigned i = 0; i < 6; ++i) snx_st_le32(message + 8 + 4 * i, argument[i]);
    unsigned before = raised[SNX_CTR_DSP_PIPE_BINARY];
    assert(!snx_ctr_dsp_pipe_write(dsp, SNX_CTR_DSP_PIPE_BINARY, message, sizeof message));
    assert(raised[SNX_CTR_DSP_PIPE_BINARY] == before + 1);
    assert(snx_ctr_dsp_pipe_read(dsp, SNX_CTR_DSP_PIPE_BINARY, answer, 32) == 32);
    assert(!snx_ctr_dsp_pipe_readable(dsp, SNX_CTR_DSP_PIPE_BINARY));
    assert(snx_ld_le16(answer) == codec && snx_ld_le16(answer + 2) == command);
}
static void poison(SnxCtrMemory* m, uint32_t pa, size_t size) {
    uint8_t bytes[0x4000];
    memset(bytes, POISON, sizeof bytes);
    assert(size <= sizeof bytes);
    put(m, pa, bytes, size);
}
static bool poisoned(SnxCtrMemory* m, uint32_t pa, size_t size) {
    uint8_t bytes[0x4000];
    assert(size <= sizeof bytes);
    get(m, pa, bytes, size);
    for (size_t i = 0; i < size; ++i)
        if (bytes[i] != POISON) return false;
    return true;
}
/* A decode of the bytes at AAC_INPUT. It returns the result, and the answer's words in out. */
static uint32_t aac_decode(SnxCtrDsp* dsp, uint32_t size, uint32_t left, uint32_t right, uint32_t out[6]) {
    const uint32_t argument[6] = {AAC_INPUT, size, left, right};
    uint8_t answer[32];
    aac_message(dsp, 1, 1, argument, answer);
    for (unsigned i = 0; i < 6; ++i) out[i] = snx_ld_le32(answer + 8 + 4 * i);
    return snx_ld_le32(answer + 4);
}
/* The zero crossings and the RMS of one frame of one channel, which a sine's frequency and amplitude give */
static void tone(SnxCtrMemory* m, uint32_t pa, unsigned* crossings, double* rms) {
    uint8_t bytes[2048];
    get(m, pa, bytes, sizeof bytes);
    double sum = 0;
    int16_t previous = (int16_t)snx_ld_le16(bytes);
    *crossings = 0;
    for (unsigned i = 0; i < 1024; ++i) {
        int16_t value = (int16_t)snx_ld_le16(bytes + 2 * i);
        *crossings += (previous < 0) != (value < 0);
        previous = value;
        sum += (double)value * value;
    }
    *rms = sqrt(sum / 1024);
}

static void aac_pipe(SnxCtrMemory* m) {
    SnxCtrDsp* dsp = initialized(m);
    uint8_t answer[32];
    const uint32_t none[6] = {0};
    /* Initialize, LoadState and SaveState answer success with the request's words */
    aac_message(dsp, 1, 0, none, answer);
    assert(snx_ld_le32(answer + 4) == AAC_SUCCESS);
    aac_message(dsp, 1, 3, none, answer);
    assert(snx_ld_le32(answer + 4) == AAC_SUCCESS);
    aac_message(dsp, 1, 4, none, answer);
    assert(snx_ld_le32(answer + 4) == AAC_SUCCESS);
    /* another codec is an error */
    aac_message(dsp, 2, 0, none, answer);
    assert(snx_ld_le32(answer + 4) == AAC_ERROR);

    /* three stereo frames in one request: both channels, all 3072 samples, the rate's index and the size */
    uint32_t out[6];
    put(m, AAC_INPUT, dsp_aac_stereo_48k, sizeof dsp_aac_stereo_48k);
    poison(m, AAC_LEFT, 3 * 2048);
    poison(m, AAC_RIGHT, 3 * 2048);
    assert(aac_decode(dsp, sizeof dsp_aac_stereo_48k, AAC_LEFT, AAC_RIGHT, out) == AAC_SUCCESS);
    assert(out[0] == 0 && out[1] == 2 && out[2] == sizeof dsp_aac_stereo_48k && out[5] == 3072);
    /* by the third frame the encoder's delay is over: the left channel is the 1000 Hz sine, the right the 1500 Hz one,
       at the amplitude they were encoded with (0.5 of full scale) */
    unsigned crossings;
    double rms;
    tone(m, AAC_LEFT + 2 * 2048, &crossings, &rms);
    assert(crossings >= 41 && crossings <= 45 && rms > 9800 && rms < 13400);
    tone(m, AAC_RIGHT + 2 * 2048, &crossings, &rms);
    assert(crossings >= 62 && crossings <= 66 && rms > 9800 && rms < 13400);

    /* a mono frame writes the left destination only, and each rate answers its index */
    const struct {
        const unsigned char* data;
        uint32_t size, rate;
    } mono[] = {{dsp_aac_mono_48k, sizeof dsp_aac_mono_48k, 0},
                {dsp_aac_mono_44k1, sizeof dsp_aac_mono_44k1, 1},
                {dsp_aac_mono_8k, sizeof dsp_aac_mono_8k, 8}};
    for (unsigned i = 0; i < sizeof mono / sizeof *mono; ++i) {
        put(m, AAC_INPUT, mono[i].data, mono[i].size);
        poison(m, AAC_LEFT, 2048);
        poison(m, AAC_RIGHT, 2048);
        assert(aac_decode(dsp, mono[i].size, AAC_LEFT, AAC_RIGHT, out) == AAC_SUCCESS);
        assert(out[0] == mono[i].rate && out[1] == 1 && out[2] == mono[i].size && out[5] == 1024);
        assert(!poisoned(m, AAC_LEFT, 2048) && poisoned(m, AAC_RIGHT, 2048));
    }
    /* a mono frame needs no right destination */
    put(m, AAC_INPUT, dsp_aac_mono_48k, sizeof dsp_aac_mono_48k);
    assert(aac_decode(dsp, sizeof dsp_aac_mono_48k, AAC_LEFT, 0, out) == AAC_SUCCESS);

    /* refused, and nothing written: a rate the DSP cannot name, a truncated frame, a byte after the last frame, a
       stereo stream with its right destination outside RAM and a source in the DSP's own RAM */
    poison(m, AAC_LEFT, 3 * 2048);
    poison(m, AAC_RIGHT, 3 * 2048);
    put(m, AAC_INPUT, dsp_aac_mono_96k, sizeof dsp_aac_mono_96k);
    assert(aac_decode(dsp, sizeof dsp_aac_mono_96k, AAC_LEFT, AAC_RIGHT, out) == AAC_ERROR);
    put(m, AAC_INPUT, dsp_aac_stereo_48k, sizeof dsp_aac_stereo_48k);
    assert(aac_decode(dsp, sizeof dsp_aac_stereo_48k - 1, AAC_LEFT, AAC_RIGHT, out) == AAC_ERROR);
    const uint8_t after = 0;
    put(m, AAC_INPUT + sizeof dsp_aac_stereo_48k, &after, 1);
    assert(aac_decode(dsp, sizeof dsp_aac_stereo_48k + 1, AAC_LEFT, AAC_RIGHT, out) == AAC_ERROR);
    assert(aac_decode(dsp, sizeof dsp_aac_stereo_48k, AAC_LEFT, 0, out) == AAC_ERROR);
    const uint32_t outside[6] = {SNX_CTR_DSP_RAM, sizeof dsp_aac_stereo_48k, AAC_LEFT, AAC_RIGHT};
    aac_message(dsp, 1, 1, outside, answer);
    assert(snx_ld_le32(answer + 4) == AAC_ERROR);
    const uint32_t vram[6] = {0x18000000u, sizeof dsp_aac_stereo_48k, AAC_LEFT, AAC_RIGHT};
    aac_message(dsp, 1, 1, vram, answer);
    assert(snx_ld_le32(answer + 4) == AAC_ERROR);
    assert(poisoned(m, AAC_LEFT, 3 * 2048) && poisoned(m, AAC_RIGHT, 3 * 2048));

    /* after the refusals, and after Shutdown, a request decodes again */
    aac_message(dsp, 1, 2, none, answer);
    assert(snx_ld_le32(answer + 4) == AAC_SUCCESS);
    assert(aac_decode(dsp, sizeof dsp_aac_stereo_48k, AAC_LEFT, AAC_RIGHT, out) == AAC_SUCCESS && out[5] == 3072);
    snx_ctr_dsp_free(dsp);
}

/* ---- the service, as libctru's ndspInit drives it ---- */

#define DATA 0x300000u
static void service(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t dsp = 0, event = 0; /* MinGW's assert is not noreturn: Clang cannot see the failed path ends */
    assert(!snx_ctr_dsp_open(p) && !snx_ctr_session_create(p, SNX_CTR_SERVICE_DSP, &dsp));
    assert(!snx_ctr_object_create(p, CTR_OBJ_EVENT, &event));
    uint32_t id = snx_ctr_handle_object(p, event);
    p->objects[id].data.event.reset_type = 1; /* sticky, as ndsp makes it */
    /* UnloadComponent, then LoadComponent of a 0x300-byte component: loaded */
    const uint32_t unload[] = {0x00120000};
    ctr_call(p, dsp, unload, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00120040 && !ctr_command_word(p, 1));
    const uint32_t load[] = {0x001100c2, 0x300, 0xff, 0xff, 0x300 << 4 | 0xa, DATA};
    ctr_call(p, dsp, load, 6, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00110082 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 1 &&
           ctr_command_word(p, 4) == DATA);
    /* the event comes in one copied or moved handle; any other descriptor is refused */
    const uint32_t pid_form[] = {0x00150082, 2, SNX_CTR_DSP_PIPE_AUDIO, 0x20, event};
    for (unsigned i = 0; i < 5; ++i) ctr_put(p, CTR_COMMAND + i * 4, pid_form[i]);
    p->cpu.state.cpu.r[0] = dsp;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && p->cpu.state.cpu.r[0] == SNX_CTR_IPC_BAD_DESCRIPTOR);
    assert(!p->dsp.interrupt[2 + SNX_CTR_DSP_PIPE_AUDIO]);
    const uint32_t interrupts[] = {0x00150082, 2, SNX_CTR_DSP_PIPE_AUDIO, 0, event};
    ctr_call(p, dsp, interrupts, 5, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00150040 && !ctr_command_word(p, 1) &&
           p->dsp.interrupt[2 + SNX_CTR_DSP_PIPE_AUDIO] == id);
    const uint32_t semaphore[] = {0x00160000};
    ctr_call(p, dsp, semaphore, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00160042 && !ctr_command_word(p, 1) && !ctr_command_word(p, 2));
    assert(snx_ctr_handle_object(p, ctr_command_word(p, 3)) == p->dsp.semaphore && p->dsp.semaphore);
    const uint32_t mask[] = {0x00170040, 0x2000};
    ctr_call(p, dsp, mask, 2, SNX_CTR_SVC_RETURN, 0);
    assert(p->dsp.semaphore_mask == 0x2000);
    /* Initialize on the audio pipe, with garbage from the stack in its last two bytes: the event is signaled */
    ctr_put(p, DATA, 0xcdcd0000u);
    const uint32_t initialize[] = {0x000d0082, SNX_CTR_DSP_PIPE_AUDIO, 4, 4 << 14 | 0x402, DATA};
    ctr_call(p, dsp, initialize, 5, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1) && p->objects[id].data.event.signaled);
    p->objects[id].data.event.signaled = 0;
    /* ReadPipeIfPossible lands in static buffer 0: the count, then the addresses */
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x180, 0x40 << 14 | 2);
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x184, DATA + 0x100);
    const uint32_t count[] = {0x001000c0, SNX_CTR_DSP_PIPE_AUDIO, 0, 2};
    ctr_call(p, dsp, count, 4, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00100082 && ctr_command_word(p, 2) == 2 &&
           ctr_command_word(p, 3) == (2 << 14 | 2));
    assert(ctr_command_word(p, 4) == DATA + 0x100 && (ctr_word(p, DATA + 0x100) & 0xffff) == 15);
    const uint32_t addresses[] = {0x001000c0, SNX_CTR_DSP_PIPE_AUDIO, 0, 30};
    ctr_call(p, dsp, addresses, 4, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 2) == 30 && (ctr_word(p, DATA + 0x100) & 0xffff) == 0xbfff);
    /* asking for more than is left reads nothing */
    ctr_call(p, dsp, addresses, 4, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 2));
    /* the frame counter's DSP address as the application maps it: region 0's last word */
    const uint32_t convert[] = {0x000c0040, 0xbfff};
    ctr_call(p, dsp, convert, 2, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x000c0080 &&
           ctr_command_word(p, 2) == SNX_CTR_DSP_RAM + DSP_REGION0 + DSP_FRAME_COUNTER);
    const uint32_t recv[] = {0x00010040, 0};
    ctr_call(p, dsp, recv, 2, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00010080 && !ctr_command_word(p, 2)); /* on */
    /* a frame due raises the audio pipe's interrupt */
    snx_ctr_dsp_frame(p, p->dsp.next_frame_ns);
    assert(p->objects[id].data.event.signaled);
    const uint32_t flush[] = {0x00130082, DATA, 0x100, 0, CTR_PROCESS_HANDLE};
    ctr_call(p, dsp, flush, 5, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00130040 && !ctr_command_word(p, 1));
    /* Shutdown: RecvData reports the DSP stopped */
    ctr_put(p, DATA, 1);
    const uint32_t shutdown[] = {0x000d0082, SNX_CTR_DSP_PIPE_AUDIO, 4, 4 << 14 | 0x402, DATA};
    ctr_call(p, dsp, shutdown, 5, SNX_CTR_SVC_RETURN, 0);
    ctr_call(p, dsp, recv, 2, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 2) == 1);
    /* unregistering releases the event */
    const uint32_t unregister[] = {0x00150082, 2, SNX_CTR_DSP_PIPE_AUDIO, 0, 0};
    ctr_call(p, dsp, unregister, 5, SNX_CTR_SVC_RETURN, 0);
    assert(!p->dsp.interrupt[2 + SNX_CTR_DSP_PIPE_AUDIO]);
    snx_ctr_process_free(p);
}

int main(void) {
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    assert(m);
    pipes_and_regions(m);
    pcm16_source(m);
    adpcm_and_linear(m);
    aac_pipe(m);
    snx_ctr_memory_free(m);
    service();
    printf("HLE DSP and dsp::DSP passed\n");
    return 0;
}
