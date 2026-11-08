/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_APP_H
#define SNX_ANDROID_APP_H
#include "android/runtime/install.h"
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The app the runtime runs: its base APK and splits, opened once, and the base's DEX files read into a DexProgram.
   The install is kept until android_app_close */
bool android_app_open(const AndroidInstall* install, char* error, size_t error_size);
void android_app_close(void);
/* the app's assets (assets/ of its APKs), for the native bridge's AAssetManager */
typedef struct {
    /* a whole asset's bytes, the caller's to free; NULL for none */
    unsigned char* (*read)(const char* name, size_t* size);
    /* a stored asset's place in its APK's file, for a file descriptor of it; false for a deflated one or none */
    bool (*locate)(const char* name, const char** apk, uint64_t* offset, uint64_t* size);
    /* the names in an asset folder, the caller's to free (each, and the array) */
    char** (*list)(const char* dir, size_t* count);
} AndroidAssets;
const AndroidAssets* android_app_assets(void);

/* the natives of snx.dalvik.AppClassLoader (the translation of each class as it loads) and snx.android.Apk */
bool android_app_register(JNIEnv* env);
#endif
