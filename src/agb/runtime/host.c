/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/renderdoc.h"
#include "agb/runtime/host.h"
#include "common/utilities/sdl_desktop.h"
#include "agb/apu/apu.h"
#include "agb/ppu/ppu.h"
#include "agb/runtime/vk.h"
#include "common/utilities/notice.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/audio_volume.h"
#include "common/utilities/window_fullscreen.h"
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
    /* the screen through Vulkan (runtime/vk.c), as the other consoles' windows; SDL's renderer and its texture where
       the host has no Vulkan window */
    AgbVk* vk;
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    SDL_AudioStream* audio;
    SnxPadMap pad;
    uint16_t keyboard, buttons; /* the keys the keyboard and the gamepads hold */
    uint64_t due;               /* when the next frame goes on screen, SDL_GetTicksNS's */
    /* a line over the game for a few seconds, as a Switch game shows one (common/utilities/notice.h): what the
       runtime's keys say. Its band's texture, made again for another size */
    SnxNotice notice;
    char* captures; /* the game's folder with its separator, whose captures/ a screenshot goes in */
    bool shot;      /* the screenshot key's: the next frame saved */
    int state_ask;  /* a save states' key (HOTKEY_SAVE_STATE...) for the loop between two frames, -1 none */
    int vsync; /* SDL's renderer's: on at the console's pace with vsync, off at another, as a Switch game's follows its
                  limit */
    SDL_Texture* notice_texture;
    int notice_w, notice_h, notice_failed;
    bool rumbling, rumble_said; /* the gamepads shake, as the cartridge's motor last asked; one that could not, said */
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

unsigned agb_host_button(int bit) {
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

void agb_host_say(const char* line) { fprintf(stderr, "scarletnx-agb: %s\n", line); }
/* the open window, whose notice shows what the runtime's keys say */
static struct {
    AgbHost* open;
} g_host;
static void show(const char* line) {
    if (g_host.open) snx_notice_show(&g_host.open->notice, line, SDL_GetTicksNS());
}
/* a runtime's key the window does: the screenshot's, at the next frame; another said to do nothing, with why */
static void key_action(AgbHost* host, int action) {
    if (action == HOTKEY_SCREENSHOT) {
        host->shot = true;
        return;
    }
    if (action == HOTKEY_SAVE_STATE || action == HOTKEY_LOAD_STATE || action == HOTKEY_NEXT_SLOT) {
        host->state_ask = action;
        return;
    }
    if (action == HOTKEY_CAPTURE) { /* RenderDoc's, the frame after the next present */
        (void)snx_renderdoc_capture(1);
        return;
    }
    snx_vpad_window_none(action, action == HOTKEY_THREAD_DUMP ? "it runs on one CPU, with no threads"
                                 : action == HOTKEY_MOUSE     ? "its mouse is the pointer of the on-screen controls"
                                 : action == HOTKEY_TV_MODE   ? "the TV mode is the Switch's console docked"
                                 : action == HOTKEY_UPSCALER || action == HOTKEY_ANTI_ALIASING
                                     ? "its screen is drawn in sharp pixels, with no upscaler nor anti-aliasing"
                                     : "its window has none of it yet");
}

AgbHost* agb_host_open(const char* title, const char* ini, bool fullscreen, char* error, size_t error_size) {
    AgbHost* host = (AgbHost*)calloc(1, sizeof(*host));
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, AGB_APU_RATE};
    if (!host) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    snx_sdl_desktop_hints();
    snx_vpad_sdl_hints();
    snx_pad_background(g_settings.background_input);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        snprintf(error, error_size, "cannot start SDL's video: %s", SDL_GetError());
        free(host);
        return NULL;
    }
    /* the screen through Vulkan, as every other console's window, where the host has a Vulkan window; else SDL's
       renderer, said */
    {
        char why[256];
        host->vk = agb_vk_open(title, AGB_SCREEN_WIDTH * START_SCALE, AGB_SCREEN_HEIGHT * START_SCALE, why, sizeof why);
        if (!host->vk) {
            fprintf(stderr, "scarletnx-agb: no Vulkan window (%s): the screen shows through SDL's renderer\n", why);
            /* the upscaler and the anti-aliasing are the Vulkan window's draw (common/gpu/vk/present_fx.h) */
            if (strcmp(g_settings.upscaler, "off") || strcmp(g_settings.anti_aliasing, "off"))
                fprintf(stderr,
                        "scarletnx-agb: upscaler=%s anti_aliasing=%s: SDL's renderer shows the screen in sharp pixels, "
                        "with neither\n",
                        g_settings.upscaler, g_settings.anti_aliasing);
        }
    }
    if (host->vk)
        host->window = agb_vk_window(host->vk);
    else if (!SDL_CreateWindowAndRenderer(title, AGB_SCREEN_WIDTH * START_SCALE, AGB_SCREEN_HEIGHT * START_SCALE,
                                          SDL_WINDOW_RESIZABLE, &host->window, &host->renderer) ||
             !(host->texture = SDL_CreateTexture(host->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                                 AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT)) ||
             !SDL_SetTextureScaleMode(host->texture, SDL_SCALEMODE_NEAREST) ||
             !SDL_SetRenderLogicalPresentation(host->renderer, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT,
                                               SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)) {
        snprintf(error, error_size, "cannot open the game's window: %s", SDL_GetError());
        agb_host_close(host);
        return NULL;
    }
    /* each frame whole on screen with vsync, as the Vulkan window's images go */
    if (!host->vk) {
        host->vsync = g_settings.vsync != 0;
        if (!SDL_SetRenderVSync(host->renderer, host->vsync))
            fprintf(stderr, "scarletnx-agb: the display refused vsync %d (%s): frames go on screen with vsync %d%s\n",
                    host->vsync, SDL_GetError(), !host->vsync, host->vsync ? " and may tear" : "");
        snx_window_icon(host->window);
    }
    char why[256];
    if (!snx_window_fullscreen(host->window, fullscreen, why, sizeof why)) fprintf(stderr, "scarletnx-agb: %s\n", why);
    /* A where the console has it, east, the player's map over it and A and B swapped as the Switch's settings say */
    snx_pad_map_console(&host->pad, agb_host_button, "GBA", g_settings.button_map, g_settings.swap_a_b);
    g_host.open = host;
    host->state_ask = -1;
    {
        char* folder = ini ? snx_dirname(ini) : NULL;
        host->captures = snx_format("%s/", folder ? folder : ".");
        free(folder);
    }
    snx_vpad_window_open(host->window, ini, agb_host_say, show);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO) ||
        !(host->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)) ||
        !SDL_ResumeAudioStreamDevice(host->audio)) {
        fprintf(stderr, "scarletnx-agb: no sound: %s\n", SDL_GetError());
        if (host->audio) SDL_DestroyAudioStream(host->audio);
        host->audio = NULL;
    } else if (!snx_audio_volume_open(host->audio, g_settings.volume, why, sizeof why)) /* the player's Volume */
        fprintf(stderr, "scarletnx-agb: %s\n", why);
    host->due = SDL_GetTicksNS() + FRAME_NS;
    return host;
}

void agb_host_rumble(AgbHost* host, float share) {
    if (share <= 0 && !host->rumbling) return;
    host->rumbling = share > 0;
    char why[256];
    /* two frames' worth, which the next frame's call renews: a motor the game keeps on shakes without a gap */
    if (snx_pads_rumble(share, g_settings.vibration, 34, why, sizeof why) && !host->rumble_said) {
        host->rumble_said = true;
        fprintf(stderr, "scarletnx-agb: %s\n", why);
    }
}

float agb_host_volume(const AgbHost* host) {
    return host && host->audio ? SDL_GetAudioDeviceGain(SDL_GetAudioStreamDevice(host->audio)) : -1;
}

void agb_host_close(AgbHost* host) {
    if (!host) return;
    if (g_host.open == host) g_host.open = NULL;
    free(host->captures);
    snx_vpad_window_close();
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

bool agb_host_poll(AgbHost* host, uint16_t* keys) {
    SDL_Event event;
    bool open = true;
    while (SDL_PollEvent(&event)) {
        int action;
        SnxVpadEvent took = snx_vpad_window_event(&event, &action);
        if (took == SNX_VPAD_QUIT) open = false;
        if (took == SNX_VPAD_ACTION) key_action(host, action);
        if (took != SNX_VPAD_PASS) continue;
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: open = false; break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(event.gdevice.which); break;
            case SDL_EVENT_KEY_DOWN: host->keyboard |= agb_host_key(event.key.scancode); break;
            case SDL_EVENT_KEY_UP: host->keyboard &= (uint16_t)~agb_host_key(event.key.scancode); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: host->keyboard = 0; break; /* no key held where it cannot be let go */
            default: break;
        }
    }
    host->buttons = gamepads(host);
    *keys = (uint16_t)(host->keyboard | host->buttons);
    u64 pressed;
    i32 sticks[4]; /* the GBA has none */
    snx_vpad_window_state(&pressed, sticks);
    for (int bit = 0; bit < SNX_PAD_BUTTONS; ++bit)
        if (pressed >> bit & 1) *keys |= agb_host_button(bit);
    if (vpad_editing()) *keys = 0;
    snx_vpad_window_showcase(*keys, agb_host_button, NULL);
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
    /* the on-screen controls in the window's own pixels, as the notice */
    SDL_SetRenderLogicalPresentation(host->renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    snx_vpad_window_render(host->renderer);
    SDL_SetRenderLogicalPresentation(host->renderer, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
}

const char* agb_host_notice(const AgbHost* host) {
    return snx_notice_shown(&host->notice, SDL_GetTicksNS()) ? host->notice.text : "";
}

/* the frame shown, as the Switch's screenshot is: a PPM in the game's captures/, said where */
static void shot(AgbHost* host, const uint32_t* framebuffer) {
    char path[SNX_PATH_SIZE], line[SNX_PATH_SIZE + 64], why[160];
    host->shot = false;
    snx_capture_path(path, sizeof path - 4, host->captures ? host->captures : "./");
    strcat(path, ".ppm");
    FILE* f = fopen(path, "wb");
    int failed = !f || snx_ppm_write(f, AGB_SCREEN_WIDTH, AGB_SCREEN_HEIGHT, (const unsigned char*)framebuffer, 4,
                                     AGB_SCREEN_WIDTH * 4, SNX_PPM_RGBA);
    if (f && fclose(f)) failed = 1;
    if (failed) {
        snprintf(line, sizeof line, "the screenshot could not be written to %s: %s", path,
                 snx_os_error(why, sizeof why));
        agb_host_say(line);
        show("Screenshot not saved: the log says why");
        return;
    }
    snprintf(line, sizeof line, "screenshot: %s", path);
    agb_host_say(line);
    show("Screenshot saved in captures");
}
int agb_host_state_ask(AgbHost* host) {
    int ask = host->state_ask;
    host->state_ask = -1;
    return ask;
}
bool agb_host_held(AgbHost* host, const uint32_t* framebuffer) {
    if (!snx_vpad_window_paused()) return false;
    agb_host_frame(host, framebuffer, NULL, 0);
    return true;
}
void agb_host_frame(AgbHost* host, const uint32_t* framebuffer, const int16_t* samples, uint32_t count) {
    if (host->shot) shot(host, framebuffer);
    if (host->vk)
        (void)agb_vk_frame(host->vk, framebuffer, &host->notice); /* no image for it: the next frame goes */
    else {
        int vsync = g_settings.vsync && snx_vpad_window_frame_ns(FRAME_NS) == FRAME_NS;
        if (vsync != host->vsync && SDL_SetRenderVSync(host->renderer, vsync)) host->vsync = vsync;
        agb_host_draw(host, framebuffer);
        SDL_RenderPresent(host->renderer);
    }
    if (host->audio && SDL_GetAudioStreamQueued(host->audio) < (int)(QUEUED_FRAMES * count * 4))
        SDL_PutAudioStreamData(host->audio, samples, (int)(count * 4));
    /* at fps_limit's pace (the console's, none, or N frames a second), which F4 switches */
    uint64_t now = SDL_GetTicksNS(), step = snx_vpad_window_frame_ns(FRAME_NS);
    if (!step) return;
    if (host->due > now)
        SDL_DelayPrecise(host->due - now);
    else if (now - host->due > 3 * step) /* behind by more than a few frames: start the count again, no rush */
        host->due = now;
    host->due += step;
}
