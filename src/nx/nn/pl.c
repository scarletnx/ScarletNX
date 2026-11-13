/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The fonts' placement in the shared memory is ported from Eden (src/core/hle/service/ns/platform_service_manager.cpp),
   through the runtime's pl:u service that this file replaces:
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* nn::pl, native (pl.hook): the shared fonts, with no pl:u session. The behavior is that of a clean-room spec of the
   SDK's functions, never the SDK's code. Six TrueType fonts are in one shared memory, each behind the 8 byte header
   the SDK's font engine reads its size from. A font comes from the game folder's fonts/<Name>.bfttf (the console's
   file, decrypted here) or fonts/<Name>.ttf, and otherwise from the stand-ins compiled in (cmake/shared_fonts.cmake):
   Noto Sans JP, SC, TC and KR, one per script the six slots name, and yuzu's controller button glyphs for
   FontNintendoExtended. Each Noto subset covers its script's standard character set (JIS X 0208, GB 2312, Big5,
   KS X 1001) and ASCII, so the five together stay inside the 0x1100000 bytes the SDK maps.

   The SDK maps that memory read only, once, at the first function that gives an address, and keeps the base for the
   life of the process. The hooks do the same through the kernel (kshmem_map_free). Every font is loaded from the
   start. The SDK stops the game on any failure of the service, so no function here returns one.

   The Ounce's SDK also has built-in fonts (GetBuiltInFontData), which are the shared fonts, each a plain TrueType
   font past its header: the same memory answers, with the font's address in it and its size. Its other built-in
   font functions (the sets, and the ones of the first console's types) have no hook: a game that calls one opens
   pl:u, and sm stops it with the name of this layer */
#include "nx/nn/pl.h"
#include "nx/nn/pl_font.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/thread.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char snx_font_japanese[], snx_font_chinese_simplified[], snx_font_chinese_traditional[],
    snx_font_korean[], snx_font_extended[];
extern const size_t snx_font_japanese_size, snx_font_chinese_simplified_size, snx_font_chinese_traditional_size,
    snx_font_korean_size, snx_font_extended_size;

#define PL_SHM_SIZE   0x1100000u /* what the SDK maps, and the fonts have to fit in */
#define PL_MAGIC      0x36f81a1eu /* the first word of a .bfttf, big-endian */
#define PL_DECRYPTED  0x7f9a0218u /* what it decrypts to: its key is the two XORed */
#define PL_LOADED     1           /* nn::pl::LoadState */

/* nn::pl::SharedFontType, in order */
static const char* const pl_names[PL_FONTS] = {"FontStandard",        "FontChineseSimplified",
                                               "FontExtendedChineseSimplified", "FontChineseTraditional",
                                               "FontKorean",          "FontNintendoExtended"};



/* a .bfttf into the TrueType font it holds, in place: each big-endian word XORed with the key, the 8 byte header
   (magic, size) dropped. Returns the font's size, 0 if it isn't one */
static u64 pl_decrypt(u8* d, u64 size) {
    u32 key, n;
    u64 i;
    if (size < 8 || snx_ld_be32(d) != PL_MAGIC) return 0;
    key = PL_MAGIC ^ PL_DECRYPTED;
    n = snx_ld_be32(d + 4) ^ key;
    if (n > size - 8) n = (u32)(size - 8);
    for (i = 0; i + 4 <= (u64)n; i += 4) snx_st_be32(d + i, snx_ld_be32(d + 8 + i) ^ key);
    for (; i < (u64)n; i++) d[i] = d[8 + i]; /* a tail shorter than a word: there is none in a real .bfttf */
    return n;
}

/* a font of the game folder's for slot i, the .bfttf before the .ttf, or NULL */
static u8* pl_game_font(int i, u64* size) {
    char path[64];
    u8* d;
    sprintf(path, "fonts/%s.bfttf", pl_names[i]);
    d = fs_read_file(path, size);
    if (d) {
        *size = pl_decrypt(d, *size);
        if (*size) return d;
        WARN("pl: %s isn't a shared font, the stand-in takes its place\n", path);
        free(d);
    }
    sprintf(path, "fonts/%s.ttf", pl_names[i]);
    d = fs_read_file(path, size);
    if (d && !*size) { /* an empty file is no font: the stand-in */
        free(d);
        d = NULL;
    }
    return d;
}

/* a font at *at with its header: the magic and the size, padded to words, XORed with the key, both big-endian, as
   the SDK decodes them. Returns 0 if it doesn't fit */
static int pl_place(u8* shm, u32 shm_size, u32* at, const u8* font, u64 size, int slot) {
    u32 padded, key = PL_MAGIC ^ PL_DECRYPTED;
    if (size > shm_size || (size + 3) / 4 * 4 + 8 > shm_size - *at) return 0;
    padded = (u32)((size + 3) & ~3ull);
    snx_st_be32(shm + *at, PL_MAGIC ^ key);
    snx_st_be32(shm + *at + 4, padded ^ key);
    memcpy(shm + *at + 8, font, (size_t)size);
    memset(shm + *at + 8 + size, 0, padded - (u32)size);
    g_hle.pl.font_off[slot] = *at + 8;
    g_hle.pl.font_size[slot] = (u32)size;
    *at += 8 + padded;
    return 1;
}

/* a stand-in with the console fonts' private-use characters answered (pl_font.c), its own font if that fails */
static int pl_place_stand_in(u8* shm, u32 shm_size, u32* at, const u8* font, u64 size, int slot, const char* which) {
    u64 covered = 0;
    u8* patched = pl_font_cover(font, size, &covered);
    int ok = pl_place(shm, shm_size, at, patched ? patched : font, patched ? covered : size, slot);
    if (patched && ok) LOG("pl: the %s stand-in answers for the console font's private-use characters\n", which);
    free(patched);
    return ok;
}

/* the six fonts into shm, and where each is into pl */
static void fonts_fill(u8* shm, u32 shm_size) {
    u32 at = 0;
    int i;
    memset(g_hle.pl.font_off, 0, sizeof g_hle.pl.font_off);
    memset(g_hle.pl.font_size, 0, sizeof g_hle.pl.font_size);
    for (i = 0; i < PL_FONTS; i++) {
        u64 size = 0;
        u8* own = pl_game_font(i, &size);
        if (own) {
            if (pl_place(shm, shm_size, &at, own, size, i))
                LOG("pl: %s from the game folder's fonts/\n", pl_names[i]);
            else
                WARN("pl: fonts/%s doesn't fit in the shared memory, the stand-in takes its place\n", pl_names[i]);
            free(own);
            if (g_hle.pl.font_size[i]) continue;
        }
        switch (i) {
        case 0:
            pl_place_stand_in(shm, shm_size, &at, snx_font_japanese, snx_font_japanese_size, i, "Japanese");
            break;
        case 1:
            pl_place_stand_in(shm, shm_size, &at, snx_font_chinese_simplified,
                              snx_font_chinese_simplified_size, i, "Chinese simplified");
            break;
        case 2: /* extended simplified Chinese is the simplified font's glyphs */
            if (g_hle.pl.font_size[1]) {
                g_hle.pl.font_off[i] = g_hle.pl.font_off[1];
                g_hle.pl.font_size[i] = g_hle.pl.font_size[1];
            } else {
                pl_place_stand_in(shm, shm_size, &at, snx_font_chinese_simplified,
                                  snx_font_chinese_simplified_size, i, "Chinese simplified");
            }
            break;
        case 3:
            pl_place_stand_in(shm, shm_size, &at, snx_font_chinese_traditional,
                              snx_font_chinese_traditional_size, i, "Chinese traditional");
            break;
        case 4:
            pl_place_stand_in(shm, shm_size, &at, snx_font_korean, snx_font_korean_size, i, "Korean");
            break;
        default:
            pl_place_stand_in(shm, shm_size, &at, snx_font_extended, snx_font_extended_size, i, "extended");
            break;
        }
    }
}

/* the host's side, which a save state never holds */
static struct {
    PMutex lock; /* the first mapping */
} g_nnpl;

void nnpl_init(void) {
    pm_init(&g_nnpl.lock);
    g_hle.pl.shm = kobj_id(&kshmem_new(PL_SHM_SIZE, "shared fonts")->o);
    kobj_ref(kobj_at(g_hle.pl.shm)); /* the fonts', for as long as the process runs */
    fonts_fill(kshmem_ptr((KShmem*)kobj_at(g_hle.pl.shm)), PL_SHM_SIZE);
}

/* ============================================================ the hooks */

/* the guest address of the fonts' memory, mapped read only at the first ask */
static u64 fonts_base(void) {
    u64 base = __atomic_load_n(&g_hle.pl.base, __ATOMIC_ACQUIRE);
    if (base) return base;
    pm_lock(&g_nnpl.lock);
    base = g_hle.pl.base;
    if (!base) {
        base = kshmem_map_free((KShmem*)kobj_at(g_hle.pl.shm), PERM_R);
        if (!base) {
            pm_unlock(&g_nnpl.lock);
            fatal("nn::pl: no free guest pages for the shared fonts' 0x%x bytes", PL_SHM_SIZE);
        }
        __atomic_store_n(&g_hle.pl.base, base, __ATOMIC_RELEASE);
        LOG("pl: the shared fonts are at 0x%llx in the game, read only\n", (unsigned long long)base);
    }
    pm_unlock(&g_nnpl.lock);
    return base;
}
/* a font type's place and size: none for a type past the six, as the service answered (what the console answers is
   not known, and the SDK has no check of its own) */
static u32 font_off(i32 type) { return (u32)type < PL_FONTS ? g_hle.pl.font_off[type] : 0; }
static u32 font_size(i32 type) { return (u32)type < PL_FONTS ? g_hle.pl.font_size[type] : 0; }

void nnpl_request_shared_font_load(CPU* c, i32 type) { (void)c, (void)type; }
i32 nnpl_get_shared_font_load_state(CPU* c, i32 type) {
    (void)c, (void)type;
    return PL_LOADED;
}
u64 nnpl_get_shared_font_size(CPU* c, i32 type) {
    (void)c;
    return font_size(type);
}
u64 nnpl_get_shared_font_address(CPU* c, i32 type) {
    (void)c;
    return fonts_base() + font_off(type);
}

/* every font, in the order of their types whatever the language, as Eden does. The count is all of them, the
   entries as many as max allows. Each entry: its type, its address and its size, by the SDK's three arrays (an
   address and a size of word bytes each: 8, or a 32-bit game's 4) or its SharedFontData (0x18 bytes: the type at 0,
   the address at 8, the size at 0x10, the padding left) */
static void in_order(CPU* c, u64 count, i32 max, u64 types, u64 addresses, u64 sizes, u64 data, u32 word) {
    snx_st_le32(hook_host(c, count), PL_FONTS);
    u64 base = fonts_base();
    for (i32 i = 0; i < max && i < PL_FONTS; i++) {
        u64 address = base + g_hle.pl.font_off[i], size = g_hle.pl.font_size[i];
        if (data) {
            u8* d = hook_host(c, data + (u64)i * 0x18);
            snx_st_le32(d, (u32)i);
            snx_st_le64(d + 8, address);
            snx_st_le64(d + 0x10, size);
            continue;
        }
        snx_st_le32(hook_host(c, types + (u64)i * 4), (u32)i);
        if (word == 4) {
            snx_st_le32(hook_host(c, addresses + (u64)i * 4), (u32)address);
            snx_st_le32(hook_host(c, sizes + (u64)i * 4), (u32)size);
            continue;
        }
        snx_st_le64(hook_host(c, addresses + (u64)i * 8), address);
        snx_st_le64(hook_host(c, sizes + (u64)i * 8), size);
    }
}
int nnpl_get_shared_font_in_order_of_priority(CPU* c, u64 count, u64 types, u64 addresses, u64 sizes, i32 max,
                                              u64 language) {
    (void)language;
    in_order(c, count, max, types, addresses, sizes, 0, 8);
    return 1;
}
/* a 32-bit game's: its arrays hold 4-byte addresses and sizes */
int nnpl_get_shared_font_in_order_of_priority_a32(CPU* c, u64 count, u64 types, u64 addresses, u64 sizes, i32 max,
                                                  u32 language, u32 language_end) {
    (void)language, (void)language_end;
    in_order(c, count, max, types, addresses, sizes, 0, 4);
    return 1;
}
int nnpl_get_shared_font_in_order_of_priority_for_system(CPU* c, u64 count, u64 types, u64 addresses, u64 sizes,
                                                         i32 max, u64 language) {
    (void)language;
    in_order(c, count, max, types, addresses, sizes, 0, 8);
    return 1;
}
int nnpl_get_shared_font(CPU* c, u64 count, u64 data, i32 max, u64 language) {
    (void)language;
    in_order(c, count, max, 0, 0, 0, data, 8);
    return 1;
}

/* GetBuiltInFontData: the caller's BuiltInFontDataOunce filled as the SDK fills it from the service's answer, two
   bytes of flags (1 and 0: the first is the value a game ran with, and what they mean is not known), the font's
   address at 8 and its size at 0x10; a type with no font leaves it and answers false */
int nnpl_get_built_in_font_data(CPU* c, u64 font, u8 type, int flag) {
    (void)flag;
    if (type >= PL_FONTS || !g_hle.pl.font_size[type]) return 0;
    u8* f = hook_host(c, font);
    f[0] = 1;
    f[1] = 0;
    snx_st_le64(f + 8, fonts_base() + g_hle.pl.font_off[type]);
    snx_st_le64(f + 0x10, g_hle.pl.font_size[type]);
    return 1;
}
