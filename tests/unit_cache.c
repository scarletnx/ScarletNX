/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The code units' cache (nx/loader/unit_cache.c) under the LLVM module build: a program of two functions, each a unit
   of its own, built into a cache folder of the test's. The second build takes both units from the cache and links the
   same module byte for byte; a program whose second function differs compiles that unit alone; an object planted
   under a unit's key is what the module runs (the build read it, not compiled); a cut, altered or short object is
   compiled again and said, and so is a whole object under another unit's key; a reuse renews the file's time, and
   the size limit removes the oldest files, never another file of the folder. Each field of the key changes it, and
   the key of fixed fields is pinned. A pack for another machine (here this host's triple) is written a record at a
   time, the bytes of snx_objects_pack, with the units the cache holds read back from it, and each failure says what
   failed and why; a unit the cache holds for the pack stays there while the disk's room is made */
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/common.h"
#include "nx/loader/loader.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_llvm.h"
#include "nx/loader/unit_cache.h"
#include "common/cpu/recompiler/llvm/objects_pack.h"
#include <errno.h>
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h> /* dup, dup2: stdout caught in a file */
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#define TEXT_OFF  0x1000
#define TEXT_SIZE 0x1000
#define F1        0x10 /* the second function's offset in the text */

/* the failures, the program's memory, and what snx_error said since the last forget() */
static struct {
    int failures;
    unsigned char memory[TEXT_OFF + TEXT_SIZE];
    char said[16][1024];
    int said_count;
    size_t wrappers; /* the last build's snx_aot_ wrappers */
    /* a disk of disk_total bytes with disk_free free, when disk is set; its room cannot be read the next disk_fails
       times */
    SnxUnitDisk disk;
    SnxU64 disk_free, disk_total;
    int disk_fails;
    int disk_plenty; /* the next calls answered with the whole disk free */
    /* a file cut to disk_cut bytes (0: removed) at the next call, as another process may between a unit's compile and
       the pack's write */
    char disk_touch[512];
    size_t disk_cut;
    const char* triple; /* the next builds' SnxLlvmBuild.triple: a pack */
} g_unit_cache;

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++g_unit_cache.failures;                                                                                   \
        }                                                                                                              \
    } while (0)

static void sink(const char* line) {
    fprintf(stderr, "said: %s\n", line);
    if (g_unit_cache.said_count < 16)
        snprintf(g_unit_cache.said[g_unit_cache.said_count++], sizeof g_unit_cache.said[0], "%s", line);
}
static void forget(void) { g_unit_cache.said_count = 0; }
static int damage(const char* path, size_t size, size_t offset);
static int fake_disk(const char* folder, SnxU64* free_bytes, SnxU64* total) {
    (void)folder;
    if (g_unit_cache.disk_touch[0]) {
        if (g_unit_cache.disk_cut ? damage(g_unit_cache.disk_touch, g_unit_cache.disk_cut, 0)
                                  : snx_remove_tree(g_unit_cache.disk_touch))
            fprintf(stderr, "cannot cut or remove %s\n", g_unit_cache.disk_touch);
        g_unit_cache.disk_touch[0] = 0;
    }
    if (g_unit_cache.disk_plenty) {
        --g_unit_cache.disk_plenty;
        *free_bytes = *total = g_unit_cache.disk_total;
        return 0;
    }
    if (g_unit_cache.disk_fails) {
        --g_unit_cache.disk_fails;
        errno = EACCES;
        return -1;
    }
    *free_bytes = g_unit_cache.disk_free;
    *total = g_unit_cache.disk_total;
    return 0;
}
static int said(const char* a, const char* b) {
    int i;
    for (i = 0; i < g_unit_cache.said_count; ++i)
        if (strstr(g_unit_cache.said[i], a) && (!b || strstr(g_unit_cache.said[i], b))) return 1;
    return 0;
}

/* f0: x9 = x30; bl f1; x30 = x9; ret. f1: x0 += add; ret */
static void program(unsigned add) {
    const SnxU32 code[] = {0xAA1E03E9u, 0x94000003u, 0xAA0903FEu, 0xD65F03C0u, 0x91000000u | (add << 10), 0xD65F03C0u};
    unsigned char* memory = g_unit_cache.memory;
    size_t i, j;
    memset(memory, 0, sizeof g_unit_cache.memory);
    for (i = 0; i < sizeof code / sizeof *code; ++i)
        for (j = 0; j < 4; ++j) memory[TEXT_OFF + 4 * i + j] = (unsigned char)(code[i] >> (8 * j));
}

/* A32 at 0x200000, as a 32-bit game's: f0: push {lr}; bl f1; pop {pc}. f1: r0 += add; bx lr */
static void arm_program(unsigned add) {
    const SnxU32 code[] = {0xE52DE004u, 0xEB000000u, 0xE49DF004u, 0xE2800000u | add, 0xE12FFF1Eu};
    unsigned char* memory = g_unit_cache.memory;
    size_t i, j;
    memset(memory, 0, sizeof g_unit_cache.memory);
    for (i = 0; i < sizeof code / sizeof *code; ++i)
        for (j = 0; j < 4; ++j) memory[TEXT_OFF + 4 * i + j] = (unsigned char)(code[i] >> (8 * j));
}
/* the AArch32 module build (module_arm.c) of arm_program's code, each block a unit, through the cache in folder */
static int build_arm(const char* folder, const char* output) {
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxLlvmBuild llvm = {0};
    image.name = "test";
    image.base = 0x200000;
    image.size = sizeof g_unit_cache.memory;
    image.image = g_unit_cache.memory;
    image.image_size = sizeof g_unit_cache.memory;
    image.text_off = TEXT_OFF;
    image.text_size = TEXT_SIZE;
    loaded.modules = &image;
    loaded.module_count = 1;
    loaded.npdm.title_id = 0x0100000000000000ull;
    loaded.npdm.stack = 0x100000;
    strcpy(loaded.npdm.name, "test");
    options.title = "test";
    options.abi = SNX_ABI;
    options.quiet = 1;
    options.chunk_instrs = 1;
    llvm.output = output;
    llvm.opt = '2';
    llvm.jobs = 1;
    llvm.quiet = 1;
    llvm.unit_cache = folder;
    llvm.unit_cache_disk = g_unit_cache.disk;
    return snx_build_module_arm(&loaded, &options, &llvm);
}

static int build(const char* folder, const char* output, SnxU64 limit) {
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxLlvmBuild llvm = {0};
    image.name = "test";
    image.base = SNX_CODE_BASE;
    image.size = sizeof g_unit_cache.memory;
    image.image = g_unit_cache.memory;
    image.image_size = sizeof g_unit_cache.memory;
    image.text_off = TEXT_OFF;
    image.text_size = TEXT_SIZE;
    image.is_nro = 1;
    loaded.modules = &image;
    loaded.module_count = 1;
    loaded.npdm.title_id = 0x0100000000000000ull;
    loaded.npdm.stack = 0x100000;
    loaded.npdm.is64 = 1;
    strcpy(loaded.npdm.name, "test");
    options.title = "test";
    options.abi = SNX_ABI;
    options.quiet = 1;
    options.chunk_instrs = 1; /* each function a unit */
    llvm.output = output;
    llvm.triple = g_unit_cache.triple;
    llvm.opt = '2';
    llvm.jobs = 1;
    llvm.quiet = 1;
    llvm.unit_cache = folder;
    llvm.unit_cache_limit = limit;
    llvm.wrappers = &g_unit_cache.wrappers;
    llvm.unit_cache_disk = g_unit_cache.disk;
    return snx_build_module_llvm(&loaded, &options, &llvm);
}

static void undefined(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
}
static void poll_clear(CPU* c) { c->poll = 0; }
static void missed(CPU* c, u64 entry) {
    (void)entry;
    c->x[0] = 0xBAD;
}

/* the module's f0 run on x0 = 41: x0 after it, 0 when the module does not load */
static u64 run(const char* output) {
    typedef const SnxModule* (*ModuleFn)(void);
    char* path = snx_absolute(output);
    const SnxModule* mod;
    ModuleFn get;
    RtAPI rt = {0};
    CPU cpu = {0};
    u64 x0 = 0;
    size_t i;
#ifdef _WIN32
    HMODULE handle = path ? LoadLibraryA(path) : NULL;
    get = handle ? (ModuleFn)(void*)GetProcAddress(handle, "snx_module") : NULL;
#else
    void* handle = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    get = handle ? (ModuleFn)dlsym(handle, "snx_module") : NULL;
#endif
    free(path);
    if (!get) return 0;
    mod = get();
    rt.undef = undefined;
    rt.poll = poll_clear;
    rt.miss = missed;
    cpu.rt = &rt;
    cpu.mem = g_unit_cache.memory;
    cpu.code_lo = mod->code_lo;
    cpu.code_hi = mod->code_hi;
    cpu.disp = (GuestFn*)calloc((size_t)(mod->code_hi - mod->code_lo) / 4 + 1, sizeof(GuestFn));
    for (i = 0; i < mod->nentries; ++i) cpu.disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
    cpu.x[0] = 41;
    cpu.x[30] = 0xDEAD0000;
    for (i = 0; i < mod->nentries; ++i)
        if (mod->entry_addr[i] == SNX_CODE_BASE + TEXT_OFF) {
            mod->entry_fn[i](&cpu, mod->entry_addr[i]);
            x0 = cpu.x[0];
        }
    free(cpu.disp);
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return x0;
}

static int contains(const unsigned char* data, size_t size, const char* text) {
    size_t n = strlen(text) + 1, i; /* with its NUL, as a symbol's name is kept */
    for (i = 0; i + n <= size; ++i)
        if (!memcmp(data + i, text, n)) return 1;
    return 0;
}

/* the cache's objects in folder: how many, and the names of those that do not name F_<f0>, f1's units, into f1 (up
   to two) */
static size_t objects(const char* folder, char f1[2][80]) {
    char **names = NULL, f0[32];
    size_t count = 0, i, n = 0, found = 0;
    snprintf(f0, sizeof f0, "F_%llX", (unsigned long long)(SNX_CODE_BASE + TEXT_OFF));
    if (f1) f1[0][0] = f1[1][0] = 0;
    if (snx_list_dir(folder, &names, &count)) return 0;
    for (i = 0; i < count; ++i) {
        char* path;
        unsigned char* data;
        size_t size = 0;
        if (strlen(names[i]) != 66 || strcmp(names[i] + 64, ".o")) continue;
        ++n;
        if (!f1) continue;
        path = snx_path_join(folder, names[i]);
        data = snx_read_file(path, &size);
        if (data && !contains(data, size, f0) && found < 2) strcpy(f1[found++], names[i]);
        free(data);
        free(path);
    }
    snx_free_strings(names, count);
    return n;
}

static int same_file(const char* a, const char* b) {
    size_t na = 0, nb = 0, at = 0, i;
    unsigned char *x = snx_read_file(a, &na), *y = snx_read_file(b, &nb);
    int same = x && y && na == nb && !memcmp(x, y, na);
    if (!same) { /* where they part and the bytes there, for a platform only CI runs */
        while (x && y && at < na && at < nb && x[at] == y[at]) ++at;
        fprintf(stderr, "%s: %zu bytes, %s: %zu bytes, the first difference at %zu:", a, x ? na : 0, b, y ? nb : 0, at);
        for (i = at; i < at + 16 && x && i < na; ++i) fprintf(stderr, " %02x", x[i]);
        fprintf(stderr, " /");
        for (i = at; i < at + 16 && y && i < nb; ++i) fprintf(stderr, " %02x", y[i]);
        fprintf(stderr, "\n");
    }
    free(x);
    free(y);
    return same;
}

/* path's bytes cut to size, or one byte at offset flipped (size 0) */
static int damage(const char* path, size_t size, size_t offset) {
    size_t n = 0;
    unsigned char* data = snx_read_file(path, &n);
    int rc;
    if (!data || offset >= n) {
        free(data);
        return -1;
    }
    if (!size) data[offset] ^= 0x40;
    rc = snx_write_file(path, data, size ? size : n);
    free(data);
    return rc;
}

static void keys(void) {
    SnxUnitIdentity base = {"LLVM 1.2.3", '2', "pipeline=default\n"}, other;
    SnxUnitCache *a, *b;
    char error[512], key[65], k2[65];
    const char *triple = "x86_64-pc-linux-gnu", *cpu = "znver4", *features = "+avx2,+sse4.2";
    a = snx_unit_cache_open("snx-test-unit-cache.keys", &base, error, sizeof error);
    CHECK(a != NULL);
    if (!a) return;
    CHECK(!snx_unit_cache_key(a, triple, cpu, features, "IR", 2, key) && strlen(key) == 64);
    /* the key's bytes pinned (each field's length, 8 bytes little-endian, then the field): "snx-unit-cache",
       SNX_IR_OBJECT_VERSION 3 as 8 bytes, the LLVM, the -O letter, 1 for a tuning and its text, the triple, the CPU,
       the features and the bitcode. Bumping SNX_IR_OBJECT_VERSION changes it */
    CHECK(!strcmp(key, "db5f319e2c67bf3bd326cf3e3b367078a39f825c747548e6e04e0a4cabed6fd0"));
    /* each field of the unit's own */
    CHECK(!snx_unit_cache_key(a, "aarch64-unknown-linux-gnu", cpu, features, "IR", 2, k2) && strcmp(key, k2));
    CHECK(!snx_unit_cache_key(a, triple, "znver3", features, "IR", 2, k2) && strcmp(key, k2));
    CHECK(!snx_unit_cache_key(a, triple, cpu, "+avx2", "IR", 2, k2) && strcmp(key, k2));
    CHECK(!snx_unit_cache_key(a, triple, cpu, features, "IS", 2, k2) && strcmp(key, k2));
    CHECK(!snx_unit_cache_key(a, triple, cpu, features, "IR", 1, k2) && strcmp(key, k2));
    /* a field's bytes do not run into the next one's */
    CHECK(!snx_unit_cache_key(a, triple, "znver4+", "avx2,+sse4.2", "IR", 2, k2) && strcmp(key, k2));
    CHECK(!snx_unit_cache_key(a, triple, cpu, features, "IR", 2, k2) && !strcmp(key, k2));
    /* each field of the identity, and the folder is none */
    other = base;
    other.llvm = "LLVM 1.2.4";
    b = snx_unit_cache_open("snx-test-unit-cache.keys", &other, error, sizeof error);
    CHECK(b && !snx_unit_cache_key(b, triple, cpu, features, "IR", 2, k2) && strcmp(key, k2));
    snx_unit_cache_close(b);
    other = base;
    other.opt = '3';
    b = snx_unit_cache_open("snx-test-unit-cache.keys", &other, error, sizeof error);
    CHECK(b && !snx_unit_cache_key(b, triple, cpu, features, "IR", 2, k2) && strcmp(key, k2));
    snx_unit_cache_close(b);
    other = base;
    other.tuning = "pipeline=default\nllvm_pass_instcombine=0\n";
    b = snx_unit_cache_open("snx-test-unit-cache.keys", &other, error, sizeof error);
    CHECK(b && !snx_unit_cache_key(b, triple, cpu, features, "IR", 2, k2) && strcmp(key, k2));
    snx_unit_cache_close(b);
    other.tuning = NULL; /* LLVM's own settings, which are not an empty text */
    b = snx_unit_cache_open("snx-test-unit-cache.keys", &other, error, sizeof error);
    CHECK(b && !snx_unit_cache_key(b, triple, cpu, features, "IR", 2, key));
    snx_unit_cache_close(b);
    other.tuning = "";
    b = snx_unit_cache_open("snx-test-unit-cache.keys", &other, error, sizeof error);
    CHECK(b && !snx_unit_cache_key(b, triple, cpu, features, "IR", 2, k2) && strcmp(key, k2));
    snx_unit_cache_close(b);
    b = snx_unit_cache_open("snx-test-unit-cache.keys2", &base, error, sizeof error);
    CHECK(b && !snx_unit_cache_key(b, triple, cpu, features, "IR", 2, k2) &&
          !snx_unit_cache_key(a, triple, cpu, features, "IR", 2, key) && !strcmp(key, k2));
    snx_unit_cache_close(b);
    snx_unit_cache_close(a);
    snx_remove_tree("snx-test-unit-cache.keys");
    snx_remove_tree("snx-test-unit-cache.keys2");
}

/* the size limit over files of known times and sizes */
static void trim(void) {
    const char* folder = "snx-test-unit-cache.trim";
    static const char* const names[] = {"1111111111111111111111111111111111111111111111111111111111111111.o",
                                        "2222222222222222222222222222222222222222222222222222222222222222.o",
                                        "3333333333333333333333333333333333333333333333333333333333333333.o",
                                        "0000000000000000000000000000000000000000000000000000000000000000.o.123.tmp",
                                        "notes.txt",
                                        "4444444444444444444444444444444444444444444444444444444444444444.o.tmp",
                                        "555555555555555555555555555555555555555555555555555555555555555g.o"};
    static const SnxU64 times[] = {300, 100, 200, 100, 50, 60, 70};
    char data[1000], error[512];
    SnxUnitTrim t;
    size_t i;
    snx_remove_tree(folder);
    CHECK(!snx_mkdirs(folder));
    memset(data, 'x', sizeof data);
    for (i = 0; i < sizeof names / sizeof *names; ++i) {
        char* path = snx_path_join(folder, names[i]);
        CHECK(!snx_write_file(path, data, sizeof data) && !snx_set_file_time(path, times[i]));
        CHECK(snx_file_time(path) == times[i]);
        free(path);
    }
    /* no limit: nothing goes */
    CHECK(!snx_unit_cache_trim(folder, 0, &t, error, sizeof error) && t.files == 4 && t.held == 4000 && !t.removed);
    /* the cache's four files (three objects and a write left), the oldest first: the .tmp and 2222 at 100, by name */
    CHECK(!snx_unit_cache_trim(folder, 3000, &t, error, sizeof error));
    CHECK(t.files == 4 && t.held == 4000 && t.removed == 1 && t.removed_bytes == 1000 && !t.failed);
    for (i = 0; i < sizeof names / sizeof *names; ++i) {
        char* path = snx_path_join(folder, names[i]);
        CHECK(snx_is_file(path) == (i != 3)); /* never notes.txt nor a name the cache does not write */
        free(path);
    }
    CHECK(!snx_unit_cache_trim(folder, 2500, &t, error, sizeof error));
    CHECK(t.files == 3 && t.held == 3000 && t.removed == 1 && t.removed_bytes == 1000 && !t.failed);
    for (i = 0; i < sizeof names / sizeof *names; ++i) {
        char* path = snx_path_join(folder, names[i]);
        CHECK(snx_is_file(path) == (i != 1 && i != 3));
        free(path);
    }
    CHECK(!snx_unit_cache_trim(folder, 2000, &t, error, sizeof error) && t.files == 2 && !t.removed);
    CHECK(!snx_unit_cache_trim(folder, 1999, &t, error, sizeof error) && t.removed == 1 && t.removed_bytes == 1000);
    {
        char* path = snx_path_join(folder, names[0]); /* the newest stays */
        CHECK(snx_is_file(path));
        free(path);
    }
    CHECK(snx_unit_cache_trim("snx-test-unit-cache.none", 1, &t, error, sizeof error) &&
          strstr(error, "snx-test-unit-cache.none"));
    snx_remove_tree(folder);
}

/* what snx_objects_pack_write's load gives for an input (NULL: nothing), and its calls */
typedef struct {
    const char* give;
    size_t loads, written[4], written_count;
} PackCalls;
static unsigned char* pack_load(void* arg, size_t index, size_t* size, char* why, size_t why_size) {
    PackCalls* calls = (PackCalls*)arg;
    unsigned char* data;
    ++calls->loads;
    if (!calls->give) {
        snprintf(why, why_size, "the test's load has nothing for input %zu", index);
        return NULL;
    }
    *size = strlen(calls->give);
    data = (unsigned char*)malloc(*size + 1);
    memcpy(data, calls->give, *size + 1);
    return data;
}
static void pack_written(void* arg, size_t index) {
    PackCalls* calls = (PackCalls*)arg;
    if (calls->written_count < 4) calls->written[calls->written_count++] = index;
}

/* a pack written a record at a time is snx_objects_pack's bytes, an input of no data read when its turn comes; a
   read that fails, one of another size, a file that cannot be made or written: each said with what and why */
static void pack_records(void) {
    const char* folder = "snx-test-unit-cache.records";
    SnxLinkInput inputs[3] = {
        {(const unsigned char*)"abc", 3, "first"}, {NULL, 5, "second"}, {(const unsigned char*)"", 0, "third"}};
    SnxLinkInput whole[3];
    PackCalls calls = {0};
    unsigned char *expected, *file;
    size_t expected_size = 0, file_size = 0, size = 0;
    char *path, *missing, error[1024], want[1024];
    snx_remove_tree(folder);
    CHECK(!snx_mkdirs(folder));
    path = snx_path_join(folder, "pack.snxobj");
    missing = snx_path_join(folder, "none/pack.snxobj");
    memcpy(whole, inputs, sizeof whole);
    whole[1].data = (const unsigned char*)"hello";
    expected = snx_objects_pack(whole, 3, &expected_size);
    calls.give = "hello";
    CHECK(!snx_objects_pack_write(inputs, 3, path, &size, pack_load, pack_written, &calls, error, sizeof error));
    file = snx_read_file(path, &file_size);
    CHECK(expected && file && file_size == expected_size && size == file_size && !memcmp(file, expected, file_size));
    CHECK(calls.loads == 1 && calls.written_count == 3 && calls.written[0] == 0 && calls.written[1] == 1 &&
          calls.written[2] == 2);
    free(file);
    calls.give = NULL;
    CHECK(snx_objects_pack_write(inputs, 3, path, &size, pack_load, NULL, &calls, error, sizeof error) &&
          !strcmp(error, "cannot read back input 1 of 3 (second): the test's load has nothing for input 1"));
    calls.give = "help";
    CHECK(snx_objects_pack_write(inputs, 3, path, &size, pack_load, NULL, &calls, error, sizeof error) &&
          !strcmp(error, "input 1 of 3 (second) read back is 4 bytes, 5 when it was made"));
    CHECK(snx_objects_pack_write(whole, 3, missing, &size, NULL, NULL, NULL, error, sizeof error));
    snprintf(want, sizeof want, "cannot write %s: ", missing);
    CHECK(!strncmp(error, want, strlen(want)) && strlen(error) > strlen(want)); /* and the system's reason */
#ifdef __linux__
    if (snx_path_exists("/dev/full")) { /* every write fails with ENOSPC */
        /* a record past the stdio buffer fails as it is written, a small pack when the file is closed */
        static const unsigned char large[1 << 16];
        SnxLinkInput big = {large, sizeof large, "big"};
        snprintf(want, sizeof want, "cannot write /dev/full: %s", strerror(ENOSPC));
        CHECK(snx_objects_pack_write(&big, 1, "/dev/full", &size, NULL, NULL, NULL, error, sizeof error) &&
              !strcmp(error, want));
        CHECK(snx_objects_pack_write(whole, 3, "/dev/full", &size, NULL, NULL, NULL, error, sizeof error) &&
              !strcmp(error, want));
    }
#endif
    free(expected);
    free(missing);
    free(path);
    snx_remove_tree(folder);
}

/* the path of f0's object in folder, the one that names F_<f0>; NULL for none */
static char* f0_object(const char* folder) {
    char **names = NULL, f0[32], *found = NULL;
    size_t count = 0, i;
    snprintf(f0, sizeof f0, "F_%llX", (unsigned long long)(SNX_CODE_BASE + TEXT_OFF));
    if (snx_list_dir(folder, &names, &count)) return NULL;
    for (i = 0; i < count && !found; ++i) {
        char* path;
        unsigned char* data;
        size_t size = 0;
        if (strlen(names[i]) != 66 || strcmp(names[i] + 64, ".o")) continue;
        path = snx_path_join(folder, names[i]);
        data = snx_read_file(path, &size);
        if (data && contains(data, size, f0))
            found = path;
        else
            free(path);
        free(data);
    }
    snx_free_strings(names, count);
    return found;
}

/* A pack of this host's triple, its units through the cache: the bytes of the pack made with every object in memory,
   compiled or reused, the units the cache holds read back from it. f0's object removed or cut after its unit took it
   and before the pack reads it back (while room is made for f1's new unit) fails the pack, saying the unit and why.
   On a nearly full disk, f1's unit gets no room by removing f0's, which the pack reads back: f1's is not cached and
   the pack is whole */
static void pack_units(void) {
    const char* folder = "snx-test-unit-cache.packs";
    const char* cache = "snx-test-unit-cache.packs/units";
    char *triple = LLVMGetDefaultTargetTriple(), *reference, *out, *tmp, *path0 = NULL, f1[2][80];
    SnxU64 size0 = 0, size1 = 0;
    snx_remove_tree(folder);
    CHECK(!snx_mkdirs(folder));
    reference = snx_path_join(folder, "reference.snxobj");
    out = snx_path_join(folder, "out.snxobj");
    tmp = snx_format("%s.tmp", out);
    g_unit_cache.triple = triple;
    g_unit_cache.disk_total = 1000000000000ull;
    program(1);
    CHECK(!build(NULL, reference, 0));
    forget();
    CHECK(!build(cache, out, 0) && same_file(reference, out) && objects(cache, NULL) == 2 && !g_unit_cache.said_count);
    CHECK(!build(cache, out, 0) && same_file(reference, out));
    path0 = f0_object(cache);
    CHECK(path0 != NULL);
    if (!path0) goto done;
    size0 = snx_file_size(path0);
    /* removed: program(5) makes f1's unit new, and its room the moment */
    g_unit_cache.disk = fake_disk;
    g_unit_cache.disk_plenty = 1;
    snprintf(g_unit_cache.disk_touch, sizeof g_unit_cache.disk_touch, "%s", path0);
    program(5);
    forget();
    CHECK(build(cache, out, 0) && !snx_is_file(tmp) && same_file(reference, out));
    CHECK(said("packing failed: cannot read back input 1 of 3 (code chunk): code_000: cannot read ", path0));
    /* cut: program(6), another new unit for f1 */
    g_unit_cache.disk = NULL;
    program(1);
    CHECK(!build(cache, out, 0) && snx_file_size(path0) == size0);
    g_unit_cache.disk = fake_disk;
    g_unit_cache.disk_plenty = 1;
    snprintf(g_unit_cache.disk_touch, sizeof g_unit_cache.disk_touch, "%s", path0);
    g_unit_cache.disk_cut = (size_t)size0 - 100;
    program(6);
    forget();
    CHECK(build(cache, out, 0) && !snx_is_file(tmp));
    CHECK(said("packing failed: cannot read back input 1 of 3 (code chunk): code_000: ", "is not a whole object (") &&
          said(path0, "which do not end in the SNXUNIT1 check"));
    g_unit_cache.disk_cut = 0;
    /* a nearly full disk: f1's room is a byte, which only f0's object, this run's, could make */
    g_unit_cache.disk = NULL;
    program(1);
    CHECK(!snx_remove_tree(cache) && !build(cache, out, 0) && objects(cache, f1) == 2 && f1[0][0]);
    {
        char* path1 = snx_path_join(cache, f1[0]);
        size1 = snx_file_size(path1);
        free(path1);
    }
    CHECK(size1 > 0 && !snx_remove_tree(cache));
    g_unit_cache.disk = fake_disk;
    g_unit_cache.disk_plenty = 1; /* f0's unit, kept */
    g_unit_cache.disk_free = g_unit_cache.disk_total / 10 + size1 - 1;
    forget();
    CHECK(!build(cache, out, 0) && same_file(reference, out));
    CHECK(snx_is_file(path0) && objects(cache, NULL) == 1);
    CHECK(said("it leaves free: units without room are not cached", NULL));
done:
    g_unit_cache.disk = NULL;
    g_unit_cache.disk_plenty = 0;
    g_unit_cache.disk_touch[0] = 0;
    g_unit_cache.triple = NULL;
    LLVMDisposeMessage(triple);
    free(path0);
    free(tmp);
    free(out);
    free(reference);
    snx_remove_tree(folder);
}

int main(void) {
#ifdef _WIN32
#define SUFFIX ".dll"
#else
#define SUFFIX ".so"
#endif
    /* a module of its own for each run, as one loaded before is not loaded again from a file of its name: one file
       name in a folder each, as a Windows module names its file in its exports, and two names are never the same
       bytes */
    static const char* const dirs[] = {"snx-test-unit-cache.0", "snx-test-unit-cache.1", "snx-test-unit-cache.2",
                                       "snx-test-unit-cache.3", "snx-test-unit-cache.4", "snx-test-unit-cache.5"};
    static const char* const out[] = {"snx-test-unit-cache.0/module" SUFFIX, "snx-test-unit-cache.1/module" SUFFIX,
                                      "snx-test-unit-cache.2/module" SUFFIX, "snx-test-unit-cache.3/module" SUFFIX,
                                      "snx-test-unit-cache.4/module" SUFFIX, "snx-test-unit-cache.5/module" SUFFIX};
    const char* folder = "snx-test-unit-cache.d";
    SnxUnitIdentity identity = {"any", '2', NULL};
    SnxUnitCache* cache;
    char f1[2][80], f1a[80], f1b[80], error[512], llvm[128], *path_a, *path_b;
    unsigned char* object;
    size_t size = 0, i;
    SnxU64 now, total;

    snx_error_sink(sink);
    snx_unit_cache_llvm(llvm, sizeof llvm);
    CHECK(strstr(llvm, LLVM_VERSION_STRING) != NULL);
    keys();
    trim();
    pack_records();
    pack_units();
    snx_remove_tree(folder);
    for (i = 0; i < sizeof dirs / sizeof *dirs; ++i) {
        snx_remove_tree(dirs[i]);
        CHECK(!snx_mkdirs(dirs[i]));
    }

    /* a compiled unit's line: its time, and the memory the process holds where the system tells it */
    {
        char line[256];
        const char* label = "0x10-0x20, 1 function, 4 instructions";
        snx_unit_line(line, sizeof line, 3, 9, "code_002", label, 0, 1.3, (SnxU64)512 << 20);
        CHECK(!strcmp(line,
                      "  compiled block 3/9: code_002, 0x10-0x20, 1 function, 4 instructions (1.3 s, 512 MiB in use)"));
        snx_unit_line(line, sizeof line, 3, 9, "code_002", label, 0, 1.3, 0);
        CHECK(!strcmp(line, "  compiled block 3/9: code_002, 0x10-0x20, 1 function, 4 instructions (1.3 s)"));
        snx_unit_line(line, sizeof line, 3, 9, "code_002", label, 1, 1.3, (SnxU64)512 << 20);
        CHECK(!strcmp(line, "  compiled block 3/9: code_002, 0x10-0x20, 1 function, 4 instructions (cached)"));
    }
    /* a 32-bit game's units kept too: built again, the same module from the same objects, none compiled again; a block
       changed, its unit compiled and kept beside the old one */
    {
        const char* arm = "snx-test-unit-cache.arm";
        snx_remove_tree(arm);
        arm_program(1);
        CHECK(!build_arm(arm, out[4]) && objects(arm, NULL) >= 2);
        {
            size_t cold = objects(arm, NULL);
            CHECK(!build_arm(arm, out[5]) && objects(arm, NULL) == cold && same_file(out[4], out[5]));
            arm_program(5);
            CHECK(!build_arm(arm, out[5]) && objects(arm, NULL) == cold + 1 && !same_file(out[4], out[5]));
        }
        snx_remove_tree(arm);
        snx_remove_tree(out[4]);
        snx_remove_tree(out[5]);
    }
    /* a build's last word on its units, from the 32-bit build as from the 64-bit one; nothing when quiet */
    {
        const char* said_to = "snx-test-unit-cache.report";
        char* text;
        size_t size = 0;
        int saved;
        fflush(stdout);
        saved = dup(1);
        CHECK(saved >= 0 && freopen(said_to, "w", stdout) != NULL);
        snx_unit_cache_report("units here", 0, 3, 5, 2500000, 0);
        snx_unit_cache_report("units here", 0, 1, 1, 0, 1);
        fflush(stdout);
        CHECK(dup2(saved, 1) == 1);
        close(saved);
        text = (char*)snx_read_file(said_to, &size);
        CHECK(text && !strcmp(text, "unit cache: 3 of 5 units reused (2.5 MB) from units here\n"));
        free(text);
        snx_remove_tree(said_to);
    }
    /* an abort asked of a build: it stops before its next unit, saying how many it compiled, and keeps none */
    program(1);
    forget();
    snx_abort_request(1);
    CHECK(build(folder, out[0], 0) && said("aborted by the player, 0 of 2 code units compiled", NULL));
    CHECK(objects(folder, NULL) == 0);
    snx_abort_request(0);
    /* cold: both units compiled and kept */
    CHECK(!build(folder, out[0], 0) && run(out[0]) == 42);
    CHECK(g_unit_cache.wrappers == 0); /* no profile cache, whose units alone call them */
    CHECK(objects(folder, f1) == 2 && f1[0][0] && !f1[1][0]);
    strcpy(f1a, f1[0]);
    /* warm: the same module, byte for byte, from the same two objects */
    CHECK(!build(folder, out[1], 0));
    if (objects(folder, NULL) != 2) fprintf(stderr, "%zu objects after the warm build\n", objects(folder, NULL));
    CHECK(objects(folder, NULL) == 2);
    CHECK(same_file(out[0], out[1]));
    /* f1 changed: its unit compiled and kept beside the old one, f0's reused */
    program(5);
    CHECK(!build(folder, out[1], 0) && run(out[1]) == 46);
    CHECK(objects(folder, f1) == 3 && f1[1][0]);
    strcpy(f1b, strcmp(f1[0], f1a) ? f1[0] : f1[1]);
    CHECK(strcmp(f1a, f1b) != 0);
    path_a = snx_path_join(folder, f1a);
    path_b = snx_path_join(folder, f1b);

    /* the module runs what the cache holds: f1's +5 object planted under the +1 unit's key */
    cache = snx_unit_cache_open(folder, &identity, error, sizeof error);
    CHECK(cache != NULL);
    object = snx_read_file(path_b, &size);
    CHECK(object && size > SNX_UNIT_TRAILER);
    if (cache && object) {
        char key[65];
        memcpy(key, f1a, 64);
        key[64] = 0;
        CHECK(!snx_unit_cache_keep(cache, key, object, size - SNX_UNIT_TRAILER, error, sizeof error));
    }
    free(object);
    program(1);
    CHECK(!build(folder, out[2], 0) && run(out[2]) == 46);
    snx_remove_tree(path_a); /* compiled again, and kept */
    CHECK(!build(folder, out[3], 0) && run(out[3]) == 42 && same_file(out[0], out[3]) && snx_is_file(path_a));

    /* objects that are not whole: compiled again, said, and kept whole again */
    size = (size_t)snx_file_size(path_a);
    CHECK(size > 2 * SNX_UNIT_TRAILER);
    forget();
    CHECK(!damage(path_a, size - 100, 0) && !build(folder, out[1], 0) && same_file(out[0], out[1]));
    CHECK(said(path_a, "not a whole object") && said("which do not end in the SNXUNIT1 check", "code_001"));
    CHECK(snx_file_size(path_a) == size);
    forget();
    CHECK(!damage(path_a, 0, size / 2) && !build(folder, out[1], 0) && same_file(out[0], out[1]));
    CHECK(said(path_a, "whose SHA-256 is not the one its check holds") && snx_file_size(path_a) == size);
    forget();
    CHECK(!damage(path_a, 10, 0) && !build(folder, out[1], 0) && same_file(out[0], out[1]));
    CHECK(said(path_a, "10 bytes, fewer than its 40-byte check") && snx_file_size(path_a) == size);
    forget();
    CHECK(!build(folder, out[1], 0) && !g_unit_cache.said_count); /* whole again: nothing to say */
    /* a whole object under another unit's name: its check holds that unit's key, not this one's */
    object = snx_read_file(path_b, &size);
    CHECK(object && !snx_write_file(path_a, object, size));
    free(object);
    forget();
    CHECK(!build(folder, out[5], 0) && run(out[5]) == 42 && said(path_a, "whose SHA-256 is not the one"));

    /* a reuse renews the time, which the size limit goes by */
    CHECK(!snx_set_file_time(path_a, 1000) && !snx_set_file_time(path_b, 2000));
    now = (SnxU64)time(NULL);
    CHECK(!build(folder, out[1], 0) && snx_file_time(path_a) + 60 >= now && snx_file_time(path_b) == 2000);
    /* a byte under what the three hold, after the build: f1+5's object, reused longest ago, goes */
    {
        SnxUnitTrim t;
        CHECK(!snx_unit_cache_trim(folder, 0, &t, error, sizeof error) && t.files == 3);
        total = t.held;
    }
    CHECK(!build(folder, out[1], total - 1) && objects(folder, NULL) == 2);
    CHECK(snx_is_file(path_a) && !snx_is_file(path_b));
    CHECK(!build(folder, out[4], 1) && objects(folder, NULL) == 0 && run(out[4]) == 42);

    /* the disk keeps a tenth of itself free (a disk of 1000 GB here): short of it, a new object takes the place of the
       cache's oldest; with no room even so it is not kept, said with the disk's numbers; a disk whose room cannot be
       read keeps nothing, said with why. The module is built all the same */
    program(1);
    CHECK(!build(folder, out[4], 0) && objects(folder, NULL) == 2 && snx_is_file(path_a));
    CHECK(!snx_set_file_time(path_a, 1000)); /* f1+1's, the oldest once f0's is reused */
    g_unit_cache.disk = fake_disk;
    g_unit_cache.disk_total = 1000000000000ull;
    g_unit_cache.disk_free = 100000000000ull; /* the tenth: a new object needs its own size made */
    program(5);
    forget();
    CHECK(!build(folder, out[5], 0) && run(out[5]) == 46 && !g_unit_cache.said_count);
    CHECK(!snx_is_file(path_a) && snx_is_file(path_b));
    g_unit_cache.disk_free = 0;
    CHECK(!snx_remove_tree(folder)); /* none reused: each unit needs room, whatever the times left kept */
    program(1);
    forget();
    CHECK(!build(folder, out[1], 0) && run(out[1]) == 42 && !snx_is_file(path_a));
    { /* both units without room: the disk's numbers said once, both counted at the close */
        int lines = 0, i;
        for (i = 0; i < g_unit_cache.said_count; ++i) lines += strstr(g_unit_cache.said[i], "it leaves free") != NULL;
        CHECK(lines == 1 && said("unit cache: 0.0 GB free on ", "under the 100.0 GB (a tenth of 1000.0 GB) it leaves "
                                                                "free: units without room are not cached"));
        CHECK(said("unit cache: 2 unit(s) (", "MB) not cached in "));
    }
    g_unit_cache.disk_fails = 2;
    forget();
    CHECK(!build(folder, out[1], 0) && run(out[1]) == 42 && !snx_is_file(path_a));
    CHECK(said("unit cache: cannot read the free space of ", "units are not cached") &&
          said("unit cache: 2 unit(s) (", "MB) not cached in "));
    /* one unit's room unreadable, the other's short: each reason said, not only the first unit's */
    g_unit_cache.disk_fails = 1;
    forget();
    CHECK(!build(folder, out[1], 0) && run(out[1]) == 42 && !snx_is_file(path_a));
    CHECK(said("unit cache: cannot read the free space of ", "units are not cached") &&
          said("unit cache: 0.0 GB free on ", "it leaves free: units without room are not cached") &&
          said("unit cache: 2 unit(s) (", "MB) not cached in "));
    g_unit_cache.disk = NULL;
    g_unit_cache.disk_fails = 0;

    /* a cache folder that cannot be made stops the build, saying why */
    CHECK(!snx_write_file("snx-test-unit-cache.file", "x", 1));
    remove(out[2]);
    forget();
    CHECK(build("snx-test-unit-cache.file/units", out[2], 0) && !snx_is_file(out[2]));
    CHECK(said("unit cache: cannot make the folder snx-test-unit-cache.file/units", NULL));
    remove("snx-test-unit-cache.file");

    snx_unit_cache_close(cache);
    free(path_a);
    free(path_b);
    snx_remove_tree(folder);
    for (i = 0; i < sizeof dirs / sizeof *dirs; ++i) snx_remove_tree(dirs[i]);
    snx_error_sink(NULL);
    if (g_unit_cache.failures) {
        fprintf(stderr, "%d failure(s)\n", g_unit_cache.failures);
        return 1;
    }
    puts("unit_cache: ok");
    return 0;
}
