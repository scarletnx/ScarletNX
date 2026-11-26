/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The build-side reader of the runtime JIT's cache: the profile run writes a unit,
   the reader hands the AOT build its metadata and object, and a wrong key or a
   cut-off record is refused instead of linked. A file of another build, an older
   LLVM's or one without LLVM's, is refused naming the field its key differs in. */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/cpu/recompiler/jit/jit_service.h"
#include "common/cpu/recompiler/mach/native.h"
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_LO 0x1000
#define CODE_HI 0x3000

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static unsigned char memory[0x4000];
static _Atomic GuestFn dispatch[(CODE_HI - CODE_LO) / 4];

static SnxJitService* profile(const char* path) {
    static const SnxU32 words[] = {0x91000400u, 0xD65F03C0u}; /* add x0, x0, #1; ret */
    static SnxJitConfig config;
    static SnxCodeImage image;
    char error[512];
    SnxJitService* service;
    memset(memory, 0, sizeof(memory));
    memcpy(memory + CODE_LO, words, sizeof(words));
    memset(dispatch, 0, sizeof(dispatch));
    memset(&image, 0, sizeof(image));
    image.image = memory;
    image.image_size = sizeof(memory);
    image.base = 0;
    image.text_off = CODE_LO;
    image.text_size = CODE_HI - CODE_LO;
    memset(&config, 0, sizeof(config));
    config.images = &image;
    config.image_count = 1;
    config.dispatch = dispatch;
    config.code_lo = CODE_LO;
    config.code_hi = CODE_HI;
    config.batch = 1;
    config.threads = 1;
    config.opt = '2';
    config.cache_path = path;
    service = snx_jit_service_new(&config, error, sizeof(error));
    if (!service) fprintf(stderr, "jit: %s\n", error);
    return service;
}

/* an empty cache at path, made with key: 1, 0 where it could not be written */
static int write_header(const char* path, const char* key) {
    const size_t n = strlen(key);
    const unsigned char length[4] = {(unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16),
                                     (unsigned char)(n >> 24)};
    FILE* f = fopen(path, "wb");
    int ok =
        f && fwrite(SNX_JIT_CACHE_MAGIC, 1, 8, f) == 8 && fwrite(length, 1, 4, f) == 4 && fwrite(key, 1, n, f) == n;
    if (f) ok = !fclose(f) && ok;
    return ok;
}
/* a cache of the key of another build, refused with an error that names its field and the build's */
static void refused_other(const char* path, const char* other, const char* key, const char* said) {
    char error[512] = "";
    CHECK(write_header(path, other));
    CHECK(snx_jit_cache_open(path, key, error, sizeof(error)) == NULL);
    CHECK(strstr(error, path) && strstr(error, "the cache key differs from the build's") && strstr(error, said));
    if (!strstr(error, said)) fprintf(stderr, "the error lacks \"%s\": %s\n", said, error);
}

static int copy_prefix(const char* from, const char* to, size_t size) {
    unsigned char* bytes = (unsigned char*)malloc(size ? size : 1);
    FILE* f = fopen(from, "rb");
    int ok = 0;
    if (bytes && f && fread(bytes, 1, size, f) == size) {
        fclose(f);
        f = fopen(to, "wb");
        if (f) {
            ok = fwrite(bytes, 1, size, f) == size;
            fclose(f);
        }
    } else if (f) {
        fclose(f);
    }
    free(bytes);
    return ok;
}

int main(void) {
    static const char* path = "snx-test-jit-cache-reader.bin";
    static const char* cut = "snx-test-jit-cache-reader-cut.bin";
    SnxJitService* service;
    SnxJitCache* cache;
    const SnxJitCacheRecord* record;
    char* key;
    char* wrong;
    char error[512];
    FILE* f;
    long size = -1;
    int ok = 0;

    remove(path);
    remove(cut);
    service = profile(path);
    CHECK(service != NULL);
    if (!service) return 1;
    ok = snx_jit_wait(service, CODE_LO) != NULL;
    CHECK(ok);
    snx_jit_service_free(service);
    CHECK(!snx_native_available() || !snx_ir_started()); /* the host's back end made the unit: LLVM never started */

    key = snx_jit_cache_key('2', 0, 0, 0);
    CHECK(key != NULL);
    CHECK(key && strstr(key, "fpsr=1") != NULL);
    if (key) {
        char expected[32];
        char* triple = LLVMGetDefaultTargetTriple();
        snprintf(expected, sizeof(expected), "obj=%d", SNX_IR_OBJECT_VERSION);
        CHECK(strstr(key, expected) != NULL);
        CHECK(triple != NULL && strstr(key, triple) != NULL);
        if (triple) LLVMDisposeMessage(triple);
    }
    if (key) {
        char* loose;
        snx_ir_fpsr_flags(0);
        loose = snx_jit_cache_key('2', 0, 0, 0);
        CHECK(loose != NULL && strstr(loose, "fpsr=0") != NULL && strcmp(key, loose));
        free(loose);
        snx_ir_fpsr_flags(1);
    }
    cache = snx_jit_cache_open(path, key, error, sizeof(error));
    CHECK(cache != NULL);
    if (cache) {
        CHECK(snx_jit_cache_count(cache) == 1);
        record = snx_jit_cache_record(cache, 0);
        CHECK(record != NULL);
        if (record) {
            CHECK(strncmp(record->name, "snx_jit_unit_", 13) == 0);
            CHECK(record->function_count == 1);
            CHECK(record->object_size > 4);
            CHECK(record->object != NULL);
            if (record->function_count == 1) {
                const SnxJitCacheFunction* function = &record->functions[0];
                CHECK(function->start == CODE_LO);
                CHECK(function->leader_count == 1);
                CHECK(function->leader_count == 1 && function->leaders[0] == CODE_LO);
                CHECK(function->run_count == 1);
                CHECK(function->run_count == 1 && function->runs[0].address == CODE_LO);
                CHECK(function->run_count == 1 && function->runs[0].words == 2);
                CHECK(function->h1 != 0 && function->h2 != 0);
            }
        }
        snx_jit_cache_close(cache);
    }
    wrong = snx_jit_cache_key('3', 0, 0, 0);
    CHECK(wrong != NULL);
    if (wrong) {
        error[0] = 0;
        CHECK(snx_jit_cache_open(path, wrong, error, sizeof(error)) == NULL);
        CHECK(strstr(error, "key") != NULL);
        CHECK(strstr(error, "made for opt=2, and this build makes opt=3") != NULL);
        free(wrong);
    }
    /* another guest's features decode other instructions: their code is another cache's */
    wrong = snx_jit_cache_key('2', 0, 0, ~(SnxU64)0);
    CHECK(wrong != NULL);
    if (wrong) {
        CHECK(snx_jit_cache_open(path, wrong, error, sizeof(error)) == NULL);
        free(wrong);
    }
    /* the file of an older LLVM's build, and the one of a build without LLVM, whose key has the native back end where
       this one's names LLVM */
    if (key) {
        static const char* old = "snx-test-jit-cache-reader-old.bin";
        const char* field = strstr(key, " llvm=");
        char *older = (char*)malloc(strlen(key) + 32), said[96], made[96];
        if (older && field) {
            const char* past = strchr(field + 1, ' ');
            snprintf(older, strlen(key) + 32, "%.*s llvm=22.1.0%s", (int)(field - key), key, past ? past : "");
            snprintf(said, sizeof(said), "made for llvm=22.1.0, and this build makes llvm=%s", LLVM_VERSION_STRING);
            refused_other(old, older, key, said);
            snprintf(made, sizeof(made), "abi=%d native=%s rtlib=0 opt=2 hook=0 svc=0", SNX_ABI, snx_native_name());
            snprintf(said, sizeof(said), "made for native=%s, and this build makes no native", snx_native_name());
            refused_other(old, made, key, said);
        } else
            CHECK(0);
        free(older);
        remove(old);
    }
    free(key);

    f = fopen(path, "rb");
    if (f && !fseek(f, 0, SEEK_END)) size = ftell(f);
    if (f) fclose(f);
    CHECK(size > 1);
    if (size > 1 && copy_prefix(path, cut, (size_t)size - 1)) {
        key = snx_jit_cache_key('2', 0, 0, 0);
        CHECK(key != NULL);
        if (key) {
            error[0] = 0;
            CHECK(snx_jit_cache_open(cut, key, error, sizeof(error)) == NULL);
            free(key);
        }
    } else {
        CHECK(0);
    }
    remove(path);
    remove(cut);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("jit_cache: ok");
    return 0;
}
