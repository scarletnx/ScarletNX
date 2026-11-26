/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's game log (GBA, DS, 3DS, Vita), as the Switch's runtime keeps its own: <folder>/<name>.log with every line
   the run writes on stdout and stderr, each stamped with its second as the Switch's are, the 4 runs before in logs/
   beside it, and past log_max the run's start kept with the last lines (run_log.h). The terminal still gets every
   line, unstamped: stdout and stderr go through a pipe each (one when both were the same file, so their order holds),
   which a thread copies to the terminal and to the file, the terminal's side dropped when it refuses a write */
#ifndef SNX_GAME_LOG_H
#define SNX_GAME_LOG_H
#include <stddef.h>

/* The log of folder's game name made and stdout and stderr through it, once in a process, at the runner's start
   before any thread of its own: never undone nor done again in the run. The log's limit in MiB is *limit_mib
   (g_settings.log_size_limit, log_max), read each time a line could pass it. "log: written to <path>" on stderr, as
   the Switch's runtime says it, for the launcher's Share Log. At the process's end (atexit) the lines still in the
   pipes reach the file and the terminal, waited for 2 seconds at most. 0; -1 with why: nothing redirected */
int snx_game_log_start(const char* folder, const char* name, const int* limit_mib, char* why, size_t size);
/* the log's path, NULL before snx_game_log_start made it */
const char* snx_game_log_path(void);
/* the lines written so far in the file and on the terminal: waits, 2 seconds at most, until the copies took every
   byte the pipes hold. Any thread but a copy's */
void snx_game_log_sync(void);
/* A crash's last line, from a signal handler: async-signal-safe. The pipes' lines first (what the game said before,
   in its order), waited for 200 ms at most, then text straight to the file's descriptor (stamped) and the terminal's,
   with write; without a log, to stderr. A fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) the runner does not
   take itself says itself so ("*** Crash: signal 11") and goes on to the handler before */
void snx_game_log_crash(const char* text, size_t size);
/* log_max in MiB once, after the log first went past it and was cut, for the window to say over the game; 0 before
   that and after it was taken */
int snx_game_log_full_take(void);
#endif
