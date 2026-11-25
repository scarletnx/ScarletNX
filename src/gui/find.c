/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/find.h"
#include <stdint.h>
#include <string.h>

#define FIND_TEXT 2048 /* the bytes of a row's text a word is looked for in: the longest help, and more */
#define FIND_WORD 64   /* the bytes of a word of a search */

/* U+00C0 to U+00FF and U+0100 to U+017F as their letter, 0 for one kept (the signs of multiplication and division) */
static const char latin1[] = "aaaaaaaceeeeiiiidnooooo\0ouuuuyts"
                             "aaaaaaaceeeeiiiidnooooo\0ouuuuyty";
static const char latin_a[] = "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkklllllll"
                              "lllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";
static_assert(sizeof latin1 == 65 && sizeof latin_a == 129, "a letter for each code point");

/* the code point at *s and its bytes, stepping past them; a byte that starts none: itself, as a byte */
static uint32_t next_point(const unsigned char** s, size_t* bytes) {
    const unsigned char* p = *s;
    uint32_t c = p[0];
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0, i;
    if (n > 1) {
        c &= 0x7F >> n;
        for (i = 1; i < n; ++i) {
            if ((p[i] & 0xC0) != 0x80) {
                n = 0;
                break;
            }
            c = c << 6 | (p[i] & 0x3F);
        }
    }
    if (!n) { /* no UTF-8: the byte alone, kept as it is */
        *bytes = 1;
        *s = p + 1;
        return 0x110000 + p[0];
    }
    *bytes = n;
    *s = p + n;
    return c;
}
static uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xFF && latin1[c - 0xC0]) return (uint32_t)latin1[c - 0xC0];
    if (c >= 0x100 && c <= 0x17F) return (uint32_t)latin_a[c - 0x100];
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32; /* Greek */
    if (c >= 0x410 && c <= 0x42F) return c + 32;               /* Cyrillic */
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
void gui_find_fold(const char* text, char* out, size_t size) {
    const unsigned char* s = (const unsigned char*)text;
    size_t at = 0, bytes;
    if (!size) return;
    while (*s) {
        const unsigned char* was = s;
        uint32_t c = next_point(&s, &bytes), f;
        unsigned char put[4];
        size_t n;
        if (c >= 0x110000) { /* a byte of no letter */
            put[0] = was[0];
            n = 1;
        } else {
            f = fold(c);
            if (f < 0x80) {
                put[0] = (unsigned char)f;
                n = 1;
            } else if (f < 0x800) {
                put[0] = (unsigned char)(0xC0 | f >> 6);
                put[1] = (unsigned char)(0x80 | (f & 0x3F));
                n = 2;
            } else { /* not folded: its own bytes */
                memcpy(put, was, bytes);
                n = bytes;
            }
        }
        if (at + n >= size) break;
        memcpy(out + at, put, n);
        at += n;
    }
    out[at] = 0;
}
/* whether folded word is in text, folded; at its start for a name's first points */
static int in_text(const char* word, const char* text, int start) {
    char folded[FIND_TEXT];
    if (!text || !*text) return 0;
    gui_find_fold(text, folded, sizeof folded);
    return start ? !strncmp(folded, word, strlen(word)) : strstr(folded, word) != NULL;
}
static int in_words(const char* word, const char* const* words) {
    for (; words && *words; ++words)
        if (in_text(word, *words, 0)) return 1;
    return 0;
}
/* the points of folded word in row's best text */
static int points(const char* word, const GuiFindRow* row) {
    int i;
    for (i = 0; i < 2; ++i)
        if (in_text(word, row->name[i], 1)) return GUI_FIND_NAME_START;
    for (i = 0; i < 2; ++i)
        if (in_text(word, row->name[i], 0)) return GUI_FIND_NAME;
    if (in_text(word, row->key, 0)) return GUI_FIND_KEY;
    for (i = 0; i < 4; ++i)
        if (in_text(word, row->where[i], 0)) return GUI_FIND_WHERE;
    for (i = 0; i < 2; ++i)
        if (in_words(word, row->words[i])) return GUI_FIND_WORDS;
    for (i = 0; i < 2; ++i)
        if (in_text(word, row->about[i], 0)) return GUI_FIND_HELP;
    return 0;
}
int gui_find_rank(const char* query, const GuiFindRow* row) {
    char folded[FIND_TEXT], word[FIND_WORD];
    const char* at = folded;
    int sum = 0, words = 0;
    gui_find_fold(query, folded, sizeof folded);
    for (;;) {
        size_t n;
        int got;
        while (*at == ' ' || *at == '\t') ++at;
        if (!*at) break;
        n = strcspn(at, " \t");
        if (n >= sizeof word) n = sizeof word - 1; /* a word longer than any text's: its start */
        memcpy(word, at, n);
        word[n] = 0;
        at += strcspn(at, " \t");
        got = points(word, row);
        if (!got) return 0;
        sum += got;
        ++words;
    }
    return words ? sum : 0;
}
