/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format reference: Azahar TarArchive (common/tar_file.cpp, GPL-3.0-or-later, read, not ported) over microtar,
   and the POSIX ustar header layout. */
/* A bundle, what Azahar's .bcci/.bcxi/.bcia is: a tar of a game file beside what a publisher's note is, read the
   way its TarArchive reads one. The headers are 512 bytes, each entry's data follows its header rounded to 512,
   and the game is the first entry whose name is a game's (a .cia, .cci, .3ds, .cxi, .app or .hbb). Everything the
   archive lays out is checked against the input's size, each header against its own checksum, names against the
   tar's field ends, and duplicates are refused, so a malformed bundle is refused rather than read loosely. */
#include "3ds/loader/internal.h"
#include <stdlib.h>
#include <string.h>

#define TAR_HEADER 512u
#define TAR_ENTRIES_LIMIT 50 /* Azahar's TarArchive reads no more of a bundle than this */
#define TAR_NAME 202         /* an entry's name with its prefix, the longest taken, and its end */
/* the game extensions an entry's name may end in, lowercased; the same ones the driver and the launcher take */
static const char* const game_suffixes[] = {".cia", ".cci", ".3ds", ".cxi", ".app", ".hbb"};

static bool entry_size(const unsigned char* h, uint64_t* out) {
    /* octal, surrounded by NULs and spaces; a GNU base-256 size (a high bit in the field) is not one of a
       bundle's, and is refused */
    *out = 0;
    bool digit = false;
    uint64_t size = 0;
    for (unsigned i = 0; i < 12; ++i) {
        unsigned char c = h[124 + i];
        if (c & 0x80) return false;
        if (!c || c == ' ') continue;
        if (c < '0' || c > '7') return false;
        digit = true;
        size = size * 8 + (unsigned)(c - '0');
        if (size > CTR_CARD_LIMIT) return false;
    }
    if (!digit) return false;
    *out = size;
    return true;
}
static bool checksum(const unsigned char* h) {
    unsigned stored = 0, sum = 0;
    unsigned digits = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned char c = h[148 + i];
        if (c == ' ' || !c) continue;
        if (c < '0' || c > '7' || digits > 6) return false;
        stored = stored * 8 + (unsigned)(c - '0');
        ++digits;
    }
    for (unsigned i = 0; i < TAR_HEADER; ++i) sum += i >= 148 && i < 156 ? ' ' : h[i];
    return digits && stored == sum;
}
static bool suffix(const char* name, size_t n, const char* suffix) {
    size_t e = strlen(suffix);
    if (n < e) return false;
    for (size_t i = 0; i < e; ++i)
        if (snx_to_lower(name[n - e + i]) != suffix[i]) return false;
    return true;
}
static bool game_name(const char* name, size_t n) {
    for (size_t i = 0; i < sizeof game_suffixes / sizeof *game_suffixes; ++i)
        if (suffix(name, n, game_suffixes[i])) return true;
    return false;
}
bool ctr_tar_game(CtrReader* r, bool* is_tar) {
    *is_tar = false;
    unsigned char h[TAR_HEADER];
    if (r->size < TAR_HEADER || !ctr_read(r, (CtrSpan){0, r->size}, 0, h, sizeof h)) return false;
    if (memcmp(h + 257, "ustar", 5)) return true; /* a bundle is a ustar archive; anything else is a game itself */
    *is_tar = true;
    char (*names)[TAR_NAME] = calloc(TAR_ENTRIES_LIMIT, sizeof *names);
    CtrSpan* games = calloc(TAR_ENTRIES_LIMIT, sizeof *games);
    if (!names || !games) {
        free(names);
        free(games);
        return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    }
    uint64_t at = 0;
    size_t entries = 0, names_count = 0, games_count = 0;
    bool ok = true;
    while (ok) {
        if (r->size - at < TAR_HEADER || !ctr_read(r, (CtrSpan){0, r->size}, at, h, sizeof h)) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "the bundle ends inside an entry's header");
            break;
        }
        bool empty = true;
        for (unsigned i = 0; i < TAR_HEADER; ++i)
            if (h[i]) {
                empty = false;
                break;
            }
        if (empty) break; /* the two zero blocks that end an archive */
        if (!checksum(h)) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "a bundle entry's header checksum is wrong");
            break;
        }
        uint64_t size = 0;
        if (!entry_size(h, &size)) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "a bundle entry's size is not one the archive takes");
            break;
        }
        if (++entries > TAR_ENTRIES_LIMIT) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "the bundle holds more entries than a game's takes");
            break;
        }
        /* the data must be whole in the archive, as Azahar's reads it */
        uint64_t data = at + TAR_HEADER;
        if (size > r->size - data) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "a bundle entry's data is not whole in the archive");
            break;
        }
        /* a regular file only ('0' or the short form's NUL); a directory, a link or pax metadata is skipped */
        if ((h[156] == '0' || !h[156]) && h[0]) {
            size_t n = 0;
            while (n < 100 && h[n]) ++n;
            if (n == 100 && h[100]) {
                ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "a bundle entry's name has no end in its field");
                break;
            }
            char name[TAR_NAME];
            size_t prefix = 0;
            while (prefix < 155 && h[345 + prefix]) ++prefix;
            if (prefix && (prefix > 155 || prefix + 1 + n >= sizeof name)) {
                ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "a bundle entry's name is too long");
                break;
            }
            memcpy(name, h + 345, prefix);
            if (prefix) name[prefix++] = '/';
            memcpy(name + prefix, h, n);
            name[prefix + n] = 0;
            for (size_t i = 0; i < names_count; ++i)
                if (!strcmp(names[i], name)) {
                    ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "the bundle holds two entries named %s", name);
                    break;
                }
            if (!ok) break;
            if (names_count < TAR_ENTRIES_LIMIT) memcpy(names[names_count++], name, strlen(name) + 1);
            /* the game is the first entry whose name is a game's, as Azahar's first-name-match is; every entry
               that could be one is held, and the pick comes when the whole archive has been read */
            if (games_count < TAR_ENTRIES_LIMIT && game_name(name, prefix + n)) {
                games[games_count++] = (CtrSpan){data, size};
            }
        }
        at = data + ((size + TAR_HEADER - 1) / TAR_HEADER) * TAR_HEADER;
    }
    if (ok && !games_count)
        ok = ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED,
                      "the bundle holds no game (a .cia, .cci, .3ds, .cxi, .app or .hbb)");
    if (ok) {
        r->base = games[0].offset;
        r->size = games[0].size;
    }
    free(names);
    free(games);
    return ok;
}
