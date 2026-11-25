/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which hook families the runtime can run where it runs (hook.h's hook_family_unready), apart from hook.c: the tests
   that install hooks over a table of their own link hook.c without the handlers this asks */
#include "nx/nn/hook.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/options.h"
#include <string.h>

/* the families whose handlers run only where the runtime has what they run on: alloc the host allocator's arena in
   guest memory (alloc.c), where the game's malloc isn't an allocator object's of its own (alloc_kept), and it, sync, cond_var, os_sync, os_light and os_core 64-bit games' registers and pointers;
   cond_var, os_sync and os_light, whose waits a save state can't hold, only with native_cond_var=1 and
   native_os_sync=1; and os_sync only where the game's SDK exports every function that writes the words it owns. The
   reason a family is off for, NULL when it may be installed */
const char* hook_family_unready(const char* family) {
    int alloc = !strcmp(family, "alloc"), sync = !strcmp(family, "sync"), cond = !strcmp(family, "cond_var"),
        os = !strcmp(family, "os_sync"), light = !strcmp(family, "os_light"), core = !strcmp(family, "os_core");
    if ((alloc || sync || cond || os || light || core) && g_arm_mod) return "its handlers are for 64-bit games";
    if (cond && !g_settings.native_cond_var)
        return "native_cond_var is 0, so the game's mutexes and condition variables run its own code, which save "
               "states hold";
    if ((os || light) && !g_settings.native_os_sync)
        return "native_os_sync is 0, so nn::os's sleeps, locks and condition variables run the game's own code, which "
               "save states hold";
    if (os && !os_sync_ready())
        return "the game's SDK doesn't export every function that writes nn::os's lock and condition variable words "
               "(InternalCriticalSectionImplByHorizon's and InternalConditionVariableImplByHorizon's)";
    if (alloc && alloc_kept()) return alloc_kept();
    if (alloc && !alloc_ready()) return "the host allocator has no arena in guest memory this run";
#if defined(_WIN32)
    if (!strcmp(family, "libc_str") || !strcmp(family, "libc_fmt"))
        return "its handlers take the C library of a POSIX host";
#endif
    if (!strcmp(family, "libc_wide") && !libc_wide_ready())
        return "the host has no 32-bit wchar_t or no UTF-8 locale for its handlers";
    return NULL;
}
