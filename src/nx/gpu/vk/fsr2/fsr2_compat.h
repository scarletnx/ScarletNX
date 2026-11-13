/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Included before AMD's FSR 2.2.1 sources (ffx_fsr2.cpp, ffx_assert.cpp, vk/ffx_fsr2_vk.cpp), which are written for
   MSVC: the few names of its C runtime they use, and the Vulkan functions through volk as the rest of the runtime
   calls them (no libvulkan is linked) */
#ifndef SNX_NX_GPU_VK_FSR2_COMPAT_H
#define SNX_NX_GPU_VK_FSR2_COMPAT_H
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include "volk.h"
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
/* the Windows C runtimes (MSVC's, MinGW-w64's) have wcscpy_s, with C++'s overload for an array; a macro there breaks
   their own headers' three-argument calls (windows.h's stralign.h, which ffx_assert.cpp includes) */
#ifndef _WIN32
#define wcscpy_s(dst, src) wcscpy(dst, src)
#endif
#ifdef __cplusplus
#include <locale>
#include <string>
#endif
#endif
