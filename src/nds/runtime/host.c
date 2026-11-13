/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The run's side of the host: the Vulkan device and its window (gpu/vk), the keyboard, the gamepads and the pointer,
   and the pace of the console's frames. */
#include "nds/runtime/host.h"
#include "nds/audio/spu.h"
#include "nds/gpu/vk/vk.h"
#include "nds/gpu/vk/vk_present.h"
#include "common/utilities/common.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/quit_key.h"
#include "common/utilities/window_icon.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a frame of the console's: 263 lines of 2130 bus cycles */
#define FRAME_NS   ((uint64_t)NDS_FRAME_CYCLES * 1000000000ull / NDS_CLOCK)
#define STICK_DEAD 16384

struct NdsHost {
    NdsBoard* board;
    int headless, gpu, paced;
    SnxPadMap pad;
    uint16_t keyboard;
    uint8_t pointer_down;
    float pointer_x, pointer_y;
    uint64_t due, quit_by;
    SDL_AudioStream* audio;
    FILE* wav;
    uint32_t wav_frames;
};

/* the sound queued at most, in frames of the console's: past that the speaker is behind and a frame's sound is dropped */
#define QUEUED_FRAMES 6
#define FRAME_SAMPLES 548u

static void wav_header(FILE* f, uint32_t frames) {
    unsigned char h[44] = "RIFF\0\0\0\0WAVEfmt ";
    uint32_t rate = (uint32_t)(NDS_SPU_RATE + 0.5), bytes = frames * 4;
    uint32_t fields[] = {36 + bytes, 0, 0, 16, 0x00020001u, rate, rate * 4, 0x00100004u, 0, bytes};
    memcpy(h + 4, &fields[0], 4);
    memcpy(h + 16, &fields[3], 4);
    memcpy(h + 20, &fields[4], 4);
    memcpy(h + 24, &fields[5], 4);
    memcpy(h + 28, &fields[6], 4);
    memcpy(h + 32, &fields[7], 4);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &fields[9], 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

/* the keyboard's keys, as the other consoles' windows have them, with S and A for X and Y */
static uint16_t key_of(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_X:
        case SDL_SCANCODE_SPACE: return 0x001;
        case SDL_SCANCODE_Z: return 0x002;
        case SDL_SCANCODE_BACKSPACE: return 0x004;
        case SDL_SCANCODE_RETURN: return 0x008;
        case SDL_SCANCODE_RIGHT: return 0x010;
        case SDL_SCANCODE_LEFT: return 0x020;
        case SDL_SCANCODE_UP: return 0x040;
        case SDL_SCANCODE_DOWN: return 0x080;
        case SDL_SCANCODE_E: return 0x100;
        case SDL_SCANCODE_Q: return 0x200;
        case SDL_SCANCODE_S: return 0x400;
        case SDL_SCANCODE_A: return 0x800;
        default: return 0;
    }
}

/* a gamepad's control, by the Switch's bit the map gives it: A, B, X, Y, the shoulders and triggers, +, -, the pad */
static uint16_t button_of(int bit) {
    switch (bit) {
        case 0: return 0x001;
        case 1: return 0x002;
        case 2: return 0x400;
        case 3: return 0x800;
        case 6:
        case 8: return 0x200;
        case 7:
        case 9: return 0x100;
        case 10: return 0x008;
        case 11: return 0x004;
        case 12: return 0x020;
        case 13: return 0x040;
        case 14: return 0x010;
        case 15: return 0x080;
        default: return 0;
    }
}

static uint16_t gamepads(const NdsHost* host) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    uint16_t keys = 0;
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        for (int c = 0; c < SNX_PAD_CONTROLS; ++c)
            if (host->pad.bit[c] >= 0 && snx_pad_held(pad, c)) keys |= button_of(host->pad.bit[c]);
        int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -STICK_DEAD) keys |= 0x020;
        if (x > STICK_DEAD) keys |= 0x010;
        if (y < -STICK_DEAD) keys |= 0x040;
        if (y > STICK_DEAD) keys |= 0x080;
    }
    SDL_free(ids);
    return keys;
}

NdsHost* nds_host_open(NdsBoard* board, const char* title, const char* wav, int headless, int fullscreen, char* error,
                       size_t error_size) {
    NdsHost* host = calloc(1, sizeof *host);
    if (!host) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    host->board = board;
    host->headless = headless;
    host->paced = !headless;
    if (!headless && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        snprintf(error, error_size, "cannot start SDL's video: %s", SDL_GetError());
        free(host);
        return NULL;
    }
    char why[256] = "";
    if (nds_vk_init(title, headless != 0, why, sizeof why)) {
        if (nds_vk_render3d_init(why, sizeof why) && nds_vk_frame_init(why, sizeof why) &&
            nds_vk_present_init(why, sizeof why))
            host->gpu = 1;
        else {
            nds_vk_present_free();
            nds_vk_frame_free();
            nds_vk_render3d_free();
            nds_vk_free();
        }
    }
    if (!host->gpu) {
        /* everything the console shows is drawn on the GPU: without one the game runs and shows nothing */
        fprintf(stderr, "scarletnx-nds: no Vulkan device (%s): no frames will be drawn, shown or saved\n", why);
        if (!headless) {
            snprintf(error, error_size, "no Vulkan device for the game's window: %s", why);
            SDL_Quit();
            free(host);
            return NULL;
        }
    }
    NdsVk* v = nds_vk();
    if (v && v->host.window) {
        snx_window_icon(v->host.window);
        if (fullscreen) SDL_SetWindowFullscreen(v->host.window, true);
        snx_pad_map_position(&host->pad);
    }
    if (!headless) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)(NDS_SPU_RATE + 0.5)};
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO) ||
            !(host->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)) ||
            !SDL_ResumeAudioStreamDevice(host->audio)) {
            fprintf(stderr, "scarletnx-nds: no sound: %s\n", SDL_GetError());
            if (host->audio) SDL_DestroyAudioStream(host->audio);
            host->audio = NULL;
        }
    }
    if (wav && (host->wav = fopen(wav, "wb"))) wav_header(host->wav, 0);
    host->due = SDL_GetTicksNS() + FRAME_NS;
    return host;
}

void nds_host_close(NdsHost* host) {
    if (!host) return;
    if (host->audio) SDL_DestroyAudioStream(host->audio);
    if (host->wav) {
        wav_header(host->wav, host->wav_frames);
        fclose(host->wav);
    }
    if (host->gpu) {
        nds_vk_present_free();
        nds_vk_frame_free();
        nds_vk_render3d_free();
        nds_vk_free();
    }
    if (!host->headless) SDL_Quit();
    free(host);
}

bool nds_host_poll(NdsHost* host, NdsInput* input) {
    bool open = true;
    if (!host->headless) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: open = false; break;
                case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(event.gdevice.which); break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE && !event.key.repeat &&
                        !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
                        char warning[64];
                        if (snx_quit_press(&host->quit_by, SDL_GetTicksNS()))
                            open = false;
                        else {
                            snx_quit_warning(warning, sizeof warning, "Esc");
                            fprintf(stderr, "scarletnx-nds: %s\n", warning);
                        }
                    } else if (event.key.key == SDLK_F11 && !event.key.repeat && nds_vk() && nds_vk()->host.window) {
                        SDL_Window* window = nds_vk()->host.window;
                        SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                    }
                    host->keyboard |= key_of(event.key.scancode);
                    break;
                case SDL_EVENT_KEY_UP: host->keyboard &= (uint16_t)~key_of(event.key.scancode); break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        host->pointer_down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                        host->pointer_x = event.button.x, host->pointer_y = event.button.y;
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION: host->pointer_x = event.motion.x, host->pointer_y = event.motion.y; break;
                default: break;
            }
        }
        input->keys = host->keyboard | gamepads(host);
        /* the pointer held on the lower screen is the pen */
        if (host->pointer_down && nds_vk() && nds_vk()->host.window) {
            int x, y;
            float scale = SDL_GetWindowPixelDensity(nds_vk()->host.window);
            if (nds_vk_present_point(host->pointer_x * scale, host->pointer_y * scale, &x, &y) &&
                y >= (int)NDS_SCREEN_HEIGHT) {
                input->touching = 1;
                input->touch_x = (uint16_t)x, input->touch_y = (uint16_t)(y - (int)NDS_SCREEN_HEIGHT);
            }
        }
    }
    return open;
}

void nds_host_frame(NdsHost* host) {
    /* the frame's sound: to the speaker unless it is too far behind, and to the file */
    int16_t samples[FRAME_SAMPLES * 4 * 2];
    nds_spu_run(host->board);
    uint32_t count = nds_spu_take(host->board, samples, FRAME_SAMPLES * 4);
    if (count && host->audio &&
        SDL_GetAudioStreamQueued(host->audio) < (int)(QUEUED_FRAMES * FRAME_SAMPLES * 4))
        SDL_PutAudioStreamData(host->audio, samples, (int)(count * 4));
    if (count && host->wav) {
        fwrite(samples, 4, count, host->wav);
        host->wav_frames += count;
    }
    if (!host->paced) return;
    /* the frame's time by the host's clock: a late frame does not hurry the ones after it past a frame's worth */
    uint64_t now = SDL_GetTicksNS();
    if (now < host->due)
        SDL_DelayPrecise(host->due - now);
    else if (now - host->due > FRAME_NS * 4)
        host->due = now;
    host->due += FRAME_NS;
}

int nds_host_shot(NdsHost* host, const char* path) {
    if (!host->gpu) return -1;
    unsigned char* rgba = malloc(NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2 * 4);
    int result = -1;
    FILE* file = rgba && nds_vk_frame_read(rgba) ? fopen(path, "wb") : NULL;
    if (file) {
        result = snx_ppm_write(file, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT * 2, rgba, 4, NDS_SCREEN_WIDTH * 4, SNX_PPM_RGBA) ? -1 : 0;
        fclose(file);
    }
    free(rgba);
    return result;
}
