/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_driver_load.h. What a run checks of a package before it loads it, as Eden's native.cpp (InitializeGpuDriver)
   does; the loading itself is gpu_driver_android.c's */
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR /* before any Vulkan header: vkCreateAndroidSurfaceKHR's types */
#endif
#include "common/gpu/vk/gpu_driver_load.h"
#include "common/gpu/vk/gpu_driver_android.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/common.h"
#include "common/utilities/gpu_driver.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#endif
#include <vulkan/vulkan.h>

static struct {
    int used;             /* snx_gpu_driver_use ran */
    int opened;           /* gpu_driver_open ran */
    void* gipa;           /* the package's vkGetInstanceProcAddr, NULL with the system's driver */
    char id[128];         /* gpu_driver */
    char freedreno[512];  /* freedreno_env */
    char name[256];       /* the package's name, its meta.json's */
    char dir[1100];       /* the package's folder, a '/' at its end */
    void (*say)(const char* line);
    void (*stop)(const char* line);
} g_gpu_driver_load;

/* a line of the run's log */
[[gnu::format(printf, 1, 2)]] static void say(const char* fmt, ...);
static void say(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_gpu_driver_load.say)
        g_gpu_driver_load.say(line);
    else
        fputs(line, stdout);
}
/* the end of a run that cannot go on, with why */
[[noreturn, gnu::format(printf, 1, 2)]] static void stop(const char* fmt, ...);
[[noreturn]] static void stop(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_gpu_driver_load.stop) g_gpu_driver_load.stop(line);
    fprintf(stderr, "scarletnx: %s\n", line);
    exit(1);
}

void snx_gpu_driver_use(const SnxGpuDriverRun* run) {
    if (g_gpu_driver_load.used || g_gpu_driver_load.opened || !run) return;
    g_gpu_driver_load.used = 1;
    snprintf(g_gpu_driver_load.id, sizeof g_gpu_driver_load.id, "%s", run->driver ? run->driver : "");
    snprintf(g_gpu_driver_load.freedreno, sizeof g_gpu_driver_load.freedreno, "%s", run->freedreno ? run->freedreno : "");
    g_gpu_driver_load.say = run->say;
    g_gpu_driver_load.stop = run->stop;
}

/* Turnip keeps one of an Adreno 6xx's five hardware descriptor sets for dynamic buffer offsets, which the renderer never
   uses, and reports four bound sets; the renderer's layout takes a set for each of five stages, and tessellation and
   geometry draws need the fifth. Mesa's tu_dont_reserve_descriptor_set gives it back (Mesa reads a drirc option from
   the environment). A value the environment has already stays */
#define SNX_TURNIP_SETS_VAR "tu_dont_reserve_descriptor_set"
static void turnip_sets(void) {
    const char* had = getenv("tu_dont_reserve_descriptor_set");
    if (had) {
        say("freedreno: %s=%s, as the environment has it\n", SNX_TURNIP_SETS_VAR, had);
        return;
    }
#ifdef _WIN32
    int failed = _putenv_s(SNX_TURNIP_SETS_VAR, "true") != 0;
#else
    int failed = setenv(SNX_TURNIP_SETS_VAR, "true", 0) != 0;
#endif
    if (failed) stop("freedreno: %s=true could not be set", SNX_TURNIP_SETS_VAR);
    say("freedreno: %s=true, for the fifth descriptor set\n", SNX_TURNIP_SETS_VAR);
}
#if defined(__ANDROID__) && defined(__aarch64__)
static void panvk_libhardware(void) {
    const char* path = "/system/lib64/libhardware.so";
    if (!dlopen(path, RTLD_NOW | RTLD_LOCAL)) say("vulkan: %s did not open by its path: %s\n", path, dlerror());
}
#endif
/* freedreno_env's variables in the environment, before any driver reads them */
static void freedreno_env(void) {
    SnxFreedrenoVar vars[SNX_FREEDRENO_MAX];
    char why[300];
    int n = snx_freedreno_parse(g_gpu_driver_load.freedreno, vars, SNX_FREEDRENO_MAX, why, sizeof why);
    if (n < 0) stop("freedreno_env=%s: %s", g_gpu_driver_load.freedreno, why);
    for (int i = 0; i < n; ++i) {
#ifdef _WIN32
        int failed = _putenv_s(vars[i].name, vars[i].value) != 0;
#else
        int failed = setenv(vars[i].name, vars[i].value, 1) != 0;
#endif
        if (failed) stop("freedreno_env: %s=%s could not be set", vars[i].name, vars[i].value);
        say("freedreno: %s=%s\n", vars[i].name, vars[i].value);
    }
}

void* gpu_driver_open(void) {
    const char* id = g_gpu_driver_load.id;
    SnxGpuDriverHost host;
    SnxGpuDriverMeta meta;
    char what[512], *dirs, *dir, *path;
    unsigned char* text;
    size_t size = 0;
    if (g_gpu_driver_load.opened) return g_gpu_driver_load.gipa;
    if (!g_gpu_driver_load.used) return NULL; /* a program that asked for nothing: the system's driver, as it always had */
    g_gpu_driver_load.opened = 1;
    turnip_sets();
    freedreno_env();
    if (!id[0]) return NULL;
    if (!snx_gpu_driver_id_valid(id))
        stop("gpu_driver=%s is no driver's folder name: letters, digits, '.', '-' and '_', 96 at most", id);
    snx_gpu_driver_host(&host);
    if (!snx_gpu_driver_loads(&host, what, sizeof what)) stop("GPU driver %s cannot load here. %s", id, what);
    dirs = snx_gpu_drivers_dir();
    if (!dirs) stop("GPU driver %s: no drivers folder, as the environment names no home (HOME)", id);
    dir = snx_path_join(dirs, id);
    path = dir ? snx_path_join(dir, "meta.json") : NULL;
    text = path ? snx_read_file(path, &size) : NULL;
    say("vulkan: GPU driver %s: %s\n", id, dir ? dir : dirs);
    if (!text) stop("GPU driver %s is not installed: its folder has no meta.json", id);
    if (!snx_gpu_driver_meta((const char*)text, size, &meta, what, sizeof what)) stop("GPU driver %s: %s", id, what);
    free(text);
    free(path);
    if (meta.min_api > host.api)
        stop("GPU driver %s (%s) needs Android API %d; this device has API %d", id, meta.name, meta.min_api, host.api);
    path = snx_path_join(dir, meta.library);
    if (!path || !snx_is_file(path))
        stop("GPU driver %s: its library %s, which its meta.json names, is not in its folder", id, meta.library);
    free(path);
    snprintf(g_gpu_driver_load.dir, sizeof g_gpu_driver_load.dir, "%s/", dir);
    snprintf(g_gpu_driver_load.name, sizeof g_gpu_driver_load.name, "%s", meta.name);
    say("vulkan: GPU driver %s is %s, %s (%s), its library %s\n", id, meta.name,
        meta.driver_version[0] ? meta.driver_version : "no driverVersion", meta.vendor[0] ? meta.vendor : "no vendor",
        meta.library);
#if defined(__ANDROID__) && defined(__aarch64__)
    {
        /* the loader's one library is in the app's libraries folder, beside this one */
        Dl_info self;
        char *hooks = dladdr((void*)gpu_driver_open, &self) && self.dli_fname ? snx_dirname(self.dli_fname) : NULL,
             *tmp = host.api < 29 ? snx_path_join(dirs, ".tmp") : NULL; /* Android 9: a kernel with no memory files */
        char said[1700];
        void* loader;
        if (!hooks) stop("GPU driver %s did not load: the app's libraries folder is not known (dladdr)", id);
        if (tmp && snx_mkdirs(tmp))
            stop("GPU driver %s did not load: the folder %s, for a copy of Android's Vulkan loader, was not made", id,
                 tmp);
        panvk_libhardware();
        loader = gpu_driver_android_open(hooks, g_gpu_driver_load.dir, meta.library, tmp, said, sizeof said);
        if (!loader) stop("GPU driver %s did not load: %s", id, said);
        g_gpu_driver_load.gipa = dlsym(loader, "vkGetInstanceProcAddr");
        if (!g_gpu_driver_load.gipa)
            stop("GPU driver %s did not load: its copy of Android's Vulkan loader has no vkGetInstanceProcAddr", id);
        free(hooks);
        free(tmp);
    }
#endif
    free(dir);
    free(dirs);
    return g_gpu_driver_load.gipa;
}

void gpu_driver_check(void) {
    FILE* f;
    char* maps = NULL;
    size_t n = 0, room = 0, got;
    if (!g_gpu_driver_load.gipa) return;
#if defined(__ANDROID__) && defined(__aarch64__)
    {
        char said[3400];
        if (!gpu_driver_android_loaded(said, sizeof said))
            stop("GPU driver %s did not load: %s", g_gpu_driver_load.id, said);
        say("vulkan: GPU driver %s: %s\n", g_gpu_driver_load.id, said);
    }
#endif
    f = fopen("/proc/self/maps", "r");
    if (!f)
        stop("GPU driver %s: /proc/self/maps did not open, so whether it loaded is not known", g_gpu_driver_load.id);
    do {
        if (room - n < 65536) {
            char* more = (char*)realloc(maps, room + 262144 + 1);
            if (!more)
                stop("GPU driver %s: no memory to read /proc/self/maps (%zu bytes read)", g_gpu_driver_load.id, n);
            maps = more;
            room += 262144;
        }
        got = fread(maps + n, 1, room - n, f);
        n += got;
    } while (got);
    fclose(f);
    maps[n] = 0;
    if (!snx_gpu_driver_mapped(maps, g_gpu_driver_load.dir)) {
        say("vulkan: no library of %s is mapped after vkCreateInstance\n", g_gpu_driver_load.dir);
        stop("GPU driver %s did not load: no library of its folder %s is mapped after the Vulkan instance was made",
             g_gpu_driver_load.id, g_gpu_driver_load.dir);
    }
    free(maps);
    say("vulkan: GPU driver %s runs: its libraries are mapped\n", g_gpu_driver_load.id);
}

int gpu_driver_custom(void) { return g_gpu_driver_load.gipa != NULL; }

int gpu_driver_no_device(char* out, size_t size) {
    SnxGpuDriverHost host;
    if (!g_gpu_driver_load.gipa) return 0;
    snx_gpu_driver_host(&host);
    snx_gpu_driver_no_device(g_gpu_driver_load.id, g_gpu_driver_load.name, &host, out, size);
    return 1;
}

int gpu_driver_surface(VkInstance instance, void* window, VkSurfaceKHR* surface) {
#ifdef __ANDROID__
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)g_gpu_driver_load.gipa;
    PFN_vkCreateAndroidSurfaceKHR create =
        gipa ? (PFN_vkCreateAndroidSurfaceKHR)gipa(instance, "vkCreateAndroidSurfaceKHR") : NULL;
    VkAndroidSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                          .window = (struct ANativeWindow*)window};
    VkResult r;
    if (!create) {
        say("video: GPU driver %s has no vkCreateAndroidSurfaceKHR\n", g_gpu_driver_load.id);
        return 0;
    }
    if (!window) {
        say("video: no Vulkan surface: the window has no ANativeWindow\n");
        return 0;
    }
    r = create(instance, &info, NULL, surface);
    if (r == VK_SUCCESS) return 1;
    say("video: no Vulkan surface from GPU driver %s: %s (%d)\n", g_gpu_driver_load.id, snx_vk_result_str(r), (int)r);
    return 0;
#else
    (void)instance;
    (void)window;
    (void)surface;
    return 0;
#endif
}
