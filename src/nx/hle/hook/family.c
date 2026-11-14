/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which hook families the runtime can run where it runs (hook.h's hook_family_unready), apart from hook.c: the tests
   that install hooks over a table of their own link hook.c without the handlers this asks */
#include "nx/hle/hook/hook.h"
#include "nx/runtime/arm.h"
#include <string.h>

/* the families whose handlers run only where the runtime has what they run on: alloc the host allocator's arena in
   guest memory (alloc.c), and both it and sync 64-bit games' registers and pointers. The reason a family is off for,
   NULL when it may be installed */
const char* hook_family_unready(const char* family) {
    int alloc = !strcmp(family, "alloc"), sync = !strcmp(family, "sync");
    if ((alloc || sync) && g_arm_mod) return "its handlers are for 64-bit games";
    if (alloc && !alloc_ready()) return "the host allocator has no arena in guest memory this run";
#if defined(_WIN32)
    if (!strcmp(family, "libc_str")) return "its handlers take the C library of a POSIX host";
#endif
    return NULL;
}
