/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_3DS_SDL_EVENTS_H
#define SNX_TEST_3DS_SDL_EVENTS_H
#include "3ds/gpu/present.h"
#include <SDL3/SDL.h>
#include <assert.h>

/* a mouse event of a window, pushed and pumped as the presenter's loop pumps them */
static inline void event_mouse(Uint32 type, SDL_WindowID id, float x, float y) {
    SDL_Event e = {0};
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.windowID = id;
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.windowID = id;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = y;
    }
    assert(SDL_PushEvent(&e));
    assert(!snx_ctr_present_closed()); /* pumps the queue */
}
#endif
