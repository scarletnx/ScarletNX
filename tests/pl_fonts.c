/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* pl.c's shared fonts with no fonts/ in the game folder: the stand-ins compiled in (cmake/shared_fonts.cmake), the
   Noto Sans JP, SC, TC and KR subsets for slots 0 to 4 and the button glyphs in slot 5, each a TrueType font behind
   the obfuscated header the SDK decodes its size from, its private-use block blanked; and a game folder's .ttf taken
   over the stand-in for its slot */
#include "nx/hle/services/pl.c"
#include "nx/hle/hle.c"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */

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
/* what the IPC side of pl.c reaches: the handle a command gives, the copy handles of a reply, and how many commands
   went to the stub that answers an undeclared one */
u8* g_mem;
#define SHM_HANDLE 0x1234u
static u32 g_copied[2], g_ncopied, g_undeclared;
u32 h_new(KObj* o) {
    (void)o;
    return SHM_HANDLE;
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    g_undeclared++;
    return 0;
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
void out_copy_h(IpcCtx* c, u32 h) {
    (void)c;
    if (g_ncopied < 2) g_copied[g_ncopied] = h;
    g_ncopied++;
}
void svc_register(const char* name, SvcCtor ctor) { (void)name, (void)ctor; }
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    (void)name, (void)dispatch;
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

/* a command of the service's with n bytes of raw data, through its dispatcher: the reply is in c */
static void command(IpcCtx* c, u32 cmd, const u8* raw, u32 n) {
    static u8 data[8];
    CHECK(n <= sizeof data);
    memset(c, 0, sizeof *c);
    memset(data, 0, sizeof data);
    if (n) memcpy(data, raw, n);
    c->cmd = cmd;
    c->raw = data;
    c->raw_size = n;
    g_ncopied = g_undeclared = 0;
    CHECK(nn_pl_detail_IPlatformServiceManager_dispatch(NULL, c) == 0);
    CHECK(!g_undeclared); /* a command the service declares */
}
static u32 command_u32(u32 cmd, u32 font_type) {
    IpcCtx c;
    u8 raw[4];
    snx_st_le32(raw, font_type);
    command(&c, cmd, raw, sizeof raw);
    CHECK(c.out_size == 4);
    return snx_ld_le32(c.out);
}
/* the built-in fonts of the later SDK: 2000 gives the shared memory of the fonts and its size, 2010 where a font is
   in it, the same font at the same place as 3 and 2 give for its type; a type past the six is not found */
static void built_in_fonts(void) {
    IpcCtx c;
    u32 type;
    command(&c, 2000, NULL, 0);
    CHECK(c.out_size == 4 && snx_ld_le32(c.out) == sizeof shm);
    CHECK(g_ncopied == 1 && g_copied[0] == SHM_HANDLE);
    for (type = 0; type < 8; type++) {
        static const u8 none[0x18];
        u8 raw[2];
        raw[0] = (u8)type, raw[1] = 0;
        command(&c, 2010, raw, sizeof raw);
        CHECK(c.out_size == 0x18 && !g_ncopied);
        if (type >= 6) {
            CHECK(!memcmp(c.out, none, sizeof none));
            continue;
        }
        CHECK(c.out[0] == 1 && !memcmp(c.out + 1, none, 7)); /* found, and the padding up to the descriptor */
        CHECK(snx_ld_le32(c.out + 8) == 0);
        CHECK(snx_ld_le32(c.out + 8 + 4) == command_u32(3, type) && snx_ld_le32(c.out + 8 + 4) != 0);
        CHECK(snx_ld_le32(c.out + 8 + 8) == command_u32(2, type) && snx_ld_le32(c.out + 8 + 8) != 0);
        CHECK(snx_ld_le32(c.out + 8 + 12) == 0);
        CHECK(truetype(snx_ld_le32(c.out + 8 + 4)));
    }
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
    built_in_fonts();

    memset(shm, 0, sizeof shm);
    memset(&g_hle.pl, 0, sizeof g_hle.pl);
    g_have = "fonts/FontKorean.ttf";
    fonts_fill(shm, sizeof shm);
    CHECK(g_hle.pl.font_size[4] == sizeof g_korean && !memcmp(shm + g_hle.pl.font_off[4], g_korean, sizeof g_korean));
    CHECK(g_hle.pl.font_off[2] == g_hle.pl.font_off[1]);
    CHECK(g_hle.pl.font_size[1] > snx_font_chinese_simplified_size);
    CHECK(g_hle.pl.font_size[0] > snx_font_japanese_size && g_hle.pl.font_size[3] > snx_font_chinese_traditional_size);
    { /* this test's handle table gives no handle, as the kernel's does when full: OutOfHandles, not success with 0 */
        u32 handle = 7;
        CHECK((u32)nn_pl_detail_IPlatformServiceManager_GetSharedMemoryNativeHandle(NULL, &handle) ==
                  SNX_RC_SVC_OUT_OF_HANDLES &&
              handle == 0);
    }
    printf("pl_fonts: %u checks passed\n", checks);
    return 0;
}
