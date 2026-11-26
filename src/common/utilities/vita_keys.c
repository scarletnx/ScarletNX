/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/vita_keys.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char* file;
    unsigned size;
} key_files[SNX_VITA_KEY_COUNT] = {
    [SNX_VITA_KEY_PFS_HMAC0] = {"pfs-hmac-key-0", 20}, [SNX_VITA_KEY_PFS_HMAC1] = {"pfs-hmac-key-1", 20},
    [SNX_VITA_KEY_PFS_IV0] = {"pfs-iv-0", 16},         [SNX_VITA_KEY_PFS_CONTRACT0] = {"pfs-contract-key-0", 16},
    [SNX_VITA_KEY_NPDRM0] = {"npdrm-key-0", 16},       [SNX_VITA_KEY_NPDRM0_IV] = {"npdrm-iv-0", 16},
    [SNX_VITA_KEY_NPDRM1] = {"npdrm-key-1", 16},       [SNX_VITA_KEY_NPDRM1_IV] = {"npdrm-iv-1", 16},
    [SNX_VITA_KEY_APP0_100] = {"app-key-0-100", 32},   [SNX_VITA_KEY_APP0_100_IV] = {"app-iv-0-100", 16},
    [SNX_VITA_KEY_APP0_180] = {"app-key-0-180", 32},   [SNX_VITA_KEY_APP0_180_IV] = {"app-iv-0-180", 16},
    [SNX_VITA_KEY_APP1] = {"app-key-1", 32},           [SNX_VITA_KEY_APP1_IV] = {"app-iv-1", 16},
    [SNX_VITA_KEY_APP2] = {"app-key-2", 32},           [SNX_VITA_KEY_APP2_IV] = {"app-iv-2", 16},
    [SNX_VITA_KEY_APP3] = {"app-key-3", 32},           [SNX_VITA_KEY_APP3_IV] = {"app-iv-3", 16},
    [SNX_VITA_KEY_APP4] = {"app-key-4", 32},           [SNX_VITA_KEY_APP4_IV] = {"app-iv-4", 16},
    [SNX_VITA_KEY_APP5] = {"app-key-5", 32},           [SNX_VITA_KEY_APP5_IV] = {"app-iv-5", 16},
};

const char* snx_vita_key_file(int key) { return key >= 0 && key < SNX_VITA_KEY_COUNT ? key_files[key].file : NULL; }
size_t snx_vita_key_size(int key) { return key >= 0 && key < SNX_VITA_KEY_COUNT ? key_files[key].size : 0; }
int snx_vita_key_named(const char* name) {
    for (int key = 0; name && key < SNX_VITA_KEY_COUNT; ++key)
        if (!strcmp(key_files[key].file, name)) return key;
    return -1;
}
char* snx_vita_keys_dir(void) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    return home && home[0] ? snx_path_join(home, ".vita") : NULL;
}
bool snx_vita_keys_parse(const char* text, size_t size, const char* path, unsigned char (*bytes)[32], unsigned* have,
                         void (*unknown)(void* context, const char* name, unsigned line), void* context, char* why,
                         size_t why_size) {
    unsigned line_of[SNX_VITA_KEY_COUNT] = {0}, number = 0;
    for (size_t at = 0; at < size;) {
        const char *line = text + at, *end = memchr(line, '\n', size - at);
        size_t n = end ? (size_t)(end - line) : size - at, i = 0;
        at += n + 1;
        ++number;
        while (n && snx_is_space((unsigned char)line[n - 1])) --n; /* CR and trailing blanks */
        while (i < n && snx_is_blank((unsigned char)line[i])) ++i;
        if (i == n || line[i] == '#') continue;
        size_t name = i;
        while (i < n && (snx_is_alnum((unsigned char)line[i]) || line[i] == '_' || line[i] == '-')) ++i;
        size_t name_length = i - name;
        while (i < n && snx_is_blank((unsigned char)line[i])) ++i;
        if (!name_length || name_length > 63 || i == n || line[i] != '=') {
            snprintf(why, why_size, "%s:%u: \"%.*s\" is not a line name = hex", path, number, (int)(n > 80 ? 80 : n),
                     line);
            return false;
        }
        char key_name[64];
        snprintf(key_name, sizeof key_name, "%.*s", (int)name_length, line + name);
        for (++i; i < n && snx_is_blank((unsigned char)line[i]); ++i) continue;
        size_t hex = i;
        while (i < n && snx_is_hex((unsigned char)line[i])) ++i;
        int key = snx_vita_key_named(key_name);
        if (key < 0) {
            if (unknown) unknown(context, key_name, number);
            continue;
        }
        size_t digits = i - hex, want = key_files[key].size;
        if (i != n) {
            snprintf(why, why_size, "%s:%u: %s's value has something besides hex digits", path, number, key_name);
            return false;
        }
        if (digits != 2 * want) {
            snprintf(why, why_size, "%s:%u: %s is %zu bytes, and the line gives %zu hex digits, %zu%s bytes", path,
                     number, key_name, want, digits, digits / 2, digits % 2 ? " and a half" : "");
            return false;
        }
        if (line_of[key]) {
            snprintf(why, why_size, "%s:%u: %s is given on line %u too", path, number, key_name, line_of[key]);
            return false;
        }
        line_of[key] = number;
        for (size_t b = 0; b < want; ++b)
            bytes[key][b] = (unsigned char)(snx_hex_value((unsigned char)line[hex + 2 * b]) << 4 |
                                            snx_hex_value((unsigned char)line[hex + 2 * b + 1]));
        *have |= 1u << key;
    }
    return true;
}
