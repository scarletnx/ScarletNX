/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_TRACE_H
#define SNX_NVN_TRACE_H

/* SNX_NVN_TRACE=1: an inventory of the NVN functions the game resolves and calls (gpu/nvn_trace.c). Init runs once the
   game's symbols and dispatch table are loaded, before any guest thread starts. */
void nvn_trace_init(void);
void nvn_trace_report(void);
/* 1 once nvn_trace_init found SNX_NVN_TRACE=1: wrappers replace dispatch slots */
int nvn_trace_active(void);

#endif
