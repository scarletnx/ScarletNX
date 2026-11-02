/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_DEX_H
#define SNX_ANDROID_DEX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A DEX file (Dalvik executable, versions 035 to 039) read in place: the caller keeps its bytes for the Dex's life.
   The format is AOSP's dex-format documentation (source.android.com/docs/core/runtime/dex-format) */

#define DEX_NO_INDEX 0xffffffffu

/* access flags that exist only in DEX */
#define DEX_ACC_CONSTRUCTOR           0x10000u
#define DEX_ACC_DECLARED_SYNCHRONIZED 0x20000u

typedef struct {
    uint32_t class_idx, access, super_idx, interfaces_off, source_file_idx, annotations_off, class_data_off,
        static_values_off;
} DexClassDef;
typedef struct {
    uint32_t field_idx, access;
} DexField;
typedef struct {
    uint32_t method_idx, access, code_off;
} DexMethod;
/* a class_data_item, decoded: the fields and methods point into one allocation (dex_class_data_free) */
typedef struct {
    uint32_t static_count, instance_count, direct_count, virtual_count;
    DexField* fields;   /* the static ones, then the instance ones */
    DexMethod* methods; /* the direct ones, then the virtual ones */
} DexClassData;
typedef struct {
    uint16_t registers, ins, outs, tries;
    uint32_t insns_size; /* in 16-bit code units */
    const unsigned char* insns;
    const unsigned char* try_items; /* tries of them, 8 bytes each */
    const unsigned char* handlers;  /* the encoded_catch_handler_list */
} DexCode;

typedef struct {
    const unsigned char* data;
    size_t size;
    uint32_t string_count, type_count, proto_count, field_count, method_count, class_count;
    uint32_t string_ids, type_ids, proto_ids, field_ids, method_ids, class_defs; /* their offsets */
    unsigned version; /* 35 to 39 */
    uint32_t* sorted; /* the class defs by their descriptors, for dex_find_class */
} Dex;

/* false with error filled when data is not a DEX file this reads */
bool dex_open(Dex* dex, const unsigned char* data, size_t size, char* error, size_t error_size);
void dex_close(Dex* dex);

/* a string, MUTF-8 and zero-terminated in the file; "" for a bad index */
const char* dex_string(const Dex* dex, uint32_t idx);
/* a type's descriptor ("Ljava/lang/String;", "[I") */
const char* dex_type(const Dex* dex, uint32_t idx);
/* a field: its class's type index, its type's, its name */
void dex_field(const Dex* dex, uint32_t idx, uint32_t* class_idx, uint32_t* type_idx, const char** name);
/* a method: its class's type index, its prototype's index, its name */
void dex_method(const Dex* dex, uint32_t idx, uint32_t* class_idx, uint32_t* proto_idx, const char** name);
/* a prototype: its return type's index, and its parameters' types as a type_list (count then 16-bit indices) */
uint32_t dex_proto_return(const Dex* dex, uint32_t proto_idx);
uint32_t dex_proto_param_count(const Dex* dex, uint32_t proto_idx);
uint32_t dex_proto_param(const Dex* dex, uint32_t proto_idx, uint32_t i);
/* a type_list at an offset (interfaces): its count, and its i-th type's index */
uint32_t dex_type_list_count(const Dex* dex, uint32_t offset);
uint32_t dex_type_list_item(const Dex* dex, uint32_t offset, uint32_t i);

DexClassDef dex_class_def(const Dex* dex, uint32_t i);
/* the class def whose class is descriptor; false for none */
bool dex_find_class(const Dex* dex, const char* descriptor, uint32_t* index);
/* false for a bad class_data_item. A class with no data (class_data_off 0) has an empty one */
bool dex_class_data(const Dex* dex, const DexClassDef* def, DexClassData* out);
void dex_class_data_free(DexClassData* data);
/* false for a bad code_item */
bool dex_code(const Dex* dex, uint32_t offset, DexCode* code);

/* LEB128 at *p, which moves past it; end bounds it, and a value past it reads as 0 */
uint32_t dex_uleb(const unsigned char** p, const unsigned char* end);
int32_t dex_sleb(const unsigned char** p, const unsigned char* end);

/* encoded_value types */
enum {
    DEX_VALUE_BYTE = 0x00,
    DEX_VALUE_SHORT = 0x02,
    DEX_VALUE_CHAR = 0x03,
    DEX_VALUE_INT = 0x04,
    DEX_VALUE_LONG = 0x06,
    DEX_VALUE_FLOAT = 0x10,
    DEX_VALUE_DOUBLE = 0x11,
    DEX_VALUE_METHOD_TYPE = 0x15,
    DEX_VALUE_METHOD_HANDLE = 0x16,
    DEX_VALUE_STRING = 0x17,
    DEX_VALUE_TYPE = 0x18,
    DEX_VALUE_FIELD = 0x19,
    DEX_VALUE_METHOD = 0x1a,
    DEX_VALUE_ENUM = 0x1b,
    DEX_VALUE_ARRAY = 0x1c,
    DEX_VALUE_ANNOTATION = 0x1d,
    DEX_VALUE_NULL = 0x1e,
    DEX_VALUE_BOOLEAN = 0x1f
};
/* one encoded_value: its type, and its value as bits (sign-extended for the signed integer types, the float and
   double types' bits in place, an index for the indexed types, 0 or 1 for a boolean). For an array or an annotation
   *p is left at its contents (dex_skip_value skips a whole one) */
typedef struct {
    unsigned type;
    uint64_t bits;
} DexValue;
DexValue dex_read_value(const unsigned char** p, const unsigned char* end);
void dex_skip_value(const unsigned char** p, const unsigned char* end);
#endif
