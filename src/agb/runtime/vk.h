/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window, as the other consoles' windows present: the board's 240x160 frame at the largest
   whole step of the screen the window holds, centred on black in sharp pixels, where SDL's renderer puts it (host.c),
   and over it a notice (common/gpu/vk/notice.h) and the on-screen controls (common/gpu/vk/rects.h). host.c keeps SDL's renderer where the host has no Vulkan window, so a
   run without one still shows its frame */
#ifndef SNX_AGB_RUNTIME_VK_H
#define SNX_AGB_RUNTIME_VK_H
#include "common/utilities/notice.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AgbVk AgbVk;
/* the window, titled title, width by height pixels and fullscreen or not, and its Vulkan device; NULL with the reason
   in error: a host with no Vulkan 1.3 device that presents to it, or no window */
AgbVk* agb_vk_open(const char* title, int width, int height, bool fullscreen, char* error, size_t error_size);
void agb_vk_close(AgbVk* vk);
/* the SDL window the events come from */
SDL_Window* agb_vk_window(AgbVk* vk);
/* where the screen goes in a window w by h pixels, its left, top, width and height into rect: the largest whole step
   of it that fits, centred, as SDL_LOGICAL_PRESENTATION_INTEGER_SCALE places it; a window smaller than the screen
   holds it shrunk to fit, its aspect kept, where SDL's renderer shows its middle at 1x */
void agb_vk_place(uint32_t w, uint32_t h, int32_t rect[4]);
/* The 240x160 RGBA frame on screen, notice over it while it shows, and the on-screen controls: false when the window
   had no image to take it */
bool agb_vk_frame(AgbVk* vk, const uint32_t* framebuffer, const SnxNotice* notice);
#endif
