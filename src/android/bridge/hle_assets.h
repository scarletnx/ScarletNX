/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_ASSETS_H
#define SNX_ANDROID_HLE_ASSETS_H
#include "android/runtime/app.h"

/* the app's assets for AAssetManager (hle_android.c): the driver's, given once before the app runs */
void hle_android_set_assets(const AndroidAssets* assets);
#endif
