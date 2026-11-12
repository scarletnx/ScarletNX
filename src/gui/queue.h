/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The compiles, run one at a time in the order asked, behind the menu's screens: R's recompile and B's AOT pack, which
   play nothing, and the first compile of a game opened that no game was made from yet, which plays once it ended. Each
   keeps the driver's argv as it was asked, its game's file and the name it is shown by, in one block of its own. The
   queue starts no process: main.c starts the first waiting one when none runs, reads it, and says here how it ended */
#ifndef SNX_GUI_QUEUE_H
#define SNX_GUI_QUEUE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_QUEUE_MAX  16 /* the jobs a queue holds: waiting, running and ended */
/* a job's argv at most, its NULL included: a run's options, and --update and a dump for each update and DLC a game's
   addons= can check (SNX_ADDONS_MAX), which a game's recompile with them gives the driver */
#define GUI_QUEUE_ARGS (64 + 2 * 2048)
typedef enum { GUI_JOB_WAITING, GUI_JOB_RUNNING, GUI_JOB_OK, GUI_JOB_FAILED } GuiJobState;
typedef enum { GUI_JOB_RECOMPILE, GUI_JOB_AOT, GUI_JOB_COMPILE } GuiJobKind;
typedef struct {
    /* the block: where each of argv's strings is in it, argc 32-bit offsets, then the strings, the file, its path
       listed and the name, a NUL each */
    char* text;
    int argc;
    /* the game's file as it runs (absolute), the path Games or the recent games list it by, and what Games shows it
       as, at text + */
    uint32_t file, listed, name;
    GuiJobKind kind;
    GuiJobState state;
    bool native; /* a game made already, which its recompile's line names */
    int console; /* the menu's item whose driver runs it (GUI_SWITCH, GUI_3DS...), which main.c sets */
    int status;  /* the driver's exit status, once it ended */
    int percent; /* how far it went, -1 before it said */
    uint64_t started_ms, ended_ms;
    uint32_t serial; /* which job it is, from 1: an output kept says whose it is */
    char said[160];  /* how it ended, once it did */
} GuiJob;
typedef struct {
    GuiJob job[GUI_QUEUE_MAX];
    int count;
    uint32_t serial; /* the last job's */
    uint32_t first;  /* the first job of the compiles going on: the one added while none waited or ran */
} GuiQueue;

/* argv (its strings up to a NULL), the game's file, the path it is listed by and its name as a job waiting at the
   queue's end: its index. A job of the same file that waits or runs, a compile or recompile for either, an AOT pack for
   an AOT pack, is that one, and *added says which
   (NULL: not said). -1 when the queue is full, or argv has more than GUI_QUEUE_ARGS - 1 strings */
int gui_queue_add(GuiQueue* queue, const char* const* argv, const char* file, const char* listed, const char* name,
                  GuiJobKind kind, bool native, bool* added);
/* the job that waits or runs of path, its file or the path it is listed by; -1 for none */
int gui_queue_find(const GuiQueue* queue, const char* path);
/* the job running, the first waiting, -1 for none; how many wait */
int gui_queue_running(const GuiQueue* queue);
int gui_queue_next(const GuiQueue* queue);
int gui_queue_waiting(const GuiQueue* queue);
/* how far the compiles going on have gone, in percent: the jobs from queue->first on, each ended one whole however it
   ended, the running one as far as it said and a waiting one at 0, of one weight each (a game's size is known only once
   it runs); -1 while none has said how far */
int gui_queue_percent(const GuiQueue* queue);
/* job i's place in the line, 1 the running one's (or the first waiting's while none runs); 0 for one that ended */
int gui_queue_place(const GuiQueue* queue, int i);
/* job i's argv into out (room for its argc and a NULL after the last): its count */
int gui_queue_argv(const GuiQueue* queue, int i, const char** out);
const char* gui_queue_file(const GuiQueue* queue, int i);
const char* gui_queue_listed(const GuiQueue* queue, int i);
const char* gui_queue_name(const GuiQueue* queue, int i);
/* job i out of the queue, those after it a place up */
void gui_queue_remove(GuiQueue* queue, int i);
void gui_queue_free(GuiQueue* queue);
/* a place past the running one as the queue's rows say it: "next" for 2, else its ordinal ("3rd"), translated */
void gui_queue_place_text(int place, char* out, size_t size);
#endif
