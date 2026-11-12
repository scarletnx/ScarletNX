/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/covers.h. The list's format is clrmamepro's (a game ( name "..." rom ( ... crc XXXXXXXX ... ) ) each); the file
   name of a box is libretro-thumbnails' rule, RetroArch's too (its README: &*\/:`<>?\| become _) */
#include "gui/covers.h"
#include <libdeflate.h>
#include <stdio.h>
#include <string.h>

uint32_t gui_cover_crc(const void* rom, size_t size) { return (uint32_t)libdeflate_crc32(0, rom, size); }

/* a hex digit's value, -1 for another character */
static int hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
/* the line from at to end, past its leading blanks, starting with word and a blank: what follows, else NULL */
static const char* starts(const char* at, const char* end, const char* word) {
    size_t n = strlen(word);
    while (at < end && (*at == ' ' || *at == '\t')) ++at;
    return (size_t)(end - at) > n && !memcmp(at, word, n) && (at[n] == ' ' || at[n] == '\t') ? at + n + 1 : NULL;
}
/* the rom line from at to end: 1 when its crc field is crc */
static int rom_has(const char* at, const char* end, uint32_t crc) {
    for (; end - at > 13; ++at) { /* " crc " and 8 digits */
        uint32_t v = 0;
        int i;
        if (memcmp(at, " crc ", 5)) continue;
        for (i = 0; i < 8 && hex(at[5 + i]) >= 0; ++i) v = v << 4 | (uint32_t)hex(at[5 + i]);
        return i == 8 && hex(at[13]) < 0 && v == crc;
    }
    return 0;
}
int gui_cover_name(const char* list, size_t n, uint32_t crc, char* out, size_t size) {
    const char *at = list, *end = list + n, *name = NULL;
    size_t name_size = 0;
    if (size) out[0] = 0;
    while (at < end) {
        const char *line_end = memchr(at, '\n', (size_t)(end - at)), *rest;
        if (!line_end) line_end = end;
        if (starts(at, line_end, "game")) /* a game's block: its name comes before its rom */
            name = NULL;
        else if ((rest = starts(at, line_end, "name")) != NULL && *rest == '"' && !name) {
            const char* close = memchr(rest + 1, '"', (size_t)(line_end - rest - 1));
            if (close) {
                name = rest + 1;
                name_size = (size_t)(close - name);
            }
        } else if (starts(at, line_end, "rom") && name && rom_has(at, line_end, crc)) {
            if (name_size >= size) return 0;
            memcpy(out, name, name_size);
            out[name_size] = 0;
            return 1;
        }
        at = line_end + 1;
    }
    return 0;
}
int gui_cover_url(const char* name, char* out, size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    size_t at = (size_t)snprintf(out, size, "%s", GUI_COVERS_BOX_URL);
    const char* c;
    for (c = name; at < size && *c; ++c) {
        unsigned char b = strchr("&*/:`<>?\\|", *c) ? '_' : (unsigned char)*c;
        int plain = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || strchr("-._~", b);
        if (plain && at + 1 < size)
            out[at++] = (char)b;
        else if (!plain && at + 3 < size) {
            out[at++] = '%';
            out[at++] = digits[b >> 4];
            out[at++] = digits[b & 15];
        } else
            at = size;
    }
    if (at + 4 >= size) {
        if (size) out[0] = 0;
        return 0;
    }
    memcpy(out + at, ".png", 5);
    return 1;
}
int gui_cover_png(const void* bytes, size_t size) { return size >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8); }
