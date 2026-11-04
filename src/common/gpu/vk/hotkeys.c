/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/hotkeys.h"
#include "common/gpu/vk/present_fx.h"
#include "common/utilities/keybind.h"
#include "snx_settings.h"
#include <stdio.h>

int snx_hotkey_match(const SDL_KeyboardEvent* e) {
    SnxKeybind bind;
    if (!e) return SNX_HOTKEY_NONE;
    if (snx_keybind_parse(g_settings.upscaler_cycle_key, &bind) && snx_keybind_match(&bind, e)) return SNX_HOTKEY_UPSCALER;
    if (snx_keybind_parse(g_settings.anti_aliasing_key, &bind) && snx_keybind_match(&bind, e)) return SNX_HOTKEY_AA;
    if (snx_keybind_parse(g_settings.hdr_key, &bind) && snx_keybind_match(&bind, e)) return SNX_HOTKEY_HDR;
    if (snx_keybind_parse(g_settings.frame_interpolation_key, &bind) && snx_keybind_match(&bind, e))
        return SNX_HOTKEY_INTERP;
    return SNX_HOTKEY_NONE;
}

const char* snx_hotkey_text(int action, int value, char* buffer, size_t size) {
    const char* name;
    const char* label;
    switch (action) {
        case SNX_HOTKEY_UPSCALER: name = "Upscaler"; label = snx_fx_upscale_label(value); break;
        case SNX_HOTKEY_AA: name = "Anti aliasing"; label = snx_fx_aa_label(value); break;
        case SNX_HOTKEY_HDR: name = "HDR"; label = snx_fx_hdr_label(value); break;
        case SNX_HOTKEY_INTERP: name = "Frame interpolation"; label = snx_fx_interp_label(value); break;
        default: buffer[0] = 0; return buffer;
    }
    snprintf(buffer, size, "%s: %s", name, label);
    return buffer;
}

const char* snx_hotkey_key_name(int action) {
    switch (action) {
        case SNX_HOTKEY_UPSCALER: return g_settings.upscaler_cycle_key;
        case SNX_HOTKEY_AA: return g_settings.anti_aliasing_key;
        case SNX_HOTKEY_HDR: return g_settings.hdr_key;
        case SNX_HOTKEY_INTERP: return g_settings.frame_interpolation_key;
        default: return "";
    }
}

int snx_hotkey_next_interp(void) {
    int mode = (snx_fx_interp_parse(g_settings.frame_interpolation) + 1) % SNX_FX_INTERP_COUNT;
    snprintf(g_settings.frame_interpolation, sizeof g_settings.frame_interpolation, "%s", snx_fx_interp_key(mode));
    return mode;
}
