/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::pl's shared fonts (src/nx/nn/pl.c) with no fonts/ in the game folder: the stand-ins compiled in
   (cmake/shared_fonts.cmake), the Noto Sans JP, SC, TC and KR subsets for slots 0 to 4 and the button glyphs in slot 5,
   each a TrueType font behind the obfuscated header the SDK decodes its size from, its private-use block blanked; a
   game folder's .ttf taken over the stand-in for its slot; and the hooks' answers on a stand-in CPU, the fonts at the
   address their memory was mapped at */
#include "nx/nn/pl.c"
#include "nx/hle/hle.c"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */
#include <setjmp.h>
#include <stdarg.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ the game folder: one font, or none */
static const char* g_have; /* the one path fs_read_file finds */
static const u8 g_korean[] = {0, 1, 0, 0, 'k', 'o', 'r', 'e', 'a', 'n'};
u8* fs_read_file(const char* path, u64* size) {
    if (!g_have || strcmp(path, g_have)) return NULL;
    u8* d = malloc(sizeof g_korean);
    memcpy(d, g_korean, sizeof g_korean);
    *size = sizeof g_korean;
    return d;
}
void kobj_ref(KObj* o) { (void)o; }
KShmem* kshmem_new(u64 size, const char* name) {
    (void)size, (void)name;
    return NULL;
}
u8* kshmem_ptr(KShmem* s) {
    (void)s;
    return NULL;
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}

static u8 shm[0x1100000];
/* the size the SDK reads from the 8 bytes before a font */
static u32 header_size(u32 off) {
    u32 key = 0x7f9a0218u ^ 0x36f81a1eu;
    CHECK(snx_ld_be32(shm + off - 8) == 0x7f9a0218u); /* the decrypted magic, not the .bfttf's own */
    return snx_ld_be32(shm + off - 4) ^ key;
}
static int truetype(u32 off) { return !memcmp(shm + off, "\0\1\0\0", 4); }
/* memmem is a GNU extension, which Windows' C library lacks */
static int contains(const u8* data, size_t size, const char* text) {
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= size; i++)
        if (!memcmp(data + i, text, n)) return 1;
    return 0;
}

/* the glyph a font's cmap gives code, 0 for none: the format 12 subtable (platform 3, encoding 10) when there is
   one, else the format 4 (platform 3, encoding 1), as the console's SDK reads them */
static u32 cmap_lookup(const u8* font, size_t size, u32 code) {
    size_t i, tables = snx_ld_be16(font + 4), cmap = 0;
    size_t records, best = 0;
    if (size < 12) return 0;
    for (i = 0; i < tables; i++) {
        if (!memcmp(font + 12 + i * 16, "cmap", 4)) cmap = snx_ld_be32(font + 12 + i * 16 + 8);
    }
    if (!cmap || cmap + 4 > size) return 0;
    records = snx_ld_be16(font + cmap + 2);
    for (i = 0; i < records; i++) {
        const u8* record = font + cmap + 4 + i * 8;
        u32 platform = snx_ld_be16(record), encoding = snx_ld_be16(record + 2), at = cmap + snx_ld_be32(record + 4);
        if (platform != 3 || (encoding != 1 && encoding != 10) || at + 2 > size) continue;
        if (best && snx_ld_be16(font + best) >= snx_ld_be16(font + at)) continue;
        best = at; /* format 12 past format 4 */
    }
    if (!best) return 0;
    if (snx_ld_be16(font + best) == 12) {
        u32 groups = snx_ld_be32(font + best + 12);
        for (i = 0; i < groups; i++) {
            const u8* group = font + best + 16 + i * 12;
            u32 first = snx_ld_be32(group), last = snx_ld_be32(group + 4), glyph = snx_ld_be32(group + 8);
            if (code >= first && code <= last) return glyph + (code - first);
        }
        return 0;
    }
    if (snx_ld_be16(font + best) == 4) {
        u16 count = snx_ld_be16(font + best + 6) / 2;
        const u8* ends = font + best + 14;
        const u8* starts = ends + 2 + 2 * count;
        const u8* deltas = starts + 2 * count;
        const u8* ranges = deltas + 2 * count;
        for (i = 0; i < count; i++) {
            u32 first = snx_ld_be16(starts + 2 * i), last = snx_ld_be16(ends + 2 * i);
            u16 delta = snx_ld_be16(deltas + 2 * i), range = snx_ld_be16(ranges + 2 * i);
            if (code < first || code > last) continue;
            if (!range) return (u16)(code + delta);
            {
                size_t cell = (size_t)(ranges + 2 * i - font) + range + 2 * (code - first);
                u16 glyph = cell + 2 <= size ? snx_ld_be16(font + cell) : 0;
                return glyph ? (u16)(glyph + delta) : 0;
            }
        }
    }
    return 0;
}

/* the font's icon block: every character there, and the space glyph when the font had no icon of its own */
static void covered(u32 off, u32 size, int own_glyphs) {
    u32 i, space = cmap_lookup(shm + off, size, ' ');
    CHECK(space != 0);
    for (i = 0xE000; i <= 0xE1FF; i++) {
        CHECK(cmap_lookup(shm + off, size, i) != 0);
        if (!own_glyphs) CHECK(cmap_lookup(shm + off, size, i) == space);
    }
    if (own_glyphs) CHECK(cmap_lookup(shm + off, size, 0xE0E0) != space); /* the button glyph is its own */
    else CHECK(cmap_lookup(shm + off, size, 'A') != 0);
}

/* the kernel's mapping of the fonts' memory: once, at one address */
#define FONTS_BASE 0x6000000ull
static unsigned g_maps;
static int g_no_pages; /* the game's address space has no room for the fonts */
u64 kshmem_map_free(KShmem* s, u32 perm) {
    (void)s;
    CHECK(perm == PERM_R);
    if (g_no_pages) return 0;
    g_maps++;
    return FONTS_BASE;
}

/* fatal: the game stops, which one check expects; its message is kept for it */
static jmp_buf g_stopped;
static int g_expect_stop;
static char g_stop_text[160];
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_stop_text, sizeof g_stop_text, fmt, a);
    va_end(a);
    if (!g_expect_stop) {
        fprintf(stderr, "%s\n", g_stop_text);
        exit(1);
    }
    longjmp(g_stopped, 1);
}

/* the hooks: every font loaded, its size and its address in the mapped memory, and the lists of all six */
static void hooks(void) {
    static CPU cpu;
    static u8 mem[0x1000];
    CPU* c = &cpu;
    cpu.mem = mem;
    /* no room to map the fonts: the game stops, and the message gives the size asked for */
    g_no_pages = g_expect_stop = 1;
    if (!setjmp(g_stopped)) {
        nnpl_get_shared_font_address(c, 0);
        CHECK(!"the game went on with no fonts mapped");
    }
    g_no_pages = g_expect_stop = 0;
    CHECK(strstr(g_stop_text, "0x1100000") && !g_maps && !g_hle.pl.base);
    for (i32 t = 0; t < 6; t++) {
        nnpl_request_shared_font_load(c, t);
        CHECK(nnpl_get_shared_font_load_state(c, t) == 1);
        CHECK(nnpl_get_shared_font_size(c, t) == g_hle.pl.font_size[t]);
        CHECK(nnpl_get_shared_font_address(c, t) == FONTS_BASE + g_hle.pl.font_off[t]);
    }
    CHECK(g_maps == 1 && g_hle.pl.base == FONTS_BASE);
    CHECK(nnpl_get_shared_font_size(c, 6) == 0 && nnpl_get_shared_font_size(c, -1) == 0);
    /* a built-in font is the shared font of its type: the flags, its address in the mapped memory and its size, the
       bytes between left; a type with no font answers false and leaves the caller's structure */
    for (u8 t = 0; t < 6; t++) {
        memset(mem, 0xee, 0x40);
        CHECK(nnpl_get_built_in_font_data(c, 0x10, t, 1) == 1 && mem[0x10] == 1 && mem[0x11] == 0 && mem[0x12] == 0xee);
        CHECK(snx_ld_le64(mem + 0x18) == FONTS_BASE + g_hle.pl.font_off[t]);
        CHECK(snx_ld_le64(mem + 0x20) == g_hle.pl.font_size[t] && mem[0x28] == 0xee);
    }
    memset(mem, 0xee, 0x40);
    CHECK(nnpl_get_built_in_font_data(c, 0x10, 6, 0) == 0 && nnpl_get_built_in_font_data(c, 0x10, 0xff, 0) == 0);
    CHECK(mem[0x10] == 0xee && mem[0x18] == 0xee && g_maps == 1);
    /* the three arrays, as many entries as max allows, and the count of all six */
    memset(mem, 0xee, sizeof mem);
    CHECK(nnpl_get_shared_font_in_order_of_priority(c, 0x10, 0x20, 0x100, 0x200, 4, 0x53552d6e65ull));
    CHECK(snx_ld_le32(mem + 0x10) == 6);
    for (u32 i = 0; i < 4; i++) {
        CHECK(snx_ld_le32(mem + 0x20 + 4 * i) == i);
        CHECK(snx_ld_le64(mem + 0x100 + 8 * i) == FONTS_BASE + g_hle.pl.font_off[i]);
        CHECK(snx_ld_le64(mem + 0x200 + 8 * i) == g_hle.pl.font_size[i]);
    }
    CHECK(mem[0x20 + 16] == 0xee && mem[0x100 + 32] == 0xee); /* the fifth entry is past max */
    CHECK(nnpl_get_shared_font_in_order_of_priority_for_system(c, 0x10, 0x20, 0x100, 0x200, 7, 0));
    CHECK(snx_ld_le32(mem + 0x10) == 6 && snx_ld_le32(mem + 0x20 + 20) == 5 && mem[0x20 + 24] == 0xee);
    /* SharedFontData: the type, the address and the size, its padding left */
    memset(mem, 0xee, sizeof mem);
    CHECK(nnpl_get_shared_font(c, 0x10, 0x400, 6, 0));
    for (u32 i = 0; i < 6; i++) {
        const u8* d = mem + 0x400 + 0x18 * i;
        CHECK(snx_ld_le32(d) == i && snx_ld_le32(d + 4) == 0xeeeeeeeeu);
        CHECK(snx_ld_le64(d + 8) == FONTS_BASE + g_hle.pl.font_off[i] && snx_ld_le64(d + 0x10) == g_hle.pl.font_size[i]);
    }
    CHECK(nnpl_get_shared_font(c, 0x10, 0x400, 0, 0) && snx_ld_le32(mem + 0x10) == 6); /* no entry, the count */
    CHECK(g_maps == 1);
}

int main(void) {
    hle_init();
    fonts_fill(shm, sizeof shm);
    CHECK(g_hle.pl.font_size[0] > snx_font_japanese_size); /* the cmap with the icon block went after the font */
    CHECK(g_hle.pl.font_size[1] > snx_font_chinese_simplified_size);
    CHECK(g_hle.pl.font_size[3] > snx_font_chinese_traditional_size);
    CHECK(g_hle.pl.font_size[4] > snx_font_korean_size);
    CHECK(g_hle.pl.font_size[5] > snx_font_extended_size);
    CHECK(g_hle.pl.font_off[2] == g_hle.pl.font_off[1] && g_hle.pl.font_size[2] == g_hle.pl.font_size[1]);
    CHECK(g_hle.pl.font_off[0] != g_hle.pl.font_off[1] && g_hle.pl.font_off[1] != g_hle.pl.font_off[3] &&
          g_hle.pl.font_off[3] != g_hle.pl.font_off[4] && g_hle.pl.font_off[4] != g_hle.pl.font_off[5]);
    for (int i = 0; i < 6; i++) {
        CHECK(truetype(g_hle.pl.font_off[i]));
        CHECK(header_size(g_hle.pl.font_off[i]) == ((g_hle.pl.font_size[i] + 3) & ~3u));
    }
    CHECK(contains(shm + g_hle.pl.font_off[5], g_hle.pl.font_size[5], "YuzuOSSExtension"));
    covered(g_hle.pl.font_off[0], g_hle.pl.font_size[0], 0);
    covered(g_hle.pl.font_off[1], g_hle.pl.font_size[1], 0);
    covered(g_hle.pl.font_off[3], g_hle.pl.font_size[3], 0);
    covered(g_hle.pl.font_off[4], g_hle.pl.font_size[4], 0);
    covered(g_hle.pl.font_off[5], g_hle.pl.font_size[5], 1);
    hooks();

    memset(shm, 0, sizeof shm);
    memset(&g_hle.pl, 0, sizeof g_hle.pl);
    g_have = "fonts/FontKorean.ttf";
    fonts_fill(shm, sizeof shm);
    CHECK(g_hle.pl.font_size[4] == sizeof g_korean && !memcmp(shm + g_hle.pl.font_off[4], g_korean, sizeof g_korean));
    CHECK(g_hle.pl.font_off[2] == g_hle.pl.font_off[1]);
    CHECK(g_hle.pl.font_size[1] > snx_font_chinese_simplified_size);
    CHECK(g_hle.pl.font_size[0] > snx_font_japanese_size && g_hle.pl.font_size[3] > snx_font_chinese_traditional_size);
    printf("pl_fonts: %u checks passed\n", checks);
    return 0;
}
