/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::fs (src/nx/nn/fs.c) on a stand-in CPU, with the file server's core stubbed to two RomFS images of no
   files, the game's and a system archive's: what the SDK's C library asks of a path before it opens it (whether its
   mount is there, and the limits of its file system), answered for a native mount and left to the SDK's own code for
   any other */
#include "nx/nn/fs.c"
#include <stdio.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

Hle g_hle;
static CPU cpu;
static u8 mem[0x2000];

/* a RomFS image of one root directory and no files: the header, then its four tables. The tables' sizes differ in
   the two images, so each one's cache size tells them apart */
#define SYSTEM_ID 0x0100000000000823ull
static u8 game_image[0x100], data_image_bytes[0x100];
static void image(u8* b, u64 dir_hash_size) {
    u64 dir_hash = 0x50, dirs = dir_hash + dir_hash_size, file_hash = dirs + 0x18, files = file_hash + 4;
    memset(b, 0, 0x100);
    snx_st_le64(b, 0x50);
    snx_st_le64(b + 0x08, dir_hash), snx_st_le64(b + 0x10, dir_hash_size);
    snx_st_le64(b + 0x18, dirs), snx_st_le64(b + 0x20, 0x18);
    snx_st_le64(b + 0x28, file_hash), snx_st_le64(b + 0x30, 4);
    snx_st_le64(b + 0x38, files), snx_st_le64(b + 0x40, 0);
    snx_st_le64(b + 0x48, files);
    memset(b + dir_hash, 0xff, dir_hash_size);                      /* empty buckets */
    memset(b + dirs + 4, 0xff, 12), memset(b + dirs + 16, 0xff, 4); /* the root: no sibling, child or file */
    memset(b + file_hash, 0xff, 4);
}
static int image_read(const u8* b, u64 off, void* dst, u64 size) {
    if (off > 0x100 || size > 0x100 - off) return -1;
    memcpy(dst, b + off, (size_t)size);
    return 0;
}

/* ---- the file server's core (fs.h), as far as these checks reach it */
int romfs_peek(u64 off, void* dst, u64 size) { return image_read(game_image, off, dst, size); }
int romfs_read_guest(u64 off, u64 addr, u64 size) { return image_read(game_image, off, mem + addr, size); }
/* the reads the native ReadFile hands to fs.c's read pacing: how many, and the last one's storage and range */
static unsigned paced;
static const void* paced_storage;
static u64 paced_off, paced_size;
void fs_pace_read(const void* storage, u64 off, u64 sz) {
    paced++;
    paced_storage = storage;
    paced_off = off;
    paced_size = sz;
}
static unsigned storages_open;
u32 fs_storage_open(u64 data_id, FsStorage* out) {
    *out = (FsStorage){0};
    if (data_id != SYSTEM_ID) return FSR(1002);
    storages_open++;
    return 0;
}
void fs_storage_close(FsStorage* st) {
    (void)st;
    storages_open--;
}
int fs_storage_read(FsStorage* st, u64 off, void* dst, u64 size) {
    (void)st;
    return image_read(data_image_bytes, off, dst, size);
}
int fs_storage_read_guest(FsStorage* st, u64 off, u64 addr, u64 size) {
    (void)st;
    return image_read(data_image_bytes, off, mem + addr, size);
}
void fs_lock(void) {}
void fs_unlock(void) {}
u32 fs_host_root_result(void) { return FSR(1); }
u32 fs_sd_root(FsNode** root) { return (void)root, FSR(1); }
u32 fs_save_root(u8 type, const u8 uid[16], u16 index, FsNode** root, int* noted) {
    return (void)type, (void)uid, (void)index, (void)root, (void)noted, FSR(1);
}
void fs_root_release(FsNode* root) { (void)root; }
void fs_root_commit(FsNode* root, int noted) { (void)root, (void)noted; }
u32 fs_root_create(FsNode* root, const char* path, i64 size, int dir) {
    return (void)root, (void)path, (void)size, (void)dir, FSR(1);
}
u32 fs_root_remove(FsNode* root, const char* path, int dir, int recursive, int clean) {
    return (void)root, (void)path, (void)dir, (void)recursive, (void)clean, FSR(1);
}
u32 fs_root_rename(FsNode* root, const char* from, const char* to, int dir) {
    return (void)root, (void)from, (void)to, (void)dir, FSR(1);
}
u32 fs_root_entry_type(FsNode* root, const char* path, u32* type) { return (void)root, (void)path, (void)type, FSR(1); }
i64 fs_root_space(int total) { return (void)total, 0; }
u32 fs_file_open(FsNode* root, const char* path, u32 mode, FileData* out) {
    return (void)root, (void)path, (void)mode, (void)out, FSR(1);
}
void fs_file_close(FileData* f) { (void)f; }
u32 fs_file_flush(FileData* f) { return (void)f, 0; }
u32 fs_file_read(FileData* f, u64 off, u64 addr, u64 size, u64* n) {
    return (void)f, (void)off, (void)addr, (void)size, (void)n, FSR(1);
}
u32 fs_file_write(FileData* f, u32 option, u64 off, u64 addr, u64 size) {
    return (void)f, (void)option, (void)off, (void)addr, (void)size, FSR(1);
}
u32 fs_file_set_size(FileData* f, i64 size) { return (void)f, (void)size, FSR(1); }
u32 fs_dir_open(FsNode* root, const char* path, u32 mode, DirData* out) {
    return (void)root, (void)path, (void)mode, (void)out, FSR(1);
}
void fs_dir_close(DirData* d) { (void)d; }
u64 fs_dir_count(const DirData* d) { return (void)d, 0; }
u32 fs_dir_read(DirData* d, u64 addr, u64 max, u64* count) {
    return (void)d, (void)addr, (void)max, (void)count, FSR(1);
}
void am_save_data_extend(u8 type, i64 normal_size, i64 journal_size) {
    (void)type, (void)normal_size, (void)journal_size;
}
void am_save_data_size(u8 type, i64* normal_size, i64* journal_size) {
    (void)type, (void)normal_size, (void)journal_size;
}
u64 aoc_base_id(u64 application_id) { return application_id + 0x1000; }
int snx_utf8_valid(const char* text, size_t n, char* hex, size_t hex_size) {
    return (void)text, (void)n, (void)hex, (void)hex_size, 1;
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void fatal(const char* fmt, ...) {
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}

/* ---- hook.c's: each function is hooked somewhere, and the SDK's own code answers 0x77, counted by its symbol */
static unsigned own_runs;
static char own_symbol[96];
static const char* site_symbol[8];
u64 hook_site(const char* symbol) {
    for (u64 i = 0; i < 8; i++) {
        if (!site_symbol[i]) site_symbol[i] = symbol;
        if (site_symbol[i] == symbol || !strcmp(site_symbol[i], symbol)) return 0x1000 + 4 * i;
    }
    return 0;
}
void hook_run_original(CPU* c, u64 addr) {
    own_runs++;
    snprintf(own_symbol, sizeof own_symbol, "%s", site_symbol[(addr - 0x1000) / 4]);
    c->x[0] = 0x77;
}
static int own(const char* symbol, unsigned runs) { return own_runs == runs && !strcmp(own_symbol, symbol); }

#define NAME  0x100u /* a mount name */
#define PATH  0x140u /* a path */
#define RESULT   0x200u /* a result's place, 0xee before each call */
#define HAS_AT   0x220u
#define CACHE 0x400u
static void text(u32 at, const char* s) { strcpy((char*)mem + at, s); }
static void outs(void) { memset(mem + RESULT, 0xee, 0x40); }

int main(void) {
    CPU* c = &cpu;
    cpu.mem = mem;
    nnfs_init();
    image(game_image, 4);
    image(data_image_bytes, 12);

    /* no native mount yet: every question about a path is the SDK's own */
    text(PATH, "rom:/icu/data.dat");
    CHECK(nnfs_is_path_mounted(c, PATH) == 1 && own(SYM_IS_PATH_MOUNTED, 1)); /* 0x77's bit 0 */
    outs();
    CHECK(nnfs_get_path_length_max(c, RESULT, PATH) == 0x77 && own(SYM_PATH_MAX, 2) && mem[RESULT] == 0xee);

    /* the game's RomFS, mounted natively by name: its paths are mounted, with the limits the SDK takes from an
       attribute that names none, 0x300 for a path and for a name and no UTF-16 limit; nothing of the SDK's runs */
    text(NAME, "rom");
    CHECK(nnfs_mount_rom_name(c, NAME) == 0);
    CHECK(nnfs_is_path_mounted(c, PATH) == 1 && own_runs == 2);
    outs();
    CHECK(nnfs_get_path_length_max(c, RESULT, PATH) == 0 && snx_ld_le64(mem + RESULT) == 0x300 && mem[RESULT + 8] == 0xee);
    outs();
    CHECK(nnfs_get_entry_name_length_max(c, RESULT, PATH) == 0 && snx_ld_le64(mem + RESULT) == 0x300);
    outs();
    CHECK(nnfs_get_utf16_path_length_max(c, HAS_AT, RESULT, PATH) == 0 && mem[HAS_AT] == 0 && mem[HAS_AT + 1] == 0xee);
    CHECK(snx_ld_le64(mem + RESULT) == 0);
    outs();
    CHECK(nnfs_get_utf16_entry_name_length_max(c, HAS_AT, RESULT, PATH) == 0 && mem[HAS_AT] == 0 && snx_ld_le64(mem + RESULT) == 0);
    CHECK(own_runs == 2);
    /* a path on another mount, one with no mount name, and no path at all stay the SDK's */
    text(PATH, "save:/file");
    CHECK(nnfs_is_path_mounted(c, PATH) == 1 && own(SYM_IS_PATH_MOUNTED, 3));
    CHECK(nnfs_get_entry_name_length_max(c, RESULT, PATH) == 0x77 && own(SYM_ENTRY_NAME_MAX, 4));
    CHECK(nnfs_get_utf16_path_length_max(c, HAS_AT, RESULT, PATH) == 0x77 && own(SYM_UTF16_PATH_MAX, 5));
    CHECK(nnfs_get_utf16_entry_name_length_max(c, HAS_AT, RESULT, PATH) == 0x77 && own(SYM_UTF16_NAME_MAX, 6));
    text(PATH, "no mount name");
    CHECK(nnfs_is_path_mounted(c, PATH) == 1 && own_runs == 7);
    CHECK(nnfs_is_path_mounted(c, 0) == 1 && own_runs == 8);

    printf("nn_fs: %u checks passed\n", checks);
    return 0;
}
