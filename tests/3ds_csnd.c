/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* csnd:SND and the CSND hardware (services/csnd.c, audio/csnd.c, audio/mix.c), as Azahar's csnd_snd.cpp, csnd.cpp and
   dsp_interface.cpp have them: the hardware's PCM16 and IMA-ADPCM samples, one-shot, held, looping and interpolated;
   the service's replies through srv:, a command list that configures and starts a channel, the state it writes back,
   a pause after the fade; and its frames in the output, mixed into the DSP's or alone, read from the recording */
#include "3ds/audio/csnd.h"
#include "3ds/audio/output.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/csnd.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/3ds_guest.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMER_1X (0x10000u - 2048u) /* a sample each output sample */
#define TIMER_2X (0x10000u - 4096u) /* a sample each two */
#define ONE_SHOT (2u << SNX_CTR_CSND_LOOP_SHIFT)
#define NORMAL   (1u << SNX_CTR_CSND_LOOP_SHIFT)
#define PCM16    (1u << SNX_CTR_CSND_FORMAT_SHIFT)
#define ADPCM    (2u << SNX_CTR_CSND_FORMAT_SHIFT)
#define PLAYING  (SNX_CTR_CSND_ENABLE | SNX_CTR_CSND_RUN)

static void put16s(SnxCtrProcess* p, uint32_t pa, const int16_t* v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        unsigned char b[2];
        snx_st_le16(b, (uint16_t)v[i]);
        assert(snx_ctr_phys_write(p->memory, pa + i * 2, b, 2) == SNX_CTR_MEM_OK);
    }
}
/* the left channel of count samples the hardware renders */
static void render(SnxCtrProcess* p, SnxCtrCsndHw* hw, int16_t* left, unsigned count) {
    int16_t out[16][2];
    assert(count <= 16);
    snx_ctr_csnd_render(hw, p->memory, out, count);
    for (unsigned i = 0; i < count; ++i) left[i] = out[i][0];
}
static void expect(const int16_t* got, const int16_t* want, unsigned n, const char* what) {
    if (!memcmp(got, want, n * sizeof *got)) return;
    fprintf(stderr, "%s:", what);
    for (unsigned i = 0; i < n; ++i) fprintf(stderr, " %d/%d", got[i], want[i]);
    fputc('\n', stderr);
    abort();
}
static void hardware(SnxCtrProcess* p, uint32_t pa) {
    static const int16_t samples[4] = {1000, -2000, 3000, -4000};
    put16s(p, pa, samples, 4);
    SnxCtrCsndHw hw = {0};
    SnxCtrCsndChannel* ch = &hw.channel[0];
    int16_t got[8];
    /* PCM16 one-shot at full volume: the four samples, then nothing */
    ch->start = pa, ch->size = 8, ch->timer = (uint16_t)TIMER_1X;
    snx_ctr_csnd_set_volume(&hw, 0, 0x8000, 0x8000);
    snx_ctr_csnd_set_cnt(&hw, 0, PLAYING | ONE_SHOT | PCM16);
    render(p, &hw, got, 6);
    expect(got, (const int16_t[]){1000, -2000, 3000, -4000, 0, 0}, 6, "one-shot");
    assert(!(ch->cnt & SNX_CTR_CSND_ENABLE) && !snx_ctr_csnd_busy(&hw));
    /* held: the last sample stays out */
    snx_ctr_csnd_set_cnt(&hw, 0, PLAYING | ONE_SHOT | PCM16 | SNX_CTR_CSND_HOLD);
    render(p, &hw, got, 6);
    expect(got, (const int16_t[]){1000, -2000, 3000, -4000, -4000, -4000}, 6, "held");
    /* half volume on the left; looping from the third sample */
    snx_ctr_csnd_set_cnt(&hw, 0, 0);
    snx_ctr_csnd_set_volume(&hw, 0, 0x4000, 0x8000);
    ch->loop = pa;
    snx_ctr_csnd_set_cnt(&hw, 0, PLAYING | NORMAL | PCM16);
    render(p, &hw, got, 8);
    expect(got, (const int16_t[]){500, -1000, 1500, -2000, 500, -1000, 1500, -2000}, 8, "loop");
    /* interpolated at half the rate: a sample read every two outputs, the output between going half way from the one
       before (each read lands where the counter is back at 0, on the sample before) */
    snx_ctr_csnd_set_cnt(&hw, 0, 0);
    snx_ctr_csnd_set_volume(&hw, 0, 0x8000, 0x8000);
    ch->timer = (uint16_t)TIMER_2X;
    snx_ctr_csnd_set_cnt(&hw, 0, PLAYING | ONE_SHOT | PCM16 | SNX_CTR_CSND_INTERPOLATE);
    render(p, &hw, got, 4);
    expect(got, (const int16_t[]){0, 0, 500, 1000}, 4, "interpolated");
    /* IMA-ADPCM from predictor 0, index 0: the byte 0x07 is +11 (index 8), then +2 (13, index 7) */
    static const unsigned char adpcm[1] = {0x07};
    assert(snx_ctr_phys_write(p->memory, pa + 16, adpcm, 1) == SNX_CTR_MEM_OK);
    snx_ctr_csnd_set_cnt(&hw, 0, 0);
    ch->start = pa + 16, ch->size = 1, ch->timer = (uint16_t)TIMER_1X, ch->adpcm_state = 0;
    snx_ctr_csnd_set_cnt(&hw, 0, PLAYING | ONE_SHOT | ADPCM);
    render(p, &hw, got, 3);
    expect(got, (const int16_t[]){11, 13, 0}, 3, "adpcm");
    assert(snx_ctr_csnd_adpcm_state(&hw, 0) == (13u | 7u << 16)); /* kept as it stopped */
}
static uint32_t call(SnxCtrProcess* p, uint32_t session, const uint32_t* words, unsigned count) {
    ctr_call(p, session, words, count, SNX_CTR_SVC_RETURN, 0);
    return ctr_command_word(p, 0);
}
/* a command of the list at offset in the client's block: its id, the next one's offset and six parameters */
static void command(SnxCtrProcess* p, uint32_t block, uint32_t offset, uint16_t id, uint16_t next,
                    const uint32_t* params) {
    unsigned char c[0x20] = {0};
    snx_st_le16(c, next);
    snx_st_le16(c + 2, id);
    for (unsigned i = 0; i < 6; ++i) snx_st_le32(c + 8 + i * 4, params[i]);
    assert(snx_ctr_phys_write(p->memory, block + offset, c, sizeof c) == SNX_CTR_MEM_OK);
}
static uint32_t block_word(SnxCtrProcess* p, uint32_t block, uint32_t offset) {
    unsigned char b[4];
    assert(snx_ctr_phys_read(p->memory, block + offset, b, 4) == SNX_CTR_MEM_OK);
    return snx_ld_le32(b);
}
int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"); /* the recording is what is read */
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t pa = snx_ctr_shared_pages(p, 4096); /* the samples: a page of FCRAM no one else has */
    assert(pa);
    hardware(p, pa);

    char* dir = snx_temp_dir(NULL);
    assert(dir);
    char* wav = snx_path_join(dir, "csnd.wav");
    assert(wav);
    assert(!snx_ctr_audio_init(NULL, true) && !SDL_WasInit(SDL_INIT_AUDIO));
    assert(snx_ctr_audio_init(wav, false));
    uint32_t srv = ctr_srv(p);
    const uint32_t registration[] = {0x00010002, 0x20, 0};
    ctr_call(p, srv, registration, 3, SNX_CTR_SVC_RETURN, 0);
    uint32_t csnd = ctr_service_handle(p, srv, "csnd:SND");
    /* Initialize(size, master, channel, capture and direct sound state offsets): a mutex and the block */
    const uint32_t init[] = {0x00010140, 0x2000, 0x100, 0x110, 0x400, 0x500};
    assert(call(p, csnd, init, 6) == 0x00010043 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 1u << 26);
    uint32_t mutex = snx_ctr_handle_object(p, ctr_command_word(p, 3));
    uint32_t shared = snx_ctr_handle_object(p, ctr_command_word(p, 4));
    assert(p->objects[mutex].type == CTR_OBJ_MUTEX && p->objects[shared].type == CTR_OBJ_SHARED);
    assert(p->objects[shared].data.shared.size == 0x2000);
    uint32_t block = p->objects[shared].data.shared.physical;
    assert(call(p, csnd, init, 6) == 0x00010040 && ctr_command_word(p, 1) == 0xe0a0183bu); /* once */
    assert(snx_ctr_csnd_deadline(p) != UINT64_MAX); /* the frames tick from Initialize */
    /* the channels a client may use, the capture units, two and no more */
    assert(ctr_send_header(p, csnd, 0x00050000) == 0x00050080 && ctr_command_word(p, 2) == 0xffffff00u);
    assert(ctr_send_header(p, csnd, 0x00070000) == 0x00070080 && ctr_command_word(p, 2) == 0);
    assert(ctr_send_header(p, csnd, 0x00070000) == 0x00070080 && ctr_command_word(p, 2) == 1);
    const uint32_t acquire[] = {0x00070000};
    assert(call(p, csnd, acquire, 1) == 0x00070080 && ctr_command_word(p, 1) == 0xc8a0b7ffu &&
           ctr_command_word(p, 2) == 0xffffffffu);
    const uint32_t release[] = {0x00080040, 1}, release_bad[] = {0x00080040, 2};
    assert(call(p, csnd, release, 2) == 0x00080040 && !ctr_command_word(p, 1));
    assert(call(p, csnd, release_bad, 2) == 0x00080040 && ctr_command_word(p, 1) == 0xc8a0b7ffu);
    /* the cache operations: the linear heap and the FCRAM's mirror, nothing else */
    const uint32_t flush_heap[] = {0x00090082, 0x14000000, 0x100, 0, CTR_PROCESS_HANDLE};
    const uint32_t flush_code[] = {0x00090082, 0x00100000, 0x100, 0, CTR_PROCESS_HANDLE};
    assert(call(p, csnd, flush_heap, 5) == 0x00090040 && !ctr_command_word(p, 1));
    assert(call(p, csnd, flush_code, 5) == 0x00090040 && ctr_command_word(p, 1) == 0xe0e3fff6u);

    /* a list: ConfigureChannel on channel 8 (PCM16, one-shot, playing, a sample each output sample, full volume) then
       ApplyMasks and UpdateState */
    const uint32_t configure[6] = {8u | 2u << 10 | 1u << 12 | 1u << 14 | 2048u << 16, 0x80008000u, 0, pa, pa, 8};
    const uint32_t none[6] = {0};
    command(p, block, 0x1000, 0x00e, 0x1020, configure);
    command(p, block, 0x1020, 0x200, 0x1040, none);
    command(p, block, 0x1040, 0x300, 0xffff, none);
    const uint32_t run_list[] = {0x00030040, 0x1000};
    assert(call(p, csnd, run_list, 2) == 0x00030040 && !ctr_command_word(p, 1));
    unsigned at = p->csnd.frame_samples; /* the pre-list samples of the frame, before the updates clear it */
    assert((block_word(p, block, 0x1004) & 0xff) == 1); /* the first command finished */
    assert((p->csnd.hw.channel[8].cnt & PLAYING) == PLAYING);
    /* the state: the mask register's channels and the channel's own (8 is the client's first: index 0) */
    assert(block_word(p, block, 0x100) == 0xffffff00u && (block_word(p, block, 0x110) & 0xff) == 1);
    /* the frame it plays reaches the recording alone, as the DSP plays none: two CSND frames, then both go out */
    for (unsigned i = 0; i < 2; ++i) snx_ctr_csnd_update(p, snx_ctr_csnd_deadline(p));
    /* a DSP frame of 100s mixed with nothing waiting */
    int16_t dsp[SNX_CTR_DSP_SAMPLES][2];
    for (unsigned i = 0; i < SNX_CTR_DSP_SAMPLES; ++i) dsp[i][0] = dsp[i][1] = 100;
    snx_ctr_audio_frame(dsp);
    /* the recording: the CSND frames alone (1000, -2000, 3000, -4000, then 0), then the DSP's 100s */
    snx_ctr_audio_free();
    size_t bytes = 0;
    unsigned char* data = snx_read_file(wav, &bytes);
    assert(data && bytes == 44 + 3 * SNX_CTR_DSP_SAMPLES * 4);
    /* the frame begins with the samples rendered before the list ran, silent: as many as the time it took */
    int16_t first[5], mixed_one;
    if (at + 5 <= SNX_CTR_DSP_SAMPLES) {
        for (unsigned i = 0; i < 5; ++i) first[i] = (int16_t)snx_ld_le16(data + 44 + (at + i) * 4);
        expect(first, (const int16_t[]){1000, -2000, 3000, -4000, 0}, 5, "the recording");
    } else
        printf("the pre-list frame filled before the list ran: the recording's offset is not checked\n");
    mixed_one = (int16_t)snx_ld_le16(data + 44 + 2 * SNX_CTR_DSP_SAMPLES * 4);
    assert(mixed_one == 100);
    free(data);
    /* SetPlayState(0) on the running channel: paused once its volume faded out over the steps */
    snx_ctr_csnd_set_cnt(&p->csnd.hw, 8, PLAYING | NORMAL | PCM16); /* looping, so it is still running */
    const uint32_t pause[6] = {8, 0};
    command(p, block, 0x1000, 0x001, 0xffff, pause);
    assert(call(p, csnd, run_list, 2) == 0x00030040);
    assert(p->csnd.pause_pending >> 8 & 1);
    unsigned steps = 0;
    while (p->csnd.next_fade_ns && steps < 1000) {
        snx_ctr_csnd_update(p, p->csnd.next_fade_ns);
        ++steps;
    }
    assert(!(p->csnd.hw.channel[8].cnt & SNX_CTR_CSND_RUN) && !(p->csnd.started >> 8 & 1) && steps >= 40);
    /* Reset and Shutdown */
    assert(ctr_send_header(p, csnd, 0x000c0000) == 0x000c0040 && !(p->csnd.hw.channel[8].cnt & PLAYING));
    assert(ctr_send_header(p, csnd, 0x00020000) == 0x00020040 && !p->csnd.initialized_clients);

    /* a headless recording: no audio device, and the WAV still holds the frames */
    char* headless_wav = snx_path_join(dir, "headless.wav");
    assert(headless_wav && snx_ctr_audio_init(headless_wav, true) && !SDL_WasInit(SDL_INIT_AUDIO));
    int16_t silence[SNX_CTR_DSP_SAMPLES][2] = {0};
    snx_ctr_audio_frame(silence);
    snx_ctr_audio_free();
    size_t headless_bytes = 0;
    unsigned char* headless_data = snx_read_file(headless_wav, &headless_bytes);
    assert(headless_data && headless_bytes == 44 + SNX_CTR_DSP_SAMPLES * 4);
    free(headless_data);
    free(headless_wav);
    assert(!snx_remove_tree(dir));
    free(wav);
    free(dir);
    snx_ctr_process_free(p);
    puts("3ds csnd:SND: the hardware's samples, the service, the fades and the mix passed");
    return 0;
}
