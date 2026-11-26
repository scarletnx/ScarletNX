/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/keys.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool vita_keys_load(const char* path, VitaKeys* keys, char* error, size_t error_size) {
    *keys = (VitaKeys){0};
    if (!path || !snx_is_dir(path)) {
        char why[256];
        snprintf(error, error_size,
                 "there are no Vita keys to decrypt it with in %s (%s): Install Keys puts them in ~/.vita, a file for "
                 "each key, and docs/platforms/vita/README.md names them",
                 path ? path : "~/.vita",
                 !path                   ? "no home folder"
                 : snx_path_exists(path) ? "it is no folder"
                                         : snx_os_error(why, sizeof why));
        return false;
    }
    snprintf(keys->path, sizeof keys->path, "%s", path);
    for (int key = 0; key < SNX_VITA_KEY_COUNT; ++key) {
        char file[SNX_PATH_SIZE];
        size_t size = 0;
        if (snx_path_format(file, sizeof file, "%s/%s", path, snx_vita_key_file(key)) || !snx_is_file(file)) continue;
        unsigned char* bytes = snx_read_file(file, &size);
        if (!bytes || size != snx_vita_key_size(key)) {
            snprintf(error, error_size, "%s is %zu bytes, and the key %s is %zu", file, bytes ? size : 0,
                     snx_vita_key_file(key), snx_vita_key_size(key));
            free(bytes);
            return false;
        }
        memcpy(keys->bytes[key], bytes, size);
        keys->have |= 1u << key;
        free(bytes);
    }
    return true;
}
const unsigned char* vita_key(const VitaKeys* keys, SnxVitaKey key, const char* purpose, char* error,
                              size_t error_size) {
    if ((unsigned)key < SNX_VITA_KEY_COUNT && keys->have >> key & 1) return keys->bytes[key];
    snprintf(error, error_size, "%s has no %s (a file of %zu bytes), which %s needs",
             keys->path[0] ? keys->path : "~/.vita", snx_vita_key_file(key), snx_vita_key_size(key), purpose);
    return NULL;
}
