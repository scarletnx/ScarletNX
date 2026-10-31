/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* g_nvn, nvn_direct's state, its initialization, and t_nvn, its thread state (gpu/nvn/nvn_state.h) */
#include "nx/gpu/nvn/nvn_state.h"

/* zero, in .bss: nvn_init sets what starts out non-zero */
Nvn g_nvn;
_Thread_local NvnThread t_nvn;

void nvn_init(void) {
    /* every lock: PMUTEX_INIT's value isn't all zero on every platform */
    pm_init(&g_nvn.direct.lock);
    pm_init(&g_nvn.object.xfb_lock);
    pm_init(&g_nvn.cmd.lock);
    pm_init(&g_nvn.replay.lock);
    pm_init(&g_nvn.copy.read_lock);
    pm_init(&g_nvn.verify.lock);
    pm_init(&g_nvn.verify.cmd.lock);
}
