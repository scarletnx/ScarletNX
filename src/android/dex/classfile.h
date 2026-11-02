/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_CLASSFILE_H
#define SNX_ANDROID_CLASSFILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The pieces of a JVM class file (The Java Virtual Machine Specification, chapter 4) that the DEX translator writes */

/* a growing byte buffer, big-endian as a class file is. A failed allocation sets failed and drops later writes */
typedef struct {
    unsigned char* data;
    size_t size, capacity;
    bool failed;
} CfBuffer;
void cf_free(CfBuffer* b);
void cf_bytes(CfBuffer* b, const void* data, size_t size);
void cf_u1(CfBuffer* b, unsigned value);
void cf_u2(CfBuffer* b, unsigned value);
void cf_u4(CfBuffer* b, uint32_t value);
/* writes over bytes already written */
void cf_put_u2(CfBuffer* b, size_t at, unsigned value);
void cf_put_u4(CfBuffer* b, size_t at, uint32_t value);

enum {
    CF_UTF8 = 1,
    CF_INTEGER = 3,
    CF_FLOAT = 4,
    CF_LONG = 5,
    CF_DOUBLE = 6,
    CF_CLASS = 7,
    CF_STRING = 8,
    CF_FIELDREF = 9,
    CF_METHODREF = 10,
    CF_INTERFACE_METHODREF = 11,
    CF_NAME_AND_TYPE = 12
};
/* the constant pool: each entry once, in the order first asked for. Its indices start at 1, and a long or a double
   takes two. More than 65535 entries sets failed */
typedef struct CfPoolEntry CfPoolEntry;
typedef struct {
    CfBuffer bytes; /* the entries as the class file has them */
    unsigned count; /* the next index */
    CfPoolEntry* entries;
    uint32_t* table; /* open addressing over entries, by hash */
    size_t entry_count, entry_capacity, table_size;
    bool failed;
} CfPool;
void cf_pool_free(CfPool* pool);
/* a modified UTF-8 string, as DEX and class files both keep strings */
unsigned cf_utf8(CfPool* pool, const char* text);
unsigned cf_utf8_n(CfPool* pool, const char* text, size_t size);
unsigned cf_integer(CfPool* pool, uint32_t bits);
unsigned cf_float(CfPool* pool, uint32_t bits);
unsigned cf_long(CfPool* pool, uint64_t bits);
unsigned cf_double(CfPool* pool, uint64_t bits);
/* a class by its internal name ("java/lang/String"), or an array class by its descriptor ("[I") */
unsigned cf_class(CfPool* pool, const char* name);
/* the same from a DEX descriptor ("Ljava/lang/String;" or "[I") */
unsigned cf_class_descriptor(CfPool* pool, const char* descriptor);
unsigned cf_string(CfPool* pool, const char* text);
unsigned cf_name_and_type(CfPool* pool, const char* name, const char* descriptor);
/* a field or method reference: owner a DEX descriptor of the class, descriptor the member's JVM descriptor */
unsigned cf_member(CfPool* pool, unsigned tag, const char* owner, const char* name, const char* descriptor);
#endif
