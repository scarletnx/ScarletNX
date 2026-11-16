/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's device (gpu/vk.c) as a Switch game's is made: vk_debug_hide_commands has the check of each extension's
   commands take vkCmdSetRenderingAttachmentLocationsKHR as missing, and the device is made again without
   VK_KHR_dynamic_rendering_local_read, which the log says; vk_ext_khr_dynamic_rendering_local_read=off leaves the
   extension off too. A device that has no local read skips (77). The log is the device's stderr, read back from a
   file */
#include "vita/gpu/vk.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "vita_vk_device:%d: %s\n", __LINE__, #c);                                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define LOCAL_READ "VK_KHR_dynamic_rendering_local_read"

/* vita_vk_init, with what it writes on stderr into log (size bytes at most) */
static bool init_said(char* log, size_t size, char* why, size_t why_size) {
    FILE* file = tmpfile();
    if (!file) return false;
    fflush(stderr);
    int saved = dup(2);
    dup2(fileno(file), 2);
    bool made = vita_vk_init(NULL, why, why_size);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(file);
    size_t n = fread(log, 1, size - 1, file);
    log[n] = 0;
    fclose(file);
    fputs(log, stdout);
    return made;
}
static bool enabled(const VitaVk* v, const char* name) {
    for (uint32_t i = 0; i < v->extension_count; i++)
        if (!strcmp(v->extensions[i], name)) return true;
    return false;
}

int main(void) {
    static char log[65536];
    char why[512];
    if (!vita_vk_init(NULL, why, sizeof why)) {
        printf("SKIP no Vulkan device (%s)\n", why);
        return 77;
    }
    bool local_read = enabled(vita_vk(), LOCAL_READ);
    vita_vk_free();
    if (!local_read) {
        puts("SKIP the device has no " LOCAL_READ);
        return 77;
    }
    ini_read(ini_find(g_debug_ini, "vk_debug_hide_commands"), "vkCmdSetRenderingAttachmentLocationsKHR");
    CHECK(init_said(log, sizeof log, why, sizeof why));
    CHECK(vita_vk() && !vita_vk()->local_read && !enabled(vita_vk(), LOCAL_READ));
    CHECK(strstr(log, "lists " LOCAL_READ " without vkCmdSetRenderingAttachmentLocationsKHR, which it requires "
                      "(vk_debug_hide_commands hides it from the check): the device is made again without it"));
    const char* off = strstr(log, "vulkan: " LOCAL_READ " is off on ");
    CHECK(off && strstr(off, "its driver lists it without vkCmdSetRenderingAttachmentLocationsKHR"));
    vita_vk_free();
    /* the extension's own setting, which the Vita now takes as a Switch game does */
    ini_read(ini_find(g_debug_ini, "vk_debug_hide_commands"), "");
    ini_read(ini_find(g_vk_extensions_ini, "vk_khr_dynamic_rendering_local_read"), "off");
    CHECK(vita_vk_init(NULL, why, sizeof why));
    CHECK(!vita_vk()->local_read && !enabled(vita_vk(), LOCAL_READ));
    vita_vk_free();
    puts("PASS the Vita's device without " LOCAL_READ ", by the check and by its setting");
    return 0;
}
