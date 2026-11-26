/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/key_said.h"
#include "common/utilities/pace.h"
#include "common/utilities/notice.h"
#include "common/utilities/quit_key.h"
#include "common/utilities/ui_colors.h"
#include <stdlib.h>
#include <string.h>

#define QUADS 2304 /* the controls' (vpad.c) and a line of letters */

static struct {
    SDL_Window* window;
    char* ini;
    void (*say)(const char* line);
    void (*show)(const char* line);
    SDL_Haptic* buzzer;
    int finger_seen;
    bool paused;           /* the pause key's */
    int pace, pace_before; /* fps_limit's (pace.h), and F4's pace before none */
    char fps_limit[16];    /* its text as read */
    uint64_t quit_by; /* a second Back before this quits (quit_key.h), the first one's line shown until then */
    SnxUiRects rects;
    int made, warned, w, h; /* what the rectangles are made for: a first time, with the line or not, the picture */
    SnxUiQuad quads[QUADS];
} g_vpad_sdl;

void snx_vpad_sdl_hints(void) {
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* a pen too, which stays a finger */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
}
u64 snx_vpad_sdl_finger(const SDL_TouchFingerEvent* f) {
    return (u64)f->fingerID | (f->touchID == SDL_PEN_TOUCHID ? 1ull << 63 : 0);
}
int snx_vpad_sdl_pointer(const SDL_Event* e, float x, float y, const int safe[4]) {
    switch (e->type) {
        case SDL_EVENT_FINGER_DOWN: return vpad_down(snx_vpad_sdl_finger(&e->tfinger), x, y, safe);
        case SDL_EVENT_FINGER_MOTION: vpad_move(snx_vpad_sdl_finger(&e->tfinger), x, y, safe); return 0;
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: vpad_up(snx_vpad_sdl_finger(&e->tfinger)); return 0;
        case SDL_EVENT_MOUSE_MOTION: vpad_move(SNX_VPAD_MOUSE, x, y, safe); return 0;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: return vpad_down(SNX_VPAD_MOUSE, x, y, safe);
        case SDL_EVENT_MOUSE_BUTTON_UP: return vpad_up(SNX_VPAD_MOUSE);
        default: return 0;
    }
}
void snx_vpad_sdl_auto(int finger_seen) {
    int n = 0, pads = 0, touch = finger_seen;
    SDL_TouchID* t = SDL_GetTouchDevices(&n);
    SDL_JoystickID* g = SDL_GetGamepads(&pads);
    for (int i = 0; t && i < n && !touch; i++) touch = SDL_GetTouchDeviceType(t[i]) == SDL_TOUCH_DEVICE_DIRECT;
    SDL_free(t);
    SDL_free(g);
    vpad_auto(touch, pads);
}
SDL_Haptic* snx_vpad_sdl_buzzer(void) {
    SDL_Haptic* buzzer = NULL;
#ifdef __ANDROID__
    int n = 0;
    SDL_HapticID* ids;
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) return NULL;
    ids = SDL_GetHaptics(&n);
    for (int i = 0; ids && i < n && !buzzer; i++) {
        const char* name = SDL_GetHapticNameForID(ids[i]);
        if (name && !strcmp(name, "VIBRATOR_SERVICE")) buzzer = SDL_OpenHaptic(ids[i]);
    }
    SDL_free(ids);
    if (buzzer && !SDL_InitHapticRumble(buzzer)) {
        SDL_CloseHaptic(buzzer);
        buzzer = NULL;
    }
#endif
    return buzzer;
}
void snx_vpad_sdl_buzz(SDL_Haptic* buzzer) {
    if (buzzer) SDL_PlayHapticRumble(buzzer, 0.5f, 15);
}

/* a control's place, kept in the game's .ini as it is moved or sized (snx_error says one that cannot be) */
static void store(const char* key, const char* value) {
    if (g_vpad_sdl.ini) (void)snx_ini_record_in(g_vpad_sdl.ini, SNX_VPAD_PAGE, key, value);
}
/* fps_limit read into the window's pace, a value not taken as given said with the one in effect */
static void fps_limit_set(const IniOpt* o, const char* v) {
    char line[192];
    int pace = g_vpad_sdl.pace;
    (void)snx_pace_parse(v, &pace, line, sizeof line);
    if (line[0]) snx_error("settings: %s", line);
    g_vpad_sdl.pace = pace;
    snx_pace_text(pace, (char*)o->var, o->size);
}
static const IniOpt FPS_LIMIT = {.key = "fps_limit",
                                 .def = "game",
                                 .type = INI_TEXT,
                                 .var = g_vpad_sdl.fps_limit,
                                 .size = sizeof g_vpad_sdl.fps_limit,
                                 .set = fps_limit_set};
static const IniOpt* setting(const char* key) { return strcmp(key, "fps_limit") ? hotkey_setting(key) : &FPS_LIMIT; }
void snx_vpad_sdl_settings(void) { vpad_settings_also(setting); }
uint64_t snx_vpad_window_frame_ns(uint64_t console_ns) {
    return g_vpad_sdl.pace == SNX_PACE_NONE      ? 0
           : g_vpad_sdl.pace == SNX_PACE_CONSOLE ? console_ns
                                                 : 1000000000ull / (uint64_t)g_vpad_sdl.pace;
}
void snx_vpad_window_open(SDL_Window* window, const char* ini, void (*say)(const char* line),
                          void (*show)(const char* line)) {
    g_vpad_sdl.window = window;
    g_vpad_sdl.ini = ini ? snx_strdup(ini) : NULL;
    g_vpad_sdl.say = say;
    g_vpad_sdl.show = show;
    vpad_set_say(say);
    vpad_set_store(store);
    snx_vpad_sdl_auto(0);
    g_vpad_sdl.buzzer = snx_vpad_sdl_buzzer();
}
void snx_vpad_window_close(void) {
    vpad_edit(0);
    vpad_set_store(NULL);
    vpad_set_say(NULL);
    if (g_vpad_sdl.buzzer) SDL_CloseHaptic(g_vpad_sdl.buzzer);
    free(g_vpad_sdl.ini);
    snx_ui_rects_free(&g_vpad_sdl.rects);
    memset(&g_vpad_sdl, 0, sizeof g_vpad_sdl);
}
void snx_vpad_window_safe(int safe[4]) {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_Rect r;
    SDL_GetWindowSizeInPixels(g_vpad_sdl.window, &pw, &ph);
    SDL_GetWindowSize(g_vpad_sdl.window, &w, &h);
    if (w > 0 && h > 0 && !(SDL_GetWindowFlags(g_vpad_sdl.window) & SDL_WINDOW_FULLSCREEN) &&
        SDL_GetWindowSafeArea(g_vpad_sdl.window, &r) && r.w > 0 && r.h > 0) {
        safe[0] = r.x * pw / w, safe[1] = r.y * ph / h;
        safe[2] = r.w * pw / w, safe[3] = r.h * ph / h;
    } else {
        safe[0] = safe[1] = 0;
        safe[2] = pw, safe[3] = ph;
    }
}
/* line on say and show */
static void say_shown(const char* line) {
    if (g_vpad_sdl.say) g_vpad_sdl.say(line);
    if (g_vpad_sdl.show) g_vpad_sdl.show(line);
}
bool snx_vpad_window_paused(void) { return g_vpad_sdl.paused; }
void snx_vpad_window_none(int action, const char* why) {
    const IniOpt* o = hotkey_row(action);
    const char* name = o && o->name ? o->name : "";
    size_t n = strlen(name) > 9 && !strcmp(name + strlen(name) - 9, " shortcut") ? strlen(name) - 9 : strlen(name);
    char line[256];
    snprintf(line, sizeof line, "%s (%.*s) does nothing in a %s game: %s", hotkey_text(action), (int)n, name,
             vpad_console_name(), why);
    say_shown(line);
}
/* the runtime's keys: the quit key's, the fullscreen keys', the menu's, which edits their layout as Android's Back
   does, and the others' for the console */
static SnxVpadEvent key_event(const SDL_KeyboardEvent* k, int* action) {
    if (k->repeat) return SNX_VPAD_PASS;
    bool back = k->key == SDLK_AC_BACK;
    int also, a = back ? HOTKEY_MENU : hotkey_find(k->key, hotkey_mods(k->mod), &also);
    if (vpad_editing() && k->key == SDLK_ESCAPE && !(k->mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
        a = HOTKEY_MENU; /* Esc ends the editing, as Done does */
    if (a == HOTKEY_QUIT) {
        char line[64];
        if (hotkey_quit_press(SDL_GetTicksNS())) return SNX_VPAD_QUIT;
        snx_quit_warning(line, sizeof line, hotkey_text(HOTKEY_QUIT));
        say_shown(line);
        return SNX_VPAD_TAKEN;
    }
    if (a == HOTKEY_FPS_LIMIT) { /* none, or back to the pace before it, as a Switch game's F4 */
        char line[96], now[16];
        snx_pace_toggle(&g_vpad_sdl.pace, &g_vpad_sdl.pace_before, line, sizeof line);
        if (g_vpad_sdl.say) g_vpad_sdl.say(line);
        snx_pace_text(g_vpad_sdl.pace, now, sizeof now);
        snx_fps_limit_now(line, sizeof line, now);
        if (g_vpad_sdl.show) g_vpad_sdl.show(line);
        return SNX_VPAD_TAKEN;
    }
    if (a == HOTKEY_PAUSE) {
        char line[96];
        g_vpad_sdl.paused = !g_vpad_sdl.paused;
        snx_key_said(line, sizeof line, "Pause", g_vpad_sdl.paused ? "on" : "off", g_vpad_sdl.paused ? "off" : "on");
        if (g_vpad_sdl.say) g_vpad_sdl.say(line);
        snx_key_now(line, sizeof line, "Pause", g_vpad_sdl.paused ? "on" : "off");
        if (g_vpad_sdl.show) g_vpad_sdl.show(line);
        return SNX_VPAD_TAKEN;
    }
    if (a == HOTKEY_FULLSCREEN || a == HOTKEY_FULLSCREEN_2) {
        SDL_SetWindowFullscreen(g_vpad_sdl.window, !(SDL_GetWindowFlags(g_vpad_sdl.window) & SDL_WINDOW_FULLSCREEN));
        return SNX_VPAD_TAKEN;
    }
    if (a >= 0 && a != HOTKEY_MENU) {
        *action = a;
        return SNX_VPAD_ACTION;
    }
    if (a < 0) return SNX_VPAD_PASS;
    if (back && SDL_GetTicksNS() < g_vpad_sdl.quit_by) return SNX_VPAD_QUIT;
    g_vpad_sdl.quit_by = 0;
    vpad_edit(!vpad_editing());
    if (back && vpad_editing()) { /* the first press, which a second within the time quits by */
        char line[64];
        (void)snx_quit_press(&g_vpad_sdl.quit_by, SDL_GetTicksNS());
        snx_quit_warning(line, sizeof line, "Back");
        if (g_vpad_sdl.say) g_vpad_sdl.say(line);
    }
    return SNX_VPAD_TAKEN;
}
bool snx_vpad_window_point(const SDL_Event* e, float* x, float* y) {
    int w = 0, h = 0, ww = 0, wh = 0;
    if (!SDL_GetWindowSizeInPixels(g_vpad_sdl.window, &w, &h) || !SDL_GetWindowSize(g_vpad_sdl.window, &ww, &wh) ||
        ww <= 0 || wh <= 0)
        return false;
    switch (e->type) {
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: /* a fraction of the window */
            *x = e->tfinger.x * (float)w, *y = e->tfinger.y * (float)h;
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            *x = e->motion.x * (float)w / (float)ww, *y = e->motion.y * (float)h / (float)wh;
            return e->motion.which != SDL_TOUCH_MOUSEID;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            *x = e->button.x * (float)w / (float)ww, *y = e->button.y * (float)h / (float)wh;
            return e->button.which != SDL_TOUCH_MOUSEID;
        default: return false;
    }
}
SnxVpadEvent snx_vpad_window_event(const SDL_Event* e, int* action) {
    int safe[4];
    float x, y;
    *action = -1;
    switch (e->type) {
        case SDL_EVENT_KEY_DOWN: return key_event(&e->key, action);
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED: snx_vpad_sdl_auto(g_vpad_sdl.finger_seen); return SNX_VPAD_PASS;
        case SDL_EVENT_WINDOW_FOCUS_LOST: vpad_release(); return SNX_VPAD_PASS;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (!g_vpad_sdl.finger_seen && SDL_GetTouchDeviceType(e->tfinger.touchID) == SDL_TOUCH_DEVICE_DIRECT) {
                g_vpad_sdl.finger_seen = 1; /* a screen SDL had not listed */
                snx_vpad_sdl_auto(1);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION: break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e->button.button != SDL_BUTTON_LEFT) return SNX_VPAD_PASS;
            break;
        default: return SNX_VPAD_PASS;
    }
    if (!snx_vpad_window_point(e, &x, &y)) return SNX_VPAD_PASS;
    snx_vpad_window_safe(safe);
    return snx_vpad_sdl_pointer(e, x, y, safe) ? SNX_VPAD_TAKEN : SNX_VPAD_PASS;
}
void snx_vpad_window_state(u64* buttons, i32 sticks[4]) {
    vpad_state(buttons, sticks);
    if (vpad_actions() & SNX_VPAD_DO_BUZZ) snx_vpad_sdl_buzz(g_vpad_sdl.buzzer);
    if (!vpad_editing()) return;
    *buttons = 0;
    for (int i = 0; i < 4; ++i) sticks[i] = 0;
}

/* Back's first press over the picture, in a band under edit mode's bar (vpad.c), as the bar's own letters */
static void warning_draw(SnxUiList* l, const int safe[4], int scale) {
    char line[64];
    snx_quit_warning(line, sizeof line, "Back");
    float w = (float)((int)(strlen(line) + 2) * 8 * scale), h = (float)(24 * scale);
    float x = (float)safe[0] + ((float)safe[2] - w) / 2, y = (float)(safe[1] + 32 * scale);
    ui_fill(l, x, y, w, h, UI_INK);
    ui_text(l, x + (float)(8 * scale), y + (float)(4 * scale), scale, line, UI_PAGE);
}
const SnxUiRects* snx_vpad_window_rects(int w, int h) {
    int safe[4], percent;
    if (!g_vpad_sdl.window || w <= 0 || h <= 0) return NULL;
    SnxUiList l = {g_vpad_sdl.quads, 0, QUADS};
    snx_vpad_window_safe(safe);
    percent = snx_display_percent(SDL_GetWindowDisplayScale(g_vpad_sdl.window));
    int scale = snx_menu_scale(w, h, percent), how = vpad_draw(&l, safe, scale);
    if (how == VPAD_DRAW_HIDDEN) return NULL;
    int warn = vpad_editing() && SDL_GetTicksNS() < g_vpad_sdl.quit_by;
    if (how == VPAD_DRAW_LAID || !g_vpad_sdl.made || warn != g_vpad_sdl.warned || w != g_vpad_sdl.w ||
        h != g_vpad_sdl.h) {
        if (warn) warning_draw(&l, safe, scale);
        if (!snx_ui_rects(&g_vpad_sdl.rects, &l, w, h) && g_vpad_sdl.say)
            g_vpad_sdl.say("out of memory for the on-screen controls' picture: they are not drawn");
        g_vpad_sdl.made = 1, g_vpad_sdl.warned = warn;
        g_vpad_sdl.w = w, g_vpad_sdl.h = h;
    }
    return g_vpad_sdl.rects.count ? &g_vpad_sdl.rects : NULL;
}
void snx_vpad_window_render(SDL_Renderer* renderer) {
    enum { AT_ONCE = 256 };
    SDL_FRect f[AT_ONCE];
    int w = 0, h = 0;
    const SnxUiRects* rects = SDL_GetCurrentRenderOutputSize(renderer, &w, &h) ? snx_vpad_window_rects(w, h) : NULL;
    for (size_t i = 0; rects && i < rects->count;) { /* runs of one colour */
        u32 c = rects->rect[i].rgba;
        int n = 0;
        for (; n < AT_ONCE && i < rects->count && rects->rect[i].rgba == c; ++i, ++n)
            f[n] = (SDL_FRect){rects->rect[i].x, rects->rect[i].y, rects->rect[i].w, rects->rect[i].h};
        SDL_SetRenderDrawColor(renderer, (Uint8)c, (Uint8)(c >> 8), (Uint8)(c >> 16), 255);
        SDL_RenderFillRects(renderer, f, n);
    }
}
