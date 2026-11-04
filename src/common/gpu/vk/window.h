/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_COMMON_VK_WINDOW_H
#define SNX_COMMON_VK_WINDOW_H
#include "common/gpu/vk/vk_common.h"
#include <SDL3/SDL.h>
/* A frontend's SDL3 window with its Vulkan instance and surface, as the 3DS's and the Vita's open them */
typedef struct {
    SDL_Window* window; /* NULL without a display (headless): frames go elsewhere */
    SnxVkInstance instance;
    VkSurfaceKHR surface; /* the window's; VK_NULL_HANDLE without one */
} SnxVkWindow;
/* The Vulkan loader (volk), a resizable window of title and width by height when SDL's video starts, an instance of
   app with the window's extensions (without them, and no window, when that fails) and the window's surface. False
   with the reason in error when there is no loader or no instance, with nothing left open */
bool snx_vk_window_open(const char* app, const char* title, int width, int height, SnxVkWindow* out, char* error,
                        size_t error_size);
/* the surface, instance and window gone; the device made on them is already */
void snx_vk_window_close(SnxVkWindow* w);
/* the window's size in pixels, the fallback's when it has none */
VkExtent2D snx_vk_window_extent(const SnxVkWindow* w, VkExtent2D fallback);
/* The display's HDR headroom over its SDR white, in percent, from SDL's window properties: 100 with none, or with no
   window. Wayland's color management (KWin), Windows' Advanced Color and Apple's EDR report it; 100 when the backend
   does not, and the caller keeps its own bound (hdr_peak) */
int snx_vk_window_headroom_pct(SDL_Window* window);
/* the refresh rate of the display the window is on, in whole Hz, 0 when it is not known */
int snx_vk_window_refresh_hz(SDL_Window* window);
/* The display's own SDR white in nits where it can be read without a window (KWin's sdrBrightness in its output
   config, the compositor this runs under on Linux), 0 when it cannot */
int snx_vk_display_white_nits(void);
/* The paper white: the display's own SDR white where it can be read (KWin's), else the setting's value, else BT.2408's
   203 */
int snx_vk_paper_nits(int setting);
/* The display's own peak, in nits, where it can be read (KWin's maxPeakBrightnessOverride), else the setting's value
   (clamped to 400..4000) */
int snx_vk_peak_nits(int setting);
/* The debug setting's forced headroom in percent (hdr_debug_headroom: the SDR white at half the peak, 200 %), or 0
   when it is off and the display's own report stands */
int snx_vk_hdr_headroom_pct(void);
#endif
