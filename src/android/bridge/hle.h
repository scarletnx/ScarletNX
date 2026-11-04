/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_HLE_H
#define SNX_ANDROID_HLE_H
#include "android/bridge/bridge.h"
#include <stdbool.h>
#include <stdint.h>

/* The system's libraries as HLE (docs/android/NATIVE.md): the symbols that bionic, libandroid, liblog, libdl and the
   graphics libraries export, each a host function or a host object */

typedef struct {
    const char* name;
    BridgeHandler handler; /* a function's, NULL for an object */
    void* data;            /* the handler's own, or the object's host address */
} HleSymbol;

/* whether a DT_NEEDED name is a system library (libc.so, libvulkan.so) that the HLE answers */
bool hle_is_system_library(const char* name);
/* the guest address of a system symbol: a function's export, an object's host address; 0 for one the HLE lacks */
uint64_t hle_symbol(const char* name);
/* an import the HLE lacks: an export that reports it when called and returns 0 */
uint64_t hle_missing(const char* name);

/* the tables of each HLE library, an empty name ending each (hle_libc.c and the others) */
extern const HleSymbol hle_libc[];
extern const HleSymbol hle_posix[];
extern const HleSymbol hle_thread[];
extern const HleSymbol hle_sync[]; /* hle_sync.c: bionic's mutexes, conds, rwlocks, barriers, semaphores */
extern const HleSymbol hle_dl[];
extern const HleSymbol hle_sles[];
extern const HleSymbol hle_media[];
extern const HleSymbol hle_android[];
extern const HleSymbol hle_egl[];
extern const HleSymbol hle_gles[]; /* hle_gles.c: GLES 1.1 over Vulkan */
/* GLES 2 and 3 over Vulkan (gles2.h): the state, the objects, the programs and the draws. The names that GLES 1.1 has
   too call the current context's */
extern const HleSymbol hle_gles2[];
extern const HleSymbol hle_gles2_object[];
extern const HleSymbol hle_gles2_program[];
extern const HleSymbol hle_gles2_draw[];
extern const HleSymbol hle_zlib[]; /* hle_zlib.c: libz, the build's zlib */
extern const HleSymbol hle_signal[];
/* the GPU the guest sees, in Vulkan's device properties and GLES's GL_RENDERER alike (hle_vulkan.c): a Mali that the
   engines' lists of Vulkan GPUs hold */
extern const char hle_gpu_name[];
/* the native window's guest address and its size in pixels (the buffers' when the guest set them) */
uint64_t hle_android_window(int* width, int* height);
/* bionic's functions that are guest code (hle_code.c): written into the code window once, after bridge_init */
void hle_code_init(void);
uint64_t hle_code_symbol(const char* name);
/* the copies of the host C library's objects that bionic exports (environ, timezone): once, before a lookup */
void hle_posix_init(void);
/* the HLE's state that needs the bridge (the stack guard): once, after bridge_init */
void hle_libc_init(void);
#endif
