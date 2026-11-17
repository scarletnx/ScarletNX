/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An .ini text a key at a time, as the game's settings (src/nx/runtime/options.c) and the GUI's themes
   (src/gui/theme.c) read it. A line ends at a newline and the text at its first NUL; spaces, tabs and a last CR around
   a key and its value are left out. A line starting with ; or # is a comment, [name] starts a section, and a line
   without = is left. A line the reading leaves for what is wrong with it (no =, a section's [ with no ], a section
   name too long) is told to the caller's `left`, when it set one. */
#ifndef SNX_INI_LINES_H
#define SNX_INI_LINES_H

typedef struct {
    char* at;         /* the rest of the text, which the reading cuts into strings */
    char section[32]; /* the section of the keys: empty before the first, and for a name too long for it */
    int line;         /* the line the reading is at, from 1 */
    /* set after snx_ini_start by a caller that says what its file has wrong: a line the reading leaves, its number,
       its text and what that does (a clause for the caller's own sentence) */
    void (*left)(void* ctx, int line, const char* text, const char* does);
    void* ctx;
} SnxIni;

/* text: NUL-terminated and writable; a UTF-8 mark at its start (a Windows editor's) is left out. Nobody is told of
   the lines left until the caller sets `left` */
void snx_ini_start(SnxIni* ini, char* text);
/* the next key and its value, both in the text; 0 at its end */
int snx_ini_next(SnxIni* ini, char** key, char** value);
#endif
