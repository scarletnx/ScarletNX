/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::fs (src/nx/nn/fs.c) on a stand-in CPU, with the file server's core stubbed to two RomFS images of no
   files, the game's and a system archive's: what the SDK's C library asks of a path before it opens it (whether its
   mount is there, and the limits of its file system), answered for a native mount and left to the SDK's own code for
   any other; and a system archive mounted by its data id, with the cache size its tables need. Then a file of that
   archive opened, read and closed by a 64-bit game and by a 32-bit one through its table's adapters (fs.hook's a32):
   the handle and the bytes read take 8 bytes of the game's memory in the first and 4 in the second, and a handle
   the SDK made goes back to the SDK's own code in both */
#include "nx/nn/fs.c"
#include "hle/hook/hooks.hook.inc" /* fs.hook's table, and a 32-bit game's */
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
/* the save asked for last: its type, and whether a user owns it */
static int save_type = -1, save_owned;
/* no save (FSR(1)) until save_given is 0 or 1: then the root save_root, an Account save's noted, and save_given as
   its journaled */
static FsNode* const save_root = (FsNode*)(uintptr_t)0x5a;
static int save_given = -1;
u32 fs_save_root(u8 type, const u8 uid[16], u16 index, FsNode** root, int* noted, int* journaled) {
    static const u8 nobody[16];
    save_type = type, save_owned = memcmp(uid, nobody, 16) != 0;
    (void)index;
    if (save_given < 0) return FSR(1);
    *root = save_root, *noted = type == 1, *journaled = save_given;
    return 0;
}
void fs_root_release(FsNode* root) { (void)root; }
/* the last commit of a save: its root, whether it was a journaled save's (fs_save_commit) and its noted; the Result a
   journaled one answers */
static FsNode* committed;
static int committed_noted, committed_journaled;
static u32 commit_result;
u32 fs_save_commit(FsNode* work, int noted) {
    committed = work, committed_noted = noted, committed_journaled = 1;
    return commit_result;
}
void fs_root_commit(FsNode* root, int noted) { committed = root, committed_noted = noted, committed_journaled = 0; }
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
/* a 32-bit game's: the own code of the function its thread is in, which answers 0x78 */
static SnxArmModule module32;
const SnxArmModule* g_arm_mod;
static SnxArmCPU a;
static unsigned own_runs32;
u64 hook_run_original_a32(CPU* c) {
    (void)c;
    own_runs32++;
    a.r[0] = 0x78;
    return 0x78;
}
CPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu; }
/* a 32-bit game's adapter for symbol, which its table has */
static SnxArmBlock a32(const char* symbol) {
    for (int i = 0; i < HOOK32_COUNT; i++)
        if (!strcmp(hook32_symbol[i], symbol)) return hook32_fn[i];
    fprintf(stderr, "%s is not in a 32-bit game's table\n", symbol);
    exit(1);
}
/* a call of a 32-bit game's: r0 to r3, and the words of the stack, at STACK */
#define STACK 0x1f00u
static u32 call32(const char* symbol, u32 r0, u32 r1, u32 r2, u32 r3, u32 s0, u32 s1, u32 s2) {
    a.r[0] = r0, a.r[1] = r1, a.r[2] = r2, a.r[3] = r3, a.r[13] = STACK, a.r[14] = 0x9000;
    snx_st_le32(mem + STACK, s0), snx_st_le32(mem + STACK + 4, s1), snx_st_le32(mem + STACK + 8, s2);
    a32(symbol)(&a);
    CHECK(a.r[15] == 0x9000 && a.r[13] == STACK);
    return a.r[0];
}

/* the archive's image with one file, /a, of the five bytes "hello" */
static void image_with_file(u8* b) {
    u64 dirs = snx_ld_le64(b + 0x18), files = snx_ld_le64(b + 0x38);
    u8* f = b + files;
    snx_st_le32(b + dirs + 12, 0); /* the root's first file */
    snx_st_le64(b + 0x40, 0x24);   /* the file table: one entry and its name */
    snx_st_le64(b + 0x48, files + 0x24);
    snx_st_le32(f, 0), snx_st_le32(f + 4, ROM_NONE);
    snx_st_le64(f + 8, 0), snx_st_le64(f + 16, 5);
    snx_st_le32(f + 24, ROM_NONE), snx_st_le32(f + 28, 1);
    memcpy(f + 32, "a\0\0\0", 4);
    memcpy(b + files + 0x24, "hello", 5);
}

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

    /* a commit of a native mount goes to the core as fs_save_root opened the save: a journaled save's to fs_save_commit,
       whose Result is the function's (WriteModeFileNotClosed, desc 6457, one the SDK stops on), and one whose files
       are its own to fs_root_commit */
    text(NAME, "save");
    save_given = 1;
    CHECK(nnfs_mount_save_data(c, NAME, CACHE) == 0);
    CHECK(nnfs_commit(c, NAME) == 0 && committed == save_root && committed_noted && committed_journaled);
    committed = NULL;
    CHECK(nnfs_commit_save_data(c, NAME) == 0 && committed == save_root && committed_journaled);
    commit_result = FSR(1);
    CHECK(nnfs_commit(c, NAME) == FSR(1) && abort_needed(FSR(6457)) && !abort_needed(FSR(1)));
    commit_result = 0;
    nnfs_unmount(c, NAME);
    save_given = 0;
    CHECK(nnfs_mount_cache_storage(c, NAME) == 0);
    CHECK(nnfs_commit(c, NAME) == 0 && committed == save_root && !committed_noted && !committed_journaled);
    nnfs_unmount(c, NAME);
    save_given = -1;
    committed = NULL;

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

    /* a system archive by its data id: the cache size is its four tables' (12 + 0x18 + 4 + 0, rounded up to 8), the
       query leaves no image open, and the mount copies the tables into the game's cache */
    outs();
    CHECK(nnfs_query_mount_system_data_cache_size(c, RESULT, SYSTEM_ID) == 0 && snx_ld_le64(mem + RESULT) == 0x28);
    CHECK(storages_open == 0);
    outs();
    CHECK(nnfs_query_mount_rom_cache_size(c, RESULT) == 0 &&
          snx_ld_le64(mem + RESULT) == 0x20); /* the game's: 4 + 0x18 + 4 */
    text(NAME, "ng");
    memset(mem + CACHE, 0xee, 0x40);
    CHECK(nnfs_mount_system_data(c, NAME, SYSTEM_ID, CACHE, 0x28) == 0 && storages_open == 1);
    CHECK(!memcmp(mem + CACHE, data_image_bytes + 0x50, 12 + 0x18 + 4) && mem[CACHE + 0x28] == 0xee);
    text(PATH, "ng:/words");
    CHECK(nnfs_is_path_mounted(c, PATH) == 1 && own_runs == 8);
    nnfs_unmount(c, NAME);
    CHECK(storages_open == 0);
    CHECK(nnfs_is_path_mounted(c, PATH) == 1 && own_runs == 9); /* unmounted: the SDK's again */

    /* the device's save is asked of the file server as the save of type 3 that no user owns, an account's as type 1
       of its user; the server's answer (here: not there) is the function's */
    text(NAME, "dev");
    CHECK(nnfs_mount_device_save_data(c, NAME) == FSR(1) && save_type == 3 && !save_owned);
    memset(mem + 0x300, 0x11, 16);
    CHECK(nnfs_mount_save_data(c, NAME, 0x300) == FSR(1) && save_type == 1 && save_owned);

    /* a file of the archive, in a 64-bit game: the handle is 8 bytes with bit 63, the bytes read a size_t of 8 */
    image_with_file(data_image_bytes);
    text(NAME, "ng");
    text(PATH, "ng:/a");
    CHECK(nnfs_mount_system_data(c, NAME, SYSTEM_ID, CACHE, 0x100) == 0);
    outs();
    CHECK(nnfs_open_file(c, RESULT, PATH, 1) == 0 && snx_ld_le64(mem + RESULT) >> 63 && mem[RESULT + 8] == 0xee);
    {
        u64 h = snx_ld_le64(mem + RESULT);
        outs();
        CHECK(nnfs_read_file(c, RESULT, h, 1, 0x600, 16) == 0 && snx_ld_le64(mem + RESULT) == 4);
        CHECK(!memcmp(mem + 0x600, "ello", 4) && mem[RESULT + 8] == 0xee);
        CHECK(nnfs_read_file(c, RESULT, 0x5000, 0, 0x600, 16) == 0x77 && own(SYM_READ_FILE, 10)); /* the SDK's */
        nnfs_close_file(c, h);
    }
    nnfs_unmount(c, NAME);

    /* the same in a 32-bit game, through its adapters. The cache size is a size_t of 4 bytes, the archive's id two
       registers, and the mount's cache and its size two words of the stack */
    module32.nmods = 0;
    g_arm_mod = &module32;
    a.mem = mem, a.mem_size = sizeof mem;
    CHECK(HOOK32_COUNT == 42);
    /* each for the one 32-bit SDK read, 0.12.16, alone: a 32-bit game of another SDK has no hook here */
    for (int i = 0; i < HOOK32_COUNT; i++)
        CHECK(hook32_key[i] == HOOK_SDK && hook32_lo[i] == 0 && hook32_hi[i] == 0x10000);
    outs();
    CHECK(call32("_ZN2nn2fs29QueryMountSystemDataCacheSizeEPjNS_3ncm12SystemDataIdE", RESULT, 0xdead, (u32)SYSTEM_ID,
                 (u32)(SYSTEM_ID >> 32), 0, 0, 0) == 0);
    CHECK(snx_ld_le32(mem + RESULT) == 0x50 && mem[RESULT + 4] == 0xee); /* 12 + 0x18 + 4 + 0x24, rounded up to 8 */
    CHECK(call32("_ZN2nn2fs15MountSystemDataEPKcNS_3ncm12SystemDataIdEPvj", NAME, 0xdead, (u32)SYSTEM_ID,
                 (u32)(SYSTEM_ID >> 32), CACHE, 0x50, 0) == 0 &&
          storages_open == 1);
    {
        /* the handle: 4 bytes, bit 0 set and bit 1 clear for a file, the 4 bytes after it left */
        u32 h, d;
        outs();
        CHECK(call32("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", RESULT, PATH, 1, 0, 0, 0, 0) == 0);
        h = snx_ld_le32(mem + RESULT);
        CHECK((h & 3) == 1 && mem[RESULT + 4] == 0xee);
        /* ReadFile: the offset in r2 and r3, the buffer and the size on the stack, the bytes read in 4 bytes */
        outs();
        memset(mem + 0x600, 0xee, 16);
        CHECK(call32("_ZN2nn2fs8ReadFileEPjNS0_10FileHandleExPvj", RESULT, h, 1, 0, 0x600, 16, 0) == 0);
        CHECK(snx_ld_le32(mem + RESULT) == 4 && mem[RESULT + 4] == 0xee && !memcmp(mem + 0x600, "ello\xee", 5));
        /* the form with no count: the handle in r0, r1 skipped for the offset's pair; short of the size asked, as
           the SDK's check has it */
        CHECK(call32("_ZN2nn2fs8ReadFileENS0_10FileHandleExPvj", h, 0xdead, 0, 0, 0x600, 5, 0) == 0);
        CHECK(!memcmp(mem + 0x600, "hello", 5));
        /* the size: an int64_t, 8 bytes in either game */
        outs();
        CHECK(call32("_ZN2nn2fs11GetFileSizeEPxNS0_10FileHandleE", RESULT, h, 0, 0, 0, 0, 0) == 0);
        CHECK(snx_ld_le64(mem + RESULT) == 5 && mem[RESULT + 8] == 0xee);
        /* a handle the SDK made is a pointer, so even: the function's own code answers it, and its r0 comes back */
        CHECK(call32("_ZN2nn2fs8ReadFileEPjNS0_10FileHandleExPvj", RESULT, 0x5000, 0, 0, 0x600, 16, 0) == 0x78);
        CHECK(own_runs32 == 1 && own_runs == 10);
        CHECK(call32("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", h, 0, 0, 0, 0, 0, 0) == 1 && own_runs32 == 1);
        /* a directory's handle has bit 1 too, and is no file's */
        outs();
        text(PATH, "ng:/");
        CHECK(call32("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", RESULT, PATH, 3, 0, 0, 0, 0) == 0);
        d = snx_ld_le32(mem + RESULT);
        CHECK((d & 3) == 3 && mem[RESULT + 4] == 0xee && file_of(d) < 0 && dir_of(d) >= 0 && dir_of(h) < 0);
        outs();
        CHECK(call32("_ZN2nn2fs22GetDirectoryEntryCountEPxNS0_15DirectoryHandleE", RESULT, d, 0, 0, 0, 0, 0) == 0);
        CHECK(snx_ld_le64(mem + RESULT) == 1);
        call32("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", d, 0, 0, 0, 0, 0, 0);
        call32("_ZN2nn2fs9CloseFileENS0_10FileHandleE", h, 0, 0, 0, 0, 0, 0);
        CHECK(file_of(h) < 0 && dir_of(d) < 0); /* closed: the handles name nothing */
        /* the slot opened again has another generation: the old handle still names nothing */
        text(PATH, "ng:/a");
        CHECK(call32("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", RESULT, PATH, 1, 0, 0, 0, 0) == 0);
        CHECK(snx_ld_le32(mem + RESULT) != h && file_of(h) < 0 && file_of(snx_ld_le32(mem + RESULT)) >= 0);
        call32("_ZN2nn2fs9CloseFileENS0_10FileHandleE", snx_ld_le32(mem + RESULT), 0, 0, 0, 0, 0, 0);
    }
    call32("_ZN2nn2fs7UnmountEPKc", NAME, 0, 0, 0, 0, 0, 0);
    CHECK(storages_open == 0 && own_runs32 == 1);
    g_arm_mod = NULL;
    printf("nn_fs: %u checks passed\n", checks);
    return 0;
}
