/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the GPU engines' files (engines.c, mme.c, compute.c, fermi2d.c, dma.c, maxwell3d.c) share: the
   front end's headers, a GPU address from its register pair, and the entry points one engine's file gives another */
#ifndef SNX_ENGINES_INT_H
#define SNX_ENGINES_INT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* Maxwell's GPU virtual addresses are 40 bits: a register pair holds bits 32-39 in its upper word's low byte and
   bits 0-31 in its lower word (Eden's PackedGPUVAddr) */
#define GPU_VA_UPPER_MASK 0xFFu
static inline u64 gpu_packed_va(u32 upper, u32 lower) { return ((u64)(upper & GPU_VA_UPPER_MASK) << 32) | lower; }

/* ampere.c: update a native stream's Maxwell limit after a size or address write.
   Keep the method filter inline: unrelated writes stay on the plain-register fast path. */
void ampere_stream_limit(GpuChannel* ch, u32 stream);
static inline void ampere_stream_written(GpuChannel* ch, u32 m) {
    u32 stream;
    if (m >= 0x180 && m < 0x1C0)
        stream = (m - 0x180) / 2;
    else if (m >= 0x700 && m < 0x780 && ((m & 3) == 1 || (m & 3) == 2))
        stream = (m - 0x700) / 4;
    else
        return;
    if (gpu_native_graphics(ch)) ampere_stream_limit(ch, stream);
}

/* engines.c: a report semaphore operation (Eden's Maxwell3D::ProcessQueryGet; the query word v written to base + 3)
   of the 3D or compute engine, whose registers r hold the address (upper, lower) at base and base + 1 and the payload
   at base + 2 */
void query_get(GpuChannel* ch, u32* r, int base, u32 v);

/* mme.c: instructions one macro call may run, past which it never ends */
#define MME_STEPS 5000000u
/* mme.c: runs macro idx over the np parameters in ch->mme_params, interpreted (it may stop at a wait, gpu.h's
   mme_resume) */
void mme_run(GpuChannel* ch, u32 idx, u32 np);
/* the macro upload methods: 0x45 (where the next code word goes), 0x46 (a code word) and 0x48 (a macro's start, at
   the slot 0x47 set) */
void mme_upload_ptr(GpuChannel* ch, u32 v);
void mme_upload(GpuChannel* ch, u32 v);
void mme_bind(GpuChannel* ch, u32 v);
/* what a macro does: sends method m (to the 3D engine, or a macro method's register only, as from the pushbuffer a
   macro method would start a macro inside this one), or reads register a */
void mme_out(GpuChannel* ch, u32 m, u32 v);
u32 mme_reg(GpuChannel* ch, u32 a);

#endif
