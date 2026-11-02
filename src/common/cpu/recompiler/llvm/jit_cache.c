/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/jit_cache.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/rtlib_object.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
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

char* snx_jit_cache_key(char opt, int hook, int svc, SnxU64 guest) {
    char* cpu = LLVMGetHostCPUName();
    char* features = LLVMGetHostCPUFeatures();
    char* triple = LLVMGetDefaultTargetTriple();
    if (!cpu || !features || !triple) {
        LLVMDisposeMessage(cpu);
        LLVMDisposeMessage(features);
        LLVMDisposeMessage(triple);
        return NULL;
    }
    size_t n = strlen(cpu) + strlen(features) + strlen(triple) + 256;
    char* key = (char*)malloc(n);
    if (key) {
        SnxU64 rtlib = snx_fnv1a(SNX_FNV_BASIS, snx_rtlib_object, (size_t)snx_rtlib_object_size);
        /* dec: the decoding (a64.decode and its profile) and a64: the guest's features choose what each word does,
           emit: what the emitters make of it */
        snprintf(key, n,
                 "abi=%d obj=%d emit=%d llvm=%s triple=%s rtlib=%016llx dec=%016llx a64=%016llx cpu=%s opt=%c hook=%d "
                 "svc=%d slp=%d fpsr=%d volatile=%d features=%s",
                 SNX_ABI, SNX_IR_OBJECT_VERSION, SNX_IR_EMIT_VERSION, LLVM_VERSION_STRING, triple,
                 (unsigned long long)rtlib, (unsigned long long)snx_a64_decoder_hash(),
                 (unsigned long long)snx_a64_features(guest), cpu, opt, hook, svc, snx_ir_slp(), snx_ir_fpsr_exact(),
                 snx_ir_volatile(), features);
    }
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    LLVMDisposeMessage(triple);
    return key;
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
        free(buffer);
        snprintf(error, error_size, "the cache key differs from the build's");
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
