/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings' search: a text folded so a word matches it whatever its letters' case and accents (capital and small
   Latin, Greek and Cyrillic letters, and Latin's accented ones of U+00C0 to U+017F as their letter), and how well the
   words of a search match a row's texts. No SDL */
#ifndef SNX_GUI_FIND_H
#define SNX_GUI_FIND_H
#include <stddef.h>

/* text folded into out (size bytes, cut before a letter that does not fit): small letters, no accents; a byte that is
   no UTF-8 letter as it is */
void gui_find_fold(const char* text, char* out, size_t size);
/* a row's texts as the search reads them, each NULL for none: its name, translated and English; its .ini key; where
   it is, its group's and its page's names, translated and English; its values' words, NULL-terminated lists,
   translated and English; and its help, translated and English */
typedef struct {
    const char* name[2];
    const char* key;
    const char* where[4];
    const char* const* words[2];
    const char* about[2];
} GuiFindRow;
/* the points a word of a search earns in the best of a row's texts it is in */
enum {
    GUI_FIND_HELP = 1,
    GUI_FIND_WORDS,
    GUI_FIND_WHERE,
    GUI_FIND_KEY,
    GUI_FIND_NAME,
    GUI_FIND_NAME_START, /* the name starts with it */
};
/* how well query matches row: 0 when one of its words, split at spaces, is in none of the row's texts (an empty query
   matches nothing), else the sum of its words' points */
int gui_find_rank(const char* query, const GuiFindRow* row);
#endif
