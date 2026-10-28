/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the game's own NVN driver would answer, read from its code without running it (gpu/nvn/nvn_driver.c) */
#ifndef SNX_NVN_DRIVER_H
#define SNX_NVN_DRIVER_H
#include "common/cpu/aarch64/cpu.h"

/* The constant the driver's nvnDeviceGetInteger at func answers for item, through the switch in it or in the first
   function it calls (tools/nvn_census.py's api_version, for any item): 1 and *out, or 0 when that item's case isn't a
   constant (it reads the driver's device state) or the switch isn't found. mem is the host address of guest address 0
   (the runtime's g_mem), and the code and tables are read from [lo, hi) */
int nvn_driver_info(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out);
/* The NVN API version a driver's answers for items 0 and 1 (NVN_DEVICE_INFO_API_MAJOR_VERSION and _MINOR_VERSION)
   make, as the hooks' nvn key takes it (hle/hook/hook.h HOOK_NVN): major << 16 | minor, 0 when either is out of
   range (a major of 0, or either past 65535) */
u32 nvn_api_pack(i32 major, i32 minor);

#endif
