/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/present.h"
#include "common/utilities/common.h"
#include "common/utilities/key_text.h"
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
    /* the keys that step the upscaler and the anti-aliasing for the run (the Switch's hotkey_upscaler and
       hotkey_anti_aliasing), 0 for none */
    unsigned shortcut_key[2], shortcut_mods[2];
    bool shared_said;    /* both are one key, said once */
    bool shortcuts_read; /* their keys read from the settings */
} g_present;
static volatile sig_atomic_t g_interrupted;
static void on_signal(int signal) {
    (void)signal;
    g_interrupted = 1;
}
/* the shortcuts' keys from their settings, once: main.c refused a text that is no key already, keeping the default's */
static void shortcuts_init(void) {
    const char* const keys[2] = {g_settings.upscaler_shortcut, g_settings.anti_aliasing_shortcut};
    char why[192];
    if (g_present.shortcuts_read) return;
    g_present.shortcuts_read = true;
    for (unsigned i = 0; i < 2; ++i)
        if (snx_key_parse(keys[i], &g_present.shortcut_key[i], &g_present.shortcut_mods[i], why, sizeof why))
            printf("vita: %s=%s: %s: it has no key\n", i ? "hotkey_anti_aliasing" : "hotkey_upscaler", keys[i], why);
}
void vita_present_init(const char* title, const char* shot) {
    char error[256];
    if (!vita_vk_init(title, error, sizeof error)) printf("vita: no Vulkan device, nothing is drawn: %s\n", error);
    g_present.shot = shot && *shot ? strdup(shot) : NULL;
    shortcuts_init();
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
        /* the upscaler's and the anti-aliasing's shortcuts, as a Switch game's */
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            shortcuts_init();
            int also, which = snx_key_find(g_present.shortcut_key, g_present.shortcut_mods, 2, event.key.key,
                                           snx_key_mods(event.key.mod), &also);
            if (which >= 0) {
                if (also >= 0 && !g_present.shared_said) {
                    g_present.shared_said = true;
                    printf("vita: %s is hotkey_upscaler's and hotkey_anti_aliasing's: it does %s's\n",
                           g_settings.upscaler_shortcut, which ? "hotkey_anti_aliasing" : "hotkey_upscaler");
                }
                vita_vk_shortcut(which);
                continue;
            }
        }
        /* Esc twice ends the game, a first one warning on the terminal and over the game; not with Ctrl, Alt or the
           system key */
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat &&
            !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
            char warning[64];
            if (snx_quit_press(&g_present.quit_by, SDL_GetTicksNS())) return false;
            snx_quit_warning(warning, sizeof warning, "Esc");
            printf("%s\n", warning);
            vita_vk_notice(warning);
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
