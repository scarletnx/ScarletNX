/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/platform/mic.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

struct SnxMicDevice {
    SDL_AudioStream* stream;
};
/* a read's failures said: the first 4 of a run */
static _Atomic unsigned g_mic_read_failures;

/* the recording device whose name holds device, ignoring case: the default when none does or device is empty */
static SDL_AudioDeviceID mic_device(const char* device, SnxMicSay say) {
    SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    int count = 0;
    if (!*device) return found;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    for (int i = 0; ids && i < count && found == SDL_AUDIO_DEVICE_DEFAULT_RECORDING; i++) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        if (name && SDL_strcasestr(name, device)) found = ids[i];
    }
    SDL_free(ids);
    if (found == SDL_AUDIO_DEVICE_DEFAULT_RECORDING)
        say(true,
            "audio: no recording device's name holds the microphone setting's text: the system default records\n");
    return found;
}

SnxMicDevice* snx_mic_device_open(uint32_t rate, const char* device, SnxMicSay say) {
    char line[512];
    if (!SDL_WasInit(SDL_INIT_AUDIO)) return NULL;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, (int)rate}; /* the host's order: read as int16_t */
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(mic_device(device, say), &spec, NULL, NULL);
    if (!stream) {
        snprintf(line, sizeof line, "audio: SDL could not open a recording device: %s\n", SDL_GetError());
        say(true, line);
        return NULL;
    }
    SnxMicDevice* m = calloc(1, sizeof *m);
    if (!m || !SDL_ResumeAudioStreamDevice(stream)) {
        snprintf(line, sizeof line, "audio: SDL could not start recording: %s\n", m ? SDL_GetError() : "out of memory");
        say(true, line);
        SDL_DestroyAudioStream(stream);
        free(m);
        return NULL;
    }
    m->stream = stream;
    /* the device by its format, never its name, as for the output */
    SDL_AudioSpec from;
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &from, &frames))
        snprintf(line, sizeof line,
                 "audio: SDL %s recording device opened, which records %s at %d Hz, %d channels, %d-frame buffer; the "
                 "game gets mono signed 16-bit PCM at %u Hz\n",
                 SDL_GetCurrentAudioDriver(), SDL_GetAudioFormatName(from.format), from.freq, from.channels, frames,
                 (unsigned)rate);
    else
        snprintf(line, sizeof line,
                 "audio: SDL %s recording device opened; the game gets mono signed 16-bit PCM at %u Hz\n",
                 SDL_GetCurrentAudioDriver(), (unsigned)rate);
    say(false, line);
    return m;
}

void snx_mic_device_close(SnxMicDevice* m) {
    if (!m) return;
    SDL_DestroyAudioStream(m->stream); /* and the device the stream opened with it */
    free(m);
}

uint32_t snx_mic_device_available(SnxMicDevice* m) {
    int n = m ? SDL_GetAudioStreamAvailable(m->stream) : 0;
    return n > 0 ? (uint32_t)n / 2 : 0;
}

uint32_t snx_mic_device_read(SnxMicDevice* m, int16_t* samples, uint32_t frames, SnxMicSay say) {
    if (!m || !frames) return 0;
    int n = SDL_GetAudioStreamData(m->stream, samples, (int)(frames > 0x3FFFFFFFu ? 0x3FFFFFFFu : frames) * 2);
    if (n < 0 && atomic_fetch_add_explicit(&g_mic_read_failures, 1, memory_order_relaxed) < 4) {
        char line[384];
        snprintf(line, sizeof line, "audio: SDL could not read the recording: %s\n", SDL_GetError());
        say(true, line);
    }
    return n > 0 ? (uint32_t)n / 2 : 0;
}
