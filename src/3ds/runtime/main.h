/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS driver (main.c): scarletnx-3ds's own program, or a name of scarletnx's (src/multicall) */
#ifndef SNX_CTR_MAIN_H
#define SNX_CTR_MAIN_H

#include <stdbool.h>
#include <string.h>

int snx_ctr_main(int argc, char** argv); /* extracts, recompiles and plays what the arguments say; its exit status */
/* Whether the last part of path (".exe" dropped) is a 3DS application's title ID, 0004 and 12 more hex digits, as the
   game's folder and its copy of the runtime are named: a Switch title ID starts 01 */
bool snx_ctr_title_name(const char* path);
/* Whether key is one of the Switch's settings the 3DS driver takes too, with its meaning, its row and its key (the
   Display, Graphics, Debug, Performance, Controllers, On-screen controls and Shortcuts tables, g_display_ini,
   g_graphics_ini, g_debug_ini, g_performance_ini, g_gamepad_ini, g_gamepad_map_ini, g_backend_ini and g_hotkeys_ini in
   src/nx/runtime/settings.txt), as
   --key=value beside the 3DS tables' own: the launcher passes them to a 3DS game's run with those */
static inline bool snx_ctr_shared_setting(const char* key) {
    return !strcmp(key, "vk_memory") || !strcmp(key, "async_pipelines") || !strcmp(key, "pipeline_threads") ||
           !strcmp(key, "gamepad_swap_ab") || !strcmp(key, "gamepad_deadzone") || !strcmp(key, "gamepad_map") ||
           !strcmp(key, "touch") || !strcmp(key, "vsync") || !strcmp(key, "fullscreen") || !strcmp(key, "anisotropy") ||
           !strcmp(key, "fps_limit") || !strcmp(key, "vk_pipeline_cache") || !strcmp(key, "vulkan_ban_ignore") ||
           !strcmp(key, "upscaler") || !strcmp(key, "upscaler_sharpness") || !strcmp(key, "anti_aliasing") ||
           !strcmp(key, "hotkey_upscaler") || !strcmp(key, "hotkey_anti_aliasing");
}

#endif
