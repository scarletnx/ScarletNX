/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_TRACE_H
#define SNX_NVN_TRACE_H
#include "common/cpu/aarch64/cpu.h"

/* nvn_trace=1: an inventory of the NVN functions the game resolves and calls (gpu/nvn_trace.c). Init runs once the
   game's symbols and dispatch table are loaded, before any guest thread starts. */
void nvn_trace_init(void);
void nvn_trace_report(void);
/* 1 once nvn_trace_init found nvn_trace=1: wrappers replace dispatch slots */
int nvn_trace_active(void);
/* a NUL-terminated name of at most 127 characters in guest memory, read a page at a time (memory permissions are per
   page): its length, or 0 when it is empty, unreadable or longer */
u32 nvn_read_name(u64 ptr, char name[128]);

#endif
