/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/audio/output.h"
#include "common/utilities/endian.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define FRAME_BYTES (SNX_CTR_DSP_SAMPLES * 4u)
/* The DSP is paced by the host clock and the device by its own: past this much queued, frames are dropped rather
   than letting the latency grow */
#define MAX_QUEUED  (SNX_CTR_DSP_RATE * 4u / 5u) /* 200 ms */
#define WAV_LIMIT   (SNX_CTR_DSP_RATE * 4u * 120u)

static struct {
    SDL_AudioStream* stream;
    FILE* wav;
    uint32_t wav_bytes;
} o;

static void wav_header(uint32_t data_bytes) {
    /* RIFF, a 16-byte fmt chunk (PCM, 2 channels, 16 bits, 4-byte frames), then the data */
    unsigned char h[44] = {'R', 'I', 'F', 'F', 0,  0, 0, 0, 'W', 'A', 'V', 'E',
                           'f', 'm', 't', ' ', 16, 0, 0, 0, 1,   0,   2,   0};
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    snx_st_le32(h + 4, 36 + data_bytes);
    snx_st_le32(h + 24, SNX_CTR_DSP_RATE);
    snx_st_le32(h + 28, SNX_CTR_DSP_RATE * 4);
    snx_st_le32(h + 40, data_bytes);
    if (fseek(o.wav, 0, SEEK_SET) || fwrite(h, sizeof h, 1, o.wav) != 1 || fseek(o.wav, 0, SEEK_END)) {
        fclose(o.wav);
        o.wav = NULL;
    }
}
bool snx_ctr_audio_init(const char* wav_path) {
    if (o.stream || o.wav) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, (int)SNX_CTR_DSP_RATE};
        o.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (o.stream && !SDL_ResumeAudioStreamDevice(o.stream)) {
            SDL_DestroyAudioStream(o.stream);
            o.stream = NULL;
        }
        if (!o.stream) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (wav_path && (o.wav = fopen(wav_path, "wb")) != NULL) wav_header(0);
    return o.stream || o.wav;
}
void snx_ctr_audio_frame(const int16_t frame[SNX_CTR_DSP_SAMPLES][2]) {
    unsigned char bytes[FRAME_BYTES];
    for (unsigned i = 0; i < SNX_CTR_DSP_SAMPLES * 2; ++i) {
        uint16_t sample = (uint16_t)frame[i / 2][i % 2];
        bytes[i * 2] = (unsigned char)sample;
        bytes[i * 2 + 1] = (unsigned char)(sample >> 8);
    }
    if (o.stream && SDL_GetAudioStreamQueued(o.stream) < (int)MAX_QUEUED)
        (void)SDL_PutAudioStreamData(o.stream, bytes, (int)sizeof bytes);
    if (o.wav && o.wav_bytes < WAV_LIMIT) {
        if (fwrite(bytes, sizeof bytes, 1, o.wav) == 1) {
            o.wav_bytes += sizeof bytes;
        } else {
            fclose(o.wav);
            o.wav = NULL;
        }
    }
}
void snx_ctr_audio_free(void) {
    if (o.stream) {
        SDL_DestroyAudioStream(o.stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (o.wav) {
        wav_header(o.wav_bytes);
        if (o.wav) fclose(o.wav);
    }
    o.stream = NULL;
    o.wav = NULL;
    o.wav_bytes = 0;
}
