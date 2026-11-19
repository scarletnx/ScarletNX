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
   or by hand: power_dpm_force_performance_level) would otherwise go back to auto when the game ends.

   NVIDIA's driver (Linux, Windows): VK_NV_low_latency2's lowLatencyBoost on each swapchain, with its low latency mode
   off: "If lowLatencyMode is VK_FALSE, lowLatencyBoost will still hint to the GPU to increase its power state"
   (VkLatencySleepModeInfoNV), what NVAPI's documentation of the same boost calls running "at max clocks even in
   scenarios where it is idle most of the frame". A setting of the swapchain, it ends with it.

   kgsl (Qualcomm's kernel driver, Android's Adreno GPUs, under its own driver or Turnip): the top power level as the
   constraint of a context of the runtime's own (KGSL_PROP_PWR_CONSTRAINT, KGSL_CONSTRAINT_PWR_MAX), which each command
   the context submits renews: a CP_NOP every 40 ms while the game presents frames. kgsl drops the constraint once
   its last command retired and its interval passed, or once its context is gone, which the process's end takes with
   it, a crash or a kill too (kgsl_pwrctrl_adjust_pwrlevel). Not KGSL_PROP_PWRCTRL (libadrenotools' turbo), which no
   process owns: a crash would leave the GPU at its top clock. */
#ifndef SNX_COMMON_VK_GPU_PSTATE_H
#define SNX_COMMON_VK_GPU_PSTATE_H
#include <stdbool.h>
#include <stdint.h>

/* gpu_pstate's values: auto holds the clocks of a discrete GPU, on of any GPU a way exists for, off of none */
enum { SNX_GPU_PSTATE_AUTO, SNX_GPU_PSTATE_ON, SNX_GPU_PSTATE_OFF };
/* the value text names, SNX_GPU_PSTATE_*; -1 for none of auto, on and off */
int snx_gpu_pstate_mode(const char* text);

#define SNX_GPU_VENDOR_AMD      0x1002u
#define SNX_GPU_VENDOR_NVIDIA   0x10deu
#define SNX_GPU_VENDOR_QUALCOMM 0x5143u

/* The device as Vulkan says it: its name, vendorID and deviceType (a VkPhysicalDeviceType), and its render node's
   device number where the driver gives one (VK_EXT_physical_device_drm's hasRender, renderMajor, renderMinor). sys and
   dev are the folders the node and its sysfs files are found under: "/sys" and "/dev". An NVIDIA GPU's boost_lacks
   says why its device was made without VK_NV_low_latency2, NULL where it was made with it */
typedef struct {
    const char* name;
    uint32_t vendor;
    int type;
    int has_render;
    int64_t render_major, render_minor;
    const char *sys, *dev;
    const char* boost_lacks;
} SnxGpuPstateDevice;

/* mode asks d's clocks held: auto a discrete GPU's and an Adreno's (a phone's, which has no other kind), on any GPU's,
   off none */
bool snx_gpu_pstate_wanted(int mode, const SnxGpuPstateDevice* d);

/* each line said: warn for one that names what was asked of the setting and could not be done */
typedef void (*SnxGpuPstateSay)(const char* line, int warn);

/* The clocks of d held as mode asks (SNX_GPU_PSTATE_*), for the rest of the process: true where they are, or were by
   an earlier call. Every case says one line: the way taken and the level it held, or why none, with the value given
   and the error a call gave */
bool snx_gpu_pstate_hold(int mode, const SnxGpuPstateDevice* d, SnxGpuPstateSay say);

/* a frame presented: the kgsl hold is renewed for the next 250 ms */
void snx_gpu_pstate_present(void);
/* One step of the kgsl hold's keeper, which its thread takes every 40 ms with the monotonic clock: the constraint
   renewed where a frame was presented less than 250 ms before now_ns, 1; nothing to renew, or no kgsl hold, 0; the
   renewal refused, -1, said once as a warning through the hold's say, and the hold given up */
int snx_gpu_pstate_keep(uint64_t now_ns, uint64_t last_present_ns);
#endif
