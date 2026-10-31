/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/inflate.h"
#include <miniz.h>

/* miniz's inflate, the library Share Log's zip is written with: a stream that ends short of out_size, or needs more
   room, is refused */
bool vita_inflate(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size) {
    return tinfl_decompress_mem_to_mem(out, out_size, in, in_size, 0) == out_size;
}
bool vita_zlib_inflate(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size) {
    return tinfl_decompress_mem_to_mem(out, out_size, in, in_size, TINFL_FLAG_PARSE_ZLIB_HEADER) == out_size;
}
