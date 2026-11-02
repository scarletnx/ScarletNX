/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from eden-ns2 (src/core/hle/service/nvdrv/devices/nvhost_tsg_gpu.cpp, native_gpu_context.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project */
/* /dev/nvhost-tsg-gpu: channel membership and subcontext selection. */
#ifndef SNX_NVDRV_TSG_H
#define SNX_NVDRV_TSG_H
#include "nx/hle/services/nvdrv_ioctl.h"

#define NV_TSG_SUBCONTEXTS 64u
#define NV_TSG_CHANNELS 64u
#define NV_TSG_BIND_CHANNEL    NV_IOCTL(NV_IOCTL_DIR_RW, 'T', 0x01, 4)
#define NV_TSG_UNBIND_CHANNEL  NV_IOCTL(NV_IOCTL_DIR_RW, 'T', 0x02, 4)
#define NV_TSG_SET_TIMESLICE   NV_IOCTL(NV_IOCTL_DIR_RW, 'T', 0x08, 4)
#define NV_TSG_BIND_CHANNEL_EX NV_IOCTL(NV_IOCTL_DIR_RW, 'T', 0x0B, 24)
/* Bind/Unbind {u32 channel_fd}, BindEx {u32 channel_fd, subcontext_id; u64 reserved[2]},
   SetTimeslice {u32 microseconds}. */
enum { TSG_CHANNEL_FD = 0, TSG_SUBCONTEXT_ID = 4, TSG_RESERVED = 8 };
#endif
