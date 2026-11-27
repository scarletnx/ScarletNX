/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The cache file read (jit_cache.h); its key is jit_cache_key.c's */
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnxJitCacheRecord pub;
    SnxJitCacheFunction* functions;
} Record;

struct SnxJitCache {
    unsigned char* buffer;
    Record* records;
    size_t count;
};

/* the length of the field at the start of key's length bytes: a key's fields are name=value, a space between two */
static size_t field_length(const char* key, size_t length) {
    size_t n = 0;
    while (n < length && key[n] && key[n] != ' ') ++n;
    return n;
}
/* the length of field's name, before its = */
static size_t name_length(const char* field, size_t length) {
    const char* equals = (const char*)memchr(field, '=', length);
    return equals ? (size_t)(equals - field) : length;
}
/* key's field (of its n bytes) named as field's first name bytes: NULL where it has none, else it with its length in
   *found */
static const char* field_named(const char* key, size_t n, const char* field, size_t name, size_t* found) {
    for (size_t at = 0; at < n; ++at) {
        const size_t here = field_length(key + at, n - at);
        if (name_length(key + at, here) == name && !memcmp(key + at, field, name)) {
            *found = here;
            return key + at;
        }
        at += here;
    }
    return NULL;
}
/* the first field of a (an bytes) that b lacks or has otherwise, said into out as what a file was made for (a is the
   file's, made) or (a is the build's) what the build makes that the file was not made for: 0 where there is none */
static int key_field_change(const char* a, size_t an, const char* b, size_t bn, int made, char* out, size_t size) {
    for (size_t at = 0; at < an; ++at) {
        const size_t here = field_length(a + at, an - at), name = name_length(a + at, here);
        size_t other = 0;
        const char* in_b = here ? field_named(b, bn, a + at, name, &other) : NULL;
        if (here && (!in_b || other != here || memcmp(in_b, a + at, here))) {
            if (!made)
                snprintf(out, size, "made for no %.*s, and this build makes %.*s", (int)name, a + at, (int)here,
                         a + at);
            else if (!in_b)
                snprintf(out, size, "made for %.*s, and this build makes no %.*s", (int)here, a + at, (int)name,
                         a + at);
            else
                snprintf(out, size, "made for %.*s, and this build makes %.*s", (int)here, a + at, (int)other, in_b);
            return 1;
        }
        at += here;
    }
    return 0;
}
void snx_jit_cache_key_change(const char* was, size_t length, const char* key, char* out, size_t size) {
    const size_t n = strlen(key);
    if (!key_field_change(was, length, key, n, 1, out, size) && !key_field_change(key, n, was, length, 0, out, size))
        snprintf(out, size, "made for the same key");
}

static void free_record(Record* r) {
    if (r->functions) {
        for (size_t i = 0; i < r->pub.function_count; ++i) {
            free((void*)r->functions[i].leaders);
            free((void*)r->functions[i].runs);
        }
        free(r->functions);
    }
    free((void*)r->pub.name);
    free((void*)r->pub.object);
    *r = (Record){0};
}

/* one record's body, already hash-checked; object and metadata point into the file buffer */
static int parse_record(const unsigned char* body, size_t length, Record* r, char* error, size_t error_size) {
    const unsigned char* p = body, *end = body + length;
    if (length < 6) {
        snprintf(error, error_size, "record is too short");
        return -1;
    }
    size_t name_length = snx_ld_le16(p);
    if (name_length >= 64 || 2 + name_length + 4 > length) {
        snprintf(error, error_size, "record name is bad");
        return -1;
    }
    char* name = (char*)malloc(name_length + 1);
    if (!name) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    memcpy(name, p + 2, name_length);
    name[name_length] = 0;
    r->pub.name = name;
    p += 2 + name_length;
    size_t function_count = snx_ld_le32(p);
    p += 4;
    if (function_count > (size_t)(end - p) / 32) {
        snprintf(error, error_size, "record has too many functions");
        return -1;
    }
    r->functions = function_count ? (SnxJitCacheFunction*)calloc(function_count, sizeof(*r->functions)) : NULL;
    if (function_count && !r->functions) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    r->pub.function_count = function_count;
    r->pub.functions = r->functions;
    for (size_t i = 0; i < function_count; ++i) {
        SnxJitCacheFunction* f = &r->functions[i];
        if ((size_t)(end - p) < 12) {
            snprintf(error, error_size, "record function is cut off");
            return -1;
        }
        f->start = snx_ld_le64(p);
        size_t leaders = snx_ld_le32(p + 8);
        p += 12;
        if (leaders > (size_t)(end - p) / 8) {
            snprintf(error, error_size, "record leaders are cut off");
            return -1;
        }
        if (leaders) {
            f->leaders = (SnxU64*)malloc(leaders * sizeof(*f->leaders));
            if (!f->leaders) {
                snprintf(error, error_size, "out of memory");
                return -1;
            }
            f->leader_count = leaders;
            for (size_t k = 0; k < leaders; ++k) ((SnxU64*)f->leaders)[k] = snx_ld_le64(p + 8 * k);
        }
        p += leaders * 8;
        if ((size_t)(end - p) < 4) {
            snprintf(error, error_size, "record runs are cut off");
            return -1;
        }
        size_t runs = snx_ld_le32(p);
        p += 4;
        if (runs > (size_t)(end - p) / 12) {
            snprintf(error, error_size, "record runs are cut off");
            return -1;
        }
        if (runs) {
            f->runs = (SnxVariantRun*)malloc(runs * sizeof(*f->runs));
            if (!f->runs) {
                snprintf(error, error_size, "out of memory");
                return -1;
            }
            f->run_count = runs;
            for (size_t k = 0; k < runs; ++k) {
                ((SnxVariantRun*)f->runs)[k].address = snx_ld_le64(p);
                ((SnxVariantRun*)f->runs)[k].words = snx_ld_le32(p + 8);
                p += 12;
            }
        }
        if ((size_t)(end - p) < 16) {
            snprintf(error, error_size, "record hashes are cut off");
            return -1;
        }
        f->h1 = snx_ld_le64(p);
        f->h2 = snx_ld_le64(p + 8);
        p += 16;
    }
    if ((size_t)(end - p) < 8) {
        snprintf(error, error_size, "record object size is cut off");
        return -1;
    }
    r->pub.object_size = (size_t)snx_ld_le64(p);
    p += 8;
    if (r->pub.object_size != (size_t)(end - p)) {
        snprintf(error, error_size, "record object size does not match the body");
        return -1;
    }
    /* the object starts at whatever offset the record's metadata ends at: ELF reads it as Elf64_* structs, which
       need the alignment a malloc gives, not the file's */
    r->pub.object = (const unsigned char*)malloc(r->pub.object_size ? r->pub.object_size : 1);
    if (!r->pub.object) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    memcpy((void*)r->pub.object, p, r->pub.object_size);
    return 0;
}

SnxJitCache* snx_jit_cache_open(const char* path, const char* key, char* error, size_t error_size) {
    long length;
    if (error && error_size) error[0] = 0;
    if (!path || !key) {
        snprintf(error, error_size, "no cache path or key");
        return NULL;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(error, error_size, "cannot open %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0) {
        fclose(f);
        snprintf(error, error_size, "cannot size %s", path);
        return NULL;
    }
    if ((size_t)length > SNX_JIT_CACHE_MAX) {
        fclose(f);
        snprintf(error, error_size, "%s is larger than the cache limit", path);
        return NULL;
    }
    size_t size = (size_t)length;
    unsigned char* buffer = (unsigned char*)malloc(size ? size : 1);
    if (!buffer) {
        fclose(f);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    if (size && (fseek(f, 0, SEEK_SET) || fread(buffer, 1, size, f) != size)) {
        fclose(f);
        free(buffer);
        snprintf(error, error_size, "cannot read %s", path);
        return NULL;
    }
    fclose(f);
    if (size < 12 || memcmp(buffer, SNX_JIT_CACHE_MAGIC, 8)) {
        free(buffer);
        snprintf(error, error_size, "%s is not a ScarletNX JIT cache", path);
        return NULL;
    }
    size_t key_length = snx_ld_le32(buffer + 8);
    size_t header = 12 + key_length;
    if (header > size) {
        free(buffer);
        snprintf(error, error_size, "%s has a bad key length", path);
        return NULL;
    }
    if (key_length != strlen(key) || memcmp(buffer + 12, key, key_length)) {
        char change[512];
        snx_jit_cache_key_change((const char*)buffer + 12, key_length, key, change, sizeof change);
        free(buffer);
        snprintf(error, error_size, "%s: the cache key differs from the build's: %s", path, change);
        return NULL;
    }
    SnxJitCache* cache = (SnxJitCache*)calloc(1, sizeof(*cache));
    if (!cache) {
        free(buffer);
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    cache->buffer = buffer;
    size_t at = header, count = 0, capacity = 0;
    while (at < size) {
        if (size - at < 12 || snx_ld_le32(buffer + at) != SNX_JIT_RECORD_MAGIC) {
            snprintf(error, error_size, "%s has a bad record header", path);
            goto fail;
        }
        SnxU64 body_length = snx_ld_le64(buffer + at + 4);
        if (body_length > (SnxU64)(size - at - 12) || (size - at - 12) - (size_t)body_length < 8) {
            snprintf(error, error_size, "%s has a record that runs past the file", path);
            goto fail;
        }
        const unsigned char* body = buffer + at + 12;
        if (snx_fnv1a(SNX_FNV_BASIS, body, (size_t)body_length) != snx_ld_le64(body + body_length)) {
            snprintf(error, error_size, "%s has a record with a bad hash", path);
            goto fail;
        }
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            Record* grown = (Record*)realloc(cache->records, capacity * sizeof(*grown));
            if (!grown) {
                snprintf(error, error_size, "out of memory");
                goto fail;
            }
            cache->records = grown;
        }
        cache->records[count] = (Record){0};
        cache->count = count + 1; /* close frees a record parse_record left half-built */
        if (parse_record(body, (size_t)body_length, &cache->records[count], error, error_size)) goto fail;
        ++count;
        cache->count = count;
        at += 12 + (size_t)body_length + 8;
    }
    cache->count = count;
    return cache;
fail:
    snx_jit_cache_close(cache);
    return NULL;
}

void snx_jit_cache_close(SnxJitCache* cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->count; ++i) free_record(&cache->records[i]);
    free(cache->records);
    free(cache->buffer);
    free(cache);
}

size_t snx_jit_cache_count(const SnxJitCache* cache) { return cache ? cache->count : 0; }

const SnxJitCacheRecord* snx_jit_cache_record(const SnxJitCache* cache, size_t index) {
    if (!cache || index >= cache->count) return NULL;
    return &cache->records[index].pub;
}
