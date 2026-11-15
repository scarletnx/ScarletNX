/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's Volume on an SDL output (common/utilities/audio_volume.h), on SDL's dummy driver: the gain of the logical
   device SDL opened for each stream, the stream's own gain (a game's volume) left as it is; asked of SDL again only when
   it changed; a stream bound to no device refused, SDL saying why */
#include "common/utilities/audio_volume.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>

static float device_gain(SDL_AudioStream* stream) { return SDL_GetAudioDeviceGain(SDL_GetAudioStreamDevice(stream)); }

int main(void) {
    const SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, 48000};
    SDL_AudioStream *game, *other, *loose;
    int a = -1, b = -1, c = -1;
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_AUDIO));
    game = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    other = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    assert(game && other && SDL_GetAudioStreamDevice(game) != SDL_GetAudioStreamDevice(other));
    assert(SDL_SetAudioStreamGain(game, 0.5f) && device_gain(game) == 1.0f); /* the game's own volume, on the stream */
    /* 40%: the device's gain, the stream's kept, the other output's device not touched */
    assert(snx_audio_volume_follow(game, 40, &a) && a == 40);
    assert(device_gain(game) == 0.4f && SDL_GetAudioStreamGain(game) == 0.5f && device_gain(other) == 1.0f);
    assert(snx_audio_volume_follow(other, 40, &b) && b == 40 && device_gain(other) == 0.4f &&
           SDL_GetAudioStreamGain(other) == 1.0f);
    /* the same percent again asks nothing of SDL: a gain set beside it stays */
    assert(SDL_SetAudioDeviceGain(SDL_GetAudioStreamDevice(game), 0.9f));
    assert(snx_audio_volume_follow(game, 40, &a) && a == 40 && device_gain(game) == 0.9f);
    /* Mute, then the loudest, the game's own volume under each */
    assert(snx_audio_volume_follow(game, 0, &a) && a == 0 && device_gain(game) == 0.0f);
    assert(snx_audio_volume_follow(game, 100, &a) && a == 100 && device_gain(game) == 1.0f &&
           SDL_GetAudioStreamGain(game) == 0.5f);
    /* a stream bound to no device: refused with SDL's reason, the percent taken all the same, so that a refusal is not
       asked again at every buffer */
    loose = SDL_CreateAudioStream(&spec, &spec);
    assert(loose && !SDL_GetAudioStreamDevice(loose));
    SDL_ClearError();
    assert(!snx_audio_volume_follow(loose, 50, &c) && c == 50 && SDL_GetError()[0]);
    assert(snx_audio_volume_follow(loose, 50, &c));
    SDL_DestroyAudioStream(loose);
    SDL_DestroyAudioStream(other);
    SDL_DestroyAudioStream(game);
    SDL_Quit();
    puts("audio volume: the device's gain over the game's, each output its own, asked again only on a change");
    return 0;
}
