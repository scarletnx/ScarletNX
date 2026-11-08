/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_module.h"
#include "common/utilities/common.h"
#include "common/platform/dl.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void snx_ctr_native_close(SnxCtrNative* native) {
    if (!native) return;
    if (native->library) plat_dl_close(native->library);
    *native = (SnxCtrNative){0};
}
bool snx_ctr_native_open(const char* path, const SnxCtrImage* image, SnxCtrNative* native, char* error,
                         size_t error_size) {
    if (!native) return false;
    *native = (SnxCtrNative){0};
    if (error_size) error[0] = 0;
    if (!path || !image) {
        snprintf(error, error_size, "invalid native module request");
        return false;
    }
    native->library = plat_dl_open(path);
    if (!native->library) {
        snprintf(error, error_size, "cannot load %s: %s", path, plat_dl_error());
        return false;
    }
    native->module = plat_dl_sym(native->library, "snx_ctr_module_v1");
    if (!snx_ctr_module_compatible(native->module, image)) {
        snprintf(error, error_size, "native module ABI/profile/image identity mismatch");
        snx_ctr_native_close(native);
        return false;
    }
    return true;
}
static int descriptor_fields(const unsigned char* record, const void* image) {
    SnxCtrModule fields;
    memcpy(&fields, record, offsetof(SnxCtrModule, arm_blocks));
    return snx_ctr_module_fields(&fields, image);
}
bool snx_ctr_module_current(const char* path, const SnxCtrImage* image) {
    const uint64_t magic = SNX_CTR_MODULE_MAGIC;
    return path && image &&
           snx_file_has_record(path, &magic, sizeof magic, offsetof(SnxCtrModule, arm_blocks), descriptor_fields, image);
}
char* snx_ctr_game_file(const char* directory, const char* suffix) {
    /* resolved, so "." and a trailing separator name the folder they are */
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
