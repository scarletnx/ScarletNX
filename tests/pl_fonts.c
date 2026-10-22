/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* pl.c's shared fonts with no fonts/ in the game folder: the stand-ins compiled in (cmake/shared_fonts.cmake), DejaVu
   Sans once for slots 0 to 4 and the button glyphs in slot 5, each a TrueType font behind the obfuscated header the
   SDK decodes its size from; and a game folder's .ttf taken over the stand-in for its slot */
#include "nx/hle/services/pl.c"
#include "nx/hle/hle.c"

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
/* what the IPC side of pl.c reaches, which the test does not run */
u8* g_mem;
u32 h_new(KObj* o) {
    (void)o;
    return 0;
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
void out_copy_h(IpcCtx* c, u32 h) { (void)c, (void)h; }
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


int main(void) {
    hle_init();
    fonts_fill(shm, sizeof shm);
    for (int i = 0; i < 5; i++) CHECK(g_hle.pl.font_off[i] == g_hle.pl.font_off[0] && g_hle.pl.font_size[i] == snx_font_standard_size);
    CHECK(!memcmp(shm + g_hle.pl.font_off[0], snx_font_standard, snx_font_standard_size) && truetype(g_hle.pl.font_off[0]));
    CHECK(g_hle.pl.font_size[5] == snx_font_extended_size && g_hle.pl.font_off[5] != g_hle.pl.font_off[0]);
    CHECK(!memcmp(shm + g_hle.pl.font_off[5], snx_font_extended, snx_font_extended_size) && truetype(g_hle.pl.font_off[5]));
    CHECK(header_size(g_hle.pl.font_off[0]) == ((snx_font_standard_size + 3) & ~3u));
    CHECK(header_size(g_hle.pl.font_off[5]) == ((snx_font_extended_size + 3) & ~3u));
    /* DejaVu Sans, as the font's name table says it */
    CHECK(contains(snx_font_standard, snx_font_standard_size, "DejaVuSans"));

    memset(shm, 0, sizeof shm);
    memset(&g_hle.pl, 0, sizeof g_hle.pl);
    g_have = "fonts/FontKorean.ttf";
    fonts_fill(shm, sizeof shm);
    CHECK(g_hle.pl.font_size[4] == sizeof g_korean && !memcmp(shm + g_hle.pl.font_off[4], g_korean, sizeof g_korean));
    CHECK(g_hle.pl.font_off[0] == g_hle.pl.font_off[3] && g_hle.pl.font_size[0] == snx_font_standard_size);
    printf("pl_fonts: %u checks passed\n", checks);
    return 0;
}
