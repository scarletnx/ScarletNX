/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Vita's offline compiler (vita/aot/compiler.c, itself): one image compiled by the host's native back end, and with
   the test having the native back end refuse some blocks (no module,
   the block and the reason said); each module loaded and run by the run loop's rules, with the block table and the
   imports called directly and without either. Every run ends in the state the code says and goes back to the loop as
   often as it should, and the module's descriptor is read back from its file. A module is the same bytes however many
   threads made it */
#include "vita/aot/module.h"
#include "vita/runtime/process.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/platform/file.h"
#include "common/platform/vm.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include "support/text_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE  0x81000000u
#define DATA  0x81100000u
#define STACK 0x81200000u
#define STOP  0x70000000u /* where the code returns to: no block is there */
#define PAGE  0x10000u
#define POLL  7 /* a slice's steps back: few, so the loops end slices */
#define MANY         6000 /* blocks, for rounds of discovery that the threads share */
#define LIST         (CODE + 0x1000u)
/* the times the loop enters a block, with the table and the direct imports and without either: the blocks go on to each
   other but past a slice's last step back and, without, at a branch through a register and an import's stub */
#define ENTERED_FAST 58u
#define ENTERED_SLOW 150u

static struct {
    int failures;
    int refuse; /* the native back end refuses the blocks at every third word (Thumb: halfword) */
    unsigned char* mem;
    VitaImage image;
    VitaSegment segment;
    VitaImport imports[2];
    uint32_t seeds[1];
    VitaImage many;
    uint32_t many_seeds[MANY];
    VitaLookup cache[VITA_LOOKUP_SLOTS];
    VitaThread thread;
    unsigned calls; /* of imports */
} g_test;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++g_test.failures;                                                                                         \
        }                                                                                                              \
    } while (0)

/* the compiler, its native back end the test's to have refuse blocks */
static const char refused[] = "a block the test refuses";
static int test_arm_block(SnxNativeArm* c, const char* name, const unsigned char* bytes, size_t count, uint32_t pc,
                          size_t* consumed, SnxNativeArmNext next, void* user) {
    *consumed = 0;
    return (!g_test.refuse || (pc >> 2) % 3) && snx_native_arm_block(c, name, bytes, count, pc, consumed, next, user);
}
static int test_thumb_block(SnxNativeArm* c, const char* name, const unsigned char* bytes, size_t count, uint32_t pc,
                            size_t* consumed, SnxNativeArmNext next, void* user) {
    *consumed = 0;
    return (!g_test.refuse || (pc >> 1) % 3) && snx_native_thumb_block(c, name, bytes, count, pc, consumed, next, user);
}
static const char* test_why(const SnxNativeArm* c) { return g_test.refuse ? refused : snx_native_arm_why(c); }
#define snx_native_arm_block   test_arm_block
#define snx_native_thumb_block test_thumb_block
#define snx_native_arm_why     test_why
#include "vita/aot/compiler.c"
#undef snx_native_arm_block
#undef snx_native_thumb_block
#undef snx_native_arm_why

/* ARM: start, which calls sum (a loop), fib (Thumb, recursive), an import's stub, pick (Thumb, through a register: a
   table branch, an IT block), vfp and count (Thumb, CBZ's loop) and a Thumb import's stub, adding up their results.
   Assembled for the Cortex-A9 at CODE:
     start: push {r4, r5, r6, lr}; mov r4, #0; mov r0, #100; bl sum; add r4, r4, r0; mov r0, #9; blx fib;
       add r4, r4, r0; movw r5, #0; movt r5, #0x8110; ldr r6, [r5]; add r4, r4, r6; mov r0, r4; bl stub_arm;
       str r0, [r5, #4]; adr r3, pick; orr r3, r3, #1; mov r0, #2; blx r3; add r4, r4, r0; mov r0, #3; bl vfp;
       add r4, r4, r0; mov r0, #5; blx count; add r4, r4, r0; mov r0, r4; blx stub_thumb; str r0, [r5, #8];
       str r4, [r5, #12]; mov r0, r4; pop {r4, r5, r6, pc}
     sum: mov r1, #0; 1: add r1, r1, r0; subs r0, r0, #1; bne 1b; mov r0, r1; bx lr
     vfp: vmov s0, r0; vcvt.f32.s32 s0, s0; vmov.f32 s1, #1.5; vmul.f32 s0, s0, s1; vcvt.s32.f32 s0, s0; vmov r0, s0;
       bx lr
     stub_arm: svc #3; bx lr
     fib: push {r4, r5, lr}; mov r4, r0; cmp r0, #2; blt 1f; subs r0, #1; bl fib; mov r5, r0; subs r0, r4, #2; bl fib;
       adds r0, r0, r5; 1: pop {r4, r5, pc}
     pick: cmp r0, #3; bhi 9f; tbb [pc, r0]; 10, 20, 30 or 40; cmp r0, #30; ite eq; addeq r0, #5; addne r0, #7; 9: bx lr
     count: movs r1, #0; 1: cbz r0, 2f; adds r1, #3; subs r0, #1; b 1b; 2: mov r0, r1; bx lr
     stub_thumb: svc #1; bx lr */
static const uint32_t code[] = {
    0xe92d4070, 0xe3a04000, 0xe3a00064, 0xeb00001b, 0xe0844000, 0xe3a00009, 0xfa000027, 0xe0844000, 0xe3005000,
    0xe3485110, 0xe5956000, 0xe0844006, 0xe1a00004, 0xeb00001e, 0xe5850004, 0xe28f3092, 0xe3833001, 0xe3a00002,
    0xe12fff33, 0xe0844000, 0xe3a00003, 0xeb00000f, 0xe0844000, 0xe3a00005, 0xfb000024, 0xe0844000, 0xe1a00004,
    0xfa000025, 0xe5850008, 0xe585400c, 0xe1a00004, 0xe8bd8070, 0xe3a01000, 0xe0811000, 0xe2500001, 0x1afffffc,
    0xe1a00001, 0xe12fff1e, 0xee000a10, 0xeeb80ac0, 0xeef70a08, 0xee200a20, 0xeebd0ac0, 0xee100a10, 0xe12fff1e,
    0xef000003, 0xe12fff1e, 0x4604b530, 0xdb072802, 0xf7ff3801, 0x4605fff9, 0xf7ff1ea0, 0x1940fff5, 0x2803bd30,
    0xe8dfd80e, 0x0402f000, 0x200a0806, 0x2014e004, 0x201ee002, 0x2028e000, 0xbf0c281e, 0x30073005, 0x21004770,
    0x3103b110, 0xe7fb3801, 0x47704608, 0x4770df01,
};
#define STUB_ARM   (CODE + 0xb4)
#define PICK       (CODE + 0xd6)
#define STUB_THUMB (CODE + 0x108)
/* what it adds up: sum(100), fib(9), the data's first word, pick(2), vfp(3) and count(5) */
#define WORD       0x1234u
#define BEFORE     (5050u + 34u + WORD)
#define TOTAL      (BEFORE + 35u + 4u + 15u)
static uint32_t import_result(uint32_t import, uint32_t r0) { return r0 * 3 + import + 1; }

static uint32_t direct_call(SnxArmCPU* cpu, uint32_t import) {
    ++g_test.calls;
    CHECK(import < 2 && cpu->r[15] == (import ? STUB_THUMB + 2 : STUB_ARM + 4));
    cpu->r[0] = import_result(import, cpu->r[0]);
    return import == 0; /* the second goes back to the loop, as a call that waits does */
}
[[clang::no_sanitize("function")]] static void run_block(SnxArmBlock block, SnxArmCPU* cpu) { block(cpu); }
/* runs the module as the run loop does (vita/runtime/run.c), with its table and its direct calls where fast: how many
   blocks the loop entered, 0 where it stopped */
static unsigned run(const VitaModule* m, int fast) {
    VitaThread* t = &g_test.thread;
    unsigned entered = 0;
    memset(t, 0, sizeof *t);
    memset(g_test.mem + DATA, 0, 16);
    snx_st_le32(g_test.mem + DATA, WORD);
    memset(g_test.cache, 0, sizeof g_test.cache);
    g_test.calls = 0;
    t->cpu.mem = g_test.mem;
    t->cpu.mem_size = VITA_SPACE;
    t->cpu.r[13] = STACK + PAGE - 64;
    t->cpu.r[14] = STOP;
    t->cpu.r[15] = CODE;
    t->cpu.cpsr = 0x10;
    t->lookup = fast ? g_test.cache : NULL;
    t->direct = fast ? direct_call : NULL;
    while (t->cpu.r[15] != STOP) {
        const uint32_t key = t->cpu.r[15] | !!(t->cpu.cpsr & SNX_ARM_T);
        const SnxArmBlock block = vita_module_block(m, key);
        if (!block || ++entered > 100000) {
            fprintf(stderr, "no block at 0x%08x, or too many (%u)\n", (unsigned)key, entered);
            ++g_test.failures;
            return 0;
        }
        g_test.cache[(key >> 1) & (VITA_LOOKUP_SLOTS - 1)] = (VitaLookup){key, block};
        if (t->poll <= 0) t->poll = POLL;
        run_block(block, &t->cpu);
        if (t->cpu.exit == SNX_ARM_SVC) { /* the loop's: the stub's address says the import */
            const uint32_t at = t->cpu.r[15] - (t->cpu.cpsr & SNX_ARM_T ? 2 : 4), import = at == STUB_ARM ? 0 : 1;
            CHECK(at == STUB_ARM || at == STUB_THUMB);
            CHECK(!fast && t->cpu.syndrome == import);
            t->cpu.exit = SNX_ARM_OK;
            ++g_test.calls;
            t->cpu.r[0] = import_result(import, t->cpu.r[0]);
        } else if (t->cpu.exit != SNX_ARM_OK) {
            fprintf(stderr, "exit %u at 0x%08x\n", t->cpu.exit, t->cpu.r[15]);
            ++g_test.failures;
            return 0;
        }
    }
    const unsigned char* data = g_test.mem + DATA;
    CHECK(t->cpu.r[0] == TOTAL && t->cpu.r[13] == STACK + PAGE - 64 && !(t->cpu.cpsr & SNX_ARM_T));
    CHECK(snx_ld_le32(data + 4) == import_result(0, BEFORE) && snx_ld_le32(data + 8) == import_result(1, TOTAL));
    CHECK(snx_ld_le32(data + 12) == TOTAL && g_test.calls == 2);
    return entered;
}
typedef struct {
    unsigned fast, slow, blocks;
} Runs;
/* the image compiled into folder as asked and run both ways; its lines in *said (the caller's to free) */
static Runs compile(const char* folder, const SnxCliRecompile* asked, char** said) {
    Runs r = {0};
    char error[512] = "", *lines = snx_path_join(folder, "lines.txt");
    char* path = vita_game_file(folder, VITA_MODULE_SUFFIX);
    size_t size = 0;
    VitaNative native;
    *said = NULL;
    CHECK(!snx_mkdirs(folder) && lines && path && freopen(lines, "w", stdout));
    if (vita_aot_compile(folder, &g_test.image, asked, error, sizeof error)) {
        fprintf(stderr, "%s: %s\n", folder, error);
        ++g_test.failures;
    } else {
        fflush(stdout);
        unsigned char* text = snx_read_file(lines, &size);
        if (text && (*said = malloc(size + 1)))
            memcpy(*said, text, size), (*said)[size] = 0, snx_test_text_lines(*said);
        free(text);
        CHECK(vita_module_current(path, &g_test.image));
        if (vita_native_open(path, &g_test.image, &native, error, sizeof error)) {
            r.blocks = native.module->count;
            for (uint32_t i = 1; i < native.module->count; ++i)
                CHECK(native.module->keys[i - 1] < native.module->keys[i]);
            if (r.blocks) r.fast = run(native.module, 1), r.slow = run(native.module, 0);
            vita_native_close(&native);
        } else {
            fprintf(stderr, "%s\n", error);
            ++g_test.failures;
        }
    }
    free(path);
    free(lines);
    return r;
}
/* the module of many blocks, made on threads threads, its size in *size (the caller's to free): NULL where it fails */
static unsigned char* made_on(const char* folder, const char* path, int threads, size_t* size) {
    char error[512] = "";
    const SnxCliRecompile quiet = {.quiet = 1};
    g_settings.aot_threads = threads;
    VitaNative native;
    CHECK(!vita_aot_compile(folder, &g_test.many, &quiet, error, sizeof error) && !error[0]);
    if (vita_native_open(path, &g_test.many, &native, error, sizeof error)) {
        CHECK(native.module->count == MANY);
        vita_native_close(&native);
    } else
        CHECK(!"the module of many blocks opens");
    return snx_read_file(path, size);
}
/* MANY blocks, each that a relocation alone goes to, the half of them Thumb: the module of one thread, and those of two
   and five, are the same bytes */
static void test_threads(const char* folder) {
    char* path = vita_game_file(folder, VITA_MODULE_SUFFIX);
    size_t size = 0;
    CHECK(path && !snx_mkdirs(folder));
    for (uint32_t i = 0; i < MANY; ++i) {
        const uint32_t at = LIST + i * 8;
        if (i % 2) /* movs r0, #i; bx lr; nop; nop */
            snx_st_le32(g_test.mem + at, 0x47702000u | (i & 255)), snx_st_le32(g_test.mem + at + 4, 0xBF00BF00u);
        else /* mov r0, #i; bx lr */
            snx_st_le32(g_test.mem + at, 0xE3A00000u | (i & 255)), snx_st_le32(g_test.mem + at + 4, 0xE12FFF1Eu);
        g_test.many_seeds[i] = at | i % 2;
    }
    g_test.many.segment[0] = (VitaSegment){LIST, MANY * 8, 5, g_test.mem + LIST};
    g_test.many.segment_count = 1;
    g_test.many.module_start = LIST;
    g_test.many.seeds = g_test.many_seeds;
    g_test.many.seed_count = MANY;
    unsigned char* one = made_on(folder, path, 1, &size);
    CHECK(one && size);
    for (int threads = 2; one && threads <= 5; threads += 3) {
        size_t other_size = 0;
        unsigned char* other = made_on(folder, path, threads, &other_size);
        CHECK(other && other_size == size && !memcmp(one, other, size));
        free(other);
    }
    free(one);
    free(path);
}

int main(void) {
    char *root = snx_temp_dir(NULL), *said = NULL, line[128];
    char* native = root ? snx_path_join(root, "native") : NULL;
    char *none = root ? snx_path_join(root, "none") : NULL, *both = root ? snx_path_join(root, "both") : NULL;
    g_test.mem = plat_mem_reserve(VITA_SPACE + 0x10000, 0);
    char* many = root ? snx_path_join(root, "many") : NULL;
    if (!none || !both || !many || !g_test.mem || plat_mem_commit(g_test.mem + CODE, PAGE) ||
        plat_mem_commit(g_test.mem + DATA, PAGE) || plat_mem_commit(g_test.mem + STACK, PAGE)) {
        fprintf(stderr, "vita_aot: no folder, or no guest memory\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof code / sizeof *code; ++i) snx_st_le32(g_test.mem + CODE + i * 4, code[i]);
    g_test.image.segment[0] = (VitaSegment){CODE, sizeof code, 5, g_test.mem + CODE};
    g_test.image.segment_count = 1;
    g_test.image.module_start = CODE;
    g_test.seeds[0] = PICK | 1; /* a relocation's: what only a register branches to */
    g_test.image.seeds = g_test.seeds;
    g_test.image.seed_count = 1;
    g_test.image.imports = g_test.imports;
    g_test.image.import_count = 2;
    g_test.imports[0].stub = STUB_ARM, g_test.imports[0].svc = 3;
    g_test.imports[1].stub = STUB_THUMB | 1, g_test.imports[1].svc = 1;

    const SnxCliRecompile jit = {.jit_only = 1};
    const Runs by_native = compile(native, NULL, &said);
    snprintf(line, sizeof line, "%s back end: %u of %u blocks\n", snx_native_name(), by_native.blocks,
             by_native.blocks);
    CHECK(said && strstr(said, line));
    CHECK(said && strstr(said, "Compiled 1/")); /* a line of progress, which the launcher reads */
    if (g_test.failures) fprintf(stderr, "the lines on stdout:\n%s", said ? said : "(none)\n");
    free(said);
    /* some blocks the native back end refuses: no module, the first of them and why said */
    {
        char error[512] = "";
        g_test.refuse = 1;
        CHECK(!snx_mkdirs(both));
        CHECK(vita_aot_compile(both, &g_test.image, NULL, error, sizeof error) != 0);
        g_test.refuse = 0;
        CHECK(strstr(error, " back end made no ") && strstr(error, refused));
        char* module = vita_game_file(both, VITA_MODULE_SUFFIX);
        CHECK(module && !snx_is_file(module));
        if (g_test.failures) fprintf(stderr, "the refusal: %s\n", error);
        free(module);
    }
    fprintf(stderr, "vita_aot: %u blocks, entered %u times with the table and %u without (%s)\n", by_native.blocks,
            by_native.fast, by_native.slow, snx_native_name());
    CHECK(by_native.blocks);
    CHECK(by_native.fast == ENTERED_FAST && by_native.slow == ENTERED_SLOW);
    CHECK(compile(none, &jit, &said).blocks == 0 && said && strstr(said, "No code ahead of time (--jit-only)"));
    free(said);
    test_threads(many);
#ifdef _WIN32
    CHECK(freopen("NUL", "w", stdout)); /* the folder's file closed */
#else
    CHECK(freopen("/dev/null", "w", stdout));
#endif
    (void)snx_remove_tree(root);
    free(native), free(none), free(both), free(many), free(root);
    plat_mem_release(g_test.mem, VITA_SPACE + 0x10000);
    if (g_test.failures) {
        fprintf(stderr, "vita_aot: %d failures\n", g_test.failures);
        return 1;
    }
    return 0;
}
