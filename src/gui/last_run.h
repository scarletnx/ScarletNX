/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Last Run (docs/user/gui.md): the run it started last kept in its state folder, beside that run's log
   (last-run.ini beside last-run.log), from its start until the menu sees it end, and deleted then. A start that finds
   one did not see that run end: the menu was ended with it (Android's low memory killer takes the menu first, then the
   game), and nothing said how the run went. What the system says of a process's end, and of its memory killer, as the
   menu reads it. No SDL here */
#ifndef SNX_GUI_LAST_RUN_H
#define SNX_GUI_LAST_RUN_H
#include <stddef.h>

typedef struct {
    char* program;     /* the game's program it plays, NULL where the run's output names its log (a computer's) */
    char* name;        /* what the menu calls it: the program's leaf, or the file opened */
    char* probe;       /* Run edenprobe's folder (gui/probe.h), NULL for a game */
    long long started; /* seconds since 1970 */
    int cases, launch; /* the probe's: its cases before this launch (-1 none yet), and the launch's number from 1 */
} GuiLastRun;

/* the record beside run_log (last-run.log's last-run.ini), owned; NULL for none */
char* gui_last_run_path(const char* run_log);
/* run written to path, whole or not at all (a .new file named over it): 0, or -1 with why saying the path and the
   system's reason */
int gui_last_run_write(const char* path, const GuiLastRun* run, char* why, size_t size);
/* the record at path read into run, then deleted: 1; 0 when there is none; -1 with why when it cannot be read, holds
   no started= or cannot be deleted (run filled all the same when it was read: the next start says it again) */
int gui_last_run_take(const char* path, GuiLastRun* run, char* why, size_t size);
void gui_last_run_free(GuiLastRun* run);

/* What the system says of a process's end: Android's ApplicationExitInfo, as MainActivity.exitFields gives it, a
   "key=value" line each (reason, said, process, status, importance, pss, rss, description), or none= saying why
   there is none */
typedef struct {
    int known;             /* the system gave the end */
    int reason;            /* ApplicationExitInfo's REASON_* number; -1 when not known */
    int status;            /* the exit status or the signal */
    long long pss, rss;    /* KiB; -1 when not said, 0 when Android measured none (a process killed by a signal) */
    char said[64];         /* the reason's name, Android's: "low memory" */
    char process[128];     /* "org.scarletnx:game" */
    char importance[64];   /* "100 foreground" */
    char description[256]; /* Android's own words, "" for none */
    char none[512];        /* why it is not known */
} GuiSystemEnd;
/* text (NULL: the system says none, none saying so) into end */
void gui_system_end_read(const char* text, GuiSystemEnd* end);
/* whether reason is one of Android's own stops: low memory, ANR, a permission or package change, too much of a
   resource, another process's end it depends on, the freezer, its other kills. Not the process's exit, crash or
   failure to start, a signal that names no sender (signaled), nor the user's stop */
int gui_system_stopped(int reason);

/* The kernel's out-of-memory kills (Linux): oom_kill of the memory cgroup this process is in and below
   (/sys/fs/cgroup/<its path>/memory.events, cgroup v2; events in its place when not NULL), else of the whole system
   (/proc/vmstat), file naming which; -1 where neither is kept (another system, or neither can be read) */
long long gui_oom_kills(const char* events, char* file, size_t size);
/* the number on text's line "oom_kill N" (a memory.events' or /proc/vmstat's lines); -1 when it has none */
long long gui_oom_kills_in(const char* text);
#endif
