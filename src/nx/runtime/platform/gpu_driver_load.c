/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_driver_load.h. libadrenotools' call with its arguments as Eden's native.cpp (InitializeGpuDriver) makes it */
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR /* before any Vulkan header: vkCreateAndroidSurfaceKHR's types */
#endif
#include "nx/runtime/platform/gpu_driver_load.h"
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/common.h"
#include "common/utilities/gpu_driver.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#endif
#include <vulkan/vulkan.h>

/* libadrenotools' entry point and its flag for a custom driver (its include/adrenotools/driver.h and priv.h), declared
   here as the library is opened by name, by the runs that ask for a package only */
typedef void* (*AdrenotoolsOpen)(int mode, int features, const char* tmp_dir, const char* hook_dir,
                                 const char* driver_dir, const char* driver_name, const char* redirect_dir,
                                 void** mapping);
#define SNX_ADRENOTOOLS_DRIVER_CUSTOM 1

static struct {
    int opened;     /* gpu_driver_open ran */
    void* gipa;     /* the package's vkGetInstanceProcAddr, NULL with the system's driver */
    char dir[1100]; /* the package's folder, a '/' at its end */
} g_gpu_driver_load;

/* freedreno_env's variables in the environment, before any driver reads them */
static void freedreno_env(void) {
    SnxFreedrenoVar vars[SNX_FREEDRENO_MAX];
    char why[300];
    int n = snx_freedreno_parse(g_settings.freedreno_variables, vars, SNX_FREEDRENO_MAX, why, sizeof why);
    if (n < 0) fatal("freedreno_env=%s: %s", g_settings.freedreno_variables, why);
    for (int i = 0; i < n; ++i) {
#ifdef _WIN32
        int failed = _putenv_s(vars[i].name, vars[i].value) != 0;
#else
        int failed = setenv(vars[i].name, vars[i].value, 1) != 0;
#endif
        if (failed) fatal("freedreno_env: %s=%s could not be set", vars[i].name, vars[i].value);
        LOG("freedreno: %s=%s\n", vars[i].name, vars[i].value);
    }
}

void* gpu_driver_open(void) {
    const char* id = g_settings.gpu_driver;
    SnxGpuDriverHost host;
    SnxGpuDriverMeta meta;
    char what[512], *dirs, *dir, *path;
    unsigned char* text;
    size_t size = 0;
    if (g_gpu_driver_load.opened) return g_gpu_driver_load.gipa;
    g_gpu_driver_load.opened = 1;
    freedreno_env();
    if (!id[0]) return NULL;
    if (!snx_gpu_driver_id_valid(id))
        fatal("gpu_driver=%s is no driver's folder name: letters, digits, '.', '-' and '_', 96 at most", id);
    snx_gpu_driver_host(&host);
    if (!snx_gpu_driver_loads(&host, what, sizeof what)) fatal("GPU driver %s cannot load here. %s", id, what);
    dirs = snx_gpu_drivers_dir();
    if (!dirs) fatal("GPU driver %s: no drivers folder, as the environment names no home (HOME)", id);
    dir = snx_path_join(dirs, id);
    path = dir ? snx_path_join(dir, "meta.json") : NULL;
    text = path ? snx_read_file(path, &size) : NULL;
    LOG("vulkan: GPU driver %s: %s\n", id, dir ? dir : dirs);
    if (!text) fatal("GPU driver %s is not installed: its folder has no meta.json", id);
    if (!snx_gpu_driver_meta((const char*)text, size, &meta, what, sizeof what)) fatal("GPU driver %s: %s", id, what);
    free(text);
    free(path);
    if (meta.min_api > host.api)
        fatal("GPU driver %s (%s) needs Android API %d; this device has API %d", id, meta.name, meta.min_api, host.api);
    path = snx_path_join(dir, meta.library);
    if (!path || !snx_is_file(path))
        fatal("GPU driver %s: its library %s, which its meta.json names, is not in its folder", id, meta.library);
    free(path);
    snprintf(g_gpu_driver_load.dir, sizeof g_gpu_driver_load.dir, "%s/", dir);
    LOG("vulkan: GPU driver %s is %s, %s (%s), its library %s\n", id, meta.name,
        meta.driver_version[0] ? meta.driver_version : "no driverVersion", meta.vendor[0] ? meta.vendor : "no vendor",
        meta.library);
#if defined(__ANDROID__) && defined(__aarch64__)
    {
        /* the hooks it loads by name are in the app's libraries folder, beside this one */
        Dl_info self;
        char *hooks = dladdr((void*)gpu_driver_open, &self) && self.dli_fname ? snx_dirname(self.dli_fname) : NULL,
             *tmp = snx_path_join(dirs, ".tmp"); /* the patched loader's copy, on API 28 only, which has no memfd */
        void *module = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL), *loader;
        AdrenotoolsOpen open_libvulkan = module ? (AdrenotoolsOpen)dlsym(module, "adrenotools_open_libvulkan") : NULL;
        char tmp_dir[1100];
        if (!module) fatal("GPU driver %s did not load: libadrenotools.so did not open: %s", id, dlerror());
        if (!open_libvulkan)
            fatal("GPU driver %s did not load: libadrenotools.so has no adrenotools_open_libvulkan", id);
        if (!hooks) fatal("GPU driver %s did not load: the app's libraries folder is not known (dladdr)", id);
        snprintf(tmp_dir, sizeof tmp_dir, "%s/", tmp ? tmp : dirs);
        if (tmp) snx_mkdirs(tmp);
        loader = open_libvulkan(RTLD_NOW, SNX_ADRENOTOOLS_DRIVER_CUSTOM, tmp_dir, hooks, g_gpu_driver_load.dir,
                                meta.library, NULL, NULL);
        if (!loader)
            fatal("GPU driver %s did not load: libadrenotools opened no Vulkan loader (the system log's adrenotools "
                  "lines say why)",
                  id);
        g_gpu_driver_load.gipa = dlsym(loader, "vkGetInstanceProcAddr");
        if (!g_gpu_driver_load.gipa)
            fatal("GPU driver %s did not load: its Vulkan loader has no vkGetInstanceProcAddr", id);
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
    f = fopen("/proc/self/maps", "r");
    if (!f)
        fatal("GPU driver %s: /proc/self/maps did not open, so whether it loaded is not known", g_settings.gpu_driver);
    do {
        if (room - n < 65536) {
            char* more = (char*)realloc(maps, room + 262144 + 1);
            if (!more)
                fatal("GPU driver %s: no memory to read /proc/self/maps (%zu bytes read)", g_settings.gpu_driver, n);
            maps = more;
            room += 262144;
        }
        got = fread(maps + n, 1, room - n, f);
        n += got;
    } while (got);
    fclose(f);
    maps[n] = 0;
    if (!snx_gpu_driver_mapped(maps, g_gpu_driver_load.dir)) {
        LOG("vulkan: no library of %s is mapped after vkCreateInstance\n", g_gpu_driver_load.dir);
        fatal("GPU driver %s did not load: the system's driver took its place (the system log's hook_impl lines say "
              "why)",
              g_settings.gpu_driver);
    }
    free(maps);
    LOG("vulkan: GPU driver %s runs: its libraries are mapped\n", g_settings.gpu_driver);
}

int gpu_driver_custom(void) { return g_gpu_driver_load.gipa != NULL; }

int gpu_driver_surface(VkInstance instance, void* window, VkSurfaceKHR* surface) {
#ifdef __ANDROID__
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)g_gpu_driver_load.gipa;
    PFN_vkCreateAndroidSurfaceKHR create =
        gipa ? (PFN_vkCreateAndroidSurfaceKHR)gipa(instance, "vkCreateAndroidSurfaceKHR") : NULL;
    VkAndroidSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                          .window = (struct ANativeWindow*)window};
    VkResult r;
    if (!create) {
        LOG("video: GPU driver %s has no vkCreateAndroidSurfaceKHR\n", g_settings.gpu_driver);
        return 0;
    }
    if (!window) {
        LOG("video: no Vulkan surface: the window has no ANativeWindow\n");
        return 0;
    }
    r = create(instance, &info, NULL, surface);
    if (r == VK_SUCCESS) return 1;
    LOG("video: no Vulkan surface from GPU driver %s: %s (%d)\n", g_settings.gpu_driver, snx_vk_result_str(r), (int)r);
    return 0;
#else
    (void)instance;
    (void)window;
    (void)surface;
    return 0;
#endif
}
