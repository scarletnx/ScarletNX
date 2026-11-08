/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_NATIVE_H
#define SNX_ANDROID_NATIVE_H
#include <stdbool.h>
#include <stddef.h>

/* The native bridge as the driver uses it (docs/android/NATIVE.md) */

/* the bridge started, with the app's library folder: false with the reason in error */
bool android_native_start(const char* library_dir, char* error, size_t error_size);
/* --native-test: a library loaded (and its constructors run), then the functions named called with no arguments, each
   one's x0 printed. The process's exit status */
int android_native_test(const char* library, const char* const* symbols, unsigned count);
#endif
