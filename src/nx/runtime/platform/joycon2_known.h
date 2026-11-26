/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* joycon2.txt, the controllers the runtime knows, which the launcher's Controllers > Bluetooth writes too: a line each,
   "AA:BB:CC:DD:EE:FF L SERIAL" (L or R a Joy-Con 2's side, P a Pro Controller 2), read and written by the rule of every
   settings file (common/utilities/config_file.h) */
#ifndef SNX_JOYCON2_KNOWN_H
#define SNX_JOYCON2_KNOWN_H
#include <stdint.h>

typedef struct {
    uint8_t address[6];
    char side; /* L, R or P */
    char serial[17];
} Jc2KnownLine;
/* joycon2.txt at path read into lines, max at most: how many. A line that names no controller (another version's, a
   hand's) is named once a run, and one that cannot be read said, with snx_error */
int joycon2_known_load(const char* path, Jc2KnownLine* lines, int max);
/* the n controllers of lines written to path, the file's lines that name none after them as they were, beside it
   first, never over one that cannot be read: 0, or -1 said with snx_error */
int joycon2_known_save(const char* path, const Jc2KnownLine* lines, int n);
#endif
