/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Port of Azahar service/ldr_ro/ldr_ro.h at a4b335e.
   Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project
   Licensed under GPLv2 or any later version. */
#ifndef SNX_CTR_LDR_H
#define SNX_CTR_LDR_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct SnxCtrLdrModule {
    uint32_t address, buffer, size, data, data_size, bss, bss_size;
} SnxCtrLdrModule;
/* ldr:ro: the loader of relocatable CRO modules, as Azahar's ldr_ro.cpp serves it. A retail game loads its own CROs at
   run time. Initialize takes the static module's CRS buffer and address, and LoadCRO aliases a private buffer into the
   process image, rebases it, links its imports against the CRS and the other registered CROs, and fixes it. */
void snx_ctr_ldr_dispatch(SnxCtrIpc* c);
/* Source, image and live data ranges stay out of FREE and PROTECT until unload. */
bool snx_ctr_ldr_held(struct SnxCtrProcess* p, uint32_t address, uint32_t size);
#endif
