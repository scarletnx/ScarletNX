/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/pad_menu.h"
#include "common/utilities/pad_sdl.h"
#include <stdio.h>
#include <string.h>

bool gui_pad_key(int control, const int labels[4], SDL_Keycode* key, SDL_Keymod* mod, bool* repeats) {
    *mod = SDL_KMOD_NONE;
    *repeats = false;
    switch (control) {
        case SNX_PAD_SOUTH:
        case SNX_PAD_EAST: {
            int label = labels[control];
            bool select = label == SNX_PAD_LABEL_A || label == SNX_PAD_LABEL_CROSS;
            if (!select && label != SNX_PAD_LABEL_B && label != SNX_PAD_LABEL_CIRCLE)
                select = control == SNX_PAD_SOUTH; /* a face SDL has no labels for: south selects, as most pads' */
            *key = select ? SDLK_RETURN : SDLK_ESCAPE;
            return true;
        }
        case SNX_PAD_WEST:
        case SNX_PAD_NORTH: { /* as their labels, X or Square, Y or Triangle, which a Switch pad has the other way */
            int label = labels[control];
            bool x = label == SNX_PAD_LABEL_X || label == SNX_PAD_LABEL_SQUARE;
            if (!x && label != SNX_PAD_LABEL_Y && label != SNX_PAD_LABEL_TRIANGLE) x = control == SNX_PAD_WEST;
            *key = x ? SDLK_DELETE : SDLK_R;
            return true;
        }
        case SNX_PAD_LB:
            *key = SDLK_TAB;
            *mod = SDL_KMOD_LSHIFT;
            return true;
        case SNX_PAD_RB: *key = SDLK_TAB; return true;
        case SNX_PAD_LS: *key = SDLK_M; return true;          /* Games' mods folder */
        case SNX_PAD_RS: *key = SDLK_B; return true;          /* and its AOT pack */
        case GUI_PAD_RIGHT_STICK: *key = SDLK_L; return true; /* and its Play */
        case SNX_PAD_START: *key = SDLK_F5; return true;
#ifdef SDL_PLATFORM_ANDROID
        /* a controller's Back key is SDL's BACK while a pad is open, and Android's back once it is not */
        case SNX_PAD_BACK: *key = SDLK_ESCAPE; return true;
#else
        case SNX_PAD_BACK: *key = SDLK_F1; return true; /* F1's box of keys */
#endif
        case SNX_PAD_LT:
        case SNX_PAD_RT: *key = control == SNX_PAD_LT ? SDLK_PAGEUP : SDLK_PAGEDOWN; break;
        case SNX_PAD_UP: *key = SDLK_UP; break;
        case SNX_PAD_DOWN: *key = SDLK_DOWN; break;
        case SNX_PAD_LEFT: *key = SDLK_LEFT; break;
        case SNX_PAD_RIGHT: *key = SDLK_RIGHT; break;
        default: return false;
    }
    *repeats = true;
    return true;
}
/* the pad open, pads' index of it, -1 when it is not */
static int pad_index(const GuiPads* p, SDL_JoystickID id) {
    int i;
    for (i = 0; i < p->count; ++i)
        if (SDL_GetGamepadID(p->pads[i]) == id) return i;
    return -1;
}
static void pad_open(GuiPads* p, SDL_JoystickID id) {
    if (p->count < GUI_PADS_MAX && pad_index(p, id) < 0 && (p->pads[p->count] = SDL_OpenGamepad(id)) != NULL)
        ++p->count;
}
bool gui_pads_start(GuiPads* p) {
    SDL_JoystickID* ids;
    int i, n = 0;
    if (p->started) return true;
    memset(p, 0, sizeof *p);
    p->held = -1;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
    p->started = true;
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n; ++i) pad_open(p, ids[i]);
    SDL_free(ids);
    return true;
}
void gui_pads_stop(GuiPads* p) {
    if (!p->started) return;
    while (p->count) SDL_CloseGamepad(p->pads[--p->count]);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    memset(p, 0, sizeof *p);
    p->held = -1;
}
static bool key_down(SDL_Keycode code, SDL_Keymod mod, bool repeat, Uint64 now_ms, SDL_Event* key) {
    SDL_zerop(key);
    key->type = SDL_EVENT_KEY_DOWN;
    key->key.timestamp = SDL_MS_TO_NS(now_ms);
    key->key.which = GUI_PAD_KEYBOARD;
    key->key.key = code;
    key->key.mod = mod;
    key->key.down = true;
    key->key.repeat = repeat;
    return true;
}
/* control of pad pressed: its key down, and held when it repeats */
static bool press(GuiPads* p, SDL_Gamepad* pad, int control, Uint64 now_ms, SDL_Event* key) {
    int labels[4];
    SDL_Keycode code;
    SDL_Keymod mod;
    bool repeats;
    snx_pad_labels(pad, labels);
    memcpy(p->labels, labels, sizeof labels);
    if (!gui_pad_key(control, labels, &code, &mod, &repeats)) return false;
    if (repeats) {
        p->held = control;
        p->key = code;
        p->next_ms = now_ms + GUI_PAD_DELAY_MS;
    }
    return key_down(code, mod, false, now_ms, key);
}
static void release(GuiPads* p, int control) {
    if (p->held == control) p->held = -1;
}
/* an axis' control past its way or back: pressed as it goes past */
static bool axis_control(GuiPads* p, SDL_Gamepad* pad, int control, bool on, Uint64 now_ms, SDL_Event* key) {
    unsigned bit = 1u << control;
    bool was = (p->axes & bit) != 0;
    p->axes = on ? p->axes | bit : p->axes & ~bit;
    if (!on && was) release(p, control);
    return on && !was && press(p, pad, control, now_ms, key);
}
bool gui_pads_event(GuiPads* p, const SDL_Event* e, Uint64 now_ms, SDL_Event* key) {
    int i;
    if (!p->started) return false;
    switch (e->type) {
        case SDL_EVENT_GAMEPAD_ADDED: pad_open(p, e->gdevice.which); return false;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if ((i = pad_index(p, e->gdevice.which)) >= 0) {
                SDL_CloseGamepad(p->pads[i]);
                p->pads[i] = p->pads[--p->count];
                p->held = -1;
                p->axes = 0;
            }
            return false;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            i = pad_index(p, e->gbutton.which);
            return i >= 0 && press(p, p->pads[i], snx_pad_of_button(e->gbutton.button), now_ms, key);
        case SDL_EVENT_GAMEPAD_BUTTON_UP: release(p, snx_pad_of_button(e->gbutton.button)); return false;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const SDL_GamepadAxisEvent* a = &e->gaxis;
            if ((i = pad_index(p, a->which)) < 0) return false;
            switch (a->axis) {
                case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
                    return axis_control(p, p->pads[i], SNX_PAD_LT, a->value > SNX_PAD_TRIGGER, now_ms, key);
                case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
                    return axis_control(p, p->pads[i], SNX_PAD_RT, a->value > SNX_PAD_TRIGGER, now_ms, key);
                case SDL_GAMEPAD_AXIS_LEFTX:
                case SDL_GAMEPAD_AXIS_LEFTY: {
                    bool y = a->axis == SDL_GAMEPAD_AXIS_LEFTY;
                    int less = y ? SNX_PAD_UP : SNX_PAD_LEFT, more = y ? SNX_PAD_DOWN : SNX_PAD_RIGHT;
                    /* both sides looked at: the one let go first, so a flick across lets the other press */
                    bool pressed = axis_control(p, p->pads[i], less, a->value < -GUI_PAD_STICK, now_ms, key);
                    return axis_control(p, p->pads[i], more, a->value > GUI_PAD_STICK, now_ms, key) || pressed;
                }
                case SDL_GAMEPAD_AXIS_RIGHTX:
                case SDL_GAMEPAD_AXIS_RIGHTY: { /* any way of either axis: a press as it goes past, one until both are back */
                    SDL_GamepadAxis other =
                        a->axis == SDL_GAMEPAD_AXIS_RIGHTX ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_RIGHTX;
                    bool on = SDL_abs(a->value) > GUI_PAD_STICK ||
                              SDL_abs(SDL_GetGamepadAxis(p->pads[i], other)) > GUI_PAD_STICK;
                    return axis_control(p, p->pads[i], GUI_PAD_RIGHT_STICK, on, now_ms, key);
                }
                default: return false;
            }
        }
        default: return false;
    }
}
bool gui_pads_repeat(GuiPads* p, Uint64 now_ms, SDL_Event* key) {
    if (!p->started || p->held < 0 || now_ms < p->next_ms) return false;
    p->next_ms = now_ms + GUI_PAD_RATE_MS;
    return key_down(p->key, SDL_KMOD_NONE, true, now_ms, key);
}
/* the name labels give the face button the foot calls word (A, B, X or Y), word itself when they are not
   PlayStation's */
static const char* face_name(const int labels[4], const char* word, size_t n) {
    static const char* const LETTERS[4] = {"A", "B", "X", "Y"};
    static const char* const SHAPES[4] = {"Cross", "Circle", "Square", "Triangle"};
    int i;
    bool shapes = false;
    for (i = 0; i < 4; ++i) shapes |= labels[i] >= SNX_PAD_LABEL_CROSS;
    for (i = 0; shapes && i < 4; ++i)
        if (strlen(LETTERS[i]) == n && !strncmp(word, LETTERS[i], n)) return SHAPES[i];
    return NULL;
}
void gui_pad_keys_named(const char* keys, const int labels[4], char* out, size_t size) {
    size_t o = 0, n;
    const char *p = keys, *end, *colon, *word;
    if (!size) return;
    while (*p && o + 1 < size) {
        while ((*p == ' ' || *p == '~') && o + 1 < size) out[o++] = *p++; /* ~: a sentence greyed */
        end = strstr(p, ". ");
        end = end ? end + 2 : p + strlen(p);
        colon = strstr(p, ": ");
        if (!colon || colon >= end) colon = p; /* no keys: the sentence as it is */
        for (word = p; word < colon;) {        /* the keys, a word between slashes each */
            const char* name;
            for (n = 0; word + n < colon && word[n] != '/'; ++n) continue;
            name = face_name(labels, word, n);
            o += (size_t)snprintf(out + o, size - o, "%.*s", (int)(name ? strlen(name) : n), name ? name : word);
            if (o >= size) o = size - 1;
            word += n;
            if (word < colon) { /* the slash, left out when out is full: past it either way */
                if (o + 1 < size) out[o++] = *word;
                ++word;
            }
        }
        o += (size_t)snprintf(out + o, size - o, "%.*s", (int)(end - colon), colon);
        if (o >= size) o = size - 1;
        p = end;
    }
    out[o] = 0;
}
