/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GBA runner's Vulkan window: the board's 240x160 frame in a window, through the shared presentation effects
   (common/gpu/vk/present_fx.h): upscaling, anti aliasing and HDR, as every other frontend. The runner keeps an SDL
   renderer when the host has no Vulkan window, so a headless or driverless run still shows its frame. */
#ifndef SNX_AGB_RUNTIME_VK_H
#define SNX_AGB_RUNTIME_VK_H
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AgbVk AgbVk;
/* the window and its Vulkan device, or NULL with the reason in error: a host with no Vulkan 1.3 device, or no window */
AgbVk* agb_vk_open(const char* title, bool fullscreen, char* error, size_t error_size);
void agb_vk_close(AgbVk* vk);
/* the SDL window the events come from */
SDL_Window* agb_vk_window(AgbVk* vk);
/* The 240x160 RGBA frame on screen: false when the swapchain had no image free this frame. generated says it is frame
   interpolation's blend, which the F6 overlay counts beside the game's own frames */
bool agb_vk_frame(AgbVk* vk, const uint32_t* framebuffer, int generated);
/* the keys the player holds (AGB_KEY_), for the F6 overlay */
void agb_vk_keys(AgbVk* vk, uint16_t keys);
/* F6's overlay: the game's frames a second, the generated ones, their total, the frame time and the held keys. The
   state after */
int agb_vk_stats_toggle(AgbVk* vk);
/* the live hotkeys, as every other frontend's: the upscaler and the anti aliasing cycle, HDR toggles. The changed
   value, or -1 with no effects */
int agb_vk_cycle_upscaler(AgbVk* vk);
int agb_vk_cycle_aa(AgbVk* vk);
int agb_vk_toggle_hdr(AgbVk* vk);
/* a message over the frame, for a couple of seconds */
void agb_vk_toast(AgbVk* vk, const char* text);
#endif
