/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/window.h"
#include "common/utilities/common.h"
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool snx_vk_window_open(const char* app, const char* title, int width, int height, SnxVkWindow* out, char* error,
                        size_t error_size) {
    *out = (SnxVkWindow){0};
#ifdef __APPLE__
    /* One driver, never two. The program's folders may carry MoltenVK (an app bundle's Frameworks, or the copy deployed
       beside a game's runtime), and SDL loads that one for the surface: its search reaches the bundle. volkInitialize()
       would take the system loader's driver instead, so a Mac with Homebrew's MoltenVK in the loader's ICD folders
       ends with both copies in one process, and the surface call hands one copy's MVKInstance to the other's
       vkGetInstanceProcAddr, which segfaults. Name the carried driver to SDL, and take volk's entry points from the
       library SDL will use, so the instance and the surface come from it */
    {
        char* self = snx_executable(NULL), *driver = self ? snx_vulkan_driver(self) : NULL;
        if (driver && !SDL_GetHint(SDL_HINT_VULKAN_LIBRARY)) {
            SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, driver);
            fprintf(stderr, "vulkan: the Vulkan driver at %s\n", driver);
        }
        free(self);
        free(driver);
    }
#endif
    /* a window first: a display server and a Vulkan surface make it the output */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        out->window = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    /* the loader's entry points, from the library SDL loads: the driver named above on Apple, so the instance and the
       surface come from one copy of it, and the system's loader elsewhere. SDL answers only once its video started */
#ifdef __APPLE__
    {
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
        if (!gipa) {
            if (out->window) SDL_DestroyWindow(out->window);
            out->window = NULL;
            snprintf(error, error_size, "no Vulkan loader");
            return false;
        }
        volkInitializeCustom(gipa);
    }
#else
    if (volkInitialize() != VK_SUCCESS) {
        if (out->window) SDL_DestroyWindow(out->window);
        out->window = NULL;
        snprintf(error, error_size, "no Vulkan loader");
        return false;
    }
#endif
    Uint32 extension_count = 0;
    const char* const* extensions = out->window ? SDL_Vulkan_GetInstanceExtensions(&extension_count) : NULL;
    /* VK_EXT_swapchain_colorspace is an instance extension, and the HDR image kinds are its color spaces: ask for it
       when the loader has it. The instance keeps the names during the create only */
    const char** asked = NULL;
    uint32_t asked_count = 0;
    if (extensions && extension_count) {
        asked = malloc((extension_count + 2) * sizeof *asked);
        if (asked) {
            memcpy(asked, extensions, extension_count * sizeof *asked);
            asked_count = extension_count;
            if (snx_vk_has_instance_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
                asked[asked_count++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
            extensions = asked;
        }
    }
    SnxVkInstanceInfo info = {.app_name = app,
                              .extensions = extensions,
                              .extension_count = extensions ? (uint32_t)(asked_count ? asked_count : extension_count) : 0,
                              /* No window (a headless run, SDL's dummy video driver) means no extensions from SDL,
                                 which is also where MoltenVK's portability enumeration normally comes from: ask for
                                 it anyway, or the loader answers VK_ERROR_INCOMPATIBLE_DRIVER on macOS. It is
                                 enabled only when the loader has it. */
                              .portability = true};
    VkResult made = snx_vk_instance_create(&info, &out->instance);
    free(asked);
    if (made != VK_SUCCESS) {
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

int snx_vk_window_headroom_pct(SDL_Window* window) {
    SDL_PropertiesID props = window ? SDL_GetWindowProperties(window) : 0;
    float headroom;
    if (!props) return 100;
    headroom = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
    if (!(headroom >= 1.0f)) headroom = 1.0f;
    if (headroom > 10.0f) headroom = 10.0f;
    return (int)(headroom * 100.0f + 0.5f);
}

VkExtent2D snx_vk_window_extent(const SnxVkWindow* w, VkExtent2D fallback) {
    int width = 0, height = 0;
    if (!w->window || !SDL_GetWindowSizeInPixels(w->window, &width, &height) || width <= 0 || height <= 0)
        return fallback;
    return (VkExtent2D){(uint32_t)width, (uint32_t)height};
}
