/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's shared libraries, the same for every driver: dlopen with RTLD_NOW | RTLD_LOCAL, or LoadLibraryW of the
   UTF-8 path */
#ifndef SNX_PLATFORM_DL_H
#define SNX_PLATFORM_DL_H

/* the library at path, its symbols bound now and kept to itself; NULL with the reason in plat_dl_error */
void* plat_dl_open(const char* path);
void* plat_dl_sym(void* library, const char* name); /* NULL when it has none of that name */
void plat_dl_close(void* library);
/* the reason the calling thread's last plat_dl_open or plat_dl_sym returned NULL */
const char* plat_dl_error(void);
/* the library of that name when the process has it loaded already (RenderDoc's program puts its own in the game it
   starts), NULL when it hasn't: never loads it */
void* plat_dl_loaded(const char* name);
/* What plat_dl_open tries when the system refuses path, NULL for nothing: Android's shared storage is mounted noexec,
   and its driver loads a copy in the app's cache folder. Set once, before the first plat_dl_open */
void plat_dl_fallback(void* (*open)(const char* path));
#endif
