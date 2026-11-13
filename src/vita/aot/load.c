/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/aot/module.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/platform/dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool vita_module_compatible(const VitaModule* m, const VitaImage* image) {
    return vita_module_fields(m, image) && m->keys && m->blocks;
}
bool vita_module_fields(const VitaModule* m, const VitaImage* image) {
    return m && image && m->magic == VITA_MODULE_MAGIC && m->abi == VITA_MODULE_ABI &&
           m->descriptor_size == sizeof *m && m->cpu_size == sizeof(SnxArmCPU) && m->arm_abi == SNX_ARM_ABI &&
           m->pointer_size == sizeof(void*) && !memcmp(m->identity, image->identity, sizeof m->identity);
}
static int descriptor_fields(const unsigned char* record, const void* image) {
    VitaModule fields;
    memcpy(&fields, record, offsetof(VitaModule, keys));
    return vita_module_fields(&fields, image);
}
bool vita_module_current(const char* path, const VitaImage* image) {
    const uint64_t magic = VITA_MODULE_MAGIC;
    return path && image &&
           snx_file_has_record(path, &magic, sizeof magic, offsetof(VitaModule, keys), descriptor_fields, image);
}
SnxArmBlock vita_module_block(const VitaModule* m, uint32_t key) {
    size_t lo = 0, hi = m ? m->count : 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->keys[mid] == key) return m->blocks[mid];
        if (m->keys[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
bool vita_native_open(const char* path, const VitaImage* image, VitaNative* native, char* error, size_t error_size) {
    *native = (VitaNative){0};
    native->library = plat_dl_open(path);
    if (!native->library) {
        snprintf(error, error_size, "cannot load %s: %s", path, plat_dl_error());
        return false;
    }
    native->module = plat_dl_sym(native->library, "snx_vita_module_v1");
    if (!vita_module_compatible(native->module, image)) {
        snprintf(error, error_size, "%s was compiled by another build or from another eboot.bin", path);
        vita_native_close(native);
        return false;
    }
    return true;
}
void vita_native_close(VitaNative* native) {
    if (native->library) plat_dl_close(native->library);
    *native = (VitaNative){0};
}
static int by_key(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}
/* keys sorted, each once: the new count */
static size_t unique(uint32_t* keys, size_t count) {
    size_t n = 0;
    if (count) qsort(keys, count, sizeof *keys, by_key);
    for (size_t i = 0; i < count; ++i)
        if (!n || keys[n - 1] != keys[i]) keys[n++] = keys[i];
    return n;
}
uint32_t* vita_hints_read(const char* folder, size_t* count) {
    char* path = vita_game_file(folder, VITA_HINTS_SUFFIX);
    uint32_t* hints = vita_hints_read_file(path, count);
    free(path);
    return hints;
}
uint32_t* vita_hints_read_file(const char* path, size_t* count) {
    FILE* f = path ? fopen(path, "r") : NULL;
    uint32_t* hints = NULL;
    size_t capacity = 0;
    unsigned value;
    *count = 0;
    while (f && fscanf(f, "%x", &value) == 1) {
        if (*count == capacity) {
            uint32_t* grown = realloc(hints, (capacity = capacity ? capacity * 2 : 1024) * sizeof *grown);
            if (!grown) break;
            hints = grown;
        }
        hints[(*count)++] = value;
    }
    if (f) fclose(f);
    *count = unique(hints, *count);
    return hints;
}
size_t vita_hints_write(const char* folder, uint32_t* keys, size_t count) {
    char *path = vita_game_file(folder, VITA_HINTS_SUFFIX), *staged = path ? snx_format("%s.tmp", path) : NULL;
    FILE* f = staged ? fopen(staged, "w") : NULL;
    bool ok = f != NULL;
    count = unique(keys, count);
    for (size_t i = 0; ok && i < count; ++i) ok = fprintf(f, "%08x\n", (unsigned)keys[i]) > 0;
    if (f && fclose(f)) ok = false;
    ok = ok && !plat_replace_file(staged, path);
    if (!ok && staged) remove(staged);
    free(staged);
    free(path);
    return ok ? count : SIZE_MAX;
}
char* vita_game_file(const char* directory, const char* suffix) {
    char* folder = snx_absolute(directory);
    if (!folder) folder = snx_strdup(directory);
    if (!folder) return NULL;
    size_t n = strlen(folder), cut;
    while (n > 1 && (folder[n - 1] == '/' || folder[n - 1] == '\\')) folder[--n] = 0;
    for (cut = n; cut && folder[cut - 1] != '/' && folder[cut - 1] != '\\'; --cut) continue;
    char file[SNX_PATH_SIZE];
    int cut_short = snx_path_format(file, sizeof file, "%s/%s%s", folder, folder + cut, suffix);
    free(folder);
    return cut_short ? NULL : snx_strdup(file);
}
