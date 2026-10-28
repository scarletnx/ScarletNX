/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvmap.h, nvmap.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* /dev/nvmap's ioctls: their numbers (group 0x01, the commands Eden's nvmap::Ioctl1 serves) and the byte offsets of
   the fields of their arguments, named after Eden's Ioc*Params members. nvdrv.c's ioctl_nvmap reads and writes the
   arguments through these offsets, little-endian, as the guest lays them out */
#ifndef SNX_NVDRV_NVMAP_H
#define SNX_NVDRV_NVMAP_H
#include "nx/hle/services/nvdrv_ioctl.h"

#define NVMAP_IOCTL_GROUP 0x01

/* commands (an ioctl's low byte) */
enum {
    NVMAP_IOC_CREATE = 0x01,  /* IocCreate */
    NVMAP_IOC_FROM_ID = 0x03, /* IocFromId */
    NVMAP_IOC_ALLOC = 0x04,   /* IocAlloc */
    NVMAP_IOC_FREE = 0x05,    /* IocFree */
    NVMAP_IOC_PARAM = 0x09,   /* IocParam */
    NVMAP_IOC_GET_ID = 0x0E   /* IocGetId */
};

/* IocCreateParams {u32 size; out u32 handle} */
enum { NVMAP_CREATE_SIZE = 0, NVMAP_CREATE_HANDLE = 4 };
/* IocFromIdParams {u32 id; out u32 handle} */
enum { NVMAP_FROM_ID_ID = 0, NVMAP_FROM_ID_HANDLE = 4 };
/* IocAllocParams {u32 handle, heap_mask, flags, align; u8 kind; 7 bytes padding; u64 address} */
enum {
    NVMAP_ALLOC_HANDLE = 0,
    NVMAP_ALLOC_HEAP_MASK = 4,
    NVMAP_ALLOC_FLAGS = 8,
    NVMAP_ALLOC_ALIGN = 12,
    NVMAP_ALLOC_KIND = 16,
    NVMAP_ALLOC_ADDRESS = 24
};
/* IocAllocParams.flags: bit 1 asks for uncached memory (Switchbrew's NV services) */
#define NVMAP_ALLOC_UNCACHED 2u
/* IocFreeParams {u32 handle; 4 bytes padding; out u64 address, u32 size, u32 flags}. The address and size are 0 unless
   this was the handle's last reference; flags is 1 (WAS_UNCACHED) when the allocation asked for uncached memory */
enum { NVMAP_FREE_HANDLE = 0, NVMAP_FREE_ADDRESS = 8, NVMAP_FREE_SIZE = 16, NVMAP_FREE_FLAGS = 20 };
#define NVMAP_FREE_WAS_UNCACHED 1u
/* IocParamParams {u32 handle, u32 param; out u32 result} */
enum { NVMAP_PARAM_HANDLE = 0, NVMAP_PARAM_PARAM = 4, NVMAP_PARAM_RESULT = 8 };
/* IocGetIdParams {out u32 id; u32 handle} */
enum { NVMAP_GET_ID_ID = 0, NVMAP_GET_ID_HANDLE = 4 };

/* IocParam's param, Eden's HandleParameterType */
enum {
    NVMAP_PARAM_SIZE = 1,
    NVMAP_PARAM_ALIGNMENT = 2,
    NVMAP_PARAM_BASE = 3,
    NVMAP_PARAM_HEAP = 4,
    NVMAP_PARAM_KIND = 5,
    NVMAP_PARAM_IS_SHARED_MEM_MAPPED = 6 /* Eden's ParamTypes calls it Compr: 0 here, nothing is compressed */
};
/* what PARAM answers for the alignment of a handle ALLOC gave none (the 4 KiB page, the least alignment Eden's IocAlloc
   forces), and for an allocated handle's heap (the value Eden's IocParam answers) */
#define NVMAP_DEFAULT_ALIGNMENT 0x1000u
#define NVMAP_HEAP_ALLOCATED    0x40000000u
#endif
