/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Settings > App's Check for updates: the newest nightly as GitHub's API gives it (scarletnx/nightly's
   /releases/latest), whether it is newer than the build that runs, and its changes as a notice says them. No SDL, no
   network: the launcher asks (gui/main.c) */
#ifndef SNX_GUI_NIGHTLY_CHECK_H
#define SNX_GUI_NIGHTLY_CHECK_H
#include <stddef.h>
/* where the launcher asks: the daily nightlies' latest, never main's prerelease */
#define GUI_NIGHTLY_LATEST  "https://api.github.com/repos/scarletnx/nightly/releases/latest"
/* a notice's changes, at most, before "and N more" */
#define GUI_NIGHTLY_CHANGES 8
typedef struct {
    char tag[64];   /* nightly-YYYY-MM-DD-<7 hex> */
    char name[128]; /* Nightly YYYY-MM-DD (<7 hex>) */
    char url[256];  /* its page */
    char changes[GUI_NIGHTLY_CHANGES][160];
    int count, more; /* the changes kept, and how many more its notes list */
} GuiNightly;
/* GitHub's answer for the latest nightly (n bytes of JSON) into out: 1, else 0 with why saying what is wrong in it */
int gui_nightly_parse(const char* json, size_t n, GuiNightly* out, char* why, size_t size);
/* tag's nightly, the latest, newer than the build that runs, of another commit than commit: a nightly's build (branch
   nightly-YYYY-MM-DD) any other of its day or after, as the latest is the one made last; another build one of a day
   after committed, its commit's day. 1 newer, 0 not; GUI_NIGHTLY_NO_TAG when tag is no nightly-YYYY-MM-DD-<7 hex>,
   GUI_NIGHTLY_NO_DAY when the build has no day */
enum { GUI_NIGHTLY_NO_TAG = -1, GUI_NIGHTLY_NO_DAY = -2 };
int gui_nightly_newer(const char* tag, const char* branch, const char* commit, const char* committed);
#endif
