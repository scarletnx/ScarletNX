/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/wide.h"
#include <stdio.h>
#include <string.h>
#include "gui/latin.h"

int gui_glyph_cols(int c) { return c >= 256 + gui_latin_count ? 2 : 1; }
uint32_t gui_utf8_step(const char** s) {
    const unsigned char* p = (const unsigned char*)*s;
    uint32_t cp;
    int more, i;
    if (!p[0]) return 0;
    if (p[0] < 0x80) {
        ++*s;
        return p[0];
    }
    if (p[0] >= 0xC2 && p[0] < 0xE0)
        more = 1;
    else if (p[0] >= 0xE0 && p[0] < 0xF0)
        more = 2;
    else if (p[0] >= 0xF0 && p[0] < 0xF5)
        more = 3;
    else
        more = 0; /* a byte no character starts with */
    cp = p[0] & (0x3F >> more);
    for (i = 1; more && i <= more; ++i) { /* a byte that is not a continuation, the end's 0 too, stops it */
        if ((p[i] & 0xC0) != 0x80) more = 0;
        cp = cp << 6 | (p[i] & 0x3F);
    }
    /* none, or longer than its value needs, a UTF-16 surrogate or past Unicode */
    if (!more || (more == 2 && cp < 0x800) || (more == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++*s;
        return 0xFFFD;
    }
    *s += more + 1;
    return cp;
}
static int cols_of(uint32_t codepoint) { return gui_glyph_cols(gui_menu_glyph(codepoint)); }
int gui_text_cols(const char* text) {
    int cols = 0;
    uint32_t cp;
    while ((cp = gui_utf8_step(&text)) != 0) cols += cols_of(cp);
    return cols;
}
size_t gui_cols_fit(const char* text, int cols) {
    const char *fit = text, *next = text;
    uint32_t cp;
    while ((cp = gui_utf8_step(&next)) != 0 && (cols -= cols_of(cp)) >= 0) fit = next;
    return (size_t)(fit - text);
}
const char* gui_cols_skip(const char* text, int cols) {
    while (cols > 0 && *text) cols -= cols_of(gui_utf8_step(&text));
    return text;
}
void gui_text_fit(const char* text, int cols, char* out, size_t size) {
    size_t n;
    if (!size) return;
    if (gui_text_cols(text) <= cols || cols < 4) {
        snprintf(out, size, "%.*s", (int)gui_cols_fit(text, cols), text);
        return;
    }
    for (n = gui_cols_fit(text, cols - 3); n && text[n - 1] == ' ';) --n; /* "the..." rather than "the ..." */
    snprintf(out, size, "%.*s...", (int)n, text);
}
static int separator(char c) { return c == '/' || c == '\\'; }
/* the columns of text's first n bytes */
static int prefix_cols(const char* text, size_t n) {
    const char* end = text + n;
    int cols = 0;
    while (text < end && *text) cols += cols_of(gui_utf8_step(&text));
    return cols;
}
void gui_path_fit(const char* path, int cols, char* out, size_t size) {
    int whole = gui_text_cols(path), room = cols - 3, head_cols;
    size_t n = strlen(path), first = strspn(path, "/\\"), head = 0, i;
    const char* tail = NULL;
    if (!size) return;
    if (whole <= cols || cols < 5) {
        snprintf(out, size, "%.*s", (int)gui_cols_fit(path, cols), path);
        return;
    }
    first += strcspn(path + first, "/\\"); /* the root and its first folder, with the separator after it */
    first += first < n;
    head_cols = prefix_cols(path, first);
    if (head_cols < room) /* the longest end from a separator that leaves the head room */
        for (i = n; i-- > first;)
            if (separator(path[i]) && whole - prefix_cols(path, i) <= room - head_cols) tail = path + i;
    if (!tail && head_cols < room) /* the last folder or file alone is too long: its last letters */
        tail = gui_cols_skip(path, whole - (room - head_cols));
    if (!tail) { /* no separator to keep: a third of the room for the start */
        head = gui_cols_fit(path, room / 3);
        tail = gui_cols_skip(path, whole - (room - prefix_cols(path, head)));
    } else /* the start up to the last separator the room left takes */
        for (i = 0; i < first || (i < n && prefix_cols(path, i + 1) <= room - gui_text_cols(tail)); ++i)
            if (separator(path[i]) || i + 1 == first) head = i + 1;
    snprintf(out, size, "%.*s...%s", (int)head, path, tail);
}
