/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/present.h"
#include "common/utilities/common.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/vpad_sdl.h"
#include "vita/gpu/render.h"
#include "vita/gpu/vk.h"
#include "vita/platform/input.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    char* shot;
    bool video;
} g_present;
static volatile sig_atomic_t g_interrupted;
static void on_signal(int signal) {
    (void)signal;
    g_interrupted = 1;
}
void vita_present_say(const char* line) { printf("%s\n", line); }
void vita_present_init(const char* title, const char* ini, const char* shot) {
    char error[256];
    snx_vpad_sdl_hints();
    if (!vita_vk_init(title, error, sizeof error)) printf("vita: no Vulkan device, nothing is drawn: %s\n", error);
    g_present.shot = shot && *shot ? strdup(shot) : NULL;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    vita_input_init();
    if (vita_vk() && vita_vk()->host.window)
        snx_vpad_window_open(vita_vk()->host.window, ini, vita_present_say, vita_vk_notice);
}
/* the framebuffer shown last as PATH<suffix>.ppm, once the GPU drew it */
static void shot_write(VitaProcess* p, const char* suffix);
/* a vblank's: the frame shown, a periodic screenshot (not again while paused), the window's events: false to end */
static bool vblank(VitaProcess* p, bool again) {
    if (p->display.base) vita_render_show(p, p->display.base, p->display.pitch, p->display.width, p->display.height);
    unsigned every = (unsigned)g_settings.vita_screenshot_interval;
    if (!again && g_present.shot && every && p->display.base && p->display.vblanks % every == 0) {
        char suffix[16];
        snprintf(suffix, sizeof suffix, "_%04u", (unsigned)(p->display.vblanks / every - 1));
        shot_write(p, suffix);
    }
    SDL_Event event;
    while (SDL_WasInit(SDL_INIT_EVENTS) && SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        int action;
        SnxVpadEvent took = snx_vpad_window_event(&event, &action);
        if (took == SNX_VPAD_QUIT) return false;
        if (took == SNX_VPAD_ACTION)
            snx_vpad_window_none(action, action == HOTKEY_MOUSE     ? "its mouse is the front panel's finger and the "
                                                                      "pointer of the on-screen controls"
                                         : action == HOTKEY_TV_MODE ? "the TV mode is the Switch's console docked"
                                                                    : "its window has none of it yet");
        if (took != SNX_VPAD_PASS) continue;
        vita_input_event(&event);
    }
    vita_input_sample();
    return !g_interrupted;
}
bool vita_present_vblank(VitaProcess* p) {
    bool open = vblank(p, false);
    while (open && snx_vpad_window_paused()) { /* the game's thread held, the frame and the keys going on */
        SDL_Delay(16);
        open = vblank(p, true);
    }
    return open;
}
/* SceDisplay's formats: A8B8G8R8 (0, the bytes R, G, B, A) and the others the runtime doesn't show yet */
static void shot_write(VitaProcess* p, const char* suffix) {
    vita_render_idle(p, true);
    char* path = malloc(strlen(g_present.shot) + strlen(suffix) + 5);
    if (!path) return;
    sprintf(path, "%s%s.ppm", g_present.shot, suffix);
    FILE* f = fopen(path, "wb");
    if (f) {
        (void)snx_ppm_write(f, p->display.width, p->display.height, vita_ptr(p, p->display.base), 4,
                            (ptrdiff_t)p->display.pitch * 4, SNX_PPM_RGBA);
        fclose(f);
        printf("Screenshot: %s\n", path);
    }
    free(path);
}
void vita_present_shot(VitaProcess* p) {
    if (g_present.shot && p->display.base) shot_write(p, "");
}
void vita_present_free(void) {
    snx_vpad_window_close();
    vita_input_free();
    vita_vk_free();
    free(g_present.shot);
    memset(&g_present, 0, sizeof g_present);
}
