/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libz, the NDK's zlib, as the build's own zlib (cmake/dependencies.cmake): its functions are the host's, called with
   the guest's arguments (hle_pass). On an LP64 host z_stream and gz_header have the guest's layout (uLong is 64 bits
   on both), so a guest's stream is the host's. A stream's allocator is the guest's code when the guest gives one:
   the init functions put host functions in its place, which call the guest's through the bridge, and the end
   functions put the guest's back. Windows' uLong is 32 bits, so the layouts differ there: no zlib yet */
#include "android/bridge/bridge.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <zlib.h>

/* a stream's allocator, the guest's, while the stream holds the host's in its place */
typedef struct {
    uint64_t zalloc, zfree, opaque;
} GuestAllocator;

static voidpf host_alloc(voidpf opaque, uInt items, uInt size) {
    const GuestAllocator* a = opaque;
    uint64_t args[3] = {a->opaque, items, size};
    return (voidpf)(uintptr_t)bridge_call(a->zalloc, args, 3);
}
static void host_free(voidpf opaque, voidpf address) {
    const GuestAllocator* a = opaque;
    uint64_t args[2] = {a->opaque, (uint64_t)(uintptr_t)address};
    bridge_call(a->zfree, args, 2);
}

/* the guest's allocator of a stream set aside for the init's call: false when the guest gave none */
static bool wrap(z_streamp s) {
    if (!s || (!s->zalloc && !s->zfree)) return false;
    GuestAllocator* a = malloc(sizeof *a);
    if (!a) return false;
    a->zalloc = (uint64_t)(uintptr_t)s->zalloc;
    a->zfree = (uint64_t)(uintptr_t)s->zfree;
    a->opaque = (uint64_t)(uintptr_t)s->opaque;
    /* zlib's defaults take a missing one: only the one the guest gave is the guest's */
    s->zalloc = a->zalloc ? host_alloc : NULL;
    s->zfree = a->zfree ? host_free : NULL;
    s->opaque = a;
    return true;
}
/* the stream's allocator the guest's again (an init that failed, an end) */
static void unwrap(z_streamp s) {
    if (!s || (s->zalloc != host_alloc && s->zfree != host_free)) return;
    GuestAllocator* a = s->opaque;
    s->zalloc = (alloc_func)(uintptr_t)a->zalloc;
    s->zfree = (free_func)(uintptr_t)a->zfree;
    s->opaque = (voidpf)(uintptr_t)a->opaque;
    free(a);
}

/* an init function (deflateInit_, deflateInit2_, inflateInit_, inflateInit2_), with the stream in x0 */
static void init_call(CPU* c, void* fn) {
    z_streamp s = PTR(0);
    bool wrapped = wrap(s);
    hle_pass(c, fn);
    if (wrapped && (int32_t)X(0) != Z_OK) unwrap(s);
}
/* deflateEnd and inflateEnd: the allocator the guest's again after the stream's memory is freed */
static void end_call(CPU* c, void* fn) {
    z_streamp s = PTR(0);
    hle_pass(c, fn);
    unwrap(s);
}

#define P(name)    {#name, hle_pass, (void*)name}
#define I(name)    {#name, init_call, (void*)name}
#define E(name)    {#name, end_call, (void*)name}
const HleSymbol hle_zlib[] = {
    P(zlibVersion), P(zError), P(adler32), P(adler32_combine), P(crc32), P(crc32_combine), P(get_crc_table),
    I(deflateInit_), I(deflateInit2_), P(deflate), E(deflateEnd), P(deflateReset), P(deflateParams), P(deflateBound),
    P(deflateSetDictionary), P(deflateSetHeader), P(deflatePrime), P(deflateTune),
    I(inflateInit_), I(inflateInit2_), P(inflate), E(inflateEnd), P(inflateReset), P(inflateReset2),
    P(inflateSetDictionary), P(inflateGetDictionary), P(inflateSync), P(inflatePrime), P(inflateGetHeader),
    P(compress), P(compress2), P(compressBound), P(uncompress), P(uncompress2),
    /* the gz files: their descriptors are the host's, as the guest's are */
    P(gzopen), P(gzdopen), P(gzbuffer), P(gzsetparams), P(gzread), P(gzfread), P(gzwrite), P(gzfwrite), P(gzputs),
    P(gzgets), P(gzputc), P(gzgetc), P(gzungetc), P(gzflush), P(gzseek), P(gzrewind), P(gztell), P(gzoffset),
    P(gzeof), P(gzdirect), P(gzclose), P(gzclose_r), P(gzclose_w), P(gzerror), P(gzclearerr),
    {"", NULL, NULL},
};
#undef P
#undef I
#undef E
#else
const HleSymbol hle_zlib[] = {
    {"", NULL, NULL},
};
#endif
