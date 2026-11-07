/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ZBIC, the compression of an Ounce game's NSO segments where their flags say compressed and the data does not start
   as LZ4 would (zbic.c): a zstd frame with the magic "ZBIC" whose FSE tables' headers are written by binary
   interpolative coding. The NSO loaders (nx/loader/loader.c, nx/runtime/loader.c, hactool's nso.c) take it alongside
   LZ4 */
#ifndef SNX_ZBIC_H
#define SNX_ZBIC_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* whether the size bytes at src start a ZBIC frame */
int snx_zbic_is(const void* src, size_t size);
/* the frame at src decompressed into dst: the bytes written, or -1 for a frame that is corrupt or does not fit */
long snx_zbic_decompress(void* dst, size_t dst_size, const void* src, size_t src_size);

#ifdef __cplusplus
}
#endif
#endif
