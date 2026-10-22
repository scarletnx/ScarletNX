/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the actual HLE dispatchers with synthetic IPC buffers and a small guest address space. */
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/kernel/ipc.h"
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

/* Including the production file exposes its service constructors without test hooks in the runtime. */
#include "nx/hle/filesystem/fs.c"
#include "nx/hle/hle.c"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
char g_datadir[1024], g_exename[256], g_exedir[1024], g_confdir[1024], g_addondir[1024], g_gamename[256];
u64 g_addon;
const u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull}; /* am.c's: the first user */
static jmp_buf* test_fatal_jump;                                   /* a fatal() the test waits for comes back here */
static char test_fatal_text[1024];
static unsigned test_checks, test_scans, test_objects;
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

int plat_dir_open(PDir* d, const char* path) {
    (void)d;
    (void)path;
    test_fail("HLE attempted platform directory enumeration", __LINE__);
    return -1;
}
int plat_dir_next(PDir* d) {
    (void)d;
    test_fail("HLE attempted platform directory enumeration", __LINE__);
    return 0;
}
void plat_dir_close(PDir* d) { (void)d; }
int plat_is_dir(const char* path) {
    (void)path;
    test_fail("HLE attempted host existence query", __LINE__);
    return 0;
}
u64 plat_file_size(const char* path) {
    (void)path;
    test_fail("HLE attempted host size query", __LINE__);
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
static char test_fs_text[1024]; /* the last warning of fs.c's own, "fs: ..." */
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
    if (!level && !strncmp(text, "fs: ", 4)) snprintf(test_fs_text, sizeof test_fs_text, "%s", text);
}
/* options.c's: the game's settings, where the runtime read them */
const char* ini_file(void) { return "config/Game.ini"; }
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

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    test_objects++;
    return obj;
}
#include "svcobj_data.h"
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj);
    test_svcobj_data_free(obj);
    free(obj);
    test_objects--;
}
/* the notifiers' events: counted, so the test sees each one dropped with its notifier */
static int test_events;
u32 owned_event(const char* name) {
    KEvent* event = calloc(1, sizeof *event);
    CHECK(event != NULL);
    (void)name;
    event->o.refs = 1;
    test_events++;
    return kobj_id(&event->o);
}
void kobj_unref(KObj* o) {
    if (--o->refs) return;
    free(o);
    test_events--;
}
static KEvent* test_handles[32]; /* handle 0x100 + i is a reference on test_handles[i], as the kernel's would be */
static unsigned test_nhandles;
u32 ev_handle(u32 ev) {
    KEvent* event = kev(ev);
    CHECK(test_nhandles < sizeof test_handles / sizeof test_handles[0]);
    event->o.refs++;
    test_handles[test_nhandles] = event;
    return 0x100 + test_nhandles++;
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void out_obj(IpcCtx* c, SvcObj* obj) { /* NULL from a failed command, which ipc.c drops */
    CHECK(c->nout_objs < 8);
    c->out_objs[c->nout_objs++] = obj;
}
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* obj, IpcCtx* c) {
    fprintf(stderr, "Unexpected stub: %s command %u\n", obj->name, c->cmd);
    test_fail("requested a command the service does not declare", __LINE__);
    return -1;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}

typedef struct {
    IpcCtx c;
    u8 raw[0x80];
} Request;

static void request(Request* r, u32 cmd) {
    memset(r, 0, sizeof *r);
    r->c.cmd = cmd;
    r->c.raw = r->raw;
    r->c.raw_size = sizeof r->raw;
}
/* the request is the guest's, little-endian whatever the host's order */
static void argument32(Request* r, unsigned off, u32 value) { snx_st_le32(r->raw + off, value); }
static void argument64(Request* r, unsigned off, u64 value) { snx_st_le64(r->raw + off, value); }
static void argument_path(Request* r, int idx, const char* path) {
    size_t size = strlen(path) + 1;
    CHECK(idx < 2 && size <= 0x1000);
    u64 address = 0x1000 + (u64)idx * 0x1000;
    memcpy(GP(address), path, size);
    r->c.x[idx] = (IpcBuf){address, size, 0};
    r->c.nx = idx + 1;
}
static void output_buffer(Request* r, u64 size) {
    CHECK(size <= 0x8000);
    r->c.b[0] = (IpcBuf){0x5000, size, 0};
    r->c.nb = 1;
    memset(GP(0x5000), 0xcc, (size_t)size);
}
static u64 output64(Request* r) {
    CHECK(r->c.out_size >= 8);
    return snx_ld_le64(r->c.out);
}
/* the dispatcher, then what ipc.c does with a failed request's reply: no data, handles or objects */
static void dispatch(SvcObj* obj, Request* r, u32 expected) {
    u32 result = (u32)obj->dispatch(obj, &r->c);
    if (result) {
        for (int i = 0; i < r->c.nout_objs; i++) svcobj_unref(r->c.out_objs[i]);
        r->c.nout_copy = r->c.nout_move = r->c.nout_objs = 0;
        r->c.out_size = 0;
    }
    if (result != expected)
        fprintf(stderr, "%s command %u returned %u, expected %u\n", obj->name, r->c.cmd, result, expected);
    CHECK(result == expected);
}
static void fs_command(SvcObj* fs, u32 command, const char* path, u64 value, u32 expected) {
    Request r;
    request(&r, command);
    argument_path(&r, 0, path);
    argument64(&r, 8, value);
    dispatch(fs, &r, expected);
}
static SvcObj* open_object(SvcObj* fs, u32 command, const char* path, u32 mode) {
    Request r;
    request(&r, command);
    argument32(&r, 0, mode);
    argument_path(&r, 0, path);
    dispatch(fs, &r, 0);
    CHECK(r.c.nout_objs == 1);
    return r.c.out_objs[0];
}
static void rename_object(SvcObj* fs, u32 command, const char* source, const char* dest, u32 expected) {
    Request r;
    request(&r, command);
    argument_path(&r, 0, source);
    argument_path(&r, 1, dest);
    dispatch(fs, &r, expected);
}
static u64 get_size(SvcObj* file) {
    Request r;
    request(&r, 4);
    dispatch(file, &r, 0);
    return output64(&r);
}
static void set_size(SvcObj* file, u64 size, u32 expected) {
    Request r;
    request(&r, 3);
    argument64(&r, 0, size);
    dispatch(file, &r, expected);
}
static void write_data(SvcObj* file, u64 offset, const char* data, size_t size, u32 expected) {
    Request r;
    request(&r, 1);
    argument32(&r, 0, 1);
    argument64(&r, 8, offset);
    argument64(&r, 16, size);
    CHECK(size < 0x1000);
    memcpy(GP(0x3000), data, size);
    r.c.a[0] = (IpcBuf){0x3000, size, 0};
    r.c.na = 1;
    dispatch(file, &r, expected);
}
static u64 read_data(SvcObj* file, u64 offset, size_t size, u32 expected) {
    Request r;
    request(&r, 0);
    argument64(&r, 8, offset);
    argument64(&r, 16, size);
    output_buffer(&r, size);
    dispatch(file, &r, expected);
    return expected ? 0 : output64(&r);
}
static u64 dir_count(SvcObj* dir) {
    Request r;
    request(&r, 1);
    dispatch(dir, &r, 0);
    return output64(&r);
}
static u64 dir_read(SvcObj* dir, unsigned capacity) {
    Request r;
    request(&r, 0);
    output_buffer(&r, (u64)capacity * 0x310);
    dispatch(dir, &r, 0);
    return output64(&r);
}

static void test_file_services(SvcObj* fs) {
    fs_command(fs, 0, "/work.bin", 4, 0);
    fs_command(fs, 0, "/work.bin", 4, FS_EXISTS);
    fs_command(fs, 0, "/missing/file.bin", 4, FS_NOTFOUND);
    SvcObj* writer = open_object(fs, 8, "/work.bin", 7);
    SvcObj* reader = open_object(fs, 8, "/work.bin", 1);
    CHECK(get_size(writer) == 4 && get_size(reader) == 4);
    write_data(writer, 4, "hello", 5, 0);
    CHECK(get_size(reader) == 9);
    CHECK(read_data(reader, 0, 16, 0) == 9);
    const u8 expected[9] = {0, 0, 0, 0, 'h', 'e', 'l', 'l', 'o'};
    CHECK(!memcmp(GP(0x5000), expected, sizeof expected));
    CHECK(GP(0x5000)[9] == 0xcc);
    write_data(reader, 0, "x", 1, FS_WRITE_NOT_PERMITTED);
    set_size(reader, 1, FS_WRITE_NOT_PERMITTED);
    SvcObj* fixed = open_object(fs, 8, "/work.bin", 3);
    write_data(fixed, 9, "x", 1, FS_NO_APPEND);
    CHECK(get_size(fixed) == 9);
    set_size(writer, 2, 0);
    CHECK(get_size(reader) == 2 && get_size(fixed) == 2);
    CHECK(read_data(reader, 0, 16, 0) == 2);
    read_data(reader, 3, 1, FS_OUT_OF_RANGE);
    set_size(writer, 8192, 0);
    CHECK(get_size(reader) == 8192 && get_size(fixed) == 8192);
    CHECK(read_data(reader, 8191, 1, 0) == 1 && GP(0x5000)[0] == 0);
    SvcObj* dir = open_object(fs, 9, "/", 2);
    u64 entries = dir_read(dir, 8);
    int found = 0;
    for (u64 i = 0; i < entries; i++) {
        u8* entry = GP(0x5000 + i * 0x310);
        if (!strcmp((char*)entry, "work.bin")) {
            u64 size;
            size = snx_ld_le64(entry + 0x308);
            CHECK(size == 8192 && entry[0x304] == 1);
            found = 1;
        }
    }
    CHECK(found);
    svcobj_unref(dir);
    svcobj_unref(fixed);
    svcobj_unref(reader);
    svcobj_unref(writer);
    rename_object(fs, 5, "/work.bin", "/renamed.bin", 0);
    fs_command(fs, 7, "/work.bin", 0, FS_NOTFOUND);
    /* GetFileTimeStampRaw (14): the host file's time for created, modified and accessed, and the flag that says so;
       nothing for a name that isn't there */
    {
        Request r;
        u64 now = (u64)time(NULL), t[3];
        request(&r, 14);
        argument_path(&r, 0, "/renamed.bin");
        dispatch(fs, &r, 0);
        CHECK(r.c.out_size == 0x20);
        memcpy(t, r.c.out, sizeof t);
        snx_le64_array(t, 3);
        CHECK(t[0] == t[1] && t[1] == t[2] && t[0] + 60 >= now && t[0] <= now + 60 && r.c.out[0x18] == 1);
        request(&r, 14);
        argument_path(&r, 0, "/work.bin");
        dispatch(fs, &r, FS_NOTFOUND);
    }
    reader = open_object(fs, 8, "/renamed.bin", 1);
    CHECK(get_size(reader) == 8192);
    svcobj_unref(reader);
    fs_command(fs, 1, "/renamed.bin", 0, 0);
    fs_command(fs, 7, "/renamed.bin", 0, FS_NOTFOUND);
    puts("PASS HLE create/open/write/read/truncate/rename/delete and shared cached size");
}

/* Expected results are taken from Atmosphere's interface adapters and fs/fsa contracts,
   not the production FS_* aliases. All requests have well-formed IPC descriptors. */
static void test_file_contracts(SvcObj* fs) {
    const u64 negative = UINT64_MAX;
    fs_command(fs, 0, "/contract.bin", 4, 0);
    SvcObj* rw = open_object(fs, 8, "/contract.bin", 3);
    SvcObj* ro = open_object(fs, 8, "/contract.bin", 1);
    SvcObj* wo = open_object(fs, 8, "/contract.bin", 2);
    static const struct {
        u64 off, size, capacity;
        u32 result;
    } cases[] = {
        {UINT64_MAX, UINT64_MAX, 4, RC(2, 6061)}, /* offset wins over invalid size */
        {UINT64_MAX, 0, 0, RC(2, 6061)},
        {0, UINT64_MAX, 4, RC(2, 6062)},
        {0, 5, 4, RC(2, 6062)},   /* never silently shorten an IPC transfer */
        {100, 5, 4, RC(2, 6062)}, /* buffer check wins over EOF and permission */
        {0, 1, 0, RC(2, 6062)},
        {INT64_MAX, 1, 4, RC(2, 3005)}, /* overflow wins over permission */
        {INT64_MAX - 1, 2, 4, RC(2, 3005)},
        {INT64_MAX, 0, 0, 0}, /* empty transfers skip EOF and mode checks */
        {0, 0, 0, 0},
    };
    for (u32 cmd = 0; cmd < 2; ++cmd) {
        for (unsigned permitted = 0; permitted < 2; ++permitted) {
            SvcObj* target = permitted ? rw : cmd ? ro : wo;
            for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
                Request r;
                request(&r, cmd);
                argument32(&r, 0, cmd); /* empty writes request Flush */
                argument64(&r, 8, cases[i].off);
                argument64(&r, 16, cases[i].size);
                output_buffer(&r, 16);
                if (cmd) { /* Write sends its data in A, and no B */
                    r.c.a[0] = (IpcBuf){0x3000, cases[i].capacity, 0};
                    r.c.na = 1;
                    r.c.nb = 0;
                } else
                    r.c.b[0].size = cases[i].capacity;
                dispatch(target, &r, cases[i].result);
                CHECK(r.c.nout_objs == 0 && r.c.nout_copy == 0 && r.c.nout_move == 0);
                if (!cases[i].result && !cmd)
                    CHECK(r.c.out_size == 8 && output64(&r) == 0);
                else
                    CHECK(r.c.out_size == 0);
                for (unsigned j = 0; j < 16; ++j) CHECK(GP(0x5000)[j] == 0xcc);
                CHECK(get_size(rw) == 4);
            }
        }
    }
    read_data(wo, 0, 1, RC(2, 6202));
    write_data(ro, 0, "x", 1, RC(2, 6203));
    write_data(rw, 4, "x", 1, RC(2, 6201));
    set_size(ro, negative, RC(2, 6062)); /* adapter rejects size before backend permissions */
    set_size(rw, negative, RC(2, 6062));
    CHECK(get_size(rw) == 4);
    CHECK(read_data(ro, 0, 4, 0) == 4);
    for (unsigned j = 0; j < 4; ++j) CHECK(GP(0x5000)[j] == 0);
    fs_command(fs, 0, "/new.bin", negative, RC(2, 6062));
    fs_command(fs, 7, "/new.bin", 0, RC(2, 1));
    fs_command(fs, 0, "/../bad", negative, RC(2, 6062)); /* before path normalization */
    static const u32 modes[] = {0, 4, 8, 0x80000000u};
    static const char* paths[] = {"/contract.bin", "/missing", "/"};
    for (u32 cmd = 8; cmd <= 9; ++cmd)
        for (size_t i = 0; i < sizeof modes / sizeof *modes; ++i)
            for (size_t j = 0; j < sizeof paths / sizeof *paths; ++j) {
                Request r;
                request(&r, cmd);
                argument32(&r, 0, modes[i]);
                argument_path(&r, 0, paths[j]);
                dispatch(fs, &r, RC(2, 6072));
                CHECK(r.c.nout_objs == 0 && r.c.out_size == 0);
            }
    SvcObj* dir = open_object(fs, 9, "/", 0x80000003u);
    svcobj_unref(dir);
    svcobj_unref(wo);
    svcobj_unref(ro);
    svcobj_unref(rw);
    fs_command(fs, 1, "/contract.bin", 0, 0);
    puts("PASS Atmosphere IFile/IFileSystem signed arguments, error priority, empty transfers and open modes");
}

/* Atmosphere FileSystemInterfaceAdapter checks normalized roots/ancestry before backend mutation. */
static void test_directory_path_contracts(SvcObj* fs) {
    fs_command(fs, 2, "/", 0, RC(2, 2));
    fs_command(fs, 2, "/./", 0, RC(2, 2));
    fs_command(fs, 2, "/absent/..", 0, RC(2, 2));
    fs_command(fs, 3, "/", 0, RC(2, 6031));
    fs_command(fs, 4, "/./", 0, RC(2, 6031));
    /* Neither path needs to exist for the strict-ancestry check to win. Both directions count. */
    rename_object(fs, 6, "/absent", "/absent/child", RC(2, 6032));
    rename_object(fs, 6, "/absent/child", "/absent", RC(2, 6032));
    rename_object(fs, 6, "/", "/absent", RC(2, 6032));
    rename_object(fs, 6, "/absent", "/", RC(2, 6032));
    /* Negative controls retain this backend's PathNotFound. IsSubPath itself excludes equality/siblings. */
    rename_object(fs, 6, "/absent", "/absent", RC(2, 1));
    rename_object(fs, 6, "/absent", "/absent-sibling", RC(2, 1));
    fs_command(fs, 2, "/ancestry", 0, 0);
    fs_command(fs, 2, "/ancestry/child", 0, 0);
    rename_object(fs, 6, "/ancestry", "/ancestry/child", RC(2, 6032)); /* before AlreadyExists */
    rename_object(fs, 6, "/ancestry/child", "/ancestry", RC(2, 6032));
    rename_object(fs, 6, "/ancestry", "/ancestry-sibling", 0);
    fs_command(fs, 7, "/ancestry-sibling/child", 0, 0);
    fs_command(fs, 4, "/ancestry-sibling", 0, 0);
    /* Eden's temporary recursive CreateDirectory behavior must not replace this contract. */
    fs_command(fs, 2, "/absent/child", 0, RC(2, 1));
    fs_command(fs, 7, "/absent", 0, RC(2, 1));
    puts("PASS Atmosphere normalized directory roots and bidirectional strict ancestry");
}

static void test_directory_services(SvcObj* fs) {
    fs_command(fs, 2, "/list", 0, 0);
    fs_command(fs, 0, "/list/a", 1, 0);
    fs_command(fs, 0, "/list/b", 2, 0);
    fs_command(fs, 0, "/list/c", 3, 0);
    SvcObj* dir = open_object(fs, 9, "/list", 3);
    CHECK(dir_count(dir) == 3);
    CHECK(dir_read(dir, 1) == 1 && !strcmp((char*)GP(0x5000), "a"));
    fs_command(fs, 1, "/list/b", 0, 0);
    fs_command(fs, 0, "/list/aa", 4, 0);
    rename_object(fs, 6, "/list", "/listed", 0);
    CHECK(dir_count(dir) == 3);
    CHECK(dir_read(dir, 2) == 2);
    CHECK(!strcmp((char*)GP(0x5000), "aa"));
    CHECK(!strcmp((char*)GP(0x5310), "c"));
    CHECK(dir_read(dir, 1) == 0);
    fs_command(fs, 0, "/listed/d", 5, 0);
    CHECK(dir_count(dir) == 4);
    CHECK(dir_read(dir, 1) == 0);
    SvcObj* fresh = open_object(fs, 9, "/listed", 2);
    CHECK(dir_count(fresh) == 4 && dir_read(fresh, 8) == 4);
    svcobj_unref(fresh);
    fs_command(fs, 3, "/listed", 0, FS_DIR_NOT_EMPTY);
    fs_command(fs, 13, "/listed", 0, 0);
    CHECK(dir_count(dir) == 0);
    fs_command(fs, 3, "/listed", 0, 0);
    Request removed;
    request(&removed, 1);
    dispatch(dir, &removed, FS_NOTFOUND);
    request(&removed, 0);
    output_buffer(&removed, 0x310);
    dispatch(dir, &removed, FS_NOTFOUND);
    svcobj_unref(dir);
    fs_command(fs, 7, "/listed", 0, FS_NOTFOUND);
    puts("PASS HLE directory cursor/count after create, delete, rename and clean");
}

static void test_index_and_paths(SvcObj* fs) {
    char path[FS_TREE_PATH_MAX];
    CHECK(snprintf(path, sizeof path, "%ssdmc/external.bin", g_datadir) < (int)sizeof path);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL && fwrite("unindexed", 1, 9, f) == 9 && !fclose(f));
    fs_command(fs, 7, "/external.bin", 0, FS_NOTFOUND);
    fs_command(fs, 1, "/external.bin", 0, FS_NOTFOUND);
    Request r;
    request(&r, 8);
    argument32(&r, 0, 1);
    argument_path(&r, 0, "/external.bin");
    dispatch(fs, &r, FS_NOTFOUND);
    CHECK(r.c.nout_objs == 0);
    u64 size = 123;
    CHECK(fs_open_read("sdmc/external.bin", &size) == NULL && size == 0);
    CHECK(fs_read_file("sdmc/external.bin", &size) == NULL && size == 0);
    PFileMap map = {0};
    CHECK(fs_map_file("sdmc/external.bin", &map) != 0);
    fs_command(fs, 7, "/../root/root.txt", 0, FS_DIR_UNOBTAINABLE);
    fs_command(fs, 7, "romfs:/asset.bin", 0, FS_DIR_UNOBTAINABLE);
    /* A NUL just beyond the descriptor must never make an unterminated path valid. */
    request(&r, 7);
    argument_path(&r, 0, "/seed.bin");
    r.c.x[0].size--;
    u32 result = (u32)fs->dispatch(fs, &r.c);
    CHECK(result != 0 && r.c.out_size == 0);
    request(&r, 7);
    r.c.x[0] = (IpcBuf){0x1000, 0x301, 0};
    r.c.nx = 1;
    memset(GP(0x1000), 'x', 0x301);
    result = (u32)fs->dispatch(fs, &r.c);
    CHECK(result != 0 && r.c.out_size == 0);
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
    puts("PASS HLE index-only visibility, confined paths, bounded IPC paths and loader helpers");
}

static void test_storage_contracts(SvcObj* storage, u64 size) {
    const struct {
        u64 off, size, capacity;
        u32 read_result, write_result;
    } cases[] = {
        {UINT64_MAX, UINT64_MAX, 4, RC(2, 6061), RC(2, 6061)},
        {UINT64_MAX, 0, 0, RC(2, 6061), RC(2, 6061)},
        {0, UINT64_MAX, 4, RC(2, 6062), RC(2, 6062)},
        {0, 5, 4, RC(2, 6062), RC(2, 6062)},
        {size + 1, 5, 4, RC(2, 6062), RC(2, 6062)},
        {INT64_MAX, 1, 4, RC(2, 3005), RC(2, 6300)},
        {size, 1, 4, RC(2, 3005), RC(2, 6300)},
        {size - 1, 2, 4, RC(2, 3005), RC(2, 6300)}, /* no short IStorage reads */
        {size + 1, 1, 4, RC(2, 3005), RC(2, 6300)},
        {size, 0, 0, 0, RC(2, 6300)},
        {INT64_MAX, 0, 0, 0, RC(2, 6300)},
    };
    CHECK(size > 0);
    for (u32 cmd = 0; cmd < 2; ++cmd)
        for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
            Request r;
            request(&r, cmd);
            argument64(&r, 0, cases[i].off);
            argument64(&r, 8, cases[i].size);
            output_buffer(&r, 16);
            if (cmd) { /* Write sends its data in A, and no B */
                r.c.a[0] = (IpcBuf){0x3000, cases[i].capacity, 0};
                r.c.na = 1;
                r.c.nb = 0;
            } else
                r.c.b[0].size = cases[i].capacity;
            dispatch(storage, &r, cmd ? cases[i].write_result : cases[i].read_result);
            CHECK(r.c.out_size == 0 && r.c.nout_objs == 0);
            for (unsigned j = 0; j < 16; ++j) CHECK(GP(0x5000)[j] == 0xcc);
            CHECK(get_size(storage) == size);
        }
    Request r;
    request(&r, 2);
    dispatch(storage, &r, 0); /* read-only image Flush is not an unsupported write */
    CHECK(r.c.out_size == 0);
    set_size(storage, UINT64_MAX, RC(2, 6062));
    set_size(storage, 0, RC(2, 6300));
    CHECK(get_size(storage) == size);
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
static void test_virtual_services(SvcObj* fs, SvcObj* proxy) {
    u8 bytes[] = {1, 0, 3, 0xff, 5};
    CHECK(!fs_add_virtual_file("sdmc/virtual.bin", bytes, sizeof bytes));
    SvcObj* file = open_object(fs, 8, "/virtual.bin", 1);
    CHECK(get_size(file) == sizeof bytes);
    CHECK(read_data(file, 1, 10, 0) == 4 && !memcmp(GP(0x5000), bytes + 1, 4));
    write_data(file, 0, "x", 1, FS_WRITE_NOT_PERMITTED);
    set_size(file, 0, FS_WRITE_NOT_PERMITTED);
    Request r;
    request(&r, 8);
    argument32(&r, 0, 3);
    argument_path(&r, 0, "/virtual.bin");
    dispatch(fs, &r, FS_WRITE_NOT_PERMITTED);
    fs_command(fs, 1, "/virtual.bin", 0, FS_WRITE_NOT_PERMITTED);
    svcobj_unref(file);

    request(&r, 202);
    argument64(&r, 8, 0x0100000000000819ull);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    SvcObj* archive = r.c.out_objs[0];
    CHECK(get_size(archive) == 0x200);
    test_storage_contracts(archive, 0x200);
    request(&r, 0);
    argument64(&r, 0, 0);
    argument64(&r, 8, 0x200);
    output_buffer(&r, 0x200);
    dispatch(archive, &r, 0);
    u64 header_size;
    header_size = snx_ld_le64(GP(0x5000));
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
    request(&r, 1); /* Write (offset 0, size at 8) of one byte from its A buffer: the archive's backend refuses it */
    argument64(&r, 8, 1);
    r.c.a[0] = (IpcBuf){0x3000, 1, 0};
    r.c.na = 1;
    dispatch(archive, &r, RC(2, 6300));
    svcobj_unref(archive);

    {
        Request a;
        request(&a, 202);
        argument64(&a, 8, 0x0100000000000823ull);
        dispatch(proxy, &a, 0);
        CHECK(a.c.nout_objs == 1);
        SvcObj* ngword = a.c.out_objs[0];
        u64 ngword_size = get_size(ngword);
        CHECK(ngword_size > 0x200 && ngword_size <= 0x2000);
        test_storage_contracts(ngword, ngword_size);
        request(&a, 0);
        argument64(&a, 0, 0);
        argument64(&a, 8, ngword_size);
        output_buffer(&a, ngword_size);
        dispatch(ngword, &a, 0);
        u8* image = GP(0x5000);
        u64 header[10];
            memcpy(header, image, sizeof header);
        snx_le64_array(header, 10);
        CHECK(header[0] == 0x50 && header[9] < ngword_size && header[7] < ngword_size);
        {
            u32 entry, count = 0;
            entry = snx_ld_le32(image + header[3] + 12); /* the root's first file, then the sibling chain */
            while (entry != UINT32_MAX) {
                CHECK(++count <= sizeof ngword2_names / sizeof ngword2_names[0]);
                entry = snx_ld_le32(image + header[7] + entry + 4);
            }
            CHECK(count == sizeof ngword2_names / sizeof ngword2_names[0]);
        }
        {
            u32 entry = romfs_find(image, header, 0, 0, "version.dat");
            u64 offset, size;
            CHECK(entry != UINT32_MAX);
            offset = snx_ld_le64(image + header[7] + entry + 8);
            size = snx_ld_le64(image + header[7] + entry + 16);
            CHECK(size == 4 && offset + 4 <= ngword_size - header[9]);
            CHECK(!memcmp(image + header[9] + offset, "\0\0\0\x1a", 4));
        }
        {
            u32 entry = romfs_find(image, header, 0, 0, "ac_common_b1_nx");
            u64 offset, size;
            CHECK(entry != UINT32_MAX);
            offset = snx_ld_le64(image + header[7] + entry + 8);
            size = snx_ld_le64(image + header[7] + entry + 16);
            CHECK(size == 44 && offset + 44 <= ngword_size - header[9]);
            CHECK(image[header[9] + offset] == 0x1f && image[header[9] + offset + 1] == 0x8b);
        }
        {
            unsigned i;
            for (i = 0; i < sizeof ngword2_names / sizeof ngword2_names[0]; i++) {
                const u8* body = strcmp(ngword2_names[i], "version.dat") ? ngword2_ac : ngword2_version;
                u32 entry = romfs_find(image, header, 0, 0, ngword2_names[i]);
                u64 offset = 0, size = 0;
                CHECK(entry != UINT32_MAX);
                offset = snx_ld_le64(image + header[7] + entry + 8);
                size = snx_ld_le64(image + header[7] + entry + 16);
                CHECK(size == (strcmp(ngword2_names[i], "version.dat") ? 44 : 4));
                CHECK(!memcmp(image + header[9] + offset, body, (size_t)size));
            }
        }
        svcobj_unref(ngword);
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
static void test_extracted_romfs(SvcObj* proxy) {
    Request r;
    request(&r, 200);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    SvcObj* storage = r.c.out_objs[0];
    u64 image_size = get_size(storage);
    CHECK(image_size < 0x8000 && image_size > 0x200);
    test_storage_contracts(storage, image_size);
    request(&r, 0);
    argument64(&r, 8, image_size);
    output_buffer(&r, image_size);
    dispatch(storage, &r, 0);
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
    request(&r, 0);
    argument64(&r, 0, header[9] + 7);
    argument64(&r, 8, 13);
    output_buffer(&r, 13);
    dispatch(storage, &r, 0);
    const u8 spanning[13] = {'e', 's', 't', 0, 0, 0, 0, 0, 0, 't', 'a', 'i', 'l'};
    CHECK(!memcmp(GP(0x5000), spanning, sizeof spanning));
    svcobj_unref(storage);
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

static void test_raw_romfs(SvcObj* proxy) {
    const u8 expected[] = {'R', 'A', 'W', 0, 'R', 'O', 'M', 'F', 'S', 0xff, ' ', 'b', 'y', 't', 'e', 's'};
    Request r;
    request(&r, 200);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    SvcObj* storage = r.c.out_objs[0];
    CHECK(g_hle.fs.rimage != NULL && g_hle.fs.rraw == NULL); /* the image is mapped, not read through a FILE */
    CHECK(get_size(storage) == sizeof expected);
    test_storage_contracts(storage, sizeof expected);
    request(&r, 0);
    argument64(&r, 0, 2);
    argument64(&r, 8, sizeof expected - 2);
    output_buffer(&r, sizeof expected);
    dispatch(storage, &r, 0);
    CHECK(!memcmp(GP(0x5000), expected + 2, sizeof expected - 2));
    CHECK(GP(0x5000)[sizeof expected - 2] == 0xcc);
    svcobj_unref(storage);
    /* OpenDataStorageWithProgramIndex (205): index 0 is this program's RomFS, another index fails with no storage */
    request(&r, 205);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    storage = r.c.out_objs[0];
    CHECK(get_size(storage) == sizeof expected);
    svcobj_unref(storage);
    request(&r, 205);
    r.raw[0] = 1;
    dispatch(proxy, &r, RC(2, 1002));
    CHECK(r.c.nout_objs == 0);
    /* OpenDataStorageByProgramId (201): this program's id is the RomFS, another program's fails */
    request(&r, 201);
    argument64(&r, 0, g_hle.kernel.title_id);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1 && get_size(r.c.out_objs[0]) == sizeof expected);
    svcobj_unref(r.c.out_objs[0]);
    request(&r, 201);
    argument64(&r, 0, g_hle.kernel.title_id + 1);
    dispatch(proxy, &r, RC(2, 1002));
    CHECK(r.c.nout_objs == 0);
    puts("PASS raw RomFS reads, cached image size and the storage by program index and id");
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
static void dump_write(int source) {
    blob_t code = {zalloc(sizeof dump_code - 1), sizeof dump_code - 1}, asset = {zalloc(sizeof dump_asset - 1), 0};
    blob_t meta = dump_npdm(), nsp;
    asset.size = sizeof dump_asset - 1;
    memcpy(code.data, dump_code, code.size);
    memcpy(asset.data, dump_asset, asset.size);
    romfs_file_t exefs[2] = {{"main.npdm", meta}, {"main", code}}, romfs[1] = {{"asset.bin", asset}};
    romfs_file_t ncas[1] = {
        {"program.nca", dnca_build(NCACONTENTTYPE_PROGRAM, 0x0100000000AB0000ULL, exefs, 2, romfs, 1)}};
    nsp = partition_build(MAGIC_PFS0, ncas, 1, 0x200);
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
static void test_dump(SvcObj* proxy) {
    blob_t meta = dump_npdm();
    PFileMap map;
    Request r;
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
    /* the RomFS image, what OpenDataStorageByCurrentProcess (200) gives: its header, then asset.bin's bytes where the
       file table says they are */
    request(&r, 200);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    SvcObj* storage = r.c.out_objs[0];
    u64 total = get_size(storage);
    CHECK(total == g_hle.fs.rtotal && total >= 0x50 && total <= 0x8000);
    test_storage_contracts(storage, total);
    request(&r, 0);
    argument64(&r, 0, 0);
    argument64(&r, 8, total);
    output_buffer(&r, total);
    dispatch(storage, &r, 0);
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
    svcobj_unref(storage);
    free(meta.data);
    puts("PASS the dump's ExeFS and RomFS read in place, nothing extracted");
}

/* the RomFS image storage gives, a file name and its bytes the first file in its table, as a dump's image of one file
   lays it out */
static void image_holds(SvcObj* storage, const char* name, const char* contents) {
    Request r;
    u64 total = get_size(storage), file_table = 0, data_offset = 0, at = 0, size = 0, name_size = 0;
    const u8* image = GP(0x5000);
    CHECK(total >= 0x50 && total <= 0x8000);
    request(&r, 0);
    argument64(&r, 0, 0);
    argument64(&r, 8, total);
    output_buffer(&r, total);
    dispatch(storage, &r, 0);
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
static void test_addon(SvcObj* proxy, int update) {
    static const char* const files[] = {"dlc.bin", "dlc2.bin"};
    Request r;
    u64 size = 0;
    u8* data = fs_read_file("exefs/main", &size);
    SvcObj* storage;
    if (update) { /* the update's ExeFS and RomFS, over the game's dump, whatever the game has extracted */
        CHECK(g_hle.fs.dump.patch.found && data && size == sizeof addon_code - 1 && !memcmp(data, addon_code, size));
        free(data);
        CHECK(fs_read_file("exefs/subsdk0", &size) == NULL);
        request(&r, 200);
        dispatch(proxy, &r, 0);
        CHECK(r.c.nout_objs == 1);
        storage = r.c.out_objs[0];
        image_holds(storage, "asset.bin", addon_asset);
        svcobj_unref(storage);
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
        request(&r, 202); /* the StorageId (u8) at 0, the DataId at 8 */
        argument64(&r, 8, 0x0100000000AB1001ULL + i);
        dispatch(proxy, &r, 0);
        CHECK(r.c.nout_objs == 1);
        storage = r.c.out_objs[0];
        image_holds(storage, files[i], dlc_assets[i]);
        svcobj_unref(storage);
    }
    request(&r, 202); /* the DLC without a dump: not installed, an empty archive */
    argument64(&r, 8, 0x0100000000AB1003ULL);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1 && get_size(r.c.out_objs[0]) == sizeof g_hle.fs.emptyrom);
    svcobj_unref(r.c.out_objs[0]);
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
static void test_mods(SvcObj* proxy, int image, int stale) {
    Request r;
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
    request(&r, 200);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    SvcObj* storage = r.c.out_objs[0];
    total = get_size(storage);
    CHECK(total > 0x200 && total <= 0x8000);
    test_storage_contracts(storage, total);
    request(&r, 0);
    argument64(&r, 8, total);
    output_buffer(&r, total);
    dispatch(storage, &r, 0);
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
        request(&r, 0);
        argument64(&r, 0, from);
        argument64(&r, 8, length);
        output_buffer(&r, length);
        dispatch(storage, &r, 0);
        CHECK(!memcmp(GP(0x5000), expect, (size_t)length) && !memcmp(GP(0x5000), "s a", 3) &&
              !memcmp(GP(0x5000) + length - z_size, "the game's z", 12));
    }
    svcobj_unref(storage);
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
    CHECK(!symlink("a.bin", "mods-data/0100000000AB0000_data/mods/3 off/romfs/link"));
#endif
    strcpy(g_exedir, "mods-data/");
    strcpy(g_exename, "0100000000AB0000");
    strcpy(g_gamename, "0100000000AB0000");
    strcpy(g_confdir, "mods-data/");
    strcpy(g_settings.game_mods_off, "3 off");
    mods = snx_mods_folder("mods-data/0100000000AB0000_data");
    stamp = snx_mods_stamp(mods, g_settings.game_mods_off);
    CHECK(strlen(stamp) == 16);
    text = stale ? snx_strdup("1") : snx_format("1 mods %s", stamp);
    CHECK(!snx_write_file("mods-data/0100000000AB0000.abi", text, strlen(text)));
    free(text);
    free(stamp);
    free(mods);
    strcpy(g_datadir, "mods-data/0100000000AB0000_data/");
}

/* OpenSaveDataFileSystem (51): the SaveDataSpaceId (u8) at 0 and the 0x40 byte SaveDataAttribute at 8, whose
   SaveDataType is at 0x20 in it, 0x28 in the request. The other bytes are filled with types that would pick another
   folder if they were read instead */
static void save_request(Request* r, u32 command, u8 type, const u64* uid) {
    request(r, command);
    r->raw[0] = 1;                           /* User */
    memset(r->raw + 8, 4, 0x20);             /* program id, uid and system save data id: Temporary */
    memset(r->raw + 0x29, 2, 0x48 - 0x29);   /* rank, index and reserved: Bcat */
    if (uid) { /* the attribute's user */
        snx_st_le64(r->raw + 0x10, uid[0]);
        snx_st_le64(r->raw + 0x18, uid[1]);
    }
    r->raw[0x28] = type;
}
static SvcObj* open_save_data(SvcObj* proxy, u32 command, u8 type) {
    Request r;
    save_request(&r, command, type, NULL);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1 && r.c.out_size == 0);
    return r.c.out_objs[0];
}
/* an Account save (51) of the user uid */
static SvcObj* open_user_save(SvcObj* proxy, const u64* uid) {
    Request r;
    save_request(&r, 51, 1, uid);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1 && r.c.out_size == 0);
    return r.c.out_objs[0];
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
/* the event notifiers (500, 501, 510) give the same event each time, never signaled, and drop it with themselves;
   OpenSaveDataTransferProhibiter (83) gives a prohibiter; OpenDataFileSystemWithProgramIndex (204) has no data
   filesystem for this program and no other program */
static void test_notifiers_and_program_index(SvcObj* proxy) {
    static const u32 notifiers[] = {500, 501, 510};
    Request r;
    for (unsigned i = 0; i < sizeof notifiers / sizeof notifiers[0]; i++) {
        request(&r, notifiers[i]);
        dispatch(proxy, &r, 0);
        CHECK(r.c.nout_objs == 1 && test_events == 1);
        SvcObj* notifier = r.c.out_objs[0];
        u32 handles[2];
        for (int k = 0; k < 2; k++) {
            request(&r, 0);
            dispatch(notifier, &r, 0);
            CHECK(r.c.nout_copy == 1);
            handles[k] = r.c.out_copy[0];
        }
        KEvent* event = test_handles[handles[0] - 0x100];
        CHECK(test_handles[handles[1] - 0x100] == event && !event->signaled);
        kobj_unref(&event->o); /* the game closes both handles */
        kobj_unref(&event->o);
        svcobj_unref(notifier);
        CHECK(test_events == 0);
    }
    request(&r, 83);
    argument64(&r, 0, g_hle.kernel.title_id);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    svcobj_unref(r.c.out_objs[0]);
    request(&r, 204);
    dispatch(proxy, &r, RC(2, 1001));
    CHECK(r.c.nout_objs == 0);
    request(&r, 204);
    r.raw[0] = 1;
    dispatch(proxy, &r, RC(2, 1002));
    puts("PASS event notifiers, the save data transfer prohibiter and the data filesystem by program index");
}

static void test_save_data(SvcObj* proxy) {
    /* each SaveDataType has its folder: System (0), Account (1), Bcat (2), Device (3) */
    SvcObj* system = open_save_data(proxy, 51, 0);
    CHECK(tree_has("save/system") && !tree_has("save/user"));
    SvcObj* bcat = open_save_data(proxy, 51, 2);
    SvcObj* device = open_save_data(proxy, 51, 3);
    CHECK(tree_has("save/bcat") && tree_has("save/device") && !tree_has("save/temp"));
    fs_command(system, 0, "/system.bin", 4, 0);
    fs_command(bcat, 7, "/system.bin", 0, FS_NOTFOUND);
    fs_command(device, 7, "/system.bin", 0, FS_NOTFOUND);
    CHECK(tree_has("save/system/system.bin"));
    fs_command(system, 1, "/system.bin", 0, 0);
    svcobj_unref(bcat);
    svcobj_unref(device);

    /* an Account save is its user's folder, save/user.<uid>: another user's has nothing to do with the first's */
    SvcObj* user = open_user_save(proxy, test_other_user);
    CHECK(tree_has(OTHER_SAVE) && !tree_has(FIRST_SAVE) && !tree_has("save/user") && tree_has("save/system"));
    fs_command(user, 0, "/other-user.bin", 4, 0);
    svcobj_unref(user);
    /* the first user's, with no save/user and save/system empty, is its folder, made then */
    user = open_user_save(proxy, g_user_id);
    CHECK(tree_has(FIRST_SAVE) && tree_has("save/system"));
    fs_command(user, 7, "/other-user.bin", 0, FS_NOTFOUND);
    svcobj_unref(user);
    CHECK(test_left_said == 0);
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_remove(&g_hle.fs.tree, NULL, FIRST_SAVE, 1, 1, 0));
    u32 rc;
    SvcObj* old = hostfs_new("save/user", &rc);
    pm_unlock(&g_hle.fs.tree.lock);
    CHECK(old && !rc);
    fs_command(old, 0, "/progress.bin", 4, 0);
    svcobj_unref(old);
    /* save/user, the one Account save of the versions before there were other users, is the first user's while it
       has no folder: played where it is, nothing moved, and said once a run with where it goes. Another user's save
       leaves it alone */
    user = open_user_save(proxy, test_other_user);
    svcobj_unref(user);
    CHECK(test_left_said == 0);
    user = open_user_save(proxy, g_user_id);
    fs_command(user, 7, "/progress.bin", 0, 0);
    fs_command(user, 0, "/played.bin", 4, 0);
    svcobj_unref(user);
    CHECK(tree_has("save/user/progress.bin") && tree_has("save/user/played.bin") && !tree_has(FIRST_SAVE));
    CHECK(test_left_said == 1 && strstr(test_left_text, "save/user, the first user's saves") &&
          strstr(test_left_text, FIRST_SAVE) && strstr(test_left_text, "DEPRECATIONS.md"));
    user = open_user_save(proxy, g_user_id);
    svcobj_unref(user);
    CHECK(test_left_said == 1);
    /* the player moves it to the first user's folder, which the save is from then */
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_rename(&g_hle.fs.tree, NULL, "save/user", FIRST_SAVE, 1));
    pm_unlock(&g_hle.fs.tree.lock);
    user = open_user_save(proxy, g_user_id);
    fs_command(user, 7, "/played.bin", 0, 0);
    svcobj_unref(user);
    CHECK(!tree_has("save/user"));
    /* saves an earlier version put in save/system (it read the type from a byte that is always 0) are the first
       user's while it has no folder and no save/user, played where they are */
    pm_lock(&g_hle.fs.tree.lock);
    CHECK(!fs_tree_remove(&g_hle.fs.tree, NULL, FIRST_SAVE, 1, 1, 0));
    pm_unlock(&g_hle.fs.tree.lock);
    fs_command(system, 0, "/slot.bin", 4, 0);
    user = open_user_save(proxy, g_user_id);
    fs_command(user, 7, "/slot.bin", 0, 0);
    svcobj_unref(user);
    CHECK(tree_has("save/system/slot.bin") && !tree_has(FIRST_SAVE) && test_left_said == 1);
    /* with its folder there, save/system is the System saves' alone */
    pm_lock(&g_hle.fs.tree.lock);
    old = hostfs_new(FIRST_SAVE, &rc);
    pm_unlock(&g_hle.fs.tree.lock);
    CHECK(old && !rc);
    svcobj_unref(old);
    fs_command(system, 1, "/slot.bin", 0, 0);
    fs_command(system, 0, "/other.bin", 4, 0);
    user = open_user_save(proxy, g_user_id);
    fs_command(user, 7, "/other.bin", 0, FS_NOTFOUND);
    fs_command(user, 0, "/progress.bin", 4, 0);
    svcobj_unref(user);
    CHECK(tree_has("save/system/other.bin") && tree_has(FIRST_SAVE "/progress.bin"));
    /* a save/user beside the first user's folder (an older version run again) is two saves of one user: the game
       stops, and both stay as they were, for the player to choose */
    pm_lock(&g_hle.fs.tree.lock);
    old = hostfs_new("save/user", &rc);
    pm_unlock(&g_hle.fs.tree.lock);
    CHECK(old && !rc);
    fs_command(old, 0, "/newer.bin", 4, 0);
    svcobj_unref(old);
    {
        jmp_buf jump;
        Request r;
        save_request(&r, 51, 1, g_user_id);
        test_fatal_text[0] = 0;
        test_fatal_jump = &jump;
        if (!setjmp(jump)) {
            dispatch(proxy, &r, 0);
            CHECK(!"opening the first user's save with two of them did not stop the game");
        }
        test_fatal_jump = NULL;
        CHECK(strstr(test_fatal_text, "Two saves of the first user") && strstr(test_fatal_text, FIRST_SAVE));
    }
    /* tree_has takes the tree's lock: fatal() came after open_save let it go, or this would wait forever */
    CHECK(tree_has("save/user/newer.bin") && tree_has(FIRST_SAVE "/progress.bin") &&
          !tree_has(FIRST_SAVE "/newer.bin"));
    /* a type past the known ones is a System save, and so is one opened by its system save data id (52) */
    SvcObj* unknown = open_save_data(proxy, 51, 0x7F);
    fs_command(unknown, 7, "/other.bin", 0, 0);
    svcobj_unref(unknown);
    SvcObj* by_id = open_save_data(proxy, 52, 0);
    fs_command(by_id, 7, "/other.bin", 0, 0);
    svcobj_unref(by_id);
    fs_command(system, 1, "/other.bin", 0, 0);
    svcobj_unref(system);

    /* IsSdCardAccessible (631): the card OpenSdCardFileSystem serves is always in */
    Request r;
    request(&r, 631);
    dispatch(proxy, &r, 0);
    u32 v;
    CHECK(r.c.out_size == 4);
    v = snx_ld_le32(r.c.out);
    CHECK(v == 1);
    /* GetProgramIndexForAccessLog (1011): program 0 of 1 */
    request(&r, 1011);
    dispatch(proxy, &r, 0);
    CHECK(r.c.out_size == 8);
    v = snx_ld_le32(r.c.out);
    CHECK(v == 0);
    v = snx_ld_le32(r.c.out + 4);
    CHECK(v == 1);
    puts(
        "PASS SaveDataType folders, a folder per user, save/user and save/system played where they are, two saves of one "
        "user "
        "refused, IsSdCardAccessible and the access log's program index");
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
        SvcObj* proxy = new_fsp();
        test_dump(proxy);
        svcobj_unref(proxy);
        CHECK(test_objects == 0);
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
        SvcObj* proxy = new_fsp();
        test_addon(proxy, update);
        svcobj_unref(proxy);
        CHECK(test_objects == 0);
        cleanup_service();
        free(g_mem);
        printf("PASS %u add-on checks\n", test_checks);
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
    if (argc == 4 && (!strcmp(argv[3], "mods") || !strcmp(argv[3], "mods-image") || !strcmp(argv[3], "mods-stale"))) {
        int image = !strcmp(argv[3], "mods-image"), stale = !strcmp(argv[3], "mods-stale");
        mods_write(image, stale);
        fs_init(NULL, NULL);
        test_scans_allowed = 0;
        svc_fs_init();
        romfs_init();
        SvcObj* proxy = new_fsp();
        test_mods(proxy, image, stale);
        svcobj_unref(proxy);
        CHECK(test_objects == 0);
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
    SvcObj* proxy = new_fsp();
    if (raw) {
        test_raw_romfs(proxy);
        svcobj_unref(proxy);
        CHECK(test_objects == 0);
        cleanup_service();
        free(g_mem);
        CHECK(!test_chdir(argv[1]));
        printf("PASS %u raw RomFS checks; %u startup scans\n", test_checks, test_scans);
        return 0;
    }
    Request r;
    request(&r, 18);
    dispatch(proxy, &r, 0);
    CHECK(r.c.nout_objs == 1);
    SvcObj* fs = r.c.out_objs[0];
    test_file_services(fs);
    test_file_contracts(fs);
    test_directory_path_contracts(fs);
    test_directory_services(fs);
    test_index_and_paths(fs);
    test_virtual_services(fs, proxy);
    test_extracted_romfs(proxy);
    test_save_data(proxy);
    test_notifiers_and_program_index(proxy);
    svcobj_unref(fs);
    svcobj_unref(proxy);
    CHECK(test_objects == 0);
    cleanup_service();
    free(g_mem);
    CHECK(!test_chdir(argv[1]));
    printf("PASS %u HLE checks; %u startup scans, none after initialization\n", test_checks, test_scans);
    return 0;
}
