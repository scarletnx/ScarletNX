/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hints every program sets before its SDL video starts (common/utilities/sdl_desktop.h): no request that the X11
   compositor leave our windows out, which KWin turns into a black flash of the whole screen when a window closes */
#include "common/utilities/sdl_desktop.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    const char* before = SDL_GetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR);
    snx_sdl_desktop_hints();
    const char* after = SDL_GetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR);
    if (!after || strcmp(after, "0")) {
        fprintf(stderr, "SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR is %s after snx_sdl_desktop_hints (was %s), not 0\n",
                after ? after : "unset", before ? before : "unset");
        return 1;
    }
    puts("PASS SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR is 0 before any window");
    return 0;
}
