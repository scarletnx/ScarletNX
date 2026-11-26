/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/config_file.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "common/platform/file.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snx_config_read(const char* path, char** text, size_t* size, char* why, size_t why_size) {
    char reason[256];
    *size = 0;
    *text = (char*)snx_read_file(path, size);
    if (*text) return 1;
    snx_os_error(reason, sizeof reason); /* before a call that sets errno */
    if (!snx_path_exists(path)) return 0;
    snprintf(why, why_size, "%s cannot be read (%s)", path, reason);
    return -1;
}
int snx_config_write(const char* path, const char* suffix, const void* text, size_t size, char* why, size_t why_size) {
    char reason[256], *dir = snx_dirname(path), *staged = snx_format("%s%s", path, suffix), *was = NULL;
    size_t was_size;
    int failed = 1;
    if (!dir || !staged) {
        snprintf(why, why_size, "out of memory");
        goto done;
    }
    if (snx_config_read(path, &was, &was_size, why, why_size) < 0) {
        size_t n = strlen(why);
        snprintf(why + n, why_size > n ? why_size - n : 0, ": nothing is written over it");
        goto done;
    }
    free(was);
    failed = snx_mkdirs(dir) || snx_write_file(staged, text, size);
    if (!failed && plat_replace_file(staged, path)) failed = 1;
    if (failed) {
        snprintf(why, why_size, "%s could not be written (%s)", path, snx_os_error(reason, sizeof reason));
        remove(staged);
    }
done:
    free(dir);
    free(staged);
    return failed ? -1 : 0;
}

int snx_config_keys(const char* path, SnxConfigTake take, void* ctx) {
    char *text, *key, *value, why[1024];
    size_t size;
    SnxIni ini;
    int read = snx_config_read(path, &text, &size, why, sizeof why);
    if (read < 0 && snx_config_first(path, "cannot be read")) snx_error("%s", why);
    if (read <= 0) return read;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) take(ctx, ini.section, key, value, ini.line);
    free(text);
    return 1;
}
/* what this run has said, "file" and "what" after a NUL each */
static struct {
    atomic_flag lock;
    char** said;
    size_t count;
} g_config_file = {ATOMIC_FLAG_INIT, NULL, 0};
static void locked(int take) {
    if (!take) {
        atomic_flag_clear_explicit(&g_config_file.lock, memory_order_release);
        return;
    }
    while (atomic_flag_test_and_set_explicit(&g_config_file.lock, memory_order_acquire)) {}
}
int snx_config_first(const char* file, const char* what) {
    size_t i, n = strlen(file) + strlen(what) + 2;
    int first = 1;
    char* key = (char*)snx_alloc(n);
    snprintf(key, n, "%s%c%s", file, '\n', what);
    locked(1);
    for (i = 0; i < g_config_file.count && first; ++i) first = strcmp(g_config_file.said[i], key) != 0;
    if (first) {
        g_config_file.said =
            (char**)snx_realloc(g_config_file.said, (g_config_file.count + 1) * sizeof *g_config_file.said);
        g_config_file.said[g_config_file.count++] = key;
        key = NULL;
    }
    locked(0);
    free(key);
    return first;
}
void snx_config_forget(void) {
    locked(1);
    for (size_t i = 0; i < g_config_file.count; ++i) free(g_config_file.said[i]);
    free(g_config_file.said);
    g_config_file.said = NULL;
    g_config_file.count = 0;
    locked(0);
}

/* the characters of a name a message may give: a key's, a section's with spaces too */
static int named(const char* text, size_t max, int spaces) {
    size_t n = strspn(text, spaces ? "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./ "
                                   : "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.");
    return n && n <= max && !text[n];
}
void snx_config_other_key(SnxConfigOthers* others, const char* section, const char* key, int line) {
    int at = others->keys++;
    size_t n = strspn(key, "abcdefghijklmnopqrstuvwxyz0123456789_");
    if (at >= SNX_CONFIG_NAMED) return;
    if (others->secret ? !n || n > 32 || key[n] : !named(key, 40, 0))
        snprintf(others->key[at], sizeof others->key[at], "line %d", line);
    else if (!section[0])
        snprintf(others->key[at], sizeof others->key[at], "%s", key);
    else if (named(section, 31, 1))
        snprintf(others->key[at], sizeof others->key[at], "[%s] %s", section, key);
    else
        snprintf(others->key[at], sizeof others->key[at], "%s of line %d's section", key, line);
}
void snx_config_twice(SnxConfigOthers* others, const char* section, const char* key, const char* first, int first_line,
                      const char* last, int line) {
    char name[80];
    int at = others->twice_count++;
    if (at >= SNX_CONFIG_NAMED) return;
    if (!named(key, 40, 0))
        snprintf(name, sizeof name, "line %d's key", line);
    else if (section[0] && named(section, 31, 1))
        snprintf(name, sizeof name, "[%s] %s", section, key);
    else
        snprintf(name, sizeof name, "%s", key);
    if (first && last && named(key, 40, 0))
        snprintf(others->twice[at], sizeof others->twice[at],
                 "%s is given twice, %s=%.24s on line %d and %s=%.24s on line %d", name, key, first, first_line, key,
                 last, line);
    else
        snprintf(others->twice[at], sizeof others->twice[at], "%s is given twice, on lines %d and %d", name, first_line,
                 line);
}
void snx_config_other_line(SnxConfigOthers* others, int line) {
    int at = others->lines++;
    if (at < SNX_CONFIG_NAMED) others->line[at] = line;
}
int snx_config_others_said(const SnxConfigOthers* others, const char* file, char* out, size_t size) {
    char keys[SNX_CONFIG_NAMED * 84] = "", lines[SNX_CONFIG_NAMED * 12] = "", twice[SNX_CONFIG_NAMED * 184] = "";
    char what[32];
    size_t at = 0, line_at = 0, twice_at = 0;
    int i, named_keys = 0, named_lines = 0, named_twice = 0;
    int hidden = others->keys > SNX_CONFIG_NAMED ? others->keys - SNX_CONFIG_NAMED : 0;
    int hidden_lines = others->lines > SNX_CONFIG_NAMED ? others->lines - SNX_CONFIG_NAMED : 0;
    int many_lines;
    if (size) out[0] = 0;
    for (i = 0; i < others->keys && i < SNX_CONFIG_NAMED; ++i)
        if (snx_config_first(file, others->key[i]))
            at += (size_t)snprintf(keys + at, sizeof keys - at, "%s%s", named_keys++ ? ", " : "", others->key[i]);
    for (i = 0; i < others->lines && i < SNX_CONFIG_NAMED; ++i) {
        snprintf(what, sizeof what, "line %d", others->line[i]);
        if (snx_config_first(file, what))
            line_at += (size_t)snprintf(lines + line_at, sizeof lines - line_at, "%s%d", named_lines++ ? ", " : "",
                                        others->line[i]);
    }
    for (i = 0; i < others->twice_count && i < SNX_CONFIG_NAMED; ++i)
        if (snx_config_first(file, others->twice[i]))
            twice_at += (size_t)snprintf(twice + twice_at, sizeof twice - twice_at, "%s%s", named_twice++ ? "; " : "",
                                         others->twice[i]);
    if (!named_keys && !named_lines && !named_twice) return 0;
    many_lines = named_lines + (hidden_lines > 0) > 1;
    at = (size_t)snprintf(out, size, "%s: ", file);
    if (named_keys && at < size) {
        at += (size_t)snprintf(out + at, size - at, "this version does not read %s", keys);
        if (hidden > 0 && at < size) at += (size_t)snprintf(out + at, size - at, " and %d more", hidden);
        if (at < size)
            at += (size_t)snprintf(out + at, size - at, ", which %s",
                                   named_keys + hidden > 1 ? "stay as they are" : "stays as it is");
    }
    if (named_lines && at < size)
        at += (size_t)snprintf(out + at, size - at, "%s%s %s%s ha%s no = and set%s nothing", named_keys ? "; " : "",
                               many_lines ? "lines" : "line", lines, hidden_lines > 0 ? " and more" : "",
                               many_lines ? "ve" : "s", many_lines ? "" : "s");
    if (named_twice && at < size)
        snprintf(out + at, size - at, "%s%s: the last line's is taken", named_keys || named_lines ? "; " : "", twice);
    return named_keys + named_lines + named_twice;
}

/* text's lines that owns does not take, kept in out (text's size and its sections' heads of room) */
char* snx_config_kept(const char* text, size_t size, int top, SnxConfigOwns owns, void* ctx) {
    char *copy = (char*)malloc(size + 1), *out, *key, *value, section[32] = "", emitted[32] = "";
    size_t n = 0, room = 2 * size + 64;
    const char *line = text, *end;
    SnxIni ini;
    int at = 0, open = 0;
    out = (char*)malloc(room);
    if (!copy || !out) {
        free(copy);
        free(out);
        return NULL;
    }
    memcpy(copy, text, size);
    copy[size] = 0;
    out[0] = 0;
    snx_ini_start(&ini, copy);
    while (snx_ini_next(&ini, &key, &value)) {
        (void)value;
        for (; at + 1 < ini.line; ++at) { /* to the line the reading is at, the raw text's */
            end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
            line = end ? end + 1 : text + size;
        }
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        snprintf(section, sizeof section, "%s", ini.section);
        if (!owns(ctx, section, key) && (top < 0 || !section[0] == !!top)) {
            size_t length = (size_t)((end ? end : text + size) - line);
            if (length && line[length - 1] == '\r') --length; /* its line end is the file's */
            if (top == 0 && (!open || strcmp(emitted, section))) {
                snprintf(emitted, sizeof emitted, "%s", section);
                open = 1;
                n += (size_t)snprintf(out + n, room - n, "[%s]\n", section);
            }
            if (n + length + 2 < room) {
                memcpy(out + n, line, length);
                n += length;
                out[n++] = '\n';
                out[n] = 0;
            }
        }
    }
    free(copy);
    return out;
}
