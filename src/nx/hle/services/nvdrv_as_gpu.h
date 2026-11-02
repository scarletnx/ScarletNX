/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvhost_as_gpu.h, nvhost_as_gpu.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 Skyline Team and Contributors */
/* /dev/nvhost-as-gpu's ioctls (group 'A', the commands Eden's nvhost_as_gpu serves), the byte offsets of their
   arguments' fields named after Eden's Ioctl* structs, the mapping flags, and the address-space layout Eden's VM
   struct defaults to. nvdrv.c's ioctl_as uses them */
#ifndef SNX_NVDRV_AS_GPU_H
#define SNX_NVDRV_AS_GPU_H
#include "nx/hle/services/nvdrv_ioctl.h"

#define NVHOST_AS_GPU_IOCTL_GROUP 'A'

/* commands (an ioctl's low byte) */
enum {
    NVHOST_AS_GPU_BIND_CHANNEL = 0x01,   /* BindChannel */
    NVHOST_AS_GPU_ALLOC_SPACE = 0x02,    /* AllocateSpace */
    NVHOST_AS_GPU_FREE_SPACE = 0x03,     /* FreeSpace */
    NVHOST_AS_GPU_UNMAP_BUFFER = 0x05,   /* UnmapBuffer */
    NVHOST_AS_GPU_MAP_BUFFER_EX = 0x06,  /* MapBufferEx */
    NVHOST_AS_GPU_GET_VA_REGIONS = 0x08, /* GetVARegions (Ioctl3 also writes the regions to its second output) */
    NVHOST_AS_GPU_ALLOC_AS_EX = 0x09,    /* AllocAsEx */
    NVHOST_AS_GPU_SYNCPOINT_SHIM = 0x0b, /* GetSyncpointShimInfo, a Switch 2 driver's */
    NVHOST_AS_GPU_REMAP = 0x14           /* Remap */
};

/* MappingFlags, of ALLOC_SPACE and MAP_BUFFER_EX */
enum {
    NVHOST_AS_MAP_FIXED = 1u << 0, /* the offset field is the GPU address, not an alignment */
    NVHOST_AS_MAP_SPARSE = 1u << 1,
    NVHOST_AS_MAP_REMAP = 1u << 8 /* MAP_BUFFER_EX: change part of an earlier mapping (with handle 0, its kind only) */
};

/* IoctlAllocSpace {u32 pages, u32 page_size, MappingFlags flags; 4 bytes padding; u64 offset or align, out offset} */
enum { AS_ALLOC_SPACE_PAGES = 0, AS_ALLOC_SPACE_PAGE_SIZE = 4, AS_ALLOC_SPACE_FLAGS = 8, AS_ALLOC_SPACE_OFFSET = 16 };
/* IoctlFreeSpace {u64 offset, u32 pages, u32 page_size} */
enum { AS_FREE_SPACE_OFFSET = 0, AS_FREE_SPACE_PAGES = 8, AS_FREE_SPACE_PAGE_SIZE = 12 };
/* IoctlUnmapBuffer {s64 offset} */
enum { AS_UNMAP_BUFFER_OFFSET = 0 };
/* IoctlMapBufferEx {MappingFlags flags, u32 kind, u32 handle, u32 page_size; s64 buffer_offset, u64 mapping_size,
   s64 offset (in and out)} */
enum {
    AS_MAP_BUFFER_FLAGS = 0,
    AS_MAP_BUFFER_KIND = 4,
    AS_MAP_BUFFER_HANDLE = 8,
    AS_MAP_BUFFER_PAGE_SIZE = 12,
    AS_MAP_BUFFER_BUFFER_OFFSET = 16,
    AS_MAP_BUFFER_MAPPING_SIZE = 24,
    AS_MAP_BUFFER_OFFSET = 32
};
/* IoctlGetVaRegions {u64 buf_addr (left alone), u32 buf_size (out: the regions' bytes), u32 reserved; VaRegion
   regions[2]} with VaRegion {u64 offset, u32 page_size, u32 pad, u64 pages} */
enum { AS_GET_VA_REGIONS_BUF_SIZE = 8, AS_GET_VA_REGIONS_REGIONS = 16 };
enum { AS_VA_REGION_OFFSET = 0, AS_VA_REGION_PAGE_SIZE = 8, AS_VA_REGION_PAGES = 16, AS_VA_REGION_BYTES = 24 };
#define AS_VA_REGIONS 2 /* the small-page region, then the big-page one */
/* IoctlAllocAsEx {u32 flags, s32 as_fd, u32 big_page_size, u32 reserved, u64 va_range_start, end, split} */
enum {
    AS_ALLOC_AS_EX_FLAGS = 0,
    AS_ALLOC_AS_EX_AS_FD = 4,
    AS_ALLOC_AS_EX_BIG_PAGE_SIZE = 8,
    AS_ALLOC_AS_EX_RESERVED = 12,
    AS_ALLOC_AS_EX_VA_START = 16,
    AS_ALLOC_AS_EX_VA_END = 24,
    AS_ALLOC_AS_EX_VA_SPLIT = 32
};
/* IoctlSyncpointShimInfo {u64 address, u32 stride, u32 count}: the GPU page whose u32 at id * stride reads as
   syncpoint id's value */
enum { AS_SYNCPOINT_SHIM_ADDRESS = 0, AS_SYNCPOINT_SHIM_STRIDE = 8, AS_SYNCPOINT_SHIM_COUNT = 12 };
/* IoctlRemapEntry {u16 flags, u16 kind, u32 handle, u32 handle_offset_big_pages, u32 as_offset_big_pages,
   u32 big_pages}, an array filling the argument */
enum {
    AS_REMAP_ENTRY_HANDLE = 4,
    AS_REMAP_ENTRY_HANDLE_OFFSET = 8, /* in big pages */
    AS_REMAP_ENTRY_AS_OFFSET = 12,    /* in big pages */
    AS_REMAP_ENTRY_BIG_PAGES = 16,
    AS_REMAP_ENTRY_BYTES = 20
};

/* Eden's VM: the address space's small page (YUZU_PAGESIZE), the big page sizes the GPU takes (64 KiB and 128 KiB,
   the characteristics' available_big_page_sizes 0x30000), the one ALLOC_AS_EX means by 0 (DEFAULT_BIG_PAGE_SIZE),
   and the default layout: the small-page region starts at the big page size << VA_START_SHIFT, the big-page one at
   DEFAULT_VA_SPLIT, and the space ends at DEFAULT_VA_RANGE */
#define AS_SMALL_PAGE_SIZE          0x1000u
#define AS_BIG_PAGE_BITS_64K        16u
#define AS_BIG_PAGE_BITS_128K       17u
#define AS_DEFAULT_BIG_PAGE_SIZE    (1u << AS_BIG_PAGE_BITS_128K)
#define AS_VA_START_SHIFT           10u
#define AS_DEFAULT_VA_SPLIT         (1ull << 34)
#define AS_DEFAULT_VA_RANGE         (1ull << 37)
#endif
