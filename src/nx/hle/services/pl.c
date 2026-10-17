/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/ns/platform_service_manager.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* pl:u and pl:s, the shared fonts: six TrueType fonts in one shared memory, each behind the 8 byte header the SDK
   reads its size from. A font comes from the game folder's fonts/<Name>.bfttf (the console's file, decrypted here)
   or fonts/<Name>.ttf, and otherwise from the stand-ins compiled in (cmake/shared_fonts.cmake), the standard one
   placed once for the five slots that share it */
#include "nx/hle/services/services_int.h"
#include "nx/hle/filesystem/fs.h"

#include "hle/services/pl.swipc.inc"

extern const unsigned char snx_font_standard[], snx_font_extended[];
extern const size_t snx_font_standard_size, snx_font_extended_size;

#define PL_SHM_SIZE   0x1100000u
#define PL_FONTS      6
#define PL_MAGIC      0x36f81a1eu /* the first word of a .bfttf, big-endian */
#define PL_DECRYPTED  0x7f9a0218u /* what it decrypts to: its key is the two XORed */
#define PL_LOADED     1           /* nn::pl::LoadState */

/* nn::pl::SharedFontType, in order */
static const char* const pl_names[PL_FONTS] = {"FontStandard",        "FontChineseSimplified",
                                               "FontExtendedChineseSimplified", "FontChineseTraditional",
                                               "FontKorean",          "FontNintendoExtended"};

static struct {
    KShmem* shm;
    u32 font_off[PL_FONTS], font_size[PL_FONTS]; /* where each font starts (past its header), and its size */
} pl;

static u32 bswap32_(u32 v) { return v >> 24 | (v >> 8 & 0xff00u) | (v << 8 & 0xff0000u) | v << 24; }
/* big-endian words, on the little-endian hosts the runtime runs on */
static u32 rd_be32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return bswap32_(v);
}
static void wr_be32(u8* p, u32 v) {
    v = bswap32_(v);
    memcpy(p, &v, 4);
}

/* a .bfttf into the TrueType font it holds, in place: each big-endian word XORed with the key, the 8 byte header
   (magic, size) dropped. Returns the font's size, 0 if it isn't one */
static u64 pl_decrypt(u8* d, u64 size) {
    u32 key, n;
    u64 i;
    if (size < 8 || rd_be32(d) != PL_MAGIC) return 0;
    key = PL_MAGIC ^ PL_DECRYPTED;
    n = rd_be32(d + 4) ^ key;
    if (n > size - 8) n = (u32)(size - 8);
    for (i = 0; i + 4 <= (u64)n; i += 4) wr_be32(d + i, rd_be32(d + 8 + i) ^ key);
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
    return fs_read_file(path, size);
}

/* a font at *at with its header: the magic and the size, padded to words, XORed with the key, both big-endian, as
   the SDK decodes them. Returns 0 if it doesn't fit */
static int pl_place(u8* shm, u32 shm_size, u32* at, const u8* font, u64 size, int slot) {
    u32 padded, key = PL_MAGIC ^ PL_DECRYPTED;
    if (size > shm_size || (size + 3) / 4 * 4 + 8 > shm_size - *at) return 0;
    padded = (u32)((size + 3) & ~3ull);
    wr_be32(shm + *at, PL_MAGIC);
    wr_be32(shm + *at + 4, padded ^ key);
    memcpy(shm + *at + 8, font, (size_t)size);
    memset(shm + *at + 8 + size, 0, padded - (u32)size);
    pl.font_off[slot] = *at + 8;
    pl.font_size[slot] = (u32)size;
    *at += 8 + padded;
    return 1;
}

/* the six fonts into shm, and where each is into pl */
static void fonts_fill(u8* shm, u32 shm_size) {
    u32 at = 0;
    int i, standard = -1; /* the slot the standard stand-in went into first */
    memset(pl.font_off, 0, sizeof pl.font_off);
    memset(pl.font_size, 0, sizeof pl.font_size);
    for (i = 0; i < PL_FONTS; i++) {
        u64 size = 0;
        u8* own = pl_game_font(i, &size);
        if (own) {
            if (pl_place(shm, shm_size, &at, own, size, i))
                LOG("pl: %s from the game folder's fonts/\n", pl_names[i]);
            else
                WARN("pl: fonts/%s doesn't fit in the shared memory, the stand-in takes its place\n", pl_names[i]);
            free(own);
            if (pl.font_size[i]) continue;
        }
        if (i == PL_FONTS - 1)
            pl_place(shm, shm_size, &at, snx_font_extended, snx_font_extended_size, i);
        else if (standard >= 0) {
            pl.font_off[i] = pl.font_off[standard];
            pl.font_size[i] = pl.font_size[standard];
        } else if (pl_place(shm, shm_size, &at, snx_font_standard, snx_font_standard_size, i))
            standard = i;
    }
}

static SvcObj* pl_new(void) { return nn_pl_detail_IPlatformServiceManager_new(0); }
void svc_pl_init(void) {
    pl.shm = kshmem_new(PL_SHM_SIZE, "shared fonts");
    kobj_ref(&pl.shm->o); /* the service's, for as long as the process runs */
    fonts_fill(kshmem_ptr(pl.shm), PL_SHM_SIZE);
    nn_pl_detail_IPlatformServiceManager_register(pl_new);
}

/* ============================================================ IPlatformServiceManager */
/* every font is loaded from the start */
static int nn_pl_detail_IPlatformServiceManager_RequestLoad(SvcObj* self, u32 font_type) {
    (void)self, (void)font_type;
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetLoadState(SvcObj* self, u32 font_type, u32* state) {
    (void)self, (void)font_type;
    *state = PL_LOADED;
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetSize(SvcObj* self, u32 font_type, u32* size) {
    (void)self;
    *size = font_type < PL_FONTS ? pl.font_size[font_type] : 0;
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedMemoryAddressOffset(SvcObj* self, u32 font_type,
                                                                              u32* offset) {
    (void)self;
    *offset = font_type < PL_FONTS ? pl.font_off[font_type] : 0;
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedMemoryNativeHandle(SvcObj* self, u32* shared_memory) {
    (void)self;
    *shared_memory = h_new(&pl.shm->o);
    return 0;
}
/* every font, in the order of their types whatever the language, as Eden does: up to 6, or as many as the buffers
   hold */
static int pl_in_order(IpcBuf codes, IpcBuf offsets, IpcBuf sizes, u8* loaded, u32* count) {
    u64 n = PL_FONTS, i;
    if (codes.size / 4 < n) n = codes.size / 4;
    if (offsets.size / 4 < n) n = offsets.size / 4;
    if (sizes.size / 4 < n) n = sizes.size / 4;
    for (i = 0; i < n; i++) {
        u32 code = (u32)i;
        memcpy(GP(codes.addr + 4 * i), &code, 4);
        memcpy(GP(offsets.addr + 4 * i), &pl.font_off[i], 4);
        memcpy(GP(sizes.addr + 4 * i), &pl.font_size[i], 4);
    }
    *loaded = 1;
    *count = (u32)n;
    return 0;
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
