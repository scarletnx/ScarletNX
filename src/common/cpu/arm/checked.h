/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ARM_CHECKED_H
#define SNX_ARM_CHECKED_H
#include "common/cpu/arm/arm.h"

/* Separate opt-in ABI; SnxArmCPU and SNX_ARM_ABI are unchanged. Generated
   checked blocks take &state.cpu, NEVER a standalone SnxArmCPU. No flat memory
   pointer is read. The runtime serializes CPU, mappings, devices and callbacks. */
/* 2: the words the native back ends' blocks move a transfer through */
#define SNX_ARM_CHECKED_ABI      2u
#define SNX_ARM_ACCESS_READ      1u
#define SNX_ARM_ACCESS_WRITE     2u
#define SNX_ARM_ACCESS_FETCH     3u
/* One whole-instruction RAM transaction, not one combined MMIO access.
   Backends must reject MULTI on devices before causing any side effect. */
#define SNX_ARM_ACCESS_MULTI     0x100u
/* Optional RAM reservation transactions (ARM11 emission only). READ |
   EXCLUSIVE loads bytes and installs a serialized bus-owned reservation. WRITE
   | EXCLUSIVE consumes bytes and atomically tests/consumes that reservation.
   Return 0 on store success, EXCLUSIVE_FAILED on reservation loss, otherwise
   a fault. Legacy callbacks reject unknown operations and fail closed. Size is
   1/2/4/8, naturally aligned. Neither operation may reach MMIO. Context-owned
   monitor metadata may change, but the callback must not modify CPU state. */
#define SNX_ARM_ACCESS_EXCLUSIVE 0x200u
#define SNX_ARM_EXCLUSIVE_FAILED 2u

/* Except for optional WRITE|EXCLUSIVE above, zero means success and nonzero
   means fault. Byte order is guest little-endian.
   READ/FETCH fill bytes; WRITE consumes bytes without changing them. A failed
   RAM transaction must not change memory; a failed READ must not expose partial
   results. Callbacks must not modify CPU state, recurse or retain bytes. Device
   callbacks own their fault/side-effect policy. Size is 1..128 for this ABI: a
   VLDM or VSTM of all sixteen double registers is 128 bytes.
   FETCH checks execute permission, independently of read permission. */
typedef uint32_t (*SnxArmAccess)(void* context, uint32_t operation, uint32_t address, uint32_t size, void* bytes);
typedef struct {
    SnxArmCPU cpu;
    uint32_t abi;
    void* context;
    SnxArmAccess access;
    uint32_t words[32]; /* a transfer's bytes, as the runtime library's calls keep them for a native block (arm.h's
                           snx_arm_checked_access): nothing lives in them from one instruction to the next */
} SnxArmCheckedCPU;
_Static_assert(offsetof(SnxArmCheckedCPU, cpu) == 0, "checked CPU prefix");
#endif
