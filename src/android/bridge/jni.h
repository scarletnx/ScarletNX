/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_BRIDGE_JNI_H
#define SNX_ANDROID_BRIDGE_JNI_H
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* snx.android.nativebridge.NativeBridge's natives: a library loaded with its JNI_OnLoad, and native methods called */
bool android_bridge_register(JNIEnv* env);
/* snx.android.opengl.Egl10's natives: Java's EGL10 over the bridge's EGL (hle_egl.c) */
bool android_egl_register(JNIEnv* env);
/* the host's JNIEnv of a guest JNIEnv* */
JNIEnv* bridge_jni_host(uint64_t guest_env);
/* the guest's JNIEnv* of the calling thread's host JNIEnv, as its native methods get it (the bridge started) */
uint64_t bridge_jni_env(JNIEnv* host);
/* Strings in ART's modified UTF-8, where HotSpot's is CESU-8: a supplementary character (a surrogate pair) is UTF-8's 4
   bytes, not 6, and NUL C0 80 and a lone surrogate its 3 bytes in both. n UTF-16 units into out (NULL: counted): its
   length, without a NUL */
size_t bridge_art_utf8(const jchar* units, size_t n, unsigned char* out);
/* n bytes of modified UTF-8 as UTF-16 units into units (room for n): their count. A 4-byte sequence is a surrogate
   pair, as ART reads it */
size_t bridge_art_units(const unsigned char* bytes, size_t n, jchar* units);
#endif
