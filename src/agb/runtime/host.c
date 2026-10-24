/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/runtime/host.h"
#include "agb/apu/apu.h"
#include "agb/ppu/ppu.h"
#include "common/utilities/pad_map.h"
#include "common/utilities/window_icon.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a frame of the console's 16.78 MHz clock: 228 lines of 1232 cycles, 59.73 a second */
#define FRAME_NS      ((uint64_t)AGB_SCANLINE_CYCLES * AGB_FRAME_LINES * 1000000000ull / 16777216ull)
/* the window at first: the screen four times */
#define START_SCALE   4
/* the sound queued at most, frames of it: past that the speaker is behind and a frame's sound is dropped */
#define QUEUED_FRAMES 6
/* a stick past this far holds its direction */
#define STICK_DEAD    16384

struct AgbHost {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    SDL_AudioStream* audio;
    SnxPadMap pad;
    uint16_t keyboard, buttons; /* the keys the keyboard and the gamepads hold */
    uint64_t due;               /* when the next frame goes on screen, SDL_GetTicksNS's */
};

uint16_t agb_host_key(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_X:
        case SDL_SCANCODE_SPACE: return AGB_KEY_A;
        case SDL_SCANCODE_Z: return AGB_KEY_B;
        case SDL_SCANCODE_Q: return AGB_KEY_L;
        case SDL_SCANCODE_E: return AGB_KEY_R;
        case SDL_SCANCODE_RETURN: return AGB_KEY_START;
        case SDL_SCANCODE_BACKSPACE: return AGB_KEY_SELECT;
        case SDL_SCANCODE_LEFT: return AGB_KEY_LEFT;
        case SDL_SCANCODE_UP: return AGB_KEY_UP;
        case SDL_SCANCODE_RIGHT: return AGB_KEY_RIGHT;
        case SDL_SCANCODE_DOWN: return AGB_KEY_DOWN;
        default: return 0;
    }
}

uint16_t agb_host_button(int bit) {
    switch (bit) {
        case 0: return AGB_KEY_A;
        case 1: return AGB_KEY_B;
        case 6:
        case 8: return AGB_KEY_L; /* L and ZL */
        case 7:
        case 9: return AGB_KEY_R; /* R and ZR */
        case 10: return AGB_KEY_START;
        case 11: return AGB_KEY_SELECT;
        case 12: return AGB_KEY_LEFT;
        case 13: return AGB_KEY_UP;
        case 14: return AGB_KEY_RIGHT;
        case 15: return AGB_KEY_DOWN;
        default: return 0;
    }
}

/* SDL's gamepad control, as pad_map.h names it by where it sits */
static int control_of(SDL_GamepadButton button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return SNX_PAD_SOUTH;
        case SDL_GAMEPAD_BUTTON_EAST: return SNX_PAD_EAST;
        case SDL_GAMEPAD_BUTTON_WEST: return SNX_PAD_WEST;
        case SDL_GAMEPAD_BUTTON_NORTH: return SNX_PAD_NORTH;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return SNX_PAD_LB;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return SNX_PAD_RB;
        case SDL_GAMEPAD_BUTTON_BACK: return SNX_PAD_BACK;
        case SDL_GAMEPAD_BUTTON_START: return SNX_PAD_START;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return SNX_PAD_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return SNX_PAD_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return SNX_PAD_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return SNX_PAD_RIGHT;
        default: return SNX_PAD_NONE;
    }
}

/* every open gamepad's buttons, triggers and left stick, through the console's map */
static uint16_t gamepads(const AgbHost* host) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    uint16_t keys = 0;
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            int control = control_of((SDL_GamepadButton)b);
            if (control != SNX_PAD_NONE && host->pad.bit[control] >= 0 &&
                SDL_GetGamepadButton(pad, (SDL_GamepadButton)b))
                keys |= agb_host_button(host->pad.bit[control]);
        }
        if (host->pad.bit[SNX_PAD_LT] >= 0 && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > STICK_DEAD)
            keys |= agb_host_button(host->pad.bit[SNX_PAD_LT]);
        if (host->pad.bit[SNX_PAD_RT] >= 0 && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > STICK_DEAD)
            keys |= agb_host_button(host->pad.bit[SNX_PAD_RT]);
        {
            int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            if (x < -STICK_DEAD) keys |= AGB_KEY_LEFT;
            if (x > STICK_DEAD) keys |= AGB_KEY_RIGHT;
            if (y < -STICK_DEAD) keys |= AGB_KEY_UP;
            if (y > STICK_DEAD) keys |= AGB_KEY_DOWN;
        }
    }
    SDL_free(ids);
    return keys;
}

AgbHost* agb_host_open(const char* title, bool fullscreen, char* error, size_t error_size) {
    AgbHost* host = (AgbHost*)calloc(1, sizeof(*host));
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, AGB_APU_RATE};
    if (!host) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        snprintf(error, error_size, "cannot start SDL's video: %s", SDL_GetError());
        free(host);
        return NULL;
    }
    if (!SDL_CreateWindowAndRenderer(title, AGB_SCREEN_WIDTH * START_SCALE, AGB_SCREEN_HEIGHT * START_SCALE,
                                     SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0), &host->window,
                                     &host->renderer) ||
        !(host->texture = SDL_CreateTexture(host->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                            AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT)) ||
        !SDL_SetTextureScaleMode(host->texture, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetRenderLogicalPresentation(host->renderer, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT,
                                          SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)) {
        snprintf(error, error_size, "cannot open the game's window: %s", SDL_GetError());
        agb_host_close(host);
        return NULL;
    }
    snx_window_icon(host->window);
    snx_pad_map_position(&host->pad); /* A where the console has it, east */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO) ||
        !(host->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)) ||
        !SDL_ResumeAudioStreamDevice(host->audio)) {
        fprintf(stderr, "scarletnx-agb: no sound: %s\n", SDL_GetError());
        if (host->audio) SDL_DestroyAudioStream(host->audio);
        host->audio = NULL;
    }
    host->due = SDL_GetTicksNS() + FRAME_NS;
    return host;
}

void agb_host_close(AgbHost* host) {
    if (!host) return;
    if (host->audio) SDL_DestroyAudioStream(host->audio);
    if (host->texture) SDL_DestroyTexture(host->texture);
    if (host->renderer) SDL_DestroyRenderer(host->renderer);
    if (host->window) SDL_DestroyWindow(host->window);
    free(host);
    SDL_Quit();
}

bool agb_host_poll(AgbHost* host, uint16_t* keys) {
    SDL_Event event;
    bool open = true;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: open = false; break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(event.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_F11 && !event.key.repeat)
                    SDL_SetWindowFullscreen(host->window, !(SDL_GetWindowFlags(host->window) & SDL_WINDOW_FULLSCREEN));
                host->keyboard |= agb_host_key(event.key.scancode);
                break;
            case SDL_EVENT_KEY_UP: host->keyboard &= (uint16_t)~agb_host_key(event.key.scancode); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: host->keyboard = 0; break; /* no key held where it cannot be let go */
            default: break;
        }
    }
    host->buttons = gamepads(host);
    *keys = (uint16_t)(host->keyboard | host->buttons);
    return open;
}

void agb_host_frame(AgbHost* host, const uint32_t* framebuffer, const int16_t* samples, uint32_t count) {
    uint64_t now;
    SDL_UpdateTexture(host->texture, NULL, framebuffer, AGB_SCREEN_WIDTH * 4);
    SDL_SetRenderDrawColor(host->renderer, 0, 0, 0, 255);
    SDL_RenderClear(host->renderer);
    SDL_RenderTexture(host->renderer, host->texture, NULL, NULL);
    SDL_RenderPresent(host->renderer);
    if (host->audio && SDL_GetAudioStreamQueued(host->audio) < (int)(QUEUED_FRAMES * count * 4))
        SDL_PutAudioStreamData(host->audio, samples, (int)(count * 4));
    now = SDL_GetTicksNS();
    if (host->due > now)
        SDL_DelayPrecise(host->due - now);
    else if (now - host->due > 3 * FRAME_NS) /* behind by more than a few frames: start the count again, no rush */
        host->due = now;
    host->due += FRAME_NS;
}
