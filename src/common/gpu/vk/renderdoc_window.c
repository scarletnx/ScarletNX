/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc's captures in a console's window (renderdoc.h): its lines in the window's notice and log, its frames' times
   on the run's clock */
#include "common/gpu/vk/renderdoc.h"
#include "common/utilities/vpad_sdl.h"

static const SnxRenderDocHost WINDOW = {
    .notice = snx_vpad_window_notice, .say = snx_vpad_window_log, .clock_ms = snx_vpad_window_ms};
void snx_renderdoc_load_window(void) { snx_renderdoc_load(&WINDOW); }
