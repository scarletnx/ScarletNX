/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_FDS_H
#define SNX_ANDROID_FDS_H
#include <jni.h>
#include <stdbool.h>

/* The natives of snx.android.os.Fds: a java.io.FileDescriptor's host descriptor and one made of a descriptor, which
   ParcelFileDescriptor gives the app's native code as Android's does (its getFd, detachFd, fromFd and adoptFd) */
bool android_fds_register(JNIEnv* env);
#endif
