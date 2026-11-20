/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DEX reader's encoded values (android/dex/dex.c, annotations.c) where a file says what no compiler writes: a
   value of arrays inside arrays is skipped to its own end, through DEX_VALUE_DEPTH levels and no further, and one of
   four million levels, each a call's frame for a reader that follows them all, is no value: what is left of the item
   goes with it. An annotation's element of such arrays becomes element values through the same levels, the array
   past them left empty */
#include "android/dex/annotations.h"
#include "android/dex/dex.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum { ARRAY_OF_ONE = 2, DEEP = 4000000 }; /* an array's header and its count of one, in bytes; levels */

/* levels arrays of one inside one another, an int in the last, and a byte after the value: where a skip of it ends */
static size_t skipped(unsigned char* bytes, size_t levels) {
    const unsigned char *p = bytes, *end = bytes + levels * ARRAY_OF_ONE + 3;
    for (size_t i = 0; i < levels; ++i) bytes[i * ARRAY_OF_ONE] = DEX_VALUE_ARRAY, bytes[i * ARRAY_OF_ONE + 1] = 1;
    memcpy(bytes + levels * ARRAY_OF_ONE, (const unsigned char[]){DEX_VALUE_INT, 7, 0xAA}, 3);
    dex_skip_value(&p, end);
    return (size_t)(p - bytes);
}

/* A file whose one class has one runtime-visible annotation of one element, levels arrays of one inside one another
   with an int in the last: the bytes of its RuntimeVisibleAnnotations attribute, 0 when there is none. The file is
   no whole DEX: the annotation set at 4, its item at 12, and no strings or types, whose names read as "" */
static size_t annotation_bytes(unsigned char* bytes, size_t levels, unsigned char last[3]) {
    enum { SET = 4, ITEM = 12, VALUE = ITEM + 4 };
    Dex dex = {.data = bytes, .size = VALUE + levels * ARRAY_OF_ONE + 2};
    CfPool pool = {0};
    CfBuffer out = {0};
    size_t size = 0;
    memset(bytes, 0, VALUE);
    bytes[SET] = 1, bytes[SET + 4] = ITEM; /* one annotation, its item's offset */
    bytes[ITEM] = 1;                       /* runtime-visible; then type 0, one element, name 0 */
    bytes[ITEM + 2] = 1;
    for (size_t i = 0; i < levels; ++i)
        bytes[VALUE + i * ARRAY_OF_ONE] = DEX_VALUE_ARRAY, bytes[VALUE + i * ARRAY_OF_ONE + 1] = 1;
    memcpy(bytes + VALUE + levels * ARRAY_OF_ONE, (const unsigned char[]){DEX_VALUE_INT, 7}, 2);
    if (cf_annotation_attributes(&pool, &out, &dex, SET) == 1 && !out.failed && out.size >= 9) {
        size = out.size - 6; /* past the attribute's name and length */
        memcpy(last, out.data + out.size - 3, 3);
    }
    cf_free(&out);
    cf_pool_free(&pool);
    return size;
}

int main(void) {
    /* an array of an int, an empty array and an annotation (type 5) of one element (name 9), a boolean */
    static const unsigned char value[] = {
        DEX_VALUE_ARRAY,   3,   DEX_VALUE_INT, 7, DEX_VALUE_ARRAY, 0, DEX_VALUE_ANNOTATION, 5, 1, 9,
        DEX_VALUE_BOOLEAN, 0xAA};
    const unsigned char* p = value;
    unsigned char* bytes = malloc((size_t)DEEP * ARRAY_OF_ONE + 32);
    assert(bytes);
    dex_skip_value(&p, value + sizeof value);
    assert(p == value + sizeof value - 1);
    assert(skipped(bytes, 1) == 1 * ARRAY_OF_ONE + 2);
    assert(skipped(bytes, DEX_VALUE_DEPTH) == DEX_VALUE_DEPTH * ARRAY_OF_ONE + 2);
    assert(skipped(bytes, DEX_VALUE_DEPTH + 1) == (DEX_VALUE_DEPTH + 1) * ARRAY_OF_ONE + 3);
    assert(skipped(bytes, DEEP) == (size_t)DEEP * ARRAY_OF_ONE + 3);
    /* the attribute: its count, the annotation's type and count, the element's name, then '[' and a count for each
       array and 'I' and a constant for the int. Past the levels the array an element holds is empty */
    unsigned char last[3];
    assert(annotation_bytes(bytes, 2, last) == 8 + 2 * 3 + 3 && last[0] == 'I');
    assert(annotation_bytes(bytes, DEX_VALUE_DEPTH, last) == 8 + DEX_VALUE_DEPTH * 3 + 3 && last[0] == 'I');
    assert(annotation_bytes(bytes, DEX_VALUE_DEPTH + 1, last) == 8 + DEX_VALUE_DEPTH * 3);
    assert(last[0] == '[' && last[1] == 0 && last[2] == 0);
    assert(annotation_bytes(bytes, DEEP, last) == 8 + DEX_VALUE_DEPTH * 3 && last[0] == '[' && last[2] == 0);
    free(bytes);
    return 0;
}
