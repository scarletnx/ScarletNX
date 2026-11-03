/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_PROC_H
#define SNX_ANDROID_HLE_PROC_H
/* The app's own process files in /proc, as an Android device shows them, on every host (docs/android/NATIVE.md):
   cmdline (the package), comm, status (no tracer), stat, maps (the guest's libraries) and the exe link
   (app_process64), of /proc/self, /proc/thread-self and /proc/<the app's pid>. An app's protection reads them, and
   with none (FreeBSD has no /proc by default) or the JVM's (Linux's) an app may go on with a wrong key. Other paths
   are the host's */
#include <stddef.h>

/* the app's package name, before its native code runs */
void hle_proc_set_app(const char* package);
/* the file at path when it is one of these: a host descriptor of a temporary file with its bytes, at offset 0, or
   -1 and errno. -2 for another path, which the caller gives the host */
int hle_proc_open(const char* path);
/* readlink of the exe link: its length (no NUL written), or -2 for another path */
long hle_proc_readlink(const char* path, char* buffer, size_t size);
#endif
