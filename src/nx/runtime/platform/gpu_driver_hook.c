/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_driver_hook.h */
#include "nx/runtime/platform/gpu_driver_hook.h"
#include <android/dlext.h>
#include <stddef.h>

#define SNX_EXPORT __attribute__((visibility("default")))

static struct {
    SnxGpuDriverHookOpen* open;
    SnxGpuDriverHookSphal* sphal;
} g_gpu_driver_hook;

SNX_EXPORT void snx_gpu_driver_hook_set(SnxGpuDriverHookOpen* open, SnxGpuDriverHookSphal* sphal) {
    g_gpu_driver_hook.open = open;
    g_gpu_driver_hook.sphal = sphal;
}

SNX_EXPORT void* android_dlopen_ext(const char* filename, int flags, const android_dlextinfo* extinfo) {
    return g_gpu_driver_hook.open ? g_gpu_driver_hook.open(filename, flags, extinfo) : NULL;
}

/* libvndksupport's, which no NDK header declares */
void* android_load_sphal_library(const char* filename, int flags);
SNX_EXPORT void* android_load_sphal_library(const char* filename, int flags) {
    return g_gpu_driver_hook.sphal ? g_gpu_driver_hook.sphal(filename, flags) : NULL;
}
