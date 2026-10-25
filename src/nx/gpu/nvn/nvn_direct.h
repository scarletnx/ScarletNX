/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DIRECT_H
#define SNX_NVN_DIRECT_H

/* nvn_direct=1: the runtime runs the game's NVN calls itself, and the game's NVN driver doesn't run
   (gpu/nvn/nvn_direct.c). Init runs once the game's symbols and dispatch table are loaded, before any guest thread
   starts, and before nvn_trace_init, which stands aside under it */
void nvn_direct_init(void);
/* 1 once nvn_direct_init found nvn_direct=1: the NVN functions the game resolves are the runtime's */
int nvn_direct_active(void);

#endif
