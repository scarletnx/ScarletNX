/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the screen of a fatal error says (backend/sdl3.c draws it in the game's window): what the error means, for the
   errors a player can act on or report, and the text cut into the screen's rows */
#ifndef SNX_FATAL_SCREEN_H
#define SNX_FATAL_SCREEN_H
#include <stddef.h>

#define FATAL_ROW 1024 /* bytes of one row: cols code points of up to 4 bytes */

/* what msg (the text fatal() logged) means, in a sentence; NULL for an error with no known cause */
const char* fatal_hint(const char* msg);
/* text in rows of cols code points at most, broken at a space when one is in the row, else cut: up to max rows into
   rows. Returns the rows the text takes, which may be more than max (those past it are not written) */
int fatal_wrap(const char* text, int cols, char (*rows)[FATAL_ROW], int max);
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
