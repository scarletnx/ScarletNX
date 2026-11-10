/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/present.h"
#include "common/utilities/common.h"
#include "common/utilities/quit_key.h"
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
    uint64_t quit_by; /* a second Esc before this ends the game (common/utilities/quit_key.h) */
} g_present;
static volatile sig_atomic_t g_interrupted;
static void on_signal(int signal) {
    (void)signal;
    g_interrupted = 1;
}
void vita_present_init(const char* title, const char* shot) {
    char error[256];
    if (!vita_vk_init(title, error, sizeof error)) printf("vita: no Vulkan device, nothing is drawn: %s\n", error);
    g_present.shot = shot && *shot ? strdup(shot) : NULL;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    vita_input_init();
}
/* the framebuffer shown last as PATH<suffix>.ppm, once the GPU drew it */
static void shot_write(VitaProcess* p, const char* suffix);
bool vita_present_vblank(VitaProcess* p) {
    if (p->display.base) vita_render_show(p, p->display.base, p->display.pitch, p->display.width, p->display.height);
    unsigned every = (unsigned)g_settings.vita_screenshot_interval;
    if (g_present.shot && every && p->display.base && p->display.vblanks % every == 0) {
        char suffix[16];
        snprintf(suffix, sizeof suffix, "_%04u", (unsigned)(p->display.vblanks / every - 1));
        shot_write(p, suffix);
    }
    SDL_Event event;
    while (SDL_WasInit(SDL_INIT_EVENTS) && SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        /* Esc twice ends the game, a first one warning on the terminal; not with Ctrl, Alt or the system key */
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
            char warning[64];
            if (snx_quit_press(&g_present.quit_by, SDL_GetTicksNS())) return false;
            snx_quit_warning(warning, sizeof warning, "Esc");
            printf("%s\n", warning);
            continue;
        }
        vita_input_event(&event);
    }
    return !g_interrupted;
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
    vita_input_free();
    vita_vk_free();
    free(g_present.shot);
    memset(&g_present, 0, sizeof g_present);
}
