/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What follows the error's panel (error_sdl.h): its action done, for every console's runtime; and the panel of a
   console whose driver returned on a stop it noted (error_kind.h's snx_error_note), which multicall's main shows */
#ifndef SNX_ERROR_CONSOLE_H
#define SNX_ERROR_CONSOLE_H
#include "common/utilities/error_kind.h"
#include <stddef.h>

/* action, taken on e's panel, done. With the launcher waiting for the run (e's from_launcher), the run's "error: action"
   line on stdout, which the launcher reads and does. With none: Try Again starts program again with argv (argv[0]
   the driver's name), an action of the launcher's starts it at its page (program scarletnx-switch --open=VALUE), Close
   does nothing. 0, or nonzero with why: the process could not be started */
int snx_error_follow(const SnxError* e, int action, const char* program, const char* const* argv, char* why,
                     size_t why_size);
/* the program's arguments as it was started (multicall's main), copied: what Try Again starts again, argv NULL to
   snx_error_follow taking them. NULL before */
void snx_error_args(int argc, const char* const* argv);
const char* const* snx_error_saved_args(void);
/* multicall's, once a console's driver returned status: its noted stop, if one is and someone is at its window, shown in
   a window of the panel's own, its "error: " lines said and its action followed (argv the driver's own). status as it
   was */
int snx_error_console_end(int status, const char* const* argv);
#endif
