/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A GPU driver package (common/utilities/gpu_driver.h) loaded on Android in place of the system's Vulkan driver, for
   gpu_driver_load.c. Android's Vulkan loader opens the driver the system names and takes no other, so a run opens a
   second copy of that loader for itself, in a linker namespace where the two functions it opens its driver with are
   this file's (gpu_driver_hook.h): asked for the system's driver, they open the package's library, in a namespace
   that looks in the package's folder first. The linker's namespaces have no public API: its own functions for them
   are found in the linker itself, by the address the kernel gave the process (AT_BASE) and the linker's symbol table.
   After libadrenotools and liblinkernsbypass (BSD-2-Clause, Billy Laws), the model only: docs/CREDITS.md */
#ifndef SNX_GPU_DRIVER_ANDROID_H
#define SNX_GPU_DRIVER_ANDROID_H
#include <stddef.h>

/* this run's copy of the Vulkan loader, whose driver is to be library in the package's folder dir (a '/' at its end):
   its handle, to ask vkGetInstanceProcAddr of; NULL with why. hooks: the app's libraries folder, where
   SNX_GPU_DRIVER_HOOK_LIBRARY is. tmp: a folder to keep the copy's file in, on an Android whose kernel may have no
   memory files (9); NULL to keep it in one. Once a run */
void* gpu_driver_android_open(const char* hooks, const char* dir, const char* library, const char* tmp, char* why,
                              size_t size);
/* after the copy made a Vulkan instance, which is when it opens its driver: 1 with what saying the driver it asked for
   and the package's library opened in its place; 0 with what saying why that library is not the driver, which the
   system's then is. The linker answers a name with a library it has under that name, so 1 is the linker's word: that
   a file of the package's folder is mapped is the caller's to check (gpu_driver_check) */
int gpu_driver_android_loaded(char* what, size_t size);
#endif
