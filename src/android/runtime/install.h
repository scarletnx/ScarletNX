/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_INSTALL_H
#define SNX_ANDROID_INSTALL_H
#include <stdbool.h>
#include <stddef.h>

/* An app installed as Android's package manager installs one: its base APK and its splits, its native libraries
   extracted for the native bridge, and its folders. An APK runs where it is. A bundle (an XAPK or an APKS, a zip of
   APKs) is unpacked once into the app's folder, base.apk and split_<name>.apk, as /data/app/<package> holds them */
enum { ANDROID_MAX_SPLITS = 64 };
typedef struct {
    char package[256];
    char* base;                         /* the base APK */
    char* splits[ANDROID_MAX_SPLITS];   /* each split APK */
    char* split_names[ANDROID_MAX_SPLITS];
    unsigned split_count;
    char* app_dir;     /* android/app/<package> in the data folder: the bundle's APKs, lib/ */
    char* library_dir; /* app_dir/lib/arm64: the native libraries of the device's ABI */
    char* data_dir;    /* android/data/<package>: the app's own data (Context.getDataDir) */
} AndroidInstall;

/* the app at path (an APK or a bundle) installed, or found installed already. False with error filled */
bool android_install(const char* path, AndroidInstall* out, char* error, size_t error_size);
void android_install_free(AndroidInstall* app);
#endif
