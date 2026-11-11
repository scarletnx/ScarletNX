/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_driver_android.h */
#include "nx/runtime/platform/gpu_driver_android.h"
#include "nx/runtime/platform/gpu_driver_hook.h"
#include "common/utilities/elf_dynamic.h"
#include "common/utilities/gpu_driver_stage.h"
#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* a namespace that starts with its parent's folders, loaded libraries and links (the platform's dlext_namespaces.h,
   ANDROID_NAMESPACE_TYPE_SHARED) */
#define NAMESPACE_SHARED 2
#define SYSTEM_VULKAN    "/system/lib64/libvulkan.so"

typedef struct android_namespace_t Namespace;
static struct {
    /* the linker's own functions (bionic's linker/dlfcn.cpp), the same from Android 9 on. The last argument of the
       first is an address in the library the call is taken to come from, whose namespace is the parent when none is
       given */
    Namespace* (*create_namespace)(const char* name, const char* ld_library_path, const char* default_library_path,
                                   uint64_t type, const char* permitted_when_isolated_path, Namespace* parent,
                                   const void* caller);
    bool (*link_namespaces)(Namespace* from, Namespace* to, const char* sonames); /* to NULL: the default one */
    bool (*link_namespaces_all_libs)(Namespace* from, Namespace* to);
    Namespace* (*get_exported_namespace)(const char* name);
    Namespace* vulkan;              /* the namespace of the run's copy of the Vulkan loader */
    char dir[1100];                 /* the package's folder, a '/' at its end */
    char library[SNX_ELF_NAME_MAX]; /* its driver */
    char asked[SNX_ELF_NAME_MAX];   /* the driver the Vulkan loader asked for, "" until it does */
    char opened[1300];              /* the package's library as opened in its place, "" when it was not */
    char why[1600];                 /* why not */
    int system;                     /* the system's driver was opened then, as the loader asked */
} g_gpu_driver_android;

/* what the linker says of the call that just failed */
static const char* linker_said(void) {
    const char* said = dlerror();
    return said ? said : "the linker says no more";
}

/* the package's library opened as the driver filename, which the Vulkan loader asked for in extinfo's namespace: its
   handle, NULL with g_gpu_driver_android.why */
static void* open_package(const char* filename, int flags, const android_dlextinfo* extinfo) {
    char path[2300], reason[900], *why = g_gpu_driver_android.why;
    const char* library = g_gpu_driver_android.library;
    const size_t size = sizeof g_gpu_driver_android.why;
    SnxElfRename* names = NULL;
    size_t count = 0;
    android_dlextinfo info;
    Namespace* ns;
    void* handle;
    if (!extinfo || !(extinfo->flags & ANDROID_DLEXT_USE_NAMESPACE) || !extinfo->library_namespace) {
        snprintf(why, size, "Android's Vulkan loader asked for its driver %s in no linker namespace", filename);
        return NULL;
    }
    /* a library of the driver's name in the process already (the system's own driver, on Android 16 from the app's
       start) is what the linker would give for it, never opening the package's: the package then loads from copies
       under names nothing has (gpu_driver_stage.h), the folder itself second for what its libraries open by name */
    info = (android_dlextinfo){.flags = ANDROID_DLEXT_USE_NAMESPACE, .library_namespace = extinfo->library_namespace};
    handle = android_dlopen_ext(library, RTLD_NOW | RTLD_NOLOAD, &info);
    if (handle) {
        dlclose(handle);
        if (!snx_gpu_driver_stage(g_gpu_driver_android.dir, &names, &count, reason, sizeof reason)) {
            snprintf(why, size,
                     "the process has a library named %s already, and the package was not copied under other "
                     "names: %s",
                     library, reason);
            return NULL;
        }
        snprintf(path, sizeof path, "%s%s/:%s", g_gpu_driver_android.dir, SNX_GPU_DRIVER_UNIQUE_DIR,
                 g_gpu_driver_android.dir);
        snprintf(g_gpu_driver_android.opened, sizeof g_gpu_driver_android.opened, "%s%s/%s", g_gpu_driver_android.dir,
                 SNX_GPU_DRIVER_UNIQUE_DIR, snx_gpu_driver_staged_name(names, count, library));
    } else {
        dlerror(); /* not loaded, as expected: not a later failure's reason */
        snprintf(path, sizeof path, "%s", g_gpu_driver_android.dir);
        snprintf(g_gpu_driver_android.opened, sizeof g_gpu_driver_android.opened, "%s%s", g_gpu_driver_android.dir,
                 library);
    }
    ns = g_gpu_driver_android.create_namespace("scarletnx-gpu-driver", path, NULL, NAMESPACE_SHARED, NULL,
                                               extinfo->library_namespace, &g_gpu_driver_android);
    if (!ns) {
        snprintf(why, size, "the linker made no namespace for the package's folder %s: %s", path, linker_said());
        g_gpu_driver_android.opened[0] = 0;
        free(names);
        return NULL;
    }
    /* libandroid.so is not in the vendor's namespaces; a package's library has always found it, as libadrenotools
       linked it in for its own hook */
    g_gpu_driver_android.link_namespaces(ns, NULL, "libandroid.so");
    info = *extinfo;
    info.library_namespace = ns;
    handle = android_dlopen_ext(snx_gpu_driver_staged_name(names, count, library), flags, &info);
    if (!handle) {
        snprintf(why, size, "its library %s did not open: %s", g_gpu_driver_android.opened, linker_said());
        g_gpu_driver_android.opened[0] = 0;
    }
    free(names);
    return handle;
}

/* the system's android_dlopen_ext as the copy of the Vulkan loader called it: with no namespace given, the linker takes
   the caller's, which is the copy's and not this library's */
static void* open_as_asked(const char* filename, int flags, const android_dlextinfo* extinfo) {
    android_dlextinfo info = {0};
    if (extinfo) info = *extinfo;
    if (!(info.flags & ANDROID_DLEXT_USE_NAMESPACE)) {
        info.flags |= ANDROID_DLEXT_USE_NAMESPACE;
        info.library_namespace = g_gpu_driver_android.vulkan;
    }
    return android_dlopen_ext(filename, flags, &info);
}

/* android_dlopen_ext for the copy of the Vulkan loader: its driver (vulkan.<name>.so) is the package's library, and
   anything else it opens is opened as asked. A package that does not open leaves the system's driver, so that the
   instance is made and the run can say why (gpu_driver_android_loaded) */
static void* hook_open(const char* filename, int flags, const void* extinfo) {
    void* handle;
    if (!filename || !strstr(filename, "vulkan.")) return open_as_asked(filename, flags, extinfo);
    snprintf(g_gpu_driver_android.asked, sizeof g_gpu_driver_android.asked, "%s", filename);
    g_gpu_driver_android.why[0] = 0;
    g_gpu_driver_android.opened[0] = 0;
    handle = open_package(filename, flags, (const android_dlextinfo*)extinfo);
    if (handle) return handle;
    handle = open_as_asked(filename, flags, extinfo);
    g_gpu_driver_android.system = handle != NULL;
    return handle;
}

/* android_load_sphal_library for the copy: the library from the vendor's namespace, as libvndksupport's opens it */
static void* hook_sphal(const char* filename, int flags) {
    static const char* const names[] = {"sphal", "vendor", "default"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        Namespace* ns = g_gpu_driver_android.get_exported_namespace(names[i]);
        if (ns) {
            const android_dlextinfo info = {.flags = ANDROID_DLEXT_USE_NAMESPACE, .library_namespace = ns};
            return hook_open(filename, flags, &info);
        }
    }
    if (filename && strstr(filename, "vulkan.")) {
        snprintf(g_gpu_driver_android.asked, sizeof g_gpu_driver_android.asked, "%s", filename);
        snprintf(g_gpu_driver_android.why, sizeof g_gpu_driver_android.why,
                 "the linker exports no sphal, vendor or default namespace for Android's Vulkan loader's driver %s",
                 filename);
    }
    return NULL;
}

/* the system's Vulkan loader loaded once more, into ns: from a copy under another soname and in another file, as the
   linker gives a library it has by either the one it has. tmp as gpu_driver_android_open's */
static void* open_vulkan_copy(Namespace* ns, const char* tmp, char* why, size_t size) {
    SnxElfRename name = {"libvulkan.so", ""};
    char path[1200], reason[700];
    android_dlextinfo info = {.flags = ANDROID_DLEXT_USE_NAMESPACE | ANDROID_DLEXT_USE_LIBRARY_FD,
                              .library_namespace = ns};
    struct stat st;
    void* handle;
    int fd, renamed;
    snx_gpu_driver_unique_name(name.from, name.to, sizeof name.to);
    if (tmp) {
        snprintf(path, sizeof path, "%s/%s", tmp, name.to);
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    } else {
        snprintf(path, sizeof path, "a memory file (memfd_create)");
        fd = (int)syscall(__NR_memfd_create, name.to, 0u);
    }
    if (fd < 0) {
        snprintf(why, size, "no file for a copy of %s: %s did not open: %s", SYSTEM_VULKAN, path, strerror(errno));
        return NULL;
    }
    renamed = snx_gpu_driver_copy(SYSTEM_VULKAN, fd, &name, 1, &st, reason, sizeof reason);
    if (renamed != 1) {
        if (renamed < 0)
            snprintf(why, size, "no copy of Android's Vulkan loader: %s", reason);
        else
            snprintf(why, size, "no copy of Android's Vulkan loader: %s does not name itself %s", SYSTEM_VULKAN,
                     name.from);
        close(fd);
        return NULL;
    }
    info.library_fd = fd;
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    handle = android_dlopen_ext(path, RTLD_NOW, &info);
    if (!handle)
        snprintf(why, size, "the copy of %s (%lld bytes, as %s) did not load: %s", SYSTEM_VULKAN, (long long)st.st_size,
                 name.to, linker_said());
    close(fd); /* the linker has it mapped */
    return handle;
}

void* gpu_driver_android_open(const char* hooks, const char* dir, const char* library, const char* tmp, char* why,
                              size_t size) {
    static const char* const symbols[] = {"__loader_android_create_namespace", "__loader_android_link_namespaces",
                                          "__loader_android_link_namespaces_all_libs",
                                          "__loader_android_get_exported_namespace"};
    const void* linker = (const void*)getauxval(AT_BASE);
    android_dlextinfo in = {.flags = ANDROID_DLEXT_USE_NAMESPACE};
    void *found[4], *hook, *set;
    Namespace *all, *ns;
    if (!linker) {
        snprintf(why, size, "the kernel gave this process no address for its dynamic linker (AT_BASE is 0)");
        return NULL;
    }
    for (size_t i = 0; i < 4; ++i) {
        found[i] = snx_elf_loaded_symbol(linker, symbols[i]);
        if (!found[i]) {
            snprintf(why, size, "the dynamic linker at %p has no %s", linker, symbols[i]);
            return NULL;
        }
    }
    /* the object pointers dlsym's own callers convert the same way */
    memcpy(&g_gpu_driver_android.create_namespace, &found[0], sizeof found[0]);
    memcpy(&g_gpu_driver_android.link_namespaces, &found[1], sizeof found[1]);
    memcpy(&g_gpu_driver_android.link_namespaces_all_libs, &found[2], sizeof found[2]);
    memcpy(&g_gpu_driver_android.get_exported_namespace, &found[3], sizeof found[3]);
    snprintf(g_gpu_driver_android.dir, sizeof g_gpu_driver_android.dir, "%s", dir);
    snprintf(g_gpu_driver_android.library, sizeof g_gpu_driver_android.library, "%s", library);
    /* the copy's namespace: the app's, with the hook's folder, where the hook is in the way of the copy alone. An
       app's namespace reaches only the system's public libraries and the Vulkan loader needs others, so it is linked
       whole to one that reaches them: a namespace made as if by libdl, the system's own library */
    ns = g_gpu_driver_android.create_namespace("scarletnx-vulkan", hooks, NULL, NAMESPACE_SHARED, NULL, NULL,
                                               &g_gpu_driver_android);
    all = g_gpu_driver_android.create_namespace("scarletnx-system", NULL, NULL, NAMESPACE_SHARED, NULL, NULL,
                                                (const void*)(uintptr_t)dlopen);
    if (!ns || !all) {
        snprintf(why, size, "the linker made no namespace for a copy of Android's Vulkan loader (%s): %s",
                 ns ? "the system's libraries'" : "the app's with the hook", linker_said());
        return NULL;
    }
    if (!g_gpu_driver_android.link_namespaces_all_libs(ns, all)) {
        snprintf(why, size, "the linker did not link the Vulkan loader's namespace to the system's libraries: %s",
                 linker_said());
        return NULL;
    }
    g_gpu_driver_android.vulkan = ns;
    in.library_namespace = ns;
    hook = android_dlopen_ext(SNX_GPU_DRIVER_HOOK_LIBRARY, RTLD_NOW | RTLD_GLOBAL, &in);
    if (!hook) {
        snprintf(why, size, "%s did not load from %s: %s", SNX_GPU_DRIVER_HOOK_LIBRARY, hooks, linker_said());
        return NULL;
    }
    set = dlsym(hook, SNX_GPU_DRIVER_HOOK_SET);
    if (!set) {
        snprintf(why, size, "%s has no %s: %s", SNX_GPU_DRIVER_HOOK_LIBRARY, SNX_GPU_DRIVER_HOOK_SET, linker_said());
        return NULL;
    }
    ((void (*)(SnxGpuDriverHookOpen*, SnxGpuDriverHookSphal*))set)(hook_open, hook_sphal);
    return open_vulkan_copy(ns, tmp, why, size);
}

int gpu_driver_android_loaded(char* what, size_t size) {
    if (!g_gpu_driver_android.asked[0]) {
        snprintf(what, size, "Android's Vulkan loader asked for no driver, so the package's %s%s was never opened",
                 g_gpu_driver_android.dir, g_gpu_driver_android.library);
        return 0;
    }
    if (!g_gpu_driver_android.opened[0]) {
        snprintf(what, size, "%s. Android's Vulkan loader asked for %s and has %s", g_gpu_driver_android.why,
                 g_gpu_driver_android.asked, g_gpu_driver_android.system ? "the system's" : "no driver");
        return 0;
    }
    snprintf(what, size, "Android's Vulkan loader asked for %s and has %s", g_gpu_driver_android.asked,
             g_gpu_driver_android.opened);
    return 1;
}
