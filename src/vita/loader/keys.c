/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/keys.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char* name;
    unsigned size;
} key_table[VITA_KEY_COUNT] = {
    [VITA_KEY_PFS_HMAC0] = {"pfs_hmac_key0", 20},
    [VITA_KEY_PFS_HMAC1] = {"pfs_hmac_key1", 20},
    [VITA_KEY_PFS_IV0] = {"pfs_iv0", 16},
    [VITA_KEY_PFS_CONTRACT0] = {"pfs_contract_key0", 16},
    [VITA_KEY_NPDRM0] = {"npdrm_key0", 16},
    [VITA_KEY_NPDRM1] = {"npdrm_key1", 16},
    [VITA_KEY_NPDRM_IV] = {"npdrm_iv", 16},
    [VITA_KEY_SELF0_169] = {"self_app_key0_169", 32},
    [VITA_KEY_SELF0_169_IV] = {"self_app_iv0_169", 16},
    [VITA_KEY_SELF0] = {"self_app_key0", 32},
    [VITA_KEY_SELF0_IV] = {"self_app_iv0", 16},
    [VITA_KEY_SELF1] = {"self_app_key1", 32},
    [VITA_KEY_SELF1_IV] = {"self_app_iv1", 16},
    [VITA_KEY_SELF2] = {"self_app_key2", 32},
    [VITA_KEY_SELF2_IV] = {"self_app_iv2", 16},
    [VITA_KEY_SELF3] = {"self_app_key3", 32},
    [VITA_KEY_SELF3_IV] = {"self_app_iv3", 16},
    [VITA_KEY_SELF4] = {"self_app_key4", 32},
    [VITA_KEY_SELF4_IV] = {"self_app_iv4", 16},
    [VITA_KEY_SELF5] = {"self_app_key5", 32},
    [VITA_KEY_SELF5_IV] = {"self_app_iv5", 16},
};
static_assert(VITA_KEY_COUNT <= 32, "VitaKeys.have holds a bit for each key");

const char* vita_key_name(VitaKeyName key) { return key < VITA_KEY_COUNT ? key_table[key].name : "?"; }
size_t vita_key_size(VitaKeyName key) { return key < VITA_KEY_COUNT ? key_table[key].size : 0; }

bool vita_keys_parse(const char* text, size_t size, const char* path, VitaKeys* keys, char* error, size_t error_size) {
    *keys = (VitaKeys){0};
    snprintf(keys->path, sizeof keys->path, "%s", path);
    unsigned line_of[VITA_KEY_COUNT] = {0}, number = 0;
    for (size_t at = 0; at < size;) {
        const char *line = text + at, *end = memchr(line, '\n', size - at);
        size_t n = end ? (size_t)(end - line) : size - at, i = 0;
        at += n + 1;
        ++number;
        while (n && snx_is_space((unsigned char)line[n - 1])) --n; /* CR and trailing blanks */
        while (i < n && snx_is_blank((unsigned char)line[i])) ++i;
        if (i == n || line[i] == '#') continue;
        size_t name = i;
        while (i < n && (snx_is_alnum((unsigned char)line[i]) || line[i] == '_')) ++i;
        size_t name_length = i - name;
        while (i < n && snx_is_blank((unsigned char)line[i])) ++i;
        if (!name_length || i == n || line[i] != '=') {
            snprintf(error, error_size, "%s:%u: \"%.*s\" is not a line name = hex", path, number,
                     (int)(n > 80 ? 80 : n), line);
            return false;
        }
        for (++i; i < n && snx_is_blank((unsigned char)line[i]); ++i) continue;
        size_t hex = i;
        while (i < n && snx_is_hex((unsigned char)line[i])) ++i;
        size_t digits = i - hex;
        unsigned key = 0;
        while (key < VITA_KEY_COUNT &&
               (strlen(key_table[key].name) != name_length || memcmp(key_table[key].name, line + name, name_length)))
            ++key;
        if (key == VITA_KEY_COUNT) {
            snprintf(
                error, error_size,
                "%s:%u: %.*s is no Vita key (pfs_hmac_key0, pfs_hmac_key1, pfs_iv0, pfs_contract_key0, npdrm_key0, "
                "npdrm_key1, npdrm_iv, self_app_key0_169 and self_app_iv0_169, self_app_key0 to 5 and "
                "self_app_iv0 to 5)",
                path, number, (int)(name_length > 64 ? 64 : name_length), line + name);
            return false;
        }
        if (i != n || digits != 2u * key_table[key].size) {
            snprintf(error, error_size, "%s:%u: %s is %u bytes, %u hex digits, and the line has %s%zu", path, number,
                     key_table[key].name, key_table[key].size, 2 * key_table[key].size,
                     i != n ? "something else after " : "", digits);
            return false;
        }
        if (line_of[key]) {
            snprintf(error, error_size, "%s:%u: %s is given on line %u too", path, number, key_table[key].name,
                     line_of[key]);
            return false;
        }
        line_of[key] = number;
        for (size_t b = 0; b < key_table[key].size; ++b)
            keys->bytes[key][b] = (unsigned char)(snx_hex_value((unsigned char)line[hex + 2 * b]) << 4 |
                                                  snx_hex_value((unsigned char)line[hex + 2 * b + 1]));
        keys->have |= 1u << key;
    }
    return true;
}
bool vita_keys_load(const char* path, VitaKeys* keys, char* error, size_t error_size) {
    size_t size = 0;
    unsigned char* text = path ? snx_read_file(path, &size) : NULL;
    if (!text) {
        char why[256];
        snprintf(error, error_size,
                 "there are no Vita keys to decrypt it with at %s (%s): Install Keys puts " VITA_KEYS_FILE
                 " in ~/.vita, and docs/platforms/vita/README.md says which keys it holds",
                 path ? path : "~/.vita/" VITA_KEYS_FILE, path ? snx_os_error(why, sizeof why) : "no home folder");
        *keys = (VitaKeys){0};
        return false;
    }
    bool ok = vita_keys_parse((const char*)text, size, path, keys, error, error_size);
    free(text);
    return ok;
}
const unsigned char* vita_key(const VitaKeys* keys, VitaKeyName key, const char* purpose, char* error,
                              size_t error_size) {
    if (key < VITA_KEY_COUNT && keys->have >> key & 1) return keys->bytes[key];
    snprintf(error, error_size, "%s has no %s (%u bytes), which %s needs", keys->path[0] ? keys->path : VITA_KEYS_FILE,
             vita_key_name(key), (unsigned)vita_key_size(key), purpose);
    return NULL;
}
char* vita_keys_default_path(void) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    char* folder = home && home[0] ? snx_path_join(home, ".vita") : NULL;
    char* path = folder ? snx_path_join(folder, VITA_KEYS_FILE) : NULL;
    free(folder);
    return path;
}
