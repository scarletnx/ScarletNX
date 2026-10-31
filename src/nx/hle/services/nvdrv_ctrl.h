/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvhost_ctrl.h, nvhost_ctrl.cpp, nvdata.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 Skyline Team and Contributors */
/* /dev/nvhost-ctrl's ioctls (group 0): the syncpoint commands (Switchbrew's numbers, Eden's Ioc*Params layouts,
   which Eden declares but doesn't serve) and the event commands Eden's nvhost_ctrl::Ioctl1 serves, the byte offsets
   of their arguments, and the event value an asynchronous wait answers (Eden's SyncpointEventValue). nvdrv.c's
   ioctl_ctrl and QueryEvent use them */
#ifndef SNX_NVDRV_CTRL_H
#define SNX_NVDRV_CTRL_H
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/nvdrv_ioctl.h"

#define NVHOST_CTRL_IOCTL_GROUP 0x00

/* commands (an ioctl's low byte) */
enum {
    NVHOST_CTRL_SYNCPT_READ = 0x14,       /* IocSyncptRead */
    NVHOST_CTRL_SYNCPT_INCR = 0x15,       /* IocSyncptIncr */
    NVHOST_CTRL_SYNCPT_WAIT = 0x16,       /* IocSyncptWait */
    NVHOST_CTRL_MODULE_MUTEX = 0x17,      /* IocModuleMutex: names a module, not a syncpoint */
    NVHOST_CTRL_MODULE_REGRDWR = 0x18,    /* IocModuleRegRDWR: likewise */
    NVHOST_CTRL_SYNCPT_WAITEX = 0x19,     /* IocSyncptWaitex */
    NVHOST_CTRL_SYNCPT_READ_MAX = 0x1A,   /* IocSyncptReadMax */
    NVHOST_CTRL_GET_CONFIG = 0x1B,        /* NvOsGetConfigU32 */
    NVHOST_CTRL_EVENT_CLEAR_WAIT = 0x1C,  /* IocCtrlClearEventWait */
    NVHOST_CTRL_EVENT_WAIT = 0x1D,        /* IocCtrlEventWait, synchronous here (Switchbrew's EVENT_WAIT) */
    NVHOST_CTRL_EVENT_WAIT_ASYNC = 0x1E,  /* IocCtrlEventWait, arming an event slot (EVENT_WAIT_ASYNC) */
    NVHOST_CTRL_EVENT_REGISTER = 0x1F,    /* IocCtrlEventRegister */
    NVHOST_CTRL_EVENT_UNREGISTER = 0x20,  /* IocCtrlEventUnregister */
    NVHOST_CTRL_EVENT_KILL = 0x21         /* IocCtrlEventUnregisterBatch */
};

/* IocSyncptReadParams, IocSyncptIncrParams, IocSyncptReadMaxParams {u32 id; out u32 value}; IocSyncptWaitParams and
   IocSyncptWaitexParams {u32 id, u32 thresh, s32 timeout (ms, negative for good); out u32 value (WAITEX)}; and
   IocCtrlEventWaitParams, the same with the value naming the event slot */
enum { CTRL_SYNCPT_ID = 0, CTRL_SYNCPT_VALUE = 4, CTRL_SYNCPT_THRESH = 4, CTRL_SYNCPT_TIMEOUT = 8 };
enum { CTRL_SYNCPT_WAITEX_VALUE = 12, CTRL_SYNCPT_WAITEX_BYTES = 16 };
enum { CTRL_EVENT_WAIT_VALUE = 12 };
/* IocCtrlEventClearParams, IocCtrlEventRegisterParams, IocCtrlEventUnregisterParams {u32 slot};
   IocCtrlEventUnregisterBatchParams {u64 mask, a bit per slot} */
enum { CTRL_EVENT_SLOT = 0, CTRL_EVENT_KILL_MASK = 0 };
/* IocGetConfigParams {char domain_str[0x41], param_str[0x41], config_str[0x101]} */
enum { CTRL_GET_CONFIG_DOMAIN_STR = 0, CTRL_GET_CONFIG_PARAM_STR = 0x41, CTRL_GET_CONFIG_NAME_BYTES = 0x41 };

/* a ctrl fd's event slot a value names, of its NV_CTRL_EVENT_SLOTS (nvdrv.h) */
static inline u32 nv_ctrl_event_slot(u32 v) { return v & (NV_CTRL_EVENT_SLOTS - 1); }

/* SyncpointEventValue in its allocation form, what EVENT_WAIT_ASYNC answers and QueryEvent takes back: the slot in
   bits 0..15, the syncpoint's id (its low 12 bits) in bits 16..27, and event_allocated in bit 28 */
#define NV_EVENT_VALUE_SYNCPT_SHIFT 16
#define NV_EVENT_VALUE_SYNCPT_BITS  12
#define NV_EVENT_VALUE_KIND_SHIFT   28 /* bits 28..31: 1 for an allocated value (event_allocated, and nothing above) */
#define NV_EVENT_VALUE_KIND_ALLOC   1u
#define NV_EVENT_VALUE_ALLOCATED    (NV_EVENT_VALUE_KIND_ALLOC << NV_EVENT_VALUE_KIND_SHIFT)
static inline u32 nv_event_value(u32 syncpt, u32 slot) {
    return NV_EVENT_VALUE_ALLOCATED |
           ((syncpt & ((1u << NV_EVENT_VALUE_SYNCPT_BITS) - 1)) << NV_EVENT_VALUE_SYNCPT_SHIFT) | slot;
}
/* whether QueryEvent's id names a ctrl fd's event slot: a plain slot number, or an allocated event value */
static inline int nv_event_value_names_slot(u32 v) {
    return v < NV_CTRL_EVENT_SLOTS || v >> NV_EVENT_VALUE_KIND_SHIFT == NV_EVENT_VALUE_KIND_ALLOC;
}
#endif
