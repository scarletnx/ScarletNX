/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The text of an error's panel (error_panel.h) and of its last resort, SDL's box: cut into rows, in the font's bytes,
   the homes as ~; Share Log's file and the log's file URI */
#ifndef SNX_FATAL_SCREEN_H
#define SNX_FATAL_SCREEN_H
#include <stddef.h>

#define FATAL_BOX_COLS 100  /* code points of a row of the box SDL shows */
#define FATAL_ROW      1024 /* bytes of one row: cols code points of up to 4 bytes */

/* text in rows of cols code points at most, broken at a space when one is in the row, else cut: up to max rows into
   rows. Returns the rows the text takes, which may be more than max (those past it are not written) */
int fatal_wrap(const char* text, int cols, char (*rows)[FATAL_ROW], int max);
/* the same rows as spans of text, from its row first: each row's first byte's offset and its bytes, up to max of them
   into at and len. Returns the rows the text takes, all of them */
int fatal_wrap_spans(const char* text, int cols, int first, int* at, int* len, int max);
/* text with each of its lines cut into rows of cols code points as fatal_wrap cuts them, the rows apart by a newline and
   its empty lines kept: for a box as wide as its longest line (SDL's on Windows). malloc'd, NULL out of memory */
char* fatal_lines(const char* text, int cols);
/* text as typed, spaces kept, in rows of cols code points: its last max rows into rows, the first of them first.
   Returns the rows written */
int fatal_cut(const char* text, int cols, char (*rows)[FATAL_ROW], int max);
/* text in the font's bytes (CP437, gui_glyph), for ui_text: what the font lacks becomes '?' */
void fatal_cp437(const char* text, char* out, size_t size);
/* An absolute desktop log path as a file URI, with reserved and UTF-8 bytes
   percent encoded. Returns 0, leaving an empty string, if it will not fit or
   the path is relative. Windows drive and UNC paths use forward slashes. */
int fatal_log_url(const char* path, char* out, size_t size);
/* text as the error's screen shows it (the message, the log's path, what Share Log and Copy path did): each home
   folder as ~ (snx_logscan_tilde), since a photo of the screen would name the player. malloc'd, NULL out of memory */
char* fatal_shown(const char* text);
/* Share Log's file, as the launcher's: the build, the error and the game's log (its last 4 MiB from the first line that
   starts in them, or a line saying it is missing), each home folder as ~ (snx_logscan_tilde) since the paths name the
   player, written to scarletnx.log in folder. 0 when written, its path in saved; else nonzero, and in saved the path
   and why it was not written (nothing without a folder) */
int fatal_share_log(const char* folder, const char* build, const char* msg, const char* game_log, char* saved,
                    size_t size);
#endif
