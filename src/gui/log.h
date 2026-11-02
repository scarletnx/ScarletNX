/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's log, a file a day, scarletnx-console-2026-10-28.log (gui_log_console_name), in its state folder, on
   iOS in logs/ of the app's Documents, where stdout and stderr go too (apple.h): what the launcher says, its failures,
   the warnings it gives as it opens and a dump's icon the driver did not give, a line each after the time and the
   launcher's pid (snx_log_lines), so two launchers open at once are told apart. The GUI_LOG_DAYS newest days are kept,
   a day's first line deleting the older ones; the scarletnx-console.log of the versions before stays as it is. A fatal
   error also goes to scarletnx-error.log beside it. Any thread may write */
#ifndef SNX_GUI_LOG_H
#define SNX_GUI_LOG_H
#include <stddef.h>

#define GUI_LOG_DAYS 14
/* today's launcher log's name, by the local date: scarletnx-console-YYYY-MM-DD.log */
void gui_log_console_name(char* out, size_t size);

/* the folder the logs go to from now on, copied (made by the caller); NULL: none, the lines go to stderr alone */
void gui_log_folder(const char* folder);
/* where a log is, file one of SNX_CONSOLE_LOG (today's, gui_log_console_name) and SNX_ERROR_LOG, or "" without a
   folder */
void gui_log_path(const char* file, char* out, size_t size);
/* a line, as printf formats it, in the log and on stderr (iOS: the log is stderr's file, written once); a text of
   several lines takes a stamp each */
void gui_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
/* a fatal error: in the log and in scarletnx-error.log */
void gui_log_fatal(const char* text);
/* how a game's run ended, from its log at path (common/utilities/logscan.h): the outcome, SNX_LOG_NONE when it logged
   no failure, with what saying it in a line (the error's message, else the kind and its signal) and lines holding the
   log's last ones, a newline after each; -1 when the log can't be read, what saying why */
int gui_log_game_end(const char* path, char* what, size_t size, char* lines, size_t lines_size);
#endif
