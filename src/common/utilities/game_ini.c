/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/ascii.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* snx_game_ini(const char* folder, const char* name) {
    char *own = snx_user_path(folder, SNX_USER_CONFIG), *base = snx_path_join(own, name);
    char* path = snx_format("%s.ini", base);
    free(own);
    free(base);
    return path;
}
/* key's value in an .ini's text, the last one's, as the runtime reads it: in the text, NULL for none */
static char* text_value(char* text, const char* key) {
    SnxIni ini;
    char *k, *value, *found = NULL;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &k, &value))
        if (!strcmp(k, key)) found = value;
    return found;
}
char* snx_ini_value(const char* path, const char* key) {
    size_t size;
    char *text = (char*)snx_read_file(path, &size), *value = text ? text_value(text, key) : NULL;
    char* copy = value && *value ? snx_strdup(value) : NULL;
    free(text);
    return copy;
}
/* a line, from line to end, that sets key= */
static int key_line(const char* line, const char* end, const char* key) {
    size_t n = strlen(key);
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if ((size_t)(end - line) < n || strncmp(line, key, n)) return 0;
    for (line += n; line < end && (*line == ' ' || *line == '\t'); line++) continue;
    return line < end && *line == '=';
}
/* a line, from line to end, that starts [Game] */
static int game_line(const char* line, const char* end) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    return end - line == 6 && !strncmp(line, "[Game]", 6);
}
int snx_ini_record(const char* path, const char* key, const char* value) {
    char *dir = snx_dirname(path), *tmp = snx_format("%s.tmp", path), *out = NULL, *text, *copy, *had;
    const char *line, *end, *next;
    size_t size = 0, n = 0, len = strlen(key) + strlen(value);
    int placed = 0, rc = 0;
    if (value[strcspn(value, "\r\n")]) {
        rc = snx_error("%s=%s has a line break, which the game's settings cannot keep", key, value);
        goto done;
    }
    text = (char*)snx_read_file(path, &size);
    if (!text && snx_path_exists(path)) {
        rc = snx_error("cannot read %s to record %s= in it", path, key);
        goto done;
    }
    if (text) {
        copy = snx_strdup(text);
        had = text_value(copy, key);
        placed = had && !strcmp(had, value);
        free(copy);
        if (placed) {
            free(text);
            goto done;
        }
    }
    out = (char*)snx_alloc(size + len + 24);
    for (line = text; line && line < text + size; line = next) {
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        next = end ? end + 1 : text + size;
        if (!end) end = next;
        if (key_line(line, end, key)) continue;
        memcpy(out + n, line, (size_t)(next - line));
        n += (size_t)(next - line);
        if (next == end) out[n++] = '\n'; /* the file's last line, without a newline */
        if (!placed && game_line(line, end)) {
            n += (size_t)sprintf(out + n, "%s=%s\n", key, value);
            placed = 1;
        }
    }
    free(text);
    if (!placed) n += (size_t)sprintf(out + n, "%s[Game]\n%s=%s\n", n ? "\n" : "", key, value);
    if (snx_mkdirs(dir) || snx_write_file(tmp, out, n) || snx_replace(tmp, path)) {
        snx_remove_tree(tmp);
        rc = snx_error("cannot record %s=%s in %s", key, value, path);
    }
done:
    free(dir);
    free(tmp);
    free(out);
    return rc;
}
int snx_ini_has(const char* path, const char* key) {
    size_t size = 0;
    char* text = (char*)snx_read_file(path, &size);
    const char *line, *end, *next;
    int found = 0;
    for (line = text; line && line < text + size && !found; line = next) {
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        next = end ? end + 1 : text + size;
        found = key_line(line, end ? end : next, key);
    }
    free(text);
    return found;
}
int snx_ini_drop(const char* path, const char* key) {
    size_t size = 0, n = 0;
    char *text = (char*)snx_read_file(path, &size), *out, *tmp;
    const char *line, *end, *next;
    int dropped = 0, rc = 0;
    if (!text) return snx_path_exists(path) ? snx_error("cannot read %s to take %s= out of it", path, key) : 0;
    out = (char*)snx_alloc(size + 1);
    for (line = text; line < text + size; line = next) {
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        next = end ? end + 1 : text + size;
        if (key_line(line, end ? end : next, key)) {
            dropped = 1;
            continue;
        }
        memcpy(out + n, line, (size_t)(next - line));
        n += (size_t)(next - line);
    }
    tmp = snx_format("%s.tmp", path);
    if (dropped && (snx_write_file(tmp, out, n) || snx_replace(tmp, path))) {
        snx_remove_tree(tmp);
        rc = snx_error("cannot take %s= out of %s", key, path);
    }
    free(tmp);
    free(out);
    free(text);
    return rc;
}
char* snx_game_source(const char* folder, const char* name, int* recorded) {
    char *ini = snx_game_ini(folder, name), *source = snx_ini_value(ini, "source"), path[SNX_PATH_SIZE];
    free(ini);
    if (!source) { /* a console driver's, in the folder: the user's config folder is elsewhere (snx_user_path) */
        ini = snx_format("%s/%s.ini", folder, name);
        source = snx_ini_value(ini, "source");
        free(ini);
    }
    if (recorded) *recorded = source != NULL;
    if (source) return source;
    if (snx_path_format(path, sizeof path, "%s/%s.source", folder, name)) return NULL;
    if (recorded) *recorded = snx_is_link(path) || snx_path_exists(path);
    return snx_strdup(path);
}
int snx_record_source(const char* folder, const char* name, const char* dump) {
    char* ini = snx_game_ini(folder, name);
    int rc = snx_ini_record(ini, "source", dump);
    free(ini);
    return rc;
}
size_t snx_addons_parse(const char* text, const char* game, SnxAddon* list, size_t max) {
    char word[32];
    size_t count = 0, n, i, update = (size_t)-1;
    SnxU64 own, id;
    if (!snx_title_parse(game, &own)) return 0;
    while (text && *text) {
        while (*text == ',' || *text == ' ' || *text == '\t') text++;
        for (n = 0; text[n] && text[n] != ',' && text[n] != ' ' && text[n] != '\t'; n++) continue;
        if (n >= 16 && n < sizeof word) {
            for (i = 0; i < n; i++) word[i] = i < 16 ? (char)snx_to_upper(text[i]) : text[i];
            word[n] = 0;
            if (snx_addon_entry_id(own, word, &id)) {
                for (i = 0; i < count && list[i].id != id; i++) continue;
                if (i < count && snx_addon_kind(own, id) == SNX_ADDON_UPDATE)
                    i = update; /* one update: the last one listed */
                if (i == update && update != (size_t)-1)
                    memcpy(list[i].entry, word, n + 1);
                else if (i == count && count < max) {
                    if (snx_addon_kind(own, id) == SNX_ADDON_UPDATE) update = count;
                    memcpy(list[count].entry, word, n + 1);
                    list[count++].id = id;
                }
            }
        }
        text += n;
    }
    return count;
}
char* snx_addons_text(const SnxAddon* list, size_t count) {
    char* text = (char*)snx_alloc(count * 32 + 1);
    text[0] = 0;
    for (size_t i = 0; i < count; i++) {
        if (i) strcat(text, ",");
        strcat(text, list[i].entry);
    }
    return text;
}

static const char* const g_system_words[] = {"switch", "3ds", "vita", "gba"};
const char* snx_system_word(SnxSystem system) { return g_system_words[system]; }
SnxSystem snx_game_system(const char* folder, const char* name) {
    char *ini = snx_format("%s/%s.ini", folder, name), *word = snx_ini_value(ini, "system"), *model = NULL,
         *card = snx_path_join(folder, "ux0");
    SnxSystem system = SNX_SYSTEM_SWITCH;
    for (size_t i = 0; word && i < sizeof(g_system_words) / sizeof(g_system_words[0]); ++i)
        if (!strcmp(word, g_system_words[i])) system = (SnxSystem)i;
    if (!word && (model = snx_ini_value(ini, "model")) != NULL)
        system = SNX_SYSTEM_3DS;
    else if (!word && snx_is_dir(card))
        system = SNX_SYSTEM_VITA;
    free(card);
    free(model);
    free(word);
    free(ini);
    return system;
}
int snx_record_system(const char* folder, const char* name, SnxSystem system) {
    char* ini = snx_format("%s/%s.ini", folder, name);
    int rc = snx_ini_record(ini, "system", snx_system_word(system));
    free(ini);
    return rc;
}
/* 1 when the files at a and b hold the same bytes */
static int same_bytes(const char* a, const char* b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    unsigned char p[16384], q[16384];
    size_t n;
    int same = x && y;
    while (same && (n = fread(p, 1, sizeof p, x)) > 0) same = fread(q, 1, n, y) == n && !memcmp(p, q, n);
    same = same && !ferror(x) && fread(q, 1, 1, y) == 0 && !ferror(y);
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}
int snx_place_runtime(const char* runtime, const char* copy, char* error, size_t error_size) {
    char *own = snx_absolute(runtime), *mine = snx_absolute(copy), *staged = snx_format("%s.tmp", copy), why[256];
    int rc = 0;
    if (!own || !mine || !staged) {
        snprintf(error, error_size, "out of memory for the game's runtime");
        rc = -1;
    } else if (strcmp(own, mine) && !(snx_is_file(copy) && same_bytes(runtime, copy))) {
        if (!snx_copy_file(runtime, staged) && !snx_make_executable(staged) && !snx_replace(staged, copy))
            rc = 1;
        else {
            snprintf(error, error_size, "cannot copy the runtime %s to %s: %s", runtime, copy,
                     snx_os_error(why, sizeof why));
            if (snx_is_file(staged)) (void)remove(staged);
            rc = -1;
        }
    }
    free(staged);
    free(mine);
    free(own);
    return rc;
}
