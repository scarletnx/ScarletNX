/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVIDIA DLSS Super Resolution, the upscalers' DLSS backend (upscaler.h, docs/layers/gpu/upscalers.md), through NVIDIA
   Streamline with its manual hooking. Only Streamline's headers (MIT) are built in, and only in a Windows build: the
   runtime itself (sl.interposer.dll, sl.common.dll, sl.dlss.dll and nvngx_dlss.dll, NVIDIA's, under NVIDIA's licences)
   is never in the program. The user downloads it from NVIDIA into a folder of their own and names that folder in a
   setting, and it is loaded from there (LoadLibrary), signed by NVIDIA, or DLSS is not available and the choice of
   upscaler goes on to the next.

   Streamline needs to know the instance and the device: it starts before the instance (snx_dlss_start), says the
   extensions and features DLSS needs (snx_dlss_instance_extensions, snx_dlss_device), takes the device once made
   (snx_dlss_attach), and wants the swapchain's calls through its own (snx_dlss_proxy) so it sees every present. Like
   the rest of the interface it knows neither the renderer nor the settings: the caller passes the folder and the
   preset, and a function for Streamline's log lines */
#ifndef SNX_NX_GPU_VK_UPSCALER_DLSS_H
#define SNX_NX_GPU_VK_UPSCALER_DLSS_H
#include <stdint.h>
#include "nx/gpu/vk/upscaler/upscaler.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SnxDlssLog)(int level, const char* text); /* level: 0 information, 1 warning, 2 error */
/* Before the Vulkan instance is made: Streamline from folder (its sl.interposer.dll, signed by NVIDIA), started for
   DLSS Super Resolution on Vulkan. preset: 0 NVIDIA's own per mode, else the DLSS preset letter's number (A = 1:
   10 J, 11 K, 12 L, 13 M). 1 when it runs, else 0 and why (an empty folder: DLSS is off). Windows only */
int snx_dlss_start(const char* folder, int preset, SnxDlssLog log, const char** why);
/* the instance extensions DLSS needs, while started: their count, and their names in *names */
uint32_t snx_dlss_instance_extensions(const char* const** names);
/* Before vkCreateDevice: DLSS's device extensions added to ext (those not there, n of cap used) and its Vulkan 1.2
   and 1.3 features to want12 and want13, when the physical device has every one (have12, have13 its features) and is
   NVIDIA's. 0 and why when it can't run there: Streamline is then stopped, and nothing added */
int snx_dlss_device(VkPhysicalDevice physical, const char** ext, uint32_t* n, uint32_t cap,
                    VkPhysicalDeviceVulkan12Features* want12, const VkPhysicalDeviceVulkan12Features* have12,
                    VkPhysicalDeviceVulkan13Features* want13, const VkPhysicalDeviceVulkan13Features* have13,
                    const char** why);
/* After vkCreateDevice, before any swapchain: the device handed to Streamline, and whether DLSS runs on it. The
   queues are the graphics and compute queues the device made (DLSS asks for none of its own). 0 and why: stopped */
int snx_dlss_attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t graphics_family,
                    uint32_t graphics_index, uint32_t compute_family, uint32_t compute_index, const char** why);
/* Streamline's own vkCreateSwapchainKHR, vkDestroySwapchainKHR, vkGetSwapchainImagesKHR, vkAcquireNextImageKHR,
   vkQueuePresentKHR and vkDeviceWaitIdle, which the caller calls in place of the driver's once attached (its present
   does Streamline's work for each frame); NULL when it isn't running */
PFN_vkVoidFunction snx_dlss_proxy(const char* name);
/* before the device is destroyed */
void snx_dlss_stop(void);

/* snx_upscaler_available's: attached and nothing failed */
int snx_dlss_available(const char** why);
typedef struct SnxDlss SnxDlss;
SnxDlss* snx_dlss_create(const SnxUpscalerInfo* info, const char** why);
/* an evaluation into e->cb (the frame's command buffer, on the thread that records it). A failure here leaves DLSS
   unavailable from then on, so the next evaluation chooses another upscaler */
int snx_dlss_evaluate(SnxDlss* d, const SnxUpscaleEval* e, const char** why);
void snx_dlss_destroy(SnxDlss* d);
/* the evaluations DLSS recorded and those it failed, since the start */
void snx_dlss_counts(uint64_t* evaluated, uint64_t* failed);
#ifdef __cplusplus
}
#endif
#endif
