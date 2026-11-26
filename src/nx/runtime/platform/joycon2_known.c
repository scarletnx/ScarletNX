/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/joycon2_known.h"
#include "common/utilities/ascii.h"
#include "common/utilities/common.h"
#include "common/utilities/config_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the controller a line of n bytes names: 1, else 0 */
static int known_line(const char* line, size_t n, Jc2KnownLine* k) {
    char text[128], side = 0;
    if (n < 19 || n >= sizeof text) return 0;
    memcpy(text, line, n);
    text[n] = 0;
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 6; i++) {
        int hi = snx_hex_value(text[3 * i]), lo = snx_hex_value(text[3 * i + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && text[3 * i + 2] != ':')) return 0;
        k->address[i] = (uint8_t)(hi << 4 | lo);
    }
    if (sscanf(text + 17, " %c %16s", &side, k->serial) < 1 || (side != 'L' && side != 'R' && side != 'P')) return 0;
    k->side = side;
    return 1;
}
/* the next line of text at *at, its length without its line end in *n; NULL at the end */
static const char* next_line(const char* text, size_t* at, size_t* n) {
    const char* start = text + *at;
    size_t len = strcspn(start, "\n");
    if (!*start) return NULL;
    *at += len + (start[len] == '\n');
    *n = len && start[len - 1] == '\r' ? len - 1 : len;
    return start;
}
int joycon2_known_load(const char* path, Jc2KnownLine* lines, int max) {
    char *text, why[1024], said[1024];
    size_t size, at = 0, n;
    const char* line;
    int count = 0, number = 0;
    SnxConfigOthers others = {0};
    if (snx_config_read(path, &text, &size, why, sizeof why) < 0) {
        if (snx_config_first(path, "cannot be read")) snx_error("joycon2: %s: no controller is known", why);
        return 0;
    }
    while (text && (line = next_line(text, &at, &n)) != NULL) {
        Jc2KnownLine k;
        ++number;
        if (!n) continue;
        if (!known_line(line, n, &k)) /* another version's or a hand's: kept by a write, named */
            snx_config_other_key(&others, "", "", number);
        else if (count < max)
            lines[count++] = k;
    }
    if (snx_config_others_said(&others, path, said, sizeof said)) snx_error("joycon2: %s", said);
    free(text);
    return count;
}
int joycon2_known_save(const char* path, const Jc2KnownLine* lines, int n) {
    char *was = NULL, *text, why[1024];
    size_t size = 0, at = 0, len, out = 0, room;
    const char* line;
    int rc;
    if (snx_config_read(path, &was, &size, why, sizeof why) < 0) {
        snx_error("joycon2: %s: the controllers known now are not written", why);
        return -1;
    }
    room = size + (size_t)n * 40 + 2;
    text = (char*)snx_alloc(room);
    for (int i = 0; i < n; i++) {
        const uint8_t* a = lines[i].address;
        out += (size_t)snprintf(text + out, room - out, "%02X:%02X:%02X:%02X:%02X:%02X %c %s\n", a[0], a[1], a[2], a[3],
                                a[4], a[5], lines[i].side, lines[i].serial);
    }
    while (was && (line = next_line(was, &at, &len)) != NULL) { /* the lines that name no controller, as they were */
        Jc2KnownLine k;
        if (!len || known_line(line, len, &k)) continue;
        memcpy(text + out, line, len);
        out += len;
        text[out++] = '\n';
    }
    rc = snx_config_write(path, ".new", text, out, why, sizeof why);
    if (rc) snx_error("joycon2: %s", why);
    free(text);
    free(was);
    return rc;
}
