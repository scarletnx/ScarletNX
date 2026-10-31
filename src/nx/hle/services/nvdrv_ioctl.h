/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/nvdata.h):
   SPDX-FileCopyrightText: 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 Skyline Team and Contributors */
/* What every nvdrv device shares: how an ioctl number is built, and the NvResult values the devices answer. Only
   nvdrv.c and the device headers beside it (nvdrv_nvmap.h, nvdrv_as_gpu.h, nvdrv_ctrl.h, nvdrv_ctrl_gpu.h,
   nvdrv_gpu.h) include it */
#ifndef SNX_NVDRV_IOCTL_H
#define SNX_NVDRV_IOCTL_H
#include "common/cpu/aarch64/cpu.h"

/* An ioctl number is Linux's _IOC encoding, as Eden's Ioctl union reads it: the command in bits 0..7, the device's
   group ("magic") in bits 8..15, the argument's size in bytes in bits 16..29, and the direction in bits 30 (_IOC_WRITE:
   the caller passes the argument in, Eden's is_in) and 31 (_IOC_READ: the device writes it back, Eden's is_out) */
#define NV_IOCTL_CMD_SHIFT    0
#define NV_IOCTL_CMD_BITS     8
#define NV_IOCTL_GROUP_SHIFT  8
#define NV_IOCTL_GROUP_BITS   8
#define NV_IOCTL_LENGTH_SHIFT 16
#define NV_IOCTL_LENGTH_BITS  14
#define NV_IOCTL_DIR_SHIFT    30
#define NV_IOCTL_DIR_WRITE    1u /* _IOC_WRITE (bit 30): the caller passes the argument in */
#define NV_IOCTL_DIR_READ     2u /* _IOC_READ (bit 31): the device writes it back */
#define NV_IOCTL_DIR_RW       (NV_IOCTL_DIR_WRITE | NV_IOCTL_DIR_READ)

#define NV_IOCTL_FIELD(v, shift, bits) (((v) >> (shift)) & ((1u << (bits)) - 1))
/* the command within its device (Eden's cmd), the device's group (group), the argument size (length) and the
   direction bits (is_in, is_out) */
static inline u32 nv_ioctl_cmd(u32 ioctl) { return NV_IOCTL_FIELD(ioctl, NV_IOCTL_CMD_SHIFT, NV_IOCTL_CMD_BITS); }
static inline u32 nv_ioctl_group(u32 ioctl) { return NV_IOCTL_FIELD(ioctl, NV_IOCTL_GROUP_SHIFT, NV_IOCTL_GROUP_BITS); }
static inline u32 nv_ioctl_length(u32 ioctl) {
    return NV_IOCTL_FIELD(ioctl, NV_IOCTL_LENGTH_SHIFT, NV_IOCTL_LENGTH_BITS);
}
static inline u32 nv_ioctl_dir(u32 ioctl) { return ioctl >> NV_IOCTL_DIR_SHIFT; }
/* the ioctl without its size: one per command whatever argument size a caller gives */
static inline u32 nv_ioctl_without_length(u32 ioctl) {
    return ioctl & ~(((1u << NV_IOCTL_LENGTH_BITS) - 1) << NV_IOCTL_LENGTH_SHIFT);
}
/* _IOC(dir, group, cmd, size), Linux's builder */
#define NV_IOCTL(dir, group, cmd, size)                                                                                \
    (((u32)(dir) << NV_IOCTL_DIR_SHIFT) | ((u32)(size) << NV_IOCTL_LENGTH_SHIFT) |                                     \
     ((u32)(group) << NV_IOCTL_GROUP_SHIFT) | ((u32)(cmd) << NV_IOCTL_CMD_SHIFT))

/* NvResult, the NvError a device answers in the IPC reply's error word (Eden's nvdata.h names, Switchbrew's
   "NV error codes" numbers). nvdrv answers these */
enum {
    NV_RESULT_SUCCESS = 0x0,
    /* also what nvdrv's lookups of a handle or id that isn't there answer, as they did before these names. Eden's
       nvmap answers BadValue (0xB) there; which of the two nvservices gives was not checked against the SDK */
    NV_RESULT_NOT_IMPLEMENTED = 0x1,
    NV_RESULT_BAD_PARAMETER = 0x4,
    NV_RESULT_TIMEOUT = 0x5,
    NV_RESULT_INSUFFICIENT_MEMORY = 0x6,
    NV_RESULT_INVALID_STATE = 0x8,             /* an fd that isn't open (Eden's nvdrv.cpp answers it for a bad fd) */
    NV_RESULT_FILE_OPERATION_FAILED = 0x30003, /* Open with every fd taken */
    NV_RESULT_CONFIG_VAR_NOT_FOUND = 0x30006   /* GET_CONFIG: a production console has none of the settings */
};
#endif
