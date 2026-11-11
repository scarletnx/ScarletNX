/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The one library a GPU driver package's loader (gpu_driver_android.h) adds to the app: the two functions Android's
   Vulkan loader opens its driver with, under their own names, for the copy of that loader a run makes to find before
   libdl's and libvndksupport's. It is linked -z global, and Android's linker looks every name up in such a library
   first for whatever loads in its namespace afterwards: so it is a library of its own and exports nothing else, where
   the runtime's own library, with those two names, would answer for every library the app loads. It asks for neither
   name itself, which it would get its own for: it hands each call to the runtime, which calls the system's */
#ifndef SNX_GPU_DRIVER_HOOK_H
#define SNX_GPU_DRIVER_HOOK_H

#define SNX_GPU_DRIVER_HOOK_LIBRARY "libscarletnx-gpu-hook.so"
#define SNX_GPU_DRIVER_HOOK_SET     "snx_gpu_driver_hook_set"
/* android_dlopen_ext's and android_load_sphal_library's calls as the runtime takes them; extinfo is an
   android_dlextinfo */
typedef void* SnxGpuDriverHookOpen(const char* filename, int flags, const void* extinfo);
typedef void* SnxGpuDriverHookSphal(const char* filename, int flags);
/* who takes the two calls from now on. Until it is called they open nothing */
void snx_gpu_driver_hook_set(SnxGpuDriverHookOpen* open, SnxGpuDriverHookSphal* sphal);
#endif
