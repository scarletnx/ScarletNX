/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu_driver.h. meta.json's fields and its size limit, and the check of a device that loads packages, after Eden's
   GpuDriverMetadata.kt and native.cpp (SupportsCustomDriver); the rest is ScarletNX's */
#include "common/utilities/gpu_driver.h"
#include "common/utilities/common.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <android/api-level.h>
#endif

/* a field's text: a string as it is, a number as JSON writes it; 1, else 0 with why naming the key, its value and why */
static int field(const cJSON* root, const char* key, int needed, char* out, size_t size, char* why, size_t why_size) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    const char* text = NULL;
    char number[64], *shown;
    out[0] = 0;
    if (!item) {
        if (needed) snprintf(why, why_size, "meta.json has no %s", key);
        return !needed;
    }
    if (cJSON_IsString(item))
        text = item->valuestring;
    else if (cJSON_IsNumber(item)) {
        snprintf(number, sizeof number, "%.17g", item->valuedouble);
        text = number;
    } else {
        shown = cJSON_PrintUnformatted(item);
        snprintf(why, why_size, "meta.json's %s is %s, not a text", key, shown ? shown : "a value it cannot write");
        cJSON_free(shown);
        return 0;
    }
    if (strlen(text) >= size) {
        snprintf(why, why_size, "meta.json's %s is %zu bytes, over the %zu it takes", key, strlen(text), size - 1);
        return 0;
    }
    if (needed && !text[0]) {
        snprintf(why, why_size, "meta.json's %s is empty", key);
        return 0;
    }
    memcpy(out, text, strlen(text) + 1);
    return 1;
}

int snx_gpu_driver_meta(const char* text, size_t n, SnxGpuDriverMeta* meta, char* why, size_t size) {
    char* copy;
    const char* end = NULL;
    cJSON *root, *api;
    char* shown;
    int ok;
    memset(meta, 0, sizeof *meta);
    if (n > SNX_GPU_DRIVER_META_MAX) {
        snprintf(why, size, "meta.json is %zu bytes, over the %d a driver's takes", n, SNX_GPU_DRIVER_META_MAX);
        return 0;
    }
    copy = (char*)malloc(n + 1);
    if (!copy) {
        snprintf(why, size, "no memory to read meta.json's %zu bytes", n);
        return 0;
    }
    memcpy(copy, text, n);
    copy[n] = 0;
    root = cJSON_ParseWithOpts(copy, &end, 1);
    if (!root || strlen(copy) != n) {
        size_t at = strlen(copy) != n ? strlen(copy) : end ? (size_t)(end - copy) : 0;
        snprintf(why, size, "meta.json is not JSON near byte %zu of %zu", at, n);
        cJSON_Delete(root);
        free(copy);
        return 0;
    }
    free(copy);
    if (!cJSON_IsObject(root)) {
        snprintf(why, size, "meta.json is not a JSON object");
        cJSON_Delete(root);
        return 0;
    }
    ok = field(root, "name", 1, meta->name, sizeof meta->name, why, size) &&
         field(root, "libraryName", 1, meta->library, sizeof meta->library, why, size) &&
         field(root, "description", 0, meta->description, sizeof meta->description, why, size) &&
         field(root, "author", 0, meta->author, sizeof meta->author, why, size) &&
         field(root, "vendor", 0, meta->vendor, sizeof meta->vendor, why, size) &&
         field(root, "driverVersion", 0, meta->driver_version, sizeof meta->driver_version, why, size) &&
         field(root, "packageVersion", 0, meta->package_version, sizeof meta->package_version, why, size);
    api = cJSON_GetObjectItemCaseSensitive(root, "minApi");
    if (ok && !api) {
        snprintf(why, size, "meta.json has no minApi");
        ok = 0;
    } else if (ok && (!cJSON_IsNumber(api) || api->valuedouble < 0 || api->valuedouble > 1000 ||
                      api->valuedouble != (double)(int)api->valuedouble)) {
        shown = cJSON_PrintUnformatted(api);
        snprintf(why, size, "meta.json's minApi is %s, not an Android API level (a whole number)",
                 shown ? shown : "a value it cannot write");
        cJSON_free(shown);
        ok = 0;
    } else if (ok)
        meta->min_api = (int)api->valuedouble;
    /* the hook opens it by name in the package's folder: a file name, which a path in it would leave */
    if (ok && (strchr(meta->library, '/') || strchr(meta->library, '\\') || meta->library[0] == '.' ||
               !snx_ends_with_nocase(meta->library, ".so") || strlen(meta->library) < 4)) {
        snprintf(why, size,
                 "meta.json's libraryName \"%s\" is not a library's file name (a name ending in .so, no "
                 "folder)",
                 meta->library);
        ok = 0;
    }
    cJSON_Delete(root);
    return ok;
}

char* snx_gpu_drivers_dir(void) {
    char *data = snx_user_dir(SNX_USER_DATA), *dir = data ? snx_path_join(data, SNX_GPU_DRIVERS_DIR) : NULL;
    free(data);
    return dir;
}

static int id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '_';
}
int snx_gpu_driver_id_valid(const char* id) {
    size_t i, n = id ? strlen(id) : 0;
    if (!n || n > 96 || id[0] == '.') return 0;
    for (i = 0; i < n; ++i)
        if (!id_char(id[i])) return 0;
    return 1;
}
int snx_gpu_driver_id(const char* file, char* id, size_t size, char* why, size_t why_size) {
    const char* leaf = file;
    size_t i, n;
    for (const char* s = file; *s; ++s)
        if (*s == '/' || *s == '\\' || *s == ':') leaf = s + 1;
    n = strlen(leaf);
    if (n >= 10 && snx_ends_with_nocase(leaf, ".adpkg.zip"))
        n -= 10;
    else if (n >= 4 && snx_ends_with_nocase(leaf, ".zip"))
        n -= 4;
    while (n && leaf[0] == '.') { /* a hidden name stays visible: the leading dots go */
        ++leaf;
        --n;
    }
    if (!n || n > 96 || n >= size) {
        snprintf(why, why_size, "\"%s\" makes no folder name: %zu characters, from 1 to %d", file, n,
                 size - 1 < 96 ? (int)(size - 1) : 96);
        return 0;
    }
    for (i = 0; i < n; ++i) id[i] = id_char(leaf[i]) ? leaf[i] : '_';
    id[n] = 0;
    return 1;
}

int snx_gpu_driver_elf(const unsigned char* head, size_t n, char* why, size_t size) {
    unsigned machine;
    if (n < 20 || memcmp(head, "\177ELF", 4)) {
        snprintf(why, size, "is not an ELF library (it starts %02x %02x %02x %02x)", n > 0 ? head[0] : 0,
                 n > 1 ? head[1] : 0, n > 2 ? head[2] : 0, n > 3 ? head[3] : 0);
        return 0;
    }
    machine = head[5] == 2 ? (unsigned)head[18] << 8 | head[19] : (unsigned)head[19] << 8 | head[18];
    if (head[4] != 2 || machine != 183) {
        snprintf(why, size, "is a %d-bit library for machine %u, not arm64's (64-bit, machine 183)",
                 head[4] == 1   ? 32
                 : head[4] == 2 ? 64
                                : 0,
                 machine);
        return 0;
    }
    return 1;
}

void snx_gpu_driver_host(SnxGpuDriverHost* host) {
    memset(host, 0, sizeof *host);
#if defined(__ANDROID__)
    host->system = "Android";
    host->api = android_get_device_api_level();
#elif defined(_WIN32)
    host->system = "Windows";
#elif defined(__APPLE__)
#if defined(SNX_IOS)
    host->system = "iOS";
#else
    host->system = "macOS";
#endif
#elif defined(__linux__)
    host->system = "Linux";
#elif defined(__FreeBSD__)
    host->system = "FreeBSD";
#else
    host->system = "this system";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    host->arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    host->arch = "x86_64";
#elif defined(__powerpc64__)
    host->arch = "ppc64";
#elif defined(__riscv)
    host->arch = "riscv64";
#else
    host->arch = "this CPU";
#endif
#if defined(__unix__) || defined(__APPLE__)
    host->kgsl = access("/dev/kgsl-3d0", F_OK) == 0;
#endif
#if defined(__ANDROID__) && defined(__aarch64__)
    host->built = 1;
#endif
}

int snx_gpu_driver_loads(const SnxGpuDriverHost* host, char* what, size_t size) {
    char device[160];
    int android = !strcmp(host->system, "Android"), arm64 = !strcmp(host->arch, "arm64");
    if (android)
        snprintf(device, sizeof device, "Android API %d, %s, %s", host->api, host->arch,
                 host->kgsl ? "Adreno GPU (/dev/kgsl-3d0)" : "no /dev/kgsl-3d0 (the GPU is not an Adreno)");
    else
        snprintf(device, sizeof device, "%s %s", host->system, host->arch);
    if (android && arm64 && host->built && host->kgsl && host->api >= SNX_GPU_DRIVER_MIN_API) {
        snprintf(what, size, "This device: %s: custom drivers load.", device);
        return 1;
    }
    if (android && arm64 && host->kgsl && host->api < SNX_GPU_DRIVER_MIN_API)
        snprintf(what, size, "This device: %s: custom drivers load on Android 9 (API %d) or newer.", device,
                 SNX_GPU_DRIVER_MIN_API);
    else if (android && arm64 && host->kgsl)
        snprintf(what, size, "This device: %s: this build of ScarletNX carries no libadrenotools.", device);
    else
        snprintf(what, size,
                 "This device: %s: custom drivers load on Android 9 (API %d) or newer, arm64, with an Adreno GPU "
                 "(/dev/kgsl-3d0). Games here use the system driver.",
                 device, SNX_GPU_DRIVER_MIN_API);
    return 0;
}

int snx_gpu_driver_mapped(const char* maps, const char* dir) {
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') --n;
    for (const char* line = maps; line && *line;) {
        const char* end = strchr(line, '\n');
        const char* path = memchr(line, '/', end ? (size_t)(end - line) : strlen(line));
        size_t len = end ? (size_t)(end - line) : strlen(line);
        /* the path is the line's last field, the only one starting with '/' */
        if (path && (size_t)(line + len - path) > n && !strncmp(path, dir, n) && path[n] == '/') return 1;
        line = end ? end + 1 : NULL;
    }
    return 0;
}

int snx_freedreno_name_valid(const char* name) {
    size_t i, n = strlen(name);
    if (n >= sizeof(((SnxFreedrenoVar*)0)->name) ||
        (strncmp(name, "TU_", 3) && strncmp(name, "FD_", 3) && strncmp(name, "IR3_", 4)))
        return 0;
    for (i = 0; i < n; ++i)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_')) return 0;
    return n > 3 && name[n - 1] != '_';
}

int snx_freedreno_parse(const char* text, SnxFreedrenoVar* vars, int max, char* why, size_t size) {
    int count = 0, i;
    const char* s = text ? text : "";
    while (*s) {
        const char *end = strchr(s, ';'), *eq;
        size_t len = end ? (size_t)(end - s) : strlen(s), name_len;
        SnxFreedrenoVar v = {0};
        while (len && *s == ' ') {
            ++s;
            --len;
        }
        if (!len) {
            s = end ? end + 1 : s + len;
            continue;
        }
        eq = memchr(s, '=', len);
        name_len = eq ? (size_t)(eq - s) : len;
        while (name_len && s[name_len - 1] == ' ') --name_len;
        if (name_len >= sizeof v.name) {
            snprintf(why, size, "\"%.*s\" is %zu bytes, over the %zu a variable's name takes", (int)name_len, s,
                     name_len, sizeof v.name - 1);
            return -1;
        }
        memcpy(v.name, s, name_len);
        if (!snx_freedreno_name_valid(v.name)) {
            snprintf(why, size,
                     "\"%s\" is not one of Freedreno's variables (capitals, digits and _, starting TU_, FD_ "
                     "or IR3_)",
                     v.name);
            return -1;
        }
        if (!eq) {
            snprintf(why, size, "%s has no value: NAME=value", v.name);
            return -1;
        }
        {
            const char* value = eq + 1;
            size_t value_len = len - (size_t)(value - s);
            while (value_len && *value == ' ') {
                ++value;
                --value_len;
            }
            while (value_len && value[value_len - 1] == ' ') --value_len;
            if (value_len >= sizeof v.value) {
                snprintf(why, size, "%s's value is %zu bytes, over the %zu it takes", v.name, value_len,
                         sizeof v.value - 1);
                return -1;
            }
            memcpy(v.value, value, value_len);
        }
        for (i = 0; i < count && strcmp(vars[i].name, v.name); ++i) {}
        if (i == count) {
            if (count == max) {
                snprintf(why, size, "%s is variable %d, over the %d a run sets", v.name, count + 1, max);
                return -1;
            }
            ++count;
        }
        vars[i] = v;
        s = end ? end + 1 : s + len;
    }
    return count;
}

int snx_freedreno_text(const SnxFreedrenoVar* vars, int count, char* out, size_t size) {
    size_t at = 0;
    int i, n;
    if (!size) return 0;
    out[0] = 0;
    for (i = 0; i < count; ++i) {
        n = snprintf(out + at, size - at, "%s%s=%s", i ? ";" : "", vars[i].name, vars[i].value);
        if (n < 0 || (size_t)n >= size - at) {
            out[at] = 0;
            return 0;
        }
        at += (size_t)n;
    }
    return 1;
}
