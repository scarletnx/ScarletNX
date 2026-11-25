/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings' search (gui/find.h): a text folded whatever its case and accents, and how a search's words rank a row */
#include "gui/find.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void folds(const char* text, const char* want) {
    char out[64];
    gui_find_fold(text, out, sizeof out);
    if (strcmp(out, want)) fprintf(stderr, "gui_find: \"%s\" folds to \"%s\", not \"%s\"\n", text, out, want);
    assert(!strcmp(out, want));
}
int main(void) {
    static const char* const words[] = {"Off", "On", "Unlimited", NULL};
    static const char* const palavras[] = {"Desligado", "Ligado", "Sem limite", NULL};
    char cut[8];
    /* case and accents off: Latin's, and Greek's and Cyrillic's capitals; anything else as it is */
    folds("Java Home", "java home");
    folds("Configura\xC3\xA7\xC3\xB5"
          "es \xC3\x81UDIO",
          "configuracoes audio"); /* c cedilla, o tilde, A acute */
    folds("\xC5\x81\xC3\xB3"
          "d\xC5\xBA",
          "lodz");                                                                 /* L stroke, o acute, z acute */
    folds("\xCE\x9A\xCE\xB1\xCE\xBB\xCF\x8C", "\xCE\xBA\xCE\xB1\xCE\xBB\xCF\x8C"); /* Greek capital kappa to small */
    folds("\xD0\x97\xD0\xB2\xD1\x83\xD0\xBA", "\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA"); /* Cyrillic capital ze to small */
    folds("\xE9\x9F\xB3\xE9\x87\x8F", "\xE9\x9F\xB3\xE9\x87\x8F");                 /* CJK as it is */
    folds("a\xFF"
          "b",
          "a\xFF"
          "b");                              /* no UTF-8: kept */
    folds("x \xC3\x97 y", "x \xC3\x97 y");   /* the sign of multiplication kept */
    gui_find_fold("abc\xC3\x97xyz", cut, 5); /* cut before a letter that does not fit whole */
    assert(!strcmp(cut, "abc"));
    gui_find_fold("abcd\xC3\x97", cut, 6);
    assert(!strcmp(cut, "abcd"));
    {
        GuiFindRow java = {{"Java home", "Java home"},
                           "java_home",
                           {"Launch", "Launch", NULL, NULL},
                           {NULL, NULL},
                           {"The JDK, 21 or later, that Android apps run on", NULL}};
        GuiFindRow fps = {{"Limite de FPS", "FPS limit"},
                          "fps_limit",
                          {"Desempenho", "Performance", NULL, NULL},
                          {palavras, words},
                          {"Quadros por segundo no m\xC3\xA1ximo", "The most frames a second"}};
        /* each word's best text: a name's start, a name, the key, where, a value's word, the help */
        assert(gui_find_rank("java", &java) == GUI_FIND_NAME_START);
        assert(gui_find_rank("JAVA", &java) == GUI_FIND_NAME_START);
        assert(gui_find_rank("home", &java) == GUI_FIND_NAME);
        assert(gui_find_rank("_home", &java) == GUI_FIND_KEY);
        assert(gui_find_rank("launch", &java) == GUI_FIND_WHERE);
        assert(gui_find_rank("android", &java) == GUI_FIND_HELP);
        assert(gui_find_rank("unlimited", &fps) == GUI_FIND_WORDS);
        assert(gui_find_rank("ligado", &fps) == GUI_FIND_WORDS);
        assert(gui_find_rank("maximo", &fps) == GUI_FIND_HELP); /* its accent off */
        assert(gui_find_rank("desempenho", &fps) == GUI_FIND_WHERE);
        /* in either language: the English name starts with "fps", the translated one with "limit" */
        assert(gui_find_rank("FPS limit", &fps) == 2 * GUI_FIND_NAME_START);
        assert(gui_find_rank("unlimited fps", &fps) == GUI_FIND_WORDS + GUI_FIND_NAME_START);
        /* a word's best text, not its first: "sem" is in Desempenho's middle before Sem limite's word */
        assert(gui_find_rank("sem", &fps) == GUI_FIND_WHERE);
        assert(gui_find_rank("java home", &java) == GUI_FIND_NAME_START + GUI_FIND_NAME);
        /* every word in some text, or nothing; spaces alone are no search */
        assert(gui_find_rank("java javva", &java) == 0);
        assert(gui_find_rank("javva", &java) == 0);
        assert(gui_find_rank("", &java) == 0);
        assert(gui_find_rank("   ", &java) == 0);
        assert(gui_find_rank("  java  ", &java) == GUI_FIND_NAME_START);
    }
    puts("gui_find: folds and ranks passed");
    return 0;
}
