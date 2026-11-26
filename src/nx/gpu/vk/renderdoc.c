/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Switch's RenderDoc captures (common/gpu/vk/renderdoc.h): its notice after the capture key's icon, its log, the
   clock --script counts from, and its recording ring's submit before a capture ends */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/unattended.h"
#include "common/utilities/hotkeys.h"

static void notice(const char* text) { overlay_notice(overlay_key_icon(HOTKEY_CAPTURE), text); }
static void say(int warn, const char* line) {
    if (warn)
        WARN("%s\n", line);
    else
        LOG("%s\n", line);
}
static const SnxRenderDocHost g_renderdoc_host = {
    .notice = notice, .say = say, .clock_ms = unattended_ms, .sync = vk_rec_sync};
void vk_renderdoc_load(void) { snx_renderdoc_load(&g_renderdoc_host); }
int vk_renderdoc_capture(int ask) { return snx_renderdoc_capture(ask); }
void vk_renderdoc_present(u64 present) {
    if (g_vk.instance) snx_renderdoc_present(g_vk.instance, present);
}
