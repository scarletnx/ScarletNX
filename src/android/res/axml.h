/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_AXML_H
#define SNX_ANDROID_AXML_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Android's binary XML (AndroidManifest.xml, compiled layouts and drawables), read in place: the HLE of
   libandroidfw's ResXMLTree and ResXMLParser. The format is AOSP's ResourceTypes.h: a string pool, the attribute
   names' resource IDs, then a chunk for each node. The caller keeps the bytes for the document's life */

/* a string pool chunk (RES_STRING_POOL_TYPE), also resources.arsc's */
typedef struct {
    const unsigned char* data; /* the chunk */
    uint32_t size, count, strings, flags;
} ResStringPool;
/* false for a chunk that is not a string pool or runs past size */
bool res_string_pool(ResStringPool* pool, const unsigned char* chunk, size_t size);
/* string idx as UTF-8 into out (truncated to out_size, always terminated); its full length in bytes, or -1 for a
   bad index */
long res_string(const ResStringPool* pool, uint32_t idx, char* out, size_t out_size);
/* string idx as UTF-16 code units into out (out_size of them at most); their count, or -1 for a bad index */
long res_string16(const ResStringPool* pool, uint32_t idx, uint16_t* out, size_t out_size);

/* Res_value: a typed value */
enum {
    RES_TYPE_NULL = 0x00,
    RES_TYPE_REFERENCE = 0x01,
    RES_TYPE_ATTRIBUTE = 0x02,
    RES_TYPE_STRING = 0x03,
    RES_TYPE_FLOAT = 0x04,
    RES_TYPE_DIMENSION = 0x05,
    RES_TYPE_FRACTION = 0x06,
    RES_TYPE_DYNAMIC_REFERENCE = 0x07,
    RES_TYPE_DYNAMIC_ATTRIBUTE = 0x08,
    RES_TYPE_INT_DEC = 0x10,
    RES_TYPE_INT_HEX = 0x11,
    RES_TYPE_INT_BOOLEAN = 0x12,
    RES_TYPE_INT_COLOR_ARGB8 = 0x1c,
    RES_TYPE_INT_COLOR_RGB8 = 0x1d,
    RES_TYPE_INT_COLOR_ARGB4 = 0x1e,
    RES_TYPE_INT_COLOR_RGB4 = 0x1f
};
typedef struct {
    uint8_t type;
    uint32_t data;
} ResValue;

/* the parser's events, XmlPullParser's numbers */
enum { AXML_START_DOCUMENT = 0, AXML_END_DOCUMENT = 1, AXML_START_TAG = 2, AXML_END_TAG = 3, AXML_TEXT = 4 };
#define AXML_NONE 0xffffffffu /* no string */

typedef struct {
    const unsigned char* data;
    size_t size;
    ResStringPool strings;
    const unsigned char* ids; /* the attribute names' resource IDs, by string index */
    uint32_t id_count;
    uint32_t first;           /* the first node's offset */
} AxmlDocument;
/* false with error filled when data is not binary XML this reads */
bool axml_open(AxmlDocument* doc, const unsigned char* data, size_t size, char* error, size_t error_size);

/* a parser's place in a document. Namespace chunks are passed over */
typedef struct {
    const AxmlDocument* doc;
    uint32_t at;   /* the current node's offset, 0 before the first */
    int event;
    int depth;
} AxmlParser;
void axml_parser(AxmlParser* p, const AxmlDocument* doc);
/* the next event: AXML_START_TAG, AXML_END_TAG, AXML_TEXT or AXML_END_DOCUMENT (also for a bad chunk) */
int axml_next(AxmlParser* p);
uint32_t axml_line(const AxmlParser* p);
/* the current tag's name and namespace (string indices), a text's string */
uint32_t axml_name(const AxmlParser* p);
uint32_t axml_namespace(const AxmlParser* p);
uint32_t axml_text(const AxmlParser* p);
/* the current start tag's attributes */
uint32_t axml_attribute_count(const AxmlParser* p);
uint32_t axml_attribute_name(const AxmlParser* p, uint32_t i);
uint32_t axml_attribute_namespace(const AxmlParser* p, uint32_t i);
/* the resource ID of the attribute's name (android:id is 0x010100d0), 0 for none */
uint32_t axml_attribute_resource(const AxmlParser* p, uint32_t i);
/* its raw string (AXML_NONE for none) and its typed value */
uint32_t axml_attribute_raw(const AxmlParser* p, uint32_t i);
ResValue axml_attribute_value(const AxmlParser* p, uint32_t i);
/* the index of the attribute whose name's resource ID is id, or of name in namespace ns (NULL for any); -1 for none */
long axml_find_resource(const AxmlParser* p, uint32_t id);
long axml_find(const AxmlParser* p, const char* ns, const char* name);
/* the start tag's id, class and style attributes' indices, -1 for none */
long axml_id_attribute(const AxmlParser* p);
long axml_class_attribute(const AxmlParser* p);
long axml_style_attribute(const AxmlParser* p);
#endif
