/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The run's side of the host: the Vulkan device and its window (gpu/vk), the keyboard, the gamepads, the on-screen
   controls and the pen, and the pace of the console's frames. */
#include "common/gpu/vk/renderdoc.h"
#include "nds/runtime/host.h"
#include "nds/audio/spu.h"
#include "nds/gpu/vk/vk.h"
#include "nds/gpu/vk/vk_present.h"
#include "common/utilities/common.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/state_header.h"
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/audio_volume.h"
#include "common/utilities/window_fullscreen.h"
#include "common/utilities/window_icon.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a frame of the console's: 263 lines of 2130 bus cycles */
#define FRAME_NS   ((uint64_t)NDS_FRAME_CYCLES * 1000000000ull / NDS_CLOCK)
#define STICK_DEAD 16384

struct NdsHost {
    NdsBoard* board;
    char* captures; /* the game's folder with its separator, whose captures/ a screenshot goes in */
    bool shot;      /* the screenshot key's: saved at the frame's end */
    int state_ask;  /* a save states' key (HOTKEY_SAVE_STATE...) for the loop between two frames, -1 none */
    int headless, gpu, paced;
    SnxPadMap pad;
    uint16_t keyboard;
    bool rumbling, rumble_said; /* the gamepads shake for the Rumble Pak's last move; one that could not, said */
    /* the pen: one finger, or the mouse's left button, the first that pressed the lower screen, until it lets go; its
       id and where it is, in the window's pixels */
    struct {
        bool down;
        u64 id;
        float x, y;
    } pen;
    uint64_t due;
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
static unsigned button_of(int bit) {
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

void nds_host_say(const char* line) { fprintf(stderr, "scarletnx-nds: %s\n", line); }

/* The pen's events. The touch setting, the Switch's, says which reach it: 0 none, 1 the fingers, 2 the fingers and the
   mouse. A press off the lower screen starts nothing, and a drag off it keeps to its edge (pen_read) */
static void pen_event(NdsHost* host, const SDL_Event* e) {
    float x, y;
    int fx, fy;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) host->pen.down = false; /* this window may not get the lift */
    if (!snx_vpad_window_point(e, &x, &y)) return;
    bool finger = e->type >= SDL_EVENT_FINGER_DOWN && e->type <= SDL_EVENT_FINGER_CANCELED;
    u64 id = finger ? snx_vpad_sdl_finger(&e->tfinger) : SNX_VPAD_MOUSE;
    switch (e->type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_FINGER_DOWN:
            if (host->pen.down || g_settings.touch_screen < (finger ? 1 : 2) ||
                (!finger && e->button.button != SDL_BUTTON_LEFT) || !nds_vk_present_point(x, y, &fx, &fy) ||
                fy < (int)NDS_SCREEN_HEIGHT)
                return;
            host->pen.down = true, host->pen.id = id;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (id == host->pen.id && (finger || e->button.button == SDL_BUTTON_LEFT)) host->pen.down = false;
            return;
        default:
            if (!host->pen.down || id != host->pen.id) return;
            break;
    }
    host->pen.x = x, host->pen.y = y;
}
static void pen_read(const NdsHost* host, NdsInput* input) {
    int x, y;
    if (!host->pen.down) return;
    (void)nds_vk_present_point(host->pen.x, host->pen.y, &x, &y);
    input->touching = 1;
    input->touch_x = (uint16_t)snx_clamp_int(x, 0, (int)NDS_SCREEN_WIDTH - 1);
    input->touch_y = (uint16_t)snx_clamp_int(y - (int)NDS_SCREEN_HEIGHT, 0, (int)NDS_SCREEN_HEIGHT - 1);
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

NdsHost* nds_host_open(NdsBoard* board, const char* title, const char* ini, const char* wav, int headless,
                       int fullscreen, char* error, size_t error_size) {
    NdsHost* host = calloc(1, sizeof *host);
    if (!host) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    host->board = board;
    host->state_ask = -1;
    {
        char* folder = ini ? snx_dirname(ini) : NULL;
        host->captures = snx_format("%s/", folder ? folder : ".");
        free(folder);
    }
    host->headless = headless;
    host->paced = !headless;
    if (!headless) snx_vpad_sdl_hints();
    if (!headless) snx_pad_background(g_settings.background_input);
    if (!headless && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        snprintf(error, error_size, "cannot start SDL's video: %s", SDL_GetError());
        free(host);
        return NULL;
    }
    char why[256] = "";
    if (nds_vk_init(title, headless != 0, why, sizeof why)) {
        if (nds_vk_render3d_init(why, sizeof why) && nds_vk_frame_init(why, sizeof why) &&
            nds_vk_present_init(why, sizeof why)) {
            host->gpu = 1;
            char* folder = ini ? snx_dirname(ini) : NULL; /* the game's, beside its save */
            nds_vk_pipeline_cache_open(folder);
            free(folder);
        } else {
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
        char said[256];
        if (!snx_window_fullscreen(v->host.window, fullscreen, said, sizeof said)) nds_host_say(said);
        /* each button where the console has it, the player's map over it and A and B swapped as the Switch's
           settings say */
        snx_pad_map_console(&host->pad, button_of, "DS", g_settings.button_map, g_settings.swap_a_b);
        snx_vpad_window_open(v->host.window, ini, nds_host_say, nds_vk_notice);
    }
    if (!headless) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)(NDS_SPU_RATE + 0.5)};
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO) ||
            !(host->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)) ||
            !SDL_ResumeAudioStreamDevice(host->audio)) {
            fprintf(stderr, "scarletnx-nds: no sound: %s\n", SDL_GetError());
            if (host->audio) SDL_DestroyAudioStream(host->audio);
            host->audio = NULL;
        } else if (!snx_audio_volume_open(host->audio, g_settings.volume, why, sizeof why)) /* the player's Volume */
            nds_host_say(why);
    }
    if (wav && (host->wav = fopen(wav, "wb"))) wav_header(host->wav, 0);
    host->due = SDL_GetTicksNS() + FRAME_NS;
    return host;
}

float nds_host_volume(const NdsHost* host) {
    return host && host->audio ? SDL_GetAudioDeviceGain(SDL_GetAudioStreamDevice(host->audio)) : -1;
}
void nds_host_close(NdsHost* host) {
    if (!host) return;
    free(host->captures);
    if (!host->headless) snx_vpad_window_close();
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

/* a runtime's key the window does: the screenshot's, at the frame's end; another said to do nothing, with why */
static void key_action(NdsHost* host, int action) {
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
    snx_vpad_window_none(action, action == HOTKEY_THREAD_DUMP ? "its ARM9 runs the game's code alone, with no threads"
                                 : action == HOTKEY_MOUSE
                                     ? "its mouse is the pen and the pointer of the on-screen controls"
                                 : action == HOTKEY_TV_MODE ? "the TV mode is the Switch's console docked"
                                 : action == HOTKEY_UPSCALER || action == HOTKEY_ANTI_ALIASING
                                     ? "its screens are drawn in sharp pixels, with no upscaler nor anti-aliasing"
                                     : "its window has none of it yet");
}
bool nds_host_poll(NdsHost* host, NdsInput* input) {
    bool open = true;
    if (!host->headless) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            int action;
            SnxVpadEvent took = snx_vpad_window_event(&event, &action);
            if (took == SNX_VPAD_QUIT) open = false;
            if (took == SNX_VPAD_ACTION) key_action(host, action);
            if (took != SNX_VPAD_PASS) continue;
            pen_event(host, &event);
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: open = false; break;
                case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(event.gdevice.which); break;
                case SDL_EVENT_KEY_DOWN: host->keyboard |= key_of(event.key.scancode); break;
                case SDL_EVENT_KEY_UP: host->keyboard &= (uint16_t)~key_of(event.key.scancode); break;
                default: break;
            }
        }
        input->keys = host->keyboard | gamepads(host);
        u64 pressed;
        i32 sticks[4]; /* the DS has none */
        snx_vpad_window_state(&pressed, sticks);
        for (int bit = 0; bit < SNX_PAD_BUTTONS; ++bit)
            if (pressed >> bit & 1) input->keys |= button_of(bit);
        pen_read(host, input);
        if (vpad_editing()) input->keys = 0, input->touching = 0;
        snx_vpad_window_showcase(input->keys, button_of, NULL);
    }
    return open;
}

static void shot(NdsHost* host);
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
    if (host->shot) shot(host);
    /* each move of the Rumble Pak's motor shakes the gamepads for two frames, which the next move renews */
    bool moved = nds_slot2_rumble(host->board);
    if (moved || host->rumbling) {
        char why[256];
        host->rumbling = moved;
        if (snx_pads_rumble(moved ? 1.0f : 0.0f, g_settings.vibration, 34, why, sizeof why) &&
            !host->rumble_said) {
            host->rumble_said = true;
            fprintf(stderr, "scarletnx-nds: %s\n", why);
        }
    }
    if (!host->paced) return;
    /* the frame's time by the host's clock, at fps_limit's pace (F4 switches it): a late frame does not hurry the ones
       after it past a frame's worth */
    uint64_t now = SDL_GetTicksNS(), step = snx_vpad_window_frame_ns(FRAME_NS);
    if (!step) return;
    if (now < host->due)
        SDL_DelayPrecise(host->due - now);
    else if (now - host->due > step * 4)
        host->due = now;
    host->due += step;
}

/* both screens as they show, as the Switch's screenshot is: a PPM in the game's captures/, said where */
static void shot(NdsHost* host) {
    char path[SNX_PATH_SIZE], line[SNX_PATH_SIZE + 64];
    host->shot = false;
    snx_capture_path(path, sizeof path - 4, host->captures ? host->captures : "./");
    strcat(path, ".ppm");
    snprintf(line, sizeof line,
             nds_host_shot(host, path) ? "the screenshot could not be written to %s" : "screenshot: %s", path);
    nds_host_say(line);
}
bool nds_host_held(NdsHost* host) {
    if (!snx_vpad_window_paused()) return false;
    if (nds_vk()) nds_vk_present();
    if (host->shot) shot(host);
    uint64_t now = SDL_GetTicksNS();
    if (now < host->due) SDL_DelayPrecise(host->due - now);
    host->due = SDL_GetTicksNS() + FRAME_NS;
    return true;
}
int nds_host_state_ask(NdsHost* host) {
    int ask = host->state_ask;
    host->state_ask = -1;
    return ask;
}
bool nds_host_thumb(NdsHost* host, uint8_t* thumb) {
    unsigned char* rgba = host->gpu ? malloc(NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2 * 4) : NULL;
    bool read = rgba && nds_vk_frame_read(rgba);
    if (read) snx_state_thumb(rgba, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT * 2, NDS_SCREEN_WIDTH * 4, thumb);
    free(rgba);
    return read;
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
int nds_host_lost(const NdsHost* host, char* why, size_t why_size) {
    const NdsVk* v = host->gpu ? nds_vk() : NULL;
    if (!v || v->ring.lost == VK_SUCCESS) return 0;
    snprintf(why, why_size, "the GPU stopped answering (%s): no frame is drawn after it",
             snx_vk_result_str(v->ring.lost));
    return 1;
}
