/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/jit/jit_settings.h"
#include "common/cpu/recompiler/loader/gdb_jit.h"
#include <stdio.h>
#include <stdlib.h>

void snx_jit_settings(const char* who, int perf_map, int gdb_jit) {
    snx_gdb_jit_perf_map(perf_map != 0);
    /* the lines a load recorded and could not write itself, appended as the process ends, as the Switch's JIT does */
    if (perf_map) (void)atexit(snx_gdb_jit_perf_map_snapshot);
    if (!gdb_jit) return;
#ifdef _WIN32
    fprintf(stderr, "%s: gdb_jit=1 names nothing to GDB on Windows, whose loader takes COFF: the JIT's code goes unnamed\n",
            who);
#else
    snx_gdb_jit_enable();
    fprintf(stderr,
            "%s: the code the JIT compiles is named to a GDB attached to this process (gdb_jit=1): it stops the game "
            "for each compile while attached\n",
            who);
#endif
}
