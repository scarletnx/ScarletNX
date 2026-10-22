/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SDL3 backend, the window backend on Linux, Windows and Android: the window and its Vulkan surface, keyboard, gamepads
   and touch. Runs natively on Wayland and on X11 (SDL_VIDEO_DRIVER=x11 picks X11), and takes the unattended-run
   options (seconds, frames, shot, shot_every, press_len, script: platform/unattended.c). */
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/pause.h"
#include "nx/runtime/state.h"
#include "nx/hle/hle.h" /* g_hle.hid: the players and the touch screen hid samples */
#include "nx/hle/services/hid.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "nx/runtime/platform/touch.h"
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "nx/runtime/platform/ui.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/gamepad.h"
#include "common/utilities/pad_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/renderer.h"
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL.h>
#include <time.h>
#include "common/utilities/common.h"
#include "common/utilities/window_icon.h"
#include "nx/runtime/platform/controls_png.h"
#include "common/utilities/build_info.h"
#ifdef __ANDROID__
#include <jni.h>
#endif

static SDL_Window* g_win;
/* the window's size in pixels, width | height << 32: the renderer reads it on its own thread, and one atomic keeps
   a width from one resize from pairing with the height of another */
static u64 g_wsize = 1280 | (720ull << 32); /* atomic */
static u64 keys_held;
static u64 g_menu_held;      /* keyboard, scripted and on-screen buttons held while the menu had them */
static u64 g_menu_held_pad0; /* No1's physical pad, tracked separately so a replacement has fresh presses */
static u64 g_menu_held_extra[SNX_GAMEPADS - 1];
static int g_fullscreen;
static void set_fullscreen(int on);
static int g_shot_req;
static int g_audio_ready;
static int g_hidden;      /* the app is in the background (Android) */
static int g_given_up;    /* the window went to a fatal error's screen: the renderer presents nothing more */
static char g_title[256]; /* the game's name, for that screen */
/* The press that skips main.c's prepare screen (backend_pressed), taken while that screen asks for one: from its
   first call until the frontend's first pass once the game's first thread runs (frontend_frame), before the game
   has drawn. It is that screen's alone: the press doesn't reach the menu or the game, nor do the moves and the lift
   of a finger that pressed, and a pad's buttons held then reach the game only once let go. A pad's buttons and
   triggers count from up to down only, so one held as the pad came (from the launcher) doesn't skip when SDL knew it
   held as the screen began to listen (press_pad_held) */
enum { PRESS_NONE, PRESS_KEY, PRESS_TOUCH, PRESS_BUTTON, PRESS_TRIGGER };
static struct {
    int listening;
    int kind, code;      /* the press since the last call: PRESS_*, and the key's scancode or the pad's button */
    SDL_TouchID touch;   /* a finger that pressed, by its device and id, whose moves and lift are the screen's too */
    SDL_FingerID finger; /* (0 for none) */
    struct {
        SDL_JoystickID id; /* 0 for a free slot */
        u32 held;          /* its buttons down (bit by SDL_GamepadButton), and its triggers past half (bits 30, 31) */
    } pad[SNX_GAMEPADS];
    char said[64];
} g_press;
static SDL_Haptic* g_buzzer; /* the phone's vibrator, for the on-screen controls' taps (Android) */
static u32 g_generation;     /* the window's surface came back this many times */

struct PAudio {
    SDL_AudioStream* stream;
    int logged_data, logged_nonzero;
};

PAudio* plat_audio_open(u32 rate, u32 channels) {
    if (g_opt_headless || !g_audio_ready) return NULL;
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, (int)channels, (int)rate};
    /* Keep the logical device tied to the system default on every SDL audio backend. */
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        WARN("audio: SDL could not open the default output: %s\n", SDL_GetError());
        return NULL;
    }
    PAudio* a = calloc(1, sizeof *a);
    if (!a) {
        SDL_DestroyAudioStream(stream);
        return NULL;
    }
    a->stream = stream;
    const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream));
    LOG("audio: SDL %s output opened on %s (%u Hz, %u channels, signed 16-bit PCM)\n", SDL_GetCurrentAudioDriver(),
        name ? name : "the system default", rate, channels);
    return a;
}

void plat_audio_close(PAudio* a) {
    if (!a) return;
    SDL_DestroyAudioStream(a->stream); /* OpenAudioDeviceStream also closes its device here. */
    free(a);
}

int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    if (!a || !bytes) return 0;
    /* SDL's length is signed. Split only at whole stereo frames, without truncating a large guest buffer. */
    const u8* p = samples;
    u32 left = bytes;
    while (left) {
        u32 n = left > 0x7FFFFFFCu ? 0x7FFFFFFCu : left;
        if (!SDL_PutAudioStreamData(a->stream, p, (int)n)) {
            WARN_LIMIT(4, "audio: SDL could not queue PCM: %s\n", SDL_GetError());
            return 0;
        }
        p += n;
        left -= n;
    }
    if (!a->logged_data) {
        a->logged_data = 1;
        LOG("audio: queued %u bytes of guest PCM\n", bytes);
    }
    if (!a->logged_nonzero) {
        u32 peak = 0;
        u32 inspect = bytes < 65536 ? bytes : 65536;
        for (u32 i = 0; i + 1 < inspect; i += 2) {
            i16 sample = (i16)snx_ld_le16((const u8*)samples + i);
            u32 magnitude = sample < 0 ? (u32) - (i32)sample : (u32)sample;
            if (magnitude > peak) peak = magnitude;
        }
        if (peak) {
            a->logged_nonzero = 1;
            LOG("audio: queued nonzero guest PCM (s16 peak %u)\n", peak);
        }
    }
    return 1;
}

void plat_audio_start(PAudio* a, int started) {
    if (!a) return;
    bool ok = started ? SDL_ResumeAudioStreamDevice(a->stream) : SDL_PauseAudioStreamDevice(a->stream);
    if (!ok) WARN_LIMIT(4, "audio: SDL could not %s output: %s\n", started ? "start" : "pause", SDL_GetError());
}

void plat_audio_clear(PAudio* a) {
    if (a && !SDL_ClearAudioStream(a->stream)) WARN_LIMIT(4, "audio: SDL could not clear PCM: %s\n", SDL_GetError());
}

u32 plat_audio_queued(PAudio* a) {
    int n = a ? SDL_GetAudioStreamQueued(a->stream) : 0;
    return n > 0 ? (u32)n : 0;
}

void plat_audio_volume(PAudio* a, float volume) {
    if (a && !SDL_SetAudioStreamGain(a->stream, volume))
        WARN_LIMIT(4, "audio: SDL could not set output volume: %s\n", SDL_GetError());
}

/* the game's touch screen is g_settings.touch_screen: 0 nothing, 1 the fingers, 2 the fingers and the mouse's left
   button (ini key touch) */
static u64 g_touch_ids[SNX_HID_TOUCHES]; /* the finger in each slot of g_hle.hid.touch */
/* a finger (or the mouse) down, moved or up on the game's touch screen: under input_lock, which hid's update reads
   the touch screen under (hid_take_input) */
enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };
static void touch_set(int how, u64 id, const int picture[4], float px, float py) {
    pm_lock(&g_host.hid.input_lock);
    if (how == TOUCH_DOWN)
        touch_down(&g_hle.hid.touch, g_touch_ids, id, picture, px, py);
    else if (how == TOUCH_MOVE)
        touch_move(&g_hle.hid.touch, g_touch_ids, id, picture, px, py);
    else
        touch_up(&g_hle.hid.touch, g_touch_ids, id);
    pm_unlock(&g_host.hid.input_lock);
}

/* the touch screen's setting changed (the .ini's setter, on the main thread): the fingers or the mouse it no longer
   takes are lifted by the next video_poll, under the input_lock it holds already. Not here: the in-game menu sets it
   from inside video_poll (overlay_controls), where taking the lock again would hang the thread */
static int g_touch_lift;
void backend_touch_mode_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    g_touch_lift = 1;
}
static void touch_lift(void) { /* under input_lock */
    if (!g_touch_lift) return;
    g_touch_lift = 0;
    if (!g_settings.touch_screen) g_hle.hid.touch.down = 0;
    if (g_settings.touch_screen != 2) touch_up(&g_hle.hid.touch, g_touch_ids, SNX_TOUCH_MOUSE);
}
/* from the in-game menu, under ui_lock: the mode as the .ini clamps it, saved */
void backend_touch_set(int mode) {
    const IniOpt* o = ini_find(g_backend_ini, "touch");
    char v[16];
    snprintf(v, sizeof v, "%d", mode);
    ini_read(o, v);
    ini_format(o, v, sizeof v);
    ini_set("touch", v);
}
int backend_ini(const char* k, const char* v) {
    const IniOpt* o = ini_find(g_backend_ini, k);
    int known;
    /* the in-game menu reads the touch mode, and the renderer the on-screen controls, under ui_lock */
    ui_lock();
    if (o) ini_read(o, v);
    known = o || vpad_ini(k, v) || gamepad_ini(k, v);
    ui_unlock();
    return known || unattended_ini(k, v);
}

static void wsize_set(int w, int h) {
    if (w > 0 && h > 0) __atomic_store_n(&g_wsize, (u64)(u32)w | (u64)(u32)h << 32, __ATOMIC_RELAXED);
}
/* the window's part clear of the system's bars, in pixels, x | y << 16 | w << 32 | h << 48, or 0 for all of it: set
   on the main thread with the size, read by the renderer on its own */
static u64 g_wsafe;
static void wsafe_set(void) {
    SDL_Rect r;
    float d = g_win ? SDL_GetWindowPixelDensity(g_win) : 0;
    u64 v = 0;
    if (!g_fullscreen && d > 0 && SDL_GetWindowSafeArea(g_win, &r))
        v = (u64)(u16)(r.x * d) | (u64)(u16)(r.y * d) << 16 | (u64)(u16)(r.w * d) << 32 | (u64)(u16)(r.h * d) << 48;
    __atomic_store_n(&g_wsafe, v, __ATOMIC_RELEASE);
}
void backend_window_safe(int* x, int* y, int* w, int* h) {
    u64 v = __atomic_load_n(&g_wsafe, __ATOMIC_ACQUIRE);
    *x = (int)(u16)v;
    *y = (int)(u16)(v >> 16);
    *w = (int)(u16)(v >> 32);
    *h = (int)(u16)(v >> 48);
    if (!*w || !*h) {
        *x = *y = 0;
        backend_window_size(w, h);
    }
}
/* as SDL queues each event, from its own thread: on Android the main loop is held while the app is in the
   background, and presenting must stop before that */
static bool SDLCALL lifecycle(void* unused, SDL_Event* e) {
    (void)unused;
    /* what the system did to the app, in the log: a GPU lost after a minute with nobody touching the screen was the
       system locking it (the app goes to the background and loses the GPU), and a low memory event comes before a kill */
    switch (e->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND: LOG("lifecycle: the app will enter the background\n"); break;
        case SDL_EVENT_DID_ENTER_BACKGROUND: LOG("lifecycle: the app is in the background\n"); break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND: LOG("lifecycle: the app will enter the foreground\n"); break;
        case SDL_EVENT_DID_ENTER_FOREGROUND: LOG("lifecycle: the app is in the foreground\n"); break;
        case SDL_EVENT_LOW_MEMORY: LOG("lifecycle: the system warns of low memory\n"); break;
        case SDL_EVENT_TERMINATING: LOG("lifecycle: the system is ending the app\n"); break;
        default: break;
    }
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) __atomic_store_n(&g_hidden, 1, __ATOMIC_RELEASE);
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
#ifndef SDL_PLATFORM_IOS
        /* Android makes the window's surface anew when the app comes back. iOS keeps it: making another from the
           renderer's thread is UIKit work off the main thread, which aborts the app */
        __atomic_add_fetch(&g_generation, 1, __ATOMIC_RELEASE);
#endif
        __atomic_store_n(&g_hidden, 0, __ATOMIC_RELEASE);
    }
    return true;
}
/* minimized or covered: a compositor shows nothing of it, and may never hand its swapchain's images back (Wayland
   with FIFO), so presenting stops until it is back (the main thread sets it, the renderer reads it) */
static int g_minimized;
int backend_window_shown(void) {
    return !g_opt_headless && !__atomic_load_n(&g_hidden, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&g_minimized, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_given_up, __ATOMIC_ACQUIRE);
}
u32 backend_window_generation(void) { return __atomic_load_n(&g_generation, __ATOMIC_ACQUIRE); }
void backend_window_size(int* w, int* h) {
    u64 s = g_opt_headless ? 0 : __atomic_load_n(&g_wsize, __ATOMIC_RELAXED);
    *w = (int)(u32)s;
    *h = (int)(u32)(s >> 32);
}

/* the on-screen controls' sprites, decoded from the PNG compiled in, to the atlas the renderer uploads */
static void controls_atlas(void) {
    SDL_Surface* png = SDL_LoadPNG_IO(SDL_IOFromConstMem(snx_controls_png, snx_controls_png_size), true);
    SDL_Surface* rgba = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32) : NULL;
    u32* px = rgba ? malloc((size_t)rgba->w * (size_t)rgba->h * 4) : NULL;
    if (px) {
        for (int y = 0; y < rgba->h; y++)
            memcpy(px + (size_t)y * (size_t)rgba->w, (const u8*)rgba->pixels + (size_t)y * (size_t)rgba->pitch,
                   (size_t)rgba->w * 4);
        ui_atlas_set(px, rgba->w, rgba->h);
    } else
        WARN("input: the on-screen controls' image could not be read: %s\n", SDL_GetError());
    free(px);
    SDL_DestroySurface(rgba);
    SDL_DestroySurface(png);
}
/* Android's vibrator, which SDL lists as VIBRATOR_SERVICE; a gamepad's motors are no buzz for a finger */
static void buzzer_open(void) {
#ifdef __ANDROID__
    int n = 0;
    SDL_HapticID* ids;
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) return;
    ids = SDL_GetHaptics(&n);
    for (int i = 0; ids && i < n && !g_buzzer; i++) {
        const char* name = SDL_GetHapticNameForID(ids[i]);
        if (name && !strcmp(name, "VIBRATOR_SERVICE")) g_buzzer = SDL_OpenHaptic(ids[i]);
    }
    SDL_free(ids);
    if (g_buzzer && !SDL_InitHapticRumble(g_buzzer)) {
        SDL_CloseHaptic(g_buzzer);
        g_buzzer = NULL;
    }
#endif
}

void video_init(const char* title, const char* addons) {
    if (g_opt_headless) {
        /* No SDL video subsystem: this also works without DISPLAY, WAYLAND_DISPLAY or a seat. */
        unattended_start();
        if (!g_renderer->start()) fatal("Headless Vulkan could not start (see the log). Vulkan 1.3 is required.");
        LOG("renderer: Vulkan headless (no window, surface or swapchain)\n");
        return;
    }
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); /* back opens the game's menu (overlay.c), not leaves */
#endif
    /* a finger and the mouse each reach the touch screen once: SDL would also send each as the other, and a pen as both
       (it stays a finger) */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
#ifdef __APPLE__
    { /* the apps carry their MoltenVK (scripts/apple-app.sh), in the bundle's Frameworks, which SDL does not search:
         macOS's beside Resources/ (SDL's base folder), iOS's in the bundle itself. Elsewhere the system's loader */
        const char* base = SDL_GetBasePath();
        const char* const own[] = {"../Frameworks/libMoltenVK.dylib", "Frameworks/MoltenVK.framework/MoltenVK"};
        size_t i;
        for (i = 0; base && !SDL_GetHint(SDL_HINT_VULKAN_LIBRARY) && i < sizeof own / sizeof *own; i++) {
            char* path = snx_format("%s%s", base, own[i]);
            if (snx_is_file(path)) SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, path);
            free(path);
        }
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) fatal("SDL could not start its video: %s", SDL_GetError());
    gamepad_scan();
    /* SDL subsystem initialization belongs to the main thread, before the guest can open audout sessions. */
    g_audio_ready = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!g_audio_ready) WARN("audio: SDL could not start its audio: %s\n", SDL_GetError());
    LOG("video: SDL %d.%d.%d on %s\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
    unattended_start();
    SDL_AddEventWatch(lifecycle, NULL);
    /* a game runs for minutes with nobody touching the screen: a phone that locks it takes the GPU from the app */
    SDL_DisableScreenSaver();
    int w, h;
    char header[768];
    snprintf(g_title, sizeof g_title, "%s", title);
    backend_window_size(&w, &h);
    if (addons[0])
        snprintf(header, sizeof header, "%s (%s) - %s", title, addons, snx_build_info());
    else
        snprintf(header, sizeof header, "%s - %s", title, snx_build_info());
    g_win = SDL_CreateWindow(header, w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_win) fatal("SDL could not open a Vulkan window: %s", SDL_GetError());
    snx_window_icon(g_win);
    overlay_set_display_scale(SDL_GetWindowDisplayScale(g_win));
    controls_atlas();
    vpad_set_store(ini_set);
    buzzer_open();
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    wsize_set(w, h);
    wsafe_set();
    if (!g_renderer->start())
        fatal("Vulkan could not start (see the log). ScarletNX needs a GPU and driver with Vulkan 1.3.");
    LOG("renderer: Vulkan\n");
    /* not for an unattended run: nobody is at the screen, and it would cover the desktop of whoever is */
    if (g_settings.fullscreen && !unattended_active()) set_fullscreen(1);
#ifdef _WIN32
    /* keep the display and the system awake while playing (it was set with the crash handler, headless runs too) */
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
}

void* backend_vk_loader(void) { return (void*)SDL_Vulkan_GetVkGetInstanceProcAddr(); }
const char* const* backend_vk_extensions(u32* n) {
    Uint32 c = 0;
    const char* const* e = SDL_Vulkan_GetInstanceExtensions(&c);
    *n = c;
    return e;
}
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface) {
    if (SDL_Vulkan_CreateSurface(g_win, instance, NULL, surface)) return 1;
    LOG("video: no Vulkan surface: %s\n", SDL_GetError());
    return 0;
}
/* F1's text. It follows keymap (the game's buttons), hotkey and menu_event (the runtime's keys): a binding changed there
   changes here */
static const char* HELP =
    "Controller: A, B, X and Y sit where the console has them (gamepad_swap_ab=1 swaps A and B). "
    "Home opens the game's menu.\n\n"
    "Keyboard, by key position on a US layout:\n"
    "  Buttons      A: X or Space    B: Z    X: C    Y: V\n"
    "  Shoulders    L: Q    R: E    ZL: 1    ZR: 3\n"
    "  Plus, minus  +: Enter    -: Backspace\n"
    "  D-Pad        the arrow keys\n"
    "  Left stick   W, A, S and D\n"
    "  Right stick  I, J, K and L\n\n"
    "Runtime keys:\n"
    "  F1  this help\n"
    "  F2  open or close the game's menu (arrows move, Enter or Space picks, Esc or Backspace goes back)\n"
    "  F3  take a screenshot\n"
    "  F4  turn the FPS limit off or back on\n"
    "  F5  write every thread's state to the log\n"
    "  F6  turn showcase mode on or off\n"
    "  F7  save the game's state to the selected slot\n"
    "  F8  load the selected slot's state\n"
    "  F9  select the next save state slot\n"
#if defined(SNX_TRACY)
    "  F10 record a 60 second Tracy capture\n"
#endif
    "  Pause  pause the whole game, and resume it\n"
    "  F11 or Alt+Enter  switch fullscreen\n"
    "  Esc  quit the game";

static PadKind gamepad_kind(int type) {
    switch (type) {
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return SNX_PAD_JOY_LEFT;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return SNX_PAD_JOY_RIGHT;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return SNX_PAD_JOY_DUAL;
        default: return SNX_PAD_FULL_KEY;
    }
}

/* keys -> HidNpadButton bits; 40..47 are the stick keys, turned into axes in video_poll. By scancode, the key's
   position: the keycode depends on the layout and on the modifiers, so Shift pressed while 1 was held made its release
   a different key, and ZL stayed held */
static u64 keymap(SDL_Scancode k) {
    switch (k) {
        case SDL_SCANCODE_X:
        case SDL_SCANCODE_SPACE: return 1ull << 0; /* A */
        case SDL_SCANCODE_Z: return 1ull << 1;     /* B */
        case SDL_SCANCODE_C: return 1ull << 2;     /* X */
        case SDL_SCANCODE_V: return 1ull << 3;     /* Y */
        case SDL_SCANCODE_Q: return 1ull << 6;     /* L */
        case SDL_SCANCODE_E: return 1ull << 7;     /* R */
        case SDL_SCANCODE_1: return 1ull << 8;     /* ZL */
        case SDL_SCANCODE_3: return 1ull << 9;     /* ZR */
        case SDL_SCANCODE_RETURN: return 1ull << 10;
        case SDL_SCANCODE_BACKSPACE: return 1ull << 11;
        case SDL_SCANCODE_LEFT: return 1ull << 12;
        case SDL_SCANCODE_UP: return 1ull << 13;
        case SDL_SCANCODE_RIGHT: return 1ull << 14;
        case SDL_SCANCODE_DOWN: return 1ull << 15;
        case SDL_SCANCODE_A: return 1ull << 40;
        case SDL_SCANCODE_W: return 1ull << 41;
        case SDL_SCANCODE_D: return 1ull << 42;
        case SDL_SCANCODE_S: return 1ull << 43;
        case SDL_SCANCODE_J: return 1ull << 44;
        case SDL_SCANCODE_I: return 1ull << 45;
        case SDL_SCANCODE_L: return 1ull << 46;
        case SDL_SCANCODE_K: return 1ull << 47;
        default: break;
    }
    return 0;
}

/* g_fullscreen follows the window: also set from the fullscreen events, for a change the window manager made */
static void set_fullscreen(int on) {
    if (SDL_SetWindowFullscreen(g_win, on))
        g_fullscreen = on;
    else
        LOG("video: fullscreen %s failed: %s\n", on ? "on" : "off", SDL_GetError());
}
/* ---- a fatal error's box (plat.c's fatal): the game's menu panel (overlay.c) with the error, what it means and what
   to do, until the player closes it. The game's renderer lets go of the window first (its device may be what was
   lost), and an SDL renderer of its own draws the panel's quads from the same atlas ---- */
static char* g_fatal_left; /* an error another thread left for the main thread to show */
static int g_fatal_taken;  /* the main thread is showing it */

/* ui.h's quads, as the Vulkan renderer draws them (gpu/vk/ui.c): the atlas's premultiplied texels times their colour */
static void ui_draw_sdl(SDL_Renderer* r, SDL_Texture* atlas, int aw, int ah, const SnxUiList* l) {
    static const int corners[6] = {0, 1, 2, 0, 2, 3};
    int i;
    for (i = 0; i < l->n; ++i) {
        const SnxUiQuad* q = &l->q[i];
        float a = (float)(q->rgba >> 24) / 255.0f;
        SDL_FColor c = {(float)(q->rgba & 0xFF) / 255.0f * a, (float)(q->rgba >> 8 & 0xFF) / 255.0f * a,
                        (float)(q->rgba >> 16 & 0xFF) / 255.0f * a, a};
        float u0 = (float)q->u / (float)aw, v0 = (float)q->v / (float)ah;
        float u1 = (float)(q->u + q->uw) / (float)aw, v1 = (float)(q->v + q->vh) / (float)ah;
        SDL_Vertex v[4] = {{{q->x, q->y}, c, {u0, v0}},
                           {{q->x + q->w, q->y}, c, {u1, v0}},
                           {{q->x + q->w, q->y + q->h}, c, {u1, v1}},
                           {{q->x, q->y + q->h}, c, {u0, v1}}};
        SDL_RenderGeometry(r, atlas, v, 4, corners, 6);
    }
}
/* 0 when no box could be shown (no window, the renderer didn't let go, no renderer or atlas here) */
#ifdef __ANDROID__
/* the activity's share(path) (GameActivity.java): the file, to the app the player picks */
static int android_share(const char* path) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    jclass type;
    jmethodID share;
    jstring text;
    int ok;
    if (!env || !activity) return 0;
    type = (*env)->GetObjectClass(env, activity);
    share = (*env)->GetMethodID(env, type, "share", "(Ljava/lang/String;)V");
    text = share ? (*env)->NewStringUTF(env, path) : NULL;
    if (text) (*env)->CallVoidMethod(env, activity, share, text);
    ok = text && !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (text) (*env)->DeleteLocalRef(env, text);
    (*env)->DeleteLocalRef(env, type);
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}
#endif
/* Share Log, as the launcher's: scarletnx.log beside the game's log, sent to the app the player picks on Android; its
   path in saved once written (else left as it was), what it did in said */
static void fatal_share(const char* msg, const char* game_log, char* saved, size_t saved_size, char* said,
                        size_t size) {
    char made[1300], *folder = snx_dirname(game_log);
    int ok = folder && !fatal_share_log(folder, snx_build_info(), msg, game_log, made, sizeof made);
    if (!ok) snprintf(said, size, "The log could not be saved: %s", folder ? made : game_log);
#ifdef __ANDROID__
    else if (!android_share(made))
        snprintf(said, size, "Log saved to %s, but nothing took it to share.", made);
#endif
    else
        snprintf(said, size, "Log saved to %s.", made);
    if (ok) snprintf(saved, saved_size, "%s", made);
    free(folder);
}
/* Copy path: the log Share Log saved, else the game's own, to paste where it is asked for */
static void fatal_copy(const char* path, char* said, size_t size) {
    if (SDL_SetClipboardText(path))
        snprintf(said, size, "Path copied: %s", path);
    else
        snprintf(said, size, "The path could not be copied: %s", SDL_GetError());
}
enum { FATAL_LINK = SNX_OVERLAY_ERROR_ITEMS, FATAL_OUT }; /* past the items: the log's link, and the rest */
/* what a press at x, y is on: an item, the link or the rest */
static int fatal_at(const SnxOverlayError* panel, float x, float y) {
    int i;
    if (overlay_error_hit(&panel->link, x, y)) return FATAL_LINK;
    for (i = 0; i < SNX_OVERLAY_ERROR_ITEMS; ++i)
        if (overlay_error_hit(&panel->items[i], x, y)) return i;
    return FATAL_OUT;
}
static int fatal_screen(const char* msg) {
#ifdef __ANDROID__
    static const char* const how = "Share Log sends the game's log, which has what ran last.";
#else
    static const char* const how = "The game's log has what ran last: ";
    char log_url[3608];
#endif
    static SnxUiQuad quads[4096]; /* as many as the renderer's list: a long message's rows on a wide window */
    SnxUiList l = {quads, 0, 4096};
    char name[256], what[1400], log_path[1200], said[1400], saved[1300] = "", *full;
    SnxOverlayError panel;
    SDL_FingerID finger = 0;
    int pointer = 0, pressed = FATAL_OUT, button = -1; /* pointer: 1 the left button, 2 a finger, 3 another button */
    SDL_Renderer* r;
    SDL_Texture* atlas = NULL;
    const u32* px;
    int aw = 0, ah = 0, done = 0, i;
    u32 version;
    Uint64 since;
    if (!g_win) return 0;
    __atomic_store_n(&g_given_up, 1, __ATOMIC_RELEASE);
    if (g_renderer_up && g_renderer->release_window && !g_renderer->release_window()) return 0;
    /* Vulkan first: the window was made for it */
    r = SDL_CreateRenderer(g_win, "vulkan");
    if (!r) r = SDL_CreateRenderer(g_win, NULL);
    if (!r) {
        LOG("video: no renderer for the error's box: %s\n", SDL_GetError());
        return 0;
    }
    controls_atlas(); /* the renderer freed its copy once it had it */
    ui_lock();
    px = ui_atlas(&aw, &ah, &version);
    if (px) {
        SDL_Surface* image = SDL_CreateSurfaceFrom(aw, ah, SDL_PIXELFORMAT_RGBA32, (void*)px, aw * 4);
        if (image) atlas = SDL_CreateTextureFromSurface(r, image);
        SDL_DestroySurface(image);
    }
    ui_unlock();
    if (!atlas) {
        LOG("video: no atlas for the error's box: %s\n", SDL_GetError());
        SDL_DestroyRenderer(r);
        return 0;
    }
    SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(atlas, SDL_SCALEMODE_NEAREST);
    for (i = 0; g_title[i] && i < (int)sizeof name - 1; ++i)
        name[i] = g_title[i] >= 'a' && g_title[i] <= 'z' ? (char)(g_title[i] - 32) : g_title[i];
    name[i] = 0;
    memset(&panel, 0, sizeof panel);
    panel.name = name;
    panel.msg = msg;
    panel.how = what;
    panel.focus = SNX_OVERLAY_ERROR_CLOSE;
    /* the log plat.c writes, in the state folder. Beside the game that is argv[0]'s folder, which may be relative (./
       for ./MyGame): Share Log and a file URI need an absolute path */
    snprintf(log_path, sizeof log_path, "%s%s.log", g_statedir, g_exename);
    full = snx_absolute(log_path);
    if (full && strlen(full) < sizeof log_path) memcpy(log_path, full, strlen(full) + 1);
    free(full);
#ifdef __ANDROID__
    snprintf(what, sizeof what, "%s", how);
#else
    /* a link only to a log that is there: plat.c makes it once the game's code has loaded, so an error before that has
       none, and the path stays plain text */
    if (snx_is_file(log_path) && fatal_log_url(log_path, log_url, sizeof log_url)) {
        snprintf(what, sizeof what, "%s", how);
        panel.log = log_path;
    } else
        snprintf(what, sizeof what, "%s%s", how, log_path);
#endif
    since = SDL_GetTicks();
    while (!done) {
        SDL_Event e;
        int w = 0, h = 0, item = -1, at;
        float x, y;
        SDL_GetRenderOutputSize(r, &w, &h);
        l.n = 0;
        overlay_error_draw(&l, w, h, &panel);
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: done = 1; break;
                /* a press made on this box: not a key held from the game, nor one that came as the error did. Up and
                   Down move between the items, Enter and Space do the one in focus, any other key closes */
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.repeat || SDL_GetTicks() - since <= 500) break;
                    if (e.key.key == SDLK_UP)
                        panel.focus = (panel.focus + SNX_OVERLAY_ERROR_ITEMS - 1) % SNX_OVERLAY_ERROR_ITEMS;
                    else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_TAB)
                        panel.focus = (panel.focus + 1) % SNX_OVERLAY_ERROR_ITEMS;
                    else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE)
                        item = panel.focus;
                    else
                        done = 1;
                    break;
                /* a gamepad's the same: the D-pad moves, A does the item in focus, any other button closes */
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: button = e.gbutton.button; break;
                case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    if (e.gbutton.button != button) break;
                    button = -1;
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP)
                        panel.focus = (panel.focus + SNX_OVERLAY_ERROR_ITEMS - 1) % SNX_OVERLAY_ERROR_ITEMS;
                    else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                        panel.focus = (panel.focus + 1) % SNX_OVERLAY_ERROR_ITEMS;
                    else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
                        item = panel.focus;
                    else
                        done = 1;
                    break;
                /* a click or a tap does what it is let go on, if it went down there: an item, the log's link (which
                   opens it), anywhere else closes. Any other mouse button closes, as before there were items */
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (e.button.which == SDL_TOUCH_MOUSEID || pointer) break;
                    pointer = e.button.button == SDL_BUTTON_LEFT ? 1 : 3;
                    pressed = SDL_RenderCoordinatesFromWindow(r, e.button.x, e.button.y, &x, &y)
                                  ? fatal_at(&panel, x, y)
                                  : FATAL_OUT;
                    break;
                case SDL_EVENT_FINGER_DOWN:
                    if (pointer) break;
                    pointer = 2;
                    finger = e.tfinger.fingerID;
                    pressed = fatal_at(&panel, e.tfinger.x * (float)w, e.tfinger.y * (float)h);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                case SDL_EVENT_FINGER_UP:
                    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                        if ((pointer != 1 && pointer != 3) || e.button.which == SDL_TOUCH_MOUSEID) break;
                        at = pointer == 3 ? FATAL_OUT
                             : SDL_RenderCoordinatesFromWindow(r, e.button.x, e.button.y, &x, &y)
                                 ? fatal_at(&panel, x, y)
                                 : FATAL_OUT;
                    } else {
                        if (pointer != 2 || e.tfinger.fingerID != finger) break;
                        at = fatal_at(&panel, e.tfinger.x * (float)w, e.tfinger.y * (float)h);
                    }
                    if (pointer == 3 || (at == pressed && at == FATAL_OUT))
                        done = 1;
                    else if (at == pressed && at == FATAL_LINK) {
#ifndef __ANDROID__
                        if (!SDL_OpenURL(log_url)) LOG("video: could not open the log: %s\n", SDL_GetError());
#endif
                    } else if (at == pressed)
                        item = at;
                    pointer = 0;
                    pressed = FATAL_OUT;
                    break;
                case SDL_EVENT_FINGER_CANCELED:
                    if (pointer == 2 && e.tfinger.fingerID == finger) {
                        pointer = 0;
                        pressed = FATAL_OUT;
                    }
                    break;
            }
            if (item == SNX_OVERLAY_ERROR_CLOSE) done = 1;
            if (item == SNX_OVERLAY_ERROR_SHARE || item == SNX_OVERLAY_ERROR_COPY) {
                panel.focus = item;
                if (item == SNX_OVERLAY_ERROR_SHARE)
                    fatal_share(msg, log_path, saved, sizeof saved, said, sizeof said);
                else
                    fatal_copy(saved[0] ? saved : log_path, said, sizeof said);
                panel.said = said;
            }
            item = -1;
        }
        /* where the game's picture was: the device that drew it may be gone */
        SDL_SetRenderDrawColor(r, 0x14, 0x14, 0x16, 255);
        SDL_RenderClear(r);
        ui_draw_sdl(r, atlas, aw, ah, &l);
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
    SDL_DestroyTexture(atlas);
    SDL_DestroyRenderer(r);
    return 1;
}
/* plat.c's fatal error: the screen above on the main thread, which owns the window; SDL's box when it can't be drawn.
   From another thread, the main thread's loop (video_poll) shows it, and this thread waits for the exit; when the main
   thread doesn't come (held up elsewhere), Windows' own box (it takes any thread) and none on Linux: the error is in
   the log and on stderr. Nothing for an unattended run, where nobody would close it */
void win_fatal_box(const char* msg) {
#ifdef SDL_PLATFORM_IOS
    /* a phone shows no stderr and no log of the app's: the Files app shows this file, in the app's Documents */
    {
        FILE* file;
        mkdir("logs", 0755);
        file = fopen("logs/scarletnx-error.log", "a");
        if (file) {
            fprintf(file, "%s\n", msg);
            fclose(file);
        }
    }
#endif
    if (g_opt_headless || unattended_active()) return;
    if (SDL_IsMainThread()) {
        /* an error another thread left first: this one followed from it */
        char* left = __atomic_load_n(&g_fatal_left, __ATOMIC_ACQUIRE);
        __atomic_store_n(&g_fatal_taken, 1, __ATOMIC_RELEASE);
        if (!fatal_screen(left ? left : msg))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", left ? left : msg, g_win);
        return;
    }
    if (g_win) {
        char* none = NULL;
        char* copy = SDL_strdup(msg);
        int i;
        /* the renderer stops presenting and lets go of the window here: the main thread may be waiting for it */
        __atomic_store_n(&g_given_up, 1, __ATOMIC_RELEASE);
        if (g_renderer_up && g_renderer->release_window) g_renderer->release_window();
        if (!copy || !__atomic_compare_exchange_n(&g_fatal_left, &none, copy, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            SDL_free(copy); /* a second error: the first one shows */
        for (i = 0; i < 300; ++i) {
            if (__atomic_load_n(&g_fatal_taken, __ATOMIC_ACQUIRE))
                for (;;) SDL_Delay(1000); /* the main thread shows it and ends the process */
            SDL_Delay(10);
        }
    }
#ifdef _WIN32
    wchar_t w[4096]; /* on the stack: this also runs in the crash report, where the heap may be what broke */
    if (MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 4096))
        MessageBoxW(NULL, w, L"ScarletNX", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    else
        MessageBoxA(NULL, msg, "ScarletNX", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

/* the window's title says so while the player has the game paused */
static void title_paused(int paused) {
    static const char mark[] = " (paused)";
    const char* t = g_win ? SDL_GetWindowTitle(g_win) : NULL;
    char title[1024];
    size_t n;
    if (!t) return;
    n = strlen(t);
    if (n >= sizeof mark - 1 && !strcmp(t + n - (sizeof mark - 1), mark)) n -= sizeof mark - 1;
    snprintf(title, sizeof title, "%.*s%s", (int)n, t, paused ? mark : "");
    SDL_SetWindowTitle(g_win, title);
}
static void hotkey(SDL_Keycode k) {
    switch (k) {
        case SDLK_F1: {
            int fs = g_fullscreen;
            if (fs) set_fullscreen(0);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "ScarletNX", HELP, g_win);
            if (fs) set_fullscreen(1);
            break;
        }
        case SDLK_F3: g_shot_req = 1; break;
        case SDLK_F4: {
            /* off and back to the limit the player set (fps_limit=120 stays 120), or to the game's pace when it
               started off */
            static int was = SNX_FPS_GAME;
            if (g_opt_fps) {
                was = g_opt_fps;
                __atomic_store_n(&g_opt_fps, 0, __ATOMIC_RELAXED); /* present_pace reads it on the game's threads */
            } else
                __atomic_store_n(&g_opt_fps, was, __ATOMIC_RELAXED);
            char v[16];
            fps_limit_text(g_opt_fps, v, sizeof v);
            ini_set("fps_limit", v);
            if (g_opt_fps == SNX_FPS_GAME)
                LOG("FPS limit: the game's pace\n");
            else if (g_opt_fps)
                LOG("FPS limit: %d\n", g_opt_fps);
            else
                LOG("FPS limit: off\n");
            break;
        }
        case SDLK_F5: kernel_dump_threads(); break;
        case SDLK_F6: {
            int on = !__atomic_load_n(&g_settings.showcase, __ATOMIC_RELAXED);
            __atomic_store_n(&g_settings.showcase, on, __ATOMIC_RELAXED); /* a run's --showcase stays out of the file */
            ini_set("showcase", on ? "1" : "0");
            if (on)
                LOG("showcase: on, showcase_corner=%s\n", g_settings.showcase_corner);
            else
                LOG("showcase: off\n");
            break;
        }
        case SDLK_F7: state_ask(STATE_SAVE, -1); break;
        case SDLK_F8: state_ask(STATE_LOAD, -1); break;
        case SDLK_F9: state_slot_next(); break;
#if defined(SNX_TRACY)
        case SDLK_F10: snx_tracy_capture_now(); break;
#endif
        case SDLK_F11: set_fullscreen(!g_fullscreen); break;
        case SDLK_PAUSE:
            game_pause_toggle();
            title_paused(game_paused());
            break;
    }
}

/* The game's menu takes the keys, the buttons, the fingers and the mouse while it is open. Back, Home and F2 open
   and close it, and they and Esc end the on-screen controls' edit mode. Under ui_lock: the renderer draws them on its
   thread */
/* a finger's id, the pen's apart: SDL gives a pen's touch the id 1 on every platform, the id Android and Wayland
   give a first finger and any finger may have, and no real finger's id sets bit 63 */
static u64 finger_id(const SDL_TouchFingerEvent* f) {
    return (u64)f->fingerID | (f->touchID == SDL_PEN_TOUCHID ? 1ull << 63 : 0);
}
static void menu_open(int open) {
    if (overlay_shown() != open) overlay_toggle();
    vpad_release();
    keys_held = 0;
    /* a finger or button down before this (Home on the on-screen controls opened it) lifts without a tap */
    overlay_pointers_reset();
}
static void overlay_do(int action) {
    if (action == OVERLAY_EXIT) g_quit = 1;
    if (action == OVERLAY_CLOSE) menu_open(0);
    /* the frontend's loop does them (state_poll), the menu closed first for a load: the game it goes back to shows */
    if (action == OVERLAY_SAVE) state_ask(STATE_SAVE, overlay_slot());
    if (action == OVERLAY_DELETE) state_ask(STATE_DELETE, overlay_slot());
    if (action == OVERLAY_LOAD) {
        menu_open(0);
        state_ask(STATE_LOAD, overlay_slot());
    }
    if (action == OVERLAY_EDIT) {
        menu_open(0);
        vpad_edit(1);
    }
}
static int menu_event(const SDL_Event* e) {
    int w, h;
    float d;
    if ((e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && (e->key.key == SDLK_AC_BACK || e->key.key == SDLK_F2)) ||
        (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
        if (vpad_editing())
            vpad_edit(0);
        else
            menu_open(!overlay_shown());
        return 1;
    }
    if (vpad_editing() && e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
        vpad_edit(0);
        return 1;
    }
    if (!overlay_shown()) return 0;
    if (e->type == SDL_EVENT_KEY_DOWN) {
        switch (e->key.key) {
            case SDLK_UP: overlay_move(-1); break;
            case SDLK_DOWN: overlay_move(1); break;
            case SDLK_LEFT: overlay_side(-1); break;
            case SDLK_RIGHT: overlay_side(1); break;
            case SDLK_RETURN:
            case SDLK_SPACE:
                if (!e->key.repeat) overlay_do(overlay_select());
                break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                if (!e->key.repeat) overlay_do(overlay_back());
                break;
            default: break;
        }
    } else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        /* the pads' presses act on the menu from the poll, with their sticks and the on-screen controls'; the button
           map page waiting for one takes it as the control it is */
        overlay_press(snx_pad_of_button(e->gbutton.button));
    } else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && overlay_listening() && e->gaxis.value > SNX_PAD_TRIGGER &&
               (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        overlay_press(e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SNX_PAD_LT : SNX_PAD_RT);
    } else if (e->type == SDL_EVENT_MOUSE_WHEEL) { /* a notch up is a row up, as the system turns the wheel */
        overlay_scroll(-e->wheel.integer_y);
    } else if (e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_MOTION ||
               e->type == SDL_EVENT_FINGER_UP) {
        /* a finger on an on-screen control stays the controls': their presses move the menu */
        int safe[4];
        float x, y;
        backend_window_size(&w, &h);
        backend_window_safe(&safe[0], &safe[1], &safe[2], &safe[3]);
        x = e->tfinger.x * (float)w;
        y = e->tfinger.y * (float)h;
        if (e->type == SDL_EVENT_FINGER_DOWN)
            overlay_pointer_down(finger_id(&e->tfinger), x, y, safe);
        else if (e->type == SDL_EVENT_FINGER_MOTION)
            overlay_pointer_move(finger_id(&e->tfinger), x, y, safe);
        else
            overlay_do(overlay_pointer_up(finger_id(&e->tfinger), x, y, w, h));
    } else if (((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                e->button.which != SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT) ||
               (e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which != SDL_TOUCH_MOUSEID)) {
        /* the mouse's left button as one more finger, as touch_mouse makes it while the menu is closed */
        int safe[4];
        float x = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x,
              y = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y;
        backend_window_size(&w, &h);
        backend_window_safe(&safe[0], &safe[1], &safe[2], &safe[3]);
        d = SDL_GetWindowPixelDensity(g_win);
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            overlay_pointer_down(SNX_TOUCH_MOUSE, x * d, y * d, safe);
        else if (e->type == SDL_EVENT_MOUSE_MOTION)
            overlay_pointer_move(SNX_TOUCH_MOUSE, x * d, y * d, safe);
        else
            overlay_do(overlay_pointer_up(SNX_TOUCH_MOUSE, x * d, y * d, w, h));
    } else if (e->type == SDL_EVENT_FINGER_CANCELED) {
        overlay_pointer_cancel(finger_id(&e->tfinger));
        return 0; /* the game's touch screen lets it go too */
    } else if (e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
               e->type != SDL_EVENT_MOUSE_BUTTON_UP && e->type != SDL_EVENT_MOUSE_MOTION)
        return 0; /* the window's events, and keys let go: the game still has them */
    return 1;
}
static int overlay_event(const SDL_Event* e) {
    int taken;
    ui_lock();
    taken = menu_event(e);
    ui_unlock();
    return taken;
}

/* the picture the renderer shows, in the window's pixels: where a finger lands on the game's touch screen */
static void touch_window(int picture[4]) {
    int x, y, w, h;
    backend_window_safe(&x, &y, &w, &h);
    touch_picture(x, y, w, h, picture);
}
/* a finger on an on-screen control is the controls', the others the game's touch screen's */
static void touch_finger(const SDL_Event* e) {
    int w, h, picture[4], safe[4], taken = 0;
    float px, py;
    backend_window_size(&w, &h);
    px = e->tfinger.x * (float)w;
    py = e->tfinger.y * (float)h;
    backend_window_safe(&safe[0], &safe[1], &safe[2], &safe[3]);
    ui_lock();
    if (e->type == SDL_EVENT_FINGER_DOWN)
        taken = vpad_down(finger_id(&e->tfinger), px, py, safe);
    else if (e->type == SDL_EVENT_FINGER_MOTION)
        vpad_move(finger_id(&e->tfinger), px, py, safe);
    else
        vpad_up(finger_id(&e->tfinger));
    ui_unlock();
    if (taken || !g_settings.touch_screen) return;
    touch_window(picture);
    touch_set(e->type == SDL_EVENT_FINGER_DOWN     ? TOUCH_DOWN
              : e->type == SDL_EVENT_FINGER_MOTION ? TOUCH_MOVE
                                                   : TOUCH_UP,
              finger_id(&e->tfinger), picture, px, py);
}
/* the mouse's left button as one more finger: on the on-screen controls when they are on, and on the touch screen
   when the touch option asks for it; positions come in window points */
static void touch_mouse(const SDL_Event* e) {
    int picture[4], safe[4], taken = 0;
    float d = SDL_GetWindowPixelDensity(g_win);
    backend_window_safe(&safe[0], &safe[1], &safe[2], &safe[3]);
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        if (e->motion.which == SDL_TOUCH_MOUSEID) return;
        ui_lock();
        vpad_move(SNX_TOUCH_MOUSE, e->motion.x * d, e->motion.y * d, safe);
        ui_unlock();
        touch_window(picture);
        if (g_settings.touch_screen == 2 && (e->motion.state & SDL_BUTTON_LMASK))
            touch_set(TOUCH_MOVE, SNX_TOUCH_MOUSE, picture, e->motion.x * d, e->motion.y * d);
    } else if (e->button.which != SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT) {
        ui_lock();
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            taken = vpad_down(SNX_TOUCH_MOUSE, e->button.x * d, e->button.y * d, safe);
        else
            taken = vpad_up(SNX_TOUCH_MOUSE);
        ui_unlock();
        if (taken || g_settings.touch_screen != 2) return;
        touch_window(picture);
        touch_set(e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? TOUCH_DOWN : TOUCH_UP, SNX_TOUCH_MOUSE, picture,
                  e->button.x * d, e->button.y * d);
    }
}
/* the keys that skip: the letters, digits, Return, Backspace, Tab, Space and the punctuation after them, the arrows
   and the keypad. Not Esc, which quits, the function keys (the runtime's own: F2 opens the menu), Home and the keys
   around it, the lock and modifier keys, media keys or Android's back; nor a key with Ctrl, Alt or the system key, the
   desktop's shortcuts (the system key alone skipped one run) */
static int press_key(const SDL_KeyboardEvent* k) {
    SDL_Scancode c = k->scancode;
    return !k->repeat && !(k->mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) &&
           ((c >= SDL_SCANCODE_A && c <= SDL_SCANCODE_SLASH && c != SDL_SCANCODE_ESCAPE) ||
            c == SDL_SCANCODE_NONUSBACKSLASH || /* the ISO layouts' key beside left shift */
            c == SDL_SCANCODE_INTERNATIONAL1 || c == SDL_SCANCODE_INTERNATIONAL3 || /* JIS's Ro and Yen */
            (c >= SDL_SCANCODE_RIGHT && c <= SDL_SCANCODE_UP) ||
            (c >= SDL_SCANCODE_KP_DIVIDE && c <= SDL_SCANCODE_KP_PERIOD));
}
/* a pad's slot in g_press, by its id: a free one taken for one not seen yet (NULL when none is) */
static u32* press_pad(SDL_JoystickID id) {
    int i, free_at = -1;
    for (i = 0; i < SNX_GAMEPADS; ++i) {
        if (g_press.pad[i].id == id) return &g_press.pad[i].held;
        if (!g_press.pad[i].id && free_at < 0) free_at = i;
    }
    if (free_at < 0) return NULL;
    g_press.pad[free_at].id = id;
    g_press.pad[free_at].held = 0;
    return &g_press.pad[free_at].held;
}
/* What a pad holds as SDL knows it, which doesn't press until let go: from nothing for a pad just opened
   (gamepad_open), else added to what was followed. SDL starts an opened pad at nothing and sends what it holds as
   presses with the pad's first report. What SDL knows is taken when the screen begins to listen and when gamepad_close
   opens a waiting pad (press_pads_held), so a report that came before then doesn't press. One that comes after does:
   no event tells a pad's first report from a press */
static void press_pad_held(SDL_JoystickID id, int opened) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(id);
    u32* held = pad ? press_pad(id) : NULL;
    if (!held) return;
    if (opened) *held = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 30; ++b)
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) *held |= 1u << b;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= SNX_PAD_TRIGGER) *held |= 1u << 30;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= SNX_PAD_TRIGGER) *held |= 1u << 31;
}
/* every open pad's (gamepad_close opens a waiting one in a freed slot without an event of its own) */
static void press_pads_held(void) {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) press_pad_held(ids[i], 0);
    SDL_free(ids);
}
/* What the event is to the prepare screen's press: 1 when the screen takes it. Every pad's buttons and triggers are
   followed, listening or not, so one held from before counts only once let go. While the menu or the controls' edit
   mode is over the screen, they take what they would take over the game, and nothing presses */
static int press_event(const SDL_Event* e) {
    u32* held;
    int bit, listening = g_press.listening && !overlay_shown() && !vpad_editing();
    switch (e->type) {
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (bit = 0; bit < SNX_GAMEPADS; ++bit)
                if (g_press.pad[bit].id == e->gdevice.which) g_press.pad[bit].id = 0;
            return 0;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            int was;
            if (e->gbutton.button >= 30 || !(held = press_pad(e->gbutton.which))) return 0;
            was = (*held >> e->gbutton.button) & 1;
            if (e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
                *held &= ~(1u << e->gbutton.button);
                return 0;
            }
            *held |= 1u << e->gbutton.button;
            /* not Home (the guide button), which opens the menu */
            if (!listening || was || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) return 0;
            g_press.kind = PRESS_BUTTON;
            g_press.code = e->gbutton.button;
            return 1;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: { /* ZL and ZR, as far as the game holds them (SNX_PAD_TRIGGER) */
            int over = e->gaxis.value >= SNX_PAD_TRIGGER;
            if (e->gaxis.axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && e->gaxis.axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                return 0;
            if (!(held = press_pad(e->gaxis.which))) return 0;
            bit = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 30 : 31;
            if (!over) {
                *held &= ~(1u << bit);
                return 0;
            }
            if ((*held >> bit) & 1) return 0;
            *held |= 1u << bit;
            if (!listening) return 0;
            g_press.kind = PRESS_TRIGGER;
            g_press.code = bit == 30 ? 'L' : 'R';
            return 1;
        }
        case SDL_EVENT_KEY_DOWN:
            if (!listening || !press_key(&e->key)) return 0;
            g_press.kind = PRESS_KEY;
            g_press.code = e->key.scancode;
            return 1;
        /* A finger on the screen itself presses, or a pen's tip (SDL makes it a touch of a direct device,
           SDL_PEN_TOUCHID), not a trackpad's (an indirect device), nor the mouse: the click that gives the window the
           focus arrives as one, and so does the pen's. While the screen listens every pointer is its own: the
           on-screen controls it hides (Home, Capture) take none */
        case SDL_EVENT_FINGER_DOWN:
            if (!listening) return 0;
            if (!g_press.finger && SDL_GetTouchDeviceType(e->tfinger.touchID) == SDL_TOUCH_DEVICE_DIRECT) {
                g_press.kind = PRESS_TOUCH;
                g_press.touch = e->tfinger.touchID;
                g_press.finger = e->tfinger.fingerID;
            }
            return 1;
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (g_press.finger && e->tfinger.fingerID == g_press.finger && e->tfinger.touchID == g_press.touch) {
                if (e->type != SDL_EVENT_FINGER_MOTION) g_press.finger = 0;
                return 1;
            }
            /* a lift goes on, so a finger that went down before the screen listened doesn't stay down in the game
               (touch_up and vpad_up pass over one they don't know) */
            return listening && e->type == SDL_EVENT_FINGER_MOTION;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL: return listening;
    }
    return 0;
}
void video_poll(void) {
    SDL_Event e;
    char* left = __atomic_load_n(&g_fatal_left, __ATOMIC_ACQUIRE);
    if (left) { /* another thread's fatal error, left for this thread to show */
        __atomic_store_n(&g_fatal_taken, 1, __ATOMIC_RELEASE);
        if (!fatal_screen(left)) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", left, g_win);
        plat_exit_now(1);
    }
    /* the menu had the controls when this poll began: the press that closes it (B) is in this poll's state */
    int menu_had = overlay_shown() || vpad_editing();
    while (!g_opt_headless && SDL_PollEvent(&e)) {
        if (press_event(&e)) continue;
        if (overlay_event(&e)) continue;
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = 1; break;
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_OCCLUDED: __atomic_store_n(&g_minimized, 1, __ATOMIC_RELEASE); break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_MAXIMIZED:
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_SHOWN: __atomic_store_n(&g_minimized, 0, __ATOMIC_RELEASE); break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (e.window.windowID == SDL_GetWindowID(g_win)) wsize_set(e.window.data1, e.window.data2);
                wsafe_set();
                break;
            case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: wsafe_set(); break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                overlay_set_display_scale(SDL_GetWindowDisplayScale(g_win));
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: touch_finger(&e); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION: touch_mouse(&e); break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                g_fullscreen = 1;
                wsafe_set();
                break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                g_fullscreen = 0;
                wsafe_set();
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) { /* a fresh press: the repeats of the Esc that closed the menu don't */
                    if (!e.key.repeat) g_quit = 1;
                    break;
                }
                if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                    if (!e.key.repeat) set_fullscreen(!g_fullscreen);
                    break;
                }
                /* not with Ctrl, Alt or the system key: Alt+F4 also arrives as F4 on Windows */
                if (!e.key.repeat && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) hotkey(e.key.key);
                /* a repeat doesn't press again: a key held as the menu closed (which cleared keys_held) stays from
                   the game until it is let go */
                if (!e.key.repeat) keys_held |= keymap(e.key.scancode);
                break;
            case SDL_EVENT_KEY_UP: keys_held &= ~keymap(e.key.scancode); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                keys_held = 0;
                ui_lock();
                vpad_release();
                overlay_pointers_reset(); /* lifts that come without their presses don't tap the menu */
                ui_unlock();
                /* SDL leaves a held mouse button to the system's implicit grab, whose release may go to the window
                   that took the focus: the game's touch from the mouse lifts now (a later release finds it up) */
                touch_set(TOUCH_UP, SNX_TOUCH_MOUSE, NULL, 0, 0);
                break;
            case SDL_EVENT_JOYSTICK_ADDED: gamepad_detect(e.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED:
                gamepad_open(e.gdevice.which);
                press_pad_held(e.gdevice.which, 1);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                gamepad_close(e.gdevice.which);
                press_pads_held();
                break;
        }
    }
    /* the scripted presses act like keys, stick keys included. Bits 16..23 (the stick directions as buttons) can be
       scripted too, but the stick keys (40..47) are no npad buttons */
    static u64 script_was; /* the scripted buttons of the last poll: a Home press opens or closes the menu */
    u64 s = unattended_buttons(), k = keys_held | s;
    if (s & ~script_was & SNX_SCRIPT_HOME) {
        ui_lock();
        menu_open(!overlay_shown());
        ui_unlock();
    }
    script_was = s;
    u64 nonpad = (k & 0xFFFFull) | (s & 0xFF0000ull), b;
    i32 st[4], pad[4], pad0[4];
    u64 pb;
    input_key_sticks(k, st);
    i32 lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    pm_lock(&g_host.hid.input_lock);
    touch_lift();
    /* Each SDL slot is one guest Npad. Keyboard, scripted and on-screen input also drive No1. */
    static u32 previous_ids[SNX_GAMEPADS];
    pad0[0] = pad0[1] = pad0[2] = pad0[3] = 0;
    for (int i = 0; i < SNX_GAMEPADS; i++) {
        PadState* player = i ? &g_hle.hid.pad_extra[i - 1] : &g_hle.hid.pad1;
        u64 one_buttons;
        i32 one_sticks[4];
        u32 id = gamepad_slot_id(i);
        int connected = id != 0;
        /* A disconnect and replacement can happen in one poll. Do not send the old player's rumble to the new pad. */
        if (previous_ids[i] != id) {
            __atomic_store_n(&player->rumble[0], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&player->rumble[1], 0, __ATOMIC_RELAXED);
            if (i)
                g_menu_held_extra[i - 1] = 0;
            else
                g_menu_held_pad0 = 0;
            previous_ids[i] = id;
        }
        gamepad_state_slot(i, &one_buttons, one_sticks);
        gamepad_rumble_slot(i, __atomic_load_n(&player->rumble[0], __ATOMIC_RELAXED),
                            __atomic_load_n(&player->rumble[1], __ATOMIC_RELAXED));
        /* each player as its settings say: seen or not, and the kind of controller the game sees */
        __atomic_store_n(&player->connected, (connected && gamepad_player_seen(i)) || i == 0, __ATOMIC_RELAXED);
        __atomic_store_n(&player->kind,
                         gamepad_player_kind(i, connected ? gamepad_kind(gamepad_slot_type(i)) : SNX_PAD_FULL_KEY),
                         __ATOMIC_RELAXED);
        player->source_id = id;
        player->buttons = one_buttons;
        player->lx = one_sticks[0];
        player->ly = one_sticks[1];
        player->rx = one_sticks[2];
        player->ry = one_sticks[3];
        gamepad_motion(i, player->motion);
        if (i == 0) memcpy(pad0, one_sticks, sizeof pad0);
    }
    gamepad_state(&pb, pad); /* the combined state drives the menu and its held-control display */
    if (!lx && !ly) {
        lx = pad0[0];
        ly = pad0[1];
    }
    if (!rx && !ry) {
        rx = pad0[2];
        ry = pad0[3];
    }
    { /* the on-screen controls, as one more pad: their sticks over the others' while held */
        u64 vb;
        i32 vs[4];
        unsigned acts;
        ui_lock();
        vpad_state(&vb, vs);
        acts = vpad_actions();
        if (acts & SNX_VPAD_DO_MENU) menu_open(1);
        /* with the menu open, the pads move it: the on-screen controls' and the connected ones', sticks too */
        overlay_do(overlay_controls(vb | pb | (s & 0xFFFFFull), vs[0] || vs[1] ? vs : pad, SDL_GetTicks()));
        ui_unlock();
        nonpad |= vb;
        if (vs[0] || vs[1]) {
            lx = vs[0];
            ly = vs[1];
        }
        if (vs[2] || vs[3]) {
            rx = vs[2];
            ry = vs[3];
        }
        if (acts & SNX_VPAD_DO_SHOT) g_shot_req = 1;
        if ((acts & SNX_VPAD_DO_BUZZ) && g_buzzer) SDL_PlayHapticRumble(g_buzzer, 0.5f, 15);
    }
    input_stick_clamp(&lx, &ly);
    input_stick_clamp(&rx, &ry);
    if (g_press.listening) { /* the loading screen's: a pad's buttons held then reach the game only once let go */
        g_menu_held_pad0 |= g_hle.hid.pad1.buttons;
        for (int i = 0; i < SNX_GAMEPADS - 1; i++) g_menu_held_extra[i] |= g_hle.hid.pad_extra[i].buttons;
    }
    if (menu_had || overlay_shown() || vpad_editing()) { /* the game's menu, or the controls' edit mode, has them */
        g_menu_held = nonpad;
        g_menu_held_pad0 = g_hle.hid.pad1.buttons;
        b = 0;
        lx = ly = rx = ry = 0;
        g_hle.hid.touch.down = 0;
        for (int i = 0; i < SNX_GAMEPADS - 1; i++) {
            g_menu_held_extra[i] = g_hle.hid.pad_extra[i].buttons;
            g_hle.hid.pad_extra[i].buttons = 0;
            g_hle.hid.pad_extra[i].lx = g_hle.hid.pad_extra[i].ly = 0;
            g_hle.hid.pad_extra[i].rx = g_hle.hid.pad_extra[i].ry = 0;
            memset(g_hle.hid.pad_extra[i].motion, 0, sizeof g_hle.hid.pad_extra[i].motion);
        }
        memset(g_hle.hid.pad1.motion, 0, sizeof g_hle.hid.pad1.motion);
    } else {
        g_menu_held &= nonpad;
        g_menu_held_pad0 &= g_hle.hid.pad1.buttons;
        b = (nonpad & ~g_menu_held) | (g_hle.hid.pad1.buttons & ~g_menu_held_pad0);
        for (int i = 0; i < SNX_GAMEPADS - 1; i++) {
            g_menu_held_extra[i] &= g_hle.hid.pad_extra[i].buttons;
            g_hle.hid.pad_extra[i].buttons &= ~g_menu_held_extra[i];
        }
    }
    g_hle.hid.pad1.buttons = b;
    g_hle.hid.pad1.lx = lx;
    g_hle.hid.pad1.ly = ly;
    g_hle.hid.pad1.rx = rx;
    g_hle.hid.pad1.ry = ry;
    {
        i32 shown[4] = {lx, ly, rx, ry};
        ui_lock();
        vpad_showcase_input(b, shown);
        ui_unlock();
    }
    pm_unlock(&g_host.hid.input_lock);
}

char* backend_launcher_folder(void) {
#ifdef SDL_PLATFORM_ANDROID
    const char* files = SDL_GetAndroidInternalStoragePath();
    return files ? snx_path_join(files, SNX_PORTABLE_DIR) : NULL;
#else
    return snx_strdup(g_exedir[0] ? g_exedir : ".");
#endif
}
const char* backend_pressed(void) {
    if (g_opt_headless) return NULL; /* no window, and SDL isn't started for one */
    if (!g_press.listening) press_pads_held();
    g_press.listening = 1;
    switch (g_press.kind) {
        case PRESS_KEY:
            snprintf(g_press.said, sizeof g_press.said, "the key %s",
                     SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)g_press.code, SDL_KMOD_NONE, false)));
            break;
        case PRESS_TOUCH: snprintf(g_press.said, sizeof g_press.said, "a touch"); break;
        case PRESS_BUTTON: {
            const char* name = SDL_GetGamepadStringForButton((SDL_GamepadButton)g_press.code);
            snprintf(g_press.said, sizeof g_press.said, "a gamepad's button %s", name ? name : "?");
            break;
        }
        case PRESS_TRIGGER: snprintf(g_press.said, sizeof g_press.said, "a gamepad's Z%c", (char)g_press.code); break;
        default: return NULL;
    }
    g_press.kind = PRESS_NONE;
    return g_press.said;
}
void frontend_frame(void) {
    char p[1200];
    g_press.listening = 0; /* the game's first thread runs: its presses are its own */
    g_press.kind = PRESS_NONE;
    /* the controls and the menu stay on the window while the game shows nothing new, and so does its last frame while
       it is paused */
    if (g_renderer->refresh && (overlay_shown() || vpad_shown() || game_paused() || overlay_toasting(plat_ns()) ||
                                __atomic_load_n(&g_settings.showcase, __ATOMIC_RELAXED)))
        g_renderer->refresh();
    if (g_shot_req) {
        g_shot_req = 0;
        time_t now = time(NULL);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        snprintf(p, sizeof p, "%scaptures", g_exedir);
        plat_mkdir(p);
        snprintf(p, sizeof p, "%scaptures/screenshot_%04d%02d%02d_%02d%02d%02d", g_exedir, t.tm_year + 1900,
                 t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        g_renderer->screenshot(p);
        ui_lock();
        overlay_toast("Screenshot saved in captures", plat_ns());
        ui_unlock();
    }
    if (unattended_periodic_shot(p, sizeof p)) g_renderer->screenshot(p);
    if (unattended_done()) {
        /* the size of the image the renderer saved isn't known here: the window's would be a different one */
        unattended_exit(g_renderer->screenshot(unattended_shot()), 0, 0);
    }
}
