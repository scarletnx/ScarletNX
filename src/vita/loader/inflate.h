/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_INFLATE_H
#define SNX_VITA_INFLATE_H
#include <stdbool.h>
#include <stddef.h>

/* Raw DEFLATE (RFC 1951), as a VPK's members hold it, and zlib (RFC 1950, its header and Adler-32 checked), as a
   compressed SELF's segments do: exactly out_size bytes from in. False for a corrupt stream or another size */
bool vita_inflate(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size);
bool vita_zlib_inflate(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size);
#endif
