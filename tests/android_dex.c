/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DEX reader's encoded values (android/dex/dex.c) where a file says what no compiler writes: a value of arrays
   inside arrays is skipped to its own end, through DEX_VALUE_DEPTH levels and no further, and one of four million
   levels, each a call's frame for a reader that follows them all, is no value: what is left of the item goes with it */
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

int main(void) {
    /* an array of an int, an empty array and an annotation (type 5) of one element (name 9), a boolean */
    static const unsigned char value[] = {
        DEX_VALUE_ARRAY,   3,   DEX_VALUE_INT, 7, DEX_VALUE_ARRAY, 0, DEX_VALUE_ANNOTATION, 5, 1, 9,
        DEX_VALUE_BOOLEAN, 0xAA};
    const unsigned char* p = value;
    unsigned char* bytes = malloc((size_t)DEEP * ARRAY_OF_ONE + 3);
    assert(bytes);
    dex_skip_value(&p, value + sizeof value);
    assert(p == value + sizeof value - 1);
    assert(skipped(bytes, 1) == 1 * ARRAY_OF_ONE + 2);
    assert(skipped(bytes, DEX_VALUE_DEPTH) == DEX_VALUE_DEPTH * ARRAY_OF_ONE + 2);
    assert(skipped(bytes, DEX_VALUE_DEPTH + 1) == (DEX_VALUE_DEPTH + 1) * ARRAY_OF_ONE + 3);
    assert(skipped(bytes, DEEP) == (size_t)DEEP * ARRAY_OF_ONE + 3);
    free(bytes);
    return 0;
}
