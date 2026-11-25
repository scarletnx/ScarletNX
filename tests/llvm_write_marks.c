/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The write marks (ir_marks.c, the setting write_marks): a function of guest stores of every kind (a plain store, a
   pair across a page boundary, a store across one from an offset, a byte, a vector, a register offset, a
   store-release, a store-exclusive through the exclusive helper, DC ZVA through its helper) and of stores off the
   stack pointer, compiled at O2 and at O0 with the marks and run on guest memory with the marks' bytes below it: each
   page a store wrote has its mark, the stack's pages and every other page none, and the stores' bytes are as without
   the marks.
     snx-test-llvm-write-marks */
#include "llvm_steps.h"
#include "common/cpu/recompiler/codegen.h"
#include "nx/loader/codegen_view.h"
#include "nx/loader/loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGES 64 /* guest memory: [0, PAGES * 4 KiB) */

static int g_failures;
static void fail(const char* level, const char* what) {
    fprintf(stderr, "llvm_write_marks at %s: %s\n", level, what);
    ++g_failures;
}

static const u32 g_words[] = {
    0xa9bf7bfd, /* stp x29, x30, [sp, #-16]!  the stack: no mark */
    0xd10083ff, /* sub sp, sp, #32 */
    0xf90007e3, /* str x3, [sp, #8]           the stack: no mark */
    0xf9000001, /* str x1, [x0]               page 1 */
    0xa93f89c1, /* stp x1, x2, [x14, #-8]     pages 2 and 3 */
    0xf907fda1, /* str x1, [x13, #4088]       pages 5 and 6 */
    0x39000081, /* strb w1, [x4]              page 8 */
    0x3d8000a0, /* str q0, [x5]               page 9 */
    0xb83069e1, /* str w1, [x15, x16]         page 10 */
    0xc89ffcc1, /* stlr x1, [x6]              page 11 */
    0xc85f7d49, /* ldxr x9, [x10] */
    0xc80b7d41, /* stxr w11, x1, [x10]        page 12 */
    0xd50b742c, /* dc zva, x12                page 13 */
    0xc8a17e82, /* cas x1, x2, [x20]          page 14 (LSE, the Ounce's features) */
    0x48267eb8, /* casp x6, x7, x24, x25, [x21] page 15: 16 bytes through snx_cas128 */
    0xf82182c9, /* swp x1, x9, [x22]          page 16 */
    0xf82102e9, /* ldadd x1, x9, [x23]        page 17 */
    0x910083ff, /* add sp, sp, #32 */
    0xa8c17bfd, /* ldp x29, x30, [sp], #16 */
    0xd65f03c0, /* ret */
};
#define WORDS (sizeof g_words / sizeof *g_words)
static const unsigned g_marked[] = {1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};

static void on_miss(CPU* c, u64 pc) {
    (void)c;
    char what[64];
    snprintf(what, sizeof what, "a miss at %llx", (unsigned long long)pc);
    fail("run", what);
}

static void run(GuestFn fn, const char* level) {
    static const RtAPI rt = {.miss = on_miss, .cas128 = snx_runtime_cas128};
    u8* block = (u8*)calloc(1, SNX_WMARK_BELOW + PAGES * 4096);
    if (!block) {
        fail(level, "no memory for the marks and guest memory");
        return;
    }
    u8 *mem = block + SNX_WMARK_BELOW, *marks = mem - SNX_WMARK_BELOW;
    CPU c;
    memset(&c, 0, sizeof c);
    c.rt = &rt;
    c.mem = mem;
    c.code_lo = SNX_CODE_BASE;
    c.code_hi = SNX_CODE_BASE + 4 * WORDS;
    c.x[31] = 0x3f000;
    c.x[30] = 0x1000; /* the return: outside the code */
    c.x[0] = 0x1000;
    c.x[1] = 0x1122334455667788ull;
    c.x[2] = 0x99aabbccddeeff00ull;
    c.x[3] = 0x5555;
    c.x[4] = 0x8000;
    c.x[5] = 0x9000;
    c.x[6] = 0xb000;
    c.x[10] = 0xc000;
    c.x[12] = 0xd000 + 5;
    c.x[13] = 0x5004;
    c.x[14] = 0x3000;
    c.x[15] = 0xa000;
    c.x[16] = 0x10;
    c.x[24] = 0x4444;
    c.x[25] = 0x5555aaaa;
    c.x[20] = 0xe000;
    c.x[21] = 0xf000;
    c.x[22] = 0x10000;
    c.x[23] = 0x11000;
    memcpy(mem + 0xe000, &c.x[1], 8); /* CAS and CASP find what they compare, and store */
    memcpy(mem + 0xf000, &c.x[6], 8);
    memcpy(mem + 0xf008, &c.x[7], 8);
    c.v[0].d[0] = 0x0123456789abcdefull;
    c.v[0].d[1] = 0xfedcba9876543210ull;
    memset(mem + 0xd000, 0xee, 64);
    fn(&c, SNX_CODE_BASE);
    char what[160];
    for (unsigned pg = 0; pg < PAGES; ++pg) {
        int want = 0;
        for (size_t i = 0; i < sizeof g_marked / sizeof *g_marked; ++i) want |= g_marked[i] == pg;
        if (marks[pg] != want) {
            snprintf(what, sizeof what, "page %u's mark is %u, expected %d", pg, marks[pg], want);
            fail(level, what);
        }
    }
    for (u64 i = PAGES; i < SNX_WMARK_BELOW; i += 4093)
        if (marks[i]) {
            snprintf(what, sizeof what, "mark %llu past guest memory is set", (unsigned long long)i);
            fail(level, what);
            break;
        }
    /* the stores' bytes, as the marks leave them */
    u64 v;
    memcpy(&v, mem + 0x1000, 8);
    if (v != c.x[1]) fail(level, "str x1, [x0] stored another value");
    memcpy(&v, mem + 0x2ff8, 8);
    if (v != c.x[1]) fail(level, "the pair's first doubleword");
    memcpy(&v, mem + 0x3000, 8);
    if (v != c.x[2]) fail(level, "the pair's second doubleword");
    memcpy(&v, mem + 0x5ffc, 8);
    if (v != c.x[1]) fail(level, "str x1, [x13, #4088]");
    if (mem[0x8000] != 0x88) fail(level, "strb w1, [x4]");
    if (memcmp(mem + 0x9000, &c.v[0], 16)) fail(level, "str q0, [x5]");
    if (c.x[11] != 0) fail(level, "the store-exclusive failed");
    memcpy(&v, mem + 0xc000, 8);
    if (v != c.x[1]) fail(level, "stxr's doubleword");
    for (int i = 0; i < 64; ++i)
        if (mem[0xd000 + i]) {
            fail(level, "dc zva left a byte");
            break;
        }
    memcpy(&v, mem + 0xe000, 8);
    if (v != 0x99aabbccddeeff00ull) fail(level, "cas stored another value");
    memcpy(&v, mem + 0xf000, 8);
    if (v != 0x4444) fail(level, "casp's first doubleword");
    memcpy(&v, mem + 0x10000, 8);
    if (v != 0x1122334455667788ull) fail(level, "swp's doubleword");
    memcpy(&v, mem + 0x3efd8, 8);
    if (v != 0x5555) fail(level, "str x3, [sp, #8]");
    if (c.x[31] != 0x3f000) fail(level, "the stack pointer at the end");
    free(block);
}

static void check(char opt) {
    static unsigned char image[WORDS * 4];
    char level[8], error[1024];
    SnxProgram program = {0};
    SnxModuleImage module = {0};
    SnxCodegen codegen;
    SnxFde range;
    LlvmSteps steps;
    snprintf(level, sizeof level, "O%c", opt);
    for (size_t i = 0; i < WORDS; ++i)
        for (unsigned b = 0; b < 4; ++b) image[4 * i + b] = (unsigned char)(g_words[i] >> 8 * b);
    module.name = (char*)"write_marks";
    module.image = image;
    module.image_size = 4 * WORDS;
    module.base = SNX_CODE_BASE;
    module.size = 0x1000;
    module.text_size = (SnxU32)(4 * WORDS);
    range.start = 0;
    range.size = 4 * WORDS;
    module.fde = &range;
    module.fde_count = 1;
    program.modules = &module;
    program.module_count = 1;
    program.npdm.title_id = 0x123456789ABCDEF0ull;
    program.npdm.stack = 0x100000;
    program.npdm.prio = 44;
    program.npdm.core = 2;
    if (snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, SNX_A64_TARGET_OUNCE) || codegen.function_count != 1) {
        fail(level, "analyze the program");
        return;
    }
    if (llvm_steps_begin_opt(&steps, 1, LLVM_STEPS_HOST, opt, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        fail(level, "start the module");
        snx_codegen_free(&codegen);
        return;
    }
    llvm_steps_add_function(&steps, &codegen, &codegen.functions[0]);
    if (llvm_steps_load(&steps, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        fail(level, "compile and load the module");
    } else
        run((GuestFn)(void (*)(void))steps.steps[0], level);
    llvm_steps_end(&steps);
    snx_codegen_free(&codegen);
}

int main(void) {
    SnxU64 stores, stack, marks;
    snx_ir_write_marks(1);
    check('2');
    check('0');
    snx_ir_write_marks_counts(&stores, &stack, &marks);
    /* two compiles, each with its stores off the stack pointer (the frame's pair, the local) left unmarked */
    if (stack < 4 || stores <= stack + 14) {
        char what[128];
        snprintf(what, sizeof what, "%llu stores seen and %llu off the stack, expected 4 or more of over 18",
                 (unsigned long long)stores, (unsigned long long)stack);
        fail("both", what);
    }
    if (g_failures) return 1;
    printf("llvm_write_marks: O2 and O0 marked the pages written and no others (%llu mark stores)\n",
           (unsigned long long)marks);
    return 0;
}
