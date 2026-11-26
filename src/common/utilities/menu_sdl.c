/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/menu_sdl.h"
#include "common/utilities/hotkeys.h"
#include "common/utilities/overlay.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/vpad.h"
#include "common/utilities/vpad_sdl.h"

/* action: the key's runtime key (hotkeys.h), -1 for none */
int snx_menu_event(const SnxMenuWindow* m, const SDL_Event* e, int action) {
    int w, h;
    float d;
    /* the Shortcuts page's wait: any key is the one it waits for, the menu's too */
    if (e->type == SDL_EVENT_KEY_DOWN && overlay_shortcut_listening()) {
        if (!e->key.repeat) overlay_shortcut_key(e->key.key, hotkey_mods(e->key.mod));
        return 1;
    }
    if ((e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && (e->key.key == SDLK_AC_BACK || action == HOTKEY_MENU)) ||
        (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
        if (vpad_editing())
            vpad_edit(0);
        else
            m->open(!overlay_shown());
        return 1;
    }
    if (vpad_editing() && e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
        vpad_edit(0);
        return 1;
    }
    if (!overlay_shown() && m->dialog_shown && m->dialog_shown()) return m->dialog_event(e, action);
    if (!overlay_shown() && !overlay_keyboard_shown()) return 0;
    if (!overlay_shown() && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT))
        return m->keyboard_event ? m->keyboard_event(e, action) : 0;
    if (e->type == SDL_EVENT_KEY_DOWN && action == HOTKEY_HELP) { /* its page of keys, or from there the game again */
        if (!e->key.repeat) m->act(overlay_keys());
    } else if (e->type == SDL_EVENT_KEY_DOWN) {
        switch (e->key.key) {
            case SDLK_UP: overlay_move(-1); break;
            case SDLK_DOWN: overlay_move(1); break;
            case SDLK_LEFT: m->act(e->key.repeat ? overlay_side_held(-1) : overlay_side(-1)); break;
            case SDLK_RIGHT: m->act(e->key.repeat ? overlay_side_held(1) : overlay_side(1)); break;
            case SDLK_RETURN:
            case SDLK_SPACE:
                if (!e->key.repeat) m->act(overlay_select());
                break;
            case SDLK_ESCAPE:
                if (!e->key.repeat) m->act(overlay_back());
                break;
            case SDLK_BACKSPACE: /* back, and on the Shortcuts page a key's default */
                if (!e->key.repeat) m->act(overlay_backspace());
                break;
            case SDLK_DELETE: /* a setting set for this game alone back to Global */
                if (!e->key.repeat) m->act(overlay_delete());
                break;
            default: /* F1's page goes back to the game on any key */
                if (!e->key.repeat) m->act(overlay_other());
                break;
        }
    } else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        /* the pads' presses act on the menu from the poll, with their sticks and the on-screen controls'; the button
           map page waiting for one takes it as the control it is */
        overlay_press(snx_pad_of_button(e->gbutton.button));
    } else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && overlay_listening() && e->gaxis.value > SNX_PAD_TRIGGER &&
               (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        overlay_press(e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SNX_PAD_LT : SNX_PAD_RT);
    } else if (e->type == SDL_EVENT_MOUSE_WHEEL) { /* a notch up is a row up, as the system turns the wheel */
        if (overlay_shown()) overlay_scroll(-e->wheel.integer_y);
    } else if (e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_FINGER_MOTION ||
               e->type == SDL_EVENT_FINGER_UP) {
        /* a finger on an on-screen control stays the controls': their presses move the menu */
        int safe[4];
        float x, y;
        m->size(&w, &h);
        m->safe(safe);
        x = e->tfinger.x * (float)w;
        y = e->tfinger.y * (float)h;
        if (e->type == SDL_EVENT_FINGER_DOWN)
            overlay_pointer_down(snx_vpad_sdl_finger(&e->tfinger), x, y, safe);
        else if (e->type == SDL_EVENT_FINGER_MOTION)
            overlay_pointer_move(snx_vpad_sdl_finger(&e->tfinger), x, y, safe);
        else
            m->act(overlay_pointer_up(snx_vpad_sdl_finger(&e->tfinger), x, y, w, h));
    } else if (((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                e->button.which != SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT) ||
               (e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which != SDL_TOUCH_MOUSEID)) {
        /* the mouse's left button as one more finger, as touch_mouse makes it while the menu is closed */
        int safe[4];
        float x = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x,
              y = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y;
        m->size(&w, &h);
        m->safe(safe);
        d = SDL_GetWindowPixelDensity(m->window());
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            overlay_pointer_down(SNX_TOUCH_MOUSE, x * d, y * d, safe);
        else if (e->type == SDL_EVENT_MOUSE_MOTION)
            overlay_pointer_move(SNX_TOUCH_MOUSE, x * d, y * d, safe);
        else
            m->act(overlay_pointer_up(SNX_TOUCH_MOUSE, x * d, y * d, w, h));
    } else if (e->type == SDL_EVENT_FINGER_CANCELED) {
        overlay_pointer_cancel(snx_vpad_sdl_finger(&e->tfinger));
        return 0; /* the game's touch screen lets it go too */
    } else if (e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
               e->type != SDL_EVENT_MOUSE_BUTTON_UP && e->type != SDL_EVENT_MOUSE_MOTION)
        return 0; /* the window's events, and keys let go: the game still has them */
    return 1;
}
