/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DEX annotations as JVM attributes (annotations.h). The formats are AOSP's dex-format documentation and the JVM
   specification's 4.7.16 to 4.7.22 */
#include "android/dex/annotations.h"
#include "common/utilities/endian.h"
#include <stdbool.h>
#include <string.h>

enum { VISIBILITY_RUNTIME = 1, VISIBILITY_SYSTEM = 2 };

static uint32_t u4(const Dex* dex, uint32_t off) {
    return (uint64_t)off + 4 <= dex->size ? snx_ld_le32(dex->data + off) : 0;
}

void dex_annotations(const Dex* dex, uint32_t off, DexAnnotations* out) {
    *out = (DexAnnotations){0};
    out->dex = dex;
    if (!off || (uint64_t)off + 16 > dex->size) return;
    out->class_set = u4(dex, off);
    out->fields = u4(dex, off + 4);
    out->methods = u4(dex, off + 8);
    out->parameters = u4(dex, off + 12);
    out->lists = off + 16;
    uint64_t end = (uint64_t)out->lists + 8ull * ((uint64_t)out->fields + out->methods + out->parameters);
    if (end > dex->size) out->fields = out->methods = out->parameters = 0;
}

/* an (index, offset) list sorted by index: the offset of index, 0 for none */
static uint32_t find(const Dex* dex, uint32_t list, uint32_t count, uint32_t index) {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, at = u4(dex, list + 8 * mid);
        if (at == index) return u4(dex, list + 8 * mid + 4);
        if (at < index) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}
uint32_t dex_field_annotations(const DexAnnotations* a, uint32_t field_idx) {
    return find(a->dex, a->lists, a->fields, field_idx);
}
uint32_t dex_method_annotations(const DexAnnotations* a, uint32_t method_idx) {
    return find(a->dex, a->lists + 8 * a->fields, a->methods, method_idx);
}
uint32_t dex_parameter_annotations(const DexAnnotations* a, uint32_t method_idx) {
    return find(a->dex, a->lists + 8 * (a->fields + a->methods), a->parameters, method_idx);
}

/* ---- element values ---- */
static bool write_annotation(CfPool* pool, CfBuffer* out, const Dex* dex, const unsigned char** p,
                             const unsigned char* end);

/* one encoded_value as an element_value; false for a value an element cannot hold (null, a method), which is
   skipped */
static bool write_element(CfPool* pool, CfBuffer* out, const Dex* dex, const unsigned char** p,
                          const unsigned char* end) {
    const unsigned char* at = *p;
    DexValue v = dex_read_value(p, end);
    switch (v.type) {
    case DEX_VALUE_BYTE: cf_u1(out, 'B'); cf_u2(out, cf_integer(pool, (uint32_t)v.bits)); return true;
    case DEX_VALUE_SHORT: cf_u1(out, 'S'); cf_u2(out, cf_integer(pool, (uint32_t)v.bits)); return true;
    case DEX_VALUE_CHAR: cf_u1(out, 'C'); cf_u2(out, cf_integer(pool, (uint32_t)v.bits)); return true;
    case DEX_VALUE_INT: cf_u1(out, 'I'); cf_u2(out, cf_integer(pool, (uint32_t)v.bits)); return true;
    case DEX_VALUE_BOOLEAN: cf_u1(out, 'Z'); cf_u2(out, cf_integer(pool, (uint32_t)v.bits)); return true;
    case DEX_VALUE_LONG: cf_u1(out, 'J'); cf_u2(out, cf_long(pool, v.bits)); return true;
    case DEX_VALUE_FLOAT: cf_u1(out, 'F'); cf_u2(out, cf_float(pool, (uint32_t)v.bits)); return true;
    case DEX_VALUE_DOUBLE: cf_u1(out, 'D'); cf_u2(out, cf_double(pool, v.bits)); return true;
    case DEX_VALUE_STRING: cf_u1(out, 's'); cf_u2(out, cf_utf8(pool, dex_string(dex, (uint32_t)v.bits))); return true;
    case DEX_VALUE_TYPE: cf_u1(out, 'c'); cf_u2(out, cf_utf8(pool, dex_type(dex, (uint32_t)v.bits))); return true;
    case DEX_VALUE_ENUM: {
        uint32_t class_idx, type_idx;
        const char* name;
        dex_field(dex, (uint32_t)v.bits, &class_idx, &type_idx, &name);
        cf_u1(out, 'e');
        cf_u2(out, cf_utf8(pool, dex_type(dex, type_idx)));
        cf_u2(out, cf_utf8(pool, name));
        return true;
    }
    case DEX_VALUE_ARRAY: {
        uint32_t n = dex_uleb(p, end);
        cf_u1(out, '[');
        size_t count_at = out->size;
        cf_u2(out, 0);
        unsigned written = 0;
        for (uint32_t i = 0; i < n && *p < end; ++i)
            if (write_element(pool, out, dex, p, end)) ++written;
        cf_put_u2(out, count_at, written);
        return true;
    }
    case DEX_VALUE_ANNOTATION:
        cf_u1(out, '@');
        return write_annotation(pool, out, dex, p, end);
    default:
        *p = at;
        dex_skip_value(p, end);
        return false;
    }
}

/* an encoded_annotation at *p as an annotation structure; its elements that cannot be one are left out */
static bool write_annotation(CfPool* pool, CfBuffer* out, const Dex* dex, const unsigned char** p,
                             const unsigned char* end) {
    uint32_t type = dex_uleb(p, end), n = dex_uleb(p, end);
    cf_u2(out, cf_utf8(pool, dex_type(dex, type)));
    size_t count_at = out->size;
    cf_u2(out, 0);
    unsigned written = 0;
    for (uint32_t i = 0; i < n && *p < end; ++i) {
        uint32_t name = dex_uleb(p, end);
        size_t mark = out->size;
        cf_u2(out, cf_utf8(pool, dex_string(dex, name)));
        if (write_element(pool, out, dex, p, end)) ++written;
        else out->size = mark;
    }
    cf_put_u2(out, count_at, written);
    return true;
}

/* the annotation_item at off: its visibility, *p at its encoded_annotation */
static unsigned annotation_at(const Dex* dex, uint32_t off, const unsigned char** p) {
    if (off >= dex->size) {
        *p = dex->data + dex->size;
        return 0;
    }
    *p = dex->data + off + 1;
    return dex->data[off];
}

/* the runtime-visible annotations of a set, as the body of a RuntimeVisibleAnnotations (or one parameter's) */
static unsigned write_visible(CfPool* pool, CfBuffer* out, const Dex* dex, uint32_t set_off) {
    const unsigned char* end = dex->data + dex->size;
    uint32_t n = set_off ? u4(dex, set_off) : 0;
    size_t count_at = out->size;
    cf_u2(out, 0);
    unsigned written = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const unsigned char* p;
        if (annotation_at(dex, u4(dex, set_off + 4 + 4 * i), &p) != VISIBILITY_RUNTIME) continue;
        write_annotation(pool, out, dex, &p, end);
        ++written;
    }
    cf_put_u2(out, count_at, written);
    return written;
}

/* the system annotation of a type in a set: *p at its elements' count, false for none */
static bool system_annotation(const Dex* dex, uint32_t set_off, const char* type, const unsigned char** p) {
    const unsigned char* end = dex->data + dex->size;
    uint32_t n = set_off ? u4(dex, set_off) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (annotation_at(dex, u4(dex, set_off + 4 + 4 * i), p) != VISIBILITY_SYSTEM) continue;
        if (!strcmp(dex_type(dex, dex_uleb(p, end)), type)) return true;
    }
    return false;
}

/* an attribute's header, its length patched by end_attribute */
static size_t begin_attribute(CfPool* pool, CfBuffer* out, const char* name) {
    cf_u2(out, cf_utf8(pool, name));
    size_t at = out->size;
    cf_u4(out, 0);
    return at;
}
static void end_attribute(CfBuffer* out, size_t at) {
    cf_put_u4(out, at, (uint32_t)(out->size - at - 4));
}

unsigned cf_annotation_attributes(CfPool* pool, CfBuffer* out, const Dex* dex, uint32_t set_off) {
    if (!set_off) return 0;
    unsigned count = 0;
    size_t mark = out->size, at = begin_attribute(pool, out, "RuntimeVisibleAnnotations");
    if (write_visible(pool, out, dex, set_off)) {
        end_attribute(out, at);
        ++count;
    } else {
        out->size = mark;
    }
    /* dalvik.annotation.Signature { String[] value }: the parts of one signature */
    const unsigned char *p, *end = dex->data + dex->size;
    if (system_annotation(dex, set_off, "Ldalvik/annotation/Signature;", &p)) {
        CfBuffer text = {0};
        for (uint32_t n = dex_uleb(&p, end); n && p < end; --n) {
            dex_uleb(&p, end); /* the element's name, value */
            DexValue v = dex_read_value(&p, end);
            if (v.type != DEX_VALUE_ARRAY) {
                p = end;
                break;
            }
            for (uint32_t k = dex_uleb(&p, end); k && p < end; --k) {
                DexValue s = dex_read_value(&p, end);
                if (s.type == DEX_VALUE_STRING) {
                    const char* part = dex_string(dex, (uint32_t)s.bits);
                    cf_bytes(&text, part, strlen(part));
                }
            }
        }
        if (text.size && !text.failed) {
            at = begin_attribute(pool, out, "Signature");
            cf_u2(out, cf_utf8_n(pool, (const char*)text.data, text.size));
            end_attribute(out, at);
            ++count;
        }
        cf_free(&text);
    }
    return count;
}

unsigned cf_parameter_attributes(CfPool* pool, CfBuffer* out, const Dex* dex, uint32_t list_off, unsigned params) {
    uint32_t n = list_off ? u4(dex, list_off) : 0;
    if (!n || (uint64_t)list_off + 4 + 4ull * n > dex->size) return 0;
    /* the list covers the DEX method's parameters; the descriptor's count is the JVM's */
    if (n > params) n = params;
    size_t mark = out->size, at = begin_attribute(pool, out, "RuntimeVisibleParameterAnnotations");
    cf_u1(out, params);
    unsigned any = 0;
    for (unsigned i = 0; i < params; ++i) any += write_visible(pool, out, dex, i < n ? u4(dex, list_off + 4 + 4 * i) : 0);
    if (!any) {
        out->size = mark;
        return 0;
    }
    end_attribute(out, at);
    return 1;
}

unsigned cf_annotation_default(CfPool* pool, CfBuffer* out, const Dex* dex, uint32_t class_set, const char* name) {
    const unsigned char *p, *end = dex->data + dex->size;
    if (!system_annotation(dex, class_set, "Ldalvik/annotation/AnnotationDefault;", &p)) return 0;
    /* AnnotationDefault { Annotation value }: an annotation of the class's type with each default */
    for (uint32_t n = dex_uleb(&p, end); n && p < end; --n) {
        dex_uleb(&p, end);
        DexValue v = dex_read_value(&p, end);
        if (v.type != DEX_VALUE_ANNOTATION) return 0;
        dex_uleb(&p, end); /* its type */
        for (uint32_t k = dex_uleb(&p, end); k && p < end; --k) {
            const char* element = dex_string(dex, dex_uleb(&p, end));
            if (strcmp(element, name)) {
                dex_skip_value(&p, end);
                continue;
            }
            size_t mark = out->size, at = begin_attribute(pool, out, "AnnotationDefault");
            if (!write_element(pool, out, dex, &p, end)) {
                out->size = mark;
                return 0;
            }
            end_attribute(out, at);
            return 1;
        }
        return 0;
    }
    return 0;
}
