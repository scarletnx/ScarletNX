/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/title.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "gui/i18n.h"
#ifdef SNX_GUI_ZSTD
#include <zstd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the bytes of a 3DS container a title is read from: a CIA's header, certificates, ticket, TMD and the start of its
   first content; a cartridge image's header and its game partition's NCCH header */
#define CTR_HEAD  0x20000u
#define CTR_UNIT  0x200u /* a cartridge's media unit */
#define TAR_BLOCK 512u

/* up to size bytes of path from at into out: how many */
static size_t read_at(const char* path, uint64_t at, unsigned char* out, size_t size) {
    FILE* f = snx_fopen(path, "rb");
    size_t got = f && !snx_seek(f, at) ? fread(out, 1, size, f) : 0;
    if (f) fclose(f);
    return got;
}
/* the first size bytes of the game a z3ds file at path holds, its zstd stream after its header and metadata, into out:
   how many; 0, why saying so, when none */
static size_t z3ds_head(const char* path, unsigned char* out, size_t size, char* why, size_t why_size) {
#ifdef SNX_GUI_ZSTD
    unsigned char header[0x20], *chunk = NULL;
    FILE* f = snx_fopen(path, "rb");
    ZSTD_DStream* z = NULL;
    ZSTD_outBuffer to = {out, size, 0};
    bool read = f && fread(header, 1, sizeof header, f) == sizeof header && !memcmp(header, "Z3DS", 4) &&
                !snx_seek(f, (uint64_t)snx_ld_le16(header + 0xA) + snx_ld_le32(header + 0xC));
    if (read && (chunk = (unsigned char*)malloc(0x10000)) != NULL && (z = ZSTD_createDStream()) != NULL &&
        !ZSTD_isError(ZSTD_initDStream(z)))
        while (to.pos < to.size) {
            size_t n = fread(chunk, 1, 0x10000, f);
            ZSTD_inBuffer from = {chunk, n, 0};
            while (from.pos < from.size && to.pos < to.size) {
                size_t left = ZSTD_decompressStream(z, &to, &from);
                if (ZSTD_isError(left)) {
                    snprintf(why, why_size, snx_tr("its z3ds stream does not decompress: %s"), ZSTD_getErrorName(left));
                    to.pos = 0;
                    goto done;
                }
            }
            if (!n) break;
        }
    if (!to.pos && !why[0]) snprintf(why, why_size, "%s", snx_tr("its z3ds header or stream does not read"));
done:
    ZSTD_freeDStream(z);
    free(chunk);
    if (f) fclose(f);
    return to.pos;
#else
    (void)path;
    (void)out;
    (void)size;
    snprintf(why, why_size, "%s", snx_tr("a z3ds file, which this build does not decompress"));
    return 0;
#endif
}
/* where the game of a bundle at path lies (a ustar archive's first regular entry named as a game, as the 3DS driver
   picks it), into *at: false when it holds none */
static bool tar_game(const char* path, uint64_t* at) {
    static const char* const games[] = {".cia", ".cci", ".3ds", ".cxi", ".app", ".hbb"};
    unsigned char h[TAR_BLOCK];
    char name[101];
    uint64_t here = 0, size;
    for (int entries = 0; entries < 4096 && read_at(path, here, h, sizeof h) == sizeof h && h[0]; ++entries) {
        size_t n = 0;
        size = 0;
        for (unsigned i = 0; i < 12; ++i)
            if (h[124 + i] >= '0' && h[124 + i] <= '7') size = size * 8 + (uint64_t)(h[124 + i] - '0');
        while (n < 100 && h[n]) ++n;
        memcpy(name, h, n);
        name[n] = 0;
        for (size_t i = 0; (h[156] == '0' || !h[156]) && i < sizeof games / sizeof *games; ++i)
            if (snx_ends_with_nocase(name, games[i])) {
                *at = here + TAR_BLOCK;
                return true;
            }
        here += TAR_BLOCK + (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    }
    return false;
}
/* an NCCH header's program ID, and its product code into code: false for no NCCH */
static bool ncch_title(const unsigned char* h, size_t n, uint64_t* id, char* code, size_t code_size) {
    size_t length = 0;
    if (n < CTR_UNIT || memcmp(h + 0x100, "NCCH", 4)) return false;
    *id = snx_ld_le64(h + 0x118);
    while (length < 16 && h[0x150 + length] >= 0x20 && h[0x150 + length] < 0x7F) ++length;
    snprintf(code, code_size, "%.*s", (int)length, (const char*)h + 0x150);
    return true;
}
/* a ticket's or a TMD's signed body: where its data starts past its signature, by the signature's type */
static size_t signed_body(uint32_t type) {
    return type == 0x10000 || type == 0x10003   ? 0x240
           : type == 0x10001 || type == 0x10004 ? 0x140
           : type == 0x10002 || type == 0x10005 ? 0x40
                                                : 0;
}
static uint64_t align64(uint64_t at) { return (at + 63) & ~(uint64_t)63; }
bool gui_ctr_title(const char* path, uint64_t* id, char* code, size_t code_size, char* why, size_t why_size) {
    unsigned char* h = (unsigned char*)malloc(CTR_HEAD);
    size_t n = h ? read_at(path, 0, h, CTR_HEAD) : 0;
    uint64_t base = 0;
    bool found = false;
    *id = 0;
    if (code_size) code[0] = 0;
    if (why_size) why[0] = 0;
    if (n >= 4 && !memcmp(h, "Z3DS", 4))
        n = z3ds_head(path, h, CTR_HEAD, why, why_size);
    else if (n >= TAR_BLOCK && !memcmp(h + 257, "ustar", 5))
        n = tar_game(path, &base) ? read_at(path, base, h, CTR_HEAD) : 0;
    if (n >= CTR_UNIT && !memcmp(h + 0x100, "NCSD", 4)) { /* the game partition's NCCH */
        uint64_t at = (uint64_t)snx_ld_le32(h + 0x120) * CTR_UNIT;
        found = at < n && ncch_title(h + at, n - (size_t)at, id, code, code_size);
    } else if (n >= CTR_UNIT && !memcmp(h + 0x100, "NCCH", 4))
        found = ncch_title(h, n, id, code, code_size);
    else if (n >= 0x2040 && snx_ld_le32(h) == 0x2020) { /* the TMD's title ID, the code from the first content */
        uint64_t ticket = align64(0x2040ull + snx_ld_le32(h + 8)), tmd = align64(ticket + snx_ld_le32(h + 12));
        uint64_t content = align64(tmd + snx_ld_le32(h + 16));
        size_t body = tmd + 4 <= n ? signed_body(snx_ld_be32(h + tmd)) : 0;
        if (body && tmd + body + 0x54 <= n) {
            uint64_t own = 0;
            found = true;
            *id = snx_ld_be64(h + tmd + body + 0x4C);
            if (content < n && (!ncch_title(h + content, n - (size_t)content, &own, code, code_size) || own != *id) &&
                code_size)
                code[0] = 0; /* its content encrypted, or another title's */
        }
    }
    if (!found && why_size && !why[0])
        snprintf(why, why_size, "%s",
                 n ? snx_tr("it is no CIA, cartridge image or NCCH, alone or in a bundle")
                   : snx_tr("it does not read"));
    free(h);
    return found;
}
bool gui_vita_title(const char* folder, char* id, size_t size) {
    char* sfo_path = snx_path_join(folder, "sce_sys/param.sfo");
    size_t length = 0;
    unsigned char* sfo = sfo_path ? snx_read_file(sfo_path, &length) : NULL;
    char* title = sfo ? snx_sfo_string(sfo, length, "TITLE_ID") : NULL;
    if (size) snprintf(id, size, "%s", title ? title : "");
    free(title);
    free(sfo);
    free(sfo_path);
    return size && id[0];
}
bool gui_gba_title(const char* rom, char code[5], char title[13], uint64_t* hash, char* why, size_t why_size) {
    size_t size = 0, n;
    unsigned char* data = (unsigned char*)snx_read_file(rom, &size);
    code[0] = title[0] = 0;
    *hash = 0;
    if (!data || size < 0xC0) {
        snprintf(why, why_size, "%s", data ? snx_tr("it is too short to be a GBA ROM") : snx_tr("it does not read"));
        free(data);
        return false;
    }
    for (n = 0; n < 4 && data[0xAC + n] >= 0x20 && data[0xAC + n] < 0x7F; ++n) code[n] = (char)data[0xAC + n];
    code[n] = 0;
    for (n = 0; n < 12 && data[0xA0 + n] >= 0x20 && data[0xA0 + n] < 0x7F; ++n) title[n] = (char)data[0xA0 + n];
    title[n] = 0;
    *hash = snx_fnv1a(SNX_FNV_BASIS, data, size);
    free(data);
    if (why_size) why[0] = 0;
    return true;
}
bool gui_gba_made_from(const char* folder, const char* name, uint64_t* hash) {
    char* map = snx_format("%s/%s_code.map", folder, name);
    char line[128];
    FILE* f = map ? snx_fopen(map, "rb") : NULL;
    int abi;
    unsigned arm;
    unsigned long long made = 0;
    bool read = f && fgets(line, sizeof line, f) && sscanf(line, "snx-agb-code %d %u %llX", &abi, &arm, &made) == 3;
    if (f) fclose(f);
    free(map);
    *hash = read ? (uint64_t)made : 0;
    return read;
}
