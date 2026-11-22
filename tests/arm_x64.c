/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's A32 blocks (src/common/cpu/recompiler/x64/x64_arm.c) against the AArch32 block compiler's LLVM
   ones (snx_arm_llvm_block, which module_arm.c makes a 32-bit program's blocks with): the same words consumed, and the
   same results from the same random states.
     snx-test-arm-x64 [--blocks N] [--states N] [--pool N] [--features MASK] [--seed N] [--word HEX]...
   Each block is words drawn from the A32 instructions the back end makes (data processing, branches, the loads and
   stores of every kind, the multiplies and divides, the bit operations, MRS and MSR, the exclusives and acquire and
   release, the barriers and hints, SVC, BKPT, the coprocessor's and UNDEFINED), under AL half of the time and any
   condition otherwise: one word alone, or two to six in a row (the block ending where one ends it). Both back ends make
   it, x64_arm.c's ELF object and LLVM's are loaded by the code loader, and each block runs from --states random states
   (registers into 64 KiB of guest memory of random bytes or anywhere, NZCV, Q and GE random, the exclusive monitor
   armed or not on a register's address with the value there or another): the registers, CPSR, the exit and its
   syndrome, the monitor, the TLS registers, the VFP registers and FPSCR, and guest memory must match. --pool N keeps
   the x86 values in N host registers, so they spill; --features takes the host's BMI1 (1), BMI2 (2) and LZCNT (4) out
   where not in MASK; --word runs the words given, one block each, and says what each state did */
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/x64/x64_arm.h"
#include "common/utilities/common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;

#define PC        0x1000u /* the blocks' address */
#define MEM_SIZE  0x10000u
#define CHUNK     256
#define MAX_WORDS 6

static struct {
    unsigned states, blocks, features;
    int pool;
    u64 seed;
    size_t failures, refused, ran, compared, words, consumed_words;
    uint32_t only[64];
    size_t nonly;
    char why[200];
    size_t kinds[64], refusals[64];
    size_t exits[16];   /* the runs' exits, LLVM's: how many ran through and how many stopped, and at what */
    const char* object; /* --object FILE: the first chunk's x86-64 object written there (objdump -d reads it) */
} g_test;

static u64 rnd(void) {
    u64 x = g_test.seed += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* ---- the words ---- */

/* a class of A32 instructions: its fixed bits (value under mask) and the kind the decoder gives its words */
typedef struct {
    const char* name;
    uint32_t mask, value;
    SnxA32Kind kind;
    int weight;
} Class;
static const Class classes[] = {
    {"data, immediate", 0x0e000000u, 0x02000000u, SNX_A32_DATA, 6},
    {"data, register", 0x0e000010u, 0x00000000u, SNX_A32_DATA, 6},
    {"data, register-shifted", 0x0e000090u, 0x00000010u, SNX_A32_DATA, 2},
    {"MOVW, MOVT", 0x0fb00000u, 0x03000000u, SNX_A32_MOVW, 2},
    {"MOVT", 0x0ff00000u, 0x03400000u, SNX_A32_MOVT, 1},
    {"B, BL", 0x0e000000u, 0x0a000000u, SNX_A32_BRANCH, 2},
    {"BX, BLX", 0x0ffffFd0u, 0x012fff10u, SNX_A32_BX, 1},
    {"BLX (immediate)", 0xfe000000u, 0xfa000000u, SNX_A32_BLX_IMM, 1},
    {"LDR, STR, immediate", 0x0e000000u, 0x04000000u, SNX_A32_SINGLE, 8},
    {"LDR, STR, register", 0x0e000010u, 0x06000000u, SNX_A32_SINGLE, 4},
    {"halfword, signed, doubleword", 0x0e000090u, 0x00000090u, SNX_A32_EXTRA, 4},
    {"LDM, STM", 0x0e000000u, 0x08000000u, SNX_A32_MULTIPLE, 4},
    {"MUL, MLA", 0x0fc000f0u, 0x00000090u, SNX_A32_MUL, 1},
    {"long multiplies", 0x0f8000f0u, 0x00800090u, SNX_A32_MULL, 1},
    {"UMAAL", 0x0ff000f0u, 0x00400090u, SNX_A32_MULL, 1},
    {"MLS", 0x0ff000f0u, 0x00600090u, SNX_A32_MLS, 1},
    {"SDIV, UDIV", 0x0fd0f0f0u, 0x0710f010u, SNX_A32_DIV, 1},
    {"CLZ", 0x0fff0ff0u, 0x016f0f10u, SNX_A32_CLZ, 1},
    {"REV, REV16, REVSH, RBIT", 0x0f3f0f70u, 0x063f0f30u, SNX_A32_REV, 1},
    {"extends", 0x0f8003f0u, 0x06800070u, SNX_A32_EXTEND, 2},
    {"SBFX, UBFX", 0x0fa00070u, 0x07a00050u, SNX_A32_BITFIELD, 1},
    {"BFI, BFC", 0x0fe00070u, 0x07c00010u, SNX_A32_BITFIELD, 1},
    {"MRS", 0x0fbf0fffu, 0x010f0000u, SNX_A32_MRS, 1},
    {"MSR (register)", 0x0ff0fff0u, 0x0120f000u, SNX_A32_MSR, 1},
    {"MSR (immediate)", 0x0ff0f000u, 0x0320f000u, SNX_A32_MSR, 1},
    {"exclusives", 0x0f8000f0u, 0x01800090u, SNX_A32_EXCLUSIVE, 2},
    {"acquire and release", 0x0f800cf0u, 0x01800c90u, SNX_A32_ACQUIRE_RELEASE, 1},
    {"CLREX", 0xffffffffu, 0xf57ff01fu, SNX_A32_CLREX, 1},
    {"barriers", 0xffffff00u, 0xf57ff000u, SNX_A32_BARRIER, 1},
    {"hints", 0x0fffff00u, 0x0320f000u, SNX_A32_HINT, 1},
    {"SVC", 0x0f000000u, 0x0f000000u, SNX_A32_SVC, 1},
    {"BKPT", 0xfff000f0u, 0xe1200070u, SNX_A32_BKPT, 1},
    {"MRC, MCR p15 c13 c0 2 and 3 (TLS)", 0x0fef0fdfu, 0x0e0d0f50u, SNX_A32_COPROC, 1},
    {"MRC, MCR p15", 0x0f000f10u, 0x0e000f10u, SNX_A32_COPROC, 1},
    {"UNDEFINED", 0x0ff000f0u, 0x07f000f0u, SNX_A32_UNDEFINED, 1},
};
#define CLASSES (sizeof classes / sizeof *classes)

/* a word of class c: its free bits random, two times in three under AL and else any condition but 1111 (a class of
   the unconditional space keeps its 1111), a register field 15 one time in eight */
static uint32_t word_of(const Class* c) {
    for (int tries = 0; tries < 100000; ++tries) {
        u64 x = rnd();
        uint32_t w = ((uint32_t)x & ~c->mask) | c->value, cond;
        if ((c->mask >> 28) != 15) {
            cond = (x >> 32) % 3 ? 14u : (uint32_t)(x >> 36) % 15;
            w = (w & 0x0fffffffu) | cond << 28;
        }
        for (unsigned f = 0; f < 4; ++f) { /* Rm, Rs, Rd, Rn, where the class leaves them free */
            static const unsigned at[4] = {0, 8, 12, 16};
            if (((x >> (40 + 3 * f)) & 7) == 0) w |= 15u << at[f] & ~c->mask;
        }
        if (snx_a32_decode(w) == c->kind) return w;
    }
    fprintf(stderr, "FAIL: no word of class %s found\n", c->name);
    exit(1);
}
static const Class* pick_class(void) {
    int total = 0, r;
    for (size_t k = 0; k < CLASSES; ++k) total += classes[k].weight;
    r = (int)(rnd() % (u64)total);
    for (size_t k = 0; k < CLASSES; ++k) {
        r -= classes[k].weight;
        if (r < 0) return &classes[k];
    }
    return &classes[0];
}

/* ---- running ---- */

typedef struct {
    SnxArmCPU cpu;
    unsigned char mem[MEM_SIZE];
} RunState;

/* a random state of the kth kind */
static void start(RunState* s, unsigned k) {
    SnxArmCPU* cpu = &s->cpu;
    memset(cpu, 0, sizeof *cpu);
    for (size_t i = 0; i < MEM_SIZE; i += 8) {
        u64 x = rnd();
        memcpy(s->mem + i, &x, 8);
    }
    for (unsigned r = 0; r < 15; ++r) {
        u64 x = rnd();
        int anywhere = k % 4 == 3 || (k % 4 == 2 && (r & 1)) || (k % 4 == 0 && !(r & 1));
        cpu->r[r] = anywhere ? (uint32_t)x : (uint32_t)(x % (MEM_SIZE - 0x400)) & (k % 4 == 1 ? ~3u : ~0u);
        if (!anywhere && (x >> 60) == 0) cpu->r[r] = (uint32_t)(x >> 32) & 31; /* small: shift amounts, divisors */
    }
    cpu->r[13] = 0x8000u + (uint32_t)(rnd() % 0x4000) * (k % 2 ? 4 : 1);
    cpu->r[15] = PC;
    cpu->cpsr = 0x10u | ((uint32_t)rnd() & 0xF80F0000u);
    cpu->exit = (uint32_t)rnd();
    cpu->syndrome = (uint32_t)rnd();
    cpu->mem = s->mem;
    cpu->mem_size = MEM_SIZE;
    {
        static const uint32_t sizes[4] = {1, 2, 4, 8};
        u64 x = rnd();
        cpu->exclusive_valid = k % 3 != 0;
        cpu->exclusive_address = x & 1 ? cpu->r[(x >> 1) % 15] : (uint32_t)(x >> 8) % MEM_SIZE;
        cpu->exclusive_size = sizes[(x >> 40) % 4];
        if ((x >> 44) & 1 && cpu->exclusive_address <= MEM_SIZE - 8)
            memcpy(&cpu->exclusive_value, s->mem + cpu->exclusive_address, 8); /* the value there: a STREX takes */
        else
            cpu->exclusive_value = rnd();
        if (cpu->exclusive_size < 8) cpu->exclusive_value &= (1ull << (8 * cpu->exclusive_size)) - 1;
    }
    cpu->tpidrurw = (uint32_t)rnd();
    cpu->tpidruro = (uint32_t)rnd();
    cpu->fpscr = (uint32_t)rnd() & 0xffc0009fu;
    for (unsigned i = 0; i < 32; ++i) cpu->d[i] = rnd();
}
/* what differs between the two runs, "" for nothing */
static const char* differs(const RunState* a, const RunState* b) {
    char* why = g_test.why;
    const SnxArmCPU *x = &a->cpu, *y = &b->cpu;
    for (unsigned r = 0; r < 16; ++r)
        if (x->r[r] != y->r[r])
            return snprintf(why, sizeof g_test.why, "r%u %08x, x86-64 %08x", r, x->r[r], y->r[r]), why;
    if (x->cpsr != y->cpsr) return snprintf(why, sizeof g_test.why, "cpsr %08x, x86-64 %08x", x->cpsr, y->cpsr), why;
    if (x->exit != y->exit) return snprintf(why, sizeof g_test.why, "exit %u, x86-64 %u", x->exit, y->exit), why;
    if (x->syndrome != y->syndrome)
        return snprintf(why, sizeof g_test.why, "syndrome %08x, x86-64 %08x", x->syndrome, y->syndrome), why;
    if (x->exclusive_valid != y->exclusive_valid || x->exclusive_address != y->exclusive_address ||
        x->exclusive_size != y->exclusive_size || x->exclusive_value != y->exclusive_value)
        return snprintf(why, sizeof g_test.why,
                        "monitor valid %u address %08x size %u value %016llx, x86-64 %u %08x %u %016llx",
                        x->exclusive_valid, x->exclusive_address, x->exclusive_size,
                        (unsigned long long)x->exclusive_value, y->exclusive_valid, y->exclusive_address,
                        y->exclusive_size, (unsigned long long)y->exclusive_value),
               why;
    if (x->tpidrurw != y->tpidrurw || x->tpidruro != y->tpidruro) return "the TLS registers";
    if (x->fpscr != y->fpscr)
        return snprintf(why, sizeof g_test.why, "fpscr %08x, x86-64 %08x", x->fpscr, y->fpscr), why;
    if (memcmp(x->d, y->d, sizeof x->d)) return "the VFP registers";
    for (size_t i = 0; i < MEM_SIZE; ++i)
        if (a->mem[i] != b->mem[i])
            return snprintf(why, sizeof g_test.why, "guest memory at %04zx: %02x, x86-64 %02x", i, a->mem[i],
                            b->mem[i]),
                   why;
    return "";
}

static SnxCodeLoader* new_loader(char* error, size_t size) {
    SnxCodeLoader* loader = snx_code_loader_new(0, error, size);
    if (loader && (snx_code_define(loader, "snx_arm_fp", (SnxU64)(uintptr_t)snx_arm_fp) ||
                   snx_code_define(loader, "snx_arm_crypto", (SnxU64)(uintptr_t)snx_arm_crypto) ||
                   snx_code_define(loader, "snx_arm_undefined", (SnxU64)(uintptr_t)snx_arm_undefined) ||
                   snx_code_define(loader, "snx_arm_coproc", (SnxU64)(uintptr_t)snx_arm_coproc) ||
                   snx_code_define(loader, "snx_arm_multiply_carry", (SnxU64)(uintptr_t)snx_arm_multiply_carry))) {
        snprintf(error, size, "cannot name the runtime library's functions");
        snx_code_loader_free(loader);
        return NULL;
    }
    return loader;
}
typedef struct {
    uint32_t words[MAX_WORDS];
    size_t n, consumed;
    int made; /* the x86-64 back end made it */
} Block;

/* the object loaded, each block's function in addr (those the x86-64 back end made alone, for its object) */
static int load(SnxCodeLoader* loader, unsigned char* object, size_t size, const char* what, const Block* blocks,
                size_t count, int made_only, SnxU64* addr) {
    char error[512], (*storage)[16] = calloc(count + 1, 16);
    const char** names = (const char**)calloc(count + 1, sizeof *names);
    SnxU64* got = (SnxU64*)calloc(count + 1, sizeof *got);
    size_t* index = (size_t*)calloc(count + 1, sizeof *index);
    SnxLinkInput input = {0};
    size_t n = 0;
    int rc;
    for (size_t i = 0; i < count; ++i) {
        if (made_only && !blocks[i].made) continue;
        snprintf(storage[n], sizeof storage[n], "B_%zu", i);
        names[n] = storage[n];
        index[n++] = i;
    }
    input.data = object;
    input.size = size;
    input.name = what;
    rc = snx_code_load_private(loader, &input, 1, names, n, got, error, sizeof error);
    if (rc) fprintf(stderr, "FAIL: the %s object: %s\n", what, error);
    for (size_t k = 0; !rc && k < n; ++k) addr[index[k]] = got[k];
    free(names);
    free(storage);
    free(got);
    free(index);
    return rc;
}

static void block_text(const Block* b, char* out, size_t size) {
    size_t at = 0;
    for (size_t i = 0; i < b->n && at < size; ++i)
        at += (size_t)snprintf(out + at, size - at, "%s%08x", i ? " " : "", b->words[i]);
}

/* the chunk's blocks made by both, loaded and run */
static int run_chunk(SnxIr* ir, SnxX64Arm* x64, Block* blocks, size_t count) {
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("llvm", ir->ctx);
    SnxCodeLoader *lloader = NULL, *xloader = NULL;
    unsigned char *lobj = NULL, *xobj = NULL;
    size_t lsize = 0, xsize = 0, made = 0;
    SnxU64 *laddr = calloc(count + 1, sizeof *laddr), *xaddr = calloc(count + 1, sizeof *xaddr);
    char error[512], text[100];
    RunState* s = (RunState*)malloc(2 * sizeof *s);
    int rc = 0;
    LLVMSetTarget(module, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(module, LLVMGetDataLayoutStr(ir->mod));
    for (size_t i = 0; i < count; ++i) {
        Block* b = &blocks[i];
        char name[16];
        size_t lc = 0, xc = 0;
        snprintf(name, sizeof name, "B_%zu", i);
        if (!snx_arm_llvm_block(module, name, b->words, b->n, PC, &lc)) {
            block_text(b, text, sizeof text);
            fprintf(stderr, "FAIL: [%s]: LLVM refused it\n", text);
            ++g_test.failures;
            rc = -1;
            break;
        }
        b->consumed = lc;
        b->made = snx_x64_arm_block(x64, name, (const unsigned char*)b->words, b->n, PC, &xc, NULL, NULL); /* the host's
                                                                                                         words: LE */
        if (!b->made) {
            SnxA32Kind k = snx_a32_decode(b->words[0]);
            block_text(b, text, sizeof text);
            ++g_test.refused;
            ++g_test.refusals[k & 63];
            fprintf(stderr, "FAIL: [%s]: the x86-64 back end refused it: %s\n", text, snx_x64_arm_why(x64));
            ++g_test.failures;
            continue;
        }
        ++made;
        if (xc != lc) {
            block_text(b, text, sizeof text);
            fprintf(stderr, "FAIL: [%s]: LLVM's block takes %zu words, the x86-64 back end's %zu\n", text, lc, xc);
            ++g_test.failures;
            b->made = 0;
        }
        g_test.words += b->n;
        g_test.consumed_words += lc;
    }
    if (!rc && (snx_ir_module_object(ir, module, '2', &lobj, &lsize, error, sizeof error) ||
                snx_x64_arm_object(x64, &xobj, &xsize, NULL))) {
        fprintf(stderr, "FAIL: an object: %s\n", error);
        ++g_test.failures;
        rc = -1;
    }
    if (!rc && g_test.object) {
        FILE* f = fopen(g_test.object, "wb");
        if (!f || fwrite(xobj, 1, xsize, f) != xsize)
            fprintf(stderr, "FAIL: cannot write %s\n", g_test.object), ++g_test.failures;
        if (f) fclose(f);
        g_test.object = NULL;
    }
    if (!rc && (!(lloader = new_loader(error, sizeof error)) || !(xloader = new_loader(error, sizeof error)))) {
        fprintf(stderr, "FAIL: no code loader: %s\n", error);
        ++g_test.failures;
        rc = -1;
    }
    if (!rc && (load(lloader, lobj, lsize, "LLVM", blocks, count, 0, laddr) ||
                (made && load(xloader, xobj, xsize, "x86-64", blocks, count, 1, xaddr)))) {
        ++g_test.failures;
        rc = -1;
    }
    for (size_t i = 0; !rc && i < count; ++i) {
        Block* b = &blocks[i];
        if (!b->made) continue;
        ++g_test.compared;
        for (unsigned k = 0; k < g_test.states; ++k) {
            u64 seed = g_test.seed;
            const char* why;
            for (int side = 0; side < 2; ++side) {
                g_test.seed = seed;
                start(&s[side], k);
                ((SnxArmBlock)(uintptr_t)(side ? xaddr[i] : laddr[i]))(&s[side].cpu);
            }
            ++g_test.ran;
            ++g_test.exits[s[0].cpu.exit & 15];
            why = differs(&s[0], &s[1]);
            if (g_test.nonly) {
                block_text(b, text, sizeof text);
                printf("[%s] state %u: LLVM exit %u r15 %08x cpsr %08x, x86-64 exit %u r15 %08x cpsr %08x%s%s\n", text,
                       k, s[0].cpu.exit, s[0].cpu.r[15], s[0].cpu.cpsr, s[1].cpu.exit, s[1].cpu.r[15], s[1].cpu.cpsr,
                       *why ? ": " : "", why);
            }
            if (*why) {
                block_text(b, text, sizeof text);
                fprintf(stderr, "FAIL: [%s] state %u: %s\n", text, k, why);
                ++g_test.failures;
                break;
            }
        }
    }
    free(s);
    free(lobj);
    free(xobj);
    free(laddr);
    free(xaddr);
    if (lloader) snx_code_loader_free(lloader);
    if (xloader) snx_code_loader_free(xloader);
    LLVMDisposeModule(module);
    return rc;
}

int main(int argc, char** argv) {
    char error[512];
    SnxIr* ir;
    SnxX64Arm* x64;
    Block* blocks;
    size_t done = 0;
    g_test.states = 16;
    g_test.blocks = 6000;
    g_test.features = ~0u;
    g_test.seed = 0xa32;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--states") && i + 1 < argc)
            g_test.states = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc)
            g_test.blocks = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--pool") && i + 1 < argc)
            g_test.pool = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--features") && i + 1 < argc)
            g_test.features = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            g_test.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--object") && i + 1 < argc)
            g_test.object = argv[++i];
        else if (!strcmp(argv[i], "--word") && i + 1 < argc && g_test.nonly < 64)
            g_test.only[g_test.nonly++] = (uint32_t)strtoul(argv[++i], NULL, 16);
        else
            return fprintf(stderr,
                           "usage: snx-test-arm-x64 [--blocks N] [--states N] [--pool N] [--features MASK] [--seed N] "
                           "[--object FILE] [--word HEX]...\n%s is not one of these\n",
                           argv[i]),
                   2;
    }
    if (!snx_x64_arm_available()) {
        printf("SKIP: no x86-64 back end on this host\n");
        return 77;
    }
    if (snx_ir_init(error, sizeof error) || !(ir = snx_ir_new_jit("arm-x64", '2', error, sizeof error))) {
        fprintf(stderr, "FAIL: no LLVM for this host: %s\n", error);
        return 1;
    }
    x64 = snx_x64_arm_new();
    snx_x64_arm_limit(x64, g_test.pool, g_test.features | ~7u);
    blocks = (Block*)calloc(CHUNK, sizeof *blocks);
    if (g_test.nonly) {
        for (size_t i = 0; i < g_test.nonly; ++i) {
            blocks[i].words[0] = g_test.only[i];
            blocks[i].n = 1;
        }
        run_chunk(ir, x64, blocks, g_test.nonly);
        done = g_test.nonly;
    }
    while (!g_test.nonly && done < g_test.blocks) {
        size_t count = g_test.blocks - done < CHUNK ? g_test.blocks - done : CHUNK;
        for (size_t i = 0; i < count; ++i) {
            Block* b = &blocks[i];
            /* one word alone a third of the time, else two to six */
            b->n = rnd() % 3 == 0 ? 1 : 2 + (size_t)(rnd() % (MAX_WORDS - 1));
            for (size_t k = 0; k < b->n; ++k) {
                const Class* c = pick_class();
                b->words[k] = word_of(c);
                ++g_test.kinds[c->kind & 63];
            }
        }
        if (run_chunk(ir, x64, blocks, count)) break;
        done += count;
    }
    printf("%zu blocks of %zu words (%zu in the blocks LLVM made of them), %zu compared from %u states each: %zu runs, "
           "%zu refused by the x86-64 back end\n",
           done, g_test.words, g_test.consumed_words, g_test.compared, g_test.states, g_test.ran, g_test.refused);
    printf("their exits:");
    for (unsigned e = 0; e < 16; ++e)
        if (g_test.exits[e])
            printf(" %u (%s) %zu", e,
                   e == SNX_ARM_OK           ? "on"
                   : e == SNX_ARM_UNDEFINED  ? "UNDEFINED"
                   : e == SNX_ARM_SVC        ? "SVC"
                   : e == SNX_ARM_BREAKPOINT ? "BKPT"
                   : e == SNX_ARM_DATA_ABORT ? "DATA_ABORT"
                   : e == SNX_ARM_ALIGNMENT  ? "ALIGNMENT"
                                             : "other",
                   g_test.exits[e]);
    printf("\n");
    free(blocks);
    snx_x64_arm_free(x64);
    snx_ir_free(ir);
    if (g_test.failures) printf("%zu failures\n", g_test.failures);
    return g_test.failures != 0;
}
