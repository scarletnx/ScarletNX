/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/game_record.h"
#include "common/utilities/common.h"
#include "common/utilities/config_file.h"
#include "common/utilities/ini_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool snx_ctr_record_read(const char* path, char** source, SnxCtrModel* model, bool* named, char* said, size_t size) {
    char *text = NULL, *key, *value;
    size_t bytes;
    SnxIni ini;
    *source = NULL;
    if (size) said[0] = 0;
    if (snx_config_read(path, &text, &bytes, said, size) < 0) return false;
    if (text) snx_ini_start(&ini, text);
    while (text && snx_ini_next(&ini, &key, &value))
        if (!strcmp(key, "source") && *value) {
            free(*source);
            *source = snx_strdup(value);
        } else if (!strcmp(key, "model") && (!strcmp(value, "old") || !strcmp(value, "new"))) {
            *model = !strcmp(value, "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
            if (named) *named = true;
        } else if (!strcmp(key, "model")) /* a record it does not read: the console it has */
            snprintf(said, size, "model=%s in %s is not old or new: it keeps %s", value, path,
                     *model == SNX_CTR_OLD ? "old" : "new");
    free(text);
    return true;
}
/* whether a line of the record sets one of the keys this file writes, which the new values replace */
static bool own_line(const char* line, size_t n) {
    static const char* const keys[] = {"source", "model", "system"};
    size_t i;
    while (n && (*line == ' ' || *line == '\t')) line++, n--;
    for (i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        size_t k = strlen(keys[i]), at;
        if (n <= k || strncmp(line, keys[i], k)) continue;
        for (at = k; at < n && (line[at] == ' ' || line[at] == '\t'); ++at) continue;
        if (at < n && line[at] == '=') return true;
    }
    return false;
}
bool snx_ctr_record_write(const char* path, const char* full_source, SnxCtrModel model, char* why, size_t size) {
    size_t bytes = 0, n = 0, tail_n;
    char *old = NULL, *text;
    char* tail = snx_format("source=%s\nmodel=%s\nsystem=3ds\n", full_source, model == SNX_CTR_OLD ? "old" : "new");
    bool written;
    if (snx_config_read(path, &old, &bytes, why, size) < 0) { /* what it holds is not known: nothing is written */
        free(tail);
        return false;
    }
    tail_n = strlen(tail);
    text = (char*)snx_alloc(bytes + tail_n + 2);
    for (const char* at = old; at && at < old + bytes;) { /* the game's other lines as they were */
        const char* end = (const char*)memchr(at, '\n', (size_t)(old + bytes - at));
        size_t length = (size_t)((end ? end + 1 : old + bytes) - at);
        if (!own_line(at, length)) {
            memcpy(text + n, at, length);
            n += length;
        }
        at += length;
    }
    if (n && text[n - 1] != '\n') text[n++] = '\n';
    memcpy(text + n, tail, tail_n + 1);
    written = !snx_config_write(path, ".new", text, n + tail_n, why, size);
    free(tail);
    free(text);
    free(old);
    return written;
}
static void picks_left(void* ctx, int line, const char* text, const char* does) {
    (void)text;
    (void)does;
    snx_config_other_line((SnxConfigOthers*)ctx, line);
}
int snx_ctr_picks_console(const char* path, char** console, char* said, size_t size) {
    char *text = NULL, *key, *value;
    const char* last = NULL;
    size_t bytes;
    SnxIni ini;
    SnxConfigOthers others = {0};
    int read, line = 0;
    *console = NULL;
    if (size) said[0] = 0;
    read = snx_config_read(path, &text, &bytes, said, size);
    if (read <= 0) return read;
    snx_ini_start(&ini, text);
    ini.left = picks_left;
    ini.ctx = &others;
    while (snx_ini_next(&ini, &key, &value))
        if (!strcmp(key, "ctr_model")) { /* the last line's, said with both */
            if (line) snx_config_twice(&others, "", key, last, line, value, ini.line);
            last = value;
            line = ini.line;
        } else /* another console's or version's: kept there, does nothing, named never by its value */
            snx_config_other_key(&others, "", key, ini.line);
    if (last) *console = snx_strdup(last);
    snx_config_others_said(&others, path, said, size);
    free(text);
    return 1;
}
