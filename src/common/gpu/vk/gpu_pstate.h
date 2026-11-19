/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_pstate: the GPU's clocks held up while a game runs, where its kernel driver lets a program without privileges do
   it. A game's frame keeps the GPU half idle (the GPU waits for the frame's commands, then the game waits for the GPU),
   and a driver that picks its clocks by how busy the GPU is then lowers them, which makes every frame slower.

   amdgpu (Linux): AMDGPU_CTX_OP_SET_STABLE_PSTATE on a context of the runtime's own, on a render node it opens itself.
   The kernel takes it from any process that can open the node, one context in the whole system at a time, and puts the
   level the context found back when the context ends: when the runtime's process ends, a crash or a kill too, as the
   kernel then closes the node (amdgpu_ctx.c: amdgpu_ctx_set_stable_pstate, amdgpu_ctx_fini). It only counts the
   automatic level as one to go back to, so the hold is taken only from "auto": a level set on the PC (LACT, CoreCtrl
   or by hand: power_dpm_force_performance_level) would otherwise go back to auto when the game ends. */
#ifndef SNX_COMMON_VK_GPU_PSTATE_H
#define SNX_COMMON_VK_GPU_PSTATE_H
#include <stdbool.h>
#include <stdint.h>

/* gpu_pstate's values: auto holds the clocks of a discrete GPU, on of any GPU a way exists for, off of none */
enum { SNX_GPU_PSTATE_AUTO, SNX_GPU_PSTATE_ON, SNX_GPU_PSTATE_OFF };
/* the value text names, SNX_GPU_PSTATE_*; -1 for none of auto, on and off */
int snx_gpu_pstate_mode(const char* text);

#define SNX_GPU_VENDOR_AMD 0x1002u

/* The device as Vulkan says it: its name, vendorID and deviceType (a VkPhysicalDeviceType), and its render node's
   device number where the driver gives one (VK_EXT_physical_device_drm's hasRender, renderMajor, renderMinor). sys and
   dev are the folders the node and its sysfs files are found under: "/sys" and "/dev" */
typedef struct {
    const char* name;
    uint32_t vendor;
    int type;
    int has_render;
    int64_t render_major, render_minor;
    const char *sys, *dev;
} SnxGpuPstateDevice;

/* each line said: warn for one that names what was asked of the setting and could not be done */
typedef void (*SnxGpuPstateSay)(const char* line, int warn);

/* The clocks of d held as mode asks (SNX_GPU_PSTATE_*), for the rest of the process: true where they are, or were by
   an earlier call. Every case says one line: the way taken and the level it held, or why none, with the value given
   and the error a call gave */
bool snx_gpu_pstate_hold(int mode, const SnxGpuPstateDevice* d, SnxGpuPstateSay say);
#endif
