/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vulkan driver a run asked for: gpu_driver, the folder of a package the launcher added (common/utilities/
   gpu_driver.h), which libadrenotools loads on Android arm64 in place of the system's, and freedreno_env, Freedreno's
   variables, set for any driver that reads them. A package that does not load, or where none can, ends the run saying
   why: no other driver takes its place */
#ifndef SNX_GPU_DRIVER_LOAD_H
#define SNX_GPU_DRIVER_LOAD_H

typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
/* Freedreno's variables set, then the package's vkGetInstanceProcAddr: NULL for the system's driver (gpu_driver
   empty). Once per run; the later calls return what the first did */
void* gpu_driver_open(void);
/* after vkCreateInstance, which opened the driver: the run ends when no library of the package's folder is mapped (its
   hook, finding the package unloadable, opens the system's driver instead and says so only to the system log) */
void gpu_driver_check(void);
/* 1 while a package's driver runs */
int gpu_driver_custom(void);
/* the window's surface made through the package's driver: SDL's own would ask the system's loader for it. window: the
   ANativeWindow. 1, else 0 with the Vulkan error logged */
int gpu_driver_surface(VkInstance instance, void* window, VkSurfaceKHR* surface);
#endif
