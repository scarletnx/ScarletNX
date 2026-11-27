/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What only LLVM left in a user's folders (old_llvm_caches.h): each file by its name's form, then its format and key */
#include "common/utilities/old_llvm_caches.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JIT_MAGIC "SNXJIT01" /* jit/jit_cache.h's: the magic, a 32-bit little-endian key length, the key */

static int hex_run(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
/* name is n hex digits then end, after prefix */
static int named(const char* name, const char* prefix, size_t n, const char* end) {
    const size_t p = strlen(prefix), e = strlen(end);
    return strlen(name) == p + n + e && !strncmp(name, prefix, p) && hex_run(name + p, n) && !strcmp(name + p + n, end);
}
static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
#ifdef _WIN32
    const char* back = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
#endif
    return slash ? slash + 1 : path;
}
/* the first n bytes of path, how many were read (0 when it cannot be opened) */
static size_t head(const char* path, unsigned char* out, size_t n) {
    FILE* f = snx_fopen(path, "rb");
    if (!f) return 0;
    const size_t got = fread(out, 1, n, f);
    fclose(f);
    return got;
}
/* a relocatable object: ELF's ET_REL, or a COFF object of x86-64 or ARM64 (a machine first, not MZ) */
static int relocatable(const char* path) {
    unsigned char h[20];
    const size_t n = head(path, h, sizeof h);
    if (n >= 18 && !memcmp(h, "\177ELF", 4)) return (h[5] == 2 ? h[16] << 8 | h[17] : h[17] << 8 | h[16]) == 1;
    if (n >= 20) {
        const unsigned machine = h[0] | h[1] << 8;
        return machine == 0x8664 || machine == 0xAA64;
    }
    return 0;
}
/* whether path holds a, and whether b, anywhere: each found in 64 KiB pieces, a piece's last bytes kept for the next */
static void holds(const char* path, const char* a, int* has_a, const char* b, int* has_b) {
    unsigned char buf[65536 + 64];
    const size_t la = strlen(a) + 1, lb = strlen(b) + 1, keep = (la > lb ? la : lb) - 1;
    size_t have = 0;
    FILE* f = snx_fopen(path, "rb");
    *has_a = *has_b = 0;
    if (!f) return;
    for (;;) {
        const size_t got = fread(buf + have, 1, 65536, f);
        if (!got) break;
        have += got;
        for (size_t i = 0; i + la <= have; ++i)
            if (!memcmp(buf + i, a, la)) *has_a = 1;
        for (size_t i = 0; i + lb <= have; ++i)
            if (!memcmp(buf + i, b, lb)) *has_b = 1;
        if (have > keep) memmove(buf, buf + have - keep, keep), have = keep;
    }
    fclose(f);
}
/* a JIT cache whose key is LLVM's: the magic, then a key that begins abi= and names llvm= */
static int jit_llvm(const char* path) {
    unsigned char h[12];
    if (head(path, h, sizeof h) != sizeof h || memcmp(h, JIT_MAGIC, 8)) return 0;
    const size_t length = (size_t)h[8] | (size_t)h[9] << 8 | (size_t)h[10] << 16 | (size_t)h[11] << 24;
    if (length < 8 || length > 4096) return 0;
    unsigned char* key = (unsigned char*)malloc(12 + length + 1);
    if (!key) return 0;
    const int ok = head(path, key, 12 + length) == 12 + length;
    key[12 + length] = 0;
    const char* text = (const char*)key + 12;
    const int llvm = ok && !strncmp(text, "abi=", 4) && strstr(text, " llvm=") != NULL;
    free(key);
    return llvm;
}

int snx_llvm_cache_is(const char* path, SnxLlvmCacheKind kind) {
    const char* name = base_name(path);
    int module = 0, descriptor = 0;
    unsigned char h[4];
    switch (kind) {
        case SNX_LLVM_CACHE_UNITS: return named(name, "", 64, ".o") && relocatable(path);
        case SNX_LLVM_CACHE_RTLIB: return named(name, "rtlib-", 32, ".o") && relocatable(path);
        case SNX_LLVM_CACHE_MODULE:
            if (!named(name, "", 32, ".so") && !named(name, "", 32, ".dll") && !named(name, "", 32, ".le.objs") &&
                !named(name, "", 32, ".be.objs"))
                return 0;
            if (head(path, h, 4) != 4) return 0;
            if (memcmp(h, "\177ELF", 4) && memcmp(h, "MZ", 2) && !strstr(name, ".objs")) return 0;
            holds(path, "snx_lib_module", &module, "snx_lib_descriptor", &descriptor);
            return module && !descriptor;
        case SNX_LLVM_CACHE_JIT: return jit_llvm(path);
        default: return 0;
    }
}

static int add(SnxLlvmCaches* found, char* path, SnxLlvmCacheKind kind) {
    SnxLlvmCache* grown = (SnxLlvmCache*)realloc(found->files, (found->count + 1) * sizeof *grown);
    if (!grown) {
        free(path);
        return -1;
    }
    found->files = grown;
    grown[found->count].path = path;
    grown[found->count].bytes = snx_file_size(path);
    grown[found->count].kind = kind;
    found->bytes += grown[found->count].bytes;
    ++found->count;
    return 0;
}
/* folder's files of the kinds asked, in found */
static int scan(const char* folder, const SnxLlvmCacheKind* kinds, size_t kind_count, SnxLlvmCaches* found) {
    char** names = NULL;
    size_t count = 0;
    int rc = 0;
    if (!snx_is_dir(folder) || snx_list_dir(folder, &names, &count)) return 0;
    for (size_t i = 0; !rc && i < count; ++i) {
        char* path = snx_path_join(folder, names[i]);
        size_t k = 0;
        if (!path) {
            rc = -1;
            break;
        }
        while (k < kind_count && !(snx_is_file(path) && snx_llvm_cache_is(path, kinds[k]))) ++k;
        if (k < kind_count)
            rc = add(found, path, kinds[k]);
        else
            free(path);
    }
    snx_free_strings(names, count);
    return rc;
}

int snx_llvm_caches_library(const char* folder, SnxLlvmCaches* found) {
    static const SnxLlvmCacheKind library[] = {SNX_LLVM_CACHE_RTLIB, SNX_LLVM_CACHE_MODULE, SNX_LLVM_CACHE_JIT};
    return scan(folder, library, sizeof library / sizeof *library, found);
}

int snx_llvm_caches_scan(const char* cache_root, SnxLlvmCaches* found) {
    static const SnxLlvmCacheKind units[] = {SNX_LLVM_CACHE_UNITS};
    static const char* const libraries[] = {"linux-jit", "android-jit"};
    char* folder = snx_path_join(cache_root, "units");
    int rc = folder ? scan(folder, units, 1, found) : -1;
    free(folder);
    for (size_t i = 0; !rc && i < sizeof libraries / sizeof *libraries; ++i) {
        folder = snx_path_join(cache_root, libraries[i]);
        rc = folder ? snx_llvm_caches_library(folder, found) : -1;
        free(folder);
    }
    return rc;
}

int snx_llvm_caches_jit(const char* path, SnxLlvmCaches* found) {
    if (!snx_is_file(path) || !snx_llvm_cache_is(path, SNX_LLVM_CACHE_JIT)) return 0;
    char* copy = snx_strdup(path);
    return copy && !add(found, copy, SNX_LLVM_CACHE_JIT) ? 1 : -1;
}

size_t snx_llvm_caches_in(const SnxLlvmCaches* found, const char* folder, uint64_t* bytes) {
    const size_t n = strlen(folder);
    size_t count = 0;
    *bytes = 0;
    for (size_t i = 0; i < found->count; ++i) {
        const char* p = found->files[i].path;
        if (!strncmp(p, folder, n) && (p[n] == '/' || p[n] == '\\') && !strchr(p + n + 1, '/'))
            ++count, *bytes += found->files[i].bytes;
    }
    return count;
}

size_t snx_llvm_caches_delete(const SnxLlvmCaches* found, uint64_t* bytes, char* error, size_t error_size) {
    size_t gone = 0;
    *bytes = 0;
    if (error_size) error[0] = 0;
    for (size_t i = 0; i < found->count; ++i) {
        const SnxLlvmCache* c = &found->files[i];
        if (!snx_llvm_cache_is(c->path, c->kind)) continue; /* changed since it was found: not LLVM's now */
        if (snx_remove_tree(c->path)) {
            char why[256];
            if (error_size && !error[0]) snprintf(error, error_size, "%s: %s", c->path, snx_os_error(why, sizeof why));
            continue;
        }
        ++gone, *bytes += c->bytes;
    }
    return gone;
}

size_t snx_llvm_caches_said(const SnxLlvmCaches* found, const char* folder, const char* where, char* out, size_t size) {
    uint64_t bytes = found->bytes;
    const size_t count = folder ? snx_llvm_caches_in(found, folder, &bytes) : found->count;
    if (size) out[0] = 0;
    if (!count) return 0;
    snprintf(out, size, "%zu file%s, %.1f MB of LLVM's old caches in %s, which nothing reads now: %s", count,
             count == 1 ? "" : "s", (double)bytes / 1e6, folder ? folder : found->files[0].path, where);
    return count;
}

void snx_llvm_caches_free(SnxLlvmCaches* found) {
    for (size_t i = 0; i < found->count; ++i) free(found->files[i].path);
    free(found->files);
    found->files = NULL;
    found->count = 0;
    found->bytes = 0;
}
