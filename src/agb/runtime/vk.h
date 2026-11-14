/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window: the board's 240x160 frame through the upscaler and the anti-aliasing
   (common/gpu/vk/present_fx.h), as the Switch's, the 3DS's and the Vita's, at the whole step of the screen the window
   holds, and a notice over it (common/gpu/vk/notice.h). host.c keeps SDL's renderer where the host has no Vulkan
   window, so a run without one still shows its frame */
#ifndef SNX_AGB_RUNTIME_VK_H
#define SNX_AGB_RUNTIME_VK_H
#include "common/utilities/notice.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AgbVk AgbVk;
/* the window, titled title, and its Vulkan device, the settings' upscaler and anti-aliasing said; NULL with the
   reason in error: a host with no Vulkan 1.3 device, or no window */
AgbVk* agb_vk_open(const char* title, bool fullscreen, char* error, size_t error_size);
void agb_vk_close(AgbVk* vk);
/* the SDL window the events come from */
SDL_Window* agb_vk_window(AgbVk* vk);
/* The 240x160 RGBA frame on screen, notice over it while it shows: false when the window had no image to take it */
bool agb_vk_frame(AgbVk* vk, const uint32_t* framebuffer, const SnxNotice* notice);
/* the upscaler's step (which 0) or the anti-aliasing's (1) for the run, its words into text (n bytes), as the Switch
   says them ("Upscaler: AMD FSR 1") */
void agb_vk_step(AgbVk* vk, int which, char* text, size_t n);
#endif
