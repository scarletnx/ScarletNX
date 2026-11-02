/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/maxwell/engines_int.h"

/* C797 SET_VERTEX_STREAM_SIZE_A/B are byte offsets 0x600/0x604, with 8 bytes per stream.
   Keep the Maxwell inclusive limit current for either order of size and address writes. */
void ampere_stream_limit(GpuChannel* ch, u32 stream) {
    const u32* r = ch->r3d;
    u64 start = gpu_packed_va(r[0x701 + stream * 4], r[0x702 + stream * 4]);
    u64 size = gpu_packed_va(r[0x180 + stream * 2], r[0x181 + stream * 2]);
    u64 end = start + size - 1;
    maxwell3d_method(ch, 0x7C0 + stream * 2, (u32)(end >> 32), 0);
    maxwell3d_method(ch, 0x7C1 + stream * 2, (u32)end, 1);
}
