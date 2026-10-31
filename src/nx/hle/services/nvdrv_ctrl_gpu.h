/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.h, nvhost_ctrl_gpu.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* /dev/nvhost-ctrl-gpu's ioctls (group 'G'), the byte offsets of their arguments' fields, and what a Tegra X1's GM20B
   reports through them: the characteristics and ZCULL info as Eden's IoctlGpuCharacteristics and
   IoctlNvgpuGpuZcullGetInfoArgs, with Eden's values. nvdrv.c's ioctl_ctrlgpu uses them */
#ifndef SNX_NVDRV_CTRL_GPU_H
#define SNX_NVDRV_CTRL_GPU_H
#include "nx/hle/services/nvdrv_ioctl.h"

#define NVHOST_CTRL_GPU_IOCTL_GROUP 'G'

/* commands (an ioctl's low byte). 0x13 and 0x14 are where this device has answered NUM_VSMS and VSMS_MAPPING
   (0xC0044714, with ZBC_GET_ACTIVE_SLOT_MASK at 0x14 read-only, as Eden serves it). Eden's table of unimplemented
   names puts NUM_VSMS and VSMS_MAPPING at 0x12 and 0x13 instead, as Linux nvgpu numbers them; which the SDK uses was
   not checked against its code */
enum {
    NVGPU_GPU_ZCULL_GET_CTX_SIZE = 0x01,         /* ZCullGetCtxSize */
    NVGPU_GPU_ZCULL_GET_INFO = 0x02,             /* ZCullGetInfo */
    NVGPU_GPU_ZBC_SET_TABLE = 0x03,              /* ZBCSetTable */
    NVGPU_GPU_ZBC_QUERY_TABLE = 0x04,            /* ZBCQueryTable */
    NVGPU_GPU_GET_CHARACTERISTICS = 0x05,        /* GetCharacteristics1/3 */
    NVGPU_GPU_GET_TPC_MASKS = 0x06,              /* GetTPCMasks1/3 */
    NVGPU_GPU_NUM_VSMS = 0x13,                   /* Ioctl3, Eden's stub */
    NVGPU_GPU_VSMS_MAPPING = 0x14,               /* with both direction bits set: 0xC0044714 */
    NVGPU_GPU_ZBC_GET_ACTIVE_SLOT_MASK = 0x14,   /* read only: GetActiveSlotMask */
    NVGPU_GPU_PMU_GET_GPU_LOAD = 0x15,           /* PmuGetGpuLoad */
    NVGPU_GPU_GET_GPU_TIME = 0x1C                /* GetGpuTime */
};

/* IoctlZcullGetCtxSize {out u32 size}: Eden's size 1 */
enum { CTRL_GPU_ZCULL_CTX_SIZE = 0 };
#define NVGPU_ZCULL_CTX_SIZE 1u
/* IoctlCharacteristics {u64 gpu_characteristics_buf_size (out: the bytes written), u64 gpu_characteristics_buf_addr
   (left alone); IoctlGpuCharacteristics gc} */
enum { CTRL_GPU_CHARACTERISTICS_BUF_SIZE = 0, CTRL_GPU_CHARACTERISTICS_GC = 16 };
/* IoctlGpuGetTpcMasksArgs {u32 mask_buffer_size; pad; u64 mask_buffer_address; u32 tcp_mask; pad} */
enum { CTRL_GPU_TPC_MASKS_MASK = 16 };
/* NUM_VSMS {out u32 num_vsms, u32 reserved} */
enum { CTRL_GPU_NUM_VSMS = 0, CTRL_GPU_NUM_VSMS_RESERVED = 4 };
/* IoctlActiveSlotMask {out u32 slot, u32 mask} */
enum { CTRL_GPU_ACTIVE_SLOT = 0, CTRL_GPU_ACTIVE_SLOT_MASK = 4 };
/* IoctlPmuGetLoad {out u32 pmu_gpu_load}, IoctlGetGpuTime {out u64 gpu_time} */
enum { CTRL_GPU_PMU_GPU_LOAD = 0, CTRL_GPU_GPU_TIME = 0 };

/* the GM20B's shape: one GPC of two TPCs, an SM each (the characteristics' num_gpc and num_tpc_per_gpc). TPC mask 3 has
   the GPC's two TPCs (Eden's tcp_mask), and VSMS_MAPPING's {gpc, tpc} byte pairs place SM 0 at TPC 0 and SM 1 at
   TPC 1 */
#define NVGPU_NUM_GPCS  1u
#define NVGPU_NUM_SMS   2u
#define NVGPU_TPC_MASK  0x3u
#define NVGPU_SM_WARPS  0x80u /* the characteristics' sm_arch_warp_count */
/* Eden's GetActiveSlotMask and PmuGetGpuLoad answers: slot 7 with mask 1, and a load of 100 */
#define NVGPU_ACTIVE_SLOT      0x07u
#define NVGPU_ACTIVE_SLOT_MASK 0x01u
#define NVGPU_PMU_GPU_LOAD     100u

/* IoctlGpuCharacteristics (0xA0 bytes), as the guest lays it out: no member needs padding before it, so the host's
   layout is the guest's and offsetof gives each field's place. Values: nvdrv.c's gpu_characteristics */
typedef struct {
    u32 arch, impl, rev, num_gpc;
    u64 l2_cache_size, on_board_video_memory_size;
    u32 num_tpc_per_gpc, bus_type, big_page_size, compression_page_size;
    u32 pde_coverage_bit_count, available_big_page_sizes, gpc_mask, sm_arch_sm_version;
    u32 sm_arch_spa_version, sm_arch_warp_count, gpu_va_bit_count, reserved;
    u64 flags;
    u32 twod_class, threed_class, compute_class, gpfifo_class, inline_to_memory_class, dma_copy_class;
    u32 max_fbps_count, fbp_en_mask, max_ltc_per_fbp, max_lts_per_ltc;
    u32 max_tex_per_tpc, max_gpc_count, rop_l2_en_mask_0, rop_l2_en_mask_1;
    u64 chipname, gr_compbit_store_base_hw;
} NvgpuGpuCharacteristics;
#define NVGPU_CHARACTERISTICS_BYTES 0xA0u

/* IoctlNvgpuGpuZcullGetInfoArgs (40 bytes, ten u32s in this order). Values: nvdrv.c's ctrlgpu_zcull_info */
typedef struct {
    u32 width_align_pixels, height_align_pixels;
    u32 pixel_squares_by_aliquots, aliquot_total;
    u32 region_byte_multiplier, region_header_size, subregion_header_size;
    u32 subregion_width_align_pixels, subregion_height_align_pixels, subregion_count;
} NvgpuGpuZcullGetInfoArgs;
#endif
