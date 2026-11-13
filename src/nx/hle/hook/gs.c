/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Ounce SDK's nn::gs::sdk image reads (hooks.hook), on guest memory: a flat mapping at c->mem, as libc.c's.

   ReadUserImageNv12 copies a rectangle of a video image, which the runtime's video service describes
   (native_video_core.c's nvideo_cpu_descriptor, type 0xa: NV12 rows a pitch apart), into the caller's luma and chroma
   buffers. One game's movie player reads every decoded picture with it, 3 MB of NV12 at 1080p: in the guest's own code,
   recompiled at -O0, a data cache invalidate of each plane by its cache lines, then a memcpy a row.

   What it does, read in the SDK of an Ounce game (21.x): its arguments are the luma buffer, its size and pitch, the
   chroma buffer, its size and pitch, the image's descriptor and the image's address in registers, then the rectangle's
   x, y, width and height, a 16-bit number each in its own 8 bytes of the stack. For a descriptor of type 0xa (the
   first byte), with the luma plane at the 32-bit offset at +8 and its pitch at +0x10, and the chroma plane at +0x18
   and its pitch at +0x20:
   - each of the rectangle's rows of luma: width bytes from x on, to the luma buffer a pitch a row;
   - when the rectangle has two rows or more, each of its height / 2 rows of chroma from row y / 2 on: width & ~1 bytes
     from x & ~1 on, to the chroma buffer a pitch a row;
   and the result is 0. The sizes aren't read. A type of 0xb to 0x15 takes other code (block linear images), which the
   runtime's descriptors never are; any other type gives result 0x100f3, as the SDK's own check does */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "common/utilities/endian.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

#define GS_NV12_PITCH_LINEAR 0xa
#define GS_BAD_DESCRIPTOR    0x100f3u

u32 gs_read_user_image_nv12(CPU* c, u64 dst_y, u64 size_y, u64 pitch_y, u64 dst_uv, u64 size_uv, u64 pitch_uv,
                            u64 desc, u64 src) {
    (void)size_y;
    (void)size_uv;
    const u8* d = hook_host(c, desc);
    const u8* args = hook_host(c, c->x[31]); /* the four after the eight in registers */
    u32 x = snx_ld_le16(args), y = snx_ld_le16(args + 8), w = snx_ld_le16(args + 16), h = snx_ld_le16(args + 24);
    if (d[0] != GS_NV12_PITCH_LINEAR) {
        if (d[0] >= 0xa && d[0] <= 0x15)
            LOG_LIMIT(4, "nn::gs: ReadUserImageNv12 of an image of descriptor type %#x, which the video service doesn't "
                         "make: nothing read\n",
                      d[0]);
        return GS_BAD_DESCRIPTOR;
    }
    u64 y_at = src + snx_ld_le32(d + 8), y_pitch = snx_ld_le64(d + 0x10);
    u64 c_at = src + snx_ld_le32(d + 0x18), c_pitch = snx_ld_le64(d + 0x20);
    for (u32 r = 0; r < h; r++)
        memcpy(hook_host(c, dst_y + r * pitch_y), hook_host(c, y_at + x + (u64)((y + r) & 0xffff) * y_pitch), w);
    if (h >= 2)
        for (u32 r = 0; r < h >> 1; r++)
            memcpy(hook_host(c, dst_uv + r * pitch_uv),
                   hook_host(c, c_at + (x & 0xfffe) + (u64)(((y >> 1) + r) & 0xffff) * c_pitch), w & 0xfffe);
    return 0;
}
