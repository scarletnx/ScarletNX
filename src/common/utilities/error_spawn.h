/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error's panel in a process of its own (scarletnx --error-screen, error_sdl.h), for an error whose game's window
   cannot show it: the window not made yet, its renderer holding it (a GPU stuck), an error of a thread while the main
   thread is held elsewhere, a crash. The game's process writes the error's record (error_kind.h) and waits for the
   panel's process, whose status says the action taken.

   Prepared once, as the run starts (snx_error_spawn_prepare): the program to run and where the record goes, so that a
   crash's handler, where the heap and the locks may be what broke, makes nothing: its record is written with write(),
   the process started with _Fork (fork where there is none) and execve, and waited for with waitpid */
#ifndef SNX_ERROR_SPAWN_H
#define SNX_ERROR_SPAWN_H
#include "common/utilities/error_kind.h"
#include <stddef.h>

/* the program that shows the panel (this one, the multicall's: its --error-screen) and the record's path, copied: 0, or
   nonzero when either is too long, said in why */
int snx_error_spawn_prepare(const char* program, const char* record, char* why, size_t why_size);
/* whether it was prepared */
int snx_error_spawn_ready(void);
/* e shown in the panel's process, waited for: the action taken there (SNX_ACTION_*), or -1 when it could not be
   started or could not show it, why in why (the system's reason, or the process's status) */
int snx_error_spawn(const SnxError* e, char* why, size_t why_size);
#endif
