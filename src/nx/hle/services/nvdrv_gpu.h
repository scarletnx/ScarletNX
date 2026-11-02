/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvhost_gpu.h, nvhost_gpu.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* /dev/nvhost-gpu's ioctls, a GPU channel's (group 'H', and 'G' for the user data), the byte offsets of their
   arguments' fields named after Eden's Ioctl* structs, and SUBMIT_GPFIFO's flags (Eden's IoctlSubmitGpfifo
   bitfield). nvdrv.c's ioctl_gpu uses them */
#ifndef SNX_NVDRV_GPU_H
#define SNX_NVDRV_GPU_H
#include "nx/hle/services/nvdrv_ioctl.h"

#define NVHOST_GPU_IOCTL_GROUP           'H'
#define NVHOST_GPU_USER_DATA_IOCTL_GROUP 'G'

/* commands (an ioctl's low byte), Eden's handler names */
enum {
    NVHOST_GPU_SET_NVMAP_FD = 0x01,          /* SetNVMAPfd */
    NVHOST_GPU_SET_TIMEOUT = 0x03,           /* ChannelSetTimeout */
    NVHOST_GPU_SUBMIT_GPFIFO = 0x08,         /* SubmitGPFIFOBase1, the entries after the header */
    NVHOST_GPU_ALLOC_OBJ_CTX = 0x09,         /* AllocateObjectContext */
    NVHOST_GPU_FREE_OBJ_CTX = 0x0A,          /* Linux nvgpu's number; Eden doesn't serve it */
    NVHOST_GPU_ZCULL_BIND = 0x0B,            /* ZCullBind */
    NVHOST_GPU_SET_ERROR_NOTIFIER = 0x0C,    /* SetErrorNotifier */
    NVHOST_GPU_SET_PRIORITY = 0x0D,          /* SetChannelPriority */
    NVHOST_GPU_SET_USER_DATA = 0x14,         /* SetClientData, group 'G' */
    NVHOST_GPU_GET_USER_DATA = 0x15,         /* GetClientData, group 'G' */
    NVHOST_GPU_GET_ERROR_NOTIFICATION = 0x16, /* IoctlGetErrorNotification's layout; Eden doesn't serve it */
    NVHOST_GPU_UNNAMED_17 = 0x17,            /* answered 0 as SET_ERROR_NOTIFIER since the import; Eden has none */
    NVHOST_GPU_ALLOC_GPFIFO_EX = 0x18,       /* AllocGPFIFOEx: not served here */
    NVHOST_GPU_ALLOC_GPFIFO_EX2 = 0x1A,      /* AllocGPFIFOEx2 */
    NVHOST_GPU_SUBMIT_GPFIFO_EX = 0x1B,      /* SubmitGPFIFOBase1/2: Ioctl2's form, the entries in its second input */
    NVHOST_GPU_SET_TIMESLICE = 0x1D,        /* ChannelSetTimeslice */
    NVHOST_GPU_SET_PREEMPTION_MODES = 0x1F  /* Switch 2: SetPreemptionModes */
};

/* IoctlSubmitGpfifo {u64 address, u32 num_entries, u32 flags; NvFence fence (in: the wait, out: the fence)}, then
   the entries for SUBMIT_GPFIFO, 8 bytes each */
enum {
    GPU_SUBMIT_NUM_ENTRIES = 8,
    GPU_SUBMIT_FLAGS = 12,
    GPU_SUBMIT_FENCE_ID = 16,
    GPU_SUBMIT_FENCE_VALUE = 20,
    GPU_SUBMIT_HEADER_BYTES = 24
};
#define GPFIFO_ENTRY_BYTES 8u
/* its flags */
enum {
    GPU_SUBMIT_FENCE_WAIT = 1u << 0,      /* fence_wait: wait for the fence before the entries */
    GPU_SUBMIT_FENCE_INCREMENT = 1u << 1, /* fence_increment: the host increments the syncpoint after them */
    GPU_SUBMIT_NEW_HW_FORMAT = 1u << 2,   /* new_hw_format: ignored */
    GPU_SUBMIT_SUPPRESS_WFI = 1u << 4,    /* suppress_wfi: ignored */
    GPU_SUBMIT_INCREMENT_VALUE = 1u << 8  /* increment_value: fence.value increments, stream-owned on Maxwell */
};
/* the increments fence_increment adds after the entries: two syncpoint increments, as Eden's
   BuildIncrementCommandList pushes them (and Eden's SubmitGPFIFOImpl promises them) */
#define GPU_SUBMIT_HOST_INCREMENTS 2u
/* NvFence's id when the fence names no syncpoint */
#define NV_FENCE_NO_SYNCPT 0xFFFFFFFFu

/* IoctlAllocObjCtx {u32 class_num, u32 flags; out u64 obj_id} */
enum { GPU_ALLOC_OBJ_CTX_CLASS_NUM = 0, GPU_ALLOC_OBJ_CTX_OBJ_ID = 8 };
/* IoctlClientData {u64 data} */
enum { GPU_USER_DATA = 0 };
/* IoctlAllocGpfifoEx {u32 num_entries, num_jobs, flags; out NvFence fence; u32 reserved[3]} */
enum { GPU_ALLOC_GPFIFO_FENCE_ID = 12, GPU_ALLOC_GPFIFO_FENCE_VALUE = 16 };
/* SetPreemptionModes {u32 graphics, compute}: 0 retains the mode, graphics 1 is WFI,
   compute 1 is WFI and 2 is CTA. Commands finish before the runtime switches channels. */
enum { GPU_PREEMPTION_GRAPHICS = 0, GPU_PREEMPTION_COMPUTE = 4 };
#endif
