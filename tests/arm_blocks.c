/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AArch32 blocks a native back end makes of a list, run one at a time for the Python tests that hold the states and
   what they must come to (arm_blocks.py): the host's back end, whose code this processor runs, or built with
   SNX_TEST_ARM64 and its like another machine's, run under Unicorn (support/arm_back.h).
     snx-test-arm-blocks LIST STATE [--profile a57|a9|arm11|arm946e|arm7tdmi] [--memory flat|checked] [--pool N]
   LIST has a block a line: arm or thumb, its address, and its words or halfwords (64 at most), all hexadecimal. Block N
   is the Nth line's. Each is said on stdout, a line each, "made UNITS NAME..." (the words or halfwords it covers, and
   each instruction of the line as the decoder has it: an SnxA32Kind, a Thumb shape, "/pc" after one that names R15) or
   "refused WHY", then "ready MACHINE". STATE is a file of Shared and guest memory, which this maps and the test too.
   Then each block number read from stdin (32 bits, this host's order) runs on the state there and is answered by Reply:
   the run's end, or before it a call of the state's bus hook or access, which the test makes and answers with 32 bits.
   --pool N keeps the values in N host registers, so they spill. Without a back end, or a Unicorn of the machine, it
   says why and exits with 77. Built with SNX_TEST_ARM_LLVM the blocks are LLVM's (support/arm_llvm_back.h), and in the
   place of --pool it takes --opt 0|2, the passes' level, and --break before|after, the module spoiled there for the
   verifier to refuse (arm_llvm_checks.py) */
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "support/arm_back.h"
#if defined(SNX_TEST_NATIVE)
#define SNX_TEST_SWAP_ARM 1
#include "support/swap_state.h"
#endif
#include "common/cpu/arm/checked.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_UNITS 64
#define MEMORY    4096u /* where guest memory is in the file of the state */

typedef struct {
    SnxArmCheckedCPU state; /* its SnxArmCPU alone where memory is not checked */
    unsigned char bus[256]; /* an access's bytes, both ways */
} Shared;
static_assert(sizeof(Shared) <= MEMORY, "the state's page");

/* what an answer is: the run over; the bus hook (address, size_write, value); checked memory (operation, address,
   size) */
enum { REPLY_DONE, REPLY_IO, REPLY_ACCESS };
typedef struct {
    uint32_t kind, a, b, c, d;
} Reply;

typedef struct {
    int thumb, made;
    uint32_t pc, units[MAX_UNITS];
    size_t n;
    SnxU64 at;
} Block;

static struct {
    Block* blocks;
    size_t count;
    SnxArmCodegenOptions options;
    Shared* shared;
    size_t size; /* the file's */
    Back* back;
#if defined(SNX_TEST_NATIVE)
    HostUc* uc;
#else
    SnxCodeLoader* loader;
#endif
} g_arm_blocks;

[[noreturn]] static void fail(const char* what, const char* why) {
    fprintf(stderr, "FAIL: %s: %s\n", what, why);
    exit(2);
}

/* ---- the decoder's names, for the list ---- */

static const char* const kinds[] = {
    [SNX_A32_UNDEFINED] = "UNDEFINED",
    [SNX_A32_DATA] = "DATA",
    [SNX_A32_SINGLE] = "SINGLE",
    [SNX_A32_EXTRA] = "EXTRA",
    [SNX_A32_MULTIPLE] = "MULTIPLE",
    [SNX_A32_BRANCH] = "BRANCH",
    [SNX_A32_BX] = "BX",
    [SNX_A32_BLX_IMM] = "BLX_IMM",
    [SNX_A32_MOVW] = "MOVW",
    [SNX_A32_MOVT] = "MOVT",
    [SNX_A32_MUL] = "MUL",
    [SNX_A32_MULL] = "MULL",
    [SNX_A32_MLS] = "MLS",
    [SNX_A32_DIV] = "DIV",
    [SNX_A32_CLZ] = "CLZ",
    [SNX_A32_REV] = "REV",
    [SNX_A32_EXTEND] = "EXTEND",
    [SNX_A32_BITFIELD] = "BITFIELD",
    [SNX_A32_MRS] = "MRS",
    [SNX_A32_MSR] = "MSR",
    [SNX_A32_EXCLUSIVE] = "EXCLUSIVE",
    [SNX_A32_ACQUIRE_RELEASE] = "ACQUIRE_RELEASE",
    [SNX_A32_CLREX] = "CLREX",
    [SNX_A32_BARRIER] = "BARRIER",
    [SNX_A32_HINT] = "HINT",
    [SNX_A32_SVC] = "SVC",
    [SNX_A32_BKPT] = "BKPT",
    [SNX_A32_TLS] = "TLS",
    [SNX_A32_VECTOR] = "VECTOR",
    [SNX_A32_UADD8] = "UADD8",
    [SNX_A32_SEL] = "SEL",
    [SNX_A32_MULH] = "MULH",
    [SNX_A32_COPROC] = "COPROC",
    [SNX_A32_SATURATE] = "SATURATE",
    [SNX_A32_PACK] = "PACK",
    [SNX_A32_SELECT] = "SELECT",
    [SNX_A32_PARALLEL] = "PARALLEL",
    [SNX_A32_QARITH] = "QARITH",
    [SNX_A32_HALF_MUL] = "HALF_MUL",
    [SNX_A32_DUAL_MUL] = "DUAL_MUL",
    [SNX_A32_ABS_DIFF] = "ABS_DIFF",
    [SNX_A32_CRC32] = "CRC32",
    [SNX_A32_SETEND] = "SETEND",
};
static const char* const shapes[] = {
    [TH_UNDEF] = "UNDEFINED",
    [TH_SHIFT_IMM] = "SHIFT_IMM",
    [TH_ADDSUB] = "ADDSUB",
    [TH_IMM8] = "IMM8",
    [TH_ALU] = "ALU",
    [TH_HI] = "HI",
    [TH_LOAD] = "LOAD",
    [TH_STORE] = "STORE",
    [TH_ADDR] = "ADDR",
    [TH_EXTEND] = "EXTEND",
    [TH_PUSHPOP] = "PUSHPOP",
    [TH_LDMSTM] = "LDMSTM",
    [TH_BRANCH] = "BRANCH",
    [TH_MUL] = "MUL",
    [TH_BITFIELD] = "BITFIELD",
    [TH_WIMM] = "WIMM",
    [TH_WDATA] = "WDATA",
    [TH_TABLE] = "TABLE",
    [TH_IT] = "IT",
    [TH_HINT] = "HINT",
    [TH_LEGACY_BL] = "LEGACY_BL",
    [TH_REV] = "REV",
    [TH_A32] = "A32",
    [TH_DUAL] = "DUAL",
};
/* an A32 word of the kind: a vector's operation by its number (simd.h's AV_) */
static void say_a32(const char* before, SnxA32Kind kind, uint32_t word) {
    SnxArmVector v = {0};
    printf(" %s%s", before, kinds[kind]);
    if (kind == SNX_A32_VECTOR && snx_arm_vector_decode(word, &v)) printf(".%d", (int)v.op);
}
static void say_block(const Block* b, size_t units) {
    printf("made %zu", units);
    for (size_t k = 0; k < b->n; ++k) {
        SnxThumb t = {0};
        unsigned len;
        int pc;
        if (!b->thumb) {
            say_a32("", snx_a32_decode_profile(g_arm_blocks.options.profile, b->units[k]), b->units[k]);
            continue;
        }
        len = snx_thumb_decode_profile(g_arm_blocks.options.profile, (uint16_t)b->units[k],
                                       (uint16_t)(k + 1 < b->n ? b->units[k + 1] : 0), &t);
        if (!len) t.shape = TH_UNDEF;
        if (t.shape == TH_A32) { /* R15 in a field an instruction may read a register from, or in a list */
            uint32_t w = t.imm;
            say_a32("A32.", (SnxA32Kind)t.op, w);
            pc = (w >> 16 & 15) == 15 || (w >> 12 & 15) == 15 || (w >> 8 & 15) == 15 || (w & 15) == 15 ||
                 (t.op == SNX_A32_MULTIPLE && (w & 0x8000u));
        } else {
            printf(" %s", shapes[t.shape]);
            pc = t.rd == 15 || t.rn == 15 || t.rm == 15;
        }
        if (pc) printf("/pc");
        if ((b->units[k] >> 11) >= 0x1D) ++k;
    }
    printf("\n");
}

/* ---- the list ---- */

static void read_list(const char* path) {
    FILE* f = fopen(path, "r");
    char* line = NULL;
    size_t size = 0, cap = 0;
    if (!f) fail("the list", path);
    while (getline(&line, &size, f) > 0) {
        Block b = {0};
        char *save = NULL, *token = strtok_r(line, " \t\r\n", &save);
        if (!token) continue;
        b.thumb = !strcmp(token, "thumb");
        if (!b.thumb && strcmp(token, "arm")) fail("a block of the list is neither arm nor thumb", token);
        if (!(token = strtok_r(NULL, " \t\r\n", &save))) fail("a block of the list", "no address");
        b.pc = (uint32_t)strtoul(token, NULL, 16);
        while ((token = strtok_r(NULL, " \t\r\n", &save))) {
            if (b.n == MAX_UNITS) fail("a block of the list has more than 64 units", token);
            b.units[b.n++] = (uint32_t)strtoul(token, NULL, 16);
        }
        if (g_arm_blocks.count == cap) {
            cap = cap ? 2 * cap : 256;
            g_arm_blocks.blocks = (Block*)realloc(g_arm_blocks.blocks, cap * sizeof b);
            if (!g_arm_blocks.blocks) fail("the list", "out of memory");
        }
        g_arm_blocks.blocks[g_arm_blocks.count++] = b;
    }
    free(line);
    fclose(f);
}

/* ---- making and loading ---- */

static const struct {
    const char* name;
    void* fn;
} natives[] = {
    {"snx_arm_fp", (void*)snx_arm_fp},
    {"snx_arm_fp_x64", (void*)snx_arm_fp_x64},
    {"snx_arm_crypto", (void*)snx_arm_crypto},
    {"snx_arm_undefined", (void*)snx_arm_undefined},
    {"snx_arm_coproc", (void*)snx_arm_coproc},
    {"snx_arm_multiply_carry", (void*)snx_arm_multiply_carry},
    {"snx_arm_io_read", (void*)snx_arm_io_read},
    {"snx_arm_io_write", (void*)snx_arm_io_write},
    {"snx_arm_checked_fetch", (void*)snx_arm_checked_fetch},
    {"snx_arm_checked_access", (void*)snx_arm_checked_access},
    {"snx_arm_checked_strex", (void*)snx_arm_checked_strex},
};

/* the blocks' object loaded, their functions' addresses */
static void load(unsigned char* object, size_t size, size_t made) {
    char error[512] = "", (*names)[24] = calloc(made, sizeof *names);
    size_t* index = (size_t*)calloc(made, sizeof *index);
    const char** pointers = (const char**)calloc(made, sizeof *pointers);
    SnxU64* at = (SnxU64*)calloc(made, sizeof *at);
    size_t n = 0;
    if (!names || !index || !pointers || !at) fail("the names of the blocks", "out of memory");
    for (size_t i = 0; i < g_arm_blocks.count; ++i)
        if (g_arm_blocks.blocks[i].made) {
            snprintf(names[n], sizeof names[n], "B_%zu", i);
            pointers[n] = names[n];
            index[n++] = i;
        }
#if defined(SNX_TEST_NATIVE)
    if (hostuc_load(g_arm_blocks.uc, object, size, error, sizeof error)) fail("the " BACK_NAME " object", error);
    for (size_t k = 0; k < n; ++k)
        if (!(at[k] = hostuc_symbol(g_arm_blocks.uc, names[k]))) fail("the " BACK_NAME " object has no", names[k]);
#else
    SnxLinkInput input = {.data = object, .size = size, .name = BACK_NAME};
    if (snx_code_load_private(g_arm_blocks.loader, &input, 1, pointers, n, at, error, sizeof error))
        fail("the " BACK_NAME " object", error);
#endif
    for (size_t k = 0; k < n; ++k) g_arm_blocks.blocks[index[k]].at = at[k];
    free(names), free(index), free(pointers), free(at);
}
static void make_blocks(void) {
    unsigned char* object = NULL;
    size_t made = 0, size = 0;
    for (size_t i = 0; i < g_arm_blocks.count; ++i) {
        Block* b = &g_arm_blocks.blocks[i];
        unsigned char code[4 * MAX_UNITS]; /* as guest memory holds it: little-endian */
        unsigned width = b->thumb ? 2 : 4;
        char name[24];
        size_t units = 0;
        for (size_t k = 0; k < b->n; ++k)
            for (unsigned byte = 0; byte < width; ++byte)
                code[k * width + byte] = (unsigned char)(b->units[k] >> 8 * byte);
        snprintf(name, sizeof name, "B_%zu", i);
        b->made = (b->thumb ? back_(thumb_block)(g_arm_blocks.back, name, code, b->n, b->pc, &units, NULL, NULL)
                            : back_(block)(g_arm_blocks.back, name, code, b->n, b->pc, &units, NULL, NULL)) &&
                  units;
        if (b->made) {
            ++made;
            say_block(b, units);
        } else {
            printf("refused %s\n", back_(why)(g_arm_blocks.back));
        }
    }
    if (!made) return;
#if defined(SNX_TEST_ARM_LLVM)
    if (back_(object)(g_arm_blocks.back, &object, &size, NULL)) fail("the LLVM object", back_(why)(g_arm_blocks.back));
#else
    if (back_(object)(g_arm_blocks.back, &object, &size, NULL)) fail("the " BACK_NAME " object", "out of memory");
#endif
    load(object, size, made);
    free(object);
}

/* ---- running ---- */

static void put(const Reply* r) {
    if (fwrite(r, sizeof *r, 1, stdout) != 1 || fflush(stdout)) exit(1); /* the test has gone */
}
static uint32_t answer(void) {
    uint32_t v;
    if (fread(&v, sizeof v, 1, stdin) != 1) exit(1);
    return v;
}
/* SnxArmCPU.io and SnxArmCheckedCPU.access: the test's */
static uint32_t relay_io(SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value) {
    (void)cpu;
    put(&(Reply){REPLY_IO, address, size_write, value, 0});
    return answer();
}
static uint32_t relay_access(void* context, uint32_t operation, uint32_t address, uint32_t size, void* bytes) {
    Shared* s = (Shared*)context;
    uint32_t v;
    if (size > sizeof s->bus) fail("an access of more bytes than a checked block has", "");
    if ((operation & 3) == SNX_ARM_ACCESS_WRITE) memcpy(s->bus, bytes, size);
    put(&(Reply){REPLY_ACCESS, operation, address, size, 0});
    v = answer();
    if (!v && (operation & 3) != SNX_ARM_ACCESS_WRITE) memcpy(bytes, s->bus, size); /* nothing of a read that faults */
    return v;
}
#if defined(SNX_TEST_NATIVE)
static void swap(void* state) { snx_test_swap_checked((SnxArmCheckedCPU*)state); }
static void call(const Block* b, Shared* s) {
    if (hostuc_call(g_arm_blocks.uc, b->at, (uint64_t)(uintptr_t)&s->state, 0, NULL)) s->state.cpu.exit = 0xdead;
}
#else
/* a block is generated code, with no type signature before its entry, which the function sanitizer's check of an
   indirect call reads: the call is unchecked, as exec.c's run_block */
[[clang::no_sanitize("function")]] static void call(const Block* b, Shared* s) {
    ((SnxArmBlock)(uintptr_t)b->at)(&s->state.cpu);
}
#endif
static void run(const Block* b) {
    Shared* s = g_arm_blocks.shared;
    SnxArmCheckedCPU* state = &s->state;
    const SnxArmCheckedCPU given = *state; /* the test's pointers, put back after the run */
    int checked = g_arm_blocks.options.memory == SNX_ARM_MEMORY_CHECKED;
    if (checked) {
        state->context = s;
        if (state->access) state->access = relay_access;
    } else {
        state->cpu.mem = (uint8_t*)s + MEMORY;
    }
    if (state->cpu.io) state->cpu.io = relay_io;
    call(b, s);
    state->cpu.mem = given.cpu.mem;
    state->cpu.io = given.cpu.io;
    if (checked) state->context = given.context, state->access = given.access;
}
/* the file of the state mapped: for another machine below 4 GiB where there is room, as Unicorn's MIPS64 raises an
   exception for an LL or LLD of an address above it */
static void map_state(const char* path) {
    int fd = open(path, O_RDWR);
    struct stat st;
    void* p = MAP_FAILED;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= (off_t)MEMORY) fail("the file of the state", path);
    g_arm_blocks.size = (size_t)st.st_size;
#if defined(SNX_TEST_NATIVE) && defined(MAP_FIXED_NOREPLACE)
    for (uint64_t at = 0x10000000; p == MAP_FAILED && at + g_arm_blocks.size <= 0x100000000ull; at += 1u << 24) {
        p = mmap((void*)(uintptr_t)at, g_arm_blocks.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd,
                 0);
        if (p != MAP_FAILED && p != (void*)(uintptr_t)at) { /* a kernel without the flag takes it as a hint */
            munmap(p, g_arm_blocks.size);
            p = MAP_FAILED;
        }
    }
#endif
    if (p == MAP_FAILED) p = mmap(NULL, g_arm_blocks.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) fail("no mapping of the file of the state", path);
    close(fd);
    g_arm_blocks.shared = (Shared*)p;
}
/* the value of a named option: its index */
static unsigned name_of(const char* option, const char* given, const char* const* names, unsigned count) {
    for (unsigned k = 0; k < count; ++k)
        if (!strcmp(given, names[k])) return k;
    fail(option, given);
}

int main(int argc, char** argv) {
    static const char* const profiles[] = {[SNX_ARM_PROFILE_A57] = "a57",
                                           [SNX_ARM_PROFILE_ARM11] = "arm11",
                                           [SNX_ARM_PROFILE_A9] = "a9",
                                           [SNX_ARM_PROFILE_ARM7TDMI] = "arm7tdmi",
                                           [SNX_ARM_PROFILE_ARM946E] = "arm946e"};
    static const char* const memories[] = {[SNX_ARM_MEMORY_FLAT] = "flat", [SNX_ARM_MEMORY_CHECKED] = "checked"};
#if defined(SNX_TEST_ARM_LLVM)
    static const char* const breaks[] = {
        [ARM_LLVM_INTACT] = "", [ARM_LLVM_BREAK_BEFORE] = "before", [ARM_LLVM_BREAK_AFTER] = "after"};
    static const char* const opts[] = {"0", "2"};
    unsigned opt = 1, spoil = ARM_LLVM_INTACT;
#define VARIANT "[--opt 0|2] [--break before|after]"
#else
    int pool = 0;
#define VARIANT "[--pool N]"
#endif
    g_arm_blocks.options.abi = SNX_ARM_CODEGEN_OPTIONS_ABI;
    if (argc < 3) fail("usage", "snx-test-arm-blocks LIST STATE [--profile NAME] [--memory NAME] " VARIANT);
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 == argc) fail("an option with no value", argv[i]);
        if (!strcmp(argv[i], "--profile"))
            g_arm_blocks.options.profile = (SnxArmProfile)name_of("--profile", argv[i + 1], profiles, 5);
        else if (!strcmp(argv[i], "--memory"))
            g_arm_blocks.options.memory = (SnxArmMemoryMode)name_of("--memory", argv[i + 1], memories, 2);
#if defined(SNX_TEST_ARM_LLVM)
        else if (!strcmp(argv[i], "--opt"))
            opt = name_of("--opt", argv[i + 1], opts, 2);
        else if (!strcmp(argv[i], "--break"))
            spoil = name_of("--break", argv[i + 1], breaks, 3);
#else
        else if (!strcmp(argv[i], "--pool"))
            pool = (int)strtol(argv[i + 1], NULL, 10);
#endif
        else
            fail("an unknown option", argv[i]);
    }
#if defined(SNX_TEST_NATIVE)
    g_arm_blocks.uc = hostuc_new(BACK_UC, (size_t)64 << 20);
    if (!g_arm_blocks.uc) {
        printf("SKIP: no Unicorn " BACK_NAME "\n");
        return 77;
    }
#else
    char error[512] = "";
    if (!back_(available)()) {
        printf("SKIP: no " BACK_NAME " back end on this host\n");
        return 77;
    }
    g_arm_blocks.loader = snx_code_loader_new(0, error, sizeof error);
    if (!g_arm_blocks.loader) fail("no code loader", error);
#endif
    back_begin();
    read_list(argv[1]);
    map_state(argv[2]);
#if defined(SNX_TEST_NATIVE)
    if (hostuc_map(g_arm_blocks.uc, g_arm_blocks.shared, g_arm_blocks.size))
        fail("the file of the state", "not mapped");
    hostuc_swapper(g_arm_blocks.uc, swap, &g_arm_blocks.shared->state);
#endif
    for (size_t k = 0; k < sizeof natives / sizeof *natives; ++k)
#if defined(SNX_TEST_NATIVE)
        if (!hostuc_native(g_arm_blocks.uc, natives[k].name, natives[k].fn))
#else
        if (snx_code_define(g_arm_blocks.loader, natives[k].name, (SnxU64)(uintptr_t)natives[k].fn))
#endif
            fail("the runtime library's function cannot be named", natives[k].name);
    g_arm_blocks.back = back_(new)();
    if (!g_arm_blocks.back) fail("the " BACK_NAME " back end's compiler", "out of memory");
    if (back_(options)(g_arm_blocks.back, &g_arm_blocks.options))
        fail("no block compiler takes this", "profile and memory");
#if defined(SNX_TEST_ARM_LLVM)
    if (arm_llvm_opt(g_arm_blocks.back, opts[opt])) fail("--opt", opts[opt]);
    arm_llvm_break(g_arm_blocks.back, (int)spoil);
#else
    back_(limit)(g_arm_blocks.back, pool, ~0u);
#endif
    make_blocks();
    printf("ready " BACK_NAME "\n");
    fflush(stdout);
    for (uint32_t index; fread(&index, sizeof index, 1, stdin) == 1;) {
        if (index >= g_arm_blocks.count || !g_arm_blocks.blocks[index].made) fail("a run of a block not made", "");
        run(&g_arm_blocks.blocks[index]);
        put(&(Reply){REPLY_DONE, 0, 0, 0, 0});
    }
    back_(free)(g_arm_blocks.back);
#if defined(SNX_TEST_NATIVE)
    hostuc_free(g_arm_blocks.uc);
#else
    snx_code_loader_free(g_arm_blocks.loader);
#endif
    munmap(g_arm_blocks.shared, g_arm_blocks.size);
    free(g_arm_blocks.blocks);
    return 0;
}
