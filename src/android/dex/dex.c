/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/dex/dex.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* bounded loads: an offset past the file reads as 0, which every caller takes as an empty or bad item */
static uint32_t ld32(const Dex* dex, uint64_t offset) {
    return offset + 4 <= dex->size ? snx_ld_le32(dex->data + offset) : 0;
}
static uint32_t ld16(const Dex* dex, uint64_t offset) {
    return offset + 2 <= dex->size ? snx_ld_le16(dex->data + offset) : 0;
}

static const char* class_descriptor(const Dex* dex, uint32_t i) {
    return dex_type(dex, ld32(dex, (uint64_t)dex->class_defs + 32ull * i));
}

uint32_t dex_uleb(const unsigned char** p, const unsigned char* end) {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (*p >= end) return 0;
        unsigned byte = *(*p)++;
        value |= (uint32_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) break;
    }
    return value;
}
int32_t dex_sleb(const unsigned char** p, const unsigned char* end) {
    uint32_t value = 0;
    unsigned shift = 0, byte = 0;
    do {
        if (*p >= end) return 0;
        byte = *(*p)++;
        value |= (uint32_t)(byte & 0x7f) << shift;
        shift += 7;
    } while ((byte & 0x80) && shift < 35);
    if (shift < 32 && (byte & 0x40)) value |= ~(uint32_t)0 << shift;
    return (int32_t)value;
}

/* the class defs sorted by descriptor: a merge sort, as qsort has no context argument in C99 */
static void sort_classes(const Dex* dex, uint32_t* items, uint32_t* scratch, uint32_t count) {
    if (count < 2) return;
    uint32_t half = count / 2;
    sort_classes(dex, items, scratch, half);
    sort_classes(dex, items + half, scratch, count - half);
    uint32_t i = 0, j = half, k = 0;
    while (i < half && j < count)
        scratch[k++] = strcmp(class_descriptor(dex, items[j]), class_descriptor(dex, items[i])) < 0 ? items[j++]
                                                                                                    : items[i++];
    while (i < half) scratch[k++] = items[i++];
    while (j < count) scratch[k++] = items[j++];
    memcpy(items, scratch, count * sizeof *items);
}
static bool index_classes(Dex* dex, char* error, size_t error_size) {
    dex->sorted = malloc(((size_t)dex->class_count + 1) * sizeof *dex->sorted);
    uint32_t* scratch = malloc(((size_t)dex->class_count + 1) * sizeof *scratch);
    if (!dex->sorted || !scratch) {
        free(scratch);
        free(dex->sorted);
        dex->sorted = NULL;
        snprintf(error, error_size, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < dex->class_count; ++i) dex->sorted[i] = i;
    sort_classes(dex, dex->sorted, scratch, dex->class_count);
    free(scratch);
    return true;
}
bool dex_open(Dex* dex, const unsigned char* data, size_t size, char* error, size_t error_size) {
    *dex = (Dex){0};
    if (size < 0x70 || memcmp(data, "dex\n0", 5) || data[7]) {
        snprintf(error, error_size, "not a DEX file");
        return false;
    }
    unsigned version = (unsigned)(data[5] - '0') * 10 + (unsigned)(data[6] - '0');
    if (version < 35 || version > 39) {
        snprintf(error, error_size, "DEX version 0%u is not one this reads (035 to 039)", version);
        return false;
    }
    if (snx_ld_le32(data + 0x28) != 0x12345678u) {
        snprintf(error, error_size, "a big-endian DEX file");
        return false;
    }
    dex->data = data;
    dex->size = size;
    dex->version = version;
    dex->string_count = snx_ld_le32(data + 0x38);
    dex->string_ids = snx_ld_le32(data + 0x3c);
    dex->type_count = snx_ld_le32(data + 0x40);
    dex->type_ids = snx_ld_le32(data + 0x44);
    dex->proto_count = snx_ld_le32(data + 0x48);
    dex->proto_ids = snx_ld_le32(data + 0x4c);
    dex->field_count = snx_ld_le32(data + 0x50);
    dex->field_ids = snx_ld_le32(data + 0x54);
    dex->method_count = snx_ld_le32(data + 0x58);
    dex->method_ids = snx_ld_le32(data + 0x5c);
    dex->class_count = snx_ld_le32(data + 0x60);
    dex->class_defs = snx_ld_le32(data + 0x64);
    /* each table inside the file */
    const struct {
        uint32_t count, offset, item;
    } tables[] = {{dex->string_count, dex->string_ids, 4}, {dex->type_count, dex->type_ids, 4},
                  {dex->proto_count, dex->proto_ids, 12}, {dex->field_count, dex->field_ids, 8},
                  {dex->method_count, dex->method_ids, 8}, {dex->class_count, dex->class_defs, 32}};
    for (size_t i = 0; i < sizeof tables / sizeof *tables; ++i)
        if ((uint64_t)tables[i].offset + (uint64_t)tables[i].count * tables[i].item > size) {
            snprintf(error, error_size, "a DEX table runs past the file's end");
            return false;
        }
    return index_classes(dex, error, error_size);
}
void dex_close(Dex* dex) {
    free(dex->sorted);
    *dex = (Dex){0};
}

const char* dex_string(const Dex* dex, uint32_t idx) {
    if (idx >= dex->string_count) return "";
    uint32_t offset = ld32(dex, (uint64_t)dex->string_ids + 4ull * idx);
    if (offset >= dex->size) return "";
    const unsigned char* p = dex->data + offset;
    dex_uleb(&p, dex->data + dex->size); /* its length in UTF-16 units */
    /* the string must end inside the file */
    if (!memchr(p, 0, (size_t)(dex->data + dex->size - p))) return "";
    return (const char*)p;
}
const char* dex_type(const Dex* dex, uint32_t idx) {
    return idx < dex->type_count ? dex_string(dex, ld32(dex, (uint64_t)dex->type_ids + 4ull * idx)) : "";
}
void dex_field(const Dex* dex, uint32_t idx, uint32_t* class_idx, uint32_t* type_idx, const char** name) {
    uint64_t at = (uint64_t)dex->field_ids + 8ull * (idx < dex->field_count ? idx : 0);
    *class_idx = ld16(dex, at);
    *type_idx = ld16(dex, at + 2);
    *name = dex_string(dex, ld32(dex, at + 4));
}
void dex_method(const Dex* dex, uint32_t idx, uint32_t* class_idx, uint32_t* proto_idx, const char** name) {
    uint64_t at = (uint64_t)dex->method_ids + 8ull * (idx < dex->method_count ? idx : 0);
    *class_idx = ld16(dex, at);
    *proto_idx = ld16(dex, at + 2);
    *name = dex_string(dex, ld32(dex, at + 4));
}
uint32_t dex_proto_return(const Dex* dex, uint32_t proto_idx) {
    return ld32(dex, (uint64_t)dex->proto_ids + 12ull * proto_idx + 4);
}
uint32_t dex_type_list_count(const Dex* dex, uint32_t offset) {
    uint32_t count = offset ? ld32(dex, offset) : 0;
    return (uint64_t)offset + 4 + 2ull * count <= dex->size ? count : 0;
}
uint32_t dex_type_list_item(const Dex* dex, uint32_t offset, uint32_t i) {
    return ld16(dex, (uint64_t)offset + 4 + 2ull * i);
}
uint32_t dex_proto_param_count(const Dex* dex, uint32_t proto_idx) {
    return dex_type_list_count(dex, ld32(dex, (uint64_t)dex->proto_ids + 12ull * proto_idx + 8));
}
uint32_t dex_proto_param(const Dex* dex, uint32_t proto_idx, uint32_t i) {
    return dex_type_list_item(dex, ld32(dex, (uint64_t)dex->proto_ids + 12ull * proto_idx + 8), i);
}

DexClassDef dex_class_def(const Dex* dex, uint32_t i) {
    uint64_t at = (uint64_t)dex->class_defs + 32ull * i;
    return (DexClassDef){ld32(dex, at),      ld32(dex, at + 4),  ld32(dex, at + 8),  ld32(dex, at + 12),
                         ld32(dex, at + 16), ld32(dex, at + 20), ld32(dex, at + 24), ld32(dex, at + 28)};
}
bool dex_find_class(const Dex* dex, const char* descriptor, uint32_t* index) {
    uint32_t low = 0, high = dex->sorted ? dex->class_count : 0;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        int order = strcmp(class_descriptor(dex, dex->sorted[mid]), descriptor);
        if (!order) {
            *index = dex->sorted[mid];
            return true;
        }
        if (order < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return false;
}
bool dex_class_data(const Dex* dex, const DexClassDef* def, DexClassData* out) {
    *out = (DexClassData){0};
    if (!def->class_data_off) return true;
    if (def->class_data_off >= dex->size) return false;
    const unsigned char *p = dex->data + def->class_data_off, *end = dex->data + dex->size;
    out->static_count = dex_uleb(&p, end);
    out->instance_count = dex_uleb(&p, end);
    out->direct_count = dex_uleb(&p, end);
    out->virtual_count = dex_uleb(&p, end);
    uint64_t fields = (uint64_t)out->static_count + out->instance_count,
             methods = (uint64_t)out->direct_count + out->virtual_count;
    /* each entry takes at least two bytes (three for a method) */
    if (fields * 2 + methods * 3 > (uint64_t)(end - p)) return false;
    out->fields = calloc(fields + 1, sizeof *out->fields);
    out->methods = calloc(methods + 1, sizeof *out->methods);
    if (!out->fields || !out->methods) {
        dex_class_data_free(out);
        return false;
    }
    /* each list's indices are deltas from the one before it, from 0 at the list's start */
    for (uint64_t i = 0, idx = 0; i < fields; ++i) {
        if (i == out->static_count) idx = 0;
        idx += dex_uleb(&p, end);
        out->fields[i] = (DexField){(uint32_t)idx, dex_uleb(&p, end)};
        if (idx >= dex->field_count) {
            dex_class_data_free(out);
            return false;
        }
    }
    for (uint64_t i = 0, idx = 0; i < methods; ++i) {
        if (i == out->direct_count) idx = 0;
        idx += dex_uleb(&p, end);
        uint32_t access = dex_uleb(&p, end);
        out->methods[i] = (DexMethod){(uint32_t)idx, access, dex_uleb(&p, end)};
        if (idx >= dex->method_count) {
            dex_class_data_free(out);
            return false;
        }
    }
    return true;
}
void dex_class_data_free(DexClassData* data) {
    free(data->fields);
    free(data->methods);
    *data = (DexClassData){0};
}
bool dex_code(const Dex* dex, uint32_t offset, DexCode* code) {
    *code = (DexCode){0};
    if ((uint64_t)offset + 16 > dex->size) return false;
    const unsigned char* p = dex->data + offset;
    code->registers = snx_ld_le16(p);
    code->ins = snx_ld_le16(p + 2);
    code->outs = snx_ld_le16(p + 4);
    code->tries = snx_ld_le16(p + 6);
    code->insns_size = snx_ld_le32(p + 12);
    uint64_t insns = (uint64_t)offset + 16, end = insns + 2ull * code->insns_size;
    if (end > dex->size || code->ins > code->registers) return false;
    code->insns = dex->data + insns;
    if (code->tries) {
        uint64_t tries = end + (code->insns_size & 1 ? 2 : 0); /* padded to four bytes */
        if (tries + 8ull * code->tries > dex->size) return false;
        code->try_items = dex->data + tries;
        code->handlers = dex->data + tries + 8ull * code->tries;
    }
    return true;
}

DexValue dex_read_value(const unsigned char** p, const unsigned char* end) {
    DexValue v = {DEX_VALUE_NULL, 0};
    if (*p >= end) return v;
    unsigned head = *(*p)++, arg = head >> 5;
    v.type = head & 0x1f;
    switch (v.type) {
    case DEX_VALUE_NULL: return v;
    case DEX_VALUE_BOOLEAN: v.bits = arg; return v;
    case DEX_VALUE_ARRAY:
    case DEX_VALUE_ANNOTATION: return v;
    default: break;
    }
    unsigned size = arg + 1;
    if ((size_t)(end - *p) < size) {
        *p = end;
        return v;
    }
    uint64_t bits = 0;
    for (unsigned i = 0; i < size; ++i) bits |= (uint64_t)(*p)[i] << (8 * i);
    *p += size;
    switch (v.type) {
    case DEX_VALUE_BYTE:
    case DEX_VALUE_SHORT:
    case DEX_VALUE_INT:
    case DEX_VALUE_LONG:
        /* sign-extended from its top byte */
        if (size < 8 && (bits >> (8 * size - 1)) & 1) bits |= ~(uint64_t)0 << (8 * size);
        break;
    case DEX_VALUE_FLOAT: bits <<= 8 * (4 - size); break;  /* zero-extended to the right */
    case DEX_VALUE_DOUBLE: bits <<= 8 * (8 - size); break; /* the same, to 64 bits */
    default: break;
    }
    v.bits = bits;
    return v;
}
void dex_skip_value(const unsigned char** p, const unsigned char* end) {
    DexValue v = dex_read_value(p, end);
    if (v.type == DEX_VALUE_ARRAY) {
        for (uint32_t n = dex_uleb(p, end); n && *p < end; --n) dex_skip_value(p, end);
    } else if (v.type == DEX_VALUE_ANNOTATION) {
        dex_uleb(p, end); /* its type */
        for (uint32_t n = dex_uleb(p, end); n && *p < end; --n) {
            dex_uleb(p, end); /* the element's name */
            dex_skip_value(p, end);
        }
    }
}
