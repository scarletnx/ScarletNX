/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::ngc, native (ngc.hook): the profanity filter, with no ngc:u session and no word lists, so nothing is found in a
   text and a masked text comes back as it was. The behavior is that of a clean-room spec of the SDK's functions,
   never the SDK's code: the checks of the arguments and their Results, which the game sees, are the SDK's, and the
   work that the service did is that of an empty list.

   From SDK 20 the filter asks the ngc:u service, and the hooks hold for those SDKs. An older SDK (7.7.0) filters in
   its own code, on the NgWord system archive (hle/filesystem/fs.c), and keeps its own code. The China functions
   (ngct:u) are not here: a game that calls them opens ngct:u, and sm stops it with the name of this layer.

   The game owns the ProfanityFilter object: the flag of the desired language at 8, initialized at 9, the skip, mask
   and normalization modes at 0xc, 0x10 and 0x14 (their setters run the SDK's own code), the work buffer and its size
   at 0x18 and 0x20, and the heap the SDK builds in it at 0x28, which no code reads with every user of it hooked */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "common/utilities/endian.h"
#include <string.h>

/* the Results of the ngc module (146) */
#define NGC_NOT_INITIALIZED     0x292u /* 146-1 */
#define NGC_ALREADY_INITIALIZED 0x492u /* 146-2 */
#define NGC_NULL                0x692u /* 146-3 */
#define NGC_SIZE                0x892u /* 146-4: a work buffer too small, or too many words */
#define NGC_ENCODING            0xa92u /* 146-5: a text that is not valid UTF-8 or UTF-16 */

#define NGC_WORK_MIN       0x20000u
#define NGC_WORDS_MAX      0x10u
#define NGC_CONTENT_VERSION 1u /* there are no lists here, and reden answers 1 for its own empty ones */

#define OBJ_LANGUAGE    0x08
#define OBJ_INITIALIZED 0x09
#define OBJ_MODES       0x0c /* to 0x18 */
#define OBJ_WORK        0x18
#define OBJ_WORK_SIZE   0x20
#define OBJ_HEAP        0x28

/* a NUL-terminated UTF-8 text the SDK takes: each lead byte with the continuation bytes it calls for. Overlong forms
   and surrogates are not refused, which the spec leaves open */
static int utf8_ok(const CPU* c, u64 a) {
    for (;;) {
        u8 b = *hook_host(c, a++);
        if (!b) return 1;
        int more = b < 0x80 ? 0 : (b & 0xe0) == 0xc0 ? 1 : (b & 0xf0) == 0xe0 ? 2 : (b & 0xf8) == 0xf0 ? 3 : -1;
        if (more < 0) return 0;
        while (more--)
            if ((*hook_host(c, a++) & 0xc0) != 0x80) return 0;
    }
}
/* a NUL-terminated UTF-16 text: each high surrogate followed by a low one, and no low one alone */
static int utf16_ok(const CPU* c, u64 a) {
    for (;;) {
        u16 u = snx_ld_le16(hook_host(c, a));
        a += 2;
        if (!u) return 1;
        if (u >= 0xdc00 && u <= 0xdfff) return 0;
        if (u >= 0xd800 && u <= 0xdbff) {
            u16 low = snx_ld_le16(hook_host(c, a));
            a += 2;
            if (low < 0xdc00 || low > 0xdfff) return 0;
        }
    }
}
static int initialized(const CPU* c, u64 self) { return *hook_host(c, self + OBJ_INITIALIZED) != 0; }

u32 nnngc_initialize(CPU* c, u64 self, u64 work, u64 size, int language) {
    if (!work) return NGC_NULL;
    if (size < NGC_WORK_MIN) return NGC_SIZE;
    if (initialized(c, self)) return NGC_ALREADY_INITIALIZED;
    u8* o = hook_host(c, self);
    memset(o + OBJ_MODES, 0, OBJ_WORK - OBJ_MODES);
    snx_st_le64(o + OBJ_WORK, work);
    snx_st_le64(o + OBJ_WORK_SIZE, size);
    snx_st_le64(o + OBJ_HEAP, 0);
    o[OBJ_LANGUAGE] = (u8)(language & 1);
    o[OBJ_INITIALIZED] = 1;
    return 0;
}
/* Finalize and the destructor: nothing for an object that is not initialized */
void nnngc_finalize(CPU* c, u64 self) {
    u8* o = hook_host(c, self);
    if (!o[OBJ_INITIALIZED]) return;
    snx_st_le64(o + OBJ_HEAP, 0);
    o[OBJ_INITIALIZED] = 0;
}
u32 nnngc_get_content_version(CPU* c, u64 self) {
    (void)c, (void)self;
    return NGC_CONTENT_VERSION;
}

/* a text with no word of a list in it: unchanged, and none counted */
static u32 mask(CPU* c, u64 self, u64 count, u64 text, int utf16) {
    if (!initialized(c, self)) return NGC_NOT_INITIALIZED;
    if (!text) return NGC_NULL;
    if (!(utf16 ? utf16_ok(c, text) : utf8_ok(c, text))) return NGC_ENCODING;
    if (count) snx_st_le32(hook_host(c, count), 0);
    return 0;
}
u32 nnngc_mask(CPU* c, u64 self, u64 count, u64 text, u32 patterns) {
    (void)patterns;
    return mask(c, self, count, text, 0);
}
u32 nnngc_mask16(CPU* c, u64 self, u64 count, u64 text, u32 patterns) {
    (void)patterns;
    return mask(c, self, count, text, 1);
}

/* words with none of a list's in them: a 0 for each */
static u32 check(CPU* c, u64 self, u64 out, u64 words, u64 n, int utf16) {
    if (!initialized(c, self)) return NGC_NOT_INITIALIZED;
    if (!out || !words) return NGC_NULL;
    if (n > NGC_WORDS_MAX) return NGC_SIZE;
    for (u64 i = 0; i < n; i++) {
        u64 word = snx_ld_le64(hook_host(c, words + i * 8));
        if (!word || !(utf16 ? utf16_ok(c, word) : utf8_ok(c, word))) return NGC_ENCODING;
    }
    for (u64 i = 0; i < n; i++) snx_st_le32(hook_host(c, out + i * 4), 0);
    return 0;
}
u32 nnngc_check(CPU* c, u64 self, u64 out, u32 patterns, u64 words, u64 n) {
    (void)patterns;
    return check(c, self, out, words, n, 0);
}
u32 nnngc_check16(CPU* c, u64 self, u64 out, u32 patterns, u64 words, u64 n) {
    (void)patterns;
    return check(c, self, out, words, n, 1);
}
