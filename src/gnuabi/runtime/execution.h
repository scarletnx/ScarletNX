/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_EXECUTION_H
#define SNX_GNU_EXECUTION_H
#include "gnuabi/process/process.h"
/* Runs thread's guest code, and the programs it execs, on this host thread until the thread or the process ends; the
   process's exit status. A fault in guest code (SIGSEGV, SIGBUS) ends the process: its registers at the fault are
   not exact, so no guest handler is entered for it */
int gnu_runtime_run(GnuThread* thread);
#endif
