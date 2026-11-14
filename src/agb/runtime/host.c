/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/runtime/host.h"
#include "common/utilities/sdl_desktop.h"
#include "agb/apu/apu.h"
#include "agb/ppu/ppu.h"
#include "agb/runtime/vk.h"
#include "common/utilities/key_text.h"
#include "common/utilities/notice.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/quit_key.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
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
    /* the screen through Vulkan (runtime/vk.c), with the upscaler and the anti-aliasing; SDL's renderer and its
       texture where the host has no Vulkan window */
    AgbVk* vk;
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    SDL_AudioStream* audio;
    SnxPadMap pad;
    uint16_t keyboard, buttons; /* the keys the keyboard and the gamepads hold */
    uint64_t due;               /* when the next frame goes on screen, SDL_GetTicksNS's */
    uint64_t quit_by;           /* a second Esc before this closes the window (common/utilities/quit_key.h) */
    /* a line over the game for a few seconds, as a Switch game shows one (common/utilities/notice.h): Esc's first
       press. Its band's texture, made again for another size */
    SnxNotice notice;
    SDL_Texture* notice_texture;
    int notice_w, notice_h, notice_failed;
    /* the keys that step the upscaler and the anti-aliasing for the run (the Switch's hotkey_upscaler and
       hotkey_anti_aliasing), 0 for none, read at the first key */
    unsigned shortcut_key[2], shortcut_mods[2];
    bool shortcuts_read, shared_said;
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

/* every open gamepad's buttons, triggers and left stick, through the console's map */
static uint16_t gamepads(const AgbHost* host) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    uint16_t keys = 0;
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        for (int c = 0; c < SNX_PAD_CONTROLS; ++c)
            if (host->pad.bit[c] >= 0 && snx_pad_held(pad, c)) keys |= agb_host_button(host->pad.bit[c]);
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
    snx_sdl_desktop_hints();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        snprintf(error, error_size, "cannot start SDL's video: %s", SDL_GetError());
        free(host);
        return NULL;
    }
    /* the screen through Vulkan, as every other console's window, where the host has a Vulkan window; else SDL's
       renderer, without the upscaler and the anti-aliasing, said */
    {
        char why[256];
        host->vk = agb_vk_open(title, fullscreen, why, sizeof why);
        if (!host->vk)
            fprintf(
                stderr,
                "scarletnx-agb: no Vulkan window (%s): the screen shows through SDL's renderer, with no upscaler or "
                "anti-aliasing\n",
                why);
    }
    if (host->vk)
        host->window = agb_vk_window(host->vk);
    else if (!SDL_CreateWindowAndRenderer(title, AGB_SCREEN_WIDTH * START_SCALE, AGB_SCREEN_HEIGHT * START_SCALE,
                                          SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0),
                                          &host->window, &host->renderer) ||
             !(host->texture = SDL_CreateTexture(host->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                                 AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT)) ||
             !SDL_SetTextureScaleMode(host->texture, SDL_SCALEMODE_NEAREST) ||
             !SDL_SetRenderLogicalPresentation(host->renderer, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT,
                                               SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)) {
        snprintf(error, error_size, "cannot open the game's window: %s", SDL_GetError());
        agb_host_close(host);
        return NULL;
    }
    /* each frame whole on screen, as the launcher presents its own (the Vulkan window's images go at the display's
       refresh too) */
    if (!host->vk) {
        if (!SDL_SetRenderVSync(host->renderer, 1))
            fprintf(stderr,
                    "scarletnx-agb: the display refused vsync 1 (%s): frames go on screen with vsync 0 and may tear\n",
                    SDL_GetError());
        snx_window_icon(host->window);
    }
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
    if (host->notice_texture) SDL_DestroyTexture(host->notice_texture);
    if (host->texture) SDL_DestroyTexture(host->texture);
    if (host->renderer) SDL_DestroyRenderer(host->renderer);
    if (host->vk)
        agb_vk_close(host->vk); /* its window with it */
    else if (host->window)
        SDL_DestroyWindow(host->window);
    free(host);
    SDL_Quit();
}

/* the upscaler's or the anti-aliasing's shortcut, as a Switch game's: the step for the run said over the game, or,
   with SDL's renderer, that it has neither. 0 for another key */
static int shortcut(AgbHost* host, const SDL_KeyboardEvent* key) {
    int also, which;
    char text[96];
    if (!host->shortcuts_read) {
        const char* const texts[2] = {g_settings.upscaler_shortcut, g_settings.anti_aliasing_shortcut};
        host->shortcuts_read = true;
        for (int i = 0; i < 2; ++i)
            if (snx_key_parse(texts[i], &host->shortcut_key[i], &host->shortcut_mods[i], text, sizeof text))
                fprintf(stderr, "scarletnx-agb: %s=%s: %s: it has no key\n",
                        i ? "hotkey_anti_aliasing" : "hotkey_upscaler", texts[i], text);
    }
    which = snx_key_find(host->shortcut_key, host->shortcut_mods, 2, key->key, snx_key_mods(key->mod), &also);
    if (which < 0) return 0;
    if (also >= 0 && !host->shared_said) {
        host->shared_said = true;
        fprintf(stderr, "scarletnx-agb: %s is hotkey_upscaler's and hotkey_anti_aliasing's: it does %s's\n",
                g_settings.upscaler_shortcut, which ? "hotkey_anti_aliasing" : "hotkey_upscaler");
    }
    if (host->vk)
        agb_vk_step(host->vk, which, text, sizeof text);
    else {
        snprintf(text, sizeof text, "%s: Off, with no Vulkan window", which ? "Anti-aliasing" : "Upscaler");
        fprintf(stderr, "scarletnx-agb: %s\n", text);
    }
    snx_notice_show(&host->notice, text, SDL_GetTicksNS());
    return 1;
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
                if (!event.key.repeat && shortcut(host, &event.key)) break;
                /* Esc twice closes the window, a first one warning on the terminal and over the game; not with Ctrl,
                   Alt or the system key */
                if (event.key.key == SDLK_ESCAPE && !event.key.repeat &&
                    !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                    char warning[64];
                    if (snx_quit_press(&host->quit_by, SDL_GetTicksNS())) {
                        open = false;
                        break;
                    }
                    snx_quit_warning(warning, sizeof warning, "Esc");
                    fprintf(stderr, "scarletnx-agb: %s\n", warning);
                    snx_notice_show(&host->notice, warning, SDL_GetTicksNS());
                    break;
                }
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

/* the notice over the frame, in the window's own pixels: the logical presentation that scales the screen is off for
   it. A texture that cannot be made is said once, and the notice shows on the terminal alone */
static void notice_draw(AgbHost* host) {
    int sw, sh, rect[4], pitch;
    void* pixels;
    if (!snx_notice_shown(&host->notice, SDL_GetTicksNS()) || host->notice_failed ||
        !SDL_GetCurrentRenderOutputSize(host->renderer, &sw, &sh))
        return;
    int scale = snx_notice_scale(sw, sh, snx_display_percent(SDL_GetWindowDisplayScale(host->window)));
    if (!snx_notice_rect(&host->notice, sw, sh, scale, rect)) return;
    if (!host->notice_texture || host->notice_w != rect[2] || host->notice_h != rect[3]) {
        if (host->notice_texture) SDL_DestroyTexture(host->notice_texture);
        host->notice_texture =
            SDL_CreateTexture(host->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, rect[2], rect[3]);
        host->notice_w = rect[2];
        host->notice_h = rect[3];
    }
    if (!host->notice_texture || !SDL_LockTexture(host->notice_texture, NULL, &pixels, &pitch)) {
        fprintf(stderr, "scarletnx-agb: the notice over the game cannot be drawn (%s): it shows here alone\n",
                SDL_GetError());
        host->notice_failed = 1;
        return;
    }
    snx_notice_pixels(&host->notice, scale, 0, rect, pixels, (size_t)pitch);
    SDL_UnlockTexture(host->notice_texture);
    SDL_FRect at = {(float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3]};
    SDL_SetRenderLogicalPresentation(host->renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_RenderTexture(host->renderer, host->notice_texture, NULL, &at);
    SDL_SetRenderLogicalPresentation(host->renderer, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
}

void agb_host_draw(AgbHost* host, const uint32_t* framebuffer) {
    if (!host->renderer) return; /* the Vulkan window's frames go through agb_host_frame */
    SDL_UpdateTexture(host->texture, NULL, framebuffer, AGB_SCREEN_WIDTH * 4);
    SDL_SetRenderDrawColor(host->renderer, 0, 0, 0, 255);
    SDL_RenderClear(host->renderer);
    SDL_RenderTexture(host->renderer, host->texture, NULL, NULL);
    notice_draw(host);
}

const char* agb_host_notice(const AgbHost* host) {
    return snx_notice_shown(&host->notice, SDL_GetTicksNS()) ? host->notice.text : "";
}

void agb_host_frame(AgbHost* host, const uint32_t* framebuffer, const int16_t* samples, uint32_t count) {
    if (host->vk)
        (void)agb_vk_frame(host->vk, framebuffer, &host->notice); /* none free: the next frame goes */
    else {
        agb_host_draw(host, framebuffer);
        SDL_RenderPresent(host->renderer);
    }
    if (host->audio && SDL_GetAudioStreamQueued(host->audio) < (int)(QUEUED_FRAMES * count * 4))
        SDL_PutAudioStreamData(host->audio, samples, (int)(count * 4));
    uint64_t now = SDL_GetTicksNS();
    if (host->due > now)
        SDL_DelayPrecise(host->due - now);
    else if (now - host->due > 3 * FRAME_NS) /* behind by more than a few frames: start the count again, no rush */
        host->due = now;
    host->due += FRAME_NS;
}
