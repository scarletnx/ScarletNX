/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/res/axml.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

enum {
    CHUNK_STRING_POOL = 0x0001,
    CHUNK_XML = 0x0003,
    CHUNK_START_NAMESPACE = 0x0100,
    CHUNK_END_NAMESPACE = 0x0101,
    CHUNK_START_ELEMENT = 0x0102,
    CHUNK_END_ELEMENT = 0x0103,
    CHUNK_CDATA = 0x0104,
    CHUNK_RESOURCE_MAP = 0x0180
};
#define POOL_UTF8 0x100u

bool res_string_pool(ResStringPool* pool, const unsigned char* chunk, size_t size) {
    *pool = (ResStringPool){0};
    if (size < 28 || snx_ld_le16(chunk) != CHUNK_STRING_POOL) return false;
    uint32_t header = snx_ld_le16(chunk + 2), total = snx_ld_le32(chunk + 4);
    if (total > size || header < 28 || header > total) return false;
    pool->data = chunk;
    pool->size = total;
    pool->count = snx_ld_le32(chunk + 8);
    pool->flags = snx_ld_le32(chunk + 16);
    pool->strings = snx_ld_le32(chunk + 20);
    if ((uint64_t)header + 4ull * pool->count > total || pool->strings > total) {
        *pool = (ResStringPool){0};
        return false;
    }
    return true;
}
/* the start of string idx's bytes, its length prefix included; NULL for a bad index */
static const unsigned char* string_at(const ResStringPool* pool, uint32_t idx) {
    if (!pool->data || idx >= pool->count) return NULL;
    uint32_t header = snx_ld_le16(pool->data + 2);
    uint64_t at = (uint64_t)pool->strings + snx_ld_le32(pool->data + header + 4ull * idx);
    return at + 2 <= pool->size ? pool->data + at : NULL;
}
/* one UTF-8 sequence's code point at *p (end bounds it); U+FFFD for a bad one */
static uint32_t utf8_next(const unsigned char** p, const unsigned char* end) {
    unsigned c = *(*p)++;
    if (c < 0x80) return c;
    unsigned extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (!extra || (size_t)(end - *p) < extra) return 0xfffd;
    uint32_t cp = c & (0x3fu >> extra);
    for (unsigned i = 0; i < extra; ++i) {
        unsigned b = *(*p)++;
        if ((b & 0xc0) != 0x80) return 0xfffd;
        cp = cp << 6 | (b & 0x3f);
    }
    return cp;
}
static size_t utf8_put(char* out, size_t room, size_t at, uint32_t cp) {
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xc0 | cp >> 6);
        b[1] = (unsigned char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xe0 | cp >> 12);
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        b[2] = (unsigned char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xf0 | cp >> 18);
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        b[3] = (unsigned char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    for (size_t i = 0; i < n; ++i)
        if (at + i + 1 < room) out[at + i] = (char)b[i];
    return at + n;
}
/* a UTF-8 string's bytes and their count, past its two length prefixes */
static const unsigned char* utf8_bytes(const ResStringPool* pool, const unsigned char* s, size_t* length) {
    const unsigned char* end = pool->data + pool->size;
    unsigned skip = s[0] & 0x80 ? 2 : 1; /* its length in UTF-16 units */
    s += skip;
    if (s + 2 > end) return NULL;
    size_t n = s[0];
    if (n & 0x80) {
        n = (n & 0x7f) << 8 | s[1];
        s += 2;
    } else {
        s += 1;
    }
    if (s + n > end) return NULL;
    *length = n;
    return s;
}
static const unsigned char* utf16_units(const ResStringPool* pool, const unsigned char* s, size_t* length) {
    const unsigned char* end = pool->data + pool->size;
    size_t n = snx_ld_le16(s);
    s += 2;
    if (n & 0x8000) {
        if (s + 2 > end) return NULL;
        n = (n & 0x7fff) << 16 | snx_ld_le16(s);
        s += 2;
    }
    if (s + 2 * n > end) return NULL;
    *length = n;
    return s;
}
long res_string(const ResStringPool* pool, uint32_t idx, char* out, size_t out_size) {
    const unsigned char* s = string_at(pool, idx);
    if (!s) return -1;
    size_t n, at = 0;
    if (pool->flags & POOL_UTF8) {
        const unsigned char* bytes = utf8_bytes(pool, s, &n);
        if (!bytes) return -1;
        if (out_size) {
            size_t copy = n < out_size - 1 ? n : out_size - 1;
            memcpy(out, bytes, copy);
            out[copy] = 0;
        }
        return (long)n;
    }
    const unsigned char* units = utf16_units(pool, s, &n);
    if (!units) return -1;
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = snx_ld_le16(units + 2 * i);
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n) {
            uint32_t low = snx_ld_le16(units + 2 * (i + 1));
            if (low >= 0xdc00 && low < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                ++i;
            }
        }
        at = utf8_put(out, out_size, at, cp);
    }
    if (out_size) out[at < out_size ? at : out_size - 1] = 0;
    return (long)at;
}
long res_string16(const ResStringPool* pool, uint32_t idx, uint16_t* out, size_t out_size) {
    const unsigned char* s = string_at(pool, idx);
    if (!s) return -1;
    size_t n, count = 0;
    if (pool->flags & POOL_UTF8) {
        const unsigned char* p = utf8_bytes(pool, s, &n);
        if (!p) return -1;
        const unsigned char* end = p + n;
        while (p < end) {
            uint32_t cp = utf8_next(&p, end);
            if (cp >= 0x10000) {
                if (count < out_size) out[count] = (uint16_t)(0xd800 + ((cp - 0x10000) >> 10));
                if (count + 1 < out_size) out[count + 1] = (uint16_t)(0xdc00 + ((cp - 0x10000) & 0x3ff));
                count += 2;
            } else {
                if (count < out_size) out[count] = (uint16_t)cp;
                ++count;
            }
        }
        return (long)count;
    }
    const unsigned char* units = utf16_units(pool, s, &n);
    if (!units) return -1;
    for (size_t i = 0; i < n && i < out_size; ++i) out[i] = (uint16_t)snx_ld_le16(units + 2 * i);
    return (long)n;
}

bool axml_open(AxmlDocument* doc, const unsigned char* data, size_t size, char* error, size_t error_size) {
    *doc = (AxmlDocument){0};
    if (size < 8 || snx_ld_le16(data) != CHUNK_XML || snx_ld_le32(data + 4) > size) {
        snprintf(error, error_size, "not binary XML");
        return false;
    }
    doc->data = data;
    doc->size = snx_ld_le32(data + 4);
    for (uint32_t at = snx_ld_le16(data + 2); at + 8 <= doc->size;) {
        uint32_t type = snx_ld_le16(data + at), chunk = snx_ld_le32(data + at + 4);
        if (chunk < 8 || chunk > doc->size - at) break;
        if (type == CHUNK_STRING_POOL) {
            if (!res_string_pool(&doc->strings, data + at, chunk)) {
                snprintf(error, error_size, "a bad string pool");
                return false;
            }
        } else if (type == CHUNK_RESOURCE_MAP) {
            uint32_t header = snx_ld_le16(data + at + 2);
            doc->ids = data + at + header;
            doc->id_count = (chunk - header) / 4;
        } else if (type >= CHUNK_START_NAMESPACE && type <= CHUNK_CDATA) {
            doc->first = at;
            return true;
        }
        at += chunk;
    }
    snprintf(error, error_size, "binary XML with no elements");
    return false;
}
void axml_parser(AxmlParser* p, const AxmlDocument* doc) {
    *p = (AxmlParser){doc, 0, AXML_START_DOCUMENT, 0};
}
static uint32_t chunk_type(const AxmlParser* p) {
    return p->at && p->at + 16 <= p->doc->size ? snx_ld_le16(p->doc->data + p->at) : 0;
}
int axml_next(AxmlParser* p) {
    const AxmlDocument* doc = p->doc;
    if (p->event == AXML_END_DOCUMENT) return AXML_END_DOCUMENT;
    if (p->event == AXML_END_TAG) --p->depth;
    for (;;) {
        uint32_t next = p->at ? p->at + snx_ld_le32(doc->data + p->at + 4) : doc->first;
        if (p->at && next <= p->at) next = (uint32_t)doc->size; /* a chunk of size 0 */
        p->at = next;
        if (next + 16 > doc->size || snx_ld_le32(doc->data + next + 4) < 16 ||
            snx_ld_le32(doc->data + next + 4) > doc->size - next) {
            p->at = 0;
            return p->event = AXML_END_DOCUMENT;
        }
        switch (snx_ld_le16(doc->data + next)) {
        case CHUNK_START_ELEMENT: ++p->depth; return p->event = AXML_START_TAG;
        case CHUNK_END_ELEMENT: return p->event = AXML_END_TAG;
        case CHUNK_CDATA: return p->event = AXML_TEXT;
        default: break; /* a namespace's start or end */
        }
    }
}
/* the node's extension (after its 16-byte header), with need bytes in it; NULL otherwise */
static const unsigned char* ext(const AxmlParser* p, uint32_t need) {
    if (!p->at) return NULL;
    const unsigned char* node = p->doc->data + p->at;
    uint32_t header = snx_ld_le16(node + 2), size = snx_ld_le32(node + 4);
    if (header < 16 || (uint64_t)header + need > size) return NULL;
    return node + header;
}
uint32_t axml_line(const AxmlParser* p) {
    return p->at ? snx_ld_le32(p->doc->data + p->at + 8) : 0;
}
uint32_t axml_name(const AxmlParser* p) {
    unsigned t = chunk_type(p);
    const unsigned char* e = t == CHUNK_START_ELEMENT || t == CHUNK_END_ELEMENT ? ext(p, 8) : NULL;
    return e ? snx_ld_le32(e + 4) : AXML_NONE;
}
uint32_t axml_namespace(const AxmlParser* p) {
    unsigned t = chunk_type(p);
    const unsigned char* e = t == CHUNK_START_ELEMENT || t == CHUNK_END_ELEMENT ? ext(p, 8) : NULL;
    return e ? snx_ld_le32(e) : AXML_NONE;
}
uint32_t axml_text(const AxmlParser* p) {
    const unsigned char* e = chunk_type(p) == CHUNK_CDATA ? ext(p, 4) : NULL;
    return e ? snx_ld_le32(e) : AXML_NONE;
}
/* the start tag's attribute i, NULL for none */
static const unsigned char* attribute(const AxmlParser* p, uint32_t i) {
    const unsigned char* e = chunk_type(p) == CHUNK_START_ELEMENT ? ext(p, 20) : NULL;
    if (!e) return NULL;
    uint32_t start = snx_ld_le16(e + 8), stride = snx_ld_le16(e + 10), count = snx_ld_le16(e + 12);
    if (i >= count || stride < 20) return NULL;
    const unsigned char* a = e + start + (size_t)stride * i;
    const unsigned char* end = p->doc->data + p->at + snx_ld_le32(p->doc->data + p->at + 4);
    return a + 20 <= end ? a : NULL;
}
uint32_t axml_attribute_count(const AxmlParser* p) {
    const unsigned char* e = chunk_type(p) == CHUNK_START_ELEMENT ? ext(p, 20) : NULL;
    if (!e) return 0;
    uint32_t count = snx_ld_le16(e + 12);
    while (count && !attribute(p, count - 1)) --count;
    return count;
}
uint32_t axml_attribute_namespace(const AxmlParser* p, uint32_t i) {
    const unsigned char* a = attribute(p, i);
    return a ? snx_ld_le32(a) : AXML_NONE;
}
uint32_t axml_attribute_name(const AxmlParser* p, uint32_t i) {
    const unsigned char* a = attribute(p, i);
    return a ? snx_ld_le32(a + 4) : AXML_NONE;
}
uint32_t axml_attribute_resource(const AxmlParser* p, uint32_t i) {
    uint32_t name = axml_attribute_name(p, i);
    return name < p->doc->id_count ? snx_ld_le32(p->doc->ids + 4ull * name) : 0;
}
uint32_t axml_attribute_raw(const AxmlParser* p, uint32_t i) {
    const unsigned char* a = attribute(p, i);
    return a ? snx_ld_le32(a + 8) : AXML_NONE;
}
ResValue axml_attribute_value(const AxmlParser* p, uint32_t i) {
    const unsigned char* a = attribute(p, i);
    return a ? (ResValue){a[15], snx_ld_le32(a + 16)} : (ResValue){RES_TYPE_NULL, 0};
}
long axml_find_resource(const AxmlParser* p, uint32_t id) {
    uint32_t count = axml_attribute_count(p);
    for (uint32_t i = 0; i < count; ++i)
        if (axml_attribute_resource(p, i) == id) return (long)i;
    return -1;
}
long axml_find(const AxmlParser* p, const char* ns, const char* name) {
    uint32_t count = axml_attribute_count(p);
    char text[256];
    for (uint32_t i = 0; i < count; ++i) {
        if (res_string(&p->doc->strings, axml_attribute_name(p, i), text, sizeof text) < 0 || strcmp(text, name))
            continue;
        if (!ns) return (long)i;
        uint32_t n = axml_attribute_namespace(p, i);
        if (n == AXML_NONE ? !ns[0]
                           : res_string(&p->doc->strings, n, text, sizeof text) >= 0 && !strcmp(text, ns))
            return (long)i;
    }
    return -1;
}
static long special_attribute(const AxmlParser* p, unsigned offset) {
    const unsigned char* e = chunk_type(p) == CHUNK_START_ELEMENT ? ext(p, 20) : NULL;
    uint32_t index = e ? snx_ld_le16(e + offset) : 0; /* 1-based, 0 for none */
    return index && index <= axml_attribute_count(p) ? (long)index - 1 : -1;
}
long axml_id_attribute(const AxmlParser* p) {
    return special_attribute(p, 14);
}
long axml_class_attribute(const AxmlParser* p) {
    return special_attribute(p, 16);
}
long axml_style_attribute(const AxmlParser* p) {
    return special_attribute(p, 18);
}
