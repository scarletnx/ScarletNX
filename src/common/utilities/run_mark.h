/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A start's mark: a file, .running-<pid>, in the folder of a program's log (the launcher's, a game's), made as it
   starts, held by it (snx_lock_file) and deleted at its exit: exit() through atexit, plat_exit_now, the launcher's end.
   Its first line says its pid and the local time it started, its second that time in seconds since 1970. A mark there
   that no program holds is a start that did not end with an exit: a crash, or a kill (the system's, for its memory or
   from the app switcher, the user's, a power loss), which the next start in that folder says and deletes. Launchers
   open at once on one folder each hold their own. A process has one mark: an iOS game runs in the launcher's process,
   under its mark */
#ifndef SNX_RUN_MARK_H
#define SNX_RUN_MARK_H
#include <stddef.h>

/* a mark that no program holds, or one that cannot be opened to tell */
typedef struct {
    const char* text;  /* its first line, "pid 4242, started 2026-11-07 10:01:02"; "" when it holds none, NULL when it
                          cannot be opened: then whether its start ended is unknown, and left says why */
    long long pid;     /* its program's, from its name; from its text for logs/.running, an older iOS app's mark */
    long long started; /* its program's start, in seconds since 1970; 0 when the mark does not say */
    const char* left;  /* NULL once it is deleted, else why it is not: its path and the system's reason */
    int held;          /* snx_run_mark_look's alone: a program holds it, its start going on; its text NULL */
} SnxRunMark;

/* The marks of folder that no program holds, each given to seen with data and then deleted (one that cannot be
   opened given to it too, and left), and this process's own made there and held until snx_run_mark_end: 0. 1, and
   nothing done, when this process holds one already. -1 with why (the path and the system's reason) when the folder
   cannot be read or this process's mark cannot be made. folder may be of any length */
int snx_run_mark_start(const char* folder, void (*seen)(void* data, const SnxRunMark* mark), void* data, char* why,
                       size_t why_size);
/* The marks of folder, held or not, each given to seen with data and none deleted: a launcher's look at those of a
   game it started, beside the game's log, which the game's next start says and deletes. A held one has held set and
   no text; one that cannot be opened to tell has no text and left saying why. 0, or -1 with why (the path and the
   system's reason) when the folder cannot be read */
int snx_run_mark_look(const char* folder, void (*seen)(void* data, const SnxRunMark* mark), void* data, char* why,
                      size_t why_size);
/* 1 while this process holds its mark */
int snx_run_mark_held(void);
/* This process's mark deleted and let go, at an exit that ends it: from any thread, the first call alone doing it. A
   mark that cannot be deleted is said on stderr */
void snx_run_mark_end(void);
#endif
