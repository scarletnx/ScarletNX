/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* F1's text (runtime/platform/help.c): the keys up to F12's and Esc last, with neither RenderDoc nor a debugger
   nothing of them; Ctrl+F12's line between F12's and Esc's while RenderDoc can capture; the debugger's name after the
   keys while one traces the game, ? as given; a room too small cut, with the whole length said */
#include "nx/runtime/platform/help.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define F12  "  F12  turn the stats on or off\n"
#define CTRL "  Ctrl+F12  capture the next frame with RenderDoc\n"
#define ESC  "  Esc  quit the game"

static char g_text[HELP_MAX];

/* whether the text ends with end */
static int ends(const char* end) {
    size_t n = strlen(g_text), e = strlen(end);
    return n >= e && !strcmp(g_text + n - e, end);
}

int main(void) {
    char small[32];
    int n, plain;
    /* neither: the keys, Esc's last, nothing of RenderDoc or a debugger */
    plain = help_text(g_text, sizeof g_text, 0, NULL);
    assert(plain > 0 && plain < HELP_MAX && (size_t)plain == strlen(g_text));
    assert(!strncmp(g_text, "Controller: ", 12) && ends(F12 ESC));
    assert(!strstr(g_text, "Ctrl+F12") && !strstr(g_text, "RenderDoc") && !strstr(g_text, "ebugger"));
    assert(help_text(g_text, sizeof g_text, 0, "") == plain && ends(F12 ESC));
    /* RenderDoc can capture: Ctrl+F12 between F12 and Esc */
    n = help_text(g_text, sizeof g_text, 1, NULL);
    assert(n == plain + (int)strlen(CTRL) && ends(F12 CTRL ESC));
    /* a debugger traces the game: its name after the keys, ? as given */
    n = help_text(g_text, sizeof g_text, 0, "gdb");
    assert(ends(F12 ESC "\n\nDebugger attached: gdb") && (size_t)n == strlen(g_text));
    help_text(g_text, sizeof g_text, 1, "?");
    assert(ends(F12 CTRL ESC "\n\nDebugger attached: ?"));
    /* a room too small: cut, ended, the whole length said */
    n = help_text(small, sizeof small, 1, "gdb");
    assert(strlen(small) == sizeof small - 1 && !strncmp(small, "Controller: ", 12));
    assert(n == plain + (int)strlen(CTRL) + (int)strlen("\n\nDebugger attached: gdb"));
    puts("help passed");
    return 0;
}
