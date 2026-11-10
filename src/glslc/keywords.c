/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A name's keyword, by the perfect hash tools/glslc_kwgen makes of builtins.c's table when glslc is built
   (glslc_keywords.inc in the build folder): one slot, then the name's length and bytes. */
#include "glslc/internal.h"
#include <string.h>
#include "glslc_keywords.inc"

const GlslcKeyword* glslc_find_keyword_hashed(const char* text, uint32_t len, uint32_t h) {
    uint32_t displacement = glslc_keyword_displacements[h & (GLSLC_KEYWORD_BUCKETS - 1u)];
    uint32_t k = glslc_keyword_slots[glslc_keyword_slot(h, displacement, GLSLC_KEYWORD_SLOTS - 1u)];
    if (!k) return NULL;
    const GlslcKeyword* keyword = &glslc_keywords[k - 1];
    return keyword->len == len && !memcmp(keyword->name, text, len) ? keyword : NULL;
}

const GlslcKeyword* glslc_find_keyword(const char* text, uint32_t len) {
    char padded[GLSLC_KEYWORD_LONGEST + 8] = {0}; /* glslc_hash reads past the text */
    if (!len || len > GLSLC_KEYWORD_LONGEST) return NULL;
    memcpy(padded, text, len);
    return glslc_find_keyword_hashed(text, len, glslc_hash(padded, len));
}
