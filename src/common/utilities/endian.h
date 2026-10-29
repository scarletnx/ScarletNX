/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ENDIAN_H
#define SNX_ENDIAN_H

/* Guest memory is little-endian on every guest the runtimes run (AArch64 Linux, the Switch, the 3DS), whatever the
   host's byte order. These read and write a guest value at a byte address, and convert one held in a register. On a
   little-endian host each is a memcpy or nothing, so the code compiled from them is what a plain memcpy gave. Some
   wire formats are big-endian (the network room, DNS, the shared fonts' header), hence the _be forms. */

#include "common/utilities/bits.h"
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SNX_HOST_BE 1
#else
#define SNX_HOST_BE 0
#endif

#if SNX_HOST_BE
#define SNX_LE16(v) snx_bswap16(v)
#define SNX_LE32(v) snx_bswap32(v)
#define SNX_LE64(v) snx_bswap64(v)
#define SNX_BE16(v) ((uint16_t)(v))
#define SNX_BE32(v) ((uint32_t)(v))
#define SNX_BE64(v) ((uint64_t)(v))
#else
#define SNX_LE16(v) ((uint16_t)(v))
#define SNX_LE32(v) ((uint32_t)(v))
#define SNX_LE64(v) ((uint64_t)(v))
#define SNX_BE16(v) snx_bswap16(v)
#define SNX_BE32(v) snx_bswap32(v)
#define SNX_BE64(v) snx_bswap64(v)
#endif

/* a value between the host's order and little-endian (the same conversion both ways) */
static inline uint16_t snx_le16(uint16_t v) { return SNX_LE16(v); }
static inline uint32_t snx_le32(uint32_t v) { return SNX_LE32(v); }
static inline uint64_t snx_le64(uint64_t v) { return SNX_LE64(v); }

static inline uint16_t snx_ld_le16(const void* p) {
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return SNX_LE16(v);
}
static inline uint32_t snx_ld_le32(const void* p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return SNX_LE32(v);
}
static inline uint64_t snx_ld_le64(const void* p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return SNX_LE64(v);
}
static inline void snx_st_le16(void* p, uint16_t v) {
    v = SNX_LE16(v);
    memcpy(p, &v, sizeof v);
}
static inline void snx_st_le32(void* p, uint32_t v) {
    v = SNX_LE32(v);
    memcpy(p, &v, sizeof v);
}
static inline void snx_st_le64(void* p, uint64_t v) {
    v = SNX_LE64(v);
    memcpy(p, &v, sizeof v);
}

static inline uint16_t snx_ld_be16(const void* p) {
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return SNX_BE16(v);
}
static inline uint32_t snx_ld_be32(const void* p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return SNX_BE32(v);
}
static inline void snx_st_be16(void* p, uint16_t v) {
    v = SNX_BE16(v);
    memcpy(p, &v, sizeof v);
}
static inline void snx_st_be32(void* p, uint32_t v) {
    v = SNX_BE32(v);
    memcpy(p, &v, sizeof v);
}
static inline uint64_t snx_ld_be64(const void* p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return SNX_BE64(v);
}
static inline void snx_st_be64(void* p, uint64_t v) {
    v = SNX_BE64(v);
    memcpy(p, &v, sizeof v);
}

/* n words at p converted in place between little-endian and the host's order (the same conversion both ways): a
   structure of guest words copied out of guest memory before its fields are read, or before it is copied back. They
   do nothing on a little-endian host */
static inline void snx_le16_array(void* p, size_t n) {
    unsigned char* b = (unsigned char*)p;
    for (; SNX_HOST_BE && n; --n, b += 2) {
        uint16_t v = snx_ld_le16(b);
        memcpy(b, &v, sizeof v);
    }
}
static inline void snx_le32_array(void* p, size_t n) {
    unsigned char* b = (unsigned char*)p;
    for (; SNX_HOST_BE && n; --n, b += 4) {
        uint32_t v = snx_ld_le32(b);
        memcpy(b, &v, sizeof v);
    }
}
static inline void snx_le64_array(void* p, size_t n) {
    unsigned char* b = (unsigned char*)p;
    for (; SNX_HOST_BE && n; --n, b += 8) {
        uint64_t v = snx_ld_le64(b);
        memcpy(b, &v, sizeof v);
    }
}

#endif
