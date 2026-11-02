/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/vpk.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/zip.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char* vita_vpk_member(const char* vpk, const char* name, size_t* size, char* error, size_t error_size) {
    return snx_zip_member(vpk, name, size, error, error_size);
}
bool vita_vpk_extract(const char* vpk, const char* folder, char* error, size_t error_size) {
    if (error_size) error[0] = 0;
    SnxZip* zip = snx_zip_open(vpk, error, error_size);
    if (!zip) return false;
    bool ok = !snx_mkdirs(folder);
    if (!ok) snprintf(error, error_size, "cannot create %s", folder);
    for (uint32_t i = 0; ok && i < snx_zip_count(zip); ++i) {
        const char* name = snx_zip_name(zip, i);
        if (!snx_zip_safe_name(name)) {
            snprintf(error, error_size, "%s names a file outside the game's folder: %s", vpk, name);
            ok = false;
            break;
        }
        char* path = snx_path_join(folder, name);
        size_t n = strlen(name);
        if (name[n - 1] == '/') { /* a folder */
            ok = path && !snx_mkdirs(path);
            if (!ok) snprintf(error, error_size, "cannot create %s", path ? path : name);
            free(path);
            continue;
        }
        char* parent = path ? snx_dirname(path) : NULL;
        size_t size = 0;
        unsigned char* data = parent && !snx_mkdirs(parent) ? snx_zip_read(zip, i, &size, error, error_size) : NULL;
        if (!data && parent && !error[0]) snprintf(error, error_size, "cannot create %s", parent);
        ok = data && !snx_write_file(path, data, size);
        if (data && !ok) {
            char why[256];
            snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(why, sizeof why));
        }
        free(data);
        free(parent);
        free(path);
    }
    snx_zip_close(zip);
    return ok;
}
char* vita_sfo_string(const unsigned char* sfo, size_t size, const char* key) {
    /* "\0PSF", version, the key table's offset, the data table's, the entries (16 bytes each from 20) */
    if (size < 20 || snx_ld_le32(sfo) != 0x46535000u) return NULL;
    uint32_t keys = snx_ld_le32(sfo + 8), data = snx_ld_le32(sfo + 12), count = snx_ld_le32(sfo + 16);
    if (keys > size || data > size || count > (size - 20) / 16) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* e = sfo + 20 + i * 16;
        uint32_t name = keys + snx_ld_le16(e), format = snx_ld_le16(e + 2), length = snx_ld_le32(e + 4),
                 at = data + snx_ld_le32(e + 12);
        if (name >= size || memchr(sfo + name, 0, size - name) == NULL || strcmp((const char*)sfo + name, key))
            continue;
        if (format != 0x0204 || at > size || length > size - at || !length) return NULL;
        char* value = malloc(length + 1);
        if (!value) return NULL;
        memcpy(value, sfo + at, length);
        value[length] = 0;
        return value;
    }
    return NULL;
}
bool vita_title_id(const char* text) {
    if (!text || strlen(text) != 9) return false;
    for (unsigned i = 0; i < 9; ++i)
        if (i < 4 ? !(text[i] >= 'A' && text[i] <= 'Z') : !(text[i] >= '0' && text[i] <= '9')) return false;
    return true;
}
