/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_BRIDGE_JNI_H
#define SNX_ANDROID_BRIDGE_JNI_H
#include <jni.h>
#include <stdbool.h>
#include <stdint.h>

/* snx.android.nativebridge.NativeBridge's natives: a library loaded with its JNI_OnLoad, and native methods called */
bool android_bridge_register(JNIEnv* env);
/* snx.android.opengl.Egl10's natives: Java's EGL10 over the bridge's EGL (hle_egl.c) */
bool android_egl_register(JNIEnv* env);
/* the host's JNIEnv of a guest JNIEnv* */
JNIEnv* bridge_jni_host(uint64_t guest_env);
/* the guest's JNIEnv* of the calling thread's host JNIEnv, as its native methods get it (the bridge started) */
uint64_t bridge_jni_env(JNIEnv* host);
#endif
