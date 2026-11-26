/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/keys.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    VitaKeysNote note;
    void* context;
    const char* path;
} Told;
static void unknown_name(void* context, const char* name, unsigned line) {
    const Told* t = context;
    char said[SNX_PATH_SIZE + 160];
    snprintf(said, sizeof said, "%s:%u: %s is no Vita key the import reads, and the line is passed over", t->path, line,
             name);
    if (t->note) t->note(t->context, said);
}
/* the vita.keys at path over keys: its lines win, a key it gives differently from a key file said */
static bool file_keys(const char* path, VitaKeys* keys, VitaKeysNote note, void* context, char* error,
                      size_t error_size) {
    size_t size = 0;
    unsigned char* text = snx_read_file(path, &size);
    unsigned char bytes[SNX_VITA_KEY_COUNT][32] = {{0}};
    unsigned have = 0;
    Told told = {note, context, path};
    if (!text) {
        char why[256];
        snprintf(error, error_size, "cannot read %s: %s", path, snx_os_error(why, sizeof why));
        return false;
    }
    bool ok = snx_vita_keys_parse((const char*)text, size, path, bytes, &have, unknown_name, &told, error, error_size);
    free(text);
    for (int key = 0; ok && key < SNX_VITA_KEY_COUNT; ++key) {
        if (!(have >> key & 1)) continue;
        if (keys->have >> key & 1 && memcmp(keys->bytes[key], bytes[key], snx_vita_key_size(key)) && note) {
            char said[SNX_PATH_SIZE + 160];
            snprintf(said, sizeof said, "%s gives %s another value than the key file %s beside it: %s's is taken", path,
                     snx_vita_key_file(key), snx_vita_key_file(key), SNX_VITA_KEYS_FILE);
            note(context, said);
        }
        memcpy(keys->bytes[key], bytes[key], snx_vita_key_size(key));
        keys->have |= 1u << key;
    }
    return ok;
}
bool vita_keys_load(const char* path, VitaKeys* keys, VitaKeysNote note, void* context, char* error,
                    size_t error_size) {
    *keys = (VitaKeys){0};
    if (path && snx_is_file(path)) { /* a vita.keys given itself */
        snprintf(keys->path, sizeof keys->path, "%s", path);
        return file_keys(path, keys, note, context, error, error_size);
    }
    if (!path || !snx_is_dir(path)) {
        char why[256];
        snprintf(error, error_size,
                 "there are no Vita keys to decrypt it with in %s (%s): Install Keys puts " SNX_VITA_KEYS_FILE
                 " there, a line name = hex for each key, and docs/platforms/vita/README.md names them",
                 path ? path : "~/.vita", !path ? "no home folder" : snx_os_error(why, sizeof why));
        return false;
    }
    snprintf(keys->path, sizeof keys->path, "%s", path);
    for (int key = 0; key < SNX_VITA_KEY_COUNT; ++key) { /* vitakeys' way: a file of raw bytes for each */
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
    char file[SNX_PATH_SIZE];
    if (!snx_path_format(file, sizeof file, "%s/" SNX_VITA_KEYS_FILE, path) && snx_is_file(file))
        return file_keys(file, keys, note, context, error, error_size);
    return true;
}
const unsigned char* vita_key(const VitaKeys* keys, SnxVitaKey key, const char* purpose, char* error,
                              size_t error_size) {
    if ((unsigned)key < SNX_VITA_KEY_COUNT && keys->have >> key & 1) return keys->bytes[key];
    snprintf(error, error_size, "%s has no %s (%zu bytes: a line %s = hex in " SNX_VITA_KEYS_FILE "), which %s needs",
             keys->path[0] ? keys->path : "~/.vita", snx_vita_key_file(key), snx_vita_key_size(key),
             snx_vita_key_file(key), purpose);
    return NULL;
}
