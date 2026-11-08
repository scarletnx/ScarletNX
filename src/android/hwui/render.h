/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_RENDER_H
#define SNX_ANDROID_RENDER_H
#include "android/bridge/hle_vulkan.h"
#include <jni.h>
#include <stdbool.h>

/* snx.android.ui.Window's natives: the window, its frames drawn with Vulkan, and its events */
bool android_render_register(JNIEnv* env);
/* the window for the native bridge's Vulkan pass-through (hle_vulkan.h's HleWindowHost) */
const HleWindowHost* android_render_host(void);
/* before the renderer starts: no window (--headless), and frames saved as binary PPM (--shot=FILE, %ld the frame's
   number, at the frames of --shot-frame=60,120, 60 when NULL) */
void android_render_set_options(bool headless, const char* shot, const char* shot_frames);
/* the renderer's device and window gone, when the app has ended */
void android_render_close(void);
#endif
