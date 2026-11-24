/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/utilities/common.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- SHA-256 (FIPS 180-4) ---- */

typedef struct {
    uint32_t h[8];
    uint64_t length; /* bytes so far */
    unsigned char block[64];
    size_t used;
} Sha256;

static const uint32_t g_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }

static void sha256_block(Sha256* s, const unsigned char* p) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + g_sha256_k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha256_init(Sha256* s) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->length = 0;
    s->used = 0;
}

static void sha256_put(void* user, const void* data, size_t size) {
    Sha256* s = (Sha256*)user;
    const unsigned char* p = (const unsigned char*)data;
    s->length += size;
    while (size) {
        size_t take = 64 - s->used < size ? 64 - s->used : size;
        memcpy(s->block + s->used, p, take);
        s->used += take, p += take, size -= take;
        if (s->used == 64) {
            sha256_block(s, s->block);
            s->used = 0;
        }
    }
}

static void sha256_end(Sha256* s, unsigned char out[32]) {
    uint64_t bits = s->length * 8;
    unsigned char pad = 0x80, zero = 0, length[8];
    sha256_put(s, &pad, 1);
    while (s->used != 56) sha256_put(s, &zero, 1);
    for (unsigned i = 0; i < 8; ++i) length[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_put(s, length, 8);
    for (unsigned i = 0; i < 32; ++i) out[i] = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

void snx_x64_cache_sha256(const void* data, size_t size, unsigned char out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_put(&s, data, size);
    sha256_end(&s, out);
}

#if defined(__x86_64__) && !defined(_WIN32) && defined(SNX_HAVE_X64)
#include <pthread.h>

typedef struct {
    unsigned char key[32];
    SnxU64 used;          /* the tick of its last use: the smallest leaves first */
    size_t count, size;   /* the unit's functions (made's bytes), and its object's bytes */
    unsigned char* bytes; /* made, then the object */
} Entry;

static struct {
    pthread_mutex_t lock;
    SnxU64 ceiling, memory, tick, bytes;
    Entry* entries;
    size_t count, capacity;
    size_t hits, misses, evicted, too_large;
    int said_memory, said_large;
} g_x64_cache = {.lock = PTHREAD_MUTEX_INITIALIZER, .ceiling = SNX_X64_CACHE_CEILING_DEFAULT};

void snx_x64_cache_ceiling(SnxU64 bytes) {
    pthread_mutex_lock(&g_x64_cache.lock);
    g_x64_cache.ceiling = bytes;
    pthread_mutex_unlock(&g_x64_cache.lock);
}
void snx_x64_cache_memory(SnxU64 bytes) {
    pthread_mutex_lock(&g_x64_cache.lock);
    g_x64_cache.memory = bytes;
    pthread_mutex_unlock(&g_x64_cache.lock);
}

/* the limit, with the lock held */
static SnxU64 limit_locked(void) {
    SnxU64 memory = g_x64_cache.memory;
    if (!memory && !snx_memory_total(&memory)) {
        if (!g_x64_cache.said_memory++)
            snx_error("x86-64 unit cache: the system does not tell its memory, so the cache holds up to its ceiling, "
                      "%.1f MB, rather than 1%% of the memory",
                      (double)g_x64_cache.ceiling / 1e6);
        return g_x64_cache.ceiling;
    }
    return memory / 100 < g_x64_cache.ceiling ? memory / 100 : g_x64_cache.ceiling;
}
SnxU64 snx_x64_cache_limit(void) {
    pthread_mutex_lock(&g_x64_cache.lock);
    SnxU64 limit = limit_locked();
    pthread_mutex_unlock(&g_x64_cache.lock);
    return limit;
}

/* the units used longest ago let go until need more bytes fit under limit */
static void room_locked(SnxU64 need, SnxU64 limit) {
    while (g_x64_cache.count && g_x64_cache.bytes + need > limit) {
        size_t oldest = 0;
        for (size_t i = 1; i < g_x64_cache.count; ++i)
            if (g_x64_cache.entries[i].used < g_x64_cache.entries[oldest].used) oldest = i;
        g_x64_cache.bytes -= g_x64_cache.entries[oldest].size + g_x64_cache.entries[oldest].count;
        free(g_x64_cache.entries[oldest].bytes);
        g_x64_cache.entries[oldest] = g_x64_cache.entries[--g_x64_cache.count];
        ++g_x64_cache.evicted;
    }
}

static Entry* find_locked(const unsigned char key[32]) {
    for (size_t i = 0; i < g_x64_cache.count; ++i)
        if (!memcmp(g_x64_cache.entries[i].key, key, 32)) return &g_x64_cache.entries[i];
    return NULL;
}

int snx_x64_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options,
                          unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                          size_t error_size) {
    static const char tag[] = "snx-x64-unit";
    size_t count = last - first;
    *reused = 0;
    SnxU64 limit = snx_x64_cache_limit();
    if (!limit) return snx_x64_object(codegen, first, last, options, made, object, size, error, error_size);
    Sha256 s;
    sha256_init(&s);
    sha256_put(&s, tag, sizeof tag);
    snx_x64_unit_facts(codegen, first, last, options, sha256_put, &s);
    unsigned char key[32];
    sha256_end(&s, key);
    pthread_mutex_lock(&g_x64_cache.lock);
    Entry* e = find_locked(key);
    if (e && e->count == count) {
        memcpy(made, e->bytes, count);
        *size = e->size;
        *object = e->size ? (unsigned char*)snx_alloc(e->size) : NULL;
        if (e->size) memcpy(*object, e->bytes + count, e->size);
        e->used = ++g_x64_cache.tick;
        ++g_x64_cache.hits;
        pthread_mutex_unlock(&g_x64_cache.lock);
        *reused = 1;
        return 0;
    }
    ++g_x64_cache.misses;
    pthread_mutex_unlock(&g_x64_cache.lock);
    int rc = snx_x64_object(codegen, first, last, options, made, object, size, error, error_size);
    if (rc) return rc;
    SnxU64 need = (SnxU64)*size + count;
    pthread_mutex_lock(&g_x64_cache.lock);
    limit = limit_locked();
    if (need > limit) {
        ++g_x64_cache.too_large;
        if (!g_x64_cache.said_large++)
            snx_error(
                "x86-64 unit cache: a unit of %llu bytes is more than the %llu bytes it holds (1%% of the memory, "
                "at most the ceiling unit_cache_ceil_mb), so it is made again each time",
                (unsigned long long)need, (unsigned long long)limit);
    } else if (!find_locked(key)) { /* another thread made the same unit meanwhile */
        room_locked(need, limit);
        if (g_x64_cache.count == g_x64_cache.capacity) {
            size_t capacity = g_x64_cache.capacity ? 2 * g_x64_cache.capacity : 64;
            g_x64_cache.entries = (Entry*)snx_realloc(g_x64_cache.entries, capacity * sizeof(Entry));
            g_x64_cache.capacity = capacity;
        }
        e = &g_x64_cache.entries[g_x64_cache.count++];
        memcpy(e->key, key, 32);
        e->used = ++g_x64_cache.tick;
        e->count = count;
        e->size = *size;
        e->bytes = (unsigned char*)snx_alloc(count + *size ? count + *size : 1);
        memcpy(e->bytes, made, count);
        if (*size) memcpy(e->bytes + count, *object, *size);
        g_x64_cache.bytes += need;
    }
    pthread_mutex_unlock(&g_x64_cache.lock);
    return 0;
}

void snx_x64_cache_stats(SnxX64CacheStats* stats) {
    pthread_mutex_lock(&g_x64_cache.lock);
    stats->units = g_x64_cache.count;
    stats->bytes = g_x64_cache.bytes;
    stats->limit = limit_locked();
    stats->hits = g_x64_cache.hits;
    stats->misses = g_x64_cache.misses;
    stats->evicted = g_x64_cache.evicted;
    stats->too_large = g_x64_cache.too_large;
    pthread_mutex_unlock(&g_x64_cache.lock);
}

void snx_x64_cache_clear(void) {
    pthread_mutex_lock(&g_x64_cache.lock);
    for (size_t i = 0; i < g_x64_cache.count; ++i) free(g_x64_cache.entries[i].bytes);
    g_x64_cache.count = 0;
    g_x64_cache.bytes = 0;
    g_x64_cache.hits = g_x64_cache.misses = g_x64_cache.evicted = g_x64_cache.too_large = 0;
    g_x64_cache.said_large = 0;
    pthread_mutex_unlock(&g_x64_cache.lock);
}

#else
/* no x86-64 back end in this build: nothing to hold */
void snx_x64_cache_ceiling(SnxU64 bytes) { (void)bytes; }
void snx_x64_cache_memory(SnxU64 bytes) { (void)bytes; }
SnxU64 snx_x64_cache_limit(void) { return 0; }
int snx_x64_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxX64Options* options,
                          unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                          size_t error_size) {
    *reused = 0;
    return snx_x64_object(codegen, first, last, options, made, object, size, error, error_size);
}
void snx_x64_cache_stats(SnxX64CacheStats* stats) { *stats = (SnxX64CacheStats){0}; }
void snx_x64_cache_clear(void) {}
#endif
