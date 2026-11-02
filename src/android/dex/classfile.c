/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/dex/classfile.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>

void cf_free(CfBuffer* b) {
    free(b->data);
    *b = (CfBuffer){0};
}
static bool reserve(CfBuffer* b, size_t more) {
    if (b->failed) return false;
    if (b->size + more <= b->capacity) return true;
    size_t capacity = b->capacity ? b->capacity : 256;
    while (capacity < b->size + more) capacity *= 2;
    unsigned char* data = realloc(b->data, capacity);
    if (!data) {
        b->failed = true;
        return false;
    }
    b->data = data;
    b->capacity = capacity;
    return true;
}
void cf_bytes(CfBuffer* b, const void* data, size_t size) {
    if (!size || !reserve(b, size)) return;
    memcpy(b->data + b->size, data, size);
    b->size += size;
}
void cf_u1(CfBuffer* b, unsigned value) {
    if (!reserve(b, 1)) return;
    b->data[b->size++] = (unsigned char)value;
}
void cf_u2(CfBuffer* b, unsigned value) {
    if (!reserve(b, 2)) return;
    b->data[b->size++] = (unsigned char)(value >> 8);
    b->data[b->size++] = (unsigned char)value;
}
void cf_u4(CfBuffer* b, uint32_t value) {
    cf_u2(b, value >> 16);
    cf_u2(b, value & 0xffff);
}
void cf_put_u2(CfBuffer* b, size_t at, unsigned value) {
    if (b->failed || at + 2 > b->size) return;
    b->data[at] = (unsigned char)(value >> 8);
    b->data[at + 1] = (unsigned char)value;
}
void cf_put_u4(CfBuffer* b, size_t at, uint32_t value) {
    cf_put_u2(b, at, value >> 16);
    cf_put_u2(b, at + 2, value & 0xffff);
}

/* an entry: its tag and its bytes after the tag, where pool->bytes keeps them */
struct CfPoolEntry {
    size_t at, size; /* in pool->bytes, past the tag */
    unsigned tag, index;
    uint32_t hash;
};
void cf_pool_free(CfPool* pool) {
    cf_free(&pool->bytes);
    free(pool->entries);
    free(pool->table);
    *pool = (CfPool){0};
}
static uint32_t entry_hash(unsigned tag, const unsigned char* data, size_t size) {
    return (uint32_t)snx_fnv_step(snx_fnv1a(SNX_FNV_BASIS, data, size), tag);
}
static bool grow_table(CfPool* pool) {
    size_t size = pool->table_size ? pool->table_size * 2 : 1024;
    uint32_t* table = malloc(size * sizeof *table);
    if (!table) return false;
    for (size_t i = 0; i < size; ++i) table[i] = UINT32_MAX;
    for (size_t e = 0; e < pool->entry_count; ++e) {
        size_t slot = pool->entries[e].hash & (size - 1);
        while (table[slot] != UINT32_MAX) slot = (slot + 1) & (size - 1);
        table[slot] = (uint32_t)e;
    }
    free(pool->table);
    pool->table = table;
    pool->table_size = size;
    return true;
}
/* the entry of tag with these bytes, added when the pool has none */
static unsigned entry(CfPool* pool, unsigned tag, const unsigned char* data, size_t size) {
    if (pool->failed) return 0;
    uint32_t hash = entry_hash(tag, data, size);
    if ((pool->entry_count + 1) * 2 > pool->table_size && !grow_table(pool)) {
        pool->failed = true;
        return 0;
    }
    size_t slot = hash & (pool->table_size - 1);
    for (; pool->table[slot] != UINT32_MAX; slot = (slot + 1) & (pool->table_size - 1)) {
        const CfPoolEntry* e = &pool->entries[pool->table[slot]];
        if (e->hash == hash && e->tag == tag && e->size == size && !memcmp(pool->bytes.data + e->at, data, size))
            return e->index;
    }
    unsigned width = tag == CF_LONG || tag == CF_DOUBLE ? 2 : 1;
    if (!pool->count) pool->count = 1;
    if (pool->count + width > 65535) {
        pool->failed = true;
        return 0;
    }
    if (pool->entry_count == pool->entry_capacity) {
        size_t capacity = pool->entry_capacity ? pool->entry_capacity * 2 : 256;
        CfPoolEntry* entries = realloc(pool->entries, capacity * sizeof *entries);
        if (!entries) {
            pool->failed = true;
            return 0;
        }
        pool->entries = entries;
        pool->entry_capacity = capacity;
    }
    cf_u1(&pool->bytes, tag);
    size_t at = pool->bytes.size;
    cf_bytes(&pool->bytes, data, size);
    if (pool->bytes.failed) {
        pool->failed = true;
        return 0;
    }
    unsigned index = pool->count;
    pool->count += width;
    pool->entries[pool->entry_count] = (CfPoolEntry){at, size, tag, index, hash};
    pool->table[slot] = (uint32_t)pool->entry_count++;
    return index;
}
unsigned cf_utf8_n(CfPool* pool, const char* text, size_t size) {
    if (size > 65535) {
        pool->failed = true;
        return 0;
    }
    unsigned char* data = malloc(size + 2);
    if (!data) {
        pool->failed = true;
        return 0;
    }
    data[0] = (unsigned char)(size >> 8);
    data[1] = (unsigned char)size;
    memcpy(data + 2, text, size);
    unsigned index = entry(pool, CF_UTF8, data, size + 2);
    free(data);
    return index;
}
unsigned cf_utf8(CfPool* pool, const char* text) {
    return cf_utf8_n(pool, text, strlen(text));
}
static unsigned u4_entry(CfPool* pool, unsigned tag, uint32_t bits) {
    unsigned char data[4] = {(unsigned char)(bits >> 24), (unsigned char)(bits >> 16), (unsigned char)(bits >> 8),
                             (unsigned char)bits};
    return entry(pool, tag, data, 4);
}
static unsigned u8_entry(CfPool* pool, unsigned tag, uint64_t bits) {
    unsigned char data[8];
    for (int i = 0; i < 8; ++i) data[i] = (unsigned char)(bits >> (56 - 8 * i));
    return entry(pool, tag, data, 8);
}
unsigned cf_integer(CfPool* pool, uint32_t bits) {
    return u4_entry(pool, CF_INTEGER, bits);
}
unsigned cf_float(CfPool* pool, uint32_t bits) {
    return u4_entry(pool, CF_FLOAT, bits);
}
unsigned cf_long(CfPool* pool, uint64_t bits) {
    return u8_entry(pool, CF_LONG, bits);
}
unsigned cf_double(CfPool* pool, uint64_t bits) {
    return u8_entry(pool, CF_DOUBLE, bits);
}
static unsigned ref_entry(CfPool* pool, unsigned tag, unsigned a, unsigned b) {
    unsigned char data[4] = {(unsigned char)(a >> 8), (unsigned char)a, (unsigned char)(b >> 8), (unsigned char)b};
    return entry(pool, tag, data, b ? 4 : 2);
}
unsigned cf_class(CfPool* pool, const char* name) {
    return ref_entry(pool, CF_CLASS, cf_utf8(pool, name), 0);
}
unsigned cf_class_descriptor(CfPool* pool, const char* descriptor) {
    size_t n = strlen(descriptor);
    if (descriptor[0] == 'L' && n >= 2 && descriptor[n - 1] == ';')
        return ref_entry(pool, CF_CLASS, cf_utf8_n(pool, descriptor + 1, n - 2), 0);
    return cf_class(pool, descriptor);
}
unsigned cf_string(CfPool* pool, const char* text) {
    return ref_entry(pool, CF_STRING, cf_utf8(pool, text), 0);
}
unsigned cf_name_and_type(CfPool* pool, const char* name, const char* descriptor) {
    return ref_entry(pool, CF_NAME_AND_TYPE, cf_utf8(pool, name), cf_utf8(pool, descriptor));
}
unsigned cf_member(CfPool* pool, unsigned tag, const char* owner, const char* name, const char* descriptor) {
    return ref_entry(pool, tag, cf_class_descriptor(pool, owner), cf_name_and_type(pool, name, descriptor));
}
