/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/fatal_screen.h"
#include "common/utilities/common.h"
#include "common/utilities/font.h"
#include "common/utilities/logscan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* fatal_shown(const char* text) {
    size_t shown = 0;
    return snx_logscan_tilde(text, strlen(text), &shown);
}

int fatal_share_log(const char* folder, const char* build, const char* msg, const char* game_log, char* saved,
                    size_t size) {
    enum { TAIL = 4 << 20 };
    size_t n = 0, shown = 0;
    char *log = game_log ? (char*)snx_read_file(game_log, &n) : NULL, *path, *text, *tilde, why[256];
    const char* from = log && n > TAIL ? log + n - TAIL : log;
    int failed;
    /* from a line's start: a home folder cut in two is no longer one to the tilde, and its account's name would stay */
    while (from > log && from < log + n && from[-1] != '\n') ++from;
    if (size) saved[0] = 0;
    path = folder ? snx_path_join(folder, "scarletnx.log") : NULL;
    text = snx_format("%s\nMessage: %s\n\n== %s ==\n%s\n", build ? build : "", msg ? msg : "",
                      game_log ? game_log : "no game log", log ? from : "(not written yet)");
    free(log);
    tilde = snx_logscan_tilde(text, strlen(text), &shown);
    free(text);
    failed = !path || !tilde || snx_write_file(path, tilde, shown);
    if (path && size && failed)
        snprintf(saved, size, "%s: %s", path, tilde ? snx_os_error(why, sizeof why) : "out of memory");
    else if (path && size)
        snprintf(saved, size, "%s", path);
    free(tilde);
    free(path);
    return failed;
}

int fatal_log_url(const char* path, char* out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    const char* prefix;
    size_t n, i;
    int drive, unc;
    if (!size) return 0;
    out[0] = 0;
    if (!path || !*path) return 0;
    drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
            (path[2] == '/' || path[2] == '\\');
    unc = path[0] == '\\' && path[1] == '\\';
    if (!drive && !unc && path[0] != '/') return 0;
    prefix = drive ? "file:///" : unc ? "file:" : "file://";
    n = strlen(prefix);
    if (n >= size) return 0;
    memcpy(out, prefix, n);
    for (i = 0; path[i]; ++i) {
        unsigned char c = (unsigned char)path[i];
        int plain;
        if ((drive || unc) && c == '\\') c = '/';
        plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/", c) ||
                (drive && i == 1 && c == ':');
        if (size - n <= (plain ? 1u : 3u)) {
            out[0] = 0;
            return 0;
        }
        if (plain)
            out[n++] = (char)c;
        else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
    return 1;
}

/* the bytes of the code point starting at s: a lead byte's continuations, three at most; any other byte alone */
static size_t step(const char* s) {
    size_t n = 1;
    if ((unsigned char)s[0] >= 0xC0)
        while (n < 4 && s[n] && ((unsigned char)s[n] & 0xC0) == 0x80) ++n;
    return n;
}

int fatal_wrap_spans(const char* text, int cols, int first, int* at, int* len, int max) {
    const char* start = text;
    int count = 0;
    if (cols < 1) cols = 1;
    while (*text) {
        const char* end = text;
        const char* space = NULL;
        size_t n;
        int k;
        while (*text == ' ') ++text; /* a row starts at a word */
        if (!*text) break;
        for (end = text, k = 0; *end && k < cols; ++k) {
            if (*end == ' ') space = end;
            end += step(end);
        }
        /* the row ends at its last space when the next character would not fit, and a word fills it otherwise */
        if (*end && *end != ' ' && space) end = space;
        n = (size_t)(end - text);
        while (n && text[n - 1] == ' ') --n;
        if (count >= first && count - first < max) {
            at[count - first] = (int)(text - start);
            len[count - first] = (int)n;
        }
        ++count;
        text = end;
    }
    return count;
}

int fatal_wrap(const char* text, int cols, char (*rows)[FATAL_ROW], int max) {
    enum { SPANS = 64 }; /* rows a pass writes: a text of more takes a pass for each SPANS of them */
    int at[SPANS], len[SPANS], count = 0;
    for (int first = 0; first == 0 || first < (count < max ? count : max); first += SPANS) {
        count = fatal_wrap_spans(text, cols, first, at, len, SPANS);
        for (int i = 0; first + i < count && first + i < max && i < SPANS; ++i) {
            size_t n = (size_t)len[i] < FATAL_ROW ? (size_t)len[i] : FATAL_ROW - 1;
            memcpy(rows[first + i], text + at[i], n);
            rows[first + i][n] = 0;
        }
    }
    return count;
}

char* fatal_lines(const char* text, int cols) {
    size_t len = strlen(text), n = 0;
    char *copy = snx_strdup(text), *out = malloc(2 * len + 2), *line = copy;
    while (copy && out && line) {
        char* next = strchr(line, '\n');
        int count, i;
        char (*rows)[FATAL_ROW];
        if (next) *next++ = 0;
        count = fatal_wrap(line, cols, NULL, 0);
        rows = malloc((size_t)(count ? count : 1) * sizeof *rows);
        if (!rows) {
            free(out);
            out = NULL;
            break;
        }
        fatal_wrap(line, cols, rows, count);
        for (i = 0; i < count; ++i) {
            size_t k = strlen(rows[i]);
            memcpy(out + n, rows[i], k);
            n += k;
            if (i + 1 < count) out[n++] = '\n';
        }
        free(rows);
        if (next) out[n++] = '\n';
        line = next;
    }
    if (out && copy) out[n] = 0;
    if (!copy) {
        free(out);
        out = NULL;
    }
    free(copy);
    return out;
}

int fatal_cut(const char* text, int cols, char (*rows)[FATAL_ROW], int max) {
    const char *s, *end;
    int total = 0, skip, row, k;
    if (cols < 1) cols = 1;
    for (s = text; *s; ++total)
        for (k = 0; *s && k < cols; ++k) s += step(s);
    skip = total > max ? total - max : 0;
    for (s = text, row = 0; *s; ++row, s = end) {
        size_t n;
        for (end = s, k = 0; *end && k < cols; ++k) end += step(end);
        if (row < skip) continue;
        n = (size_t)(end - s) < FATAL_ROW ? (size_t)(end - s) : FATAL_ROW - 1;
        memcpy(rows[row - skip], s, n);
        rows[row - skip][n] = 0;
    }
    return total - skip;
}

void fatal_cp437(const char* text, char* out, size_t size) {
    size_t n = 0;
    while (*text && n + 1 < size) {
        const unsigned char* s = (const unsigned char*)text;
        size_t len = step(text);
        unsigned long cp = s[0];
        if (s[0] >= 0xF0 && len == 4)
            cp = (s[0] & 0x07ul) << 18 | (s[1] & 0x3Ful) << 12 | (s[2] & 0x3Ful) << 6 | (s[3] & 0x3Ful);
        else if (s[0] >= 0xE0 && len == 3)
            cp = (s[0] & 0x0Ful) << 12 | (s[1] & 0x3Ful) << 6 | (s[2] & 0x3Ful);
        else if (s[0] >= 0xC0 && len == 2)
            cp = (s[0] & 0x1Ful) << 6 | (s[1] & 0x3Ful);
        else if (s[0] >= 0x80)
            cp = 0xFFFD; /* not UTF-8 */
        out[n++] = (char)gui_glyph((uint32_t)cp);
        text += len;
    }
    out[n] = 0;
}
