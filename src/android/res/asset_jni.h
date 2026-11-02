/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_ASSET_JNI_H
#define SNX_ANDROID_ASSET_JNI_H
#include <jni.h>
#include <stdbool.h>

/* android.content.res.AssetManager's natives */
bool android_assets_register(JNIEnv* env);
#endif
