/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What stops a game, as the person playing meets it: the kind of error, the one line the error's panel says of it and
   what to do, and the actions that fix it, in the order the panel lists them. One table for every console's runtime,
   the launcher and the panel's own process (error_panel.h).

   An error goes from the runtime to the panel's process as a file of key=value lines (snx_error_write, snx_error_read),
   and to the launcher as the run's "error: " lines (snx_error_said, snx_error_heard): what stopped the game, whether
   its panel showed, and the action the person took there that the launcher does */
#ifndef SNX_ERROR_KIND_H
#define SNX_ERROR_KIND_H
#include <stddef.h>

/* the kinds, in the order of the table (error_kind.c); INTERNAL is every error of the runtime's own */
enum {
    SNX_ERROR_INTERNAL,
    SNX_ERROR_SETTING,       /* a setting is off that games need (hooks=0, hooks_off) */
    SNX_ERROR_SETTING_VALUE, /* a setting's value is no choice it has */
    SNX_ERROR_SDK32,         /* a 32-bit game of an SDK the runtime has not read */
    SNX_ERROR_NOT_YET,       /* a function, service or instruction the runtime does not do yet */
    SNX_ERROR_GAME_STOPPED,  /* the game stopped itself: svcBreak, an SDK abort, an error code */
    SNX_ERROR_CRASH,
    SNX_ERROR_GPU_OLD, /* the device has no Vulkan the runtime needs */
    SNX_ERROR_GPU_START,
    SNX_ERROR_GPU_LOST,
    SNX_ERROR_MEMORY,
    SNX_ERROR_CODE_OLD,     /* the recompiled code is another version's or another copy's */
    SNX_ERROR_CODE_MISSING, /* no recompiled code */
    SNX_ERROR_DUMP_MISSING, /* the dump the game reads is not there */
    SNX_ERROR_KEYS,
    SNX_ERROR_DISK,
    SNX_ERROR_SAVE,
    SNX_ERROR_NETWORK,
    SNX_ERROR_KILLED, /* the system ended the game's process */
    SNX_ERROR_BAD_FILE,
    SNX_ERROR_RECOMPILE, /* a recompile failed for another reason than these */
    SNX_ERROR_KINDS
};
/* what the panel offers; NONE is no action. The last three are its Details page's */
enum {
    SNX_ACTION_NONE,
    SNX_ACTION_OPEN_SETTINGS, /* Settings, at the row of the error's key */
    SNX_ACTION_GPU_DRIVERS,
    SNX_ACTION_ADDONS, /* the game's Updates and DLCs */
    SNX_ACTION_RECOMPILE,
    SNX_ACTION_LOCATE, /* where the game's dump is */
    SNX_ACTION_INSTALL_KEYS,
    SNX_ACTION_OPEN_FOLDER,
    SNX_ACTION_TRY_AGAIN,
    SNX_ACTION_SHARE_LOG,
    SNX_ACTION_DETAILS,
    SNX_ACTION_CLOSE, /* "Back to Menu" with the launcher behind the run */
    SNX_ACTION_OPEN_LOG,
    SNX_ACTION_COPY_TEXT,
    SNX_ACTION_BACK,
    SNX_ACTIONS
};
/* the panel's narrowest is SNX_MENU_COLS (40) columns, a column of margin at each side: a line is that wide at most */
#define SNX_ERROR_LINE_COLS   38
#define SNX_ERROR_ACTIONS_MAX 6

/* one error: what the runtime knows of it, each a text in its own bytes ("" for none). No pointer, so a crash handler
   fills one with nothing allocated and a file carries it whole */
typedef struct {
    int kind;
    int from_launcher;  /* the launcher started the run and waits for its end (--from-launcher) */
    int notice;         /* no game stopped: the launcher's own failure, its message whole and OK alone */
    char key[64];       /* SETTING, SETTING_VALUE, NETWORK: the .ini key Open Settings goes to */
    char line[160];     /* the panel's line where the kind's own says too little ("Vulkan API is 9: no such choice.") */
    char hint[160];     /* and the second line, where the kind's own does */
    char message[2048]; /* the error as the log says it, whole: Details shows it */
    char game[256];     /* the game's name, as the window's title says it */
    char program[1024]; /* what Try Again runs again and the launcher's pages are for: the game's program or dump */
    char folder[1024];  /* what Open Folder opens */
    char log[1024];     /* the game's log */
} SnxError;

/* the kind's name in an "error: " line and a record ("setting", "gpu-lost"), and back: -1 for a name of none */
const char* snx_error_kind_name(int kind);
int snx_error_kind_of(const char* name);
/* the action's name ("open-settings") and back (-1 for none), and its label on the panel: Close is "Back to Menu" when
   the launcher waits for the run */
const char* snx_error_action_name(int action);
int snx_error_action_of(const char* name);
const char* snx_error_action_label(int action, const SnxError* e);
/* the panel's line and second line: e's own where it has one, else its kind's */
const char* snx_error_line(const SnxError* e);
const char* snx_error_hint(const SnxError* e);
/* the actions the panel lists for e, in order, the one that fixes it first: those its kind has that e can do (Open
   Settings with a key, Recompile, Locate... and Updates and DLCs with a program, Open Folder with a folder, Try Again
   with a program or the launcher, Share Log with a log), then Details and Close. How many, SNX_ERROR_ACTIONS_MAX at most */
int snx_error_actions(const SnxError* e, int out[SNX_ERROR_ACTIONS_MAX]);
/* the kind of a message that no caller gave one: MEMORY for the words the runtime says running out of memory with
   ("out of memory", "no memory"), else INTERNAL */
int snx_error_kind_of_message(const char* message);

/* e as a file of key=value lines at path, and read back from one: 0, else nonzero with why (the path and the reason)
   in why. A value's line breaks are written as spaces; a key the reader does not know is refused with its name */
int snx_error_write(const SnxError* e, const char* path, char* why, size_t why_size);
/* the file's text into out (size bytes, ended by a 0): its length, 0 when it does not fit. Byte copies alone, no
   formatting, for a crash's handler (error_spawn.h) */
size_t snx_error_text(const SnxError* e, char* out, size_t size);
int snx_error_read(SnxError* e, const char* path, char* why, size_t why_size);

/* The run's "error: " lines, which the launcher reads in its output: the error ("error: setting key=hooks shown=1",
   shown 1 when its panel showed, 0 when the launcher has to show it), and the action taken on it that the launcher does
   ("error: action open-settings key=hooks"), each into out */
#define SNX_ERROR_SAID "error: "
void snx_error_said(const SnxError* e, int shown, char* out, size_t size);
void snx_error_action_said(const SnxError* e, int action, char* out, size_t size);
/* one line of a run's output read: 1 when it is an "error: " line, what it says into heard (an error's kind, key and
   shown, or an action and its key), 0 for another line, -1 for an "error: " line that says what no kind or action is,
   why in heard->why */
typedef struct {
    int kind;   /* -1 when the line is an action's */
    int shown;  /* the error's panel showed */
    int action; /* SNX_ACTION_NONE when the line is an error's */
    char key[64];
    char why[160];
} SnxErrorHeard;
int snx_error_heard(const char* line, SnxErrorHeard* heard);

/* A console runtime's stop (the GBA's, the DS's, the 3DS's, the Vita's), noted where it is said and shown once its
   driver returned (multicall's main, error_console.h): the line "<program>: <what>" on stderr, which the log's summary
   reads, and the error of that kind kept, the last one noted. A run with nobody at the window (headless, a frame or time
   limit) notes it quiet, and nothing is shown. snx_error_noted gives it, NULL for none or a quiet one */
[[gnu::format(printf, 3, 4)]] void snx_error_note(int kind, const char* program, const char* fmt, ...);
void snx_error_note_game(const char* game); /* the game's name, as its window's title says it */
void snx_error_note_quiet(int quiet);
const SnxError* snx_error_noted(void);
/* the run's --from-launcher=1 (game_ini.h): every error of it, the noted one too, carries from_launcher */
void snx_error_launched(int from_launcher);
int snx_error_from_launcher(void);

/* the launcher's --open=VALUE for action on e, for a run that no launcher waits for: "settings:hooks", "drivers",
   "keys", "addons:<program>", "recompile:<program>", "locate:<program>"; "" for an action the launcher does not open */
void snx_error_open_value(const SnxError* e, int action, char* out, size_t size);
#endif
