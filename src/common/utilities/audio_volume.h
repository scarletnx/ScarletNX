/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The player's Volume (settings.txt's volume, 0 to 100 percent) on an SDL output, the Switch's (backend/sdl3.c) and the
   3DS's (audio/output.c) */
#ifndef SNX_AUDIO_VOLUME_H
#define SNX_AUDIO_VOLUME_H
#include <SDL3/SDL_audio.h>

/* volume as the gain of the logical device SDL_OpenAudioDeviceStream opened for stream, so that a game's own volume,
   the stream's gain, is multiplied by it and stays the game's. Given only when it is not *applied (the percent given
   last, -1 for none), which it becomes either way: 1 when the device has it, 0 when SDL refused it (SDL_GetError says
   why) */
static inline int snx_audio_volume_follow(SDL_AudioStream* stream, int volume, int* applied) {
    if (volume == *applied) return 1;
    *applied = volume;
    return SDL_SetAudioDeviceGain(SDL_GetAudioStreamDevice(stream), (float)volume / 100.0f);
}

#endif
