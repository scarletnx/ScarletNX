/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The setup a first start goes through, in the launcher (gui/main.c) and in a terminal (the driver's setup.c): what
   older versions did that this one does otherwise (the notices), then the language, the install, the folders, the keys
   and the first user. setup.ini in the config folder says how far it went: the version of the steps it asked and the
   notices it showed. A step a newer version adds raises SNX_SETUP_VERSION and is asked alone where setup.ini is of an
   older one; a notice goes last in snx_notices and is shown alone the next start. --no-setup leaves it out of a run,
   writing nothing: a run without users.ini plays as Player, of the fixed uid. --setup goes through it again */
#ifndef SNX_COMMON_SETUP_H
#define SNX_COMMON_SETUP_H
#include <stddef.h>
#include <stdint.h>

#define SNX_SETUP_VERSION 1
enum { SNX_SETUP_LANGUAGE, SNX_SETUP_INSTALL, SNX_SETUP_FOLDERS, SNX_SETUP_KEYS, SNX_SETUP_USER, SNX_SETUP_STEPS };
extern const int snx_setup_since[SNX_SETUP_STEPS]; /* the version each step came in */
/* what an older version did that this one does otherwise, on the systems it says: what, where it was, where it is now,
   and words of its row in docs/DEPRECATIONS.md's table, which lists it in full. The first three in English, those with
   words marked for scripts/build-languages.py: the launcher shows them in its language (gui/i18n.h), the terminal as
   they are */
#ifndef SNX_TR_NOOP
#define SNX_TR_NOOP(s) s
#endif
enum { SNX_NOTICE_ALL, SNX_NOTICE_LINUX, SNX_NOTICE_ANDROID };
typedef struct {
    const char *what, *before, *now, *row;
    int systems;
} SnxNotice;
#define SNX_NOTICES 18
extern const SnxNotice snx_notices[SNX_NOTICES]; /* the oldest first */
int snx_notice_here(int notice);                 /* 1 for one of this system's */

typedef struct {
    int version; /* the steps asked: those of SNX_SETUP_VERSION and before; 0 before the first setup */
    int notices; /* the first snx_notices shown */
} SnxSetup;
/* setup.ini of the data folder data: in its config folder (snx_menu_dir); NULL with no home to name it by */
char* snx_setup_file(const char* data);
/* setup.ini at path into setup, zeros without the file: 0, else -1 with why, setup holding what could be read */
int snx_setup_read(const char* path, SnxSetup* setup, char* why, size_t why_size);
/* setup.ini at path once this version's steps and notices are done, a newer file's version kept: 0, else -1 with why */
int snx_setup_done(const char* path, const SnxSetup* setup, char* why, size_t why_size);
int snx_setup_asks(const SnxSetup* setup, int step); /* 1 for a step newer than setup.ini */
int snx_setup_here(int step); /* 1 for a step this system has: an app (Android, iOS) has no install to pick */
int snx_setup_shows(const SnxSetup* setup, int notice); /* 1 for this system's notice not shown yet */
int snx_setup_due(const SnxSetup* setup);               /* 1 while a step or a notice is left */

/* --no-setup (its old names --debug and --skip --skip too) and --setup taken out of argv, before a "--": snx_setup_skip
   and snx_setup_again say which. The arguments left, else -1 with why */
extern int snx_setup_skip, snx_setup_again;
int snx_setup_options(int argc, char** argv, char* why, size_t why_size);

/* a new user's uid: 16 bytes of the system's generator (RAND_bytes), its text (users.h) a UUID's of RFC 4122's version
   4 and variant. 0, else -1 */
int snx_setup_uid(uint64_t uid[2]);
/* the folders of games with Account saves of the fixed uid (users.h), which user 1 with another uid does not see */
int snx_setup_first_saves(const char* games);
/* the uid of a new users.ini's first user: the fixed one while games has saves of it, else snx_setup_uid's. How many
   games have them, else -1 when the generator failed */
int snx_setup_first_uid(const char* games, uint64_t uid[2]);

/* 1 for text as a prod.keys or a title.keys has it: no NUL byte, and a line "name = hex" at least */
int snx_keys_text(const char* data, size_t size);
/* name (prod.keys, title.keys) where the driver reads it: in the data folder data, else in ~/.switch (%USERPROFILE% on
   Windows); NULL for neither */
char* snx_keys_found(const char* data, const char* name);
/* key paths explicitly supplied to the current driver's run, visible to the setup's key check */
typedef struct {
    const char* prod;
    const char* title;
} SnxSetupKeysOverride;
SnxSetupKeysOverride snx_setup_keys_override_get(void);
void snx_setup_keys_override_set(const char* prod, const char* title);
/* the folder the keys are installed in: data when it is the user's data folder on an XDG system, which the driver
   reads first, so nothing is written in the home folder itself; else ~/.switch. NULL with no home */
char* snx_keys_dir(const char* data);
/* data written at path, readable by its owner alone where the system says so, through a file beside it first: a failed
   copy leaves the keys there. 0, else nonzero with the system's reason in why */
int snx_keys_put(const char* path, const void* data, size_t size, char* why, size_t why_size);
/* folder's prod.keys installed in dir, and its title.keys when it has one (*titles says so): 0, else -1 with why */
int snx_keys_install(const char* folder, const char* dir, int* titles, char* why, size_t why_size);

/* path as the setup shows it: ~/ for the home folder at its start (not on Windows) */
void snx_home_short(const char* path, char* out, size_t size);

/* key = value in section of the .ini at path, its other lines as they were, the file made when it is not there: 0,
   else -1 with why */
int snx_ini_put(const char* path, const char* section, const char* key, const char* value, char* why, size_t why_size);
/* line at the end of the file of lines at path (the launcher's folders.txt) unless one is it: 0, else -1 with why */
int snx_lines_add(const char* path, const char* line, char* why, size_t why_size);
#endif
