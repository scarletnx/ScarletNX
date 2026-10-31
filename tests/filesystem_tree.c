/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic integration tests. The Python runner creates and removes the sandbox. */
#include "nx/hle/filesystem/fs_tree.h"
#include <stdlib.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define host_chdir _chdir
#define host_rmdir _rmdir
#else
#include <dirent.h>
#include <unistd.h>
#define host_chdir chdir
#define host_rmdir rmdir
#endif

static unsigned checks, scan_count;
static int scans_allowed = 1;
static FsTree tree;
static char original_base[FS_TREE_PATH_MAX];

static void fail(const char* expression, int line) {
    fprintf(stderr, "filesystem_tree.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) fail(#expr, __LINE__);                                                                            \
    } while (0)
#define OK(expr) CHECK((expr) == FS_TREE_OK)

static void scanned(void) {
    if (!scans_allowed) fail("directory enumeration occurred after initialization", __LINE__);
    scan_count++;
}

#if defined(_WIN32)
HANDLE WINAPI fs_test_FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data);
HANDLE WINAPI fs_test_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data);
HANDLE WINAPI fs_test_FindFirstFileA(LPCSTR path, LPWIN32_FIND_DATAA data) {
    scanned();
    return FindFirstFileA(path, data);
}
HANDLE WINAPI fs_test_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    scanned();
    return FindFirstFileW(path, data);
}
void pm_init(PMutex* m) { InitializeSRWLock(&m->l); }
void pm_lock(PMutex* m) { AcquireSRWLockExclusive(&m->l); }
void pm_unlock(PMutex* m) { ReleaseSRWLockExclusive(&m->l); }
int plat_mkdir(const char* path) { return CreateDirectoryA(path, NULL) ? 0 : -1; }
#else
DIR* fs_test_opendir(const char* path);
DIR* fs_test_opendir(const char* path) {
    scanned();
    return opendir(path);
}
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
int plat_mkdir(const char* path) { return mkdir(path, 0777); }
#endif
/* the tree suite shares files in test_share_limits alone: a descriptor of its own count for each, nothing mapped */
static int sharing, shares;
int plat_file_share(const char* path, u64 size, int* fd, const u8** data) {
    (void)path;
    (void)size;
    CHECK(sharing && "the tree suite shares files in test_share_limits alone");
    *fd = 1000 + shares++;
    *data = NULL;
    return 0;
}
void plat_file_unshare(int fd, const u8* data, u64 size) {
    (void)data;
    (void)size;
    CHECK(sharing && fd >= 1000);
}

static void host_path(const char* relative, char* out) {
    CHECK(snprintf(out, FS_TREE_PATH_MAX, "%s/%s", original_base, relative) < FS_TREE_PATH_MAX);
}

static int host_exists(const char* relative) {
    char path[FS_TREE_PATH_MAX];
    host_path(relative, path);
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return !stat(path, &st);
#endif
}

static void host_write(const char* relative, const char* content) {
    char path[FS_TREE_PATH_MAX];
    host_path(relative, path);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(content, 1, strlen(content), f) == strlen(content));
    CHECK(!fclose(f));
}

static void host_remove(const char* relative, int directory) {
    char path[FS_TREE_PATH_MAX];
    host_path(relative, path);
    CHECK(!(directory ? host_rmdir(path) : remove(path)));
}

static FsNode* lookup(FsNode* scope, const char* path) {
    FsNode* node = NULL;
    OK(fs_tree_lookup(&tree, scope, path, &node));
    CHECK(node != NULL && node->attached);
    return node;
}

static void absent(FsNode* scope, const char* path) {
    FsNode* node = NULL;
    CHECK(fs_tree_lookup(&tree, scope, path, &node) == FS_TREE_NOTFOUND);
    CHECK(node == NULL);
}

static void path_equals(FsNode* node, const char* relative) {
    char actual[FS_TREE_PATH_MAX], expected[FS_TREE_PATH_MAX];
    OK(fs_tree_path(&tree, node, actual, sizeof actual));
    host_path(relative, expected);
    for (char* p = actual; *p; p++)
        if (*p == '\\') *p = '/';
    for (char* p = expected; *p; p++)
        if (*p == '\\') *p = '/';
    CHECK(!strcmp(actual, expected));
}

static size_t children(FsNode* dir) {
    size_t count = 0;
    const char* previous = NULL;
    CHECK(dir->is_dir);
    for (FsNode* n = dir->children; n; n = n->next) {
        CHECK(n->parent == dir && n->attached);
        CHECK(!previous || strcmp(previous, n->name) < 0);
        previous = n->name;
        count++;
    }
    return count;
}

static void test_lookup(void) {
    FsNode* drive = lookup(NULL, "drive:/");
    FsNode* seed = lookup(NULL, "drive:/seed.bin");
    CHECK(drive->is_dir && !seed->is_dir && seed->size == 7);
    CHECK(lookup(NULL, "drive/seed.bin") == seed);
    CHECK(lookup(drive, "/seed.bin") == seed);
    CHECK(lookup(drive, "./folder/../seed.bin") == seed);
    CHECK(lookup(drive, "folder//sub/./../nested.bin")->size == 6);
    CHECK(lookup(NULL, "/root.txt") == lookup(NULL, "root/root.txt"));
    CHECK(lookup(NULL, "romfs.bin")->size == 3);
    path_equals(seed, "drive/seed.bin");
    path_equals(lookup(NULL, "/root.txt"), "root/root.txt");
    FsNode* out = NULL;
    CHECK(fs_tree_lookup(&tree, drive, "../romfs/asset.bin", &out) != FS_TREE_OK);
    CHECK(fs_tree_lookup(&tree, drive, "romfs:/asset.bin", &out) != FS_TREE_OK);
    CHECK(fs_tree_lookup(&tree, drive, "seed.bin/child", &out) != FS_TREE_OK);
    absent(drive, "missing.bin");
    char short_path[2];
    CHECK(fs_tree_path(&tree, seed, short_path, sizeof short_path) == FS_TREE_TOOLONG);
    char long_path[FS_TREE_PATH_MAX + 100];
    memset(long_path, 'x', sizeof long_path - 1);
    long_path[sizeof long_path - 1] = 0;
    CHECK(fs_tree_lookup(&tree, drive, long_path, &out) == FS_TREE_TOOLONG);
    puts("PASS startup scan, absolute base, mounts, normalization and scope boundaries");
}

static void test_mutations(void) {
    FsNode* drive = lookup(NULL, "drive:/");
    size_t before = children(drive);
    FsNode *dir = NULL, *file = NULL;
    OK(fs_tree_create(&tree, drive, "newdir", 1, 0, &dir));
    CHECK(children(drive) == before + 1 && host_exists("drive/newdir"));
    OK(fs_tree_create(&tree, dir, "file.bin", 0, 513, &file));
    CHECK(file->size == 513 && children(dir) == 1);
    char path[FS_TREE_PATH_MAX];
    OK(fs_tree_path(&tree, file, path, sizeof path));
    FILE* f = fopen(path, "rb");
    CHECK(f != NULL);
    CHECK(!fseek(f, 0, SEEK_END) && ftell(f) == 513);
    CHECK(!fseek(f, 0, SEEK_SET) && fgetc(f) == 0);
    CHECK(!fclose(f));
    CHECK(fs_tree_create(&tree, drive, "newdir", 1, 0, NULL) == FS_TREE_EXISTS);
    CHECK(fs_tree_create(&tree, drive, "missing/child", 0, 0, NULL) == FS_TREE_NOTFOUND);
    CHECK(!host_exists("drive/missing"));
    CHECK(fs_tree_create(&tree, file, "child", 0, 0, NULL) != FS_TREE_OK);
    CHECK(fs_tree_remove(&tree, dir, "file.bin", 1, 0, 0) != FS_TREE_OK);
    CHECK(fs_tree_remove(&tree, drive, "newdir", 0, 0, 0) != FS_TREE_OK);
    CHECK(fs_tree_remove(&tree, drive, "newdir", 1, 0, 0) == FS_TREE_NOTEMPTY);
    CHECK(lookup(dir, "file.bin") == file && host_exists("drive/newdir/file.bin"));

    fs_tree_retain(dir);
    fs_tree_retain(file);
    OK(fs_tree_rename(&tree, drive, "newdir", "renamed", 1));
    CHECK(lookup(drive, "renamed") == dir);
    CHECK(lookup(dir, "file.bin") == file);
    absent(drive, "newdir");
    CHECK(!host_exists("drive/newdir") && host_exists("drive/renamed/file.bin"));
    path_equals(file, "drive/renamed/file.bin");
    CHECK(fs_tree_rename(&tree, drive, "renamed", "renamed/inside", 1) != FS_TREE_OK);
    CHECK(fs_tree_rename(&tree, drive, "renamed/file.bin", "seed.bin", 0) == FS_TREE_EXISTS);
    CHECK(fs_tree_rename(&tree, drive, "renamed/file.bin", "moved.bin", 1) != FS_TREE_OK);
    OK(fs_tree_rename(&tree, drive, "renamed/file.bin", "moved.bin", 0));
    CHECK(lookup(drive, "moved.bin") == file && children(dir) == 0);
    path_equals(file, "drive/moved.bin");
    OK(fs_tree_remove(&tree, drive, "renamed", 1, 0, 0));
    CHECK(!dir->attached && dir->is_dir);
    CHECK(fs_tree_path(&tree, dir, path, sizeof path) == FS_TREE_NOTFOUND);
    CHECK(fs_tree_create(&tree, dir, "after-delete", 0, 0, NULL) != FS_TREE_OK);
    OK(fs_tree_remove(&tree, drive, "moved.bin", 0, 0, 0));
    CHECK(!file->attached && file->size == 513);
    absent(drive, "moved.bin");
    CHECK(!host_exists("drive/moved.bin") && children(drive) == before);
    fs_tree_release(file);
    fs_tree_release(dir);
    puts("PASS create, size, enumeration, typed mutation, rename and retained handles");
}

static void test_virtual(void) {
    const unsigned char expected[] = {0, 1, 0xff, 3, 0, 5};
    unsigned char content[sizeof expected];
    memcpy(content, expected, sizeof content);
    FsNode* file = NULL;
    OK(fs_tree_add_virtual(&tree, "system/archive/font.bin", content, sizeof content, &file));
    memset(content, 0xcc, sizeof content);
    CHECK(file->is_virtual && file->readonly && file->size == sizeof expected);
    CHECK(!memcmp(file->virtual_data, expected, sizeof expected));
    CHECK(lookup(NULL, "system:/archive/font.bin") == file);
    CHECK(!host_exists("system/archive/font.bin"));
    CHECK(fs_tree_remove(&tree, NULL, "system/archive/font.bin", 0, 0, 0) == FS_TREE_READONLY);
    CHECK(fs_tree_rename(&tree, NULL, "system/archive/font.bin", "system/archive/moved", 0) == FS_TREE_READONLY);
    CHECK(fs_tree_create(&tree, NULL, "system/archive/font.bin", 0, 0, NULL) == FS_TREE_EXISTS);
    CHECK(lookup(NULL, "system:/archive/font.bin") == file);
    OK(fs_tree_create(&tree, NULL, "system/archive/save.bin", 0, 9, NULL));
    CHECK(host_exists("system/archive/save.bin") && !host_exists("system/archive/font.bin"));
    CHECK(lookup(NULL, "system/archive/save.bin")->size == 9);
    CHECK(!memcmp(file->virtual_data, expected, sizeof expected));
    puts("PASS copied virtual content, readonly entries and materialized parent directories");
}

static void test_rename_path_limit(void) {
    char source[FS_TREE_PATH_MAX], destination[151];
    size_t target = FS_TREE_PATH_MAX - 6 - strlen(tree.base) - 1;
    strcpy(source, "drive/long/");
    size_t used = strlen(source);
    CHECK(target > used + 240);
    while (target - used > 240) {
        memset(source + used, 'x', 240);
        used += 240;
        source[used++] = '/';
    }
    memset(source + used, 'v', target - used);
    source[target] = 0;
    FsNode* file = NULL;
    OK(fs_tree_add_virtual(&tree, source, "x", 1, &file));
    FsNode* dir = lookup(NULL, "drive/long");
    CHECK(dir->is_virtual && !host_exists("drive/long"));
    memset(destination, 'y', sizeof destination - 1);
    destination[sizeof destination - 1] = 0;
    CHECK(fs_tree_rename(&tree, lookup(NULL, "drive:/"), "long", destination, 1) == FS_TREE_TOOLONG);
    CHECK(lookup(NULL, source) == file && dir->is_virtual);
    CHECK(!host_exists("drive/long"));
    absent(lookup(NULL, "drive:/"), destination);
    puts("PASS rename rejects an overlong descendant before materializing or moving anything");
}

static void test_no_rescan_and_failures(void) {
    FsNode* drive = lookup(NULL, "drive:/");
    size_t count = children(drive);
    host_write("drive/external.bin", "not indexed");
    absent(drive, "external.bin");
    CHECK(children(drive) == count);
    CHECK(fs_tree_remove(&tree, drive, "external.bin", 0, 0, 0) == FS_TREE_NOTFOUND);
    CHECK(fs_tree_rename(&tree, drive, "external.bin", "unexpected.bin", 0) == FS_TREE_NOTFOUND);
    CHECK(host_exists("drive/external.bin"));
    CHECK(fs_tree_create(&tree, drive, "external.bin", 0, 0, NULL) != FS_TREE_OK);
    absent(drive, "external.bin");
    char path[FS_TREE_PATH_MAX];
    host_path("drive/external.bin", path);
    FILE* f = fopen(path, "rb");
    CHECK(f != NULL && fgetc(f) == 'n');
    CHECK(!fclose(f));

    OK(fs_tree_create(&tree, drive, "gone", 1, 0, NULL));
    FsNode* gone = lookup(drive, "gone");
    host_remove("drive/gone", 1);
    CHECK(fs_tree_create(&tree, gone, "file.bin", 0, 0, NULL) != FS_TREE_OK);
    absent(gone, "file.bin");
    CHECK(lookup(drive, "gone") == gone);

    OK(fs_tree_create(&tree, drive, "rename-source", 0, 8, NULL));
    FsNode* source = lookup(drive, "rename-source");
    CHECK(fs_tree_rename(&tree, drive, "rename-source", "gone/target", 0) != FS_TREE_OK);
    CHECK(lookup(drive, "rename-source") == source && host_exists("drive/rename-source"));
    absent(gone, "target");

    host_write("drive/partial/blocked/external.bin", "keep");
    CHECK(fs_tree_remove(&tree, drive, "partial", 1, 1, 0) != FS_TREE_OK);
    absent(drive, "partial/a.bin");
    absent(drive, "partial/blocked/known.bin");
    CHECK(!host_exists("drive/partial/a.bin") && !host_exists("drive/partial/blocked/known.bin"));
    CHECK(lookup(drive, "partial/blocked")->is_dir);
    CHECK(host_exists("drive/partial/blocked/external.bin"));
    absent(drive, "partial/blocked/external.bin");
    host_remove("drive/partial/blocked/external.bin", 0);
    OK(fs_tree_remove(&tree, drive, "partial", 1, 1, 0));
    absent(drive, "partial");
    CHECK(!host_exists("drive/partial"));

    FsNode* folder = lookup(drive, "folder");
    FsNode* deep = lookup(folder, "sub/deep.bin");
    fs_tree_retain(deep);
    OK(fs_tree_remove(&tree, drive, "folder", 1, 1, 1));
    CHECK(lookup(drive, "folder") == folder && children(folder) == 0);
    CHECK(host_exists("drive/folder") && !host_exists("drive/folder/sub"));
    CHECK(!deep->attached && deep->size == 4);
    CHECK(fs_tree_path(&tree, deep, path, sizeof path) == FS_TREE_NOTFOUND);
    fs_tree_release(deep);
    puts("PASS missing index stays absent, failed host mutations and partial recursive removal");
}

typedef struct {
    unsigned id, won;
    int error;
} Worker;

static void create_worker(Worker* worker) {
    for (unsigned i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof path, "drive/thread-%u-%u", worker->id, i);
        pm_lock(&tree.lock);
        if (fs_tree_create(&tree, NULL, path, 0, i, NULL) != FS_TREE_OK) worker->error = 1;
        pm_unlock(&tree.lock);
    }
    pm_lock(&tree.lock);
    FsTreeResult result = fs_tree_create(&tree, NULL, "drive/one-winner", 0, 42, NULL);
    worker->won = result == FS_TREE_OK;
    if (result != FS_TREE_OK && result != FS_TREE_EXISTS) worker->error = 1;
    pm_unlock(&tree.lock);
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID arg) {
    create_worker(arg);
    return 0;
}
#else
static void* worker_main(void* arg) {
    create_worker(arg);
    return NULL;
}
#endif

static void test_concurrent(void) {
    Worker workers[8] = {0};
#if defined(_WIN32)
    HANDLE threads[8];
#else
    pthread_t threads[8];
#endif
    unsigned winners = 0;
    for (unsigned i = 0; i < 8; i++) {
        workers[i].id = i;
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, worker_main, &workers[i], 0, NULL);
        CHECK(threads[i] != NULL);
#else
        CHECK(!pthread_create(&threads[i], NULL, worker_main, &workers[i]));
#endif
    }
    for (unsigned i = 0; i < 8; i++) {
#if defined(_WIN32)
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        CHECK(CloseHandle(threads[i]));
#else
        CHECK(!pthread_join(threads[i], NULL));
#endif
        CHECK(!workers[i].error);
        winners += workers[i].won;
    }
    CHECK(winners == 1);
    pm_lock(&tree.lock);
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 8; j++) {
            char path[64];
            snprintf(path, sizeof path, "drive/thread-%u-%u", i, j);
            CHECK(lookup(NULL, path)->size == j && host_exists(path));
        }
    }
    CHECK(lookup(NULL, "drive/one-winner")->size == 42);
    children(lookup(NULL, "drive:/"));
    pm_unlock(&tree.lock);
    puts("PASS concurrent creation under the caller's PMutex lock");
}

static void test_missing_root(const char* scratch) {
    char path[FS_TREE_PATH_MAX];
    CHECK(snprintf(path, sizeof path, "%s/without-root", scratch) < (int)sizeof path);
    scans_allowed = 1;
    FsTree other;
    OK(fs_tree_init(&other, path));
    scans_allowed = 0;
    pm_lock(&other.lock);
    FsNode* root = NULL;
    OK(fs_tree_lookup(&other, NULL, "/", &root));
    CHECK(root != NULL && root->is_dir && root->is_virtual);
    OK(fs_tree_create(&other, root, "created.bin", 0, 11, NULL));
    FsNode* file = NULL;
    OK(fs_tree_lookup(&other, NULL, "/created.bin", &file));
    CHECK(file->size == 11 && !root->is_virtual);
    OK(fs_tree_path(&other, file, path, sizeof path));
    FILE* f = fopen(path, "rb");
    CHECK(f != NULL && !fclose(f));
    fs_tree_destroy(&other);
    pm_unlock(&other.lock);
    puts("PASS absent guest root materializes inside the captured base");
}

static void test_failed_initialization(const char* scratch) {
    char path[FS_TREE_PATH_MAX];
    CHECK(snprintf(path, sizeof path, "%s/hardlinks", scratch) < (int)sizeof path);
    FsTree invalid;
    scans_allowed = 1;
    CHECK(fs_tree_init(&invalid, path) == FS_TREE_INVALID);
    scans_allowed = 0;
    CHECK(invalid.root == NULL && invalid.base == NULL);
    CHECK(snprintf(path, sizeof path, "%s/no-such-directory", scratch) < (int)sizeof path);
    scans_allowed = 1;
    CHECK(fs_tree_init(&invalid, path) != FS_TREE_OK);
    scans_allowed = 0;
    CHECK(invalid.root == NULL && invalid.base == NULL);
    puts("PASS failed scans discard the index and initial hardlinks are rejected");
}

/* fs_tree_graft: a folder elsewhere as a readonly child of the data directory (the RomFS of Extract to tmpfs), its
   host paths in that folder and every other in the data directory */
static void test_graft(const char* scratch) {
    char base[FS_TREE_PATH_MAX], ram[FS_TREE_PATH_MAX], path[FS_TREE_PATH_MAX], text[8] = {0};
    const char* tail = "/graft-ram/sub/file.bin";
    FsTree other;
    FsNode *file = NULL, *dir = NULL, *made = NULL;
    FILE* f;
    CHECK(snprintf(base, sizeof base, "%s/graft-base", scratch) < (int)sizeof base);
    CHECK(snprintf(ram, sizeof ram, "%s/graft-ram", scratch) < (int)sizeof ram);
    CHECK(!plat_mkdir(base) && !plat_mkdir(ram));
    CHECK(snprintf(path, sizeof path, "%s/sub", ram) < (int)sizeof path && !plat_mkdir(path));
    CHECK(snprintf(path, sizeof path, "%s%s", scratch, tail) < (int)sizeof path);
    f = fopen(path, "wb");
    CHECK(f != NULL && fwrite("bytes", 1, 5, f) == 5 && !fclose(f));
    scans_allowed = 1;
    OK(fs_tree_init(&other, base));
    pm_lock(&other.lock);
    OK(fs_tree_graft(&other, "romfs", ram));
    CHECK(fs_tree_graft(&other, "romfs", ram) == FS_TREE_EXISTS);
    CHECK(snprintf(path, sizeof path, "%s/no-such-directory", scratch) < (int)sizeof path);
    CHECK(fs_tree_graft(&other, "missing", path) != FS_TREE_OK);
    scans_allowed = 0;
    CHECK(fs_tree_lookup(&other, NULL, "missing", &dir) == FS_TREE_NOTFOUND);
    OK(fs_tree_lookup(&other, NULL, "romfs/sub/file.bin", &file));
    CHECK(file->size == 5 && file->readonly);
    OK(fs_tree_path(&other, file, path, sizeof path));
    CHECK(strlen(path) > strlen(tail) && !strcmp(path + strlen(path) - strlen(tail), tail));
    f = fopen(path, "rb");
    CHECK(f != NULL && fread(text, 1, sizeof text, f) == 5 && !fclose(f) && !strcmp(text, "bytes"));
    /* nothing under it changes */
    OK(fs_tree_lookup(&other, NULL, "romfs/sub", &dir));
    CHECK(fs_tree_create(&other, dir, "new.bin", 0, 1, NULL) == FS_TREE_READONLY);
    CHECK(fs_tree_remove(&other, NULL, "romfs/sub/file.bin", 0, 0, 0) == FS_TREE_READONLY);
    CHECK(fs_tree_remove(&other, NULL, "romfs", 1, 1, 0) == FS_TREE_READONLY);
    CHECK(fs_tree_rename(&other, NULL, "romfs/sub/file.bin", "moved.bin", 0) == FS_TREE_READONLY);
    /* the rest is the data directory's */
    OK(fs_tree_create(&other, NULL, "save.bin", 0, 3, &made));
    OK(fs_tree_path(&other, made, path, sizeof path));
    CHECK(!strncmp(path, other.base, strlen(other.base)) && strstr(path, "/graft-base/save.bin"));
    fs_tree_destroy(&other);
    pm_unlock(&other.lock);
    puts("PASS a graft reads its own folder, readonly, and the rest stays in the data directory");
}

/* path, a full one, is on the host, as host_exists asks */
static int host_there(const char* path) {
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return !stat(path, &st);
#endif
}
/* fs_tree_graft_writable: a folder elsewhere the guest changes, its host paths in that folder (a save in the folder of
   every game's). fs_tree_graft_new: a folder made now, empty, with nothing enumerated; one there already is refused,
   as only a scan could index it */
static void test_graft_writable(const char* scratch) {
    char base[FS_TREE_PATH_MAX], saves[FS_TREE_PATH_MAX], path[FS_TREE_PATH_MAX];
    FsTree other;
    FsNode *file = NULL, *dir = NULL, *made = NULL;
    FILE* f;
    CHECK(snprintf(base, sizeof base, "%s/writable-base", scratch) < (int)sizeof base);
    CHECK(snprintf(saves, sizeof saves, "%s/writable-saves", scratch) < (int)sizeof saves);
    CHECK(!plat_mkdir(base) && !plat_mkdir(saves));
    CHECK(snprintf(path, sizeof path, "%s/old.bin", saves) < (int)sizeof path);
    f = fopen(path, "wb");
    CHECK(f != NULL && fwrite("save", 1, 4, f) == 4 && !fclose(f));
    scans_allowed = 1;
    OK(fs_tree_init(&other, base));
    pm_lock(&other.lock);
    OK(fs_tree_graft_writable(&other, "global-user", saves));
    scans_allowed = 0;
    OK(fs_tree_lookup(&other, NULL, "global-user/old.bin", &file));
    CHECK(file->size == 4 && !file->readonly);
    OK(fs_tree_lookup(&other, NULL, "global-user", &dir));
    OK(fs_tree_create(&other, dir, "new.bin", 0, 2, &made));
    OK(fs_tree_path(&other, made, path, sizeof path));
    CHECK(strstr(path, "/writable-saves/new.bin") != NULL);
    f = fopen(path, "rb");
    CHECK(f != NULL && !fclose(f));
    OK(fs_tree_rename(&other, dir, "new.bin", "renamed.bin", 0));
    CHECK(snprintf(path, sizeof path, "%s/renamed.bin", saves) < (int)sizeof path && host_there(path));
    OK(fs_tree_remove(&other, dir, "old.bin", 0, 0, 0));
    CHECK(snprintf(path, sizeof path, "%s/old.bin", saves) < (int)sizeof path && !host_there(path));
    /* a folder made now: grafted empty, with no scan, and its files on the host there */
    CHECK(snprintf(path, sizeof path, "%s/fresh", saves) < (int)sizeof path);
    OK(fs_tree_graft_new(&other, "global-device", path));
    CHECK(host_there(path));
    OK(fs_tree_lookup(&other, NULL, "global-device", &dir));
    CHECK(!dir->children && !dir->readonly);
    OK(fs_tree_create(&other, dir, "d.bin", 0, 1, &made));
    OK(fs_tree_path(&other, made, path, sizeof path));
    CHECK(strstr(path, "/writable-saves/fresh/d.bin") != NULL);
    /* one there already, or a name taken, or no parent: refused, nothing made */
    CHECK(snprintf(path, sizeof path, "%s/fresh", saves) < (int)sizeof path);
    CHECK(fs_tree_graft_new(&other, "global-bcat", path) == FS_TREE_EXISTS);
    CHECK(fs_tree_lookup(&other, NULL, "global-bcat", &dir) == FS_TREE_NOTFOUND);
    CHECK(fs_tree_graft_new(&other, "global-device", saves) == FS_TREE_EXISTS);
    CHECK(snprintf(path, sizeof path, "%s/no-parent/child", saves) < (int)sizeof path);
    CHECK(fs_tree_graft_new(&other, "global-cache", path) == FS_TREE_IO);
    CHECK(fs_tree_lookup(&other, NULL, "global-cache", &dir) == FS_TREE_NOTFOUND);
    fs_tree_destroy(&other);
    pm_unlock(&other.lock);
    puts("PASS a writable graft changes its own folder; a new one is made empty, with nothing scanned");
}

/* fs_tree_share maps what its limits let it, in bytes and in files; the rest is left to be read on demand, counted
   apart from what failed. A lazy share maps nothing but makes the files readonly, and fs_tree_map maps each when
   asked, within the same limits */
static void test_share_limits(const char* scratch) {
    static const char* const files[][2] = {
        {"a.bin", "12345"}, {"b.bin", "12345"}, {"big.bin", "12345678901234567890"}, {"sub/d.bin", "12345"}};
    const FsShare limits[] = {{.bytes = 12}, {.files = 2}, {0}, {.files = 2, .lazy = 1}};
    for (size_t k = 0; k < sizeof limits / sizeof *limits; ++k) {
        char base[FS_TREE_PATH_MAX], path[FS_TREE_PATH_MAX];
        FsShare share = limits[k];
        FsTree other;
        FsNode *romfs = NULL, *big = NULL, *a = NULL, *b = NULL;
        CHECK(snprintf(base, sizeof base, "%s/share-%zu", scratch, k) < (int)sizeof base && !plat_mkdir(base));
        CHECK(snprintf(path, sizeof path, "%s/romfs", base) < (int)sizeof path && !plat_mkdir(path));
        CHECK(snprintf(path, sizeof path, "%s/romfs/sub", base) < (int)sizeof path && !plat_mkdir(path));
        for (size_t i = 0; i < sizeof files / sizeof *files; ++i) {
            FILE* f;
            CHECK(snprintf(path, sizeof path, "%s/romfs/%s", base, files[i][0]) < (int)sizeof path);
            f = fopen(path, "wb");
            CHECK(f != NULL && fwrite(files[i][1], 1, strlen(files[i][1]), f) == strlen(files[i][1]) && !fclose(f));
        }
        scans_allowed = 1;
        OK(fs_tree_init(&other, base));
        scans_allowed = 0;
        pm_lock(&other.lock);
        OK(fs_tree_lookup(&other, NULL, "romfs", &romfs));
        OK(fs_tree_lookup(&other, NULL, "romfs/big.bin", &big));
        OK(fs_tree_lookup(&other, NULL, "romfs/a.bin", &a));
        OK(fs_tree_lookup(&other, NULL, "romfs/b.bin", &b));
        sharing = 1;
        OK(fs_tree_share(&other, romfs, &share));
        if (share.lazy) { /* none mapped until asked, then two, the third past the limit, each once */
            CHECK(!share.shared && !share.skipped && big->readonly && a->readonly && !fs_tree_mapped(a));
            CHECK(!fs_tree_map(&other, a, &share) && fs_tree_mapped(a) && !fs_tree_map(&other, a, &share));
            CHECK(!fs_tree_map(&other, b, &share) && fs_tree_map(&other, big, &share) == -1);
            CHECK(share.shared == 2 && share.skipped == 1 && !fs_tree_mapped(big));
            CHECK(fs_tree_map(&other, romfs, &share) == -1); /* a folder isn't mapped */
        } else
            CHECK(!share.failed && share.shared + share.skipped == 4 && big->readonly);
        if (k == 0) /* 12 bytes: two files of five, the twenty past it */
            CHECK(share.shared == 2 && share.mapped_bytes == 10 && big->fd < 0);
        else if (k == 1)
            CHECK(share.shared == 2 && share.skipped == 2);
        else if (k == 2)
            CHECK(share.shared == 4 && share.mapped_bytes == 35 && big->fd >= 1000);
        fs_tree_destroy(&other);
        sharing = 0;
        pm_unlock(&other.lock);
    }
    puts("PASS a share maps what its limits let it, the rest read on demand and counted apart");
}

int main(int argc, char** argv) {
    CHECK(argc == 3);
    CHECK(strstr(argv[1], "scarletnx-fs-tree-") != NULL);
    CHECK(!host_chdir(argv[1]));
    FILE* marker = fopen(".snx-fs-test", "rb");
    char token[64] = {0};
    CHECK(marker != NULL);
    CHECK(fread(token, 1, sizeof token - 1, marker) == strlen(argv[2]));
    CHECK(!fclose(marker) && !strcmp(token, argv[2]));
    OK(fs_tree_init(&tree, "data"));
    CHECK(scan_count > 0);
    scans_allowed = 0;
    CHECK(tree.base != NULL && strlen(tree.base) < sizeof original_base);
    strcpy(original_base, tree.base);
#if defined(_WIN32)
    CHECK((tree.base[0] && tree.base[1] == ':') || (tree.base[0] == '\\' && tree.base[1] == '\\'));
#else
    CHECK(tree.base[0] == '/');
#endif
    /* Resolve future paths against the captured absolute base after cwd changes. */
    CHECK(!host_chdir("data/romfs"));
    pm_lock(&tree.lock);
    test_lookup();
    test_mutations();
    test_virtual();
    test_rename_path_limit();
    test_no_rescan_and_failures();
    pm_unlock(&tree.lock);
    test_concurrent();
    pm_lock(&tree.lock);
    fs_tree_destroy(&tree);
    pm_unlock(&tree.lock);
    test_missing_root(argv[1]);
    test_failed_initialization(argv[1]);
    test_graft(argv[1]);
    test_graft_writable(argv[1]);
    test_share_limits(argv[1]);
    CHECK(!host_chdir(argv[1]));
    printf("PASS %u checks; native enumeration was restricted to initialization (%u opens)\n", checks, scan_count);
    return 0;
}
