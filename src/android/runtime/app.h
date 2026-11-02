/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_APP_H
#define SNX_ANDROID_APP_H
#include "android/runtime/install.h"
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>

/* The app the runtime runs: its base APK and splits, opened once, and the base's DEX files read into a DexProgram.
   The install is kept until android_app_close */
bool android_app_open(const AndroidInstall* install, char* error, size_t error_size);
void android_app_close(void);
/* the natives of snx.dalvik.AppClassLoader (the translation of each class as it loads) and snx.android.Apk */
bool android_app_register(JNIEnv* env);
#endif
