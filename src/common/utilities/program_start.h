/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the screens of a program a collection's program started show (transition_screen): the runtime's loading
   screen in the new program's window and the launcher's run screen while no window is open. Each says what it knows
   in the facts below, and one function writes the rows both draw as a loading screen's steps, "?" for what is not
   known: never a value made up */
#ifndef SNX_PROGRAM_START_H
#define SNX_PROGRAM_START_H

#define SNX_START_TEXT 96 /* a value's bytes, the row's width cutting it where it is drawn */
#define SNX_START_ROWS 10
typedef struct {
    int program, programs, previous; /* the program that starts, the application's count of them and the one that
                                        started it: -1 for one not known */
    char user[SNX_START_TEXT];       /* the user it plays as, its nickname: "" not known */
    int user_number;                 /* that user's number, 1 on; 0 not known */
    int user_kept;                   /* 1 the program before stored its open user for this one, 0 not, -1 not known */
    char version[24];                /* the game's version as it shows it: "" not known */
    int updated;                     /* 1 an update plays, 0 none, -1 not known */
    char update[24];                 /* the update's version, "v196608": "" not known */
    int dlcs;                        /* the DLCs that play: -1 not known */
    char dlc[SNX_START_TEXT];        /* and they, "DLC 1, DLC 2" */
    int storages;                    /* what the program before handed over: -1 a screen that can't say (no row) */
    unsigned long long storage_bytes;
    char gpu[SNX_START_TEXT];    /* the GPU the game draws on: "" not known */
    unsigned long long title_id; /* the program's: 0 not known */
    char build[SNX_START_TEXT];  /* ScarletNX's build: "" not known */
    int recompile;               /* 0 the program is made, 1 it is recompiled (its own step says it), -1 no row */
} SnxProgramStart;
typedef struct {
    char label[16];
    char value[SNX_START_TEXT];
} SnxStartRow;
/* the rows of facts into rows (SNX_START_ROWS at most), in their order: PROGRAM, STARTED BY, USER, VERSION, DLC,
   HANDED OVER, GPU, TITLE ID, BUILD, RECOMPILE; a row a screen can't say left out. Their count */
int snx_program_start_rows(const SnxProgramStart* facts, SnxStartRow rows[SNX_START_ROWS]);
/* facts with nothing known: every number -1 (user_number and title_id 0), every text "" */
void snx_program_start_unknown(SnxProgramStart* facts);
#endif
