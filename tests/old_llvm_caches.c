/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What only LLVM left in a user's folders (utilities/old_llvm_caches.h), in a cache folder made here: each kind found by
   its name's form and its format and key, and beside each a file of the same name and another format, or of the same
   format and another key, that stays. Found is not deleted; deleting takes what was found and is still of its kind,
   and nothing else */
#include "common/utilities/old_llvm_caches.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(ok, ...)                                                                                                 \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            printf("FAIL: ");                                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

#define HEX64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define HEX32 "0123456789abcdef0123456789abcdef"

static char* g_root;
static char* at(const char* folder, const char* name) {
    char* dir = snx_path_join(g_root, folder);
    char* path = dir ? snx_path_join(dir, name) : NULL;
    if (dir) snx_mkdirs(dir);
    free(dir);
    return path;
}
static void put(const char* folder, const char* name, const void* data, size_t size) {
    char* path = at(folder, name);
    CHECK(path && !snx_write_file(path, data, size), "writing %s/%s", folder, name);
    free(path);
}
static int there(const char* folder, const char* name) {
    char* path = at(folder, name);
    const int is = path && snx_is_file(path);
    free(path);
    return is;
}
/* an ELF64 little-endian header of type (1 relocatable, 3 shared), then text */
static size_t elf(unsigned char* out, unsigned type, const char* text) {
    memset(out, 0, 64);
    memcpy(out, "\177ELF\2\1\1", 7);
    out[16] = (unsigned char)type;
    out[18] = 62;
    const size_t n = strlen(text) + 1;
    memcpy(out + 64, text, n);
    return 64 + n;
}
/* a JIT cache: the magic, the key's length, the key */
static size_t jit(unsigned char* out, const char* key) {
    const size_t n = strlen(key);
    memcpy(out, "SNXJIT01", 8);
    out[8] = (unsigned char)n, out[9] = (unsigned char)(n >> 8), out[10] = out[11] = 0;
    memcpy(out + 12, key, n);
    memcpy(out + 12 + n, "records", 7);
    return 12 + n + 7;
}

int main(void) {
    static const char old_key[] =
        "abi=19 obj=5 llvm=22.1.8 triple=x86_64-pc-linux-gnu rtlib=3fc7f8c2b45fea22 cpu=znver4";
    static const char new_key[] = "abi=20 native=x86-64 rtlib=3fc7f8c2b45fea22 native_code=0123456789abcdef opt=2";
    unsigned char b[512];
    size_t n;
    g_root = snx_temp_dir(NULL);
    CHECK(g_root != NULL, "a folder");
    if (!g_root) return 1;
    /* units/: a relocatable object of the name's form; a text file of that name; an object of another name */
    put("units", HEX64 ".o", b, elf(b, 1, "snx_fp_host_binary"));
    put("units", "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210.o", "not an object", 13);
    put("units", "notes.o", b, elf(b, 1, "x"));
    /* linux-jit/: LLVM's runtime library and a module of LLVM's descriptor, a module of the native one, a module of
       both names (not LLVM's alone), the JIT's cache of LLVM's key */
    put("linux-jit", "rtlib-" HEX32 ".o", b, elf(b, 1, "snx_fp_env"));
    put("linux-jit", "rtlib-nothex0123456789abcdef0123456789.o", b, elf(b, 1, "snx_fp_env")); /* 32, not all hex */
    put("linux-jit", HEX32 ".so", b, elf(b, 3, "snx_lib_module"));
    put("linux-jit", "fedcba9876543210fedcba9876543210.so", b, elf(b, 3, "snx_lib_descriptor"));
    n = elf(b, 3, "snx_lib_module");
    memcpy(b + n, "snx_lib_descriptor", 19);
    put("linux-jit", "00000000000000000000000000000000.so", b, n + 19);
    put("linux-jit", "library-jit.cache", b, jit(b, old_key));
    /* android-jit/: the JIT's cache of the native key, which stays */
    put("android-jit", "library-jit.cache", b, jit(b, new_key));
    /* a game's .jitcache, each key */
    put("game", "old.jitcache", b, jit(b, old_key));
    put("game", "new.jitcache", b, jit(b, new_key));

    SnxLlvmCaches found = {0};
    CHECK(!snx_llvm_caches_scan(g_root, &found), "the scan");
    char* p = at("game", "old.jitcache");
    CHECK(snx_llvm_caches_jit(p, &found) == 1, "the game's old .jitcache");
    free(p);
    p = at("game", "new.jitcache");
    CHECK(snx_llvm_caches_jit(p, &found) == 0, "the game's new .jitcache is not LLVM's");
    free(p);
    unsigned kinds[SNX_LLVM_CACHE_KINDS] = {0};
    for (size_t i = 0; i < found.count; ++i) ++kinds[found.files[i].kind];
    CHECK(found.count == 5 && kinds[SNX_LLVM_CACHE_UNITS] == 1 && kinds[SNX_LLVM_CACHE_RTLIB] == 1 &&
              kinds[SNX_LLVM_CACHE_MODULE] == 1 && kinds[SNX_LLVM_CACHE_JIT] == 2,
          "found %zu: units %u, rtlib %u, modules %u, JIT caches %u", found.count, kinds[SNX_LLVM_CACHE_UNITS],
          kinds[SNX_LLVM_CACHE_RTLIB], kinds[SNX_LLVM_CACHE_MODULE], kinds[SNX_LLVM_CACHE_JIT]);
    uint64_t bytes = 0, all = 0;
    char* folder = snx_path_join(g_root, "linux-jit");
    const size_t in = snx_llvm_caches_in(&found, folder, &bytes);
    CHECK(in == 3 && bytes > 0, "linux-jit/'s: %zu files, %llu bytes", in, (unsigned long long)bytes);
    free(folder);
    for (size_t i = 0; i < found.count; ++i) all += found.files[i].bytes;
    CHECK(all == found.bytes, "the bytes add up (%llu, %llu)", (unsigned long long)all,
          (unsigned long long)found.bytes);
    /* one library folder alone, as a runtime reads its own, and the line it says of it: linux-jit/'s three; none in
       android-jit/, which says nothing; the game's .jitcache by its path */
    {
        SnxLlvmCaches lib = {0}, none = {0}, game = {0};
        char said[1024];
        char *dir = snx_path_join(g_root, "linux-jit"), *native = snx_path_join(g_root, "android-jit");
        char* old_jit = at("game", "old.jitcache");
        CHECK(!snx_llvm_caches_library(dir, &lib) && lib.count == 3, "linux-jit/ alone: %zu files, not 3", lib.count);
        CHECK(snx_llvm_caches_said(&lib, dir, SNX_LLVM_CACHES_WHERE, said, sizeof said) == 3 &&
                  !strncmp(said, "3 files, ", 9) && strstr(said, " MB of LLVM's old caches in ") && strstr(said, dir) &&
                  strstr(said, "which nothing reads now: the launcher deletes them in Settings > App > Move Old Files"),
              "linux-jit/ said: %s", said);
        CHECK(!snx_llvm_caches_library(native, &none) && !none.count &&
                  !snx_llvm_caches_said(&none, native, SNX_LLVM_CACHES_WHERE, said, sizeof said) && !said[0],
              "android-jit/ said: %s", said);
        CHECK(snx_llvm_caches_jit(old_jit, &game) == 1 &&
                  snx_llvm_caches_said(&game, NULL, SNX_LLVM_CACHE_JIT_WHERE, said, sizeof said) == 1 &&
                  !strncmp(said, "1 file, ", 8) && strstr(said, old_jit) && strstr(said, "Delete Old Caches"),
              "the .jitcache said: %s", said);
        snx_llvm_caches_free(&lib);
        snx_llvm_caches_free(&none);
        snx_llvm_caches_free(&game);
        free(dir);
        free(native);
        free(old_jit);
    }
    /* found, not deleted: every file is still there */
    CHECK(there("units", HEX64 ".o") && there("linux-jit", "rtlib-" HEX32 ".o") && there("linux-jit", HEX32 ".so"),
          "the scan deleted a file");

    /* one changed since it was found (the module now the native one's): it stays */
    put("linux-jit", HEX32 ".so", b, elf(b, 3, "snx_lib_descriptor"));
    char error[512];
    const size_t gone = snx_llvm_caches_delete(&found, &bytes, error, sizeof error);
    CHECK(gone == 4 && !error[0], "deleted %zu, not 4 (%s)", gone, error);
    CHECK(!there("units", HEX64 ".o") && !there("linux-jit", "rtlib-" HEX32 ".o") &&
              !there("linux-jit", "library-jit.cache") && !there("game", "old.jitcache"),
          "each found file of its kind gone");
    CHECK(there("linux-jit", HEX32 ".so"), "the module changed since it was found stays");
    CHECK(there("units", "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210.o") &&
              there("units", "notes.o") && there("linux-jit", "rtlib-nothex0123456789abcdef0123456789.o") &&
              there("linux-jit", "fedcba9876543210fedcba9876543210.so") &&
              there("linux-jit", "00000000000000000000000000000000.so") && there("android-jit", "library-jit.cache") &&
              there("game", "new.jitcache"),
          "a file not LLVM's went");
    snx_llvm_caches_free(&found);
    snx_remove_tree(g_root);
    free(g_root);
    if (!g_failures) printf("LLVM's old caches: found by format and key, and only those deleted\n");
    return g_failures != 0;
}
