/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bionic's libdl as HLE: dlopen and dlsym of the guest's libraries through the bridge's linker, and of the system's
   (libvulkan.so, libEGL.so) as handles whose symbols are the HLE's */
#define _GNU_SOURCE
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_proc.h"
#include "android/bridge/linker.h"
#include <stdlib.h>
#include <string.h>

/* dlerror's text, each thread's own, and whether it is new */
static _Thread_local char dl_error[1024];
static _Thread_local bool dl_error_set;
static void set_error(const char* text) {
    snprintf(dl_error, sizeof dl_error, "%s", text);
    dl_error_set = true;
}

/* a system library's handle: an address in this table, which no guest library has */
static const char* const systems[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so",
                                      "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so",
                                      "libOpenSLES.so", "libaaudio.so", "libmediandk.so", "libnativewindow.so",
                                      "libjnigraphics.so", "libsync.so", "libstdc++.so", "ld-android.so",
                                      "libdl_android.so"};
static unsigned char system_handles[sizeof systems / sizeof *systems];

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* a system library's handle, 0 for another name */
static uint64_t system_handle(const char* path) {
    const char* name = base_name(path);
    for (size_t i = 0; i < sizeof systems / sizeof *systems; ++i)
        if (!strcmp(systems[i], name)) return (uint64_t)(uintptr_t)&system_handles[i];
    return 0;
}
bool hle_dl_is_system(const char* name) { return system_handle(name) != 0; }

/* dlopen's handle of a path: the main program for none, a system library's, or the guest library loaded */
static uint64_t open_library(const char* path) {
    if (!path) return (uint64_t)(uintptr_t)&system_handles[0]; /* the main program: RTLD_DEFAULT's search */
    uint64_t system = system_handle(path);
    if (system) return system;
    char error[1024];
    GuestLibrary* l = linker_load(path, error, sizeof error);
    if (!l) {
        char text[1100];
        snprintf(text, sizeof text, "dlopen failed: %s", error);
        set_error(text);
    }
    return (uint64_t)(uintptr_t)l;
}
static void g_dlopen(CPU* c, void* data) {
    (void)data;
    X(0) = open_library(X(0) ? STR(0) : NULL);
}

/*
 * The linker's namespaces (libdl_android's and ld-android's __loader_android_* functions, which an app reaches
 * through dlopen of the linker itself, as liblinkernsbypass does). Every library here is in the process's one
 * namespace, so a namespace is a name the app keeps, links between them always succeed, and a library loads in any
 * of them as dlopen loads it.
 */
typedef struct {
    char name[64];
} Namespace;
static Namespace exported = {"default"};
/* (name, ld_library_path, default_library_path, type, permitted_when_isolated_path, parent, caller) */
static void g_create_namespace(CPU* c, void* data) {
    (void)data;
    Namespace* ns = calloc(1, sizeof *ns);
    if (ns && X(0)) snprintf(ns->name, sizeof ns->name, "%s", STR(0));
    X(0) = (uint64_t)(uintptr_t)ns;
}
static void g_link_namespaces(CPU* c, void* data) {
    (void)data;
    X(0) = 1;
}
static void g_get_exported_namespace(CPU* c, void* data) {
    (void)data;
    X(0) = (uint64_t)(uintptr_t)&exported;
}

/*
 * android_dlopen_ext(filename, flags, extinfo): dlopen, in any namespace (ANDROID_DLEXT_USE_NAMESPACE, 0x200). A
 * library from a descriptor (ANDROID_DLEXT_USE_LIBRARY_FD, 0x10) that is a copy of a system library's file
 * (hle_proc.h) is that system library; another one loads by its name. android_dlextinfo: flags at 0, library_fd at 28
 */
static void g_android_dlopen_ext(CPU* c, void* data) {
    (void)data;
    const uint8_t* info = X(2) ? (const uint8_t*)(uintptr_t)X(2) : NULL;
    uint64_t flags = 0;
    if (info) memcpy(&flags, info, sizeof flags);
    if (info && (flags & 0x10)) {
        int32_t fd;
        memcpy(&fd, info + 28, sizeof fd);
        char name[256];
        if (hle_proc_stub_library(fd, name, sizeof name)) {
            X(0) = system_handle(name);
            return;
        }
    }
    X(0) = open_library(X(0) ? STR(0) : NULL);
}

static void g_dlsym(CPU* c, void* data) {
    (void)data;
    uint64_t handle = X(0), a;
    const char* name = STR(1);
    uint64_t systems_lo = (uint64_t)(uintptr_t)system_handles;
    if (handle == 0) a = linker_symbol(NULL, name);                         /* RTLD_DEFAULT */
    else if (handle == UINT64_MAX) a = linker_next_symbol(X(30), name);     /* RTLD_NEXT */
    else if (handle >= systems_lo && handle < systems_lo + sizeof system_handles) a = hle_symbol(name);
    else a = linker_symbol((GuestLibrary*)(uintptr_t)handle, name);
    if (!a) {
        char text[1100];
        snprintf(text, sizeof text, "undefined symbol: %s", name);
        set_error(text);
    }
    X(0) = a;
}

/* libraries stay loaded: bionic unloads only what nothing else holds, and engines seldom close theirs */
static void g_dlclose(CPU* c, void* data) {
    (void)data;
    X(0) = 0;
}

static void g_dlerror(CPU* c, void* data) {
    (void)data;
    X(0) = dl_error_set ? (uint64_t)(uintptr_t)dl_error : 0;
    dl_error_set = false;
}

/* Dl_info: dli_fname, dli_fbase, dli_sname, dli_saddr */
static void g_dladdr(CPU* c, void* data) {
    (void)data;
    const char *library, *symbol;
    uint64_t base, at;
    uint64_t* info = PTR(1);
    if (!linker_address(X(0), &library, &base, &symbol, &at)) {
        X(0) = 0;
        return;
    }
    info[0] = (uint64_t)(uintptr_t)library;
    info[1] = base;
    info[2] = (uint64_t)(uintptr_t)symbol;
    info[3] = at;
    X(0) = 1;
}

/* dl_iterate_phdr(callback, data): the guest's callback for each library, with a dl_phdr_info */
typedef struct {
    uint64_t callback, data;
} Iterate;
static int each_library(void* context, uint64_t base, const char* name, uint64_t phdr, unsigned count) {
    const Iterate* it = context;
    struct {
        uint64_t addr, name, phdr;
        uint16_t phnum;
        uint64_t adds, subs, tls_modid, tls_data;
    } info = {base, (uint64_t)(uintptr_t)name, phdr, (uint16_t)count, 1, 0, 0, 0};
    uint64_t args[3] = {(uint64_t)(uintptr_t)&info, sizeof info, it->data};
    return (int)bridge_call(it->callback, args, 3);
}
static void g_dl_iterate_phdr(CPU* c, void* data) {
    (void)data;
    Iterate it = {X(0), X(1)};
    X(0) = (uint64_t)(int64_t)linker_each(each_library, &it);
}

const HleSymbol hle_dl[] = {
    /* dlopen itself is guest code that calls this, as libdl's calls the linker's (hle_code.c) */
    {"__loader_dlopen", g_dlopen, NULL},
    {"dlsym", g_dlsym, NULL},
    {"dlclose", g_dlclose, NULL},
    {"dlerror", g_dlerror, NULL},
    {"dladdr", g_dladdr, NULL},
    {"dl_iterate_phdr", g_dl_iterate_phdr, NULL},
    {"android_dlopen_ext", g_android_dlopen_ext, NULL},
    {"__loader_android_dlopen_ext", g_android_dlopen_ext, NULL},
    {"__loader_android_create_namespace", g_create_namespace, NULL},
    {"android_create_namespace", g_create_namespace, NULL},
    {"__loader_android_link_namespaces", g_link_namespaces, NULL},
    {"__loader_android_link_namespaces_all_libs", g_link_namespaces, NULL},
    {"android_link_namespaces", g_link_namespaces, NULL},
    {"__loader_android_get_exported_namespace", g_get_exported_namespace, NULL},
    {"android_get_exported_namespace", g_get_exported_namespace, NULL},
    {"", NULL, NULL},
};
