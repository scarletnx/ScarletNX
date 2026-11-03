/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/window.h"
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>

bool snx_vk_window_open(const char* app, const char* title, int width, int height, SnxVkWindow* out, char* error,
                        size_t error_size) {
    *out = (SnxVkWindow){0};
    if (volkInitialize() != VK_SUCCESS) {
        snprintf(error, error_size, "no Vulkan loader");
        return false;
    }
    /* a window first: a display server and a Vulkan surface make it the output */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        out->window = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    Uint32 extension_count = 0;
    const char* const* extensions = out->window ? SDL_Vulkan_GetInstanceExtensions(&extension_count) : NULL;
    SnxVkInstanceInfo info = {.app_name = app,
                              .extensions = extensions,
                              .extension_count = extensions ? extension_count : 0,
                              /* No window (a headless run, SDL's dummy video driver) means no extensions from SDL,
                                 which is also where MoltenVK's portability enumeration normally comes from: ask for
                                 it anyway, or the loader answers VK_ERROR_INCOMPATIBLE_DRIVER on macOS. It is
                                 enabled only when the loader has it. */
                              .portability = true};
    if (snx_vk_instance_create(&info, &out->instance) != VK_SUCCESS) {
        if (out->window) SDL_DestroyWindow(out->window);
        out->window = NULL;
        info.extensions = NULL;
        info.extension_count = 0;
        if (snx_vk_instance_create(&info, &out->instance) != VK_SUCCESS) {
            snprintf(error, error_size, "no Vulkan 1.3 instance");
            return false;
        }
    }
    if (out->window && !SDL_Vulkan_CreateSurface(out->window, out->instance.instance, NULL, &out->surface)) {
        SDL_DestroyWindow(out->window);
        out->window = NULL;
        out->surface = VK_NULL_HANDLE;
    }
    return true;
}

void snx_vk_window_close(SnxVkWindow* w) {
    if (w->surface) vkDestroySurfaceKHR(w->instance.instance, w->surface, NULL);
    snx_vk_instance_free(&w->instance);
    if (w->window) SDL_DestroyWindow(w->window);
    *w = (SnxVkWindow){0};
}

VkExtent2D snx_vk_window_extent(const SnxVkWindow* w, VkExtent2D fallback) {
    int width = 0, height = 0;
    if (!w->window || !SDL_GetWindowSizeInPixels(w->window, &width, &height) || width <= 0 || height <= 0)
        return fallback;
    return (VkExtent2D){(uint32_t)width, (uint32_t)height};
}
