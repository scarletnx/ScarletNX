/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/ns/platform_service_manager.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* pl:u and pl:s, the shared fonts: six TrueType fonts in one shared memory, each behind the 8 byte header the SDK
   reads its size from. A font comes from the game folder's fonts/<Name>.bfttf (the console's file, decrypted here)
   or fonts/<Name>.ttf, and otherwise from the stand-ins compiled in (cmake/shared_fonts.cmake): Noto Sans JP, SC,
   TC and KR, one per script the six slots name, and yuzu's controller button glyphs for FontNintendoExtended. Each
   Noto subset covers its script's standard character set (JIS X 0208, GB 2312, Big5, KS X 1001) and ASCII, so the
   five together stay inside the 0x1100000 bytes the SDK maps */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/services/pl_font.h"

#include "hle/services/pl.swipc.inc"

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

static SvcObj* pl_new(void) { return nn_pl_detail_IPlatformServiceManager_new(); }
void svc_pl_init(void) {
    g_hle.pl.shm = kobj_id(&kshmem_new(PL_SHM_SIZE, "shared fonts")->o);
    kobj_ref(kobj_at(g_hle.pl.shm)); /* the service's, for as long as the process runs */
    fonts_fill(shm_ptr(g_hle.pl.shm), PL_SHM_SIZE);
    nn_pl_detail_IPlatformServiceManager_register(pl_new);
}

/* ============================================================ IPlatformServiceManager */
/* every font is loaded from the start */
static int nn_pl_detail_IPlatformServiceManager_RequestLoad(SvcObj* self, u32 font_type) {
    (void)self, (void)font_type;
    R_SUCCEED();
}
static int nn_pl_detail_IPlatformServiceManager_GetLoadState(SvcObj* self, u32 font_type, u32* state) {
    (void)self, (void)font_type;
    *state = PL_LOADED;
    R_SUCCEED();
}
static int nn_pl_detail_IPlatformServiceManager_GetSize(SvcObj* self, u32 font_type, u32* size) {
    (void)self;
    *size = font_type < PL_FONTS ? g_hle.pl.font_size[font_type] : 0;
    R_SUCCEED();
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedMemoryAddressOffset(SvcObj* self, u32 font_type,
                                                                              u32* offset) {
    (void)self;
    *offset = font_type < PL_FONTS ? g_hle.pl.font_off[font_type] : 0;
    R_SUCCEED();
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedMemoryNativeHandle(SvcObj* self, u32* shared_memory) {
    (void)self;
    *shared_memory = h_new(kobj_at(g_hle.pl.shm));
    R_SUCCEED();
}
/* every font, in the order of their types whatever the language, as Eden does: up to 6, or as many as the buffers
   hold */
static int pl_in_order(IpcBuf codes, IpcBuf offsets, IpcBuf sizes, u8* loaded, u32* count) {
    u64 n = PL_FONTS, i;
    if (codes.size / 4 < n) n = codes.size / 4;
    if (offsets.size / 4 < n) n = offsets.size / 4;
    if (sizes.size / 4 < n) n = sizes.size / 4;
    for (i = 0; i < n; i++) {
        gw32(codes.addr + 4 * i, (u32)i);
        gw32(offsets.addr + 4 * i, g_hle.pl.font_off[i]);
        gw32(sizes.addr + 4 * i, g_hle.pl.font_size[i]);
    }
    *loaded = 1;
    *count = (u32)n;
    R_SUCCEED();
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedFontInOrderOfPriority(
    SvcObj* self, const nn_settings_LanguageCode* language_code, IpcBuf font_codes, IpcBuf offsets, IpcBuf sizes,
    u8* loaded, u32* count) {
    (void)self, (void)language_code;
    return pl_in_order(font_codes, offsets, sizes, loaded, count);
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedFontInOrderOfPriorityForSystem(
    SvcObj* self, const nn_settings_LanguageCode* language_code, IpcBuf font_codes, IpcBuf offsets, IpcBuf sizes,
    u8* loaded, u32* count) {
    (void)self, (void)language_code;
    return pl_in_order(font_codes, offsets, sizes, loaded, count);
}
/* Switch 2 (nn::pl::GetBuiltInFontData): the built-in fonts, which the SDK maps from 2000's memory and finds by 2010's
   offset in it. The fonts are the shared fonts, a built-in font type taken as the shared font type of the same
   number, as eden-ns2 does (nn::pl::BuiltInFontTypeOunce's own list is not known). A type past them is not available */
static int nn_pl_detail_IPlatformServiceManager_GetBuiltInFontSharedMemory(SvcObj* self, u32* shared_memory,
                                                                          u32* size) {
    (void)self;
    *shared_memory = h_new(kobj_at(g_hle.pl.shm));
    *size = PL_SHM_SIZE;
    R_SUCCEED();
}
/* option is read as one bit and changes nothing (eden-ns2) */
static int nn_pl_detail_IPlatformServiceManager_GetBuiltInFontData(SvcObj* self, u8 font_type, u8 option,
                                                                   u8* available,
                                                                   nn_pl_BuiltInFontDescriptor* descriptor) {
    (void)self, (void)option;
    if (font_type < PL_FONTS && g_hle.pl.font_size[font_type]) {
        *available = 1;
        *descriptor = (nn_pl_BuiltInFontDescriptor){.type = font_type,
                                                    .offset = g_hle.pl.font_off[font_type],
                                                    .size = g_hle.pl.font_size[font_type]};
    }
    R_SUCCEED();
}
