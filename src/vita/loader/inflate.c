/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/inflate.h"
#include <libdeflate.h>

/* libdeflate refuses a stream that ends short of out_size or needs more room, as both callers want */
bool vita_inflate(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size) {
    struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    bool ok = d && libdeflate_deflate_decompress(d, in, in_size, out, out_size, NULL) == LIBDEFLATE_SUCCESS;
    libdeflate_free_decompressor(d);
    return ok;
}
bool vita_zlib_inflate(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size) {
    struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    bool ok = d && libdeflate_zlib_decompress(d, in, in_size, out, out_size, NULL) == LIBDEFLATE_SUCCESS;
    libdeflate_free_decompressor(d);
    return ok;
}
