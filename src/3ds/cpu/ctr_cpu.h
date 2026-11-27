/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_CPU_H
#define SNX_CTR_CPU_H
#include "common/cpu/arm/checked.h"
#include "3ds/memory/ctr_memory.h"

/* Opt-in checked A32 state, no NX/A64 CPU casts and no JIT dependency.
   This adapter does NOT discover/load native blocks or implement an OS. */
typedef struct {
    SnxArmCheckedCPU state;
    SnxCtrAddressSpace* space;
    uint64_t reservation_epoch;
    uint32_t reservation_address, reservation_size;
} SnxCtrCpu;
/* Fresh USR state. Caller owns process mappings and executable entry validation.
   Space must outlive the CPU; all accesses/mapping changes are serialized.
   Do not copy/move initialized CPUs: the checked context refers to its owner. */
bool snx_ctr_cpu_init(SnxCtrCpu* cpu, SnxCtrAddressSpace* space, uint32_t pc, uint32_t sp, bool thumb);
#endif
