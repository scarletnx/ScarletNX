/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the file system server's core (fs.h), as nn/fs.c calls it, with a small guest address space. */
#include "nx/hle/filesystem/fs.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/mods.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#define test_chdir _chdir
#else
#include <dirent.h>
#include <unistd.h>
#define test_chdir chdir
#endif

/* Including the production file exposes its static state and helpers without test hooks in the runtime. */
#include "nx/hle/filesystem/fs.c"
#include "common/utilities/mii_model.h"
#include "nx/hle/hle.c"

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
/* The guest memory this test models: the g_mem calloc at address 0, TEST_GUEST_SIZE bytes, mapped and writable for
   its whole length and free below it. mem.c's blocks are what the file system's transfer buffers are checked against
   (fs.c's buffer_valid), so they answer as the flat buffer they stand in for. */
#define TEST_GUEST_SIZE 0x10000
void mem_query(u64 addr, MemBlock* out) {
    memset(out, 0, sizeof *out);
    out->addr = 0;
    out->size = TEST_GUEST_SIZE;
    if (addr >= TEST_GUEST_SIZE) {
        out->state = MS_FREE;
        out->perm = PERM_NONE;
        out->size = AS_SIZE - addr;
        out->addr = addr;
    } else {
        out->state = MS_NORMAL;
        out->perm = PERM_RW;
    }
}
char g_datadir[1024], g_exename[256], g_exedir[1024], g_confdir[1024], g_addondir[1024], g_gamename[256];
u64 g_addon;
int g_program; /* main.c's: the game's own program unless a test says another */
char g_addonrec[256];
const u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull}; /* am.c's: the first user */
static jmp_buf* test_fatal_jump;                                   /* a fatal() the test waits for comes back here */
static char test_fatal_text[1024];
static unsigned test_checks, test_scans;
static int test_scans_allowed = 1;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "filesystem_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

static void test_scan(void) {
    CHECK(test_scans_allowed);
    test_scans++;
}

#if defined(_WIN32)
HANDLE WINAPI fs_test_FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data);
HANDLE WINAPI fs_test_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data);
HANDLE WINAPI fs_test_FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data) {
    test_scan();
    return FindFirstFileA(path, data);
}
HANDLE WINAPI fs_test_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    test_scan();
    return FindFirstFileW(path, data);
}
void pm_init(PMutex* m) { InitializeSRWLock(&m->l); }
void pm_lock(PMutex* m) { AcquireSRWLockExclusive(&m->l); }
void pm_unlock(PMutex* m) { ReleaseSRWLockExclusive(&m->l); }
int plat_mkdir(const char* path) { return CreateDirectoryA(path, NULL) ? 0 : -1; }
#else
DIR* fs_test_opendir(const char* path);
DIR* fs_test_opendir(const char* path) {
    test_scan();
    return opendir(path);
}
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
int plat_mkdir(const char* path) { return mkdir(path, 0777); }
#endif

int plat_is_dir(const char* path) {
    (void)path;
    test_fail("HLE attempted host existence query", __LINE__);
    return 0;
}
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off) {
#if defined(_WIN32)
    if (_fseeki64(f, (i64)off, SEEK_SET)) return -1;
#else
    if (fseeko(f, (off_t)off, SEEK_SET)) return -1;
#endif
    size_t got = fread(buf, 1, (size_t)size, f);
    return ferror(f) ? -1 : (i64)got;
}
int plat_file_map(PFileMap* map, const char* path) {
    (void)map;
    (void)path;
    test_fail("HLE attempted mapping without its indexed size", __LINE__);
    return -1;
}
int plat_file_map_sized(PFileMap* map, const char* path, u64 size) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    u8* data = malloc(size ? (size_t)size : 1);
    CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)size, f) == (size_t)size);
    CHECK(!fclose(f));
    map->data = data;
    map->size = size;
    return 0;
}
void plat_file_unmap(PFileMap* map) {
    if (!map->borrowed) free((void*)map->data);
    map->data = NULL;
    map->size = 0;
    map->borrowed = 0;
}
static int test_shared; /* files shared and not yet unshared */
int plat_file_share(const char* path, u64 size, int* fd, const u8** data) {
    *fd = -1;
    *data = NULL;
    /* zulu.bin stays unmapped so RomFS reads also cover the on-demand fallback */
    size_t length = strlen(path);
    if (length >= 8 && !strcmp(path + length - 8, "zulu.bin")) return -1;
    u8* bytes = NULL;
    if (size) {
        FILE* f = fopen(path, "rb");
        CHECK(f != NULL);
        bytes = malloc((size_t)size);
        CHECK(bytes != NULL);
        CHECK(fread(bytes, 1, (size_t)size, f) == (size_t)size);
        CHECK(!fclose(f));
    }
    *fd = ++test_shared;
    *data = bytes;
    return 0;
}
void plat_file_unshare(int fd, const u8* data, u64 size) {
    (void)size;
    CHECK(fd > 0 && test_shared > 0);
    test_shared--;
    free((void*)data);
}
static int test_left_said; /* the warnings that a folder was left by an older version, the last in test_left_text */
static char test_left_text[1024];
/* memory/own.c: nothing is GPU-owned here */
void own_acquire(u64 addr, u64 size) { (void)addr, (void)size; }
/* platform/plat_posix.c: no page here is protected for the write tracking */
void plat_ww_open(u8* p, u64 size) { (void)p, (void)size; }
void plat_ww_close(u8* p, u64 size) { (void)p, (void)size; }
static char test_fs_text[1024];    /* the last warning of fs.c's own, "fs: ..." */
static char test_report_text[1024]; /* the last warning of a report's, indented: "  ..." */
static char test_romfs_text[1024]; /* the last warning about the RomFS, "RomFS: ..." */
static char test_mod_link_text[2048]; /* every warning about a mod that is a symbolic link */
#if !defined(_WIN32)
static unsigned test_mod_links; /* their count, which only the systems with symbolic links check */
#endif
void logf_(int level, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (!level && strstr(text, "was left by an older version")) {
        test_left_said++;
        snprintf(test_left_text, sizeof test_left_text, "%s", text);
    }
    if (!level && strstr(text, "which mods aren't read through")) { /* kept apart from the last warning */
        size_t said = strlen(test_mod_link_text);
        snprintf(test_mod_link_text + said, sizeof test_mod_link_text - said, "%s", text);
#if !defined(_WIN32)
        test_mod_links++;
#endif
    } else if (!level && !strncmp(text, "fs: ", 4))
        snprintf(test_fs_text, sizeof test_fs_text, "%s", text);
    if (!level && !strncmp(text, "  ", 2)) snprintf(test_report_text, sizeof test_report_text, "%s", text);
    if (!level && !strncmp(text, "RomFS: ", 7)) snprintf(test_romfs_text, sizeof test_romfs_text, "%s", text);
}
/* options.c's: the game's settings, where the runtime read them */
const char* ini_file(void) { return "config/Game.ini"; }
/* am.c's: the display version of the program's control data, which this test sets */
static char test_display[16] = "1.0.0";
void am_display_version(char version[16]) {
    memcpy(version, test_display, sizeof test_display);
    version[15] = 0;
}
/* am.c's: an account's and the device's save sizes, which this test sets */
void am_save_data_size(u8 type, i64* normal_size, i64* journal_size) {
    *normal_size = type == 1 ? 0x1230000 : type == 3 ? 0x450000 : 0;
    *journal_size = type == 1 ? 0x670000 : type == 3 ? 0x89000 : 0;
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (test_fatal_jump) {
        vsnprintf(test_fatal_text, sizeof test_fatal_text, format, args);
        va_end(args);
        longjmp(*test_fatal_jump, 1);
    }
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}
void frontend_frame(void) {}

/* The test's handles: slots of g_hle.fs's pools, as nn/fs.c keeps its mounts, files and directories (obj set
   while one is open), so that the save states find them (fs_state_save) */
typedef struct {
    u32 slot;
} TFs;
typedef struct {
    u32 slot;
} TFile;
typedef struct {
    u32 slot;
} TDir;
/* a storage: the game's RomFS (romfs_read_guest) or a data storage (fs_storage_open), as nn/fs.c's MountRom reads them */
typedef struct {
    int game;
    FsStorage st;
} TStorage;
/* every slot of the pools free again: nothing the test opened is left open */
static int handles_closed(void) {
    for (u32 i = 0; i < FS_FS_MAX; i++)
        if (g_hle.fs.fss[i].obj) return 0;
    for (u32 i = 0; i < FS_FILE_MAX; i++)
        if (g_hle.fs.files[i].obj) return 0;
    for (u32 i = 0; i < FS_DIR_MAX; i++)
        if (g_hle.fs.dirs[i].obj) return 0;
    return 1;
}

/* ---- file systems: a root folder, retained, in a slot of g_hle.fs.fss */
static TFs mount_root(FsNode* root, int noted) {
    u32 i = 0;
    fs_lock();
    while (i < FS_FS_MAX && g_hle.fs.fss[i].obj) i++;
    CHECK(i < FS_FS_MAX);
    g_hle.fs.fss[i] = (FsData){.obj = 1, .root = root, .noted = (u32)noted};
    fs_unlock();
    return (TFs){i};
}
static void close_fs(TFs fs) {
    fs_lock();
    CHECK(g_hle.fs.fss[fs.slot].obj);
    fs_root_release(g_hle.fs.fss[fs.slot].root);
    g_hle.fs.fss[fs.slot] = (FsData){0};
    fs_unlock();
}
static FsNode* root_of(TFs fs) { return g_hle.fs.fss[fs.slot].root; }
/* the folder at path in the tree as a file system, made when it is not there: a save's folder, without the choice of
   save_folder */
static TFs mount_folder(const char* path) {
    FsNode* node = NULL;
    fs_lock();
    FsTreeResult r = fs_tree_mkdirs(&g_hle.fs.tree, path, &node);
    if (r == FS_TREE_OK) fs_tree_retain(node);
    fs_unlock();
    CHECK(r == FS_TREE_OK && node != NULL);
    return mount_root(node, 0);
}
static TFs mount_sd(void) {
    FsNode* root = NULL;
    CHECK(fs_sd_root(&root) == 0 && root != NULL);
    return mount_root(root, 0);
}
/* the save of type for the user uid (u64 halves, NULL for one with every byte 4) and index, as fs_save_root opens it */
static u32 mount_save_rc(u8 type, const u64* uid, u16 index, TFs* out) {
    u8 user[16];
    FsNode* root = NULL;
    int noted = 0;
    memset(user, 4, sizeof user);
    if (uid) {
        snx_st_le64(user, uid[0]);
        snx_st_le64(user + 8, uid[1]);
    }
    u32 rc = fs_save_root(type, user, index, &root, &noted);
    CHECK(rc ? root == NULL : root != NULL);
    CHECK(noted == (type >= 1 && type <= 3));
    if (!rc) *out = mount_root(root, noted);
    return rc;
}
static TFs mount_save(u8 type) {
    TFs fs = {0};
    CHECK(mount_save_rc(type, NULL, 0, &fs) == 0);
    return fs;
}
/* an Account save of the user uid */
static TFs mount_user_save(const u64* uid) {
    TFs fs = {0};
    CHECK(mount_save_rc(1, uid, 0, &fs) == 0);
    return fs;
}

/* ---- the operations on a root, under the tree's lock as nn/fs.c takes it */
enum { CREATE_FILE, DELETE_FILE, CREATE_DIR, DELETE_DIR, DELETE_TREE, CLEAN_TREE, ENTRY_TYPE };
static const char* const root_op_names[] = {"CreateFile",          "DeleteFile",        "CreateDirectory",
                                            "DeleteDirectory",     "DeleteTree",        "CleanDirectory",
                                            "GetEntryType"};
static void root_op(TFs fs, int op, const char* path, i64 size, u32 expected) {
    u32 rc, type = 2;
    fs_lock();
    FsNode* root = root_of(fs);
    switch (op) {
    case CREATE_FILE: rc = fs_root_create(root, path, size, 0); break;
    case DELETE_FILE: rc = fs_root_remove(root, path, 0, 0, 0); break;
    case CREATE_DIR: rc = fs_root_create(root, path, 0, 1); break;
    case DELETE_DIR: rc = fs_root_remove(root, path, 1, 0, 0); break;
    case DELETE_TREE: rc = fs_root_remove(root, path, 1, 1, 0); break;
    case CLEAN_TREE: rc = fs_root_remove(root, path, 1, 1, 1); break;
    default: rc = fs_root_entry_type(root, path, &type); break;
    }
    fs_unlock();
    if (rc != expected) fprintf(stderr, "%s %s gave %x, expected %x\n", root_op_names[op], path, rc, expected);
    CHECK(rc == expected);
    if (op == ENTRY_TYPE) CHECK(rc ? type == 2 : type <= 1);
}
/* 0 a directory, 1 a file */
static u32 entry_type(TFs fs, const char* path) {
    u32 type = 2;
    fs_lock();
    u32 rc = fs_root_entry_type(root_of(fs), path, &type);
    fs_unlock();
    CHECK(rc == 0);
    return type;
}
static void root_rename(TFs fs, int dir, const char* source, const char* dest, u32 expected) {
    fs_lock();
    u32 rc = fs_root_rename(root_of(fs), source, dest, dir);
    fs_unlock();
    if (rc != expected) fprintf(stderr, "rename %s %s gave %x, expected %x\n", source, dest, rc, expected);
    CHECK(rc == expected);
}
static void commit(TFs fs) {
    fs_lock();
    fs_root_commit(root_of(fs), (int)g_hle.fs.fss[fs.slot].noted);
    fs_unlock();
}

/* ---- files, in slots of g_hle.fs.files. A failed open leaves its FileData as it was: nothing to close */
static u32 open_file_rc(TFs fs, const char* path, u32 mode, TFile* out) {
    FileData opened = {0};
    u32 i = 0;
    fs_lock();
    while (i < FS_FILE_MAX && g_hle.fs.files[i].obj) i++;
    CHECK(i < FS_FILE_MAX);
    u32 rc = fs_file_open(root_of(fs), path, mode, &opened);
    if (!rc) {
        g_hle.fs.files[i] = opened;
        g_hle.fs.files[i].obj = 1;
    }
    fs_unlock();
    CHECK(rc ? opened.node == NULL && opened.f == NULL && opened.mode == 0 : opened.node != NULL);
    out->slot = i;
    return rc;
}
static TFile open_file(TFs fs, const char* path, u32 mode) {
    TFile f;
    u32 rc = open_file_rc(fs, path, mode, &f);
    if (rc) fprintf(stderr, "open %s (mode %u) gave %x\n", path, mode, rc);
    CHECK(rc == 0);
    return f;
}
static void close_file(TFile f) {
    fs_lock();
    CHECK(g_hle.fs.files[f.slot].obj);
    fs_file_close(&g_hle.fs.files[f.slot]);
    g_hle.fs.files[f.slot] = (FileData){0};
    fs_unlock();
}
static FileData* file_of(TFile f) { return &g_hle.fs.files[f.slot]; }
static u64 get_size(TFile f) {
    fs_lock();
    u64 size = file_of(f)->node->size;
    fs_unlock();
    return size;
}
static void set_size(TFile f, i64 size, u32 expected) {
    fs_lock();
    u32 rc = fs_file_set_size(file_of(f), size);
    fs_unlock();
    CHECK(rc == expected);
}
/* size bytes of data, put in guest memory at 0x3000, written at offset with the Flush option */
static void write_data(TFile f, u64 offset, const char* data, size_t size, u32 expected) {
    CHECK(size < 0x1000);
    memcpy(GP(0x3000), data, size);
    fs_lock();
    u32 rc = fs_file_write(file_of(f), 1, offset, 0x3000, size);
    fs_unlock();
    if (rc != expected) fprintf(stderr, "write at %llx gave %x, expected %x\n", (unsigned long long)offset, rc, expected);
    CHECK(rc == expected);
}
static u32 read_raw(TFile f, u64 offset, u64 addr, u64 size, u64* n) {
    *n = UINT64_MAX;
    fs_lock();
    u32 rc = fs_file_read(file_of(f), offset, addr, size, n);
    fs_unlock();
    CHECK(rc ? *n == 0 : *n <= size);
    return rc;
}
/* up to size bytes at offset into guest memory at 0x5000, its bytes 0xcc before: the bytes read */
static u64 read_data(TFile f, u64 offset, size_t size, u32 expected) {
    u64 n;
    CHECK(size <= 0x8000);
    memset(GP(0x5000), 0xcc, size);
    u32 rc = read_raw(f, offset, 0x5000, size, &n);
    if (rc != expected) fprintf(stderr, "read at %llx gave %x, expected %x\n", (unsigned long long)offset, rc, expected);
    CHECK(rc == expected);
    return n;
}

/* ---- directories, in slots of g_hle.fs.dirs */
static u32 open_dir_rc(TFs fs, const char* path, u32 mode, TDir* out) {
    DirData opened = {0};
    u32 i = 0;
    fs_lock();
    while (i < FS_DIR_MAX && g_hle.fs.dirs[i].obj) i++;
    CHECK(i < FS_DIR_MAX);
    u32 rc = fs_dir_open(root_of(fs), path, mode, &opened);
    if (!rc) {
        g_hle.fs.dirs[i] = opened;
        g_hle.fs.dirs[i].obj = 1;
    }
    fs_unlock();
    CHECK(rc ? opened.node == NULL : opened.node != NULL);
    out->slot = i;
    return rc;
}
static TDir open_dir(TFs fs, const char* path, u32 mode) {
    TDir d;
    CHECK(open_dir_rc(fs, path, mode, &d) == 0);
    return d;
}
static void close_dir(TDir d) {
    fs_lock();
    CHECK(g_hle.fs.dirs[d.slot].obj);
    fs_dir_close(&g_hle.fs.dirs[d.slot]);
    g_hle.fs.dirs[d.slot] = (DirData){0};
    fs_unlock();
}
static u64 dir_count(TDir d) {
    fs_lock();
    u64 n = fs_dir_count(&g_hle.fs.dirs[d.slot]);
    fs_unlock();
    return n;
}
/* up to capacity 0x310-byte entries into guest memory at 0x5000: how many */
static u64 dir_read(TDir d, unsigned capacity) {
    u64 n = UINT64_MAX;
    CHECK((u64)capacity * 0x310 <= 0x8000);
    memset(GP(0x5000), 0xcc, (size_t)capacity * 0x310);
    fs_lock();
    u32 rc = fs_dir_read(&g_hle.fs.dirs[d.slot], 0x5000, capacity, &n);
    fs_unlock();
    CHECK(rc == 0 && n <= capacity);
    return n;
}

/* ---- storages */
static TStorage open_romfs(void) {
    TStorage s = {0};
    s.game = 1;
    s.st.aoc = -1;
    return s;
}
static TStorage open_data(u64 data_id) {
    TStorage s = {0};
    CHECK(fs_storage_open(data_id, &s.st) == 0);
    return s;
}
static void tstorage_close(TStorage* s) {
    if (!s->game) fs_storage_close(&s->st);
}
static u64 tstorage_size(const TStorage* s) { return s->game ? g_hle.fs.rtotal : fs_storage_size(&s->st); }
static int tstorage_read_at(TStorage* s, u64 off, u64 addr, u64 size) {
    return s->game ? romfs_read_guest(off, addr, size) : fs_storage_read_guest(&s->st, off, addr, size);
}
/* size bytes at off into guest memory at 0x5000, it and the 16 bytes past it 0xcc before */
static void tstorage_read(TStorage* s, u64 off, u64 size) {
    CHECK(size <= 0x8000);
    memset(GP(0x5000), 0xcc, (size_t)size + 16);
    CHECK(tstorage_read_at(s, off, 0x5000, size) == 0);
}

static void test_file_services(TFs fs) {
    root_op(fs, CREATE_FILE, "/work.bin", 4, 0);
    root_op(fs, CREATE_FILE, "/work.bin", 4, SNX_RC_FS_PATH_ALREADY_EXISTS);
    root_op(fs, CREATE_FILE, "/missing/file.bin", 4, SNX_RC_FS_PATH_NOT_FOUND);
    TFile writer = open_file(fs, "/work.bin", 7);
    TFile reader = open_file(fs, "/work.bin", 1);
    CHECK(get_size(writer) == 4 && get_size(reader) == 4);
    write_data(writer, 4, "hello", 5, 0);
    CHECK(get_size(reader) == 9);
    CHECK(read_data(reader, 0, 16, 0) == 9);
    const u8 expected[9] = {0, 0, 0, 0, 'h', 'e', 'l', 'l', 'o'};
    CHECK(!memcmp(GP(0x5000), expected, sizeof expected));
    CHECK(GP(0x5000)[9] == 0xcc);
    write_data(reader, 0, "x", 1, SNX_RC_FS_WRITE_UNPERMITTED);
    set_size(reader, 1, SNX_RC_FS_WRITE_UNPERMITTED);
    TFile fixed = open_file(fs, "/work.bin", 3);
    write_data(fixed, 9, "x", 1, SNX_RC_FS_FILE_EXTENSION_WITHOUT_OPEN_MODE_ALLOW_APPEND);
    CHECK(get_size(fixed) == 9);
    set_size(writer, 2, 0);
    CHECK(get_size(reader) == 2 && get_size(fixed) == 2);
    CHECK(read_data(reader, 0, 16, 0) == 2);
    read_data(reader, 3, 1, SNX_RC_FS_OUT_OF_RANGE);
    set_size(writer, 8192, 0);
    CHECK(get_size(reader) == 8192 && get_size(fixed) == 8192);
    CHECK(read_data(reader, 8191, 1, 0) == 1 && GP(0x5000)[0] == 0);
    TDir dir = open_dir(fs, "/", 2);
    u64 entries = dir_read(dir, 8);
    int found = 0;
    for (u64 i = 0; i < entries; i++) {
        u8* entry = GP(0x5000 + i * 0x310);
        if (!strcmp((char*)entry, "work.bin")) {
            u64 size = snx_ld_le64(entry + 0x308);
            CHECK(size == 8192 && entry[0x304] == 1);
            found = 1;
        }
    }
    CHECK(found);
    close_dir(dir);
    close_file(fixed);
    close_file(reader);
    close_file(writer);
    root_rename(fs, 0, "/work.bin", "/renamed.bin", 0);
    root_op(fs, ENTRY_TYPE, "/work.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    CHECK(entry_type(fs, "/renamed.bin") == 1 && entry_type(fs, "/") == 0);
    /* the host file's modification time, which the tree keeps for it: what a file's time stamp is made of */
    {
        FsNode* node;
        u64 now = (u64)time(NULL), ms = 0;
        fs_lock();
        CHECK(!fs_tree_lookup(&g_hle.fs.tree, root_of(fs), "/renamed.bin", &node));
        CHECK(!fs_tree_mtime(&g_hle.fs.tree, node, &ms));
        fs_unlock();
        CHECK(ms / 1000 + 60 >= now && ms / 1000 <= now + 60);
    }
    reader = open_file(fs, "/renamed.bin", 1);
    CHECK(get_size(reader) == 8192);
    close_file(reader);
    root_op(fs, DELETE_FILE, "/renamed.bin", 0, 0);
    root_op(fs, ENTRY_TYPE, "/renamed.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    puts("PASS HLE create/open/write/read/truncate/rename/delete and shared cached size");
}

/* The checks of the file server's core, which nn/fs.c calls with the game's arguments as they are: a range past
   INT64_MAX fails before the open mode is checked, an empty transfer skips the end-of-file and mode checks, a buffer
   the guest has not mapped writable fails, and a negative size fails before the mode and the path */
static void test_file_contracts(TFs fs) {
    root_op(fs, CREATE_FILE, "/contract.bin", 4, 0);
    TFile rw = open_file(fs, "/contract.bin", 3);
    TFile ro = open_file(fs, "/contract.bin", 1);
    TFile wo = open_file(fs, "/contract.bin", 2);
    static const struct {
        u64 off, size;
        u32 result;
    } cases[] = {
        {INT64_MAX, 1, RC(2, 3005)}, /* overflow wins over permission */
        {INT64_MAX - 1, 2, RC(2, 3005)},
        {1, UINT64_MAX, RC(2, 3005)}, /* nn/fs.c refuses a negative offset itself */
        {INT64_MAX, 0, 0}, /* empty transfers skip EOF and mode checks */
        {UINT64_MAX, 0, 0},
        {0, 0, 0},
    };
    for (int write = 0; write < 2; ++write) {
        for (unsigned permitted = 0; permitted < 2; ++permitted) {
            TFile target = permitted ? rw : write ? ro : wo;
            for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
                memset(GP(0x5000), 0xcc, 16);
                if (write) {
                    fs_lock();
                    u32 rc = fs_file_write(file_of(target), 1, cases[i].off, 0x5000, cases[i].size);
                    fs_unlock();
                    CHECK(rc == cases[i].result);
                } else {
                    u64 n;
                    CHECK(read_raw(target, cases[i].off, 0x5000, cases[i].size, &n) == cases[i].result && n == 0);
                }
                for (unsigned j = 0; j < 16; ++j) CHECK(GP(0x5000)[j] == 0xcc);
                CHECK(get_size(rw) == 4);
            }
        }
    }
    /* a buffer past the guest's memory: refused, for a read and a write */
    {
        u64 n;
        CHECK(read_raw(rw, 0, TEST_GUEST_SIZE, 4, &n) == SNX_RC_FS_OUT_OF_RANGE);
        CHECK(read_raw(rw, 0, TEST_GUEST_SIZE - 2, 4, &n) == SNX_RC_FS_OUT_OF_RANGE);
        fs_lock();
        u32 rc = fs_file_write(file_of(rw), 0, 0, TEST_GUEST_SIZE, 4);
        fs_unlock();
        CHECK(rc == SNX_RC_FS_OUT_OF_RANGE && get_size(rw) == 4);
    }
    read_data(wo, 0, 1, RC(2, 6202));
    write_data(ro, 0, "x", 1, RC(2, 6203));
    write_data(rw, 4, "x", 1, RC(2, 6201));
    set_size(ro, -1, RC(2, 6062)); /* the size is checked before the permissions */
    set_size(rw, -1, RC(2, 6062));
    CHECK(get_size(rw) == 4);
    CHECK(read_data(ro, 0, 4, 0) == 4);
    for (unsigned j = 0; j < 4; ++j) CHECK(GP(0x5000)[j] == 0);
    root_op(fs, CREATE_FILE, "/new.bin", -1, RC(2, 6062));
    root_op(fs, ENTRY_TYPE, "/new.bin", 0, RC(2, 1));
    root_op(fs, CREATE_FILE, "/../bad", -1, RC(2, 6062)); /* before path normalization */
    static const u32 modes[] = {0, 4, 8, 0x80000000u};
    static const char* paths[] = {"/contract.bin", "/missing", "/"};
    for (int dir = 0; dir < 2; ++dir)
        for (size_t i = 0; i < sizeof modes / sizeof *modes; ++i)
            for (size_t j = 0; j < sizeof paths / sizeof *paths; ++j) {
                TFile f;
                TDir d;
                CHECK((dir ? open_dir_rc(fs, paths[j], modes[i], &d) : open_file_rc(fs, paths[j], modes[i], &f)) ==
                      RC(2, 6072));
            }
    TDir dir = open_dir(fs, "/", 0x80000003u);
    close_dir(dir);
    close_file(wo);
    close_file(ro);
    close_file(rw);
    root_op(fs, DELETE_FILE, "/contract.bin", 0, 0);
    puts("PASS file core: range overflow, empty transfers, guest buffers, permissions, sizes and open modes");
}

/* Atmosphere FileSystemInterfaceAdapter checks normalized roots/ancestry before backend mutation. */
static void test_directory_path_contracts(TFs fs) {
    root_op(fs, CREATE_DIR, "/", 0, RC(2, 2));
    root_op(fs, CREATE_DIR, "/./", 0, RC(2, 2));
    root_op(fs, CREATE_DIR, "/absent/..", 0, RC(2, 2));
    root_op(fs, DELETE_DIR, "/", 0, RC(2, 6031));
    root_op(fs, DELETE_TREE, "/./", 0, RC(2, 6031));
    /* Neither path needs to exist for the strict-ancestry check to win. Both directions count. */
    root_rename(fs, 1, "/absent", "/absent/child", RC(2, 6032));
    root_rename(fs, 1, "/absent/child", "/absent", RC(2, 6032));
    root_rename(fs, 1, "/", "/absent", RC(2, 6032));
    root_rename(fs, 1, "/absent", "/", RC(2, 6032));
    /* Negative controls retain this backend's PathNotFound. IsSubPath itself excludes equality/siblings. */
    root_rename(fs, 1, "/absent", "/absent", RC(2, 1));
    root_rename(fs, 1, "/absent", "/absent-sibling", RC(2, 1));
    root_op(fs, CREATE_DIR, "/ancestry", 0, 0);
    root_op(fs, CREATE_DIR, "/ancestry/child", 0, 0);
    root_rename(fs, 1, "/ancestry", "/ancestry/child", RC(2, 6032)); /* before AlreadyExists */
    root_rename(fs, 1, "/ancestry/child", "/ancestry", RC(2, 6032));
    root_rename(fs, 1, "/ancestry", "/ancestry-sibling", 0);
    root_op(fs, ENTRY_TYPE, "/ancestry-sibling/child", 0, 0);
    root_op(fs, DELETE_TREE, "/ancestry-sibling", 0, 0);
    /* Eden's temporary recursive CreateDirectory behavior must not replace this contract. */
    root_op(fs, CREATE_DIR, "/absent/child", 0, RC(2, 1));
    root_op(fs, ENTRY_TYPE, "/absent", 0, RC(2, 1));
    puts("PASS Atmosphere normalized directory roots and bidirectional strict ancestry");
}

static void test_directory_services(TFs fs) {
    root_op(fs, CREATE_DIR, "/list", 0, 0);
    root_op(fs, CREATE_FILE, "/list/a", 1, 0);
    root_op(fs, CREATE_FILE, "/list/b", 2, 0);
    root_op(fs, CREATE_FILE, "/list/c", 3, 0);
    TDir dir = open_dir(fs, "/list", 3);
    CHECK(dir_count(dir) == 3);
    CHECK(dir_read(dir, 1) == 1 && !strcmp((char*)GP(0x5000), "a"));
    root_op(fs, DELETE_FILE, "/list/b", 0, 0);
    root_op(fs, CREATE_FILE, "/list/aa", 4, 0);
    root_rename(fs, 1, "/list", "/listed", 0);
    CHECK(dir_count(dir) == 3);
    CHECK(dir_read(dir, 2) == 2);
    CHECK(!strcmp((char*)GP(0x5000), "aa"));
    CHECK(!strcmp((char*)GP(0x5310), "c"));
    CHECK(dir_read(dir, 1) == 0);
    root_op(fs, CREATE_FILE, "/listed/d", 5, 0);
    CHECK(dir_count(dir) == 4);
    CHECK(dir_read(dir, 1) == 0);
    TDir fresh = open_dir(fs, "/listed", 2);
    CHECK(dir_count(fresh) == 4 && dir_read(fresh, 8) == 4);
    close_dir(fresh);
    root_op(fs, DELETE_DIR, "/listed", 0, SNX_RC_FS_DIRECTORY_NOT_EMPTY);
    root_op(fs, CLEAN_TREE, "/listed", 0, 0);
    CHECK(dir_count(dir) == 0);
    root_op(fs, DELETE_DIR, "/listed", 0, 0);
    /* the open directory's folder is out of the tree now: nothing to count or list */
    CHECK(!g_hle.fs.dirs[dir.slot].node->attached && dir_count(dir) == 0 && dir_read(dir, 1) == 0);
    close_dir(dir);
    root_op(fs, ENTRY_TYPE, "/listed", 0, SNX_RC_FS_PATH_NOT_FOUND);
    puts("PASS HLE directory cursor/count after create, delete, rename and clean");
}

static void test_index_and_paths(TFs fs) {
    char path[FS_TREE_PATH_MAX];
    CHECK(snprintf(path, sizeof path, "%ssdmc/external.bin", g_datadir) < (int)sizeof path);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL && fwrite("unindexed", 1, 9, f) == 9 && !fclose(f));
    root_op(fs, ENTRY_TYPE, "/external.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    root_op(fs, DELETE_FILE, "/external.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    TFile file;
    CHECK(open_file_rc(fs, "/external.bin", 1, &file) == SNX_RC_FS_PATH_NOT_FOUND);
    u64 size = 123;
    CHECK(fs_open_read("sdmc/external.bin", &size) == NULL && size == 0);
    CHECK(fs_read_file("sdmc/external.bin", &size) == NULL && size == 0);
    PFileMap map = {0};
    CHECK(fs_map_file("sdmc/external.bin", &map) != 0);
    root_op(fs, ENTRY_TYPE, "/../root/root.txt", 0, SNX_RC_FS_DIRECTORY_UNOBTAINABLE);
    root_op(fs, ENTRY_TYPE, "romfs:/asset.bin", 0, SNX_RC_FS_DIRECTORY_UNOBTAINABLE);
    size = 0;
    u8* bytes = fs_read_file("sdmc:/seed.bin", &size);
    CHECK(bytes != NULL && size == 4 && !memcmp(bytes, "seed", 4));
    free(bytes);
    CHECK(!fs_map_file("sdmc/seed.bin", &map));
    CHECK(map.size == 4 && !memcmp(map.data, "seed", 4));
    plat_file_unmap(&map);
    CHECK(snprintf(path, sizeof path, "%ssdmc/seed.bin", g_datadir) < (int)sizeof path);
    f = fopen(path, "ab");
    CHECK(f != NULL && fwrite("unindexed extension", 1, 19, f) == 19 && !fclose(f));
    CHECK(!fs_map_file("sdmc/seed.bin", &map));
    CHECK(map.size == 4 && !memcmp(map.data, "seed", 4));
    plat_file_unmap(&map);
    puts("PASS HLE index-only visibility, confined paths and loader helpers");
}

/* a thread's storage reads of 64 KiB or less that go on where its last one ended take the console's time, as a
   read-ahead's pages do; its first read, a bigger one, one elsewhere and one of another storage take none */
static void test_read_pacing(void) {
    const u64 page = FS_PACE_MAX_BYTES, page_ns = page * 1000000000ull / FS_PACE_BYTES_PER_S;
    TlsFs t = {0};
    const int a = 0, b = 0; /* two storages, by their addresses */
    CHECK(page == 0x10000 && page_ns > 0);
    CHECK(fs_pace_ns(&t, &a, 0, page) == 0);
    for (u64 i = 1; i < 8; i++) CHECK(fs_pace_ns(&t, &a, i * page, page) == page_ns);
    CHECK(fs_pace_ns(&t, &a, 8 * page, 0x400000) == 0); /* a load */
    CHECK(fs_pace_ns(&t, &a, 8 * page + 0x400000, 0x800) == 0x800 * 1000000000ull / FS_PACE_BYTES_PER_S);
    CHECK(fs_pace_ns(&t, &a, 0, page) == 0);
    CHECK(fs_pace_ns(&t, &b, page, page) == 0);
    CHECK(fs_pace_ns(&t, &a, 2 * page, page) == 0); /* b's read came between */
    CHECK(fs_pace_ns(&t, &a, 3 * page, page + 1) == 0);
    puts("PASS a thread's 64 KiB read-ahead on one storage paced, its loads and other reads not");
}

/* a storage's reads: whole, none at its end or past INT64_MAX, never short (a range past its end fails, and writes
   nothing), never into a buffer the guest has not mapped writable; its size the same after each */
static void test_storage_contracts(TStorage* storage, u64 size) {
    const struct {
        u64 off, size;
        int result;
    } cases[] = {
        {UINT64_MAX, UINT64_MAX, -1},
        {0, UINT64_MAX, -1},
        {size + 1, 5, -1},
        {INT64_MAX, 1, -1},
        {size, 1, -1},
        {size - 1, 2, -1}, /* no short reads */
        {size + 1, 1, -1},
        {size, 0, 0},
        {INT64_MAX, 0, 0},
        {UINT64_MAX, 0, 0},
    };
    CHECK(size > 0 && tstorage_size(storage) == size);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        memset(GP(0x5000), 0xcc, 16);
        CHECK(tstorage_read_at(storage, cases[i].off, 0x5000, cases[i].size) == cases[i].result);
        for (unsigned j = 0; j < 16; ++j) CHECK(GP(0x5000)[j] == 0xcc);
        CHECK(tstorage_size(storage) == size);
    }
    CHECK(tstorage_read_at(storage, 0, TEST_GUEST_SIZE, 1) == -1);
    CHECK(tstorage_read_at(storage, 0, TEST_GUEST_SIZE - 1, 2) == -1);
    tstorage_read(storage, size - 1, 1);
    CHECK(GP(0x5000)[1] == 0xcc);
}
/* a read the storage refuses says why, with the values: one past its end, one into guest memory that ends in free
   pages, one into memory past the address space (the result, 2002-3005, names none of them) */
static void test_storage_refusals(TStorage* storage, u64 size) {
    const struct {
        u64 off, size, buffer;
    } cases[] = {{size - 2, 3, 0x5000}, {0, 0x10, TEST_GUEST_SIZE - 8}, {0x10, 0x10, AS_SIZE - 8}};
    char want[3][256];
    CHECK(size >= 0x20);
    snprintf(want[0], sizeof want[0],
             "fs: a storage read of 3 bytes at %llx passes the storage's end, %llx: it answers 2002-3005\n",
             (unsigned long long)(size - 2), (unsigned long long)size);
    snprintf(want[1], sizeof want[1],
             "fs: a storage read of 10 bytes at 0 goes into guest memory at fff8, whose address 10000 can't take it "
             "(state 0, permission 0): it answers 2002-3005\n");
    snprintf(want[2], sizeof want[2],
             "fs: a storage read of 10 bytes at 10 goes into guest memory at %llx, past the address space's end, %llx: "
             "it answers 2002-3005\n",
             (unsigned long long)(AS_SIZE - 8), (unsigned long long)AS_SIZE);
    for (int i = 0; i < 3; i++) {
        test_fs_text[0] = 0;
        CHECK(tstorage_read_at(storage, cases[i].off, cases[i].buffer, cases[i].size) == -1);
        if (strcmp(test_fs_text, want[i])) fprintf(stderr, "said:     %sexpected: %s", test_fs_text, want[i]);
        CHECK(!strcmp(test_fs_text, want[i]));
    }
    test_fs_text[0] = 0; /* a read of no bytes is refused nowhere, and says nothing */
    CHECK(tstorage_read_at(storage, size, AS_SIZE - 8, 0) == 0 && !test_fs_text[0]);
}

/* finds a RomFS entry the way the SDK does: hash its parent's table offset and its name (bytes unsigned), then walk
   that bucket's chain */
static u32 romfs_find(const u8* image, const u64* header, int dir, u32 parent, const char* name) {
    const unsigned char* p = (const unsigned char*)name;
    u64 table = dir ? header[3] : header[7], buckets = (dir ? header[2] : header[6]) / 4;
    u32 fixed = dir ? 0x18 : 0x20, hash = parent ^ 123456789u, entry;
    while (*p) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= *p++;
    }
    entry = snx_ld_le32(image + (dir ? header[1] : header[5]) + (hash % buckets) * 4);
    while (entry != UINT32_MAX) {
        const u8* e = image + table + entry;
        u32 entry_parent, length;
        entry_parent = snx_ld_le32(e);
        length = snx_ld_le32(e + fixed - 4);
        if (entry_parent == parent && length == strlen(name) && !memcmp(e + fixed, name, length)) return entry;
        entry = snx_ld_le32(e + fixed - 8);
    }
    return UINT32_MAX;
}
/* NgWord2's 52 file names, as the 11.0.1 archive names them */
static const char* const ngword2_names[52] = {
    "ac_0_b1_nx", "ac_0_b2_nx", "ac_0_not_b_nx", "ac_1_b1_nx", "ac_1_b2_nx", "ac_1_not_b_nx",
    "ac_2_b1_nx", "ac_2_b2_nx", "ac_2_not_b_nx", "ac_3_b1_nx", "ac_3_b2_nx", "ac_3_not_b_nx",
    "ac_4_b1_nx", "ac_4_b2_nx", "ac_4_not_b_nx", "ac_5_b1_nx", "ac_5_b2_nx", "ac_5_not_b_nx",
    "ac_6_b1_nx", "ac_6_b2_nx", "ac_6_not_b_nx", "ac_7_b1_nx", "ac_7_b2_nx", "ac_7_not_b_nx",
    "ac_8_b1_nx", "ac_8_b2_nx", "ac_8_not_b_nx", "ac_9_b1_nx", "ac_9_b2_nx", "ac_9_not_b_nx",
    "ac_10_b1_nx", "ac_10_b2_nx", "ac_10_not_b_nx", "ac_11_b1_nx", "ac_11_b2_nx", "ac_11_not_b_nx",
    "ac_12_b1_nx", "ac_12_b2_nx", "ac_12_not_b_nx", "ac_13_b1_nx", "ac_13_b2_nx", "ac_13_not_b_nx",
    "ac_14_b1_nx", "ac_14_b2_nx", "ac_14_not_b_nx", "ac_15_b1_nx", "ac_15_b2_nx", "ac_15_not_b_nx",
    "ac_common_b1_nx", "ac_common_b2_nx", "ac_common_not_b_nx", "version.dat",
};
static void test_virtual_services(TFs fs) {
    u8 bytes[] = {1, 0, 3, 0xff, 5};
    CHECK(!fs_add_virtual_file("sdmc/virtual.bin", bytes, sizeof bytes));
    TFile file = open_file(fs, "/virtual.bin", 1);
    CHECK(get_size(file) == sizeof bytes);
    CHECK(read_data(file, 1, 10, 0) == 4 && !memcmp(GP(0x5000), bytes + 1, 4));
    write_data(file, 0, "x", 1, SNX_RC_FS_WRITE_UNPERMITTED);
    set_size(file, 0, SNX_RC_FS_WRITE_UNPERMITTED);
    TFile writable;
    CHECK(open_file_rc(fs, "/virtual.bin", 3, &writable) == SNX_RC_FS_WRITE_UNPERMITTED);
    root_op(fs, DELETE_FILE, "/virtual.bin", 0, SNX_RC_FS_WRITE_UNPERMITTED);
    close_file(file);

    TStorage archive = open_data(0x0100000000000819ull);
    CHECK(tstorage_size(&archive) == 0x200);
    test_storage_contracts(&archive, 0x200);
    test_storage_refusals(&archive, 0x200);
    tstorage_read(&archive, 0, 0x200);
    u64 header_size = snx_ld_le64(GP(0x5000));
    CHECK(header_size == 0x50);
    {
        /* a valid empty image: the root alone, in its bucket, with no folders or files under it */
        u64 header[10];
        u32 links[3];
        memcpy(header, GP(0x5000), sizeof header);
        snx_le64_array(header, 10);
        CHECK(header[9] == 0x200 && header[4] == 0x18 && header[8] == 0 && header[7] <= 0x200);
        CHECK(romfs_find(GP(0x5000), header, 1, 0, "") == 0);
        memcpy(links, GP(0x5000) + header[3] + 4, sizeof links);
        snx_le32_array(links, 3);
        CHECK(links[0] == UINT32_MAX && links[1] == UINT32_MAX && links[2] == UINT32_MAX);
    }
    tstorage_close(&archive);

    {
        TStorage ngword = open_data(0x0100000000000823ull);
        u64 ngword_size = tstorage_size(&ngword);
        CHECK(ngword_size > 0x200 && ngword_size <= 0x2000);
        test_storage_contracts(&ngword, ngword_size);
        tstorage_read(&ngword, 0, ngword_size);
        u8* image = GP(0x5000);
        u64 header[10];
        memcpy(header, image, sizeof header);
        snx_le64_array(header, 10);
        CHECK(header[0] == 0x50 && header[9] < ngword_size && header[7] < ngword_size);
        {
            u32 count = 0;
            u32 entry = snx_ld_le32(image + header[3] + 12); /* the root's first file, then the sibling chain */
            while (entry != UINT32_MAX) {
                CHECK(++count <= sizeof ngword2_names / sizeof ngword2_names[0]);
                entry = snx_ld_le32(image + header[7] + entry + 4);
            }
            CHECK(count == sizeof ngword2_names / sizeof ngword2_names[0]);
        }
        {
            u32 entry = romfs_find(image, header, 0, 0, "version.dat");
            CHECK(entry != UINT32_MAX);
            u64 offset = snx_ld_le64(image + header[7] + entry + 8);
            u64 size = snx_ld_le64(image + header[7] + entry + 16);
            CHECK(size == 4 && offset + 4 <= ngword_size - header[9]);
            CHECK(!memcmp(image + header[9] + offset, "\0\0\0\x1a", 4));
        }
        {
            u32 entry = romfs_find(image, header, 0, 0, "ac_common_b1_nx");
            CHECK(entry != UINT32_MAX);
            u64 offset = snx_ld_le64(image + header[7] + entry + 8);
            u64 size = snx_ld_le64(image + header[7] + entry + 16);
            CHECK(size == 44 && offset + 44 <= ngword_size - header[9]);
            CHECK(image[header[9] + offset] == 0x1f && image[header[9] + offset + 1] == 0x8b);
        }
        for (unsigned i = 0; i < sizeof ngword2_names / sizeof ngword2_names[0]; i++) {
            const u8* body = strcmp(ngword2_names[i], "version.dat") ? ngword2_ac : ngword2_version;
            u32 entry = romfs_find(image, header, 0, 0, ngword2_names[i]);
            CHECK(entry != UINT32_MAX);
            u64 offset = snx_ld_le64(image + header[7] + entry + 8);
            u64 size = snx_ld_le64(image + header[7] + entry + 16);
            CHECK(size == (strcmp(ngword2_names[i], "version.dat") ? 44 : 4));
            CHECK(!memcmp(image + header[9] + offset, body, (size_t)size));
        }
        tstorage_close(&ngword);
    }

    /* MiiModel (0100000000000802), with none in system/archives: the archive mii_model.c generates (its parts are
       checked in mii_model), made at the first open and not before; its six files, a Linear and an SRGB copy sharing
       their bytes */
    {
        static u8 file[0x280000], want[0x280000];
        u64 at[6];
        CHECK(g_hle.fs.miimodelrom_size == 0);
        TStorage model = open_data(0x0100000000000802ull);
        u64 model_size = tstorage_size(&model);
        CHECK(model_size == g_hle.fs.miimodelrom_size && model_size <= sizeof g_hle.fs.miimodelrom);
        test_storage_contracts(&model, model_size);
        /* the RomFS header and tables, then each file read whole in pieces the test's guest memory holds */
        u8 tables[0x400];
        tstorage_read(&model, 0, sizeof tables);
        memcpy(tables, GP(0x5000), sizeof tables);
        u64 header[10];
        memcpy(header, tables, sizeof header);
        snx_le64_array(header, 10);
        CHECK(header[0] == 0x50 && header[9] <= sizeof tables && header[9] < model_size);
        for (unsigned i = 0; i < SNX_MII_MODEL_FILES; i++) {
            char name[32];
            snx_mii_model_name(i, name);
            u32 entry = romfs_find(tables, header, 0, 0, name);
            CHECK(entry != UINT32_MAX);
            u64 offset = snx_ld_le64(tables + header[7] + entry + 8);
            u64 size = snx_ld_le64(tables + header[7] + entry + 16);
            at[i] = offset;
            memset(want, 0, sizeof want);
            CHECK(snx_mii_model_write(i, NULL) <= sizeof want && snx_mii_model_write(i, want) == size &&
                  size <= sizeof file && header[9] + offset + size <= model_size);
            CHECK(snx_mii_model_same(i) == i || at[snx_mii_model_same(i)] == offset);
            for (u64 done = 0; done < size && size <= sizeof file; done += 0x8000) {
                u64 len = size - done < 0x8000 ? size - done : 0x8000;
                tstorage_read(&model, header[9] + offset + done, len);
                memcpy(file + done, GP(0x5000), (size_t)len);
            }
            CHECK(!memcmp(file, i < 4 ? "NFTR\1\0\0\0" : "NFSR\1\0\0\0", 8) && snx_ld_le32(file + 8) == size &&
                  size <= sizeof want && !memcmp(file, want, (size_t)size));
        }
        tstorage_close(&model);
    }
    puts("PASS HLE readonly virtual files and synthetic system archive");
}

#if !defined(_WIN32)
/* the folders past the root, found through the hash tables and linked as siblings, and their non-ASCII files */
static void test_romfs_names(const u8* image, const u64* header) {
    u32 root = romfs_find(image, header, 1, 0, ""), dir, mu, file, link;
    u64 offset, size;
    CHECK(root == 0);
    dir = romfs_find(image, header, 1, 0, "dir");
    mu = romfs_find(image, header, 1, 0, "mu");
    CHECK(dir != UINT32_MAX && mu != UINT32_MAX);
    link = snx_ld_le32(image + header[3] + 8); /* the root's first folder, then its sibling */
    CHECK(link == dir);
    link = snx_ld_le32(image + header[3] + dir + 4);
    CHECK(link == mu);
    link = snx_ld_le32(image + header[3] + mu + 4);
    CHECK(link == UINT32_MAX);
    file = romfs_find(image, header, 0, dir, "\xc3\xa9t\xc3\xa9.bin");
    CHECK(file != UINT32_MAX);
    link = snx_ld_le32(image + header[3] + dir + 12);
    CHECK(link == file);
    offset = snx_ld_le64(image + header[7] + file + 8);
    size = snx_ld_le64(image + header[7] + file + 16);
    CHECK(offset == 32 && size == 6 && !memcmp(image + header[9] + offset, "summer", 6));
    file = romfs_find(image, header, 0, mu, "\xce\xbc.bin");
    CHECK(file != UINT32_MAX);
    offset = snx_ld_le64(image + header[7] + file + 8);
    CHECK(offset == 48 && !memcmp(image + header[9] + offset, "mu", 2));
    CHECK(romfs_find(image, header, 0, dir, "\xce\xbc.bin") == UINT32_MAX);
}
#endif

/* romfs_files' entries, as the shader pass lists them: their names, places in the image and sizes, until stop_at */
static struct {
    int n, stop_at;
    char name[4][16];
    u64 off[4], size[4];
} listed;
static int list_file(void* ctx, const char* name, u64 off, u64 size) {
    (void)ctx;
    if (listed.n < 4) {
        snprintf(listed.name[listed.n], sizeof listed.name[0], "%s", name);
        listed.off[listed.n] = off;
        listed.size[listed.n] = size;
    }
    return ++listed.n == listed.stop_at;
}
/* the RomFS as the runtime itself reads it, outside the service: each file's entry, and its bytes by their place */
static void test_romfs_listing(const u8* image, const u64* header) {
    u8 got[16];
    int asset = -1;
    memset(&listed, 0, sizeof listed);
    /* asset.bin, empty.bin and zulu.bin, and off Windows the two in folders with names past ASCII */
    CHECK(romfs_files(list_file, NULL) == 0 && listed.n >= 3 && listed.n <= 5);
    for (int i = 0; i < listed.n && i < 4; i++)
        if (!strcmp(listed.name[i], "asset.bin")) asset = i;
    CHECK(asset >= 0 && listed.off[asset] == header[9] && listed.size[asset] == 10);
    CHECK(!romfs_peek(listed.off[asset], got, 10) && !memcmp(got, "romfs test", 10) &&
          !memcmp(got, image + header[9], 10));
    CHECK(!romfs_peek(g_hle.fs.rtotal, got, 0));          /* nothing, at the end */
    CHECK(romfs_peek(g_hle.fs.rtotal - 4, got, 8) == -1); /* past the end */
    /* the pass's reads are quiet: none is logged as the game's first read of a file */
    for (int i = 0; i < g_hle.fs.nrfiles; i++) g_hle.fs.rfiles[i].logged = g_hle.fs.rfiles[i].warned = 0;
    CHECK(!romfs_peek(header[9], got, 10));
    for (int i = 0; i < g_hle.fs.nrfiles; i++) CHECK(!g_hle.fs.rfiles[i].logged && !g_hle.fs.rfiles[i].warned);
    /* a file table whose first name runs past it: malformed, so nothing after it can be listed, on any call */
    {
        u8* len = g_hle.fs.rmeta + header[7] + 0x1C;
        u32 was = snx_ld_le32(len);
        int all;
        snx_st_le32(len, 0xFFFFu);
        memset(&listed, 0, sizeof listed);
        CHECK(romfs_files(list_file, NULL) == -2 && listed.n == 0);
        snx_st_le32(len, was);
        /* an entry whose data lies past the image: left out, the others listed */
        memset(&listed, 0, sizeof listed);
        CHECK(romfs_files(list_file, NULL) == 0);
        all = listed.n;
        {
            u8* off = g_hle.fs.rmeta + header[7] + 8;
            u64 at = snx_ld_le64(off);
            snx_st_le64(off, g_hle.fs.rtotal);
            memset(&listed, 0, sizeof listed);
            CHECK(romfs_files(list_file, NULL) == -2 && listed.n == all - 1);
            snx_st_le64(off, at);
        }
    }
    memset(&listed, 0, sizeof listed);
    listed.stop_at = 2;
    CHECK(romfs_files(list_file, NULL) == 1 && listed.n == 2); /* stopped by the second */
}
static void test_extracted_romfs(void) {
    TStorage storage = open_romfs();
    u64 image_size = tstorage_size(&storage);
    CHECK(image_size < 0x8000 && image_size > 0x200);
    test_storage_contracts(&storage, image_size);
    test_storage_refusals(&storage, image_size);
    tstorage_read(&storage, 0, image_size);
    u8* image = GP(0x5000);
    u64 header[10];
    memcpy(header, image, sizeof header);
    snx_le64_array(header, 10);
    CHECK(header[0] == 0x50 && header[3] < image_size && header[7] < image_size && header[9] < image_size);
    u32 entry;
    entry = snx_ld_le32(image + header[3] + 12);
    u64 data_offsets[3] = {0}, sizes[3] = {0};
    unsigned found = 0;
    while (entry != UINT32_MAX) {
        CHECK((u64)entry + 0x20 <= header[8]);
        const u8* file = image + header[7] + entry;
        u32 length;
        length = snx_ld_le32(file + 0x1c);
        CHECK((u64)entry + 0x20 + length <= header[8]);
        int which = -1;
        if (length == 9 && !memcmp(file + 0x20, "asset.bin", 9)) which = 0;
        if (length == 9 && !memcmp(file + 0x20, "empty.bin", 9)) which = 1;
        if (length == 8 && !memcmp(file + 0x20, "zulu.bin", 8)) which = 2;
        CHECK(which >= 0 && !(found & (1u << which)));
        data_offsets[which] = snx_ld_le64(file + 8);
        sizes[which] = snx_ld_le64(file + 16);
        found |= 1u << which;
        entry = snx_ld_le32(file + 4);
    }
    CHECK(found == 7 && sizes[0] == 10 && sizes[1] == 0 && sizes[2] == 4);
    CHECK(data_offsets[0] == 0 && data_offsets[1] == data_offsets[2] && data_offsets[2] == 16);
    CHECK(!memcmp(image + header[9], "romfs test", 10));
    for (u64 i = 10; i < 16; i++) CHECK(image[header[9] + i] == 0);
    CHECK(!memcmp(image + header[9] + data_offsets[2], "tail", 4));
#if !defined(_WIN32)
    test_romfs_names(image, header);
#endif
    test_romfs_listing(image, header);
    tstorage_read(&storage, header[9] + 7, 13);
    const u8 spanning[13] = {'e', 's', 't', 0, 0, 0, 0, 0, 0, 't', 'a', 'i', 'l'};
    CHECK(!memcmp(GP(0x5000), spanning, sizeof spanning) && GP(0x5000)[13] == 0xcc);
    tstorage_close(&storage);
    /* RomFS files are mapped from startup and readonly; zulu.bin was refused and is read on demand */
    FsNode *asset, *zulu;
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs/asset.bin", &asset) && asset->fd >= 0 && asset->readonly);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs/zulu.bin", &zulu) && zulu->fd < 0 && zulu->readonly);
    CHECK(fs_tree_remove(&g_hle.fs.tree, NULL, "romfs/asset.bin", 0, 0, 0) == FS_TREE_READONLY);
    CHECK(fs_tree_rename(&g_hle.fs.tree, NULL, "romfs/asset.bin", "romfs/moved.bin", 0) == FS_TREE_READONLY);
    CHECK(fs_tree_create(&g_hle.fs.tree, NULL, "romfs/new.bin", 0, 0, NULL) == FS_TREE_READONLY);
    pm_unlock(&g_hle.fs.tree.lock);
    PFileMap map;
    CHECK(!fs_map_file("romfs/asset.bin", &map) && map.borrowed && map.data == asset->map && map.size == 10);
    plat_file_unmap(&map);
    CHECK(!map.data && !map.borrowed && asset->map != NULL);
    u64 size = 0;
    u8* bytes = fs_read_file("romfs/asset.bin", &size);
    CHECK(bytes != NULL && size == 10 && !memcmp(bytes, "romfs test", 10));
    free(bytes);
    puts("PASS extracted RomFS metadata, padding, cross-file reads and zero-length shared offsets");
}

static void test_raw_romfs(void) {
    const u8 expected[] = {'R', 'A', 'W', 0, 'R', 'O', 'M', 'F', 'S', 0xff, ' ', 'b', 'y', 't', 'e', 's'};
    TStorage storage = open_romfs();
    CHECK(g_hle.fs.rimage != NULL && g_hle.fs.rraw == NULL); /* the image is mapped, not read through a FILE */
    CHECK(tstorage_size(&storage) == sizeof expected);
    test_storage_contracts(&storage, sizeof expected);
    tstorage_read(&storage, 2, sizeof expected - 2);
    CHECK(!memcmp(GP(0x5000), expected + 2, sizeof expected - 2));
    CHECK(GP(0x5000)[sizeof expected - 2] == 0xcc);
    tstorage_close(&storage);
    puts("PASS raw RomFS reads and cached image size");
}

#if !defined(_WIN32) /* the dump's link: Windows makes one only with the right to */
/* the synthetic dumps of tests/hac_synth.h, under names fs.c doesn't use */
#define put32 synth_put32
#define put64 synth_put64
#define romfs_build synth_romfs_build
#include "nx/runtime/driver/hactool/hfs0.h"
#include "nx/runtime/driver/hactool/nca.h"
#include "nx/runtime/driver/hactool/pfs0.h"
#include "nx/runtime/driver/hactool/sha.h"
#include "nx/runtime/driver/hactool/xci.h"
#include "hac_synth.h"
#undef put32
#undef put64
#undef romfs_build
#endif
#if !defined(_WIN32)
/* Extract dump off: dump-data/Game_data has no ExeFS or RomFS of its own, and dump-data/Game.source links to
   an NSP of decrypted NCAs, which fs_init opens and maps. The game's ExeFS (exefs/...) and RomFS come out of it, read
   in place: nothing is written beside it. With source, the settings' source= names the NSP instead, and Game.source
   is a file that is no dump, which the setting is read over */
static const char dump_code[] = "synthetic main", dump_asset[] = "an asset of the dump's RomFS";
static blob_t dump_npdm(void) {
    blob_t b = {zalloc(0x80), 0x80};
    memcpy(b.data, "META", 4);
    b.data[0x40] = 0x2A; /* told apart from a blank one */
    return b;
}
/* synthetic keys, never a console's: a header_key (dump_header_key as bytes, its XTS halves apart as OpenSSL requires)
   and key area keys up to master key 19 */
#define DUMP_SECRET "0123456789abcdef0123456789abcdef"
static const char dump_keys[] = "header_key = " DUMP_SECRET "fedcba9876543210fedcba9876543210\n"
                                "key_area_key_application_00 = " DUMP_SECRET "\n"
                                "key_area_key_application_13 = " DUMP_SECRET "\n";
static const unsigned char dump_header_key[32] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45,
                                                  0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54,
                                                  0x32, 0x10, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
/* how dump_write_nsp makes the dump: whole; its program NCA encrypted with master key 20, past what
   dump-data/prod.keys (dump_keys) has; its RomFS behind a compression table that lists 0x20 bytes past what it stores,
   or 64 MiB of zeros past its image (bigger than the dump, and whole); or the file cut 1024 bytes short of its table */
enum { DUMP_WHOLE, DUMP_SEALED, DUMP_ROMFS_CUT, DUMP_ROMFS_ZEROS, DUMP_CUT_SHORT };
static u64 dump_table_end; /* where the NSP's table puts its last file's end */
static void dump_write_nsp(int source, int kind) {
    blob_t code = {zalloc(sizeof dump_code - 1), sizeof dump_code - 1}, asset = {zalloc(sizeof dump_asset - 1), 0};
    blob_t meta = dump_npdm(), nsp;
    asset.size = sizeof dump_asset - 1;
    memcpy(code.data, dump_code, code.size);
    memcpy(asset.data, dump_asset, asset.size);
    romfs_file_t exefs[2] = {{"main.npdm", meta}, {"main", code}}, romfs[1] = {{"asset.bin", asset}};
    romfs_file_t ncas[1] = {{"program.nca", dnca_build_romfs(NCACONTENTTYPE_PROGRAM, 0x0100000000AB0000ULL, exefs, 2,
                                                             romfs, 1, kind == DUMP_ROMFS_CUT ? 0x20 : 0,
                                                             kind == DUMP_ROMFS_ZEROS ? 64ull << 20 : 0)}};
    if (kind == DUMP_SEALED) {
        nca_encrypt(&ncas[0].content, dump_header_key, 20);
        CHECK(!snx_mkdirs("dump-data") && !snx_write_file("dump-data/prod.keys", dump_keys, sizeof dump_keys - 1));
    }
    nsp = partition_build(MAGIC_PFS0, ncas, 1, 0x200);
    dump_table_end = pfs0_files_end(nsp.data);
    if (kind == DUMP_CUT_SHORT) nsp.size = (size_t)dump_table_end - 1024;
    CHECK(!snx_mkdirs("dump-data/Game_data") && !snx_write_file("dump-data/Game.nsp", nsp.data, nsp.size));
    if (source)
        CHECK(!snx_write_file("dump-data/Game.source", "no dump", 7));
    else
        CHECK(!snx_symlink("Game.nsp", "dump-data/Game.source"));
    free(nsp.data);
    free(ncas[0].content.data);
    free(code.data);
    free(asset.data);
    free(meta.data);
}
static void dump_write(int source) { dump_write_nsp(source, DUMP_WHOLE); }
/* the error lines the dump's reading says (snx_error_sink), the last in test_error_text */
static char test_error_text[2048];
static void test_error_line(const char* line) { snprintf(test_error_text, sizeof test_error_text, "%s", line); }
static void test_dump(void) {
    blob_t meta = dump_npdm();
    PFileMap map;
    u64 size = 0;
    u8* data = fs_read_file("exefs/main.npdm", &size);
    CHECK(g_hle.fs.dfs != NULL && data && size == meta.size && !memcmp(data, meta.data, meta.size));
    free(data);
    CHECK(!fs_map_file("exefs/main", &map) && map.heap && map.size == sizeof dump_code - 1);
    CHECK(!memcmp(map.data, dump_code, map.size));
    map.borrowed = 0;
    plat_file_unmap(&map); /* the test's frees it, as the platform's does for heap */
    CHECK(fs_read_file("exefs/subsdk0", &size) == NULL && fs_read_file("romfs/asset.bin", &size) == NULL);
    CHECK(!snx_path_exists("dump-data/Game_data/exefs") && !snx_path_exists("dump-data/Game_data/romfs"));
    /* the RomFS image, what the game's MountRom reads: its header, then asset.bin's bytes where the file table says
       they are */
    TStorage storage = open_romfs();
    u64 total = tstorage_size(&storage);
    CHECK(total >= 0x50 && total <= 0x8000);
    test_storage_contracts(&storage, total);
    tstorage_read(&storage, 0, total);
    const u8* image = GP(0x5000);
    u64 file_table = 0, data_offset = 0, at, name_size;
    file_table = snx_ld_le64(image + 0x38);
    data_offset = snx_ld_le64(image + 0x48);
    CHECK(file_table + 0x20 + 9 <= total && !memcmp(image + file_table + 0x20, "asset.bin", 9));
    at = snx_ld_le64(image + file_table + 8);
    size = snx_ld_le64(image + file_table + 0x10);
    name_size = 0;
    name_size = snx_ld_le32(image + file_table + 0x1c);
    CHECK(name_size == 9 && size == sizeof dump_asset - 1 && data_offset + at + size <= total);
    CHECK(!memcmp(image + data_offset + at, dump_asset, size));
    tstorage_close(&storage);
    free(meta.data);
    puts("PASS the dump's ExeFS and RomFS read in place, nothing extracted, its read-ahead paced");
}

/* the RomFS image storage gives, a file name and its bytes the first file in its table, as a dump's image of one file
   lays it out */
static void image_holds(TStorage* storage, const char* name, const char* contents) {
    u64 total = tstorage_size(storage), file_table = 0, data_offset = 0, at = 0, size = 0, name_size = 0;
    const u8* image = GP(0x5000);
    CHECK(total >= 0x50 && total <= 0x8000);
    test_storage_contracts(storage, total);
    test_storage_refusals(storage, total);
    tstorage_read(storage, 0, total);
    file_table = snx_ld_le64(image + 0x38);
    data_offset = snx_ld_le64(image + 0x48);
    CHECK(file_table + 0x20 + strlen(name) <= total && !memcmp(image + file_table + 0x20, name, strlen(name)));
    at = snx_ld_le64(image + file_table + 8);
    size = snx_ld_le64(image + file_table + 0x10);
    name_size = snx_ld_le32(image + file_table + 0x1c);
    CHECK(name_size == strlen(name) && size == strlen(contents) && data_offset + at + size <= total);
    CHECK(!memcmp(image + data_offset + at, contents, size));
}
/* an update's or DLCs' dumps beside the game's (dump_write), the game named by its title ID, 0100000000AB0000. An
   update runs as its own program, <game>_<its ID>, whose .ini names its dump; it is its game's ID with 0x800, with
   an ExeFS and RomFS of its own. DLCs are checked in the game's addons= and played by the game's program, each a
   PublicData NCA past the game's 0x1000 in a dump its own .ini names: two read in place, a third checked without a
   dump recorded, and the update listed too, which the game's program leaves out. With extracted, the game's ExeFS
   and RomFS are extracted in its data folder too, and the update's are still read over them */
static const char addon_code[] = "the update's main", addon_asset[] = "the update's asset";
static const char* const dlc_assets[] = {"a DLC's data", "another DLC's"};
static void addon_dump(const char* path, int update, SnxU64 id, const char* file, const char* contents,
                       const char* own) {
    blob_t code = {zalloc(sizeof addon_code - 1), sizeof addon_code - 1}, meta = dump_npdm(), nsp;
    blob_t asset = {zalloc(64), strlen(contents)};
    romfs_file_t exefs[2] = {{"main.npdm", meta}, {"main", code}}, romfs[1] = {{file, asset}};
    romfs_file_t ncas[1];
    char *ini = snx_format("dump-data/%s.ini", own), *text = snx_format("[Game]\nsource=%s\n", path);
    memcpy(code.data, addon_code, code.size);
    memcpy(asset.data, contents, asset.size);
    ncas[0].name = update ? "patch.nca" : "dlc.nca";
    ncas[0].content = update ? dnca_build(NCACONTENTTYPE_PROGRAM, id, exefs, 2, romfs, 1)
                             : dnca_build(NCACONTENTTPYE_PUBLICDATA, id, NULL, 0, romfs, 1);
    nsp = partition_build(MAGIC_PFS0, ncas, 1, 0x200);
    CHECK(!snx_write_file(path, nsp.data, nsp.size));
    CHECK(!snx_write_file(ini, text, strlen(text)));
    free(ini);
    free(text);
    free(nsp.data);
    free(ncas[0].content.data);
    free(code.data);
    free(asset.data);
    free(meta.data);
}
static void addon_write(int update, int extracted) {
    strcpy(g_confdir, "dump-data/");
    strcpy(g_exedir, "dump-data/");
    strcpy(g_gamename, "0100000000AB0000");
    if (update) {
        g_addon = 0x0100000000AB0800ULL;
        strcpy(g_exename, "0100000000AB0000_0100000000AB0800");
        strcpy(g_addonrec, g_exename); /* main.c's addon_init: the update's own program, its own record */
        snprintf(g_addondir, sizeof g_addondir, "dump-data/%s_data/", g_exename);
        addon_dump("dump-data/Update.nsp", 1, g_addon, "asset.bin", addon_asset, g_exename);
    } else {
        addon_dump("dump-data/Dlc1.nsp", 0, 0x0100000000AB1001ULL, "dlc.bin", dlc_assets[0],
                   "0100000000AB0000_0100000000AB1001");
        addon_dump("dump-data/Dlc2.nsp", 0, 0x0100000000AB1002ULL, "dlc2.bin", dlc_assets[1],
                   "0100000000AB0000_0100000000AB1002");
        strcpy(g_settings.game_addons, "0100000000AB1001, 0100000000ab1002,0100000000AB1003,0100000000AB0800");
    }
    if (extracted)
        CHECK(!snx_mkdirs("dump-data/Game_data/exefs") && !snx_mkdirs("dump-data/Game_data/romfs") &&
              !snx_write_file("dump-data/Game_data/exefs/main", "the game's main", 15) &&
              !snx_write_file("dump-data/Game_data/romfs/asset.bin", "the game's", 10));
}
static void test_addon(int update) {
    static const char* const files[] = {"dlc.bin", "dlc2.bin"};
    u64 size = 0;
    u8* data = fs_read_file("exefs/main", &size);
    if (update) { /* the update's ExeFS and RomFS, over the game's dump, whatever the game has extracted */
        CHECK(g_hle.fs.dump.patch.found && data && size == sizeof addon_code - 1 && !memcmp(data, addon_code, size));
        free(data);
        CHECK(fs_read_file("exefs/subsdk0", &size) == NULL);
        TStorage storage = open_romfs();
        image_holds(&storage, "asset.bin", addon_asset);
        tstorage_close(&storage);
        puts("PASS an update's ExeFS and RomFS read from its dump over the game's");
        return;
    }
    /* DLCs: the game's own program from its dump, the update left out, and each DLC's image as installed, by its
       data ID; the one without a dump is not, and said so */
    CHECK(data && size == sizeof dump_code - 1 && !memcmp(data, dump_code, size));
    free(data);
    CHECK(g_hle.fs.naocs == 2 && !g_hle.fs.dump.patch.found);
    { /* the DLCs the game plays with, which its window names: the two found, in addons= order */
        const u64* ids;
        CHECK(fs_dlcs(&ids) == 2 && ids[0] == 0x0100000000AB1001ULL && ids[1] == 0x0100000000AB1002ULL);
    }
    CHECK(fs_aoc_has(0x0100000000AB1001ULL) && fs_aoc_has(0x0100000000AB1002ULL) && !fs_aoc_has(0x0100000000AB1003ULL));
    CHECK(strstr(test_fs_text, "the DLC 0100000000AB1003 is checked") != NULL);
    for (u64 i = 0; i < 2; i++) {
        TStorage storage = open_data(0x0100000000AB1001ULL + i);
        CHECK(storage.st.aoc >= 0); /* read in place from its dump */
        image_holds(&storage, files[i], dlc_assets[i]);
        tstorage_close(&storage);
    }
    { /* the DLC without a dump: not installed, an empty archive */
        TStorage storage = open_data(0x0100000000AB1003ULL);
        CHECK(storage.st.aoc < 0 && tstorage_size(&storage) == sizeof g_hle.fs.emptyrom);
        tstorage_close(&storage);
    }
    puts("PASS two DLCs' images read from their dumps as installed, the game's program from the game's");
}
#endif
/* An update extracted in its data folder (<game>_<its ID>_data), laid over the game's (<game>_data), and a DLC checked
   in addons= extracted in its own: the update's ExeFS and RomFS whole, the game's RomFS files it does not have gone, its other folders merged, all of it read-only. The game's saves
   stay where they are, writable */
static void test_overlay(void) {
    static const char* const made[][2] = {
        {"overlay-data/0100000000AB0000_data/exefs/main", "the game's main"},
        {"overlay-data/0100000000AB0000_data/romfs/a.bin", "the game's a"},
        {"overlay-data/0100000000AB0000_data/romfs/b.bin", "the game's b"},
        {"overlay-data/0100000000AB0000_data/system/archives/0100000000AB1002.romfs", "the game's DLC"},
        {"overlay-data/0100000000AB0000_data/save/user/progress", "a save"},
        {"overlay-data/0100000000AB0000_0100000000AB0800_data/exefs/main", "the update's main"},
        {"overlay-data/0100000000AB0000_0100000000AB0800_data/romfs/a.bin", "the update's a"},
        {"overlay-data/0100000000AB0000_0100000000AB0800_data/system/archives/0100000000AB1003.romfs", "another DLC"},
        {"overlay-data/0100000000AB0000_0100000000AB0800_data/control.nacp", "the update's control"},
        {"overlay-data/0100000000AB0000_0100000000AB1001_data/system/archives/0100000000AB1001.romfs", "a DLC"},
        {"overlay-data/0100000000AB0000_0100000000AB1004_data/system/archives/0100000000AB1004.romfs", "unchecked"},
        {"overlay-data/0100000000AB0000_0100000000AB0800_data/sdmc/stray", "an SD card file left there"},
        {"overlay-data/0100000000AB0000_0100000000AB0800_data/save/user/stray", "a save left there"},
    };
    FsNode* node;
    u64 size = 0;
    u8* data;
    for (size_t i = 0; i < sizeof made / sizeof made[0]; i++) {
        char* folder = snx_dirname(made[i][0]);
        CHECK(!snx_mkdirs(folder) && !snx_write_file(made[i][0], made[i][1], strlen(made[i][1])));
        free(folder);
    }
    g_addon = 0x0100000000AB0800ULL;
    strcpy(g_exedir, "overlay-data/");
    strcpy(g_confdir, "overlay-data/");
    strcpy(g_gamename, "0100000000AB0000");
    strcpy(g_settings.game_addons, "0100000000AB1001");
    strcpy(g_addondir, "overlay-data/0100000000AB0000_0100000000AB0800_data/");
    strcpy(g_datadir, "overlay-data/0100000000AB0000_data/");
    fs_init(NULL, NULL);
    data = fs_read_file("exefs/main", &size);
    CHECK(data && size == 17 && !memcmp(data, "the update's main", 17));
    free(data);
    data = fs_read_file("romfs/a.bin", &size);
    CHECK(data && size == 14 && !memcmp(data, "the update's a", 14));
    free(data);
    data = fs_read_file("control.nacp", &size);
    CHECK(data && size == 20 && !memcmp(data, "the update's control", 20));
    free(data);
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs/b.bin", &node) == FS_TREE_NOTFOUND);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "system/archives/0100000000AB1002.romfs", &node) && !node->readonly);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "system/archives/0100000000AB1003.romfs", &node) && node->readonly);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "save/user/progress", &node) && !node->readonly);
    /* what the update's folder holds besides its parts left out: the game's saves and SD card its own, writable */
    CHECK(fs_tree_lookup(&g_hle.fs.tree, NULL, "sdmc/stray", &node) == FS_TREE_NOTFOUND);
    CHECK(fs_tree_lookup(&g_hle.fs.tree, NULL, "save/user/stray", &node) == FS_TREE_NOTFOUND);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "save/user", &node) && !node->readonly);
    /* the checked DLC's extraction laid over it too, the unchecked one's not */
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "system/archives/0100000000AB1001.romfs", &node) && node->readonly);
    CHECK(fs_tree_lookup(&g_hle.fs.tree, NULL, "system/archives/0100000000AB1004.romfs", &node) == FS_TREE_NOTFOUND);
    {
        const u64* ids;
        CHECK(fs_dlcs(&ids) == 1 && ids[0] == 0x0100000000AB1001ULL);
    }
    pm_unlock(&g_hle.fs.tree.lock);
    CHECK(snx_is_file("overlay-data/0100000000AB0000_data/romfs/b.bin")); /* hidden, never removed */
    romfs_init();
    CHECK(g_hle.fs.nrfiles == 1);
    puts("PASS an update's and a checked DLC's extractions laid over their game's data folder");
}

/* Mods in the game's data folder, mods/<mod>/: the played ones' exefs/ files read in place of the game's when the program
   was recompiled with them (its stamp, <program>.abi, says so), else the game's own, said; and their romfs/ folders
   laid over the game's RomFS in their names' order, a file of a later one over an earlier one's and the game's. A mod
   mods_off= names, a hidden folder and a file are no mods played, and nothing under mods/ is indexed with the data
   folder: a link in a mod left out doesn't stop the game. image: the game's RomFS a romfs.bin image instead of its
   romfs/ folder. stale: the program was recompiled without the mods' ExeFS files */
static const char* const mod_made[][2] = {
    {"mods-data/0100000000AB0000_data/exefs/main", "the game's main"},
    {"mods-data/0100000000AB0000_data/exefs/main.npdm", "the game's npdm"},
    {"mods-data/0100000000AB0000_data/mods/1 first/romfs/a.bin", "first's a"},
    {"mods-data/0100000000AB0000_data/mods/1 first/romfs/new/n.bin", "first's new"},
    {"mods-data/0100000000AB0000_data/mods/2 second/romfs/a.bin", "second's a"},
    {"mods-data/0100000000AB0000_data/mods/2 second/romfs/dir/b.bin", "second's b"},
    {"mods-data/0100000000AB0000_data/mods/2 second/exefs/main", "second's main"},
    {"mods-data/0100000000AB0000_data/mods/2 second/exefs/notes.txt", "no part of the program"},
    {"mods-data/0100000000AB0000_data/mods/2 second/exefs/frames.pchtxt", "@nsobid-0123\n@enabled\n"},
    {"mods-data/0100000000AB0000_data/mods/1 first/exefs/0123.ips", "PATCHEOF"},
    {"mods-data/0100000000AB0000_data/mods/3 off/romfs/a.bin", "off's a"},
    {"mods-data/0100000000AB0000_data/mods/3 off/exefs/subsdk0", "off's subsdk0"},
    {"mods-data/0100000000AB0000_data/mods/3 off/exefs/0123.ips", "PATCHEOF"},
    {"mods-data/0100000000AB0000_data/mods/.hidden/romfs/a.bin", "hidden's a"},
    {"mods-data/0100000000AB0000_data/mods/readme.txt", "no mod"},
};
/* the bytes of the file at path (folders split by /) in the RomFS image, with header its header: NULL for none */
static const u8* image_file(const u8* image, const u64* header, const char* path, u64* size) {
    char part[64];
    u32 parent = 0, entry;
    const char* slash;
    while ((slash = strchr(path, '/'))) {
        CHECK((size_t)(slash - path) < sizeof part);
        memcpy(part, path, (size_t)(slash - path));
        part[slash - path] = 0;
        if ((parent = romfs_find(image, header, 1, parent, part)) == UINT32_MAX) return NULL;
        path = slash + 1;
    }
    if ((entry = romfs_find(image, header, 0, parent, path)) == UINT32_MAX) return NULL;
    *size = snx_ld_le64(image + header[7] + entry + 16);
    return image + header[9] + snx_ld_le64(image + header[7] + entry + 8);
}
static void image_has(const u8* image, const u64* header, const char* path, const char* contents) {
    u64 size = 0;
    const u8* data = image_file(image, header, path, &size);
    if (!data || size != strlen(contents) || memcmp(data, contents, size))
        fprintf(stderr, "%s: expected \"%s\"\n", path, contents);
    CHECK(data && size == strlen(contents) && !memcmp(data, contents, size));
}
static void test_mods(int image, int stale) {
    u64 size = 0, header[10], total;
    u8* data = fs_read_file("exefs/main", &size);
    const u8* bytes;
    FsNode* node;
    if (stale) { /* the program's own ExeFS, said, and no patch: its code was made without them */
        CHECK(data && size == 15 && !memcmp(data, "the game's main", 15));
        CHECK(strstr(test_fs_text, "the program was recompiled with none of them") != NULL && g_hle.fs.nmexefs == 0);
        CHECK(g_hle.fs.npatches == 0);
    } else { /* and the patches it was made with, for the loader: the played mods' in their order */
        CHECK(data && size == 13 && !memcmp(data, "second's main", 13) && g_hle.fs.nmexefs == 1);
        CHECK(g_hle.fs.npatches == 2 && strstr(g_hle.fs.patches[0], "1 first") &&
              strstr(g_hle.fs.patches[0], "0123.ips") && strstr(g_hle.fs.patches[1], "2 second") &&
              strstr(g_hle.fs.patches[1], "frames.pchtxt"));
    }
    free(data);
#if !defined(_WIN32)
    /* the two played mods a symbolic link keeps out are said, each with its path; the one turned off is not */
    if (test_mod_links != 2) fprintf(stderr, "said of the mods that are links:\n%s", test_mod_link_text);
    CHECK(test_mod_links == 2);
    CHECK(strstr(test_mod_link_text, "/mods/4 linked is a symbolic link, which mods aren't read through: it is left "
                                     "out\n") &&
          strstr(test_mod_link_text, "fs: the mod's ") &&
          strstr(test_mod_link_text, "/mods/5 part/romfs is a symbolic link, which mods aren't read through: it is "
                                     "left out\n"));
#endif
    data = fs_read_file("exefs/main.npdm", &size);
    CHECK(data && size == 15 && !memcmp(data, "the game's npdm", 15));
    free(data);
    CHECK(fs_read_file("exefs/subsdk0", &size) == NULL && fs_read_file("exefs/notes.txt", &size) == NULL);
    { /* mapped, as the loader takes a module */
        PFileMap map;
        CHECK(!fs_map_file("exefs/main", &map) && map.size == (stale ? 15u : 13u));
        plat_file_unmap(&map);
    }
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(fs_tree_lookup(&g_hle.fs.tree, NULL, "mods", &node) == FS_TREE_NOTFOUND);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "mods.1/a.bin", &node) && node->readonly);
    CHECK(fs_tree_lookup(&g_hle.fs.tree, NULL, image ? "mods.3" : "mods.2", &node) == FS_TREE_NOTFOUND);
    pm_unlock(&g_hle.fs.tree.lock);
    CHECK(g_hle.fs.nmromfs == (image ? 3u : 2u) && g_hle.fs.rlayered);
    TStorage storage = open_romfs();
    total = tstorage_size(&storage);
    CHECK(total > 0x200 && total <= 0x8000);
    test_storage_contracts(&storage, total);
    tstorage_read(&storage, 0, total);
    const u8* view = GP(0x5000);
    memcpy(header, view, sizeof header);
    snx_le64_array(header, 10);
    image_has(view, header, "a.bin", "second's a");
    image_has(view, header, "dir/b.bin", "second's b");
    image_has(view, header, "dir/c.bin", "the game's c");
    image_has(view, header, "new/n.bin", "first's new");
    image_has(view, header, "z.bin", "the game's z");
    CHECK(!image_file(view, header, "link", &size)); /* the mod left out's */
    bytes = image_file(view, header, "empty.bin", &size);
    CHECK(bytes && size == 0);
    { /* a read across a mod's file and the game's, from where each is */
        u64 a_size = 0, z_size = 0;
        const u8* a = image_file(view, header, "a.bin", &a_size);
        const u8* z = image_file(view, header, "z.bin", &z_size);
        u64 from = (u64)(a - view) + a_size - 3, length = (u64)(z - view) + z_size - from;
        u8 expect[64];
        CHECK(z > a && length <= sizeof expect);
        memcpy(expect, view + from, (size_t)length);
        tstorage_read(&storage, from, length);
        CHECK(!memcmp(GP(0x5000), expect, (size_t)length) && !memcmp(GP(0x5000), "s a", 3) &&
              !memcmp(GP(0x5000) + length - z_size, "the game's z", 12));
    }
    tstorage_close(&storage);
    puts(stale ? "PASS mods' RomFS laid over the game's, their ExeFS left out of a program made without it"
               : image ? "PASS mods' ExeFS and RomFS laid over the game's RomFS image"
                       : "PASS mods' ExeFS and RomFS laid over the game's extracted files");
}
static void mods_write(int image, int stale) {
    char *mods, *stamp, *text;
    for (size_t i = 0; i < sizeof mod_made / sizeof mod_made[0]; i++) {
        char* folder = snx_dirname(mod_made[i][0]);
        CHECK(!snx_mkdirs(folder) && !snx_write_file(mod_made[i][0], mod_made[i][1], strlen(mod_made[i][1])));
        free(folder);
    }
    if (image) {
#if !defined(_WIN32)
        static const char* const names[] = {"a.bin", "empty.bin", "z.bin"};
        static const char* const contents[] = {"the game's a", "", "the game's z"};
        romfs_file_t files[3];
        blob_t made;
        for (int i = 0; i < 3; i++) {
            files[i].name = names[i];
            files[i].content.data = (unsigned char*)contents[i];
            files[i].content.size = strlen(contents[i]);
        }
        made = synth_romfs_build(files, 3);
        CHECK(!snx_write_file("mods-data/0100000000AB0000_data/romfs.bin", made.data, made.size));
        free(made.data);
        /* the game's folder dir/ is only in the mods' then: c.bin from a mod of its own */
        CHECK(!snx_mkdirs("mods-data/0100000000AB0000_data/mods/0 c/romfs/dir") &&
              !snx_write_file("mods-data/0100000000AB0000_data/mods/0 c/romfs/dir/c.bin", "the game's c", 12));
#endif
    } else {
        CHECK(!snx_mkdirs("mods-data/0100000000AB0000_data/romfs/dir") &&
              !snx_write_file("mods-data/0100000000AB0000_data/romfs/a.bin", "the game's a", 12) &&
              !snx_write_file("mods-data/0100000000AB0000_data/romfs/empty.bin", "", 0) &&
              !snx_write_file("mods-data/0100000000AB0000_data/romfs/z.bin", "the game's z", 12) &&
              !snx_write_file("mods-data/0100000000AB0000_data/romfs/dir/b.bin", "the game's b", 12) &&
              !snx_write_file("mods-data/0100000000AB0000_data/romfs/dir/c.bin", "the game's c", 12));
    }
#if !defined(_WIN32)
    CHECK(!snx_symlink("a.bin", "mods-data/0100000000AB0000_data/mods/3 off/romfs/link"));
    /* a mod that is a link, one whose romfs/ is one, and one turned off that is one: none is played */
    CHECK(!snx_symlink("1 first", "mods-data/0100000000AB0000_data/mods/4 linked") &&
          !snx_mkdirs("mods-data/0100000000AB0000_data/mods/5 part") &&
          !snx_symlink("../1 first/romfs", "mods-data/0100000000AB0000_data/mods/5 part/romfs") &&
          !snx_symlink("1 first", "mods-data/0100000000AB0000_data/mods/6 off"));
#endif
    strcpy(g_exedir, "mods-data/");
    strcpy(g_exename, "0100000000AB0000");
    strcpy(g_gamename, "0100000000AB0000");
    strcpy(g_confdir, "mods-data/");
    strcpy(g_settings.game_mods_off, "3 off/6 off");
    mods = snx_mods_folder("mods-data/0100000000AB0000_data");
    stamp = snx_mods_stamp(mods, g_settings.game_mods_off);
    CHECK(strlen(stamp) == 16);
    /* the image run's stamp ends with the made line the recompile writes now, the other's is one from before it */
    text = stale ? snx_strdup("1")
                 : snx_format("1 mods %s%s", stamp, image ? "\nmade opt=2 aot=1 found=10 code=20\n" : "");
    CHECK(!snx_write_file("mods-data/0100000000AB0000.abi", text, strlen(text)));
    free(text);
    free(stamp);
    free(mods);
    strcpy(g_datadir, "mods-data/0100000000AB0000_data/");
}

#define FIRST_SAVE "save/user.0123456789abcdeffedcba9876543210"
#define OTHER_SAVE "save/user.04040404040404040404040404040404"
static const u64 test_other_user[2] = {0x0404040404040404ull, 0x0404040404040404ull};
static int tree_has(const char* path) {
    FsNode* node;
    pm_lock(&g_hle.fs.tree.lock);
    int found = !fs_tree_lookup(&g_hle.fs.tree, NULL, path, &node);
    pm_unlock(&g_hle.fs.tree.lock);
    return found;
}
/* save states (runtime/state.c): the bytes a state's sections hold, in memory */
typedef struct {
    u8* p;
    u64 n, at;
} StateBytes;
static int bytes_put(void* ctx, const void* p, u64 n) {
    StateBytes* b = (StateBytes*)ctx;
    u8* more = (u8*)realloc(b->p, (size_t)(b->n + n + 1));
    if (!more) return -1;
    b->p = more;
    memcpy(b->p + b->n, p, (size_t)n);
    b->n += n;
    return 0;
}
static int bytes_get(void* ctx, void* p, u64 n) {
    StateBytes* b = (StateBytes*)ctx;
    if (n > b->n - b->at) return -1;
    memcpy(p, b->p + b->at, (size_t)n);
    b->at += n;
    return 0;
}
static void temp_empty(void) { /* as fs_init leaves it: every launch starts the temporary storage empty */
    pm_lock(&g_hle.fs.tree.lock);
    FsTreeResult r = fs_tree_remove(&g_hle.fs.tree, NULL, "save/temp", 1, 1, 0);
    pm_unlock(&g_hle.fs.tree.lock);
    CHECK(r == FS_TREE_OK || r == FS_TREE_NOTFOUND);
}
/* the state's open file systems, files and folders found again, as state_load does first: NULL with why when refused */
static FsStaged* resolve_fs(StateBytes* list, StateBytes* temp, const HleFs* saved, char* why, size_t size) {
    StateIo lio = {list, bytes_put, bytes_get}, tio = {temp, bytes_put, bytes_get};
    list->at = 0;
    if (temp) temp->at = 0;
    return fs_state_resolve(&lio, temp ? &tio : NULL, saved, why, size);
}
/* and then what the running game had open let go of, and the state's in place: 0, or -1 with why when refused */
static int load_fs(StateBytes* list, StateBytes* temp, const HleFs* saved, char* why, size_t size) {
    FsStaged* st = resolve_fs(list, temp, saved, why, size);
    if (!st) return -1;
    fs_state_drop();
    fs_state_install(st);
    return 0;
}
/* A state holds the application's temporary storage whole, which every launch empties: a load in a fresh process puts
   its folders and files back before it opens what the game had open there, and a load in the running game takes what
   the game wrote there since away. A state from before states held it is refused as it was, and one whose save/temp
   names a path out of it, or lacks a file the game had open there, is refused before anything changes */
static void test_state_temp(void) {
    StateBytes list = {0}, temp = {0}, bad = {0};
    StateIo lio = {&list, bytes_put, bytes_get}, tio = {&temp, bytes_put, bytes_get}, bio = {&bad, bytes_put, bytes_get};
    char why[256];
    TFs fs = mount_save(4); /* Temporary */
    CHECK(tree_has("save/temp"));
    root_op(fs, CREATE_FILE, "/a.bin", 0, 0);
    root_op(fs, CREATE_DIR, "/d", 0, 0);
    root_op(fs, CREATE_FILE, "/d/b.bin", 0, 0);
    TFile a = open_file(fs, "/a.bin", 7);
    write_data(a, 0, "hello", 5, 0);
    TFile b = open_file(fs, "/d/b.bin", 7);
    write_data(b, 0, "xyz", 3, 0);
    close_file(b);
    HleFs* saved = (HleFs*)malloc(sizeof *saved);
    CHECK(saved != NULL);
    *saved = g_hle.fs; /* what the state's g_hle holds of fs */
    CHECK(!fs_state_save(&lio) && !fs_state_temp_save(&tio));

    /* a fresh process: nothing open (Windows deletes no open file), save/temp empty, and the game had it and a.bin in it
       open */
    fs_state_drop();
    temp_empty();
    CHECK(!tree_has("save/temp"));
    CHECK(!resolve_fs(&list, NULL, saved, why, sizeof why) &&
          strstr(why, "the game had save/temp open, which isn't there"));
    FsStaged* fresh = resolve_fs(&list, &temp, saved, why, sizeof why);
    CHECK(fresh != NULL);
    fs_state_install(fresh);
    CHECK(tree_has("save/temp/a.bin") && tree_has("save/temp/d") && tree_has("save/temp/d/b.bin"));
    CHECK(read_data(a, 0, 16, 0) == 5 && !memcmp(GP(0x5000), "hello", 5)); /* a.bin open again, its bytes the state's */
    b = open_file(fs, "/d/b.bin", 1);
    CHECK(read_data(b, 0, 16, 0) == 3 && !memcmp(GP(0x5000), "xyz", 3));
    close_file(b);

    /* the running game: what it wrote there since the save goes, what was there comes back */
    write_data(a, 0, "HELLO, again", 12, 0);
    root_op(fs, CREATE_FILE, "/c.bin", 4, 0);
    CHECK(!load_fs(&list, &temp, saved, why, sizeof why));
    CHECK(!tree_has("save/temp/c.bin") && tree_has("save/temp/d/b.bin"));
    CHECK(read_data(a, 0, 16, 0) == 5 && !memcmp(GP(0x5000), "hello", 5));

    /* a save/temp naming a path out of it, and one without the a.bin the game had open: refused, nothing changed */
    static const char out[] = "../escape.bin";
    u32 one = 1, file = 2, n = sizeof out - 1, zero = 0;
    u64 size = 1;
    CHECK(!io_put32(&bio, one) && !io_put32(&bio, file) && !io_put32(&bio, n) && !io_put(&bio, out, n) &&
          !io_put64(&bio, size) && !io_put(&bio, "x", 1) && !io_put32(&bio, zero));
    CHECK(load_fs(&list, &bad, saved, why, sizeof why) && !strcmp(why, "its temporary storage is damaged"));
    free(bad.p);
    bad = (StateBytes){0};
    CHECK(!io_put32(&bio, one) && !io_put32(&bio, zero));
    CHECK(load_fs(&list, &bad, saved, why, sizeof why) &&
          strstr(why, "the game had save/temp/a.bin open, which the state's temporary storage doesn't hold"));
    CHECK(tree_has("save/temp/a.bin") && read_data(a, 0, 16, 0) == 5);

    close_file(a);
    close_fs(fs);
    temp_empty();
    free(bad.p);
    free(list.p);
    free(temp.p);
    free(saved);
    puts("PASS a state's save/temp: put back in a fresh process and over the running game's, damaged ones refused");
}
static void test_save_data(void) {
    /* each SaveDataType has its folder: System (0), Account (1), Bcat (2), Device (3) */
    TFs system = mount_save(0);
    CHECK(tree_has("save/system") && !tree_has("save/user"));
    TFs bcat = mount_save(2);
    TFs device = mount_save(3);
    CHECK(tree_has("save/bcat") && tree_has("save/device") && !tree_has("save/temp"));
    root_op(system, CREATE_FILE, "/system.bin", 4, 0);
    root_op(bcat, ENTRY_TYPE, "/system.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    root_op(device, ENTRY_TYPE, "/system.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    CHECK(tree_has("save/system/system.bin"));
    root_op(system, DELETE_FILE, "/system.bin", 0, 0);
    close_fs(bcat);
    close_fs(device);

    /* an Account save is its user's folder, save/user.<uid>: another user's has nothing to do with the first's */
    TFs user = mount_user_save(test_other_user);
    CHECK(tree_has(OTHER_SAVE) && !tree_has(FIRST_SAVE) && !tree_has("save/user") && tree_has("save/system"));
    root_op(user, CREATE_FILE, "/other-user.bin", 4, 0);
    close_fs(user);
    /* the first user's, with no save/user and save/system empty, is its folder, made then */
    user = mount_user_save(g_user_id);
    CHECK(tree_has(FIRST_SAVE) && tree_has("save/system"));
    root_op(user, ENTRY_TYPE, "/other-user.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    close_fs(user);
    CHECK(test_left_said == 0);
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_remove(&g_hle.fs.tree, NULL, FIRST_SAVE, 1, 1, 0));
    pm_unlock(&g_hle.fs.tree.lock);
    TFs old = mount_folder("save/user");
    root_op(old, CREATE_FILE, "/progress.bin", 4, 0);
    close_fs(old);
    /* save/user, the one Account save of the versions before there were other users, is the first user's while it
       has no folder: played where it is, nothing moved, and said once a run with where it goes. Another user's save
       leaves it alone */
    user = mount_user_save(test_other_user);
    close_fs(user);
    CHECK(test_left_said == 0);
    user = mount_user_save(g_user_id);
    root_op(user, ENTRY_TYPE, "/progress.bin", 0, 0);
    root_op(user, CREATE_FILE, "/played.bin", 4, 0);
    close_fs(user);
    CHECK(tree_has("save/user/progress.bin") && tree_has("save/user/played.bin") && !tree_has(FIRST_SAVE));
    CHECK(test_left_said == 1 && strstr(test_left_text, "save/user, the first user's saves") &&
          strstr(test_left_text, FIRST_SAVE) && strstr(test_left_text, "deprecations.md"));
    user = mount_user_save(g_user_id);
    close_fs(user);
    CHECK(test_left_said == 1);
    /* the player moves it to the first user's folder, which the save is from then */
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_rename(&g_hle.fs.tree, NULL, "save/user", FIRST_SAVE, 1));
    pm_unlock(&g_hle.fs.tree.lock);
    user = mount_user_save(g_user_id);
    root_op(user, ENTRY_TYPE, "/played.bin", 0, 0);
    close_fs(user);
    CHECK(!tree_has("save/user"));
    /* saves an earlier version put in save/system (it read the type from a byte that is always 0) are the first
       user's while it has no folder and no save/user, played where they are */
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_remove(&g_hle.fs.tree, NULL, FIRST_SAVE, 1, 1, 0));
    pm_unlock(&g_hle.fs.tree.lock);
    root_op(system, CREATE_FILE, "/slot.bin", 4, 0);
    user = mount_user_save(g_user_id);
    root_op(user, ENTRY_TYPE, "/slot.bin", 0, 0);
    close_fs(user);
    CHECK(tree_has("save/system/slot.bin") && !tree_has(FIRST_SAVE) && test_left_said == 1);
    /* with its folder there, save/system is the System saves' alone */
    old = mount_folder(FIRST_SAVE);
    close_fs(old);
    root_op(system, DELETE_FILE, "/slot.bin", 0, 0);
    root_op(system, CREATE_FILE, "/other.bin", 4, 0);
    user = mount_user_save(g_user_id);
    root_op(user, ENTRY_TYPE, "/other.bin", 0, SNX_RC_FS_PATH_NOT_FOUND);
    root_op(user, CREATE_FILE, "/progress.bin", 4, 0);
    close_fs(user);
    CHECK(tree_has("save/system/other.bin") && tree_has(FIRST_SAVE "/progress.bin"));
    /* a save/user beside the first user's folder (an older version run again) is two saves of one user: the game
       stops, and both stay as they were, for the player to choose */
    old = mount_folder("save/user");
    root_op(old, CREATE_FILE, "/newer.bin", 4, 0);
    close_fs(old);
    {
        jmp_buf jump;
        TFs two;
        test_fatal_text[0] = 0;
        test_fatal_jump = &jump;
        if (!setjmp(jump)) {
            mount_save_rc(1, g_user_id, 0, &two);
            CHECK(!"opening the first user's save with two of them did not stop the game");
        }
        test_fatal_jump = NULL;
        CHECK(strstr(test_fatal_text, "Two saves of the first user") && strstr(test_fatal_text, FIRST_SAVE));
    }
    /* tree_has takes the tree's lock: fatal() came after fs_save_root let it go, or this would wait forever */
    CHECK(tree_has("save/user/newer.bin") && tree_has(FIRST_SAVE "/progress.bin") &&
          !tree_has(FIRST_SAVE "/newer.bin"));
    /* a type past the known ones is a System save, and so is the System save opened again */
    TFs unknown = mount_save(0x7F);
    root_op(unknown, ENTRY_TYPE, "/other.bin", 0, 0);
    close_fs(unknown);
    TFs again = mount_save(0);
    root_op(again, ENTRY_TYPE, "/other.bin", 0, 0);
    close_fs(again);
    root_op(system, DELETE_FILE, "/other.bin", 0, 0);
    close_fs(system);
    puts(
        "PASS SaveDataType folders, a folder per user, save/user and save/system played where they are, two saves of one "
        "user refused");
}

/* path on the host is a file */
static int host_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}
/* a file at path on the host, made with its folders */
static void host_put(const char* path) {
    char* folder = snx_strdup(path);
    *strrchr(folder, '/') = 0;
    CHECK(!snx_mkdirs(folder));
    free(folder);
    FILE* f = fopen(path, "wb");
    CHECK(f && fwrite("save", 1, 4, f) == 4 && !fclose(f));
}
/* save_layout (fs.c's save_folder): global keeps a save in the data folder's save/, apart from the game, as the
   console does (user.<uid>/<title ID>, device/<title ID>, bcat/<title ID>, cache/<title ID>/<index>); a save only the
   other layout holds is played where it is, said once; both holding one stops the game, which keeps both. Temporary
   storage and a program with no title ID of its own stay the game's. Each folder there at startup is grafted then,
   and one made later is made with nothing enumerated */
static void test_save_layout(void) {
    char *root = snx_absolute("../../layout-saves"), path[2048];
    jmp_buf jump;
    TFs save;
    FsNode* node;
#define TITLE "0100000000010000"
    snprintf(path, sizeof path, "%s/user.0123456789abcdeffedcba9876543210/" TITLE "/kept.bin", root);
    host_put(path);
    snprintf(path, sizeof path, "%s/bcat/" TITLE "/b.bin", root);
    host_put(path);
    save = mount_save(2); /* the game's own BCAT save, which has a file now: two of it */
    root_op(save, CREATE_FILE, "/p.bin", 4, 0);
    close_fs(save);
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_remove(&g_hle.fs.tree, NULL, "save/user", 1, 1, 0)); /* test_save_data's two first-user saves */
    pm_unlock(&g_hle.fs.tree.lock);
    g_user_count = 2;
    memcpy(g_users[0].uid, g_user_id, sizeof g_users[0].uid);
    memcpy(g_users[1].uid, test_other_user, sizeof g_users[1].uid);
    strcpy(g_settings.save_layout, "global");
    test_scans_allowed = 1;
    fs_saves_init(root);
    test_scans_allowed = 0;
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "global-user.0123456789abcdeffedcba9876543210/kept.bin", &node));
    CHECK(fs_tree_lookup(&g_hle.fs.tree, NULL, "global-device", &node) == FS_TREE_NOTFOUND);
    pm_unlock(&g_hle.fs.tree.lock);
    /* the first user's: in both, the game's own (test_save_data's) and the global one, so the game stops */
    test_fatal_text[0] = 0;
    test_fatal_jump = &jump;
    if (!setjmp(jump)) {
        mount_save_rc(1, g_user_id, 0, &save);
        CHECK(!"two saves of one user's did not stop the game");
    }
    test_fatal_jump = NULL;
    CHECK(strstr(test_fatal_text, "Two saves of this game's user save") && strstr(test_fatal_text, FIRST_SAVE) &&
          strstr(test_fatal_text, "layout-saves/user.0123456789abcdeffedcba9876543210/" TITLE));
    /* with the game's own moved out, the global one */
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_remove(&g_hle.fs.tree, NULL, FIRST_SAVE, 1, 1, 0));
    pm_unlock(&g_hle.fs.tree.lock);
    save = mount_user_save(g_user_id);
    root_op(save, ENTRY_TYPE, "/kept.bin", 0, 0);
    root_op(save, CREATE_FILE, "/new.bin", 4, 0);
    close_fs(save);
    snprintf(path, sizeof path, "%s/user.0123456789abcdeffedcba9876543210/" TITLE "/new.bin", root);
    CHECK(host_file(path) && !tree_has(FIRST_SAVE));
    /* another user's, only in the game's own folder: played there, said once with where it goes */
    test_fs_text[0] = 0;
    save = mount_user_save(test_other_user);
    root_op(save, ENTRY_TYPE, "/other-user.bin", 0, 0);
    close_fs(save);
    CHECK(strstr(test_fs_text, OTHER_SAVE) && strstr(test_fs_text, "save_layout=global keeps in") &&
          strstr(test_fs_text, "layout-saves/user.04040404040404040404040404040404/" TITLE));
    snprintf(path, sizeof path, "%s/user.04040404040404040404040404040404", root);
    CHECK(!snx_is_dir(path));
    test_fs_text[0] = 0;
    save = mount_user_save(test_other_user);
    close_fs(save);
    CHECK(!test_fs_text[0]);
    /* the Device save, in neither (the game's own empty): made in the global folder, nothing enumerated */
    save = mount_save(3);
    root_op(save, CREATE_FILE, "/d.bin", 4, 0);
    close_fs(save);
    snprintf(path, sizeof path, "%s/device/" TITLE "/d.bin", root);
    CHECK(host_file(path) && !tree_has("save/device/d.bin"));
    /* cache storage, by its index */
    CHECK(mount_save_rc(5, NULL, 3, &save) == 0);
    root_op(save, CREATE_FILE, "/c.bin", 4, 0);
    close_fs(save);
    snprintf(path, sizeof path, "%s/cache/" TITLE "/3/c.bin", root);
    CHECK(host_file(path));
    /* BCAT in both: the game stops, and keeps both */
    test_fatal_text[0] = 0;
    test_fatal_jump = &jump;
    if (!setjmp(jump)) {
        mount_save_rc(2, NULL, 0, &save);
        CHECK(!"two BCAT saves did not stop the game");
    }
    test_fatal_jump = NULL;
    CHECK(strstr(test_fatal_text, "Two saves of this game's bcat save") && tree_has("save/bcat/p.bin"));
    snprintf(path, sizeof path, "%s/bcat/" TITLE "/b.bin", root);
    CHECK(host_file(path));
    /* temporary storage is the game's */
    save = mount_save(4);
    close_fs(save);
    CHECK(tree_has("save/temp"));
    /* save_layout=game: the game's own, and a save only the global folder holds played there, said */
    strcpy(g_settings.save_layout, "game");
    test_fs_text[0] = 0;
    save = mount_save(3);
    root_op(save, ENTRY_TYPE, "/d.bin", 0, 0);
    close_fs(save);
    CHECK(strstr(test_fs_text, "save_layout=game keeps in") && strstr(test_fs_text, "save/device"));
    /* a program with no title ID of its own: every save its own folder's */
    strcpy(g_settings.save_layout, "global");
    g_hle.kernel.title_id = SNX_NPDM_DEFAULT_TITLE;
    save = mount_save(5);
    root_op(save, CREATE_FILE, "/homebrew.bin", 4, 0);
    close_fs(save);
    CHECK(tree_has("save/cache/homebrew.bin"));
    g_hle.kernel.title_id = 0x0100000000010000ull;
    /* a collection's program 2: its saves are its application's, the owner its control.nacp names (SaveDataOwnerId),
       the folder program 0 plays, not one of its own ID */
    {
        static u8 nacp[0x4000];
        snx_st_le64(nacp + 0x3078, 0x0100000000010000ull);
        pm_lock(&g_hle.fs.tree.lock);
        fs_tree_forget(&g_hle.fs.tree, "control.nacp");
        CHECK(!fs_tree_add_virtual(&g_hle.fs.tree, "control.nacp", nacp, sizeof nacp, NULL));
        pm_unlock(&g_hle.fs.tree.lock);
        g_program = 2;
        g_hle.kernel.title_id = 0x0100000000010002ull;
        pm_lock(&g_hle.fs.tree.lock); /* as a new process's: none of program 0's folders grafted */
        fs_tree_forget(&g_hle.fs.tree, "global-user.0123456789abcdeffedcba9876543210");
        pm_unlock(&g_hle.fs.tree.lock);
        test_scans_allowed = 1;
        fs_saves_init(root);
        test_scans_allowed = 0;
        CHECK(g_host.fs.save_title == 0x0100000000010000ull);
        /* the application's folder grafted, the one program 0 wrote kept.bin into */
        CHECK(tree_has("global-user.0123456789abcdeffedcba9876543210/kept.bin"));
        save = mount_user_save(g_user_id);
        root_op(save, CREATE_FILE, "/program2.bin", 4, 0);
        close_fs(save);
        snprintf(path, sizeof path, "%s/user.0123456789abcdeffedcba9876543210/" TITLE "/program2.bin", root);
        CHECK(host_file(path));
        snprintf(path, sizeof path, "%s/user.0123456789abcdeffedcba9876543210/0100000000010002", root);
        CHECK(!snx_is_dir(path));
        /* its control.nacp naming no owner: its game's, the folder's name */
        memset(nacp + 0x3078, 0, 8);
        pm_lock(&g_hle.fs.tree.lock);
        fs_tree_forget(&g_hle.fs.tree, "control.nacp");
        CHECK(!fs_tree_add_virtual(&g_hle.fs.tree, "control.nacp", nacp, sizeof nacp, NULL));
        pm_unlock(&g_hle.fs.tree.lock);
        char was[sizeof g_gamename];
        snprintf(was, sizeof was, "%s", g_gamename);
        snprintf(g_gamename, sizeof g_gamename, "%s", "0100000000010000");
        test_scans_allowed = 1;
        fs_saves_init(root);
        test_scans_allowed = 0;
        CHECK(g_host.fs.save_title == 0x0100000000010000ull);
        pm_lock(&g_hle.fs.tree.lock);
        fs_tree_forget(&g_hle.fs.tree, "control.nacp");
        pm_unlock(&g_hle.fs.tree.lock);
        snprintf(g_gamename, sizeof g_gamename, "%s", was);
        g_program = 0;
        g_hle.kernel.title_id = 0x0100000000010000ull;
    }
    /* fs_user_saves_take, DeleteUser's: the folders of a user's saves, out of the index and their host paths */
    {
        char paths[2][FS_TREE_PATH_MAX];
        int n = fs_user_saves_take(g_user_id, paths, 2);
        CHECK(n == 1 && strstr(paths[0], "layout-saves/user.0123456789abcdeffedcba9876543210") &&
              !tree_has("global-user.0123456789abcdeffedcba9876543210"));
        n = fs_user_saves_take(test_other_user, paths, 2);
        CHECK(n == 1 && strstr(paths[0], OTHER_SAVE) && !tree_has(OTHER_SAVE) && snx_is_dir(paths[0]));
    }
    g_user_count = 0;
    g_settings.save_layout[0] = 0;
    g_host.fs.save_root[0] = 0;
    free(root);
#undef TITLE
    puts("PASS save_layout: global's folders, game's, a save played where it is, two of one refused");
}

/* the program the run is, as main.c's addon_init leaves its names: the game's own (update 0), or its update of
   version, whose program is named <game>_<update>v<version>; display its control data's display version */
static void test_program(u64 update, unsigned long version, const char* display) {
    strcpy(g_gamename, "0100000000010000");
    if (update)
        snprintf(g_exename, sizeof g_exename, "%s_%016llXv%lu", g_gamename, (unsigned long long)update, version);
    else
        strcpy(g_exename, g_gamename);
    g_addon = update;
    snprintf(g_addonrec, sizeof g_addonrec, "%s", update ? g_exename : "");
    snprintf(test_display, sizeof test_display, "%s", display);
}
/* the host folder of a save's file system */
static void save_host_folder(TFs save, char* out, size_t size) {
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_path(&g_hle.fs.tree, root_of(save), out, size));
    pm_unlock(&g_hle.fs.tree.lock);
}
/* the file at path is text */
static int host_text(const char* path, const char* text) {
    size_t size;
    char* had = (char*)snx_read_file(path, &size);
    int same = had && size == strlen(text) && !memcmp(had, text, size);
    if (had && !same) fprintf(stderr, "%s holds:\n%s", path, had);
    free(had);
    return same;
}
/* path, written with text */
static void host_write(const char* path, const char* text) { CHECK(!snx_write_file(path, text, strlen(text))); }
/* A save's note (fs.c's save_note): the program that committed the save last, in <its folder>.version beside it, out
   of the game's reach. A save opened whose note names a newer version of the game says both versions, once, and the
   crash reports say it again; an older, the same version or another game's says nothing. Versions compare by their
   content meta's when both are known, else by the numbers of their display versions. An Account, Device and BCAT save
   have one, temporary and cache storage none, and nor does a program with no title ID of its own; save_layout=global's
   is beside the title's folder in user.<uid>/ */
static void test_save_note(void) {
    static const u64 update = 0x0100000000010800ull, third[2] = {0x0505050505050505ull, 0x0505050505050505ull},
                     fourth[2] = {0x0606060606060606ull, 0x0606060606060606ull};
    char folder[FS_TREE_PATH_MAX], note[FS_TREE_PATH_MAX + 16], other[FS_TREE_PATH_MAX + 16], *root;
    size_t n;
    TFs save;
    strcpy(g_settings.save_layout, "game");
    g_host.fs.save_newer[0] = 0;
    /* the update 2.1.0, content meta version 983040: a write alone notes nothing, its commit does */
    test_program(update, 983040, "2.1.0");
    save = mount_user_save(g_user_id);
    save_host_folder(save, folder, sizeof folder);
    n = strlen(folder);
    CHECK(n > 0 && strstr(folder, FIRST_SAVE) && folder[n - 1] != '/');
    snprintf(note, sizeof note, "%s.version", folder);
    root_op(save, CREATE_FILE, "/progress.bin", 4, 0);
    CHECK(!host_file(note));
    commit(save);
    CHECK(host_text(note, "program=0100000000010800\nversion=983040\ndisplay=2.1.0\n"));
    snprintf(other, sizeof other, "%s/.version", folder);
    CHECK(!host_file(other));
    root_op(save, ENTRY_TYPE, "/progress.bin", 0, 0); /* the save's own files are the game's alone */
    close_fs(save);
    /* the game's own program 1.0.0 opens it: said with both versions, once however often it opens it */
    test_program(0, 0, "1.0.0");
    test_fs_text[0] = 0;
    save = mount_user_save(g_user_id);
    close_fs(save);
    CHECK(strstr(test_fs_text, folder) &&
          strstr(test_fs_text, ", this game's user save, was last written by version 2.1.0 (983040, program "
                               "0100000000010800); this program is 1.0.0 (program 0100000000010000): a game may not "
                               "read a save a newer version of it wrote\n"));
    test_fs_text[0] = 0;
    save = mount_user_save(g_user_id);
    CHECK(!test_fs_text[0]);
    /* a crash report says it again */
    test_report_text[0] = 0;
    fs_save_report();
    CHECK(!strncmp(test_report_text, "  ", 2) && strstr(test_report_text, folder) &&
          strstr(test_report_text, "last written by version 2.1.0 (983040"));
    /* its commit: the note names it from then on, its version not known (its program extracted, no dump read) */
    commit(save);
    close_fs(save);
    CHECK(host_text(note, "program=0100000000010000\ndisplay=1.0.0\n"));
    /* the update again: an older version's note, nothing said */
    test_program(update, 983040, "2.1.0");
    g_host.fs.save_newer[0] = 0;
    test_fs_text[0] = 0;
    save = mount_user_save(g_user_id);
    commit(save);
    close_fs(save);
    CHECK(!test_fs_text[0] && !g_host.fs.save_newer[0]);
    /* an older update, version 65536: the content meta versions tell, whatever the display versions say */
    test_program(update, 65536, "9.9.9");
    save = mount_user_save(g_user_id);
    close_fs(save);
    CHECK(strstr(test_fs_text, "last written by version 2.1.0 (983040, program 0100000000010800); this program is "
                               "9.9.9 (65536, program 0100000000010800):"));
    /* the same version: nothing */
    test_program(update, 983040, "2.1.0");
    g_host.fs.save_newer[0] = 0;
    test_fs_text[0] = 0;
    save = mount_user_save(g_user_id);
    close_fs(save);
    CHECK(!test_fs_text[0] && !g_host.fs.save_newer[0]);
    /* by the numbers of the display versions when a content meta version is not known: 1.10.0 after 1.9.2 */
    test_program(0, 0, "1.9.2");
    host_write(note, "program=0100000000010000\ndisplay=1.10.0\n");
    save = mount_user_save(g_user_id);
    close_fs(save);
    CHECK(strstr(test_fs_text, "last written by version 1.10.0 (program 0100000000010000); this program is 1.9.2 "
                               "(program 0100000000010000):"));
    test_program(0, 0, "1.10.0");
    host_write(note, "program=0100000000010000\ndisplay=1.9.2\n");
    g_host.fs.save_newer[0] = 0;
    test_fs_text[0] = 0;
    save = mount_user_save(g_user_id);
    close_fs(save);
    CHECK(!test_fs_text[0] && !g_host.fs.save_newer[0]);
    /* another game's note: not compared, newer as its version is */
    test_program(update, 983040, "2.1.0");
    host_write(note, "program=0100000000020000\nversion=99999999\ndisplay=99.0.0\n");
    save = mount_user_save(g_user_id);
    close_fs(save);
    CHECK(!test_fs_text[0] && !g_host.fs.save_newer[0]);
    /* the Device and BCAT saves have one; temporary and cache storage none */
    for (u8 type = 2; type <= 5; type++) {
        save = mount_save(type);
        save_host_folder(save, folder, sizeof folder);
        commit(save);
        close_fs(save);
        snprintf(note, sizeof note, "%s.version", folder);
        CHECK(type <= 3 ? host_text(note, "program=0100000000010800\nversion=983040\ndisplay=2.1.0\n")
                        : !host_file(note));
    }
    /* a program with no title ID of its own: none */
    g_hle.kernel.title_id = SNX_NPDM_DEFAULT_TITLE;
    save = mount_user_save(fourth); /* test_save_layout took the other user's saves out of the index */
    save_host_folder(save, folder, sizeof folder);
    commit(save);
    close_fs(save);
    snprintf(note, sizeof note, "%s.version", folder);
    CHECK(!host_file(note));
    g_hle.kernel.title_id = 0x0100000000010000ull;
    /* save_layout=global: beside the title's folder in user.<uid>/ */
    root = snx_absolute("../../note-saves");
    CHECK(root != NULL);
    strcpy(g_settings.save_layout, "global");
    fs_saves_init(root);
    test_program(update, 983040, "2.1.0");
    save = mount_user_save(third);
    root_op(save, CREATE_FILE, "/g.bin", 4, 0);
    commit(save);
    close_fs(save);
    snprintf(note, sizeof note, "%s/user.05050505050505050505050505050505/0100000000010000.version", root);
    CHECK(host_text(note, "program=0100000000010800\nversion=983040\ndisplay=2.1.0\n"));
    g_settings.save_layout[0] = 0;
    g_host.fs.save_root[0] = 0;
    g_host.fs.save_newer[0] = 0;
    test_program(0, 0, "1.0.0");
    g_gamename[0] = g_exename[0] = 0;
    free(root);
    puts("PASS a save's note: written on commit beside its folder, a newer version's said once and in a crash report, "
         "an older, the same or another game's not, by the content meta's versions else the display versions'");
}

/* fs_sd_write_new, a file the system writes on the SD card (caps' album): its folders made, written through the index,
   so the game's SD card file system finds it and its data; a second of the same name refused, the first left */
static void test_sd_write(void) {
    static const char data[] = "a picture";
    CHECK(fs_sd_write_new("Nintendo/Album/2026/11/07", "x.jpg", data, sizeof data) == FS_TREE_OK);
    CHECK(fs_sd_write_new("Nintendo/Album/2026/11/07", "x.jpg", "other", 6) == FS_TREE_EXISTS);
    CHECK(tree_has("sdmc/Nintendo/Album/2026/11/07/x.jpg"));
    TFs sd = mount_sd();
    TFile file = open_file(sd, "/Nintendo/Album/2026/11/07/x.jpg", 1);
    CHECK(get_size(file) == sizeof data);
    CHECK(read_data(file, 0, sizeof data, 0) == sizeof data && !memcmp(GP(0x5000), data, sizeof data));
    close_file(file);
    close_fs(sd);
    puts("PASS a file written on the SD card: its folders made, found through the index, a second of its name refused");
}

static void cleanup_service(void) {
    pm_lock(&g_hle.fs.tree.lock);
    for (unsigned i = 0; i < NOPEN; i++)
        if (g_hle.fs.openf[i].f) fclose(g_hle.fs.openf[i].f);
    if (g_hle.fs.rraw) fclose(g_hle.fs.rraw);
    for (int i = 0; i < g_hle.fs.nrfiles; i++)
        if (g_hle.fs.rfiles[i].node) fs_tree_release(g_hle.fs.rfiles[i].node);
    snx_free_strings(g_hle.fs.mexefs_names, g_hle.fs.nmexefs);
    snx_free_strings(g_hle.fs.mexefs_paths, g_hle.fs.nmexefs);
    free(g_hle.fs.rmeta);
    free(g_hle.fs.rfiles);
    fs_tree_destroy(&g_hle.fs.tree);
    pm_unlock(&g_hle.fs.tree.lock);
    CHECK(test_shared == 0);
}


/* rhash against the RomFS format's definition, worked by hand: the seed is the parent's offset xor 123456789
   (0x075BCD15), each byte rotates right by 5 then xors in. "a": 0x075BCD15 ror 5 = 0xA83ADE68, xor 0x61 = 0xA83ADE09.
   A byte past 0x7F counts unsigned */
static void test_rhash(void) {
    CHECK(rhash(0, "") == 0x075BCD15u);
    CHECK(rhash(0, "a") == 0xA83ADE09u);
    CHECK(rhash(0x18, "Data") == 0x0418D017u);
    CHECK(rhash(0, "\xC3\xA9") == 0x5D41D65Cu);
}
int main(int argc, char** argv) {
    hle_init();
    test_rhash();
    test_read_pacing();
    g_hle.kernel.title_id = 0x0100000000010000ull; /* the title the test was written for */
    CHECK((argc == 3 || argc == 4) && strstr(argv[1], "scarletnx-fs-tree-") != NULL);
    CHECK(!test_chdir(argv[1]));
    FILE* marker = fopen(".snx-fs-test", "rb");
    char token[64] = {0};
    CHECK(marker != NULL);
    CHECK(fread(token, 1, sizeof token - 1, marker) == strlen(argv[2]));
    CHECK(!fclose(marker) && !strcmp(token, argv[2]));
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    int raw = argc == 4 && !strcmp(argv[3], "raw");
#if !defined(_WIN32)
    if (argc == 4 && (!strcmp(argv[3], "dump") || !strcmp(argv[3], "dump-source"))) {
        int source = !strcmp(argv[3], "dump-source");
        dump_write(source);
        if (source) strcpy(g_settings.game_dump, "dump-data/Game.nsp");
        strcpy(g_datadir, "dump-data/Game_data/");
        fs_init(NULL, NULL);
        test_scans_allowed = 0;
        svc_fs_init();
        romfs_init();
        test_dump();
        CHECK(handles_closed());
        cleanup_service();
        free(g_mem);
        printf("PASS %u dump checks; %u startup scans\n", test_checks, test_scans);
        return 0;
    }
    if (argc == 4 && (!strcmp(argv[3], "dump-update") || !strcmp(argv[3], "dump-update-extracted") ||
                      !strcmp(argv[3], "dump-dlc"))) {
        int update = strncmp(argv[3], "dump-update", 11) == 0;
        dump_write(1);
        addon_write(update, !strcmp(argv[3], "dump-update-extracted"));
        strcpy(g_settings.game_dump, "dump-data/Game.nsp"); /* the game's .ini's, which the add-on runs with */
        strcpy(g_datadir, "dump-data/Game_data/");
        fs_init(NULL, NULL);
        test_scans_allowed = 0;
        svc_fs_init();
        romfs_init();
        test_addon(update);
        CHECK(handles_closed());
        cleanup_service();
        free(g_mem);
        printf("PASS %u add-on checks\n", test_checks);
        return 0;
    }
    /* a dump whose program NCA needs master key 20 and a keys file that goes up to 19: the game stops before reading
       it, naming both and the keys file, where it would read garbage and miss its modules */
    if (argc == 4 && !strcmp(argv[3], "dump-keys")) {
        jmp_buf jump;
        char *keys, *named;
        dump_write_nsp(0, DUMP_SEALED);
        keys = snx_absolute("dump-data/prod.keys");
        named =
            snx_format("and %s goes up to master key 19 (firmware 20.0.0 and later): it needs the keys of a console "
                       "with master key 20, on its firmware or a later one",
                       keys);
        strcpy(g_datadir, "dump-data/Game_data/");
        snx_error_sink(test_error_line);
        test_fatal_text[0] = 0;
        test_fatal_jump = &jump;
        if (!setjmp(jump)) {
            fs_init("dump-data/prod.keys", NULL);
            CHECK(!"a dump its keys can't read did not stop the game");
        }
        test_fatal_jump = NULL;
        snx_error_sink(NULL);
        if (!strstr(test_error_text, named)) fprintf(stderr, "said: %s\nfatal: %s\n", test_error_text, test_fatal_text);
        CHECK(!strcmp(
            test_fatal_text,
            "A dump the game reads in place needs keys its keys file does not have: the line before says which"));
        CHECK(strstr(test_error_text, "The game's Program NCA is encrypted with master key 20 (a firmware newer than "
                                      "20.0.0), and ") == test_error_text);
        CHECK(strstr(test_error_text, named) != NULL);
        CHECK(g_hle.fs.dfs == NULL); /* nothing read out of it */
        free(named);
        free(keys);
        free(g_mem);
        printf("PASS %u checks: a dump its keys file stops short of names the master key it needs and the file\n",
               test_checks);
        return 0; /* the fatal left the tree locked: nothing more of it */
    }
    /* a dump whose file is cut short of its table: the game stops at start, naming both sizes, where the dump could
       not be mapped */
    if (argc == 4 && !strcmp(argv[3], "dump-cut")) {
        jmp_buf jump;
        char has[32], needs[32], *said;
        dump_write_nsp(1, DUMP_CUT_SHORT);
        strcpy(g_settings.game_dump, "dump-data/Game.nsp");
        strcpy(g_datadir, "dump-data/Game_data/");
        snx_size_text(dump_table_end - 1024, has, sizeof has);
        snx_size_text(dump_table_end, needs, sizeof needs);
        said = snx_format("dump-data/Game.nsp is %s (%llu bytes), and its table puts its files up to %s (%llu bytes): "
                          "the dump is incomplete, 1024 bytes short. Copy or dump it again",
                          has, (unsigned long long)(dump_table_end - 1024), needs, (unsigned long long)dump_table_end);
        snx_error_sink(test_error_line);
        test_fatal_jump = &jump;
        if (!setjmp(jump)) {
            fs_init(NULL, NULL);
            CHECK(!"a dump cut short did not stop the game");
        }
        test_fatal_jump = NULL;
        snx_error_sink(NULL);
        if (strcmp(test_error_text, said)) fprintf(stderr, "said:     %s\nexpected: %s\n", test_error_text, said);
        CHECK(!strcmp(test_error_text, said));
        CHECK(strstr(test_fatal_text, "the line before says why") != NULL && g_hle.fs.dfs == NULL);
        free(said);
        free(g_mem);
        printf("PASS %u checks: a dump cut short says so at start, with its size and its table's\n", test_checks);
        return 0; /* the fatal left the tree locked: nothing more of it */
    }
    /* a dump whose RomFS can't be read to its end (its compression table lists 0x20 bytes past what it stores): said at
       start with the image's size and the dump's, and still where the game reads there, each time with hactool's
       reason, the entry and the stored bytes it reads past the stored data (hac_synth.h's compressed_build: one NONE
       entry, its data cut to a multiple of 0x10). One whose image is far bigger than the dump, all of it there (zeros
       past its files), is said nothing of */
    if (argc == 4 && (!strcmp(argv[3], "dump-romfs-cut") || !strcmp(argv[3], "dump-romfs-zeros"))) {
        int cut = !strcmp(argv[3], "dump-romfs-cut");
        char image[32], mapped[32], *said;
        unsigned long long stored;
        dump_write_nsp(0, cut ? DUMP_ROMFS_CUT : DUMP_ROMFS_ZEROS);
        strcpy(g_datadir, "dump-data/Game_data/");
        fs_init(NULL, NULL);
        test_scans_allowed = 0;
        svc_fs_init();
        romfs_init();
        CHECK(g_hle.fs.dfs != NULL && g_hle.fs.rtotal > 0x50);
        snx_size_text(g_hle.fs.rtotal, image, sizeof image);
        snx_size_text(g_hle.fs.dmap.size, mapped, sizeof mapped);
        stored = (unsigned long long)(g_hle.fs.rtotal - 0x20 + 0xF) & ~0xFULL;
        said = snx_format("RomFS: the dump's image is %s, and its last byte, at %llx, can't be read from the dump's %s "
                          "(hactool: Compressed storage entry 0 (NONE, image 0x0+0x%llx, stored at 0x0) reads stored "
                          "bytes 0x%llx+0x1, past the stored data's 0x%llx!): the dump is incomplete or damaged\n",
                          image, (unsigned long long)(g_hle.fs.rtotal - 1), mapped, (unsigned long long)g_hle.fs.rtotal,
                          (unsigned long long)(g_hle.fs.rtotal - 1), stored);
        if (cut && strcmp(test_romfs_text, said)) fprintf(stderr, "said:     %sexpected: %s", test_romfs_text, said);
        CHECK(cut ? !strcmp(test_romfs_text, said)
                  : !test_romfs_text[0] && g_hle.fs.rtotal > snx_file_size("dump-data/Game.nsp"));
        /* the image's last 0x10 bytes as the game reads them: the read's own warning, or zeros */
        TStorage storage = open_romfs();
        test_romfs_text[0] = 0;
        CHECK(tstorage_read_at(&storage, g_hle.fs.rtotal - 0x10, 0x5000, 0x10) == (cut ? -1 : 0));
        free(said);
        said =
            snx_format("RomFS: could not read the dump at %llx (10 bytes; hactool: Compressed storage entry 0 (NONE, "
                       "image 0x0+0x%llx, stored at 0x0) reads stored bytes 0x%llx+0x10, past the stored data's "
                       "0x%llx!): the read fails, its buffer zeroed\n",
                       (unsigned long long)(g_hle.fs.rtotal - 0x10), (unsigned long long)g_hle.fs.rtotal,
                       (unsigned long long)(g_hle.fs.rtotal - 0x10), stored);
        if (cut && strcmp(test_romfs_text, said)) fprintf(stderr, "said:     %sexpected: %s", test_romfs_text, said);
        CHECK(cut ? !strcmp(test_romfs_text, said) : !test_romfs_text[0]);
        free(said);
        tstorage_close(&storage);
        CHECK(handles_closed());
        cleanup_service();
        free(g_mem);
        printf("PASS %u checks: a dump's RomFS %s\n", test_checks,
               cut ? "that can't be read to its end is said at start" : "bigger than the dump, all there, is not");
        return 0;
    }
    /* a data folder whose sdmc/ is a symbolic link to a folder: the game plays with that folder's files as its card's,
       a file made in sdmc/ is made there, and removing the game's data folder takes the link and leaves the folder. A
       link below the top (one in that folder) stops the game at start, the line before naming the link's path and why,
       the stop's own text free of paths */
    if (argc == 4 && !strcmp(argv[3], "data-link")) {
        jmp_buf jump;
        char *real, *said;
        CHECK(!snx_mkdirs("link-data/Game_data/exefs") && !snx_mkdirs("link-data/card"));
        CHECK(!snx_write_file("link-data/Game_data/exefs/main", "main", 4) &&
              !snx_write_file("link-data/card/a.bin", "card", 4));
        CHECK(!snx_symlink("../card", "link-data/Game_data/sdmc") && !snx_symlink("a.bin", "link-data/card/deep"));
        real = realpath("link-data/card", NULL);
        CHECK(real != NULL);
        said = snx_format("fs: could not index the game's files in link-data/Game_data/: %s/deep is a symbolic link, "
                          "which the index follows only as a folder at the top of the game's data folder\n",
                          real);
        strcpy(g_datadir, "link-data/Game_data/");
        test_fs_text[0] = test_fatal_text[0] = 0;
        test_fatal_jump = &jump;
        if (!setjmp(jump)) {
            fs_init(NULL, NULL);
            CHECK(!"a link below the data folder's top did not stop the game");
        }
        test_fatal_jump = NULL;
        if (strcmp(test_fs_text, said)) fprintf(stderr, "said:     %sexpected: %s", test_fs_text, said);
        CHECK(!strcmp(test_fs_text, said));
        CHECK(!strcmp(test_fatal_text,
                      "Could not index the game's files, as one is a symbolic link, which the index follows only as a "
                      "folder at the top of the game's data folder: the line before says which"));
        CHECK(snx_is_file("link-data/card/a.bin") && g_hle.fs.tree.root == NULL);
        free(said);
        CHECK(!remove("link-data/card/deep"));
        strcpy(g_datadir, "link-data/Game_data/");
        fs_init(NULL, NULL);
        test_scans_allowed = 0;
        CHECK(tree_has("sdmc/a.bin") && !tree_has("sdmc/deep"));
        pm_lock(&g_hle.fs.tree.lock);
        CHECK(fs_tree_create(&g_hle.fs.tree, NULL, "sdmc/b.bin", 0, 3, NULL) == FS_TREE_OK);
        pm_unlock(&g_hle.fs.tree.lock);
        CHECK(snx_is_file("link-data/card/b.bin"));
        cleanup_service();
        CHECK(!snx_remove_tree(g_datadir) && !snx_path_exists("link-data/Game_data"));
        CHECK(snx_is_dir(real) && snx_is_file("link-data/card/a.bin") && snx_is_file("link-data/card/b.bin"));
        free(real);
        free(g_mem);
        printf("PASS %u checks: a linked sdmc/ is the game's card, and stays when the game's folder goes\n",
               test_checks);
        return 0;
    }
    /* a game read from its dump that finds none: where it looked, its .ini and the link an older version made */
    if (argc == 4 && (!strcmp(argv[3], "dump-none") || !strcmp(argv[3], "dump-gone"))) {
        int gone = !strcmp(argv[3], "dump-gone");
        char *dir, *ini, *said;
        CHECK(!snx_mkdirs("dump-data/Game_data"));
        dir = snx_absolute("dump-data"); /* fs_init's data folder is a full path, and so is the .ini said */
        ini = snx_absolute(ini_file());
        said = gone ? snx_format("fs: the game's dump dump-data/Gone.nsp, its source= setting, is not there; its "
                                 "settings are %s\n",
                                 ini)
                    : snx_format("fs: the game has no ExeFS in %s/Game_data/exefs/ and no dump to read it from: "
                                 "%s sets no source=, and %s/Game.source is not there\n",
                                 dir, ini, dir);
        CHECK(strcmp(ini, ini_file()) != 0);
        if (gone) strcpy(g_settings.game_dump, "dump-data/Gone.nsp");
        strcpy(g_datadir, "dump-data/Game_data/");
        fs_init(NULL, NULL);
        if (strcmp(test_fs_text, said)) fprintf(stderr, "said:     %sexpected: %s", test_fs_text, said);
        CHECK(!strcmp(test_fs_text, said) && g_hle.fs.dfs == NULL);
        free(dir);
        free(ini);
        free(said);
        cleanup_service();
        free(g_mem);
        printf("PASS %u checks: the game without a dump says where it looked\n", test_checks);
        return 0;
    }
#endif
    /* source= an extracted game's folder, outside the data folder: its ExeFS and RomFS read where they are, read-only;
       one that is not there is said to be a folder, with the settings that name it */
    if (argc == 4 && (!strcmp(argv[3], "folder-source") || !strcmp(argv[3], "folder-gone"))) {
        int gone = !strcmp(argv[3], "folder-gone");
        char *source, *ini = snx_absolute(ini_file()), *said = NULL;
        u64 size = 0;
        u8* bytes;
        FsNode* node;
        CHECK(!snx_mkdirs("folder-data/Game_data") && !snx_mkdirs("Extracted game/exefs") &&
              !snx_mkdirs("Extracted game/romfs/sub"));
        CHECK(!snx_write_file("Extracted game/exefs/main", "main", 4) &&
              !snx_write_file("Extracted game/romfs/asset.bin", "romfs test", 10) &&
              !snx_write_file("Extracted game/romfs/sub/deep.bin", "deep", 4));
        source = snx_absolute(gone ? "Moved game" : "Extracted game");
        CHECK(source && ini && strlen(source) < sizeof g_settings.game_dump);
        strcpy(g_settings.game_dump, source);
        strcpy(g_datadir, "folder-data/Game_data/");
        test_fs_text[0] = 0;
        fs_init(NULL, NULL);
        if (gone) {
            said = snx_format("fs: the game's folder %s, its source= setting, is not there; its settings are %s\n",
                              source, ini);
            if (strcmp(test_fs_text, said)) fprintf(stderr, "said:     %sexpected: %s", test_fs_text, said);
            CHECK(!strcmp(test_fs_text, said) && fs_read_file("romfs/asset.bin", &size) == NULL);
        } else {
            bytes = fs_read_file("exefs/main", &size);
            CHECK(bytes && size == 4 && !memcmp(bytes, "main", 4));
            free(bytes);
            bytes = fs_read_file("romfs/sub/deep.bin", &size);
            CHECK(bytes && size == 4 && !memcmp(bytes, "deep", 4));
            free(bytes);
            pm_lock(&g_hle.fs.tree.lock);
            CHECK(!fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs/asset.bin", &node) && node->readonly);
            CHECK(fs_tree_remove(&g_hle.fs.tree, NULL, "romfs/asset.bin", 0, 0, 0) == FS_TREE_READONLY);
            pm_unlock(&g_hle.fs.tree.lock);
            CHECK(snx_is_file("Extracted game/romfs/asset.bin") && !snx_path_exists("folder-data/Game_data/romfs"));
        }
        free(source);
        free(ini);
        free(said);
        cleanup_service();
        free(g_mem);
        printf("PASS %u checks: a game read from %s\n", test_checks,
               gone ? "a folder that is not there says so" : "its extracted folder in place");
        return 0;
    }
    if (argc == 4 && (!strcmp(argv[3], "mods") || !strcmp(argv[3], "mods-image") || !strcmp(argv[3], "mods-stale"))) {
        int image = !strcmp(argv[3], "mods-image"), stale = !strcmp(argv[3], "mods-stale");
        mods_write(image, stale);
        fs_init(NULL, NULL);
        test_scans_allowed = 0;
        svc_fs_init();
        romfs_init();
        test_mods(image, stale);
        CHECK(handles_closed());
        cleanup_service();
        free(g_mem);
        printf("PASS %u mod checks\n", test_checks);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[3], "addon-overlay")) {
        test_overlay();
        cleanup_service();
        free(g_mem);
        printf("PASS %u overlay checks\n", test_checks);
        return 0;
    }
    strcpy(g_datadir, raw ? "raw-data/" : "service-data/");
    fs_init(NULL, NULL);
    CHECK(test_scans > 0);
    /* the temporary storage the fixture left is gone, from the index and the disk: each launch starts it empty */
    FILE* left = fopen("service-data/save/temp/last-launch.bin", "rb");
    CHECK(raw || (!left && !tree_has("save/temp")));
    if (left) fclose(left);
    test_scans_allowed = 0;
    svc_fs_init();
    romfs_init();
    CHECK(!test_chdir(raw ? "raw-data" : "service-data/romfs"));
    if (raw) {
        test_raw_romfs();
        CHECK(handles_closed());
        cleanup_service();
        free(g_mem);
        CHECK(!test_chdir(argv[1]));
        printf("PASS %u raw RomFS checks; %u startup scans\n", test_checks, test_scans);
        return 0;
    }
    TFs fs = mount_sd();
    test_file_services(fs);
    test_file_contracts(fs);
    test_directory_path_contracts(fs);
    test_directory_services(fs);
    test_index_and_paths(fs);
    test_virtual_services(fs);
    test_extracted_romfs();
    test_save_data();
    test_save_layout();
    test_save_note();
    test_sd_write();
    test_state_temp();
    close_fs(fs);
    CHECK(handles_closed());
    cleanup_service();
    free(g_mem);
    CHECK(!test_chdir(argv[1]));
    printf("PASS %u HLE checks; %u startup scans, none after initialization\n", test_checks, test_scans);
    return 0;
}
