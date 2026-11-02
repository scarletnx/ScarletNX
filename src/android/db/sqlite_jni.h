/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_SQLITE_JNI_H
#define SNX_ANDROID_SQLITE_JNI_H
#include <jni.h>
#include <stdbool.h>

/* snx.android.db.SQLite's natives: SQLite itself, as libsqlite and the JNI of android.database.sqlite are on Android */
bool android_sqlite_register(JNIEnv* env);
#endif
