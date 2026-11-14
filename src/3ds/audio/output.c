/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/audio/output.h"
#include "3ds/board/clock.h"
#include "common/utilities/endian.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define FRAME_BYTES (SNX_CTR_DSP_SAMPLES * 4u)
/* The DSP is paced by the host clock and the device by its own: past this much queued, frames are dropped rather
   than letting the latency grow */
#define MAX_QUEUED  (SNX_CTR_DSP_RATE * 4u / 5u) /* 200 ms */
#define WAV_LIMIT   (SNX_CTR_DSP_RATE * 4u * 120u)
#define WAV_HEADER_EVERY (SNX_CTR_DSP_RATE * 4u * 2u) /* two seconds: a killed run still has a playable file */

static struct {
    SDL_AudioStream* stream;
    FILE* wav;
    char path[1024];
    uint32_t wav_bytes, wav_header_at;
    int pace; /* the clock's pace the device plays at (board/clock.h): the console's until a frame finds another */
    int put_failed;
} g_output;

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
    if (fseek(g_output.wav, 0, SEEK_SET) || fwrite(h, sizeof h, 1, g_output.wav) != 1 ||
        fseek(g_output.wav, 0, SEEK_END)) {
        fprintf(stderr, "3ds audio: cannot write the recording %s: %s\n", g_output.path, strerror(errno));
        fclose(g_output.wav);
        g_output.wav = NULL;
    }
}
bool snx_ctr_audio_init(const char* wav_path, bool headless) {
    if (g_output.stream || g_output.wav) return true;
    if (!headless && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, (int)SNX_CTR_DSP_RATE};
        g_output.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (g_output.stream && !SDL_ResumeAudioStreamDevice(g_output.stream)) {
            SDL_DestroyAudioStream(g_output.stream);
            g_output.stream = NULL;
        }
        if (!g_output.stream) {
            fprintf(stderr, "3ds audio: no audio device: %s\n", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }
    if (wav_path) {
        snprintf(g_output.path, sizeof g_output.path, "%s", wav_path);
        g_output.wav = fopen(wav_path, "wb");
        if (g_output.wav)
            wav_header(0);
        else
            fprintf(stderr, "3ds audio: cannot write %s: %s\n", wav_path, strerror(errno));
    }
    return g_output.stream || g_output.wav;
}
void snx_ctr_audio_frame(const int16_t frame[SNX_CTR_DSP_SAMPLES][2]) {
    int16_t mixed[SNX_CTR_DSP_SAMPLES][2];
    snx_ctr_audio_mix_csnd(frame, mixed); /* CSND's samples waiting (mix.c) */
    frame = (const int16_t (*)[2])mixed;
    unsigned char bytes[FRAME_BYTES];
    for (unsigned i = 0; i < SNX_CTR_DSP_SAMPLES * 2; ++i) {
        uint16_t sample = (uint16_t)frame[i / 2][i % 2];
        bytes[i * 2] = (unsigned char)sample;
        bytes[i * 2 + 1] = (unsigned char)(sample >> 8);
    }
    /* the frames come at the clock's pace (fps_limit): the device plays them at it, the pitch with it, and with no
       limit none is played, as they come faster than any device plays them */
    int pace = snx_ctr_clock_pace();
    if (g_output.stream && pace != g_output.pace) {
        char limit[16];
        snx_ctr_clock_text(pace, limit, sizeof limit);
        if (pace == SNX_CTR_PACE_NONE)
            fprintf(stderr, "3ds audio: fps_limit=%s runs the game as fast as it goes: its sound is not played\n",
                    limit);
        else if (!SDL_SetAudioStreamFrequencyRatio(g_output.stream, (float)snx_ctr_clock_rate()))
            fprintf(stderr, "3ds audio: fps_limit=%s: the device cannot play at %.3f times the rate: %s\n", limit,
                    snx_ctr_clock_rate(), SDL_GetError());
        else
            fprintf(stderr, "3ds audio: fps_limit=%s: the sound plays at %.3f times its rate\n", limit,
                    snx_ctr_clock_rate());
        g_output.pace = pace;
    }
    if (g_output.stream && pace != SNX_CTR_PACE_NONE && SDL_GetAudioStreamQueued(g_output.stream) < (int)MAX_QUEUED &&
        !SDL_PutAudioStreamData(g_output.stream, bytes, (int)sizeof bytes) && !g_output.put_failed) {
        fprintf(stderr, "3ds audio: the device stopped taking its frames: %s\n", SDL_GetError());
        g_output.put_failed = 1;
    }
    if (g_output.wav && g_output.wav_bytes < WAV_LIMIT) {
        if (fwrite(bytes, sizeof bytes, 1, g_output.wav) == 1) {
            g_output.wav_bytes += sizeof bytes;
            if (g_output.wav_bytes - g_output.wav_header_at >= WAV_HEADER_EVERY) {
                wav_header(g_output.wav_bytes);
                g_output.wav_header_at = g_output.wav_bytes;
            }
        } else {
            fprintf(stderr, "3ds audio: cannot write the recording %s: %s\n", g_output.path, strerror(errno));
            wav_header(g_output.wav_bytes);
            if (g_output.wav) fclose(g_output.wav);
            g_output.wav = NULL;
        }
    }
}
void snx_ctr_audio_free(void) {
    snx_ctr_audio_mix_reset();
    if (g_output.stream) {
        SDL_DestroyAudioStream(g_output.stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (g_output.wav) {
        wav_header(g_output.wav_bytes);
        if (g_output.wav) fclose(g_output.wav);
    }
    g_output.stream = NULL;
    g_output.wav = NULL;
    g_output.path[0] = 0;
    g_output.wav_bytes = g_output.wav_header_at = 0;
    g_output.pace = SNX_CTR_PACE_CONSOLE;
    g_output.put_failed = 0;
}
