/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vpad_sdl.h"
#include "common/utilities/console_settings.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/key_said.h"
#include "common/utilities/pad_map.h"
#include "common/utilities/pace.h"
#include "common/utilities/state_header.h"
#include "common/utilities/stats.h"
#include "common/utilities/vpad_showcase.h"
#include <stdarg.h>
#include <stdatomic.h>
#include "common/utilities/notice.h"
#include "common/utilities/quit_key.h"
#include "common/utilities/ui_colors.h"
#include "common/utilities/window_fullscreen.h"
#include <stdlib.h>
#include <string.h>

#define QUADS 4096 /* the controls' (vpad.c), a line of letters, the showcase's and the stats' box */

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
    int slot;              /* the save states' slot F7 and F8 use, 1 to state_slots, F9's step */
    char state_save[256], state_load[1024]; /* an unattended run's (state_save, state_load) */
    SnxSystem console;                      /* whose settings of the Switch's the rest are (console_settings.h) */
    IniOpt with_set[2];                     /* rows of COMMON_SETS, the console table's with their set function */
    uint32_t saves_done, loads_done;        /* a bit for each of their items done */
    uint64_t quit_by; /* a second Back before this quits (quit_key.h), the first one's line shown until then */
    SnxUiRects rects;
    int made, warned, w, h; /* what the rectangles are made for: a first time, with the line or not, the picture */
    int over;               /* and with the showcase or the stats, which change every frame */
    SDL_WindowID key_window; /* the window the last key was pressed in, 0 for none: a console's of two (the 3DS's) */
    u32 presents;           /* the frames presented, the showcase's frames a second */
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
/* state_save and state_load, read as they are and done by snx_vpad_window_states_due */
static const IniOpt STATE_SAVE = {.key = "state_save",
                                  .def = "",
                                  .type = INI_TEXT,
                                  .var = g_vpad_sdl.state_save,
                                  .size = sizeof g_vpad_sdl.state_save};
static const IniOpt STATE_LOAD = {.key = "state_load",
                                  .def = "",
                                  .type = INI_TEXT,
                                  .var = g_vpad_sdl.state_load,
                                  .size = sizeof g_vpad_sdl.state_load};
/* the Switch's rows whose set functions are common code, which the console table leaves out with every other one's */
static const struct {
    const char* key;
    void (*set)(const IniOpt* o, const char* v);
} COMMON_SETS[] = {{"stats_corner", stats_corner_set}, {"showcase_corner", vpad_showcase_corner_set}};
static_assert(sizeof COMMON_SETS / sizeof *COMMON_SETS == sizeof g_vpad_sdl.with_set / sizeof *g_vpad_sdl.with_set);
static const IniOpt* setting(const char* key) {
    if (!strcmp(key, "fps_limit")) return &FPS_LIMIT;
    if (!strcmp(key, "state_save")) return &STATE_SAVE;
    if (!strcmp(key, "state_load")) return &STATE_LOAD;
    const IniOpt* o = hotkey_setting(key);
    if (o) return o;
    o = snx_console_opt(g_vpad_sdl.console, key);
    for (size_t i = 0; o && i < sizeof COMMON_SETS / sizeof *COMMON_SETS; ++i)
        if (!strcmp(key, COMMON_SETS[i].key)) {
            g_vpad_sdl.with_set[i] = *o;
            g_vpad_sdl.with_set[i].set = COMMON_SETS[i].set;
            return &g_vpad_sdl.with_set[i];
        }
    return o;
}
void snx_vpad_sdl_settings(SnxSystem console) {
    g_vpad_sdl.console = console;
    vpad_settings_also(setting);
}
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
    if (g_vpad_sdl.say)
        g_vpad_sdl.say(line);
    else
        snx_error("%s", line); /* a run with no window: its log alone */
    if (g_vpad_sdl.show) g_vpad_sdl.show(line);
}
bool snx_vpad_window_paused(void) { return g_vpad_sdl.paused; }
/* a line of the save states', on say and show */
[[gnu::format(printf, 1, 2)]] static void states_say(const char* fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    say_shown(line);
}
/* a state's file by what an item names: a slot's number, auto, or a path */
static void item_path(char* out, size_t size, const SnxVpadStates* s, const char* what, size_t n) {
    size_t digits = strspn(what, "0123456789");
    if ((digits && digits == n) || (n == 4 && !strncmp(what, "auto", 4)))
        snprintf(out, size, "%sstates/%s.%.*s" SNX_STATE_EXT, s->prefix, s->name, (int)n, what);
    else
        snprintf(out, size, "%.*s", (int)n, what);
}
/* the items of list due by ms and not done (a bit each in *done) saved or loaded, said with their path */
static void due(const char* list, uint32_t* done, int load, uint64_t ms, const SnxVpadStates* s, const uint8_t* thumb) {
    char path[SNX_PATH_SIZE], why[256];
    int i = 0;
    for (const char* at = list; *at && i < 32; ++i) {
        const char *end = at + strcspn(at, ","), *colon = memchr(at, ':', (size_t)(end - at));
        char* stop;
        unsigned long long when = strtoull(at, &stop, 10);
        if (!colon || stop != colon) {
            if (!(*done >> i & 1))
                states_say("state_%s=%s: %.*s is not at:slot or at:path", load ? "load" : "save", list, (int)(end - at),
                           at);
            *done |= 1u << i;
        } else if (!(*done >> i & 1) && when <= ms) {
            *done |= 1u << i;
            item_path(path, sizeof path, s, colon + 1, (size_t)(end - colon - 1));
            if (load ? s->load(s->game, path, why, sizeof why) : s->save(s->game, path, thumb, why, sizeof why))
                states_say("Not %s: %s", load ? "loaded" : "saved", why);
            else
                states_say("%s %s", load ? "Loaded" : "Saved", path);
        }
        at = *end ? end + 1 : end;
    }
}
/* the bits of list's items, at most 32, that due() marks done */
static uint32_t items(const char* list) {
    int n = 0;
    for (const char* at = list; *at && n < 32; ++n) at += strcspn(at, ","), at += *at == ',';
    return n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1;
}
bool snx_vpad_window_states_waiting(void) {
    uint32_t saves = items(g_vpad_sdl.state_save), loads = items(g_vpad_sdl.state_load);
    return (g_vpad_sdl.saves_done & saves) != saves || (g_vpad_sdl.loads_done & loads) != loads;
}
bool snx_vpad_window_save_due(uint64_t ms) {
    int i = 0;
    for (const char* at = g_vpad_sdl.state_save; *at && i < 32; ++i) {
        char* stop;
        unsigned long long when = strtoull(at, &stop, 10);
        if (!(g_vpad_sdl.saves_done >> i & 1) && *stop == ':' && when <= ms) return true;
        at += strcspn(at, ","), at += *at == ',';
    }
    return false;
}
void snx_vpad_window_states_due(uint64_t ms, const SnxVpadStates* s, const uint8_t* thumb) {
    if (g_vpad_sdl.state_save[0]) due(g_vpad_sdl.state_save, &g_vpad_sdl.saves_done, 0, ms, s, thumb);
    if (g_vpad_sdl.state_load[0]) due(g_vpad_sdl.state_load, &g_vpad_sdl.loads_done, 1, ms, s, thumb);
}
void snx_vpad_window_states(int action, const SnxVpadStates* s, const uint8_t* thumb) {
    char name[16], label[24], start[24], path[SNX_PATH_SIZE], why[256], when[32];
    SnxStateHeader h;
    int slot = g_vpad_sdl.slot < 1 || g_vpad_sdl.slot > g_settings.save_state_slots ? 1 : g_vpad_sdl.slot;
    if (action == HOTKEY_NEXT_SLOT) slot = slot % g_settings.save_state_slots + 1;
    g_vpad_sdl.slot = slot;
    snx_state_slot_name(name, sizeof name, slot);
    snx_state_slot_label(label, sizeof label, slot, 0);
    snx_state_slot_label(start, sizeof start, slot, 1);
    snprintf(path, sizeof path, "%sstates/%s.%s" SNX_STATE_EXT, s->prefix, s->name, name);
    if (action == HOTKEY_NEXT_SLOT) {
        if (snx_state_peek(path, &h, NULL, why, sizeof why))
            states_say("Slot %d: empty", slot);
        else {
            snx_state_slot_when(when, sizeof when, &h);
            states_say("Slot %d: %s", slot, when);
        }
    } else if (action == HOTKEY_SAVE_STATE) {
        int over = snx_is_file(path);
        if (s->save(s->game, path, thumb, why, sizeof why))
            states_say("Not saved: %s", why);
        else
            states_say("Saved %s %s", over ? "over" : "to", label);
    } else if (action == HOTKEY_LOAD_STATE) {
        if (snx_state_peek(path, &h, NULL, why, sizeof why)) {
            states_say("%s is empty", start);
            return;
        }
        if (g_settings.save_state_backup) { /* the game as it was, kept to go back to */
            char back[SNX_PATH_SIZE], bwhy[256];
            snprintf(back, sizeof back, "%sstates/%s.auto" SNX_STATE_EXT, s->prefix, s->name);
            if (s->save(s->game, back, thumb, bwhy, sizeof bwhy)) states_say("No auto slot before the load: %s", bwhy);
        }
        if (s->load(s->game, path, why, sizeof why))
            states_say("Not loaded: %s", why);
        else
            states_say("Loaded %s", label);
    }
}
void snx_vpad_window_effect(int action, const char* label) {
    char text[96], line[128];
    snprintf(text, sizeof text, "%s: %s", action == HOTKEY_UPSCALER ? "Upscaler" : "Anti-aliasing", label);
    snprintf(line, sizeof line, "%s, for this run", text);
    if (g_vpad_sdl.say) g_vpad_sdl.say(line);
    if (g_vpad_sdl.show) g_vpad_sdl.show(text);
}
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
    g_vpad_sdl.key_window = k->windowID;
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
    if (a == HOTKEY_STATS || a == HOTKEY_SHOWCASE) { /* on or off for the run, as a Switch game's F12 and F6 */
        bool stats = a == HOTKEY_STATS;
        _Atomic int* setting = stats ? (_Atomic int*)&g_settings.stats : (_Atomic int*)&g_settings.showcase;
        const char* what = stats ? "Stats" : "Showcase";
        int on = !atomic_load_explicit(setting, memory_order_relaxed);
        char line[96];
        atomic_store_explicit(setting, on, memory_order_relaxed); /* the renderer's thread reads it */
        snx_key_said(line, sizeof line, what, on ? "on" : "off", on ? "off" : "on");
        if (g_vpad_sdl.say) g_vpad_sdl.say(line);
        snx_key_now(line, sizeof line, what, on ? "on" : "off");
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
    if (a == HOTKEY_FULLSCREEN || a == HOTKEY_FULLSCREEN_2) { /* the window it was pressed in, of a console's two */
        SDL_Window* w = k->windowID ? SDL_GetWindowFromID(k->windowID) : NULL;
        if (!w) w = g_vpad_sdl.window;
        char why[256];
        if (!snx_window_fullscreen(w, !(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN), why, sizeof why) &&
            g_vpad_sdl.say)
            g_vpad_sdl.say(why);
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
void snx_vpad_window_notice(const char* line) {
    if (g_vpad_sdl.show) g_vpad_sdl.show(line);
}
void snx_vpad_window_log(int warn, const char* line) {
    if (g_vpad_sdl.say)
        g_vpad_sdl.say(line);
    else
        snx_error("%s", line); /* a failure or a fact alike: there is no window to say it in */
    (void)warn;
}
uint64_t snx_vpad_window_ms(void) { return SDL_GetTicks(); }
SDL_WindowID snx_vpad_window_key_window(void) { return g_vpad_sdl.key_window; }
int snx_vpad_window_pace(void) { return g_vpad_sdl.pace; }
void snx_vpad_window_set_pace(int pace) { g_vpad_sdl.pace = pace; }
bool snx_vpad_window_point(const SDL_Event* e, float* x, float* y) {
    int w = 0, h = 0, ww = 0, wh = 0;
    /* a console's other window (the 3DS's lower screen on its own) has no controls */
    SDL_WindowID id = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.windowID
                      : e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP
                          ? e->button.windowID
                          : e->tfinger.windowID;
    if (id && id != SDL_GetWindowID(g_vpad_sdl.window)) return false;
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

/* Back's first press over the picture, in a band right under edit mode's bar (vpad.c), as the bar's own letters */
static void warning_draw(SnxUiList* l, const int safe[4], int scale) {
    char line[64];
    float bar[4];
    snx_quit_warning(line, sizeof line, "Back");
    vpad_edit_bar(safe, bar);
    float w = (float)((int)(strlen(line) + 2) * 8 * scale), h = (float)(24 * scale);
    float x = (float)safe[0] + ((float)safe[2] - w) / 2, y = bar[1] + bar[3] + (float)(4 * scale);
    ui_fill(l, x, y, w, h, UI_INK);
    ui_text(l, x + (float)(8 * scale), y + (float)(4 * scale), scale, line, UI_PAGE);
}
void snx_vpad_window_showcase(u64 keys, unsigned (*key)(int bit), const i32 sticks[4]) {
    u64 buttons = 0;
    for (int bit = 0; bit < SNX_PAD_BUTTONS; ++bit) {
        unsigned k = key(bit);
        if (k && (keys & k) == k) buttons |= 1ull << bit;
    }
    ui_lock();
    vpad_showcase_input(buttons, sticks ? sticks : (const i32[4]){0});
    ui_unlock();
}
const SnxUiRects* snx_vpad_window_rects(int w, int h) {
    int safe[4], percent;
    if (!g_vpad_sdl.window || w <= 0 || h <= 0) return NULL;
    SnxUiList l = {g_vpad_sdl.quads, 0, QUADS};
    u64 now = SDL_GetTicksNS();
    ++g_vpad_sdl.presents;
    stats_frame(now, &(SnxStatsGpu){0}); /* the GPU's numbers are the Switch's renderer's alone: shown as ? */
    snx_vpad_window_safe(safe);
    percent = snx_display_percent(SDL_GetWindowDisplayScale(g_vpad_sdl.window));
    int scale = snx_menu_scale(w, h, percent), how = vpad_draw(&l, safe, scale);
    ui_lock();
    int over = vpad_showcase_draw(&l, safe, vpad_showcase_fps(g_vpad_sdl.presents, now), scale) != 2;
    int before = l.n;
    stats_draw(&l, safe, scale, now);
    ui_unlock();
    over |= l.n > before;
    if (how == VPAD_DRAW_HIDDEN && !over && !g_vpad_sdl.over) return NULL;
    int warn = vpad_editing() && now < g_vpad_sdl.quit_by;
    if (how == VPAD_DRAW_LAID || over || g_vpad_sdl.over || !g_vpad_sdl.made || warn != g_vpad_sdl.warned ||
        w != g_vpad_sdl.w || h != g_vpad_sdl.h) {
        if (warn) warning_draw(&l, safe, scale);
        if (!snx_ui_rects(&g_vpad_sdl.rects, &l, w, h) && g_vpad_sdl.say)
            g_vpad_sdl.say("out of memory for the on-screen controls' picture: they are not drawn");
        g_vpad_sdl.made = 1, g_vpad_sdl.warned = warn, g_vpad_sdl.over = over;
        g_vpad_sdl.w = w, g_vpad_sdl.h = h;
    }
    return g_vpad_sdl.rects.count ? &g_vpad_sdl.rects : NULL;
}
void snx_vpad_window_render(SDL_Renderer* renderer) {
    int w = 0, h = 0;
    const SnxUiRects* rects = SDL_GetCurrentRenderOutputSize(renderer, &w, &h) ? snx_vpad_window_rects(w, h) : NULL;
    if (rects) snx_ui_rects_render(renderer, rects);
}
void snx_ui_rects_render(SDL_Renderer* renderer, const SnxUiRects* rects) {
    enum { AT_ONCE = 256 };
    SDL_FRect f[AT_ONCE];
    for (size_t i = 0; i < rects->count;) { /* runs of one colour */
        u32 c = rects->rect[i].rgba;
        int n = 0;
        for (; n < AT_ONCE && i < rects->count && rects->rect[i].rgba == c; ++i, ++n)
            f[n] = (SDL_FRect){rects->rect[i].x, rects->rect[i].y, rects->rect[i].w, rects->rect[i].h};
        SDL_SetRenderDrawColor(renderer, (Uint8)c, (Uint8)(c >> 8), (Uint8)(c >> 16), 255);
        SDL_RenderFillRects(renderer, f, n);
    }
}
