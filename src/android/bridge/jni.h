/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_BRIDGE_JNI_H
#define SNX_ANDROID_BRIDGE_JNI_H
#include <jni.h>
#include <stdbool.h>

/* snx.android.nativebridge.NativeBridge's natives: a library loaded with its JNI_OnLoad, and native methods called */
bool android_bridge_register(JNIEnv* env);
#endif
