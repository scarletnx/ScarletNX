/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's command buffers (gpu/nvn/nvn_cmd.c): the game's NVN commands recorded into its command memory. The
   functions are the NVN functions' (gpu/nvn/nvn.hook.h); here is the file's state */
#ifndef SNX_NVN_CMD_H
#define SNX_NVN_CMD_H
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h"

/* a segment of the runtime's own command memory lent to a command buffer, from the game's memory it continues
   (origin), taken until the game adds that memory again */
typedef struct {
    u64 at, size, origin;
    u32 taken, pad;
} NvnLent;
/* the file's state, g_nvn.cmd (gpu/nvn/nvn_state.h): the segments lent, under the lock (the game's threads record
   into several command buffers at once), and the bytes of the kernel region lent */
typedef struct {
    PMutex lock;
    NvnLent* l;
    u32 n, cap;
    u64 total;
} NvnCmdState;

#endif
