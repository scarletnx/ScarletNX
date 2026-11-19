# GPU clocks

A frame keeps the GPU half idle: the GPU waits while the frame's commands are recorded, then the game waits for the
GPU. A driver that picks the GPU's clocks by how busy it is sees a GPU busy half the time and lowers them, which makes
every frame slower, which keeps the GPU no busier. `gpu_pstate` ([Settings](../../user/settings.md#performance)) holds
the clocks up while a game runs, on each system where a program without privileges can, and the log says the way taken
or why there is none (`src/common/gpu/vk/gpu_pstate.c`, called once the Switch renderer has made its device).

| Value | What it holds |
| --- | --- |
| `auto` (default) | a discrete GPU's clocks, and a phone's Adreno's; a computer's integrated GPU's are left to its driver, as it shares its power with the CPU, whose threads a held GPU clock would take |
| `on` | those too |
| `off` | nothing: the driver picks the clocks |

## Linux, AMD (amdgpu)

The runtime opens the render node of the device Vulkan made (`VK_EXT_physical_device_drm` gives its device number;
sysfs's `/sys/dev/char/MAJOR:MINOR/uevent` its name), checks its kernel driver is `amdgpu` (`DRM_IOCTL_VERSION`),
makes a context of its own (`AMDGPU_CTX_OP_ALLOC_CTX`) and sets the peak stable pstate on it
(`AMDGPU_CTX_OP_SET_STABLE_PSTATE`, `AMDGPU_CTX_STABLE_PSTATE_PEAK`). The level reads `profile_peak` until the game
ends.

- **No root.** `DRM_IOCTL_AMDGPU_CTX` is `DRM_RENDER_ALLOW` and the stable pstate checks no capability (Linux 6.18,
  `amdgpu_drv.c`, `amdgpu_ctx.c` `amdgpu_ctx_ioctl`): any process that opens the render node, as Vulkan already does,
  may set one. Linux 5.19 brought it; an older kernel answers `EINVAL`.
- **One holder.** One context in the whole system holds a stable pstate at a time: a second process (a profiler, a
  second game) gets `EBUSY`, said as a warning, and the clocks stay as the first holds them.
- **Given back by the kernel.** A context ending puts back the level it found (`amdgpu_ctx_fini`), and the node is
  closed when the process ends, by a crash or a kill too. Checked on a Radeon RX 6700 XT, Linux 6.18: the level went
  `auto`, `profile_peak` while held, `auto` again after a normal exit, a `SIGKILL` and a `SIGSEGV`.
- **Only from `auto`.** The kernel counts a level set on the PC (`power_dpm_force_performance_level`: `high`,
  `manual`, by LACT, CoreCtrl or by hand) as no stable pstate, and would put `auto` back over it when the game ends. Such
  a level is left as it is, and the log names it. Where the level does not read, `auto` holds nothing and `on` holds
  anyway.
- **Peak.** `AMDGPU_CTX_STABLE_PSTATE_PEAK` is the highest pstate the GPU sustains. Holding a profile level also keeps
  the GFX block ungated (`amdgpu_dpm.c`), so the GPU draws more power while the game runs.

## NVIDIA's driver (Linux, Windows)

The device is made with `VK_NV_low_latency2` where the driver has it (`vk_nv_low_latency2`), each swapchain with
`VkSwapchainLatencyCreateInfoNV`, and `vkSetLatencySleepModeNV` asks the boost alone: `lowLatencyMode` off, so no
sleep paces the game, and `lowLatencyBoost` on. The specification: "If lowLatencyMode is VK_FALSE, lowLatencyBoost will
still hint to the GPU to increase its power state" (`VkLatencySleepModeInfoNV`); NVAPI's documentation of the same
boost: it "will request the GPU run at max clocks even in scenarios where it is idle most of the frame and would
normally try to save power" (`nvapi.h`, `NvAPI_D3D_SetSleepMode`). A setting of the swapchain, it ends with it, and
needs no privilege. The extension requires presents with ids (`VK_KHR_present_id` with `present_wait`, or
`present_id2`), which a run without a window does not turn on: the log then says there is no boost. Not checked on an NVIDIA GPU: none is at hand; the decision and its lines are (`tests/vk_gpu_pstate.c`).

## Android, Adreno (kgsl)

kgsl, Qualcomm's kernel driver (under Qualcomm's Vulkan driver or Turnip), takes a power-level constraint per context:
`KGSL_PROP_PWR_CONSTRAINT` with `KGSL_CONSTRAINT_PWR_MAX` on a context of the runtime's own (`IOCTL_KGSL_DRAWCTXT_CREATE`
with `KGSL_CONTEXT_PWR_CONSTRAINT`), applied each time that context submits a command. A thread submits one `CP_NOP`
every 40 ms while the game has presented a frame in the last 250 ms. kgsl holds the GPU's top level while those come,
and drops the constraint once its last command retired and its interval passed, or once its context is gone
(`kgsl_pwrctrl_adjust_pwrlevel`), which the process's end takes with it.

- **No root.** The calls are the ones every Vulkan driver on kgsl makes from the app.
- **Checked on a Poco M3** (Adreno 610, Linux 4.19, Android 17), a program running as the app (`run-as`), the clock
  read from `/sys/class/kgsl/kgsl-3d0/gpuclk`: 320 MHz idle, 950 MHz (its top) while held with the GPU otherwise idle,
  320 MHz again within 0.2 s of the program's normal end, a `SIGKILL` and a `SIGSEGV`.
- **Not `KGSL_PROP_PWRCTRL`**, libadrenotools' `adrenotools_set_turbo`: it turns the whole device's frequency scaling
  off (`a6xx_setproperty` sets its `ctrl_flags` and disables `pwrscale`), state no process owns and that closing the
  device does not touch (`kgsl_release`), so a crash or a kill would leave it until something set it back.

## Other systems

No way for a program without privileges that the kernel or driver gives back when the game ends:

| System and GPU | What exists, and why it is not used |
| --- | --- |
| NVIDIA, other ways | NVML's `nvmlDeviceSetGpuLockedClocks`: "Requires root/admin permissions" (`nvml.h`). `nvidia-settings`' `GPUPowerMizerMode` (NV-CONTROL): X11 only, and it stays set after the program ends, so a crash would leave it. NVAPI's driver profile setting "Power management mode" (`PREFERRED_PSTATE_ID`): a change to the player's saved driver profiles, which outlives the game |
| Linux, nouveau (NVK) | no `VK_NV_low_latency2`; its clocks are the firmware's and debugfs', root's |
| Linux, Intel (i915, xe) | `I915_CONTEXT_PARAM_LOW_LATENCY` and `DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT` are a frequency hint for the work of the context that carries it (GuC's policy, `intel_guc_submission.c`, `xe_guc_submit.c`), which is the Vulkan driver's, not a context of the runtime's own; `gt_min_freq_mhz` is root's |
| Linux, Qualcomm (msm), Arm (panfrost, panthor) | msm's `MSM_PARAM_SYSPROF` needs `CAP_SYS_ADMIN` (`adreno_gpu.c`); panfrost's and panthor's clocks are devfreq's, root's |
| Windows, AMD | ADLX's manual tuning (`SetGPUMinFrequency`) changes the card's tuning that Adrenalin keeps until `ResetToFactory`, whatever the program does. D3D12's `SetStablePowerState` works only in Windows' developer mode, removes the device without it, and fixes clocks "significantly lower" than a game's |
| Windows, Intel | D3D12's `SetStablePowerState`, as above |
| macOS | no API sets a GPU's clocks. Game Mode (macOS 14) gives a full-screen game priority on the CPU and GPU from its bundle's category, not a held clock |
