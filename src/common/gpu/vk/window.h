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
#endif
