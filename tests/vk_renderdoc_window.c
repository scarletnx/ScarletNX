/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc's captures in a console's window (common/gpu/vk/renderdoc_window.c): with RenderDoc not in the process, the
   window's capture key is said in the window's log, as the Switch's is in its own, and nothing is captured */
#include "common/gpu/vk/renderdoc.h"
#include "common/utilities/vpad_sdl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char g_said[512], g_shown[256];
static void said(const char* line) { snprintf(g_said, sizeof g_said, "%s", line); }
static void shown(const char* line) { snprintf(g_shown, sizeof g_shown, "%s", line); }
int main(void) {
    snx_vpad_window_open(NULL, NULL, said, shown);
    g_said[0] = 0; /* the controls' own line as the window opens */
    snx_renderdoc_load_window();
    assert(!g_said[0] && !g_shown[0]); /* none asked for, none in the process: nothing said */
    assert(!snx_renderdoc_capture(0) && !g_said[0]);
    assert(!snx_renderdoc_capture(1));
    assert(!strcmp(g_said, "renderdoc: no frame is captured: RenderDoc isn't in the process (renderdoc= is empty, and "
                           "RenderDoc's program didn't start the game)") &&
           !g_shown[0]);
    snx_renderdoc_present(VK_NULL_HANDLE, 1); /* no instance, nothing loaded: nothing */
    snx_vpad_window_close();
    puts("vk_renderdoc_window: a console's capture key said in its window's log");
    return 0;
}
