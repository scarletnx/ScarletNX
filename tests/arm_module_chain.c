/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 32-bit game's module (module_arm.c) going from block to block by itself: a program of A32 and Thumb loops built
   into a module, loaded, and its blocks called as the runtime's loop calls them (arm_thread_run). One call runs a
   loop of ten million turns to the return that leaves it, A32 and Thumb, through an interworking call, without the
   host's stack growing; a poll set stops it at its backward branch, and not at a forward one; a block that sets an
   exit (an SVC) returns at once, and so does one whose branch reaches the next block's address in the other state. Each block its own unit (the next block a declaration, linked from another object),
   and all in one */
#include "common/utilities/common.h"
#include "nx/loader/loader.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_llvm.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define BASE     0x200000u
#define TEXT_OFF 0x1000u
#define TEXT     (BASE + TEXT_OFF)
#define LOOP     (TEXT + 0x00) /* subs r0, r0, #1; bne LOOP */
#define AFTER    (TEXT + 0x08) /* add r1, r1, #1; bx lr */
#define CALL     (TEXT + 0x10) /* mov r4, lr; blx THUMB */
#define BACK     (TEXT + 0x18) /* bx r4 */
#define THUMB    (TEXT + 0x1C) /* subs r0, #1; bne THUMB; adds r1, #1; bx lr */
#define SVC      (TEXT + 0x30) /* svc #0x27 */
#define STATE    (TEXT + 0x42) /* cmp r0, #0; it eq; bx r3 */
#define FALL     (TEXT + 0x48) /* adds r1, #1; bx lr: STATE's next instruction, and r3, an A32 address */
#define RETURN   0x300000u     /* lr: no code there */
#define TURNS    10000000u
#ifdef _WIN32
#define SUFFIX ".dll"
#else
#define SUFFIX ".so"
#endif

static struct {
    int failures;
    unsigned char memory[0x4000];
} g_arm_module_chain;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_arm_module_chain.failures;                                                                             \
        }                                                                                                              \
    } while (0)

/* a thread as the runtime lays one out for a block: its SnxArmCPU, and its poll elsewhere (arm.c, KThread) */
typedef struct {
    SnxArmCPU arm;
    u64 other;
    u32 poll;
} Thread;

static void program(void) {
    static const SnxU32 a32[] = {
        0xE2500001u, 0x1AFFFFFDu, /* LOOP: subs r0, r0, #1; bne LOOP */
        0xE2811001u, 0xE12FFF1Eu, /* AFTER: add r1, r1, #1; bx lr */
        0xE1A0400Eu, 0xFA000000u, /* CALL: mov r4, lr; blx THUMB */
        0xE12FFF14u,              /* BACK: bx r4 */
    };
    static const unsigned short thumb[] = {0x3801u, 0xD1FDu, 0x3101u, 0x4770u}; /* THUMB */
    static const SnxU32 svc[] = {0xEF000027u, 0xE2811001u, 0xE12FFF1Eu};        /* SVC: svc #0x27; then AFTER's */
    static const unsigned short state[] = {0x2800u, 0xBF08u, 0x4718u, 0x3101u, 0x4770u}; /* STATE, FALL */
    unsigned char* text = g_arm_module_chain.memory + TEXT_OFF;
    size_t i, j;
    for (i = 0; i < sizeof a32 / sizeof *a32; ++i)
        for (j = 0; j < 4; ++j) text[4 * i + j] = (unsigned char)(a32[i] >> (8 * j));
    for (i = 0; i < sizeof thumb / sizeof *thumb; ++i) {
        text[THUMB - TEXT + 2 * i] = (unsigned char)thumb[i];
        text[THUMB - TEXT + 2 * i + 1] = (unsigned char)(thumb[i] >> 8);
    }
    for (i = 0; i < sizeof svc / sizeof *svc; ++i)
        for (j = 0; j < 4; ++j) text[SVC - TEXT + 4 * i + j] = (unsigned char)(svc[i] >> (8 * j));
    for (i = 0; i < sizeof state / sizeof *state; ++i) {
        text[STATE - TEXT + 2 * i] = (unsigned char)state[i];
        text[STATE - TEXT + 2 * i + 1] = (unsigned char)(state[i] >> 8);
    }
}

/* the module of program's code, each block a unit of its own (units 1) or all in one */
static int build(const char* output, size_t units) {
    static const SnxU64 hints[] = {CALL, SVC, STATE | 1}; /* what no branch from the module's start reaches */
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxLlvmBuild llvm = {0};
    image.name = "test";
    image.base = BASE;
    image.size = sizeof g_arm_module_chain.memory;
    image.image = g_arm_module_chain.memory;
    image.image_size = sizeof g_arm_module_chain.memory;
    image.text_off = TEXT_OFF;
    image.text_size = 0x1000;
    loaded.modules = &image;
    loaded.module_count = 1;
    loaded.npdm.title_id = 0x0100000000000000ull;
    loaded.npdm.stack = 0x100000;
    strcpy(loaded.npdm.name, "test");
    options.title = "test";
    options.abi = SNX_ABI;
    options.quiet = 1;
    options.chunk_instrs = units;
    options.hints = hints;
    options.hint_count = sizeof hints / sizeof *hints;
    llvm.output = output;
    llvm.opt = '2';
    llvm.jobs = 1;
    llvm.quiet = 1;
    return snx_build_module_arm(&loaded, &options, &llvm);
}

static SnxArmBlock block(const SnxArmModule* m, SnxU32 key) {
    u32 i;
    for (i = 0; i < m->nentries; ++i)
        if (m->entry_addr[i] == key) return m->entry_fn[i];
    return NULL;
}

/* the block at key called once on t, in the state its key gives, with r0 = r0, r1 = 0 and r3 = FALL, as
   arm_thread_run calls one */
static void run(const SnxArmModule* m, Thread* t, SnxU32 key, SnxU32 r0, u32 poll) {
    SnxArmBlock b = block(m, key);
    memset(&t->arm, 0, sizeof t->arm);
    t->arm.cpsr = 0x10u | (key & 1 ? SNX_ARM_T : 0);
    t->arm.r[0] = r0;
    t->arm.r[3] = FALL;
    t->arm.r[14] = RETURN;
    t->arm.r[15] = key & ~1u;
    /* no block of the program reads or writes memory: mem stays NULL */
    t->poll = poll;
    if (b) b(&t->arm);
}

static void check_module(const char* output, const char* what) {
    typedef const SnxArmModule* (*ModuleFn)(void);
    char* path = snx_absolute(output);
    const SnxArmModule* m;
    ModuleFn get;
    Thread t;
#ifdef _WIN32
    HMODULE handle = path ? LoadLibraryA(path) : NULL;
    get = handle ? (ModuleFn)(void*)GetProcAddress(handle, "snx_module_arm") : NULL;
#else
    void* handle = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    get = handle ? (ModuleFn)dlsym(handle, "snx_module_arm") : NULL;
#endif
    free(path);
    CHECK(get, "%s: the module does not load", what);
    if (!get) return;
    m = get();
    CHECK(m->abi == SNX_ARM_MODULE_ABI && m->poll_offset, "%s: abi %x, poll_offset %p", what, (unsigned)m->abi,
          (void*)m->poll_offset);
    *m->poll_offset = (i64)offsetof(Thread, poll) - (i64)offsetof(Thread, arm); /* as arm_module writes it */
    CHECK(block(m, LOOP) && block(m, AFTER) && block(m, CALL) && block(m, BACK) && block(m, THUMB | 1) &&
              block(m, (THUMB + 4) | 1) && block(m, SVC) && block(m, SVC + 4) && block(m, STATE | 1) &&
              block(m, FALL | 1),
          "%s: a block of the program is missing", what);

    /* the loop to its end and on to AFTER in one call: ten million tail calls on the host's stack */
    run(m, &t, LOOP, TURNS, 0);
    CHECK(t.arm.r[0] == 0 && t.arm.r[1] == 1 && t.arm.r[15] == RETURN && !t.arm.exit,
          "%s: the A32 loop ended at %x with r0 %u, r1 %u (want %x, 0 and 1)", what, t.arm.r[15], t.arm.r[0],
          t.arm.r[1], RETURN);
    /* a waiting thread: back to the runtime at the backward branch, after one turn */
    run(m, &t, LOOP, 5, 1);
    CHECK(t.arm.r[0] == 4 && t.arm.r[1] == 0 && t.arm.r[15] == LOOP, "%s: with the poll set, at %x with r0 %u, r1 %u",
          what, t.arm.r[15], t.arm.r[0], t.arm.r[1]);
    /* and not at the forward one */
    run(m, &t, LOOP, 1, 1);
    CHECK(t.arm.r[0] == 0 && t.arm.r[1] == 1 && t.arm.r[15] == RETURN,
          "%s: with the poll set, the last turn ended at %x with r1 %u", what, t.arm.r[15], t.arm.r[1]);
    /* A32 to Thumb by BLX, the Thumb loop, back to A32 by BX lr, where the indirect branch returns */
    run(m, &t, CALL, TURNS, 0);
    CHECK(t.arm.r[0] == 0 && t.arm.r[1] == 1 && t.arm.r[15] == BACK && !(t.arm.cpsr & SNX_ARM_T) &&
              t.arm.r[4] == RETURN,
          "%s: the Thumb call ended at %x%s with r0 %u, r1 %u", what, t.arm.r[15],
          t.arm.cpsr & SNX_ARM_T ? " (Thumb)" : "", t.arm.r[0], t.arm.r[1]);
    run(m, &t, THUMB | 1, 3, 1);
    CHECK(t.arm.r[0] == 2 && t.arm.r[15] == THUMB && (t.arm.cpsr & SNX_ARM_T),
          "%s: the Thumb loop with the poll set ended at %x with r0 %u", what, t.arm.r[15], t.arm.r[0]);
    /* an exit: the runtime handles the SVC before the next block runs */
    run(m, &t, SVC, 0, 0);
    CHECK(t.arm.exit == SNX_ARM_SVC && t.arm.syndrome == 0x27 && t.arm.r[15] == SVC + 4 && t.arm.r[1] == 0,
          "%s: the SVC block ended at %x with exit %u, r1 %u", what, t.arm.r[15], (unsigned)t.arm.exit, t.arm.r[1]);
    /* BX r3 to FALL's address in A32 state: back to the runtime, which looks that up; the Thumb block isn't entered */
    run(m, &t, STATE | 1, 0, 0);
    CHECK(t.arm.r[15] == FALL && !(t.arm.cpsr & SNX_ARM_T) && !t.arm.exit && t.arm.r[1] == 0,
          "%s: the branch to A32 ended at %x%s with exit %u, r1 %u", what, t.arm.r[15],
          t.arm.cpsr & SNX_ARM_T ? " (Thumb)" : "", (unsigned)t.arm.exit, t.arm.r[1]);
    run(m, &t, STATE | 1, 1, 0);
    CHECK(t.arm.r[15] == RETURN && t.arm.r[1] == 1, "%s: the branch not taken ended at %x with r1 %u", what,
          t.arm.r[15], t.arm.r[1]);
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

int main(void) {
    const char* each = "snx-test-arm-module-chain.each" SUFFIX;
    const char* one = "snx-test-arm-module-chain.one" SUFFIX;
    program();
    CHECK(!build(each, 1), "the module of a unit per block does not build");
    check_module(each, "a unit per block");
    CHECK(!build(one, 4096), "the module of one unit does not build");
    check_module(one, "one unit");
    remove(each);
    remove(one);
    if (g_arm_module_chain.failures) {
        printf("%d failure(s)\n", g_arm_module_chain.failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
