/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/renderdoc.h"
#include "vita/gpu/present.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/common.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/vpad_sdl.h"
#include "vita/gpu/render.h"
#include "vita/gpu/vk.h"
#include "vita/platform/input.h"
#include "vita/runtime/state.h"
#include "common/utilities/state_header.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    char why[256]; /* the device's failure */
    char* shot;
    bool video;
    char* captures; /* the game's folder with its separator, whose captures/ a screenshot goes in */
    bool shot_req;  /* the screenshot key's: the frame saved at the vblank's end */
    char* name;     /* the game's, its .ini's without .ini: its states are states/<name>.<slot>.snxstate */
    int state_ask;  /* a save states' key (HOTKEY_SAVE_STATE...) asked, done between two slices; -1 for none */
    bool dump;      /* the thread dump's key (F5): the threads listed between two slices */
} g_present = {.state_ask = -1};
static volatile sig_atomic_t g_interrupted;
static void on_signal(int signal) {
    (void)signal;
    g_interrupted = 1;
}
void vita_present_say(const char* line) { printf("%s\n", line); }
void vita_present_init(const char* title, const char* ini, const char* shot) {
    snx_vpad_sdl_hints();
    g_present.why[0] = 0;
    if (!vita_vk_init(title, g_present.why, sizeof g_present.why))
        printf("vita: no Vulkan device, nothing is drawn: %s\n", g_present.why);
    g_present.shot = shot && *shot ? strdup(shot) : NULL;
    {
        char* folder = ini ? snx_dirname(ini) : NULL;
        g_present.captures = snx_format("%s/", folder ? folder : ".");
        free(folder);
        g_present.name = snx_strdup(ini ? snx_path_leaf(ini) : "game");
        char* dot = strrchr(g_present.name, '.');
        if (dot && dot != g_present.name) *dot = 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    vita_input_init();
    if (vita_vk() && vita_vk()->host.window)
        snx_vpad_window_open(vita_vk()->host.window, ini, vita_present_say, vita_vk_notice);
}
static int state_save(void* game, const char* path, const uint8_t* thumb, char* why, size_t whysize) {
    return snx_vita_state_save((VitaProcess*)game, path, thumb, why, whysize);
}
static int state_load(void* game, const char* path, char* why, size_t whysize) {
    return snx_vita_state_load((VitaProcess*)game, path, why, whysize);
}
bool vita_present_dump_asked(void) {
    bool asked = g_present.dump;
    g_present.dump = false;
    return asked;
}
void vita_present_states(VitaProcess* p, uint64_t ms) {
    int ask = g_present.state_ask;
    if (ask < 0 && !snx_vpad_window_states_waiting()) return;
    g_present.state_ask = -1;
    uint8_t* thumb = p->display.base ? malloc((size_t)SNX_STATE_THUMB_W * SNX_STATE_THUMB_H * 4) : NULL;
    if (thumb)
        snx_state_thumb(vita_ptr(p, p->display.base), (int)p->display.width, (int)p->display.height,
                        (size_t)p->display.pitch * 4, thumb);
    SnxVpadStates states = {g_present.captures ? g_present.captures : "./", g_present.name ? g_present.name : "game", p,
                            state_save, state_load};
    if (ask >= 0) snx_vpad_window_states(ask, &states, thumb);
    snx_vpad_window_states_due(ms, &states, thumb);
    free(thumb);
}
/* the framebuffer shown last as PATH<suffix>.ppm, once the GPU drew it */
static void shot_write(VitaProcess* p, const char* suffix);
static void shot_key(VitaProcess* p);
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
        if (took == SNX_VPAD_ACTION && action == HOTKEY_SCREENSHOT)
            g_present.shot_req = true;
        else if (took == SNX_VPAD_ACTION &&
                 (action == HOTKEY_SAVE_STATE || action == HOTKEY_LOAD_STATE || action == HOTKEY_NEXT_SLOT))
            g_present.state_ask = action;
        else if (took == SNX_VPAD_ACTION && action == HOTKEY_THREAD_DUMP)
            g_present.dump = true;
        else if (took == SNX_VPAD_ACTION && action == HOTKEY_CAPTURE)
            (void)snx_renderdoc_capture(1); /* RenderDoc's, the frame after the next present */
        else if (took == SNX_VPAD_ACTION && (action == HOTKEY_UPSCALER || action == HOTKEY_ANTI_ALIASING))
            snx_vpad_window_effect(action, vita_vk_effect_step(action == HOTKEY_ANTI_ALIASING));
        else if (took == SNX_VPAD_ACTION)
            snx_vpad_window_none(action, action == HOTKEY_MOUSE     ? "its mouse is the front panel's finger and the "
                                                                      "pointer of the on-screen controls"
                                         : action == HOTKEY_TV_MODE ? "the TV mode is the Switch's console docked"
                                                                    : "its window has none of it yet");
        if (took != SNX_VPAD_PASS) continue;
        vita_input_event(&event);
    }
    vita_input_sample();
    if (g_present.shot_req) shot_key(p);
    return !g_interrupted;
}
const char* vita_present_why(void) { return vita_vk() ? "" : g_present.why; }
bool vita_present_lost(char* why, size_t why_size) {
    const VitaVk* v = vita_vk();
    if (!v || v->ring.lost == VK_SUCCESS) return false;
    snprintf(why, why_size, "the GPU stopped answering (%s): no frame is drawn after it",
             snx_vk_result_str(v->ring.lost));
    return true;
}
bool vita_present_vblank(VitaProcess* p) {
    char why[160];
    if (vita_present_lost(why, sizeof why)) return false; /* the run ends: its driver says why */
    bool open = vblank(p, false);
    while (open && snx_vpad_window_paused()) { /* the game's thread held, the frame and the keys going on */
        SDL_Delay(16);
        open = vblank(p, true);
    }
    return open;
}
/* SceDisplay's formats: A8B8G8R8 (0, the bytes R, G, B, A) and the others the runtime doesn't show yet. 0 once the
   frame is at path, said there */
static int shot_to(VitaProcess* p, const char* path) {
    vita_render_idle(p, true);
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    int failed = snx_ppm_write(f, p->display.width, p->display.height, vita_ptr(p, p->display.base), 4,
                               (ptrdiff_t)p->display.pitch * 4, SNX_PPM_RGBA);
    if (fclose(f)) failed = 1;
    if (!failed) printf("Screenshot: %s\n", path);
    return failed ? -1 : 0;
}
static void shot_write(VitaProcess* p, const char* suffix) {
    char* path = malloc(strlen(g_present.shot) + strlen(suffix) + 5);
    if (!path) return;
    sprintf(path, "%s%s.ppm", g_present.shot, suffix);
    (void)shot_to(p, path);
    free(path);
}
/* the screenshot key's: the frame as it shows in the game's captures/, as the Switch's is */
static void shot_key(VitaProcess* p) {
    char path[SNX_PATH_SIZE];
    g_present.shot_req = false;
    if (!p->display.base) {
        vita_present_say("Screenshot not saved: the game has shown no frame yet");
        vita_vk_notice("Screenshot not saved: the game has shown no frame yet");
        return;
    }
    snx_capture_path(path, sizeof path - 4, g_present.captures ? g_present.captures : "./");
    strcat(path, ".ppm");
    if (shot_to(p, path)) {
        printf("the screenshot could not be written to %s\n", path);
        vita_vk_notice("Screenshot not saved: the log says why");
    } else
        vita_vk_notice("Screenshot saved in captures");
}
void vita_present_shot(VitaProcess* p) {
    if (g_present.shot && p->display.base) shot_write(p, "");
}
void vita_present_free(void) {
    snx_vpad_window_close();
    vita_input_free();
    vita_vk_free();
    free(g_present.shot);
    free(g_present.captures);
    free(g_present.name);
    memset(&g_present, 0, sizeof g_present);
    g_present.state_ask = -1;
}
