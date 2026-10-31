/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/unit_cache.h"
#include "common/cpu/recompiler/llvm/link.h"
#include <llvm-c/BitWriter.h>
#include <llvm/Config/llvm-config.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* the folder's path and its NUL, then the identity's bytes as each key hashes them first */
struct SnxUnitCache {
    char opt;
    SnxUnitDisk disk; /* the disk's room: snx_disk_room, or a test's */
    /* an object's room on the disk and its write, one unit at a time: two would take the same free bytes */
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    /* what the run's units met, said once and counted for the close: the units not cached and their bytes, the oldest
       removed for room (to leave reserve bytes free), the reused ones whose time could not be renewed (stale_why) */
    size_t left, trimmed, stale;
    SnxU64 left_bytes, trimmed_bytes, reserve;
    unsigned said; /* the reasons said, each its UNIT_SAID_ bit */
    char stale_why[256];
    /* the keys of the objects the caller reads back (snx_unit_cache_object's key), which room() never removes */
    SnxLinkNames held;
    size_t folder_size, identity_size;
    char bytes[];
};

static const char g_trailer_magic[8] = {'S', 'N', 'X', 'U', 'N', 'I', 'T', '1'};

static void put64(unsigned char* p, SnxU64 v) {
    int i;
    for (i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
/* a field of a key: its length, 8 bytes little-endian, then its bytes, so no two fields' bytes run together */
static int field(EVP_MD_CTX* ctx, const void* data, size_t size) {
    unsigned char length[8];
    put64(length, size);
    return EVP_DigestUpdate(ctx, length, sizeof length) == 1 && (!size || EVP_DigestUpdate(ctx, data, size) == 1);
}
static size_t put_field(unsigned char* p, const char* text) {
    size_t n = strlen(text);
    put64(p, n);
    memcpy(p + 8, text, n);
    return 8 + n;
}

void snx_unit_cache_llvm(char* out, size_t size) {
    unsigned major = 0, minor = 0, patch = 0;
    LLVMGetVersion(&major, &minor, &patch);
#ifdef SNX_LLVM_REVISION /* VCSRevision.h's, which CMake reads: the header is C++ */
    snprintf(out, size, "headers %s, library %u.%u.%u, revision %s", LLVM_VERSION_STRING, major, minor, patch,
             SNX_LLVM_REVISION);
#else
    snprintf(out, size, "headers %s, library %u.%u.%u", LLVM_VERSION_STRING, major, minor, patch);
#endif
}

SnxUnitCache* snx_unit_cache_open(const char* folder, const SnxUnitIdentity* identity, char* error, size_t error_size) {
    static const char tag[] = "snx-unit-cache";
    const char* tuning = identity->tuning ? identity->tuning : "";
    size_t folder_size = strlen(folder) + 1, identity_size, at;
    SnxUnitCache* cache;
    unsigned char* p;
    char reason[256];
    if (snx_mkdirs(folder)) {
        snprintf(error, error_size, "cannot make the folder %s: %s", folder, snx_os_error(reason, sizeof reason));
        return NULL;
    }
    identity_size = 8 + strlen(tag) + 8 + 8 + strlen(identity->llvm) + 2 + 8 + strlen(tuning);
    cache = (SnxUnitCache*)snx_alloc(sizeof *cache + folder_size + identity_size);
    cache->opt = identity->opt;
    cache->disk = snx_disk_room;
#ifdef _WIN32
    InitializeCriticalSection(&cache->lock);
#else
    pthread_mutex_init(&cache->lock, NULL);
#endif
    cache->left = cache->trimmed = cache->stale = 0;
    cache->said = 0;
    cache->left_bytes = cache->trimmed_bytes = cache->reserve = 0;
    cache->stale_why[0] = 0;
    memset(&cache->held, 0, sizeof cache->held);
    cache->folder_size = folder_size;
    cache->identity_size = identity_size;
    memcpy(cache->bytes, folder, folder_size);
    p = (unsigned char*)cache->bytes + folder_size;
    at = put_field(p, tag);
    put64(p + at, SNX_IR_OBJECT_VERSION);
    at += 8;
    at += put_field(p + at, identity->llvm);
    p[at++] = (unsigned char)identity->opt;
    p[at++] = identity->tuning != NULL; /* LLVM's own settings are not an empty text */
    put_field(p + at, tuning);
    return cache;
}

void snx_unit_cache_close(SnxUnitCache* cache) {
    if (!cache) return;
    if (cache->trimmed)
        printf("unit cache: %zu oldest units removed (%.1f MB) to leave %.1f GB free on %s\n", cache->trimmed,
               (double)cache->trimmed_bytes / 1e6, (double)cache->reserve / 1e9, cache->bytes);
    if (cache->stale)
        snx_error("unit cache: cannot set the time of %zu reused units (%s): the size limit sees them as never reused",
                  cache->stale, cache->stale_why);
    if (cache->left)
        snx_error("unit cache: %zu unit(s) (%.1f MB) not cached in %s", cache->left, (double)cache->left_bytes / 1e6,
                  cache->bytes);
#ifdef _WIN32
    DeleteCriticalSection(&cache->lock);
#else
    pthread_mutex_destroy(&cache->lock);
#endif
    snx_link_names_free(&cache->held);
    free(cache);
}

void snx_unit_cache_disk(SnxUnitCache* cache, SnxUnitDisk disk) { cache->disk = disk; }

const char* snx_unit_cache_folder(const SnxUnitCache* cache) { return cache->bytes; }

int snx_unit_cache_key(const SnxUnitCache* cache, const char* triple, const char* cpu, const char* features,
                       const void* bitcode, size_t size, char key[65]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char hash[32];
    unsigned length = 0, i;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, cache->bytes + cache->folder_size, cache->identity_size) == 1 &&
             field(ctx, triple, strlen(triple)) && field(ctx, cpu, strlen(cpu)) &&
             field(ctx, features, strlen(features)) && field(ctx, bitcode, size) &&
             EVP_DigestFinal_ex(ctx, hash, &length) == 1 && length == sizeof hash;
    EVP_MD_CTX_free(ctx);
    if (!ok) return -1;
    for (i = 0; i < sizeof hash; ++i) {
        key[2 * i] = digits[hash[i] >> 4];
        key[2 * i + 1] = digits[hash[i] & 15];
    }
    key[64] = 0;
    return 0;
}

/* the SHA-256 that ends a kept object: of its key's digits, then of the object */
static int check_hash(const char* key, const unsigned char* object, size_t size, unsigned char hash[32]) {
    unsigned length = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 && EVP_DigestUpdate(ctx, key, 64) == 1 &&
             EVP_DigestUpdate(ctx, object, size) == 1 && EVP_DigestFinal_ex(ctx, hash, &length) == 1 && length == 32;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* 0 when file (size bytes) is a whole object kept under key, its object's size in *object; else why */
static int whole(const unsigned char* file, size_t size, const char* key, size_t* object, char* why, size_t why_size) {
    unsigned char hash[32];
    const unsigned char* trailer;
    if (size < SNX_UNIT_TRAILER) {
        snprintf(why, why_size, "%zu bytes, fewer than its %d-byte check", size, SNX_UNIT_TRAILER);
        return -1;
    }
    trailer = file + size - SNX_UNIT_TRAILER;
    if (memcmp(trailer, g_trailer_magic, sizeof g_trailer_magic)) {
        snprintf(why, why_size, "%zu bytes, which do not end in the SNXUNIT1 check", size);
        return -1;
    }
    if (check_hash(key, file, size - SNX_UNIT_TRAILER, hash)) {
        snprintf(why, why_size, "%zu bytes, which OpenSSL cannot hash", size);
        return -1;
    }
    if (memcmp(hash, trailer + sizeof g_trailer_magic, sizeof hash)) {
        snprintf(why, why_size, "%zu bytes, whose SHA-256 is not the one its check holds", size);
        return -1;
    }
    *object = size - SNX_UNIT_TRAILER;
    return 0;
}

static unsigned long process_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

int snx_unit_cache_keep(const SnxUnitCache* cache, const char* key, const unsigned char* object, size_t size,
                        char* error, size_t error_size) {
    unsigned char trailer[SNX_UNIT_TRAILER];
    char *name = snx_format("%s.o", key), *path = snx_path_join(snx_unit_cache_folder(cache), name), reason[256];
    /* another process's write of the same unit goes to a name of its own */
    char* tmp = snx_format("%s.%lu.tmp", path, process_id());
    FILE* file = NULL;
    int rc = -1, written;
    memcpy(trailer, g_trailer_magic, sizeof g_trailer_magic);
    if (check_hash(key, object, size, trailer + sizeof g_trailer_magic))
        snprintf(error, error_size, "cannot keep %s: OpenSSL cannot hash it", path);
    else if (!(file = snx_fopen(tmp, "wb")))
        snprintf(error, error_size, "cannot write %s: %s", tmp, snx_os_error(reason, sizeof reason));
    else {
        written = fwrite(object, 1, size, file) == size && fwrite(trailer, 1, sizeof trailer, file) == sizeof trailer;
        if (!written) snx_os_error(reason, sizeof reason);
        if (fclose(file) && written) {
            written = 0;
            snx_os_error(reason, sizeof reason);
        }
        if (!written)
            snprintf(error, error_size, "cannot write %s: %s", tmp, reason);
        else if (snx_replace(tmp, path))
            snprintf(error, error_size, "cannot rename %s to %s: %s", tmp, path, snx_os_error(reason, sizeof reason));
        else
            rc = 0;
        if (rc && snx_path_exists(tmp)) snx_remove_tree(tmp);
    }
    free(tmp);
    free(path);
    free(name);
    return rc;
}

static int trim_files(const char* folder, SnxU64 limit, SnxU64 at_least, const char* why, const SnxLinkNames* held,
                      SnxUnitTrim* trim, char* error, size_t error_size);
#define GB(bytes) ((double)(bytes) / 1e9)
#define MB(bytes) ((double)(bytes) / 1e6)
/* a unit of size bytes not cached: counted for the close */
static void not_cached(SnxUnitCache* cache, SnxU64 size) {
    ++cache->left;
    cache->left_bytes += size;
}
/* why a unit is not cached, each said for the first unit it stops */
enum { UNIT_SAID_DISK = 1, UNIT_SAID_TRIM = 2, UNIT_SAID_ROOM = 4, UNIT_SAID_KEEP = 8 };
static int first_said(SnxUnitCache* cache, unsigned reason) {
    if (cache->said & reason) return 0;
    cache->said |= reason;
    return 1;
}
/* room for a kept object of size bytes on the cache's disk, which always keeps a tenth of itself free: the cache's
   oldest objects removed for it when it is short, never one the caller reads back (held). 0 to keep it; -1 when there
   is none, each reason said for the first unit it stops and the units counted for the close */
static int room(SnxUnitCache* cache, SnxU64 size) {
    const char* folder = snx_unit_cache_folder(cache);
    SnxU64 free_bytes = 0, total = 0, reserve, need;
    SnxUnitTrim trim;
    char reason[256], error[512];
    if (cache->disk(folder, &free_bytes, &total)) {
        if (first_said(cache, UNIT_SAID_DISK))
            snx_error("unit cache: cannot read the free space of %s (%s): units are not cached", folder,
                      snx_os_error(reason, sizeof reason));
        not_cached(cache, size);
        return -1;
    }
    reserve = total / 10;
    if (free_bytes >= reserve + size) return 0;
    need = reserve + size - free_bytes;
    if (trim_files(folder, 0, need, "the disk's tenth kept free", &cache->held, &trim, error, sizeof error)) {
        if (first_said(cache, UNIT_SAID_TRIM)) snx_error("unit cache: %s: units are not cached", error);
        not_cached(cache, size);
        return -1;
    }
    cache->trimmed += trim.removed;
    cache->trimmed_bytes += trim.removed_bytes;
    cache->reserve = reserve;
    if (trim.removed_bytes < need) {
        if (first_said(cache, UNIT_SAID_ROOM))
            snx_error("unit cache: %.1f GB free on %s, under the %.1f GB (a tenth of %.1f GB) it leaves free: units "
                      "without room are not cached",
                      GB(free_bytes), folder, GB(reserve), GB(total));
        not_cached(cache, size);
        return -1;
    }
    return 0;
}

/* key's object held for the caller to read back: room(), on any unit's thread, removes it no more */
static void hold(SnxUnitCache* cache, const char* key) {
#ifdef _WIN32
    EnterCriticalSection(&cache->lock);
#else
    pthread_mutex_lock(&cache->lock);
#endif
    snx_link_insert(&cache->held, key, 0);
#ifdef _WIN32
    LeaveCriticalSection(&cache->lock);
#else
    pthread_mutex_unlock(&cache->lock);
#endif
}

unsigned char* snx_unit_cache_read(const SnxUnitCache* cache, const char* key, size_t* size, char* error,
                                   size_t error_size) {
    char *name = snx_format("%s.o", key), *path = snx_path_join(snx_unit_cache_folder(cache), name), why[512];
    size_t file_size = 0, object_size = 0;
    unsigned char* file = snx_read_file(path, &file_size);
    if (!file)
        snprintf(error, error_size, "cannot read %s: %s", path, snx_os_error(why, sizeof why));
    else if (whole(file, file_size, key, &object_size, why, sizeof why)) {
        snprintf(error, error_size, "%s is not a whole object (%s)", path, why);
        free(file);
        file = NULL;
    } else
        *size = object_size;
    free(path);
    free(name);
    return file;
}

int snx_unit_cache_object(SnxUnitCache* cache, SnxIr* ir, unsigned char** data, size_t* size, int* reused, char* kept,
                          char* error, size_t error_size) {
    char key[65], why[512], reason[256], *triple, *cpu, *features, *name, *path;
    LLVMMemoryBufferRef bitcode;
    unsigned char* file;
    size_t file_size = 0, object_size = 0, length;
    const char* unit = LLVMGetModuleIdentifier(ir->mod, &length); /* code_NNN */
    int rc;
    *data = NULL;
    *size = 0;
    *reused = 0;
    if (kept) kept[0] = 0;
    /* snx_ir_object refuses a unit with an instruction no emitter covers, which nothing is looked up for */
    if (ir->fallbacks) return snx_ir_object(ir, cache->opt, data, size, error, error_size);
    bitcode = LLVMWriteBitcodeToMemoryBuffer(ir->mod);
    triple = LLVMGetTargetMachineTriple(ir->tm);
    cpu = LLVMGetTargetMachineCPU(ir->tm);
    features = LLVMGetTargetMachineFeatureString(ir->tm);
    rc = bitcode ? snx_unit_cache_key(cache, triple, cpu, features, LLVMGetBufferStart(bitcode),
                                      LLVMGetBufferSize(bitcode), key)
                 : -1;
    if (bitcode) LLVMDisposeMemoryBuffer(bitcode);
    LLVMDisposeMessage(triple);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    if (rc) {
        snprintf(error, error_size, "unit cache: cannot make the key of %s: %s", unit,
                 bitcode ? "OpenSSL cannot hash its IR" : "LLVM cannot write its IR as bitcode");
        return -1;
    }
    name = snx_format("%s.o", key);
    path = snx_path_join(snx_unit_cache_folder(cache), name);
    if (kept) hold(cache, key); /* before it is read, so that it is still there when it is read back */
    file = snx_read_file(path, &file_size);
    if (!file) snx_os_error(reason, sizeof reason);
    if (file && !whole(file, file_size, key, &object_size, why, sizeof why)) {
        *data = file;
        *size = object_size;
        *reused = 1;
        if (kept) memcpy(kept, key, sizeof key);
        /* the size limit removes the objects reused longest ago first */
        if (snx_set_file_time(path, (SnxU64)time(NULL))) {
            snx_os_error(reason, sizeof reason);
#ifdef _WIN32
            EnterCriticalSection(&cache->lock);
#else
            pthread_mutex_lock(&cache->lock);
#endif
            if (!cache->stale++) snprintf(cache->stale_why, sizeof cache->stale_why, "%s: %s", path, reason);
#ifdef _WIN32
            LeaveCriticalSection(&cache->lock);
#else
            pthread_mutex_unlock(&cache->lock);
#endif
        }
        free(path);
        free(name);
        return 0;
    }
    if (file)
        snx_error("unit cache: %s is not a whole object (%s): %s is compiled again", path, why, unit);
    else if (snx_path_exists(path))
        snx_error("unit cache: cannot read %s (%s): %s is compiled again", path, reason, unit);
    free(file);
    rc = snx_ir_object(ir, cache->opt, data, size, error, error_size);
    if (!rc) {
#ifdef _WIN32
        EnterCriticalSection(&cache->lock);
#else
        pthread_mutex_lock(&cache->lock);
#endif
        if (!room(cache, (SnxU64)*size + SNX_UNIT_TRAILER)) {
            if (!snx_unit_cache_keep(cache, key, *data, *size, why, sizeof why)) {
                if (kept) memcpy(kept, key, sizeof key);
            } else {
                if (first_said(cache, UNIT_SAID_KEEP))
                    snx_error("unit cache: %s: this run's units are not cached", why);
                not_cached(cache, (SnxU64)*size + SNX_UNIT_TRAILER);
            }
        }
#ifdef _WIN32
        LeaveCriticalSection(&cache->lock);
#else
        pthread_mutex_unlock(&cache->lock);
#endif
    }
    free(path);
    free(name);
    return rc;
}

/* a file the cache writes: 64 hex digits and ".o", or that and ".<process>.tmp", a write that has not ended */
static int cache_file(const char* name) {
    size_t i;
    for (i = 0; i < 64; ++i)
        if (!name[i] || !strchr("0123456789abcdef", name[i])) return 0;
    if (strncmp(name + 64, ".o", 2)) return 0;
    if (!name[66]) return 1;
    if (name[66] != '.') return 0;
    for (i = 67; name[i] >= '0' && name[i] <= '9'; ++i) {}
    return i > 67 && !strcmp(name + i, ".tmp");
}

typedef struct {
    SnxU64 time, size;
    size_t name; /* its index in the listing, which snx_list_dir sorts by name */
} TrimFile;
static int older(const void* a, const void* b) {
    const TrimFile *x = (const TrimFile*)a, *y = (const TrimFile*)b;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    return x->name < y->name ? -1 : x->name > y->name;
}

/* the cache's files in folder held to limit bytes (0: none), or at_least bytes of them removed, the oldest first; why
   names the removal in a failure's line. The objects of the keys in held (NULL: none) are left out, and nothing is
   removed when the others hold fewer than at_least */
static int trim_files(const char* folder, SnxU64 limit, SnxU64 at_least, const char* why, const SnxLinkNames* held,
                      SnxUnitTrim* trim, char* error, size_t error_size) {
    char **names = NULL, reason[256];
    size_t count = 0, i;
    TrimFile* files;
    SnxU64 left;
    int go;
    memset(trim, 0, sizeof *trim);
    if (snx_list_dir(folder, &names, &count)) {
        snprintf(error, error_size, "cannot list %s: %s", folder, snx_os_error(reason, sizeof reason));
        return -1;
    }
    files = (TrimFile*)snx_alloc((count ? count : 1) * sizeof *files);
    for (i = 0; i < count; ++i) {
        char* path;
        size_t found;
        if (!cache_file(names[i])) continue;
        if (held && !names[i][66]) {
            char key[65];
            memcpy(key, names[i], 64);
            key[64] = 0;
            if (snx_link_find(held, key, &found)) continue;
        }
        path = snx_path_join(folder, names[i]);
        if (snx_is_file(path)) {
            files[trim->files].time = snx_file_time(path);
            files[trim->files].size = snx_file_size(path);
            files[trim->files].name = i;
            trim->held += files[trim->files++].size;
        }
        free(path);
    }
    left = trim->held;
    go = (limit && left > limit) || (at_least && trim->held >= at_least);
    if (go) qsort(files, trim->files, sizeof *files, older);
    for (i = 0; go && ((limit && left > limit) || trim->removed_bytes < at_least) && i < trim->files; ++i) {
        char* path = snx_path_join(folder, names[files[i].name]);
        if (snx_remove_tree(path)) {
            ++trim->failed;
            snx_error("unit cache: cannot remove %s (%s), which %s removes", path, snx_os_error(reason, sizeof reason),
                      why);
        } else {
            left -= files[i].size;
            ++trim->removed;
            trim->removed_bytes += files[i].size;
        }
        free(path);
    }
    free(files);
    snx_free_strings(names, count);
    return 0;
}

int snx_unit_cache_trim(const char* folder, SnxU64 limit, SnxUnitTrim* trim, char* error, size_t error_size) {
    char why[64];
    snprintf(why, sizeof why, "a size limit of %llu bytes", (unsigned long long)limit);
    return trim_files(folder, limit, 0, why, NULL, trim, error, error_size);
}
void snx_unit_cache_report(FILE* out, const char* folder, SnxU64 limit, size_t reused, size_t count, SnxU64 bytes,
                           int quiet) {
    SnxUnitTrim trim;
    char error[512];
    if (!quiet)
        fprintf(out, "unit cache: %zu of %zu units reused (%.1f MB) from %s\n", reused, count,
                (double)bytes / 1000000.0, folder);
    if (!limit) return;
    if (snx_unit_cache_trim(folder, limit, &trim, error, sizeof error))
        snx_error("unit cache: %s: not cut to its %.1f MB limit", error, (double)limit / 1000000.0);
    else if (trim.removed && !quiet)
        fprintf(out,
                "unit cache: %zu files held %.1f MB, over the limit of %.1f MB: the %zu oldest removed (%.1f MB), %.1f "
                "MB left\n",
                trim.files, (double)trim.held / 1000000.0, (double)limit / 1000000.0, trim.removed,
                (double)trim.removed_bytes / 1000000.0, (double)(trim.held - trim.removed_bytes) / 1000000.0);
}
