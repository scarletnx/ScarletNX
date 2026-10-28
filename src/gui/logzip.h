/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Share Log's file: the logs in one zip, deflated by miniz, a text of what the launcher says first (summary.txt) and
   each log under a name of its own; every home folder written as ~ (logscan.h's snx_logscan_tilde), since the paths
   name the player */
#ifndef SNX_GUI_LOGZIP_H
#define SNX_GUI_LOGZIP_H
#include <stddef.h>

typedef struct {
    const char* path; /* on disk */
    const char* name; /* in the zip: launcher/scarletnx-console-2026-10-28.log, games/Some Game [0100...]/... */
} GuiZipFile;

/* the zip at path: summary.txt (summary, then a line for each file left out: its path and why), and each of the count
   files that could be read, under its name and with its last change. 0, or nonzero with why in error */
int gui_logs_zip(const char* path, const char* summary, const GuiZipFile* files, size_t count, char* error,
                 size_t size);
#endif
