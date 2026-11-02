/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_TEXT_H
#define SNX_ANDROID_TEXT_H
#include <hb.h>
#include <jni.h>
#include <stdbool.h>

/* snx.android.hwui.Text's natives: fonts and shaping, the HLE of minikin, over HarfBuzz */
bool android_text_register(JNIEnv* env);
/* the HarfBuzz font of a handle that Text gives (its scale is the face's units per em), NULL for a bad handle. The
   renderer draws glyph outlines from it */
hb_font_t* android_text_font(jint handle);
#endif
