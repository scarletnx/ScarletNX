/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_DRIVER_SETUP_H
#define SNX_DRIVER_SETUP_H
#include <stdio.h>

/* The setup (common/utilities/setup.h) in a terminal: its questions on out, each answer a line of in, Enter the one in
   brackets. all goes through every notice and step, else through those setup.ini has left, saying nothing with none.
   program_dir is the program's folder, the data folder's and a portable copy's. 0 once done, else 1, out saying why:
   in ended, or a file could not be written. What a step did stays; setup.ini is written once all are done */
int snx_setup_terminal(FILE* in, FILE* out, const char* program_dir, int all);
/* the launcher's languages (gui/main.c), for the first step: the code of the i-th and its name, NULL past the last.
   Weak: a build of the driver alone has no launcher, and no language to ask */
[[gnu::weak]] const char* snx_menu_language(int i, const char** name);
#endif
