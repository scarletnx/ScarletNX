/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The characters the console's shared fonts carry and the stand-ins do not: the console's icons. A game's font map (an
   FCPX in a bfarc) names the font it belongs to by the console font's file name and lists the private-use characters
   that font draws, U+E000 to U+E1FF, and the SDK checks each one against the font engine's cmap
   (nn::fontll::ScalableFontEngine::CheckGlyphExist). A character with no cmap entry leaves the layout without a glyph,
   and the caller that draws it follows a null pointer (Super Mario 3D All-Stars does at its launcher's title text).
   The stand-ins are the Noto Sans JP, SC, TC and KR subsets (docs/platforms/nx/filesystem.md) and yuzu's controller button glyphs, none of
   which has the console's code points, and the engine has no fallback face to consult: it checks the one font the layout
   named (putting Last Resort, which maps every PUA code point, in another shared font slot still crashed at the same
   instruction). So every stand-in gets cmap entries for that block, each to its own space glyph: the character is in
   the font, the layout goes on, and the icon is blank instead of the game ending. A console font placed in fonts/
   keeps its own cmap and is not touched.
   The new entries are a new cmap table appended to the font: the table directory's cmap record, its checksum, and
   head's checkSumAdjustment are updated, and the font grows by that table. */
#include "nx/nn/pl_font.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

#define PUA_FIRST  0xE000u /* the console fonts' icon block, as their FCPX maps list it */
#define PUA_LAST   0xE1FFu
#define SPACE      0x20u   /* the glyph a character with no icon of its own draws as: nothing */
#define MAX_GLYPHS (1u << 20)

typedef struct {
    u32 code;
    u32 glyph;
} Glyph;

typedef struct {
    Glyph* at;
    size_t n, cap;
} Glyphs;

typedef struct {
    u64 offset, length;
    u8* record; /* the directory record, for its offset, length and checksum */
} Table;

static int in_range(const u8* font, u64 size, u64 offset, u64 length) {
    return offset <= size && length <= size - offset;
}

/* the table tag's record, if the directory has it */
static int find_table(const u8* font, u64 size, const char* tag, Table* table) {
    u64 i, count;
    if (!in_range(font, size, 0, 12)) return 0;
    count = snx_ld_be16(font + 4);
    if (!in_range(font, size, 12, count * 16)) return 0;
    for (i = 0; i < count; i++) {
        const u8* record = font + 12 + i * 16;
        if (memcmp(record, tag, 4)) continue;
        table->record = (u8*)record;
        table->offset = snx_ld_be32(record + 8);
        table->length = snx_ld_be32(record + 12);
        return in_range(font, size, table->offset, table->length);
    }
    return 0;
}

static int glyph_add(Glyphs* g, u32 code, u32 glyph) {
    if (g->n == g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 256;
        Glyph* at;
        if (cap > MAX_GLYPHS) return 0;
        at = realloc(g->at, cap * sizeof *at);
        if (!at) return 0;
        g->at = at;
        g->cap = cap;
    }
    g->at[g->n].code = code;
    g->at[g->n].glyph = glyph;
    g->n++;
    return 1;
}

static int glyph_order(const void* a, const void* b) {
    const Glyph* x = a;
    const Glyph* y = b;
    if (x->code != y->code) return x->code < y->code ? -1 : 1;
    return x->glyph < y->glyph ? -1 : x->glyph > y->glyph;
}

/* the last glyph of each code: the richer subtable, the format 12 (all of Unicode) one, comes after the format 4 one */
static void glyph_dedupe(Glyphs* g) {
    size_t i, n = 0;
    if (!g->n) return;
    qsort(g->at, g->n, sizeof *g->at, glyph_order);
    for (i = 1; i < g->n; i++)
        if (g->at[i].code != g->at[n].code) g->at[++n] = g->at[i];
    g->n = n + 1;
}

static u32 glyph_find(const Glyphs* g, u32 code, int* found) {
    size_t lo = 0, hi = g->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g->at[mid].code < code)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (found) *found = lo < g->n && g->at[lo].code == code;
    return lo < g->n && g->at[lo].code == code ? g->at[lo].glyph : 0;
}

/* a format 4 subtable's code points and glyphs */
static int cmap4(const u8* font, u64 size, u64 at, Glyphs* out) {
    u64 seg, i, ends, starts, deltas, ranges;
    u16 length, count;
    if (!in_range(font, size, at, 14)) return 0;
    length = snx_ld_be16(font + at + 2);
    count = snx_ld_be16(font + at + 6);
    if (length < 16 || !in_range(font, size, at, length) || count < 2 || (count & 1)) return 0;
    seg = count / 2;
    if (16 + 8 * seg > length) return 0;
    ends = at + 14;
    starts = at + 16 + 2 * seg;
    deltas = at + 16 + 4 * seg;
    ranges = at + 16 + 6 * seg;
    for (i = 0; i < seg; i++) {
        u32 start = snx_ld_be16(font + starts + 2 * i);
        u32 last = snx_ld_be16(font + ends + 2 * i);
        u16 delta = snx_ld_be16(font + deltas + 2 * i);
        u16 range = snx_ld_be16(font + ranges + 2 * i);
        u32 code;
        if (start > last) continue;
        for (code = start;; code++) {
            u32 glyph;
            if (!range) {
                glyph = (code + delta) & 0xFFFF;
            } else {
                u64 cell = ranges + 2 * i + range + 2 * (u64)(code - start);
                glyph = in_range(font, size, cell, 2) ? snx_ld_be16(font + cell) : 0;
                if (glyph) glyph = (glyph + delta) & 0xFFFF;
            }
            if (glyph && !glyph_add(out, code, glyph)) return 0;
            if (code == last || code == 0xFFFF) break;
        }
    }
    return 1;
}

/* a format 12 subtable's code points and glyphs */
static int cmap12(const u8* font, u64 size, u64 at, Glyphs* out) {
    u64 i, groups;
    u32 length;
    if (!in_range(font, size, at, 16)) return 0;
    length = snx_ld_be32(font + at + 4);
    groups = snx_ld_be32(font + at + 12);
    if (length < 16 || !in_range(font, size, at, length) || groups > (length - 16) / 12) return 0;
    for (i = 0; i < groups; i++) {
        const u8* group = font + at + 16 + i * 12;
        u32 code = snx_ld_be32(group), last = snx_ld_be32(group + 4), glyph = snx_ld_be32(group + 8);
        if (code > last || last > 0x10FFFF || last - code >= MAX_GLYPHS) return 0;
        for (; code <= last; code++) {
            if (glyph && !glyph_add(out, code, glyph)) return 0;
            glyph++;
            if (code == last) break;
        }
    }
    return 1;
}

/* the font's Unicode code points, from every Unicode subtable its cmap has, most complete last */
static int font_read(const u8* font, u64 size, Glyphs* out) {
    Table cmap;
    u64 i, count;
    int any = 0;
    memset(out, 0, sizeof *out);
    if (!find_table(font, size, "cmap", &cmap) || !in_range(font, size, cmap.offset, 4)) return 0;
    if (snx_ld_be16(font + cmap.offset) != 0) return 0;
    count = snx_ld_be16(font + cmap.offset + 2);
    if (!in_range(font, size, cmap.offset + 4, count * 8)) return 0;
    for (i = 0; i < count; i++) {
        const u8* record = font + cmap.offset + 4 + i * 8;
        u32 platform = snx_ld_be16(record), encoding = snx_ld_be16(record + 2), at = snx_ld_be32(record + 4);
        u64 sub = cmap.offset + at;
        u16 format;
        if (!(platform == 0 || (platform == 3 && (encoding == 0 || encoding == 1 || encoding == 10)))) continue;
        if (!in_range(font, size, sub, 2)) continue;
        format = snx_ld_be16(font + sub);
        if (format == 4 && cmap4(font, size, sub, out)) any = 1;
        if (format == 12 && cmap12(font, size, sub, out)) any = 1;
    }
    if (!any) {
        free(out->at);
        memset(out, 0, sizeof *out);
        return 0;
    }
    glyph_dedupe(out);
    return 1;
}

/* the console's icon block, added with the font's own space glyph where the font has no mapping already */
static int font_cover(Glyphs* g) {
    u32 code, blank;
    size_t i;
    int found;
    blank = glyph_find(g, SPACE, &found);
    if (!found) {
        blank = 0;
        for (i = 0; i < g->n && !blank; i++) blank = g->at[i].glyph;
    }
    if (!blank) return 0; /* nothing to answer with: the font has no glyph at all */
    {
        unsigned char have[(PUA_LAST - PUA_FIRST + 8) / 8] = {0};
        for (i = 0; i < g->n; i++) {
            if (g->at[i].code >= PUA_FIRST && g->at[i].code <= PUA_LAST)
                have[(g->at[i].code - PUA_FIRST) / 8] |= (unsigned char)(1u << ((g->at[i].code - PUA_FIRST) % 8));
        }
        for (code = PUA_FIRST; code <= PUA_LAST; code++)
            if (!(have[(code - PUA_FIRST) / 8] & (1u << ((code - PUA_FIRST) % 8))) && !glyph_add(g, code, blank))
                return 0;
    }
    glyph_dedupe(g);
    return 1;
}

/* the runs of code points a format 4 subtable needs: contiguous codes with one glyph-to-code delta */
static size_t f4_runs(const Glyphs* g) {
    size_t i, runs = 0;
    u32 last = 0;
    int have = 0;
    u16 delta = 0;
    for (i = 0; i < g->n; i++) {
        if (g->at[i].code > 0xFFFE || !g->at[i].glyph) continue;
        if (!have || g->at[i].code != last + 1 || (u16)(g->at[i].glyph - g->at[i].code) != delta) {
            runs++;
            delta = (u16)(g->at[i].glyph - g->at[i].code);
        }
        last = g->at[i].code;
        have = 1;
    }
    return runs;
}

static size_t f4_size(const Glyphs* g) {
    return 16 + 8 * (f4_runs(g) + 1);
}

/* a format 4 subtable of the code points a BMP cmap can hold, then the 0xFFFF sentinel */
static size_t f4_write(u8* p, const Glyphs* g) {
    size_t i, run, runs = f4_runs(g), seg = runs + 1;
    u16 entry = 0, search;
    u32 last = 0;
    int have = 0;
    u16 delta = 0;
    memset(p, 0, f4_size(g));
    while ((1u << (entry + 1)) <= seg) entry++;
    search = (u16)(2 << entry);
    snx_st_be16(p, 4);
    snx_st_be16(p + 2, (u16)f4_size(g));
    snx_st_be16(p + 6, (u16)(seg * 2));
    snx_st_be16(p + 8, search);
    snx_st_be16(p + 10, entry);
    snx_st_be16(p + 12, (u16)(seg * 2 - search));
    run = 0;
    for (i = 0; i < g->n; i++) {
        if (g->at[i].code > 0xFFFE || !g->at[i].glyph) continue;
        if (!have || g->at[i].code != last + 1 || (u16)(g->at[i].glyph - g->at[i].code) != delta) {
            if (have) run++;
            delta = (u16)(g->at[i].glyph - g->at[i].code);
            snx_st_be16(p + 16 + 2 * seg + 2 * run, (u16)g->at[i].code);
            snx_st_be16(p + 16 + 4 * seg + 2 * run, delta);
        }
        snx_st_be16(p + 14 + 2 * run, (u16)g->at[i].code);
        last = g->at[i].code;
        have = 1;
    }
    snx_st_be16(p + 14 + 2 * runs, 0xFFFF);
    snx_st_be16(p + 16 + 2 * seg + 2 * runs, 0xFFFF);
    snx_st_be16(p + 16 + 4 * seg + 2 * runs, 1);
    return f4_size(g);
}

/* the groups of a format 12 subtable: contiguous codes with contiguous glyphs */
static size_t f12_groups(const Glyphs* g) {
    size_t i, groups = 0;
    u32 last = 0, last_glyph = 0;
    for (i = 0; i < g->n; i++) {
        if (!g->at[i].glyph) continue;
        if (!groups || g->at[i].code != last + 1 || g->at[i].glyph != last_glyph + 1) groups++;
        last = g->at[i].code;
        last_glyph = g->at[i].glyph;
    }
    return groups;
}

static size_t f12_size(const Glyphs* g) {
    return 16 + 12 * f12_groups(g);
}

static size_t f12_write(u8* p, const Glyphs* g) {
    size_t i, group = 0;
    u32 last = 0, last_glyph = 0;
    memset(p, 0, f12_size(g));
    snx_st_be16(p, 12);
    snx_st_be32(p + 4, (u32)f12_size(g));
    snx_st_be32(p + 12, (u32)f12_groups(g));
    for (i = 0; i < g->n; i++) {
        if (!g->at[i].glyph) continue;
        if (!group || g->at[i].code != last + 1 || g->at[i].glyph != last_glyph + 1) {
            snx_st_be32(p + 16 + 12 * group, g->at[i].code);
            snx_st_be32(p + 20 + 12 * group, g->at[i].code);
            snx_st_be32(p + 24 + 12 * group, g->at[i].glyph);
            group++;
        } else {
            snx_st_be32(p + 20 + 12 * (group - 1), g->at[i].code);
        }
        last = g->at[i].code;
        last_glyph = g->at[i].glyph;
    }
    return f12_size(g);
}

static u32 checksum(const u8* p, u64 size) {
    u64 i;
    u32 sum = 0;
    for (i = 0; i + 4 <= size; i += 4) sum += snx_ld_be32(p + i);
    if (i < size) {
        u8 tail[4] = {0, 0, 0, 0};
        memcpy(tail, p + i, (size_t)(size - i));
        sum += snx_ld_be32(tail);
    }
    return sum;
}

/* font with the PUA entries in a new cmap, the old one left in place */
static int font_patch(const u8* font, u64 size, const Glyphs* g, u8** out, u64* out_size) {
    Table cmap, head;
    u8* record;
    u64 f4 = 36, f12, new_size, at;
    size_t f4_len, f12_len, cmap_len;
    u8* patched;
    if (!find_table(font, size, "cmap", &cmap) || !find_table(font, size, "head", &head) ||
        !in_range(font, size, head.offset, 54))
        return 0;
    f4_len = f4_size(g);
    f12_len = f12_size(g);
    if (f4_len > 0xFFFF) return 0; /* a format 4 subtable's length is a 16-bit field */
    f12 = f4 + f4_len;
    cmap_len = f12 + f12_len;
    at = (size + 3) & ~(u64)3;
    if (at > SIZE_MAX - cmap_len) return 0;
    new_size = at + cmap_len;
    patched = calloc(1, (size_t)new_size);
    if (!patched) return 0;
    memcpy(patched, font, (size_t)size);
    record = patched + (u64)(cmap.record - (u8*)font);
    f4_write(patched + at + f4, g);
    f12_write(patched + at + f12, g);
    snx_st_be16(patched + at, 0);  /* the cmap table: version, four encoding records */
    snx_st_be16(patched + at + 2, 4);
    snx_st_be16(patched + at + 4, 0);           /* platform 0 (Unicode), format 4 */
    snx_st_be16(patched + at + 6, 3);
    snx_st_be32(patched + at + 8, (u32)f4);
    snx_st_be16(patched + at + 12, 0);          /* platform 0, format 12 */
    snx_st_be16(patched + at + 14, 4);
    snx_st_be32(patched + at + 16, (u32)f12);
    snx_st_be16(patched + at + 20, 3);          /* platform 3 (Windows), format 4 */
    snx_st_be16(patched + at + 22, 1);
    snx_st_be32(patched + at + 24, (u32)f4);
    snx_st_be16(patched + at + 28, 3);          /* platform 3, format 12 */
    snx_st_be16(patched + at + 30, 10);
    snx_st_be32(patched + at + 32, (u32)f12);
    snx_st_be32(record + 4, checksum(patched + at, cmap_len));
    snx_st_be32(record + 8, (u32)at);
    snx_st_be32(record + 12, (u32)cmap_len);
    snx_st_be32(patched + head.offset + 8, 0);
    snx_st_be32(patched + head.offset + 8, 0xB1B0AFBAu - checksum(patched, new_size));
    *out = patched;
    *out_size = new_size;
    return 1;
}

u8* pl_font_cover(const u8* font, u64 size, u64* out) {
    Glyphs glyphs;
    u8* patched = NULL;
    u64 patched_size = 0;
    *out = 0;
    if (!font || !size || !font_read(font, size, &glyphs)) return NULL;
    if (font_cover(&glyphs) && font_patch(font, size, &glyphs, &patched, &patched_size)) *out = patched_size;
    free(glyphs.at);
    return patched;
}
